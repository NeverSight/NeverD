//===- JumpTableResolverSlice.cpp - Backward data-flow slicing ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Backward data-flow slicing over an instruction's micro-ops: reaching-
/// definition lookup, copy/extend chain tracing to a source register, scaled
/// index recovery, table load-address decomposition, and frame-slot keying.
/// Also hosts the two single-record base detectors that are built directly on
/// this slicing — the generic absolute-table slice and the PIC-relative table.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for the shared
/// declarations of the helpers defined here.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/DivisionByConstantInfo.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

bool detail::recordUniqueJumpTableProofPoint(
    std::map<JumpTableProofPoint, JumpTableProofLocation> &UniquePoints,
    std::set<JumpTableProofPoint> &AmbiguousPoints,
    JumpTableProofPoint Point, JumpTableProofLocation Location) {
  if (AmbiguousPoints.count(Point))
    return false;
  auto [It, Inserted] = UniquePoints.try_emplace(Point, Location);
  if (Inserted)
    return true;
  UniquePoints.erase(It);
  AmbiguousPoints.insert(Point);
  return false;
}

namespace {

size_t orderedSetLookupWork(size_t Count) {
  size_t Work = 1;
  for (size_t N = Count; N > 1; N = N / 2 + N % 2)
    ++Work;
  return Work;
}

bool intrinsicMayClobberFrameMemory(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC)
    return false;
  if (Op.NumInputs < 1 || !Op.Inputs[0].isConst())
    return true;
  const Intrinsic Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  switch (Id) {
  // Ordering/cache hints do not change the stored scalar value.  Everything
  // else marked side-effecting is conservatively a memory barrier here:
  // system calls and architecture memory intrinsics lack a LowIR summary that
  // could prove an escaped frame slot unchanged.
  case Intrinsic::Dmb:
  case Intrinsic::Dsb:
  case Intrinsic::Isb:
  case Intrinsic::ArmDmb:
  case Intrinsic::ArmDsb:
  case Intrinsic::ArmIsb:
  case Intrinsic::Mfence:
  case Intrinsic::Lfence:
  case Intrinsic::Sfence:
  case Intrinsic::Prefetch:
  case Intrinsic::Pause:
    return false;
  default:
    return isSideeffectIntrinsic(Id);
  }
}

std::optional<int64_t> signedFrameDelta(const NdVar &Value,
                                        uint16_t ArithmeticSize) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t) ||
      ArithmeticSize == 0 || ArithmeticSize > sizeof(uint64_t) ||
      Value.Provenance != ConstantAddressProvenance::Scalar)
    return std::nullopt;
  const unsigned SourceBits = static_cast<unsigned>(Value.Size) * 8;
  const unsigned ArithmeticBits = static_cast<unsigned>(ArithmeticSize) * 8;
  const uint64_t SourceMask = SourceBits == 64
                                  ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t{1} << SourceBits) - 1;
  const uint64_t ArithmeticMask = ArithmeticBits == 64
                                      ? std::numeric_limits<uint64_t>::max()
                                      : (uint64_t{1} << ArithmeticBits) - 1;

  // LowIR arithmetic zero-extends the narrower operand to the operation's
  // width before applying ADD/SUB.  Interpret signedness only after that
  // coercion: i8(0xf0) in a 32-bit SP add is +240, while i32(0xfffffff0) is
  // the genuine -16 frame displacement.  Sign-extending at the literal's own
  // width would merge two different runtime frame epochs.
  const uint64_t Raw = (Value.Offset & SourceMask) & ArithmeticMask;
  const uint64_t Sign = uint64_t{1} << (ArithmeticBits - 1);
  if ((Raw & Sign) == 0)
    return static_cast<int64_t>(Raw);
  return -1 - static_cast<int64_t>((~Raw) & ArithmeticMask);
}

std::optional<int64_t> checkedFrameOffset(int64_t Base, int64_t Delta,
                                          bool Subtract) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  if (!Subtract) {
    if ((Delta > 0 && Base > Max - Delta) || (Delta < 0 && Base < Min - Delta))
      return std::nullopt;
    return Base + Delta;
  }
  if ((Delta > 0 && Base < Min + Delta) || (Delta < 0 && Base > Max + Delta))
    return std::nullopt;
  return Base - Delta;
}

/// Architecture-neutral snapshot of the instruction facts needed by the
/// jump-table provenance proof.  Keeping this separate from CFGBuilder's
/// private InsnRecord lets the graph/data-flow implementation stay local to
/// this translation unit without exposing another public CFG type.
struct ResolverInsnSnapshot {
  va_t Addr = 0;
  uint16_t Size = 0;
  std::vector<LowOp> Ops;
  bool IsBranch = false;
  bool IsCond = false;
  bool IsCall = false;
  bool IsRet = false;
  bool IsIndirect = false;
  bool IsNoReturnCall = false;
  bool IsInstructionGuard = false;
  va_t BranchTarget = InvalidVA;
  std::vector<va_t> JumpTableTargets;
};

struct ResolverFlowBlock {
  va_t Start = 0;
  std::vector<LowOp> Ops;
  std::vector<int> Preds;
  std::vector<int> Succs;
  unsigned ExternalSuccs = 0;
  const ResolverInsnSnapshot *LastInsn = nullptr;
};

struct ResolverFlowGraph {
  std::vector<ResolverFlowBlock> Blocks;
  std::map<va_t, int> InsnToBlock;
  std::map<detail::JumpTableProofPoint, detail::JumpTableProofLocation>
      PointToOp;
  std::set<detail::JumpTableProofPoint> AmbiguousPoints;
  std::set<va_t> InstructionGuards;
  /// Durable and conditional block-entry roots that actually seeded the final
  /// pruned graph.  Dominance and live-in identity must use this set rather
  /// than graph indegree or PersistentCFGRoots alone.
  std::vector<int> RootBlocks;
};

/// Consume one candidate-local graph-construction allowance.  A null budget
/// keeps the established non-fixed-point callers unmetered; fixed-point callers
/// pass the same balance through snapshotting, graph construction, and value
/// resolution so no nested graph can receive a fresh allowance.
static bool consumeResolverGraphWork(size_t *Budget, size_t Amount = 1) {
  if (!Budget)
    return true;
  if (Amount > *Budget) {
    *Budget = 0;
    return false;
  }
  *Budget -= Amount;
  return true;
}

template <typename InsnMap, typename TargetSelector, typename Consume>
static bool copyResolverInsnSnapshots(
    const InsnMap &Insns, std::vector<ResolverInsnSnapshot> &Snapshot,
    TargetSelector &&SelectTargets, Consume &&ConsumeWork) {
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count) {
      ConsumeWork(std::numeric_limits<size_t>::max());
      return false;
    }
    return ConsumeWork(Count * Cost);
  };

  // Pay the outer buffer and its fixed lifetime before allocating it.  Each
  // snapshot then pays its fixed/actual element lifetime plus independent
  // deep-copy lifetimes for both nested vectors.
  if (!consumeProduct(Insns.size(), 2) || !ConsumeWork(2))
    return false;
  Snapshot.reserve(Insns.size());
  for (const auto &[Addr, Rec] : Insns) {
    const std::vector<va_t> &Targets = SelectTargets(Addr, Rec);
    if (!ConsumeWork(6) || !consumeProduct(Rec.Ops.size(), 3) ||
        !consumeProduct(Targets.size(), 3))
      return false;
    ResolverInsnSnapshot S;
    S.Addr = Addr;
    S.Size = Rec.Size;
    S.Ops = Rec.Ops;
    S.IsBranch = Rec.IsBranch;
    S.IsCond = Rec.IsCond;
    S.IsCall = Rec.IsCall;
    S.IsRet = Rec.IsRet;
    S.IsIndirect = Rec.IsIndirect;
    S.IsNoReturnCall = Rec.IsNoReturnCall;
    S.IsInstructionGuard = Rec.IsInstructionGuard;
    S.BranchTarget = Rec.BranchTarget;
    S.JumpTableTargets = Targets;
    Snapshot.push_back(std::move(S));
  }
  return true;
}

static ResolverFlowGraph buildResolverFlowGraph(
    const std::vector<ResolverInsnSnapshot> &Insns,
    const std::set<va_t> &BlockStarts, const std::set<va_t> &PersistentRoots,
    const std::map<va_t, std::set<va_t>> &ConditionalCodeRefRoots,
    const std::function<std::optional<bool>(va_t, const std::set<va_t> *)>
        &IsTableStorage,
    size_t *GraphWorkBudget = nullptr, bool *AnalysisComplete = nullptr) {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  auto consumeWork = [&](size_t Amount = 1) {
    return consumeResolverGraphWork(GraphWorkBudget, Amount);
  };
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (!GraphWorkBudget)
      return true;
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count) {
      *GraphWorkBudget = 0;
      return false;
    }
    return consumeWork(Count * Cost);
  };
  auto consumeLookup = [&](size_t Count) {
    return consumeWork(orderedSetLookupWork(Count));
  };
  auto consumeMapNodeInsert = [&](size_t Count) {
    const size_t Lookup = orderedSetLookupWork(Count);
    if (GraphWorkBudget && Lookup > std::numeric_limits<size_t>::max() - 5) {
      *GraphWorkBudget = 0;
      return false;
    }
    // Key and mapped-object construction/destruction plus the tree node's
    // allocation lifetime are paid before insertion.  Dynamic mapped payloads
    // (for example Grouped's vector elements) are charged separately.
    return consumeWork(Lookup + 5);
  };
  auto consumeSetNodeInsert = [&](size_t Count) {
    const size_t Lookup = orderedSetLookupWork(Count);
    if (GraphWorkBudget && Lookup > std::numeric_limits<size_t>::max() - 3) {
      *GraphWorkBudget = 0;
      return false;
    }
    return consumeWork(Lookup + 3);
  };
  constexpr size_t ProofPointKeyWork = 2;
  constexpr size_t ProofLocationWork = 2;
  auto consumeProofPointLookup = [&](size_t Count) {
    return consumeProduct(ProofPointKeyWork, orderedSetLookupWork(Count));
  };
  auto failIncomplete = [&]() { return ResolverFlowGraph{}; };
  ResolverFlowGraph Graph;
  std::map<va_t, size_t> GroupCounts;
  for (const ResolverInsnSnapshot &Insn : Insns) {
    if (!consumeWork() || !consumeLookup(BlockStarts.size()))
      return failIncomplete();
    auto BI = BlockStarts.upper_bound(Insn.Addr);
    if (BI == BlockStarts.begin())
      continue;
    --BI;
    if (!consumeLookup(GroupCounts.size()))
      return failIncomplete();
    auto Count = GroupCounts.lower_bound(*BI);
    if (Count == GroupCounts.end() || Count->first != *BI) {
      if (!consumeWork(5))
        return failIncomplete();
      Count = GroupCounts.emplace_hint(Count, *BI, 0);
    }
    if (Count->second == std::numeric_limits<size_t>::max()) {
      if (GraphWorkBudget)
        *GraphWorkBudget = 0;
      return failIncomplete();
    }
    ++Count->second;
  }

  std::map<va_t, std::vector<const ResolverInsnSnapshot *>> Grouped;
  for (const auto &[Start, Count] : GroupCounts) {
    if (!consumeWork() || !consumeMapNodeInsert(Grouped.size()) ||
        !consumeProduct(Count, 2) || !consumeWork(2))
      return failIncomplete();
    auto Group = Grouped.try_emplace(Start).first;
    Group->second.reserve(Count);
  }
  for (const ResolverInsnSnapshot &Insn : Insns) {
    if (!consumeWork() || !consumeLookup(BlockStarts.size()))
      return failIncomplete();
    auto BI = BlockStarts.upper_bound(Insn.Addr);
    if (BI == BlockStarts.begin())
      continue;
    --BI;
    if (!consumeLookup(Grouped.size()))
      return failIncomplete();
    auto Group = Grouped.find(*BI);
    if (Group == Grouped.end())
      return failIncomplete();
    Group->second.push_back(&Insn);
  }

  std::map<va_t, int> StartToBlock;
  if (Grouped.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !consumeProduct(Grouped.size(), 2) || !consumeWork(2))
    return failIncomplete();
  Graph.Blocks.reserve(Grouped.size());
  for (const auto &[Start, Members] : Grouped) {
    if (!consumeWork() || !consumeMapNodeInsert(StartToBlock.size()))
      return failIncomplete();
    const int Id = static_cast<int>(Graph.Blocks.size());
    StartToBlock.try_emplace(Start, Id);
    ResolverFlowBlock Block;
    Block.Start = Start;
    size_t BlockOpCount = 0;
    for (const ResolverInsnSnapshot *Insn : Members) {
      if (!consumeWork() ||
          Insn->Ops.size() >
              std::numeric_limits<size_t>::max() - BlockOpCount) {
        if (GraphWorkBudget)
          *GraphWorkBudget = 0;
        return failIncomplete();
      }
      BlockOpCount += Insn->Ops.size();
    }
    if (!consumeProduct(BlockOpCount, 2) || !consumeWork(2))
      return failIncomplete();
    Block.Ops.reserve(BlockOpCount);
    for (const ResolverInsnSnapshot *Insn : Members) {
      if (!consumeWork())
        return failIncomplete();
      if (!consumeMapNodeInsert(Graph.InsnToBlock.size()))
        return failIncomplete();
      Graph.InsnToBlock.try_emplace(Insn->Addr, Id);
      if (Insn->IsInstructionGuard) {
        if (!consumeSetNodeInsert(Graph.InstructionGuards.size()))
          return failIncomplete();
        Graph.InstructionGuards.insert(Insn->Addr);
      }
      for (const LowOp &Op : Insn->Ops) {
        if (!consumeWork())
          return failIncomplete();
        if (Block.Ops.size() >=
            static_cast<size_t>(std::numeric_limits<int>::max()))
          return failIncomplete();
        const int OpIndex = static_cast<int>(Block.Ops.size());
        Block.Ops.push_back(Op);
        const detail::JumpTableProofPoint Point{Op.Addr, Op.Seq};
        if (!consumeProofPointLookup(Graph.AmbiguousPoints.size()))
          return failIncomplete();
        auto Ambiguous = Graph.AmbiguousPoints.lower_bound(Point);
        if (Ambiguous != Graph.AmbiguousPoints.end() && *Ambiguous == Point)
          continue;
        if (!consumeProofPointLookup(Graph.PointToOp.size()))
          return failIncomplete();
        auto Unique = Graph.PointToOp.lower_bound(Point);
        if (Unique == Graph.PointToOp.end() || Unique->first != Point) {
          if (!consumeProduct(ProofPointKeyWork + ProofLocationWork, 2) ||
              !consumeWork())
            return failIncomplete();
          Graph.PointToOp.emplace_hint(
              Unique, Point, detail::JumpTableProofLocation{Id, OpIndex});
          continue;
        }
        Graph.PointToOp.erase(Unique);
        if (!consumeProduct(ProofPointKeyWork, 2) || !consumeWork())
          return failIncomplete();
        Graph.AmbiguousPoints.emplace_hint(Ambiguous, Point);
      }
      Block.LastInsn = Insn;
    }
    Graph.Blocks.push_back(std::move(Block));
  }

  auto blockAtInsn = [&](va_t Addr) -> int {
    auto It = Graph.InsnToBlock.find(Addr);
    return It == Graph.InsnToBlock.end() ? -1 : It->second;
  };
  auto addEdge = [&](int From, va_t Target, bool CountsExternal) {
    if (!consumeWork())
      return false;
    if (Target == InvalidVA)
      return true;
    if (!consumeLookup(Graph.InsnToBlock.size()))
      return false;
    const int To = blockAtInsn(Target);
    if (To < 0) {
      if (CountsExternal)
        ++Graph.Blocks[From].ExternalSuccs;
      return true;
    }
    auto &Succs = Graph.Blocks[From].Succs;
    for (int Existing : Succs) {
      if (!consumeWork())
        return false;
      if (Existing == To)
        return true;
    }
    Succs.push_back(To);
    return true;
  };

  for (int B = 0; B < static_cast<int>(Graph.Blocks.size()); ++B) {
    if (!consumeWork())
      return failIncomplete();
    ResolverFlowBlock &Block = Graph.Blocks[B];
    const ResolverInsnSnapshot *Rec = Block.LastInsn;
    if (!Rec)
      continue;
    size_t EdgeCapacity = 0;
    if (Rec->IsRet && Rec->IsCond && Rec->IsBranch) {
      EdgeCapacity = 1;
    } else if (!Rec->IsRet && Rec->IsBranch && Rec->IsIndirect) {
      EdgeCapacity = Rec->JumpTableTargets.size();
      if (Rec->IsCond) {
        if (EdgeCapacity == std::numeric_limits<size_t>::max()) {
          if (GraphWorkBudget)
            *GraphWorkBudget = 0;
          return failIncomplete();
        }
        ++EdgeCapacity;
      }
    } else if (!Rec->IsRet && Rec->IsBranch && !Rec->IsIndirect &&
               !Rec->IsCall) {
      EdgeCapacity = Rec->IsCond ? 2 : 1;
    } else if (!Rec->IsRet && (!Rec->IsBranch || Rec->IsCall) &&
               (!Rec->IsNoReturnCall || Rec->IsCond)) {
      EdgeCapacity = 1;
    }
    if (!consumeProduct(EdgeCapacity, 2) || !consumeWork(2))
      return failIncomplete();
    Block.Succs.reserve(EdgeCapacity);
    const va_t Fall = Rec->Addr + Rec->Size;
    if (Rec->IsRet && Rec->IsCond && Rec->IsBranch) {
      if (!addEdge(B, Rec->BranchTarget, true))
        return failIncomplete();
    } else if (Rec->IsRet) {
      // Terminal.
    } else if (Rec->IsBranch && Rec->IsIndirect) {
      if (Rec->IsCond && !addEdge(B, Fall, true))
        return failIncomplete();
      for (va_t Target : Rec->JumpTableTargets)
        if (!addEdge(B, Target, true))
          return failIncomplete();
    } else if (Rec->IsBranch && !Rec->IsIndirect && !Rec->IsCall) {
      if (Rec->IsCond && !addEdge(B, Fall, true))
        return failIncomplete();
      if (!addEdge(B, Rec->BranchTarget, true))
        return failIncomplete();
    } else if (!Rec->IsBranch || Rec->IsCall) {
      if ((!Rec->IsNoReturnCall || Rec->IsCond) &&
          !addEdge(B, Fall, false))
        return failIncomplete();
    }
  }

  if (!consumeProduct(Graph.Blocks.size(), 3) || !consumeWork(2))
    return failIncomplete();
  std::vector<size_t> PredecessorCounts(Graph.Blocks.size(), 0);
  for (const ResolverFlowBlock &Block : Graph.Blocks) {
    if (!consumeWork())
      return failIncomplete();
    for (int Succ : Block.Succs) {
      if (!consumeWork())
        return failIncomplete();
      if (Succ < 0 || Succ >= static_cast<int>(PredecessorCounts.size()) ||
          PredecessorCounts[Succ] == std::numeric_limits<size_t>::max()) {
        if (GraphWorkBudget)
          *GraphWorkBudget = 0;
        return failIncomplete();
      }
      ++PredecessorCounts[Succ];
    }
  }
  for (size_t B = 0; B < Graph.Blocks.size(); ++B) {
    if (!consumeWork() || !consumeProduct(PredecessorCounts[B], 2) ||
        !consumeWork(2))
      return failIncomplete();
    Graph.Blocks[B].Preds.reserve(PredecessorCounts[B]);
  }
  for (int B = 0; B < static_cast<int>(Graph.Blocks.size()); ++B) {
    if (!consumeWork())
      return failIncomplete();
    for (int S : Graph.Blocks[B].Succs) {
      if (!consumeWork())
        return failIncomplete();
      // addEdge has already made every (B,S) pair unique, so rebuilding the
      // inverse relation needs no second quadratic vector search.
      Graph.Blocks[S].Preds.push_back(B);
    }
  }

  // Decoded instructions are retained across fixed-point rounds so a target
  // can be re-admitted without decoding churn.  They are not all roots: once
  // a provisional table slot is removed, its old case block (and any backedge
  // it contains) must disappear from the proof graph as well as from LowFunc.
  // Seed only durable roots and follow the current successor relation, whose
  // indirect edges already reflect the latest JumpTableTargets.
  if (!consumeProduct(Graph.Blocks.size(), 3) || !consumeWork(2))
    return failIncomplete();
  std::vector<bool> Reachable(Graph.Blocks.size(), false);
  std::list<int> Worklist;
  auto Enqueue = [&](int Block) {
    if (!consumeWork(3))
      return false;
    Worklist.push_back(Block);
    return true;
  };
  auto Flood = [&] {
    while (!Worklist.empty()) {
      if (!consumeWork())
        return false;
      const int Block = Worklist.front();
      Worklist.pop_front();
      for (int Succ : Graph.Blocks[Block].Succs) {
        if (!consumeWork())
          return false;
        if (Succ >= 0 && Succ < static_cast<int>(Graph.Blocks.size()) &&
            !Reachable[Succ]) {
          Reachable[Succ] = true;
          if (!Enqueue(Succ))
            return false;
        }
      }
    }
    return true;
  };
  std::set<va_t> ActiveTableOwners;
  std::set<int> ActiveRootBlocks;
  for (;;) {
    if (!consumeWork(Reachable.size()))
      return failIncomplete();
    std::fill(Reachable.begin(), Reachable.end(), false);
    Worklist.clear();
    if (!consumeWork(ActiveRootBlocks.size()))
      return failIncomplete();
    ActiveRootBlocks.clear();
    for (va_t Root : PersistentRoots) {
      if (!consumeWork() || !consumeLookup(StartToBlock.size()))
        return failIncomplete();
      auto It = StartToBlock.find(Root);
      if (It == StartToBlock.end())
        continue;
      if (!consumeLookup(ActiveRootBlocks.size()))
        return failIncomplete();
      auto ActiveRoot = ActiveRootBlocks.lower_bound(It->second);
      if (ActiveRoot == ActiveRootBlocks.end() || *ActiveRoot != It->second) {
        if (!consumeWork(3))
          return failIncomplete();
        ActiveRootBlocks.emplace_hint(ActiveRoot, It->second);
      }
      if (!Reachable[It->second]) {
        Reachable[It->second] = true;
        if (!Enqueue(It->second))
          return failIncomplete();
      }
    }
    if (!Flood())
      return failIncomplete();
    for (bool Added = true; Added;) {
      Added = false;
      for (const auto &[Target, Sources] : ConditionalCodeRefRoots) {
        if (!consumeWork())
          return failIncomplete();
        if (IsTableStorage) {
          const std::optional<bool> Owned =
              IsTableStorage(Target, &ActiveTableOwners);
          if (!Owned)
            return failIncomplete();
          if (*Owned)
            continue;
        }
        if (!consumeLookup(StartToBlock.size()))
          return failIncomplete();
        auto TargetBlock = StartToBlock.find(Target);
        if (TargetBlock == StartToBlock.end())
          continue;
        bool HasReachableSource = false;
        for (va_t Source : Sources) {
          if (!consumeWork() || !consumeLookup(Graph.InsnToBlock.size()))
            return failIncomplete();
          auto SourceBlock = Graph.InsnToBlock.find(Source);
          if (SourceBlock != Graph.InsnToBlock.end() &&
              Reachable[SourceBlock->second]) {
            HasReachableSource = true;
            break;
          }
        }
        if (!HasReachableSource)
          continue;
        if (!consumeLookup(ActiveRootBlocks.size()))
          return failIncomplete();
        auto ActiveRoot = ActiveRootBlocks.lower_bound(TargetBlock->second);
        if (ActiveRoot == ActiveRootBlocks.end() ||
            *ActiveRoot != TargetBlock->second) {
          if (!consumeWork(3))
            return failIncomplete();
          ActiveRootBlocks.emplace_hint(ActiveRoot, TargetBlock->second);
        }
        if (Reachable[TargetBlock->second])
          continue;
        Reachable[TargetBlock->second] = true;
        if (!Enqueue(TargetBlock->second))
          return failIncomplete();
        Added = true;
      }
      if (!Flood())
        return failIncomplete();
    }

    bool OwnerAdded = false;
    for (const ResolverInsnSnapshot &Insn : Insns) {
      if (!consumeWork())
        return failIncomplete();
      if (Insn.JumpTableTargets.empty())
        continue;
      if (!consumeLookup(Graph.InsnToBlock.size()))
        return failIncomplete();
      auto It = Graph.InsnToBlock.find(Insn.Addr);
      if (It != Graph.InsnToBlock.end() && Reachable[It->second]) {
        if (!consumeLookup(ActiveTableOwners.size()))
          return failIncomplete();
        auto Owner = ActiveTableOwners.lower_bound(Insn.Addr);
        if (Owner == ActiveTableOwners.end() || *Owner != Insn.Addr) {
          if (!consumeWork(3))
            return failIncomplete();
          ActiveTableOwners.emplace_hint(Owner, Insn.Addr);
          OwnerAdded = true;
        }
      }
    }
    if (!OwnerAdded)
      break;
  }
  if (!consumeProduct(ActiveRootBlocks.size(), 3) || !consumeWork(2))
    return failIncomplete();
  Graph.RootBlocks.assign(ActiveRootBlocks.begin(), ActiveRootBlocks.end());

  bool NeedsPruning = false;
  size_t ReachableCount = 0;
  for (bool IsReachable : Reachable) {
    if (!consumeWork())
      return failIncomplete();
    NeedsPruning |= !IsReachable;
    ReachableCount += IsReachable ? 1 : 0;
  }
  if (NeedsPruning) {
    if (!consumeProduct(Graph.Blocks.size(), 3) ||
        !consumeWork(2) || !consumeProduct(ReachableCount, 2) ||
        !consumeWork(2))
      return failIncomplete();
    std::vector<int> OldToNew(Graph.Blocks.size(), -1);
    std::vector<ResolverFlowBlock> Pruned;
    Pruned.reserve(ReachableCount);
    for (size_t Old = 0; Old < Graph.Blocks.size(); ++Old) {
      if (!consumeWork())
        return failIncomplete();
      if (!Reachable[Old])
        continue;
      OldToNew[Old] = static_cast<int>(Pruned.size());
      Pruned.push_back(std::move(Graph.Blocks[Old]));
    }
    for (ResolverFlowBlock &Block : Pruned) {
      if (!consumeWork())
        return failIncomplete();
      std::vector<int> Succs;
      if (!consumeProduct(Block.Succs.size(), 2) || !consumeWork(2))
        return failIncomplete();
      Succs.reserve(Block.Succs.size());
      for (int Succ : Block.Succs) {
        if (!consumeWork())
          return failIncomplete();
        if (Succ >= 0 && Succ < static_cast<int>(OldToNew.size()) &&
            OldToNew[Succ] >= 0)
          Succs.push_back(OldToNew[Succ]);
      }
      Block.Succs = std::move(Succs);
      Block.Preds.clear();
    }
    for (size_t Block = 0; Block < Pruned.size(); ++Block) {
      if (!consumeWork())
        return failIncomplete();
      for (int Succ : Pruned[Block].Succs) {
        if (!consumeWork())
          return failIncomplete();
        if (!consumeWork(2))
          return failIncomplete();
        Pruned[Succ].Preds.push_back(static_cast<int>(Block));
      }
    }

    for (auto It = Graph.InsnToBlock.begin(); It != Graph.InsnToBlock.end();) {
      if (!consumeWork())
        return failIncomplete();
      const int Old = It->second;
      if (Old < 0 || Old >= static_cast<int>(OldToNew.size()) ||
          OldToNew[Old] < 0)
        It = Graph.InsnToBlock.erase(It);
      else {
        It->second = OldToNew[Old];
        ++It;
      }
    }
    for (auto It = Graph.PointToOp.begin(); It != Graph.PointToOp.end();) {
      if (!consumeWork())
        return failIncomplete();
      const int Old = It->second.first;
      if (Old < 0 || Old >= static_cast<int>(OldToNew.size()) ||
          OldToNew[Old] < 0)
        It = Graph.PointToOp.erase(It);
      else {
        It->second.first = OldToNew[Old];
        ++It;
      }
    }
    for (auto It = Graph.InstructionGuards.begin();
         It != Graph.InstructionGuards.end();) {
      if (!consumeWork() || !consumeLookup(Graph.InsnToBlock.size()))
        return failIncomplete();
      if (!Graph.InsnToBlock.count(*It))
        It = Graph.InstructionGuards.erase(It);
      else
        ++It;
    }
    if (!consumeProduct(Graph.RootBlocks.size(), 2) || !consumeWork(2))
      return failIncomplete();
    std::vector<int> RemappedRoots;
    RemappedRoots.reserve(Graph.RootBlocks.size());
    for (int Root : Graph.RootBlocks) {
      if (!consumeWork())
        return failIncomplete();
      if (Root < 0 || Root >= static_cast<int>(OldToNew.size()) ||
          OldToNew[Root] < 0)
        continue;
      bool Duplicate = false;
      for (int Existing : RemappedRoots) {
        if (!consumeWork())
          return failIncomplete();
        if (Existing == OldToNew[Root]) {
          Duplicate = true;
          break;
        }
      }
      if (!Duplicate)
        RemappedRoots.push_back(OldToNew[Root]);
    }
    Graph.RootBlocks = std::move(RemappedRoots);
    Graph.Blocks = std::move(Pruned);
  }
  if (AnalysisComplete)
    *AnalysisComplete = true;
  return Graph;
}

struct ResolverValueExpr;
using ResolverValue = std::shared_ptr<const ResolverValueExpr>;

struct ResolverScalarModelOrigin {
  RelocatedInstructionScalarModelOccurrence::ModelKind Model =
      RelocatedInstructionScalarModelOccurrence::ModelKind::
          I386ELFGOTBaseZero;
  va_t FieldVA = InvalidVA;
  va_t InstructionAddr = InvalidVA;
  int OpSeq = -1;
  uint8_t Width = 0;

  bool operator==(const ResolverScalarModelOrigin &Other) const = default;
};

struct ResolverValueExpr {
  enum class Kind : uint8_t {
    Root,
    Constant,
    Zero,
    ZeroExtend,
    SignExtend,
    Slice,
    Merge,
    Transform,
  } K = Kind::Root;
  uint16_t Size = 0;
  uint16_t SliceOffset = 0;
  uint64_t Constant = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
  uint64_t AddressOwnerVA = InvalidVA;
  std::string Root;
  NdOp Opcode = NdOp::NOP;
  bool HasOpcode = false;
  std::optional<ResolverScalarModelOrigin> ScalarModelOrigin;
  ResolverValue Input;
  std::vector<ResolverValue> Inputs;
};

static ResolverValue resolverRoot(uint16_t Size, std::string Root) {
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Root;
  E->Size = Size;
  E->Root = std::move(Root);
  return E;
}

static uint64_t resolverWidthMask(uint16_t Size) {
  if (Size == 0)
    return 0;
  if (Size >= sizeof(uint64_t))
    return std::numeric_limits<uint64_t>::max();
  return (uint64_t{1} << (Size * 8)) - 1;
}

static ResolverValue resolverConstant(uint64_t Value, uint16_t Size,
                                      ConstantAddressProvenance Provenance,
                                      uint64_t AddressOwnerVA = InvalidVA,
                                      std::optional<ResolverScalarModelOrigin>
                                          ScalarModelOrigin = std::nullopt) {
  if (Size == 0 || Size > sizeof(uint64_t))
    return {};
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Constant;
  E->Size = Size;
  E->Constant = Value & resolverWidthMask(Size);
  E->Provenance = Provenance;
  E->AddressOwnerVA = AddressOwnerVA;
  E->ScalarModelOrigin = std::move(ScalarModelOrigin);
  return E;
}

static ResolverValue resolverZero(uint16_t Size) {
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Zero;
  E->Size = Size;
  return E;
}

static ResolverValue resolverMerge(uint16_t Size, std::string Root,
                                   std::vector<ResolverValue> Inputs) {
  if (Size == 0 || Inputs.empty())
    return {};
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Merge;
  E->Size = Size;
  E->Root = std::move(Root);
  E->Inputs = std::move(Inputs);
  return E;
}

static ResolverValue
resolverTransform(uint16_t Size, std::string Root,
                  std::vector<ResolverValue> Inputs,
                  std::optional<NdOp> Opcode = std::nullopt) {
  if (Size == 0 || Inputs.empty())
    return {};
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Transform;
  E->Size = Size;
  E->Root = std::move(Root);
  if (Opcode) {
    E->Opcode = *Opcode;
    E->HasOpcode = true;
  }
  E->Inputs = std::move(Inputs);
  return E;
}

template <typename ConsumeWorkFn>
static bool consumeResolverProduct(size_t Count, size_t Cost,
                                   ConsumeWorkFn &&ConsumeWork) {
  if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count)
    return ConsumeWork(std::numeric_limits<size_t>::max());
  return ConsumeWork(Count * Cost);
}

template <typename ConsumeWorkFn>
static ResolverValue budgetedResolverRoot(uint16_t Size,
                                          std::string_view Root,
                                          ConsumeWorkFn &&ConsumeWork) {
  // Pay the shared node/control-block lifetime and the retained Root buffer
  // before either allocation.  The caller has already paid to format Root.
  if (!consumeResolverProduct(Root.size(), 2, ConsumeWork) ||
      !ConsumeWork(5))
    return {};
  return resolverRoot(Size, std::string(Root));
}

template <typename ConsumeWorkFn>
static ResolverValue budgetedResolverConstant(
    uint64_t Value, uint16_t Size, ConstantAddressProvenance Provenance,
    uint64_t AddressOwnerVA, ConsumeWorkFn &&ConsumeWork,
    std::optional<ResolverScalarModelOrigin> ScalarModelOrigin =
        std::nullopt) {
  if (!ConsumeWork(3))
    return {};
  return resolverConstant(Value, Size, Provenance, AddressOwnerVA,
                          std::move(ScalarModelOrigin));
}

template <typename ConsumeWorkFn>
static ResolverValue budgetedResolverZero(uint16_t Size,
                                          ConsumeWorkFn &&ConsumeWork) {
  if (!ConsumeWork(3))
    return {};
  return resolverZero(Size);
}

template <typename ConsumeWorkFn>
static ResolverValue budgetedResolverMerge(
    uint16_t Size, std::string_view Root, std::vector<ResolverValue> &&Inputs,
    ConsumeWorkFn &&ConsumeWork) {
  if (!consumeResolverProduct(Root.size(), 2, ConsumeWork) ||
      !ConsumeWork(5))
    return {};
  return resolverMerge(Size, std::string(Root), std::move(Inputs));
}

template <typename ConsumeWorkFn>
static ResolverValue budgetedResolverTransform(
    uint16_t Size, std::string_view Root, std::vector<ResolverValue> &&Inputs,
    std::optional<NdOp> Opcode, ConsumeWorkFn &&ConsumeWork) {
  if (!consumeResolverProduct(Root.size(), 2, ConsumeWork) ||
      !ConsumeWork(5))
    return {};
  return resolverTransform(Size, std::string(Root), std::move(Inputs),
                           Opcode);
}

struct ResolverRootKey {
  std::array<char, 256> Storage;
  size_t Length;

  std::string_view view() const { return {Storage.data(), Length}; }
};

template <typename ConsumeWorkFn>
static bool makeResolverRootKey(ResolverRootKey &Key,
                                std::string_view Prefix,
                                std::initializer_list<uint64_t> Fields,
                                ConsumeWorkFn &&ConsumeWork,
                                bool *AnalysisIncomplete = nullptr) {
  constexpr size_t MaxFieldWork = 21; // ':' plus uint64_t decimal digits.
  if (!consumeResolverProduct(Fields.size(), MaxFieldWork, ConsumeWork) ||
      !ConsumeWork(Prefix.size()))
    return false;
  if (Prefix.size() > Key.Storage.size() ||
      Fields.size() > (Key.Storage.size() - Prefix.size()) / MaxFieldWork) {
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    (void)ConsumeWork(std::numeric_limits<size_t>::max());
    return false;
  }
  std::copy(Prefix.begin(), Prefix.end(), Key.Storage.begin());
  char *Out = Key.Storage.data() + Prefix.size();
  char *const End = Key.Storage.data() + Key.Storage.size();
  for (uint64_t Field : Fields) {
    if (Out == End) {
      if (AnalysisIncomplete)
        *AnalysisIncomplete = true;
      return false;
    }
    *Out++ = ':';
    const auto Converted = std::to_chars(Out, End, Field);
    if (Converted.ec != std::errc{}) {
      if (AnalysisIncomplete)
        *AnalysisIncomplete = true;
      return false;
    }
    Out = Converted.ptr;
  }
  Key.Length = static_cast<size_t>(Out - Key.Storage.data());
  return true;
}

template <typename ConsumeWorkFn>
static bool budgetedResolverRootsEqual(const std::string &A,
                                       const std::string &B,
                                       ConsumeWorkFn &&ConsumeWork) {
  return A.size() == B.size() && ConsumeWork(A.size()) && A == B;
}

static ResolverValue resolverExtend(
    const ResolverValue &Input, uint16_t Size, bool Signed,
    const std::function<bool(size_t)> &ConsumeWork = {},
    bool *AnalysisIncomplete = nullptr) {
  std::map<const ResolverValueExpr *, ResolverValue> Memo;
  auto incomplete = [&]() {
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return ResolverValue{};
  };
  auto consume = [&](size_t Amount = 1) {
    if (!ConsumeWork || ConsumeWork(Amount))
      return true;
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return false;
  };
  auto consumeMemoLookup = [&](size_t Count) {
    return consume(orderedSetLookupWork(Count));
  };
  auto consumeMemoInsert = [&](size_t Count) {
    const size_t Lookup = orderedSetLookupWork(Count);
    if (Lookup > std::numeric_limits<size_t>::max() - 5)
      return consume(std::numeric_limits<size_t>::max());
    return consume(Lookup + 5);
  };
  auto remember = [&](const ResolverValue &Source,
                      ResolverValue Result) -> ResolverValue {
    if (!Result || !consumeMemoInsert(Memo.size()))
      return {};
    Memo.emplace(Source.get(), Result);
    return Result;
  };
  std::function<ResolverValue(const ResolverValue &, unsigned)> Extend =
      [&](const ResolverValue &Node, unsigned Depth) -> ResolverValue {
    if (Depth > limits::kMaxJumpTableGuardExpressionDepth)
      return incomplete();
    if (!consume())
      return {};
    if (!Node || Node->Size == 0 || Size < Node->Size)
      return {};
    if (Size == Node->Size)
      return Node;
    if (!consumeMemoLookup(Memo.size()))
      return {};
    if (auto It = Memo.find(Node.get()); It != Memo.end())
      return It->second;
    if (Node->K == ResolverValueExpr::Kind::Zero) {
      return remember(Node, budgetedResolverZero(Size, consume));
    }
    if (Node->K == ResolverValueExpr::Kind::Constant) {
      uint64_t Value = Node->Constant & resolverWidthMask(Node->Size);
      if (Signed && Node->Size < sizeof(uint64_t)) {
        const unsigned Bits = Node->Size * 8;
        const uint64_t Sign = uint64_t{1} << (Bits - 1);
        if (Value & Sign)
          Value |= ~resolverWidthMask(Node->Size);
      }
      return remember(
          Node, budgetedResolverConstant(
                    Value, Size, Node->Provenance, Node->AddressOwnerVA,
                    consume, Node->ScalarModelOrigin));
    }
    if (Node->K == ResolverValueExpr::Kind::Merge) {
      if (!consumeResolverProduct(Node->Inputs.size(), 2, consume) ||
          !consume(2))
        return {};
      std::vector<ResolverValue> Inputs;
      Inputs.reserve(Node->Inputs.size());
      for (const ResolverValue &Arm : Node->Inputs) {
        ResolverValue Extended = Extend(Arm, Depth + 1);
        if (!Extended || !consume())
          return {};
        Inputs.push_back(std::move(Extended));
      }
      return remember(Node, budgetedResolverMerge(
                                Size, Node->Root, std::move(Inputs), consume));
    }
    // Canonicalize consecutive extensions with the same signedness.  The
    // machine value of zext(zext(x, A), B) is zext(x, B), and likewise for two
    // sign extensions.  Keeping the redundant middle width in the symbolic
    // identity made an explicit `movzbl %al,%eax` followed by the architectural
    // EAX-to-RAX zero extension differ from a guard on AL even though an AH write
    // between them is provably non-overlapping.
    if (Node->Input &&
        ((!Signed && Node->K == ResolverValueExpr::Kind::ZeroExtend) ||
         (Signed && Node->K == ResolverValueExpr::Kind::SignExtend))) {
      ResolverValue Result = Extend(Node->Input, Depth + 1);
      return Result ? remember(Node, std::move(Result)) : ResolverValue{};
    }
    if (!consume(4))
      return {};
    auto E = std::make_shared<ResolverValueExpr>();
    E->K = Signed ? ResolverValueExpr::Kind::SignExtend
                  : ResolverValueExpr::Kind::ZeroExtend;
    E->Size = Size;
    E->Input = Node;
    return remember(Node, std::move(E));
  };
  return Extend(Input, 0);
}

static bool isRoleNeutralNumericOccurrence(const ResolverValue &Value) {
  return Value && Value->K == ResolverValueExpr::Kind::Constant &&
         Value->Provenance == ConstantAddressProvenance::Unknown &&
         Value->AddressOwnerVA == InvalidVA && !Value->ScalarModelOrigin &&
         !Value->Input && Value->Inputs.empty() && Value->Root.empty();
}

template <typename SameAllowedValueFn, typename ConsumeWorkFn>
static bool resolverNumericOccurrenceExtensionMatches(
    const ResolverValue &Value, const ResolverValue &Allowed, bool Signed,
    bool RequireExactAddressOwner, SameAllowedValueFn &&SameAllowedValue,
    ConsumeWorkFn &&ConsumeWork, bool *AnalysisIncomplete = nullptr) {
  if (!Value || !Allowed || Value->Size == Allowed->Size)
    return false;

  const bool ValueIsSmaller = Value->Size < Allowed->Size;
  const ResolverValue &Smaller = ValueIsSmaller ? Value : Allowed;
  const ResolverValue &Larger = ValueIsSmaller ? Allowed : Value;
  if (!isRoleNeutralNumericOccurrence(Smaller))
    return false;

  // Decoder literals are role-neutral until an operation consumes them.  At
  // this exact numeric query, promote only the plain leaf occurrence to the
  // scalar role before applying the query-authorized virtual extension.  Do
  // not mutate the original expression: Unknown-to-Unknown architectural
  // transports must continue to use the ordinary structural path above.
  ResolverValue Scalar = budgetedResolverConstant(
      Smaller->Constant, Smaller->Size,
      ConstantAddressProvenance::Scalar, InvalidVA, ConsumeWork);
  if (!Scalar) {
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return false;
  }
  ResolverValue Extended = resolverExtend(
      std::move(Scalar), Larger->Size, Signed,
      std::forward<ConsumeWorkFn>(ConsumeWork), AnalysisIncomplete);
  if (!Extended)
    return false;
  return ValueIsSmaller
             ? SameAllowedValue(Extended, Allowed, RequireExactAddressOwner)
             : SameAllowedValue(Value, Extended, RequireExactAddressOwner);
}

static ResolverValue resolverSlice(
    const ResolverValue &Input, uint16_t Offset, uint16_t Size,
    const std::function<bool(size_t)> &ConsumeWork = {},
    bool *AnalysisIncomplete = nullptr) {
  using SliceMemoKey = std::pair<const ResolverValueExpr *, uint16_t>;
  std::map<SliceMemoKey, ResolverValue> Memo;
  auto incomplete = [&]() {
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return ResolverValue{};
  };
  auto consume = [&](size_t Amount = 1) {
    if (!ConsumeWork || ConsumeWork(Amount))
      return true;
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return false;
  };
  constexpr size_t SliceMemoKeyWork = 2;
  auto consumeMemoLookup = [&](size_t Count) {
    const size_t Lookup = orderedSetLookupWork(Count);
    if (Lookup > std::numeric_limits<size_t>::max() / SliceMemoKeyWork)
      return consume(std::numeric_limits<size_t>::max());
    return consume(SliceMemoKeyWork * Lookup);
  };
  auto consumeMemoInsert = [&](size_t Count) {
    const size_t Lookup = orderedSetLookupWork(Count);
    constexpr size_t RetainedWork = SliceMemoKeyWork * 2 + 3;
    if (Lookup >
        (std::numeric_limits<size_t>::max() - RetainedWork) /
            SliceMemoKeyWork)
      return consume(std::numeric_limits<size_t>::max());
    return consume(SliceMemoKeyWork * Lookup + RetainedWork);
  };
  auto remember = [&](const ResolverValue &Source, uint16_t CurrentOffset,
                      ResolverValue Result) -> ResolverValue {
    if (!Result || !consumeMemoInsert(Memo.size()))
      return {};
    Memo.emplace(SliceMemoKey{Source.get(), CurrentOffset}, Result);
    return Result;
  };
  std::function<ResolverValue(const ResolverValue &, uint16_t, unsigned)> Slice =
      [&](const ResolverValue &Node, uint16_t CurrentOffset,
          unsigned Depth) -> ResolverValue {
    if (Depth > limits::kMaxJumpTableGuardExpressionDepth)
      return incomplete();
    if (!consume())
      return {};
    if (!Node || Size == 0 ||
        uint32_t(CurrentOffset) + Size > Node->Size)
      return {};
    if (CurrentOffset == 0 && Size == Node->Size)
      return Node;
    const SliceMemoKey MemoKey{Node.get(), CurrentOffset};
    if (!consumeMemoLookup(Memo.size()))
      return {};
    if (auto It = Memo.find(MemoKey); It != Memo.end())
      return It->second;
    if (Node->K == ResolverValueExpr::Kind::Zero) {
      return remember(Node, CurrentOffset,
                      budgetedResolverZero(Size, consume));
    }
    if (Node->K == ResolverValueExpr::Kind::Constant) {
      const uint64_t SlicedConstant =
          CurrentOffset >= sizeof(uint64_t)
              ? 0
              : Node->Constant >> (unsigned(CurrentOffset) * 8u);
      ConstantAddressProvenance Provenance = Node->Provenance;
      uint64_t Owner = Node->AddressOwnerVA;
      if (CurrentOffset != 0 || Size != Node->Size) {
        if (isAddressProvenance(Provenance))
          Provenance = ConstantAddressProvenance::AddressFragment;
        Owner = InvalidVA;
      }
      return remember(
          Node, CurrentOffset,
          budgetedResolverConstant(SlicedConstant, Size, Provenance, Owner,
                                   consume));
    }
    if (Node->K == ResolverValueExpr::Kind::Merge) {
      if (!consumeResolverProduct(Node->Inputs.size(), 2, consume) ||
          !consume(2))
        return {};
      std::vector<ResolverValue> Inputs;
      Inputs.reserve(Node->Inputs.size());
      for (const ResolverValue &Arm : Node->Inputs) {
        ResolverValue Sliced = Slice(Arm, CurrentOffset, Depth + 1);
        if (!Sliced || !consume())
          return {};
        Inputs.push_back(std::move(Sliced));
      }
      return remember(
          Node, CurrentOffset,
          budgetedResolverMerge(Size, Node->Root, std::move(Inputs), consume));
    }
    if (CurrentOffset == 0 && Node->Input &&
        (Node->K == ResolverValueExpr::Kind::ZeroExtend ||
         Node->K == ResolverValueExpr::Kind::SignExtend) &&
        Size == Node->Input->Size)
      return remember(Node, CurrentOffset, Node->Input);
    if (Node->K == ResolverValueExpr::Kind::Slice && Node->Input) {
      const uint32_t CombinedOffset =
          uint32_t(Node->SliceOffset) + CurrentOffset;
      if (CombinedOffset > std::numeric_limits<uint16_t>::max())
        return incomplete();
      ResolverValue Result = Slice(Node->Input,
                                   static_cast<uint16_t>(CombinedOffset),
                                   Depth + 1);
      return Result ? remember(Node, CurrentOffset, std::move(Result))
                    : ResolverValue{};
    }
    if (!consume(4))
      return {};
    auto E = std::make_shared<ResolverValueExpr>();
    E->K = ResolverValueExpr::Kind::Slice;
    E->Size = Size;
    E->SliceOffset = CurrentOffset;
    E->Input = Node;
    return remember(Node, CurrentOffset, std::move(E));
  };
  return Slice(Input, Offset, 0);
}

/// Prove the complete unsigned remainder recipe emitted for division by a
/// constant.  This is deliberately a structural theorem over the expression
/// produced by the point-sensitive resolver, not a lexical recognition of a
/// multiply and shift: direct URem, explicit UDiv, and LLVM's reciprocal
/// quotient lowering each require the exact scalar N, while the latter two
/// require the same dividend expression in the quotient and `x - q*N`.
/// Consequently a sibling quotient, a predicated reaching definition, or one
/// wrong magic/shift bit cannot authorize a table domain.
static bool provesExactUnsignedModuloRecipe(
    symbolic::SymContext &Ctx, symbolic::SymRef Index, uint64_t Divisor,
    const std::function<bool(size_t)> &ConsumeWork = {},
    bool *AnalysisIncomplete = nullptr) {
  using symbolic::SymOp;
  using symbolic::SymRef;

  auto consume = [&](size_t Amount = 1) {
    if (!ConsumeWork || ConsumeWork(Amount))
      return true;
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return false;
  };

  if (!Index || Divisor < limits::kMinJumpTableEntries)
    return false;

  // Table selectors are commonly widened to the internal address container
  // after the machine-width remainder.  Only zero extension preserves the
  // unsigned domain theorem; sign extension and arbitrary truncation do not.
  SymRef Remainder = Index;
  for (;;) {
    if (!consume())
      return false;
    if (Ctx.op(Remainder) != SymOp::ZExt)
      break;
    if (Ctx.numOperands(Remainder) != 1)
      return false;
    Remainder = Ctx.operand(Remainder, 0);
  }
  const uint32_t Width = Ctx.width(Remainder);
  if (Width < 2 || Width > 64)
    return false;
  if (Width < 64 && Divisor >= (uint64_t{1} << Width))
    return false;

  // At -O0 the exact machine operation can survive directly as urem.  It is
  // the same occurrence-level theorem as the reciprocal lowering below: the
  // divisor must be this scalar N in the current CFG snapshot, while the
  // dividend remains an arbitrary bit-vector.  Never admit srem here; a
  // negative signed remainder does not establish an unsigned table domain.
  if (Ctx.op(Remainder) == SymOp::URem &&
      Ctx.numOperands(Remainder) == 2) {
    const SymRef CandidateDivisor = Ctx.operand(Remainder, 1);
    const std::optional<llvm::APInt> ExactDivisor =
        Ctx.asConst(CandidateDivisor);
    if (ExactDivisor && ExactDivisor->getBitWidth() == Width &&
        ExactDivisor->getActiveBits() <= 64 &&
        ExactDivisor->getZExtValue() == Divisor)
      return true;
  }

  // Builders below intern comparison forms into the same context.  Restrict
  // structural searches to nodes that came from the resolved selector so a
  // failed candidate cannot seed a later proof with theorem-created nodes.
  const size_t ExpressionNodeCount = Ctx.numNodes();
  if (ExpressionNodeCount > std::numeric_limits<uint32_t>::max()) {
    if (AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return false;
  }

  auto consumeSortWork = [&](size_t Count) {
    const size_t Levels =
        Count > 1 ? llvm::Log2_64_Ceil(static_cast<uint64_t>(Count)) : 0;
    const size_t Cost = Levels + 3;
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count)
      return consume(std::numeric_limits<size_t>::max());
    return consume(Count * Cost);
  };
  // mkAdd/mkMul flatten nested n-ary nodes, build ordered coefficient sets,
  // and sort retained operands.  Count the same appearance tree before calling
  // a canonical builder; a depth overflow is incomplete evidence, never
  // permission for hidden unbounded work.
  std::function<bool(SymRef, SymOp, unsigned, size_t &)>
      consumeCanonicalOperand =
          [&](SymRef Value, SymOp Builder, unsigned Depth, size_t &Leaves) {
            if (!Value || Depth > limits::kMaxJumpTableGuardExpressionDepth) {
              if (AnalysisIncomplete)
                *AnalysisIncomplete = true;
              return false;
            }
            if (!consume())
              return false;
            if (Ctx.op(Value) == Builder) {
              const llvm::ArrayRef<SymRef> Children = Ctx.operands(Value);
              if (!consume(Children.size()))
                return false;
              for (SymRef Child : Children)
                if (!consumeCanonicalOperand(Child, Builder, Depth + 1, Leaves))
                  return false;
              return true;
            }
            if (Leaves == std::numeric_limits<size_t>::max())
              return consume(std::numeric_limits<size_t>::max());
            ++Leaves;
            // Addition's coefficient splitter rebuilds the non-constant tail of
            // a product with three or more factors.  Prepay that nested mul's
            // walk and sort as part of the same builder transaction.
            if (Builder == SymOp::Add && Ctx.op(Value) == SymOp::Mul &&
                Ctx.numOperands(Value) >= 3) {
              const size_t Tail = Ctx.numOperands(Value) - 1;
              if (!consume(Tail) || !consumeSortWork(Tail))
                return false;
            }
            return true;
          };
  auto consumeCanonicalBuilder = [&](SymOp Builder,
                                     llvm::ArrayRef<SymRef> Inputs) {
    size_t Leaves = 0;
    if (!consume(Inputs.size()))
      return false;
    for (SymRef Input : Inputs)
      if (!consumeCanonicalOperand(Input, Builder, 0, Leaves))
        return false;
    return consumeSortWork(Leaves);
  };
  auto mkAddBudgeted = [&](llvm::ArrayRef<SymRef> Inputs) -> SymRef {
    return consumeCanonicalBuilder(SymOp::Add, Inputs) ? Ctx.mkAdd(Inputs)
                                                       : SymRef{};
  };
  auto mkMulBudgeted = [&](llvm::ArrayRef<SymRef> Inputs) -> SymRef {
    return consumeCanonicalBuilder(SymOp::Mul, Inputs) ? Ctx.mkMul(Inputs)
                                                       : SymRef{};
  };
  auto mkAdd2Budgeted = [&](SymRef A, SymRef B) -> SymRef {
    const SymRef Inputs[] = {A, B};
    return mkAddBudgeted(Inputs);
  };
  auto mkMul2Budgeted = [&](SymRef A, SymRef B) -> SymRef {
    const SymRef Inputs[] = {A, B};
    return mkMulBudgeted(Inputs);
  };
  auto mkSubBudgeted = [&](SymRef A, SymRef B) -> SymRef {
    const SymRef AddInputs[] = {A, B};
    const SymRef MulInputs[] = {B};
    if (!consumeCanonicalBuilder(SymOp::Mul, MulInputs) ||
        !consumeCanonicalBuilder(SymOp::Add, AddInputs))
      return {};
    return Ctx.mkSub(A, B);
  };
  auto consumeExtractBuilder = [&](SymRef Value) {
    // mkExtract can recursively collapse Extract/ZExt chains and, for a
    // Concat, walk every part before rebuilding the kept slice.  Every concat
    // operand has positive width, so ValueWidth bounds the fan-out; arena size
    // bounds every recursive chain.  Prepay their product (plus the rebuild
    // pass) before entering the canonical builder.
    const size_t ArenaNodes = Ctx.numNodes();
    const size_t ValueWidth = Ctx.width(Value);
    const size_t Max = std::numeric_limits<size_t>::max();
    if (ArenaNodes != 0 && ValueWidth > Max / ArenaNodes)
      return consume(Max);
    const size_t RecursiveWork = ArenaNodes * ValueWidth;
    if (RecursiveWork > Max - ValueWidth)
      return consume(Max);
    return consume(RecursiveWork + ValueWidth);
  };
  auto shiftRight = [&](SymRef Value, uint32_t Amount) -> SymRef {
    if (Amount == 0)
      return Value;
    if (!consume(2))
      return {};
    return Ctx.mkLShr(Value, Ctx.mkConst(Ctx.width(Value), Amount));
  };
  auto ensureAppendCapacity = [&](auto &Values, size_t Additional = 1) {
    if (Additional > std::numeric_limits<size_t>::max() - Values.size()) {
      consume(std::numeric_limits<size_t>::max());
      return false;
    }
    const size_t Required = Values.size() + Additional;
    if (Required <= Values.capacity())
      return true;
    size_t NewCapacity = Values.capacity() == 0 ? 1 : Values.capacity();
    while (NewCapacity < Required) {
      if (NewCapacity > std::numeric_limits<size_t>::max() / 2) {
        NewCapacity = Required;
        break;
      }
      NewCapacity *= 2;
    }
    if (NewCapacity < Required ||
        NewCapacity >
            (std::numeric_limits<size_t>::max() - Values.size()) / 2) {
      consume(std::numeric_limits<size_t>::max());
      return false;
    }
    if (!consume(NewCapacity * 2 + Values.size()))
      return false;
    Values.reserve(NewCapacity);
    return true;
  };
  auto addUnique = [&](std::vector<SymRef> &Values, SymRef Value) {
    if (!Value)
      return true;
    if (!consume(Values.size()))
      return false;
    if (std::find(Values.begin(), Values.end(), Value) == Values.end()) {
      if (!ensureAppendCapacity(Values) || !consume())
        return false;
      Values.push_back(Value);
    }
    return true;
  };

  // AArch64 and other -O0 paths can preserve the exact machine identity
  //
  //   x - (x /u N) * N.
  //
  // Search only UDiv factors of the selector's top-level subtraction term,
  // rebuild that one identity with the context's canonical arithmetic, and
  // require node equality with the selector.  This keeps reciprocal recipes
  // on their established budget path while admitting commuted multiplication
  // and rejecting SDiv/SRem, a foreign dividend, or either scalar differing
  // from N; it is not a generic range or algebraic-equivalence fallback.
  auto matchesExplicitUnsignedDivision = [&](SymRef Quotient) {
    if (Ctx.op(Quotient) != SymOp::UDiv ||
        Ctx.width(Quotient) != Width || Ctx.numOperands(Quotient) != 2)
      return false;
    const SymRef Dividend = Ctx.operand(Quotient, 0);
    const SymRef CandidateDivisor = Ctx.operand(Quotient, 1);
    const std::optional<llvm::APInt> ExactDivisor =
        Ctx.asConst(CandidateDivisor);
    if (!ExactDivisor || Ctx.width(Dividend) != Width ||
        ExactDivisor->getBitWidth() != Width ||
        ExactDivisor->getActiveBits() > 64 ||
        ExactDivisor->getZExtValue() != Divisor)
      return false;
    const SymRef Product = mkMul2Budgeted(Quotient, CandidateDivisor);
    const SymRef Expected =
        Product ? mkSubBudgeted(Dividend, Product) : SymRef{};
    if (!Expected)
      return false;
    return Expected == Remainder;
  };
  std::vector<SymRef> ExplicitQuotients;
  if (!consume(2))
    return false;
  if (Ctx.op(Remainder) == SymOp::Add) {
    const llvm::ArrayRef<SymRef> Terms = Ctx.operands(Remainder);
    if (!consume(Terms.size()))
      return false;
    for (SymRef Term : Terms) {
      if (!consume())
        return false;
      if (Ctx.op(Term) != SymOp::Mul)
        continue;
      const llvm::ArrayRef<SymRef> Factors = Ctx.operands(Term);
      if (!consume(Factors.size()))
        return false;
      for (SymRef Factor : Factors) {
        if (!consume())
          return false;
        if (Ctx.op(Factor) == SymOp::UDiv) {
          if (!ensureAppendCapacity(ExplicitQuotients) || !consume())
            return false;
          ExplicitQuotients.push_back(Factor);
        }
      }
    }
  }
  // operands() borrows the context's shared operand pool.  Compare only after
  // every candidate ref has been copied: the canonical builders may intern new
  // nodes and reallocate that pool when a candidate is a completed negative.
  for (SymRef Quotient : ExplicitQuotients) {
    if (!consume())
      return false;
    if (matchesExplicitUnsignedDivision(Quotient))
      return true;
  }

  if (Width > 32) {
    // The two exact machine identities above need no widened reciprocal
    // construction and therefore remain valid for a 64-bit selector.  The
    // compiler-reciprocal theorem below uses a bounded 2W-bit full product and
    // is deliberately limited to widths whose double fits this symbolizer's
    // 64-bit bit-vector envelope.
    return false;
  }

  const llvm::APInt D(Width, Divisor, /*isSigned=*/false,
                      /*implicitTrunc=*/true);
  if (D.isZero() || D.isOne())
    return false;
  const llvm::UnsignedDivisionByConstantInfo Magic =
      llvm::UnsignedDivisionByConstantInfo::get(
          D, /*LeadingZeros=*/0, /*AllowEvenDivisorOptimization=*/true,
          /*AllowWidenOptimization=*/false);
  if (Magic.Widen || Magic.Magic.getBitWidth() != Width ||
      Magic.PreShift >= Width || Magic.PostShift >= Width)
    return false;

  const uint32_t WideWidth = Width * 2;

  struct QuotientForm {
    SymRef Narrow;
    SymRef Wide;
  };
  auto addQuotient = [&](std::vector<QuotientForm> &Forms, SymRef Narrow,
                         SymRef Wide) {
    if (!Narrow || !Wide || Ctx.width(Narrow) != Width ||
        Ctx.width(Wide) != WideWidth)
      return true;
    if (!consume(Forms.size()))
      return false;
    if (std::none_of(Forms.begin(), Forms.end(), [&](const QuotientForm &F) {
          return F.Narrow == Narrow && F.Wide == Wide;
        })) {
      if (!ensureAppendCapacity(Forms) || !consume())
        return false;
      Forms.push_back({Narrow, Wide});
    }
    return true;
  };

  // Low-bit extraction is a ring homomorphism for addition and
  // multiplication.  Backends may therefore spell q*N in a wider address
  // container (including factored LEAs) before taking the low machine word,
  // while the theorem below constructs the equivalent W-bit product.  Reduce
  // only those exact ring operations through low extracts/zexts; every other
  // wide expression stays an opaque, exact low-extract leaf.  This is a
  // structural normalization, not a numeric range approximation.
  // SymRef is a stable arena index.  Index the memo directly instead of using
  // an ordered map whose hidden logarithmic find/emplace work would escape the
  // exact-recipe account.  Resize is prepaid before allocation; nodes interned
  // by normalization are incorporated lazily on their first visit.
  std::vector<SymRef> LowRingMemo;
  if (!consume(2))
    return false;
  std::function<SymRef(SymRef, unsigned)> normalizeLowRing =
      [&](SymRef Value, unsigned Depth) -> SymRef {
    if (!Value || Depth > limits::kMaxJumpTableGuardExpressionDepth) {
      if (AnalysisIncomplete)
        *AnalysisIncomplete = true;
      return {};
    }
    if (!consume())
      return {};
    if (Value.index() >= LowRingMemo.size()) {
      const size_t NodeCount = Ctx.numNodes();
      if (Value.index() >= NodeCount)
        return {};
      const size_t AddedNodes = NodeCount - LowRingMemo.size();
      if (!ensureAppendCapacity(LowRingMemo, AddedNodes) ||
          !consume(AddedNodes))
        return {};
      LowRingMemo.resize(NodeCount);
    }
    if (LowRingMemo[Value.index()])
      return LowRingMemo[Value.index()];

    const uint32_t ValueWidth = Ctx.width(Value);
    if (ValueWidth < Width)
      return {};
    SymRef Result;
    const SymOp Op = Ctx.op(Value);
    if (Ctx.isConst(Value)) {
      if (!consume())
        return {};
      Result = Ctx.mkConst(Ctx.constValue(Value).zextOrTrunc(Width));
    } else if ((Op == SymOp::ZExt || Op == SymOp::SExt) &&
               Ctx.numOperands(Value) == 1 &&
               Ctx.width(Ctx.operand(Value, 0)) == Width) {
      Result = Ctx.operand(Value, 0);
    } else if (Op == SymOp::Shl && ValueWidth == Width &&
               Ctx.numOperands(Value) == 2) {
      // A width-preserving constant left shift is multiplication by an exact
      // power of two in the same modulo-2^W ring.  Backends routinely use this
      // spelling inside factored q*N back-multiplies (for example
      // `(q << 2) * 3` for q*12).  Normalize only an in-range scalar constant;
      // a variable, oversized, or differently sized shift remains opaque and
      // therefore cannot authenticate a modulo domain.
      const SymRef Input = Ctx.operand(Value, 0);
      const SymRef Amount = Ctx.operand(Value, 1);
      const std::optional<llvm::APInt> Shift = Ctx.asConst(Amount);
      if (!Shift || Shift->getActiveBits() > 64 ||
          Shift->getZExtValue() >= Width)
        return {};
      SymRef Low = normalizeLowRing(Input, Depth + 1);
      if (!Low || !consume(2))
        return {};
      const llvm::APInt Factor =
          llvm::APInt::getOneBitSet(Width,
                                    static_cast<unsigned>(Shift->getZExtValue()));
      Result = mkMul2Budgeted(Low, Ctx.mkConst(Factor));
    } else if ((Op == SymOp::Add || Op == SymOp::Mul) && ValueWidth >= Width) {
      const size_t OperandCount = Ctx.numOperands(Value);
      if (OperandCount == 0 || !consume(OperandCount))
        return {};
      std::vector<SymRef> Reduced;
      if (!consume(2) || !ensureAppendCapacity(Reduced, OperandCount))
        return {};
      for (size_t OperandIndex = 0; OperandIndex < OperandCount;
           ++OperandIndex) {
        const SymRef Operand = Ctx.operand(Value, OperandIndex);
        SymRef Low = normalizeLowRing(Operand, Depth + 1);
        if (!Low || !consume())
          return {};
        Reduced.push_back(Low);
      }
      if (!consume())
        return {};
      Result =
          Op == SymOp::Add ? mkAddBudgeted(Reduced) : mkMulBudgeted(Reduced);
    } else if (Op == SymOp::Extract && Ctx.numOperands(Value) == 1 &&
               Ctx.node(Value).Aux == 0 && ValueWidth == Width) {
      Result = normalizeLowRing(Ctx.operand(Value, 0), Depth + 1);
    } else if (ValueWidth == Width) {
      Result = Value;
    } else {
      if (!consumeExtractBuilder(Value))
        return {};
      Result = Ctx.mkExtract(Value, 0, Width);
    }
    if (!Result || !consume())
      return {};
    LowRingMemo[Value.index()] = Result;
    return Result;
  };

  for (size_t NodeIndex = 0; NodeIndex < ExpressionNodeCount; ++NodeIndex) {
    if (!consume())
      return false;
    SymRef Dividend(static_cast<uint32_t>(NodeIndex));
    if (Ctx.width(Dividend) != Width || Ctx.isConst(Dividend))
      continue;

    SymRef DivInput = shiftRight(Dividend, Magic.PreShift);
    if (!DivInput || !consume(3))
      return false;
    SymRef WideInput = Ctx.mkZExt(DivInput, WideWidth);
    SymRef WideMagic = Ctx.mkConst(Magic.Magic.zextOrTrunc(WideWidth));
    SymRef FullProduct = mkMul2Budgeted(WideMagic, WideInput);
    if (!FullProduct)
      return false;

    // Backends spell mulhi either as extract(full, W, W), or as a logical
    // shift followed by a low extract.  Keep both exact forms; no algebraic
    // approximation or numeric coincidence is accepted.
    std::vector<SymRef> HighForms;
    if (!consume(2) || !consume())
      return false;
    SymRef DirectHigh = Ctx.mkExtract(FullProduct, Width, Width);
    SymRef ShiftedProduct = shiftRight(FullProduct, Width);
    if (!ShiftedProduct || !consume())
      return false;
    SymRef ShiftedHigh = Ctx.mkExtract(ShiftedProduct, 0, Width);
    if (!addUnique(HighForms, DirectHigh) || !addUnique(HighForms, ShiftedHigh))
      return false;

    std::vector<QuotientForm> Quotients;
    if (!consume(2))
      return false;
    if (!Magic.IsAdd) {
      for (SymRef High : HighForms) {
        SymRef Narrow = shiftRight(High, Magic.PostShift);
        if (!Narrow || !consume())
          return false;
        if (!addQuotient(Quotients, Narrow, Ctx.mkZExt(Narrow, WideWidth)))
          return false;
      }
      // x86 commonly performs the post-shift directly on the full widened
      // product and keeps that W-bit quotient in a wide register until the
      // low-width back-multiply.
      SymRef Wide = shiftRight(FullProduct, Width + Magic.PostShift);
      if (!Wide || !consume())
        return false;
      if (!addQuotient(Quotients, Ctx.mkExtract(Wide, 0, Width), Wide))
        return false;
    } else {
      if (Magic.PreShift != 0)
        continue; // LLVM's IsAdd recipe and pre-shift are mutually exclusive.
      for (SymRef High : HighForms) {
        if (!consume())
          return false;
        SymRef Difference = mkSubBudgeted(DivInput, High);
        SymRef HalfDifference =
            Difference ? shiftRight(Difference, 1) : SymRef{};
        if (!HalfDifference || !consume())
          return false;
        SymRef Adjusted = mkAdd2Budgeted(High, HalfDifference);
        if (!Adjusted)
          return false;
        SymRef Narrow = shiftRight(Adjusted, Magic.PostShift);
        if (!Narrow || !consume())
          return false;
        if (!addQuotient(Quotients, Narrow, Ctx.mkZExt(Narrow, WideWidth)))
          return false;
      }
    }

    auto lowWideProduct = [&](uint64_t Factor, SymRef WideQuotient) -> SymRef {
      if (!consume(3))
        return {};
      SymRef Product =
          mkMul2Budgeted(Ctx.mkConst(WideWidth, Factor), WideQuotient);
      if (!Product)
        return {};
      return Ctx.mkExtract(Product, 0, Width);
    };
    auto matchesRemainder = [&](const QuotientForm &Q) {
      if (!consume(3))
        return false;
      SymRef NarrowProduct =
          mkMul2Budgeted(Ctx.mkConst(Width, Divisor), Q.Narrow);
      SymRef NarrowRemainder =
          NarrowProduct ? mkSubBudgeted(Dividend, NarrowProduct) : SymRef{};
      SymRef WideProduct = lowWideProduct(Divisor, Q.Wide);
      if (!NarrowRemainder || !WideProduct || !consume())
        return false;
      SymRef WideRemainder = mkSubBudgeted(Dividend, WideProduct);
      if (!WideRemainder)
        return false;
      if (NarrowRemainder == Remainder || WideRemainder == Remainder)
        return true;

      SymRef NormalizedActual = normalizeLowRing(Remainder, 0);
      SymRef NormalizedNarrow = normalizeLowRing(NarrowRemainder, 0);
      SymRef NormalizedWide = normalizeLowRing(WideRemainder, 0);
      if (!NormalizedActual || !NormalizedNarrow || !NormalizedWide)
        return false;
      if (NormalizedActual == NormalizedNarrow ||
          NormalizedActual == NormalizedWide)
        return true;

      // LEA often realizes q*(2^k-1) as q*2^k-q in the widened address
      // container.  Match that exact identity rather than treating an
      // arbitrary linear tree as a modulus.
      const uint64_t Above = llvm::PowerOf2Ceil(Divisor);
      if (Above > Divisor && Above - Divisor == 1) {
        SymRef Product = lowWideProduct(Above, Q.Wide);
        if (!Product || !consume(2))
          return false;
        SymRef Sum = mkAdd2Budgeted(Dividend, Q.Narrow);
        SymRef Expected = Sum ? mkSubBudgeted(Sum, Product) : SymRef{};
        if (Expected == Remainder)
          return true;
      }
      if (Divisor > 1 && llvm::isPowerOf2_64(Divisor - 1)) {
        const uint64_t Below = Divisor - 1;
        SymRef Product = lowWideProduct(Below, Q.Wide);
        if (!Product || !consume(2))
          return false;
        SymRef Sum = mkAdd2Budgeted(Q.Narrow, Product);
        SymRef Expected = Sum ? mkSubBudgeted(Dividend, Sum) : SymRef{};
        if (Expected == Remainder)
          return true;
      }
      return false;
    };
    if (std::any_of(Quotients.begin(), Quotients.end(), matchesRemainder))
      return true;
  }
  return false;
}

enum class ResolverResultKind : uint8_t { Invalid, Cycle, Value };
struct ResolverResult {
  ResolverResultKind Kind = ResolverResultKind::Invalid;
  ResolverValue Value;
};

static ResolverResult resolverInvalid() { return {}; }
static ResolverResult resolverCycle() {
  return {ResolverResultKind::Cycle, {}};
}
static ResolverResult resolverValue(ResolverValue Value) {
  return Value ? ResolverResult{ResolverResultKind::Value, std::move(Value)}
               : resolverInvalid();
}

static ResolverResult
mergeResolverResults(llvm::ArrayRef<ResolverResult> Incoming,
                     std::string_view MergeRoot = {},
                     bool IgnoreTransparentCycles = false,
                     const std::function<bool(size_t)> &ConsumeWork = {},
                     bool *AnalysisIncomplete = nullptr) {
  bool WorkExhausted = false;
  auto consume = [&](size_t Amount = 1) {
    const bool Available = !ConsumeWork || ConsumeWork(Amount);
    WorkExhausted |= !Available;
    if (!Available && AnalysisIncomplete)
      *AnalysisIncomplete = true;
    return Available;
  };
  std::vector<ResolverValue> Values;
  if (Incoming.size() >
      (std::numeric_limits<size_t>::max() - 2) / 2) {
    consume(std::numeric_limits<size_t>::max());
    return resolverInvalid();
  }
  // Reserve the result vector transactionally.  The vector buffer lifetime
  // and eventual element cleanup are paid before the first push; each actual
  // retained shared value is charged separately below.
  if (!consume(Incoming.size() * 2 + 2))
    return resolverInvalid();
  Values.reserve(Incoming.size());
  bool SawValue = false;
  bool SawCycle = false;
  for (const ResolverResult &R : Incoming) {
    if (!consume())
      return resolverInvalid();
    if (R.Kind == ResolverResultKind::Cycle) {
      SawCycle = true;
      continue;
    }
    if (R.Kind != ResolverResultKind::Value || !R.Value)
      return resolverInvalid();
    if (!consume())
      return resolverInvalid();
    SawValue = true;
    Values.push_back(R.Value);
  }
  if (!SawValue)
    return resolverCycle();
  // A raw predecessor cycle is transparent only when the caller reached it
  // without crossing any overlapping definition.  Definitions propagate a
  // cycle as Invalid below (except an exact self-copy), so this opt-in cannot
  // erase a loop-carried SELECT/arithmetic/partial-lane update.
  if (SawCycle && !IgnoreTransparentCycles)
    return resolverInvalid();
  const ResolverValue &Common = Values.front();
  std::function<bool(const ResolverValue &, const ResolverValue &, unsigned)>
      Same = [&](const ResolverValue &A, const ResolverValue &B,
                 unsigned Depth) {
        if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
          WorkExhausted = true;
          if (AnalysisIncomplete)
            *AnalysisIncomplete = true;
          return false;
        }
        if (!consume())
          return false;
        if (A == B)
          return true;
        if (!A || !B || A->K != B->K || A->Size != B->Size ||
            A->SliceOffset != B->SliceOffset || A->Constant != B->Constant ||
            A->Provenance != B->Provenance ||
            A->AddressOwnerVA != B->AddressOwnerVA)
          return false;
        if (!budgetedResolverRootsEqual(A->Root, B->Root, consume) ||
            A->Opcode != B->Opcode ||
            A->HasOpcode != B->HasOpcode ||
            A->ScalarModelOrigin != B->ScalarModelOrigin ||
            A->Inputs.size() != B->Inputs.size())
          return false;
        if (!Same(A->Input, B->Input, Depth + 1))
          return false;
        for (size_t I = 0; I < A->Inputs.size(); ++I)
          if (!Same(A->Inputs[I], B->Inputs[I], Depth + 1))
            return false;
        return true;
      };
  if (std::all_of(Values.begin(), Values.end(),
                  [&](const ResolverValue &V) {
                    return Same(Common, V, /*Depth=*/0);
                  }))
    return resolverValue(std::move(Values.front()));
  if (WorkExhausted)
    return resolverInvalid();
  if (MergeRoot.empty())
    return resolverInvalid();
  const uint16_t Size = Common ? Common->Size : 0;
  if (!consume(Values.size()) || Size == 0 ||
      std::any_of(Values.begin(), Values.end(), [&](const ResolverValue &V) {
        return !V || V->Size != Size;
      }))
    return resolverInvalid();
  return resolverValue(
      budgetedResolverMerge(Size, MergeRoot, std::move(Values), consume));
}

} // namespace

//===----------------------------------------------------------------------===//
// sliceBackForTableBase — backward data-flow slicing
//===----------------------------------------------------------------------===//

bool CFGBuilder::sliceBackForTableBase(const InsnRecord &Rec,
                                       JumpTableInfo &Info) {
  bool FoundBase = false;
  bool FoundSize = false;
  bool SawLoad = false;
  uint16_t LoadWidth = 0;

  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;

    int Depth = 0;
    for (int J = I - 1; J >= 0 && Depth < limits::kMaxSliceDepth; --J) {
      ++Depth;
      auto &Op = Rec.Ops[J];
      switch (Op.Opcode) {
      case NdOp::INT_ADD:
        if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
            Op.Inputs[1].Offset != 0) {
          Info.setBaseAddr(Op.Inputs[1].Offset);
          FoundBase = true;
        } else if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[0].isConst() &&
                   Op.Inputs[0].Offset != 0) {
          Info.setBaseAddr(Op.Inputs[0].Offset);
          FoundBase = true;
        }
        break;

      case NdOp::INT_MULT:
        if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          Info.EntrySize = static_cast<uint16_t>(Op.Inputs[1].Offset);
          FoundSize = true;
        }
        break;

      case NdOp::INT_LEFT:
        if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          uint64_t Shift = Op.Inputs[1].Offset;
          if (Shift <= limits::kMaxShiftForEntrySize) {
            Info.EntrySize = static_cast<uint16_t>(1u << Shift);
            FoundSize = true;
          }
        }
        break;

      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR:
        if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
          uint64_t Shift = Op.Inputs[1].Offset;
          if (Shift <= limits::kMaxShiftForEntrySize && Op.Output.Size > 0) {
            Info.EntrySize = Op.Output.Size;
            FoundSize = true;
          }
        }
        break;

      case NdOp::INT_SEXT:
        Info.IsSigned = true;
        Info.IsRelative = true;
        break;

      case NdOp::INT_ZEXT:
        if (!Info.IsSigned)
          Info.IsRelative = true;
        break;

      case NdOp::SUBBYTES:
        if (!FoundSize && Op.Output.Size > 0 &&
            Op.Output.Size <= limits::kMaxEntryBytes) {
          Info.EntrySize = Op.Output.Size;
          FoundSize = true;
        }
        break;

      case NdOp::COPY:
        break;

      case NdOp::LOAD:
        SawLoad = true;
        LoadWidth = Op.Output.Size;
        if (Info.TargetLoads.empty())
          Info.TargetLoads.push_back(
              {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true});
        if (Op.NumInputs >= 1 && Op.Inputs[0].isConst() && !FoundBase) {
          Info.setBaseAddr(Op.Inputs[0].Offset);
          FoundBase = true;
        }
        break;

      default:
        break;
      }
    }
    break;
  }

  if (SawLoad && !FoundSize && LoadWidth > 0 &&
      LoadWidth <= limits::kMaxEntryBytes) {
    Info.EntrySize = LoadWidth;
    FoundSize = true;
  }

  if (SawLoad && FoundBase && LoadWidth > 0 &&
      LoadWidth < limits::kMaxEntryBytes)
    Info.IsRelative = true;

  return FoundBase && FoundSize;
}

//===----------------------------------------------------------------------===//
// tryRelativeTable — PIC-relative jump table detection
//===----------------------------------------------------------------------===//

// The backward-slicing helpers (reachingDefIdx, traceToRegister,
// scaledIndexReg, ...) are declared in JumpTableResolverDetail.h and defined
// further below; the relative-table heuristic uses them to reject spill/reload
// relays.

/// Whether the LOAD address \p AddrV (defined before \p FromIdx in \p Ops) is a
/// plain stack/frame slot — `frameReg` or `frameReg + const`, with no scaled
/// index.  An indirect jump dispatched through such a load is a spill/reload
/// relay (`mov [esp+k], target; jmp *[esp+k]`): the computed target was stored
/// there a few instructions earlier, so the single-instruction relative-table
/// heuristic must not mistake the stack displacement for a table base.  The
/// genuine indexed table that produced the stored target is recovered by the
/// cross-instruction resolver instead.
static bool loadAddrIsFrameSlot(const std::vector<LowOp> &Ops, int FromIdx,
                                NdVar AddrV, const TargetRegInfo &TRI) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (AddrV.isReg())
      return TRI.isFrameReg(AddrV.Offset);
    int D = reachingDefIdx(Ops, FromIdx, AddrV);
    if (D < 0)
      return false;
    const LowOp &A = Ops[D];
    if (A.Opcode == NdOp::COPY && A.NumInputs >= 1) {
      AddrV = A.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (A.Opcode != NdOp::INT_ADD || A.NumInputs < 2)
      return false;
    int CW = A.Inputs[1].isConst() ? 1 : (A.Inputs[0].isConst() ? 0 : -1);
    if (CW < 0)
      return false; // base + base: not a simple frame slot
    if (scaledIndexReg(Ops, D - 1, A.Inputs[1 - CW]) != InvalidVA)
      return false; // base + index*scale: a genuine table access
    uint64_t Reg = traceToRegister(Ops, D - 1, A.Inputs[1 - CW]);
    return Reg != InvalidVA && TRI.isFrameReg(Reg);
  }
  return false;
}

bool CFGBuilder::tryRelativeTable(const BinaryImage &Img, const InsnRecord &Rec,
                                  JumpTableInfo &Info) {
  va_t CodeBase = 0;
  va_t TableBase = 0;
  uint16_t LoadSize = 0;
  bool HasSext = false;

  for (int I = static_cast<int>(Rec.Ops.size()) - 1; I >= 0; --I) {
    if (Rec.Ops[I].Opcode != NdOp::INDIR_BR)
      continue;

    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
        if (Op.Inputs[1].isConst() && Op.Inputs[1].Offset != 0) {
          CodeBase = Op.Inputs[1].Offset;
          break;
        }
        if (Op.Inputs[0].isConst() && Op.Inputs[0].Offset != 0) {
          CodeBase = Op.Inputs[0].Offset;
          break;
        }
      }
    }

    for (int J = I - 1; J >= 0; --J) {
      auto &Op = Rec.Ops[J];
      if (Op.Opcode == NdOp::INT_SEXT)
        HasSext = true;
      if (Op.Opcode == NdOp::LOAD) {
        // Reject a spill/reload relay: a target loaded from a frame slot is not
        // a table entry, and its stack displacement must not be read as a table
        // base.  Defer to the cross-instruction resolver for the real table.
        const NdVar &LAddr = (Op.NumInputs >= 2) ? Op.Inputs[1] : Op.Inputs[0];
        if (loadAddrIsFrameSlot(Rec.Ops, J - 1, LAddr,
                                getTargetRegInfo(Img.Arch)))
          return false;
        LoadSize = Op.Output.Size;
        Info.TargetLoads = {
            {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true}};
        for (int K = J - 1; K >= 0; --K) {
          if (Rec.Ops[K].Opcode == NdOp::INT_ADD && Rec.Ops[K].NumInputs >= 2) {
            if (Rec.Ops[K].Inputs[1].isConst())
              TableBase = Rec.Ops[K].Inputs[1].Offset;
            else if (Rec.Ops[K].Inputs[0].isConst())
              TableBase = Rec.Ops[K].Inputs[0].Offset;
            break;
          }
        }
        break;
      }
    }
    break;
  }

  if (CodeBase == 0 || LoadSize == 0)
    return false;

  if (!Img.hasExecutableCodeOwnerAt(CodeBase))
    return false;

  if (TableBase == 0)
    TableBase = CodeBase;

  Info.setBaseAddr(TableBase);
  Info.EntrySize = LoadSize;
  Info.IsRelative = true;
  Info.IsSigned = HasSext || (LoadSize < limits::kMaxEntryBytes);
  return true;
}

//===----------------------------------------------------------------------===//
// tryCrossInstrRelativeTable — PIC table whose base is set in a prior insn
//===----------------------------------------------------------------------===//

/// Reaching-definition index of `V` searching backward from `FromIdx`.
int reachingDefIdx(const std::vector<LowOp> &Ops, int FromIdx, const NdVar &V) {
  for (int I = FromIdx; I >= 0; --I) {
    const NdVar &O = Ops[I].Output;
    if (O.Space == V.Space && O.Offset == V.Offset)
      return I;
  }
  return -1;
}

/// Trace a nd-var backward through COPY chains to a plain register.
uint64_t traceToRegister(const std::vector<LowOp> &Ops, int FromIdx, NdVar V) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (V.isReg())
      return V.Offset;
    if (!V.isTemp())
      return InvalidVA;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0 || Ops[D].Opcode != NdOp::COPY || Ops[D].NumInputs < 1)
      return InvalidVA;
    V = Ops[D].Inputs[0];
    FromIdx = D - 1;
  }
  return InvalidVA;
}

/// Trace a register back through reaching value-preserving definitions (COPY,
/// ZEXT/SEXT, low-half SUBBYTES) within the op list to its ultimate source
/// register.  Unlike traceToRegister this follows register->register copies and
/// register<-temp chains, recovering e.g. the `mov ecx,edi` or the
/// `movzbl sil,eax` (lifted as ZEXT of a SUBBYTES temp) that aliases a switch
/// index before it is used to address the table — so a guard on the original
/// register (`cmp edi,N` / `cmpb sil,N`) is still matched to the table index.
uint64_t traceRegSource(const std::vector<LowOp> &Ops, int FromIdx,
                        uint64_t RegOff) {
  NdVar Cur = NdVar::reg(RegOff, 8);
  uint64_t LastReg = RegOff;
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    int D = -1;
    for (int I = FromIdx; I >= 0; --I) {
      const NdVar &O = Ops[I].Output;
      if (O.Space == Cur.Space && O.Offset == Cur.Offset) {
        D = I;
        break;
      }
    }
    if (D < 0)
      return LastReg;
    const LowOp &Op = Ops[D];
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs < 1 || (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()))
        return LastReg;
      Cur = Op.Inputs[0];
      break;
    case NdOp::SUBBYTES:
      if (Op.NumInputs < 2 ||
          (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()) ||
          !Op.Inputs[1].isConst() || Op.Inputs[1].Offset != 0)
        return LastReg;
      Cur = Op.Inputs[0];
      break;
    default:
      return LastReg;
    }
    if (Cur.isReg())
      LastReg = Cur.Offset;
    FromIdx = D - 1;
  }
  return LastReg;
}

/// Like traceToRegister but also follows zero/sign-extension and low-half
/// subpiece.  Recovers the index register of a scaled table index that was
/// widened before scaling — e.g. AArch64 `ldr x,[base,w,uxtw #3]`, where the
/// 32-bit index is zero-extended (INT_ZEXT) ahead of the `<<3`.
static uint64_t traceIndexToRegister(const std::vector<LowOp> &Ops, int FromIdx,
                                     NdVar V) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (V.isReg())
      return V.Offset;
    if (!V.isTemp())
      return InvalidVA;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return InvalidVA;
    const LowOp &Op = Ops[D];
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs < 1)
        return InvalidVA;
      V = Op.Inputs[0];
      break;
    case NdOp::SUBBYTES:
      if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
          Op.Inputs[1].Offset != 0)
        return InvalidVA;
      V = Op.Inputs[0];
      break;
    default:
      return InvalidVA;
    }
    FromIdx = D - 1;
  }
  return InvalidVA;
}

/// If a nd-var is a scaled index (traced through COPY/ZEXT/SEXT to an
/// INT_MULT(reg, const>1) or INT_LEFT(reg, const)), return the source index
/// register; otherwise InvalidVA.
uint64_t scaledIndexReg(const std::vector<LowOp> &Ops, int FromIdx, NdVar V,
                        NdVar *IndexValue, va_t *IndexUseAddr,
                        int *IndexUseSeq) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (!V.isTemp() && !V.isReg())
      return InvalidVA;
    int D = reachingDefIdx(Ops, FromIdx, V);
    if (D < 0)
      return InvalidVA;
    const LowOp &Op = Ops[D];
    bool Scaled = (Op.Opcode == NdOp::INT_MULT && Op.NumInputs >= 2 &&
                   Op.Inputs[1].isConst() && Op.Inputs[1].Offset > 1) ||
                  (Op.Opcode == NdOp::INT_LEFT && Op.NumInputs >= 2 &&
                   Op.Inputs[1].isConst() && Op.Inputs[1].Offset >= 1);
    if (Scaled) {
      // Keep the public selector occurrence in its logical integer lane when
      // the address calculation merely widens it before scaling.  Recording
      // the widened operand makes a 32-bit `idx + bias; cmp idx,N; zext;
      // idx*W` look like an unrelated 64-bit selector to the exact guard and
      // High/LLVM selector-plan consumers.  The ZEXT input is itself an exact
      // LowIR use occurrence; the address-role proof below still has to prove
      // that this precise value reaches the scaled LOAD address, so this does
      // not reintroduce a register-name or truncation heuristic.
      NdVar ExactIndex = Op.Inputs[0];
      va_t ExactUseAddr = Op.Addr;
      int ExactUseSeq = Op.Seq;
      const int WidenDef = reachingDefIdx(Ops, D - 1, Op.Inputs[0]);
      if (WidenDef >= 0 && Ops[WidenDef].Opcode == NdOp::INT_ZEXT &&
          Ops[WidenDef].NumInputs >= 1 && Ops[WidenDef].Inputs[0].Size != 0 &&
          Ops[WidenDef].Inputs[0].Size < Ops[WidenDef].Output.Size) {
        ExactIndex = Ops[WidenDef].Inputs[0];
        ExactUseAddr = Ops[WidenDef].Addr;
        ExactUseSeq = Ops[WidenDef].Seq;
      }
      if (IndexValue)
        *IndexValue = ExactIndex;
      if (IndexUseAddr)
        *IndexUseAddr = ExactUseAddr;
      if (IndexUseSeq)
        *IndexUseSeq = ExactUseSeq;
      return traceIndexToRegister(Ops, D - 1, Op.Inputs[0]);
    }
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs < 1)
        return InvalidVA;
      V = Op.Inputs[0];
      FromIdx = D - 1;
      break;
    default:
      return InvalidVA;
    }
  }
  return InvalidVA;
}

std::optional<va_t> CFGBuilder::lookupAmbiguousI386GOTOFFField(
    va_t Begin, va_t End, bool &HasAdditionalField) const {
  HasAdditionalField = false;
  if (!CurrentImg || Begin > End)
    return std::nullopt;

  // Keep the actual ordered lookup and its test observer behind one bounded
  // entry point.  This makes "no debit, no lookup" structural: a zero
  // bookkeeping allowance cannot even inspect the attacker-sized field set.
  if (!consumeIncompleteBranchMarkerEvidence(
          orderedSetLookupWork(
              CurrentImg->AmbiguousI386GOTOFFFields.size()) +
          1))
    return std::nullopt;
  ++I386GOTOFFTombstoneLookupCountForTesting;
  auto Field = CurrentImg->AmbiguousI386GOTOFFFields.lower_bound(Begin);
  if (Field == CurrentImg->AmbiguousI386GOTOFFFields.end() || *Field > End ||
      End - *Field < 4)
    return std::nullopt;
  auto NextField = Field;
  ++NextField;
  HasAdditionalField =
      NextField != CurrentImg->AmbiguousI386GOTOFFFields.end() &&
      *NextField < End;
  return *Field;
}

bool CFGBuilder::isExactI386GOTOFFInput(const LowOp &Add,
                                        int ConstantSide) const {
  if (!CurrentImg || CurrentImg->Arch != Arch::X86 || !CurrentImg->isELF() ||
      CurrentImg->getPointerSize() != 4 || ConstantSide < 0 ||
      ConstantSide >= Add.NumInputs || Add.Output.Size != 4)
    return false;
  const NdVar &Constant = Add.Inputs[ConstantSide];
  if (!Constant.isConst() || Constant.Size != 4)
    return false;
  const auto Insn = Insns.find(Add.Addr);
  if (Insn == Insns.end() || Insn->second.IsInstructionGuard ||
      Insn->second.Size == 0 || Insn->second.Size > InvalidVA - Add.Addr)
    return false;
  const va_t End = Add.Addr + Insn->second.Size;

  const bool HasPositiveGOTOFFRole =
      Constant.Provenance == ConstantAddressProvenance::DataAddress &&
      Constant.AddressOwnerVA != InvalidVA;
  if (!HasPositiveGOTOFFRole) {
    // A multiply-owned GOTOFF field has no positive address provenance, but
    // its exact instruction span is already a table-shape certificate.  Claim
    // that shape before the budgeted occurrence scan: exhaustion must preserve
    // this branch opaquely rather than turn it into a callback.  A completed
    // scan still requires the precise LowOp input below, so an unrelated field
    // or callback in the same function cannot acquire the semantic marker.
    // This lookup is required to claim the exact negative shape, so it cannot
    // run on the user-controlled nested proof balance: budget zero must still
    // identify the affected branch.  Debit the existing function-scoped
    // incomplete-marker bookkeeping account before touching the ordered set.
    // Its exhaustion is already function-level fail-closed, avoiding both an
    // unmetered lookup and a shape-less tail-call conversion without opening
    // a second function-scoped work allowance.
    bool HasAdditionalField = false;
    const std::optional<va_t> Field = lookupAmbiguousI386GOTOFFField(
        Add.Addr, End, HasAdditionalField);
    if (!Field)
      return false;
    const bool PriorShapeClaimed = I386GOTOFFProposalShapeClaimed;
    I386GOTOFFProposalShapeClaimed = true;
    if (HasAdditionalField) {
      I386GOTOFFProposalEvidenceIncomplete = true;
      return false;
    }
    if (!consumeI386GOTOFFProposalEvidence(
            RelocatedInstructionScalarOperandOccurrences.size()))
      return false;

    const RelocatedInstructionScalarOperandOccurrence *Ambiguous = nullptr;
    for (const RelocatedInstructionScalarOperandOccurrence &Occurrence :
         RelocatedInstructionScalarOperandOccurrences) {
      if (Occurrence.Kind !=
              RelocatedInstructionScalarOperandOccurrence::OperandKind::
                  I386ELFAmbiguousGOTOFF ||
          Occurrence.InstructionAddr != Add.Addr ||
          Occurrence.OpSeq != Add.Seq || Occurrence.FieldVA != *Field ||
          Occurrence.Width != 4 || Occurrence.InputIndex != ConstantSide ||
          Occurrence.EncodedValue != static_cast<uint32_t>(Constant.Offset) ||
          Occurrence.Opcode != Add.Opcode ||
          Occurrence.OutputWitness != Add.Output)
        continue;
      if (Ambiguous) {
        I386GOTOFFProposalEvidenceIncomplete = true;
        return false;
      }
      Ambiguous = &Occurrence;
    }
    if (!Ambiguous) {
      I386GOTOFFProposalShapeClaimed = PriorShapeClaimed;
      return false;
    }
    if (!consumeI386GOTOFFProposalEvidence(CurrentImg->Segments.size()))
      return false;
    const uint8_t *EncodedBytes = CurrentImg->readVA(Ambiguous->FieldVA, 4);
    uint32_t Encoded = 0;
    if (!EncodedBytes) {
      I386GOTOFFProposalEvidenceIncomplete = true;
      return false;
    }
    std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
    if (Encoded != static_cast<uint32_t>(Ambiguous->EncodedValue)) {
      I386GOTOFFProposalEvidenceIncomplete = true;
      return false;
    }
    if (ActiveJumpTableCandidateAddr == InvalidVA) {
      I386GOTOFFProposalEvidenceIncomplete = true;
      return false;
    }
    const I386GOTOFFAmbiguityReplayKey ReplayKey = std::make_tuple(
        ActiveJumpTableCandidateAddr, Add.Addr, Add.Seq, ConstantSide,
        static_cast<va_t>(Encoded));
    constexpr size_t ReplayKeyWork = 5;
    auto PrepayKeySetInsert = [&](const auto &Set) {
      const size_t Lookup = orderedSetLookupWork(Set.size());
      if (Lookup > std::numeric_limits<size_t>::max() / ReplayKeyWork)
        return consumeI386GOTOFFProposalEvidence(
            std::numeric_limits<size_t>::max());
      return consumeI386GOTOFFProposalEvidence(ReplayKeyWork * Lookup) &&
             consumeI386GOTOFFProposalEvidence(ReplayKeyWork) &&
             consumeI386GOTOFFProposalEvidence(1);
    };
    if (!PrepayKeySetInsert(StageReplayedI386GOTPCKeys) ||
        !PrepayKeySetInsert(CurrentI386GOTOFFAmbiguityKeys))
      return false;
    StageReplayedI386GOTPCKeys.insert(ReplayKey);
    CurrentI386GOTOFFAmbiguityKeys.insert(ReplayKey);
    I386GOTOFFAmbiguousModelReach = true;
    return false;
  }

  unsigned InstructionDataFields = 0;
  const RelocatedAddressField *InstructionField = nullptr;
  va_t InstructionFieldVA = InvalidVA;
  auto DataField = CurrentImg->DataAddressRelocOperands.lower_bound(Add.Addr);
  for (; DataField != CurrentImg->DataAddressRelocOperands.end() &&
         DataField->first < End;
       ++DataField) {
    ++InstructionDataFields;
    InstructionField = &DataField->second;
    InstructionFieldVA = DataField->first;
  }
  if (InstructionDataFields != 1 || !InstructionField ||
      InstructionField->Kind != RelocatedAddressFieldKind::I386ELFGOTOFF)
    return false;

  unsigned MatchingInputs = 0;
  int MatchingSide = -1;
  for (uint8_t Input = 0; Input < Add.NumInputs; ++Input)
    if (Add.Inputs[Input].isConst() &&
        Add.Inputs[Input].Size == Constant.Size &&
        Add.Inputs[Input].Offset == Constant.Offset &&
        Add.Inputs[Input].Provenance == Constant.Provenance &&
        Add.Inputs[Input].AddressOwnerVA == Constant.AddressOwnerVA) {
      ++MatchingInputs;
      MatchingSide = Input;
    }
  if (MatchingInputs != 1 || MatchingSide != ConstantSide)
    return false;

  const RelocatedInstructionAddressOccurrence *Exact = nullptr;
  for (const RelocatedInstructionAddressOccurrence &Occurrence :
       RelocatedInstructionAddressOccurrences) {
    if (Occurrence.InstructionAddr != Add.Addr || Occurrence.OpSeq != Add.Seq ||
        Occurrence.FieldVA < Add.Addr || Occurrence.FieldVA >= End ||
        Occurrence.Width != 4 || Occurrence.TargetVA != Constant.Offset ||
        Occurrence.TargetOwnerVA != Constant.AddressOwnerVA ||
        Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
        Occurrence.PCRelativeFromInstructionEnd || Occurrence.DefinesOutput ||
        Occurrence.OutputMayDepend || Occurrence.InputIndex != ConstantSide ||
        Occurrence.FieldVA != InstructionFieldVA)
      continue;
    const auto Field =
        CurrentImg->DataAddressRelocOperands.find(Occurrence.FieldVA);
    if (Field == CurrentImg->DataAddressRelocOperands.end() ||
        Field->second.Width != Occurrence.Width ||
        Field->second.TargetVA != Occurrence.TargetVA ||
        Field->second.TargetOwnerVA != Occurrence.TargetOwnerVA ||
        Field->second.PCRelativeFromInstructionEnd ||
        Field->second.Kind != RelocatedAddressFieldKind::I386ELFGOTOFF ||
        !CurrentImg->relocatedI386GOTOFFTargetBelongsToOwner(
            Field->second.TargetVA, Field->second.TargetOwnerVA))
      continue;
    if (Exact)
      return false;
    Exact = &Occurrence;
  }
  return Exact != nullptr;
}

bool CFGBuilder::exactI386ModelZeroReaches(const LowOp &Use, int BaseSide,
                                           va_t TableBase) const {
  // Reaching a relocation-authenticated model is a complete-CFG proof even
  // when the exact query is served from this stage's cache.  Preserve that
  // publication contract on cache hits so a sibling dispatch cannot be
  // mislabeled as independent of the final proof graph.
  RequestedCompleteJumpTableProof = true;
  if (!CurrentImg || CurrentImg->Arch != Arch::X86 || !CurrentImg->isELF() ||
      CurrentImg->getPointerSize() != 4 || BaseSide < 0 ||
      BaseSide >= Use.NumInputs || Use.Inputs[BaseSide].Size != 4)
    return false;
  // Both callers have already authenticated the outer R_386_GOTOFF field and
  // a scaled table-load shape.  Record that exact branch identity before any
  // model inventory or graph budget is consumed; an unfinished proof is
  // resource-incomplete, not evidence that the branch is a callback.
  I386GOTOFFProposalShapeClaimed = true;
  if (I386GOTModelEvidenceIncomplete) {
    I386GOTOFFProposalEvidenceIncomplete = true;
    return false;
  }
  auto OrderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };
  auto ConsumeProduct = [&](size_t Count, size_t Cost) {
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count)
      return consumeI386GOTOFFProposalEvidence(
          std::numeric_limits<size_t>::max());
    return consumeI386GOTOFFProposalEvidence(Count * Cost);
  };
  auto AccumulateProduct = [&](size_t &Total, size_t Count, size_t Cost) {
    const size_t Max = std::numeric_limits<size_t>::max();
    if (Count != 0 && Cost > Max / Count)
      return false;
    const size_t Product = Count * Cost;
    if (Product > Max - Total)
      return false;
    Total += Product;
    return true;
  };
  if (!consumeI386GOTOFFProposalEvidence(
          RelocatedInstructionScalarModelOccurrences.size()))
    return false;
  std::vector<JumpTableValueOccurrence> Alternatives;
  using ModelOutputIdentity =
      std::tuple<va_t, int, uint8_t, uint64_t, uint16_t, uint8_t, uint64_t>;
  constexpr size_t ModelOutputKeyWork = 7;
  std::set<ModelOutputIdentity> SeenModelOutputs;
  std::set<ModelOutputIdentity> DuplicateModelOutputs;
  std::vector<ModelOutputIdentity> AlternativeIdentities;
  std::vector<JumpTableValueOccurrence> DuplicateAlternatives;
  const size_t ModelCount = RelocatedInstructionScalarModelOccurrences.size();
  // Three bounded vectors: capacity initialization, element writes/moves, and
  // eventual destruction are all charged before the first push.
  if (!ConsumeProduct(ModelCount, 6))
    return false;
  Alternatives.reserve(ModelCount);
  AlternativeIdentities.reserve(ModelCount);
  DuplicateAlternatives.reserve(ModelCount);
  for (const RelocatedInstructionScalarModelOccurrence &Model :
       RelocatedInstructionScalarModelOccurrences) {
    if (Model.Model != RelocatedInstructionScalarModelOccurrence::ModelKind::
                           I386ELFGOTBaseZero ||
        Model.Width != 4 || Model.OutputWitness.Size != 4 ||
        (!Model.OutputWitness.isReg() && !Model.OutputWitness.isTemp()))
      continue;
    if (!consumeI386GOTOFFProposalEvidence(OrderedLookupWork(Insns.size())))
      return false;
    const auto ModelInsn = Insns.find(Model.InstructionAddr);
    if (ModelInsn == Insns.end() || ModelInsn->second.IsInstructionGuard ||
        ModelInsn->second.Size == 0 ||
        ModelInsn->second.Size > InvalidVA - Model.InstructionAddr ||
        Model.FieldVA < Model.InstructionAddr ||
        Model.FieldVA >= Model.InstructionAddr + ModelInsn->second.Size)
      continue;
    if (!consumeI386GOTOFFProposalEvidence(
            OrderedLookupWork(CurrentImg->I386GOTPCFields.size())))
      return false;
    if (!CurrentImg->I386GOTPCFields.count(Model.FieldVA))
      continue;
    if (!consumeI386GOTOFFProposalEvidence(ModelInsn->second.Ops.size()))
      return false;
    const LowOp *ExactOp = nullptr;
    for (const LowOp &Op : ModelInsn->second.Ops)
      if (Op.Addr == Model.InstructionAddr && Op.Seq == Model.OpSeq &&
          Op.Opcode == Model.OutputOpcode && Op.Output == Model.OutputWitness) {
        if (ExactOp) {
          ExactOp = nullptr;
          break;
        }
        ExactOp = &Op;
      }
    if (!ExactOp)
      continue;
    const ModelOutputIdentity Identity =
        std::make_tuple(Model.InstructionAddr, Model.OpSeq,
                        static_cast<uint8_t>(Model.OutputWitness.Space),
                        Model.OutputWitness.Offset, Model.OutputWitness.Size,
                        static_cast<uint8_t>(Model.OutputWitness.Provenance),
                        Model.OutputWitness.AddressOwnerVA);
    if (!ConsumeProduct(ModelOutputKeyWork,
                        OrderedLookupWork(SeenModelOutputs.size())) ||
        !ConsumeProduct(ModelOutputKeyWork, 2) ||
        !consumeI386GOTOFFProposalEvidence(1))
      return false;
    if (!SeenModelOutputs.insert(Identity).second) {
      if (!ConsumeProduct(ModelOutputKeyWork,
                          OrderedLookupWork(DuplicateModelOutputs.size())) ||
          !ConsumeProduct(ModelOutputKeyWork, 2) ||
          !consumeI386GOTOFFProposalEvidence(1))
        return false;
      if (DuplicateModelOutputs.insert(Identity).second) {
        // Duplicate model claims are negative alternatives only for a branch
        // whose selected base may actually depend on this exact output.  A
        // function-global duplicate must not preserve unrelated GOTOFF-shaped
        // siblings.
        DuplicateAlternatives.push_back(
            {Model.OutputWitness, Model.InstructionAddr, Model.OpSeq,
             /*DefinedAtPoint=*/true});
      }
      continue;
    }
    Alternatives.push_back({Model.OutputWitness, Model.InstructionAddr,
                            Model.OpSeq, /*DefinedAtPoint=*/true});
    AlternativeIdentities.push_back(Identity);
  }

  if (!DuplicateModelOutputs.empty()) {
    const size_t Lookup = OrderedLookupWork(DuplicateModelOutputs.size());
    if (!ConsumeProduct(Alternatives.size(), 3) ||
        !ConsumeProduct(Alternatives.size(), ModelOutputKeyWork * Lookup))
      return false;
    std::vector<JumpTableValueOccurrence> UniqueAlternatives;
    UniqueAlternatives.reserve(Alternatives.size());
    for (size_t I = 0; I < Alternatives.size(); ++I)
      if (!DuplicateModelOutputs.count(AlternativeIdentities[I]))
        UniqueAlternatives.push_back(std::move(Alternatives[I]));
    Alternatives.swap(UniqueAlternatives);
  }

  // A multiply-owned GOTPC field is intentionally absent from the completed
  // scalar-model inventory.  Keep its decoded output as a negative witness so
  // an exact GOTOFF consumer can distinguish "no model reaches here" from
  // "the reaching model field is ambiguous".  This remains occurrence-local:
  // an unrelated ambiguous field elsewhere in the function does not preserve
  // this branch unless the whole-CFG MayDepend query below connects it to the
  // selected base input.
  if (!consumeI386GOTOFFProposalEvidence(
          RelocatedInstructionScalarOperandOccurrences.size()))
    return false;
  std::vector<JumpTableValueOccurrence> AmbiguousAlternatives;
  const size_t OperandCount =
      RelocatedInstructionScalarOperandOccurrences.size();
  if (!ConsumeProduct(OperandCount, 2))
    return false;
  AmbiguousAlternatives.reserve(OperandCount);
  std::set<ModelOutputIdentity> SeenAmbiguousOutputs;
  for (const RelocatedInstructionScalarOperandOccurrence &Operand :
       RelocatedInstructionScalarOperandOccurrences) {
    if (Operand.Kind !=
            RelocatedInstructionScalarOperandOccurrence::OperandKind::
                I386ELFGOTPC ||
        Operand.Width != 4 || Operand.InputIndex >= 2 ||
        Operand.Opcode != NdOp::INT_ADD || Operand.OutputWitness.Size != 4 ||
        (!Operand.OutputWitness.isReg() &&
         !Operand.OutputWitness.isTemp()))
      continue;
    if (!consumeI386GOTOFFProposalEvidence(
            OrderedLookupWork(CurrentImg->AmbiguousI386GOTPCFields.size())))
      return false;
    if (!CurrentImg->AmbiguousI386GOTPCFields.count(Operand.FieldVA))
      continue;
    if (!consumeI386GOTOFFProposalEvidence(OrderedLookupWork(Insns.size())))
      return false;
    const auto OperandInsn = Insns.find(Operand.InstructionAddr);
    if (OperandInsn == Insns.end() || OperandInsn->second.IsInstructionGuard ||
        OperandInsn->second.Size == 0 ||
        OperandInsn->second.Size > InvalidVA - Operand.InstructionAddr ||
        Operand.FieldVA < Operand.InstructionAddr ||
        Operand.FieldVA >= Operand.InstructionAddr + OperandInsn->second.Size)
      continue;
    const uint8_t *EncodedBytes = CurrentImg->readVA(Operand.FieldVA, 4);
    uint32_t Encoded = 0;
    if (!EncodedBytes)
      continue;
    std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
    if (Encoded != static_cast<uint32_t>(Operand.EncodedValue))
      continue;
    if (!consumeI386GOTOFFProposalEvidence(OperandInsn->second.Ops.size()))
      return false;

    const LowOp *ExactOp = nullptr;
    for (const LowOp &Op : OperandInsn->second.Ops)
      if (Op.Addr == Operand.InstructionAddr && Op.Seq == Operand.OpSeq &&
          Op.Opcode == Operand.Opcode && Op.Output == Operand.OutputWitness) {
        if (ExactOp) {
          ExactOp = nullptr;
          break;
        }
        ExactOp = &Op;
      }
    if (!ExactOp || Operand.InputIndex >= ExactOp->NumInputs ||
        !ExactOp->Inputs[Operand.InputIndex].isConst() ||
        ExactOp->Inputs[Operand.InputIndex].Size != 4 ||
        ExactOp->Inputs[Operand.InputIndex].Provenance !=
            ConstantAddressProvenance::Scalar ||
        static_cast<uint32_t>(ExactOp->Inputs[Operand.InputIndex].Offset) !=
            Encoded)
      continue;
    const ModelOutputIdentity Identity = std::make_tuple(
        Operand.InstructionAddr, Operand.OpSeq,
        static_cast<uint8_t>(Operand.OutputWitness.Space),
        Operand.OutputWitness.Offset, Operand.OutputWitness.Size,
        static_cast<uint8_t>(Operand.OutputWitness.Provenance),
        Operand.OutputWitness.AddressOwnerVA);
    if (!ConsumeProduct(ModelOutputKeyWork,
                        OrderedLookupWork(SeenAmbiguousOutputs.size())) ||
        !ConsumeProduct(ModelOutputKeyWork, 2) ||
        !consumeI386GOTOFFProposalEvidence(1))
      return false;
    if (!SeenAmbiguousOutputs.insert(Identity).second)
      continue;
    AmbiguousAlternatives.push_back(
        {Operand.OutputWitness, Operand.InstructionAddr, Operand.OpSeq,
         /*DefinedAtPoint=*/true});
  }
  if (!DuplicateAlternatives.empty()) {
    if (DuplicateAlternatives.size() >
        std::numeric_limits<size_t>::max() - AmbiguousAlternatives.size())
      return false;
    const size_t CombinedAlternatives =
        AmbiguousAlternatives.size() + DuplicateAlternatives.size();
    if (!ConsumeProduct(CombinedAlternatives, 2))
      return false;
    AmbiguousAlternatives.reserve(CombinedAlternatives);
    std::move(DuplicateAlternatives.begin(), DuplicateAlternatives.end(),
              std::back_inserter(AmbiguousAlternatives));
  }
  if (Alternatives.empty() && AmbiguousAlternatives.empty())
    return false;

  // A peeled/loop-body pair can dispatch through the same GOTOFF table.  On
  // the first multistage pass, relocation targets are independent CFG roots;
  // after the peeled dispatch is published those roots feed the loop-body
  // use and obscure an otherwise unchanged GOT register.  Break that
  // candidate-local cycle only when the exact GOTOFF field names a complete,
  // owner-bounded run of code-pointer relocations.  This is proposal evidence
  // only: resolveJumpTable later narrows storage to the authenticated runtime
  // domain, audits independent consumers, restores every retained root, and
  // replays both the index and address-role certificates before publication.
  std::optional<std::set<va_t>> SavedProofRoots;
  if (ActiveJumpTableProofRoots) {
    // Copy traversal, destination nodes, and eventual local destruction.
    if (!ConsumeProduct(ActiveJumpTableProofRoots->size(), 3))
      return false;
    SavedProofRoots = *ActiveJumpTableProofRoots;
  }
  auto RestoreProofRoots = [&]() {
    ActiveJumpTableProofRoots = std::move(SavedProofRoots);
  };
  if (!ActiveJumpTableProofRoots) {
    const auto ProposalRootKey = detail::makeI386GOTOFFProposalRootCacheKey(
        ActiveJumpTableCandidateAddr, TableBase,
        ActiveJumpTableCandidateProofRank, ActiveJumpTableConsumerAudit);
    constexpr size_t ProposalRootKeyWork =
        std::tuple_size_v<detail::I386GOTOFFProposalRootCacheKey>;
    if (!ConsumeProduct(
            ProposalRootKeyWork,
            OrderedLookupWork(I386GOTOFFProposalRootCache.size())))
      return false;
    auto Cached = I386GOTOFFProposalRootCache.find(ProposalRootKey);
    if (Cached == I386GOTOFFProposalRootCache.end()) {
      std::optional<std::set<va_t>> ProposalRoots;
      const uint32_t PointerSize = CurrentImg->getPointerSize();
      uint32_t Run = 0;
      va_t SlotVA = TableBase;
      bool RunAnalysisComplete = true;
      while (Run < limits::kMaxJumpTableEntries) {
        if (!consumeI386GOTOFFProposalEvidence()) {
          RunAnalysisComplete = false;
          break;
        }
        if (!consumeI386GOTOFFProposalEvidence(
                OrderedLookupWork(CurrentImg->CodePtrRelocSlots.size()))) {
          RunAnalysisComplete = false;
          break;
        }
        if (!CurrentImg->CodePtrRelocSlots.count(SlotVA))
          break;
        ++Run;
        if (PointerSize > InvalidVA - SlotVA)
          break;
        SlotVA += PointerSize;
      }
      if (!RunAnalysisComplete)
        return false;

      if (Run >= limits::kMinJumpTableEntries) {
        const size_t AddressOccurrenceCount =
            RelocatedInstructionAddressOccurrences.size();
        const size_t RelAnchorCount = CurrentImg->RelCodeTableAnchors.size();
        const size_t DataFieldCount =
            CurrentImg->DataAddressRelocOperands.size();
        if (AddressOccurrenceCount >
                std::numeric_limits<size_t>::max() - RelAnchorCount ||
            AddressOccurrenceCount + RelAnchorCount >
                std::numeric_limits<size_t>::max() - DataFieldCount)
          return consumeI386GOTOFFProposalEvidence(
                     std::numeric_limits<size_t>::max()),
                 false;
        const size_t AnchorUpper =
            AddressOccurrenceCount + RelAnchorCount + DataFieldCount;
        // currentRelocatedInstructionTableAnchors performs three ordered
        // membership lookups per occurrence and may retain one set node.  Pay
        // the maximum result allocation/destruction before construction.
        const size_t CurrentAnchorPerOccurrence =
            OrderedLookupWork(PublishedReachableInsns.size()) +
            OrderedLookupWork(CurrentImg->RelCodeRelocSlots.size()) +
            OrderedLookupWork(CurrentImg->CodePtrRelocSlots.size()) +
            OrderedLookupWork(AddressOccurrenceCount) + 3;
        if (!ConsumeProduct(AddressOccurrenceCount,
                            CurrentAnchorPerOccurrence))
          return false;

        // boundCodePtrRunByNextAnchor copies/merges the complete anchor
        // universe, audits every relocation field's executable owner and then
        // walks the ordered suffix looking for the next code-pointer slot.
        // The ELF owner classifier's worst case scans the segment/section,
        // import-range, known-code and symbol inventories.  This conservative
        // prepayment happens before either helper allocates a node.
        size_t OwnerQueryWork = 1;
        if (!AccumulateProduct(OwnerQueryWork, 3,
                               CurrentImg->Segments.size()) ||
            !AccumulateProduct(OwnerQueryWork, 2,
                               CurrentImg->Sections.size()) ||
            !AccumulateProduct(
                OwnerQueryWork, 2,
                OrderedLookupWork(CurrentImg->ImportStubIndices.size())) ||
            !AccumulateProduct(OwnerQueryWork, 2,
                               CurrentImg->ImportStubRanges.size()) ||
            !AccumulateProduct(
                OwnerQueryWork, 3,
                OrderedLookupWork(CurrentImg->RuntimeFunctionAddrs.size())) ||
            !AccumulateProduct(
                OwnerQueryWork, 1,
                OrderedLookupWork(
                    CurrentImg->VerifiedFunctionEntries.size())) ||
            !AccumulateProduct(OwnerQueryWork, 1,
                               CurrentImg->KnownCodeRanges.size()) ||
            !AccumulateProduct(OwnerQueryWork, 1,
                               CurrentImg->Symbols.size()))
          return consumeI386GOTOFFProposalEvidence(
                     std::numeric_limits<size_t>::max()),
                 false;
        const size_t AnchorLookup = OrderedLookupWork(AnchorUpper);
        if (!ConsumeProduct(RelAnchorCount, AnchorLookup + 3) ||
            !ConsumeProduct(AddressOccurrenceCount, AnchorLookup + 3) ||
            !ConsumeProduct(DataFieldCount,
                            OwnerQueryWork + AnchorLookup + 4) ||
            !consumeI386GOTOFFProposalEvidence(AnchorLookup) ||
            !ConsumeProduct(
                AnchorUpper,
                OrderedLookupWork(CurrentImg->CodePtrRelocSlots.size()) + 1))
          return false;
        const std::set<va_t> DecodedAnchors =
            currentRelocatedInstructionTableAnchors(*CurrentImg);
        Run = boundCodePtrRunByNextAnchor(*CurrentImg, TableBase, PointerSize,
                                          Run, DecodedAnchors);

        // codePtrRelocRunHasExactBoundary first resolves the mapped owner,
        // then checks both anchor sets and finally scans all data fields.  Its
        // complete work is prepaid independently of the boolean result.
        size_t OwnerEndWork = 1;
        if (!AccumulateProduct(OwnerEndWork, 2,
                               CurrentImg->Segments.size()) ||
            !AccumulateProduct(OwnerEndWork, 2,
                               CurrentImg->Sections.size()))
          return consumeI386GOTOFFProposalEvidence(
                     std::numeric_limits<size_t>::max()),
                 false;
        if (!consumeI386GOTOFFProposalEvidence(OwnerEndWork) ||
            !consumeI386GOTOFFProposalEvidence(
                OrderedLookupWork(CurrentImg->RelCodeTableAnchors.size())) ||
            !consumeI386GOTOFFProposalEvidence(
                OrderedLookupWork(DecodedAnchors.size())) ||
            !ConsumeProduct(DataFieldCount, OwnerQueryWork + 1))
          return false;
        if (Run < limits::kMinJumpTableEntries ||
            !codePtrRelocRunHasExactBoundary(
                *CurrentImg, TableBase, PointerSize, Run, DecodedAnchors)) {
          Run = 0;
        }

        if (Run >= limits::kMinJumpTableEntries) {
          // Candidate owns one storage-range node and Run suppressible slot
          // values.  Pay their construction and destruction before either
          // temporary container is populated.
          if (!ConsumeProduct(size_t{1}, size_t{2}) ||
              !ConsumeProduct(Run, size_t{4}))
            return false;
          JumpTableInfo Candidate;
          Candidate.setBaseAddr(TableBase);
          Candidate.EntrySize = PointerSize;
          Candidate.EntryStride = PointerSize;
          Candidate.PhysicalCapacity = Run;
          Candidate.RelocAbsolute = true;
          Candidate.StorageRanges.push_back({
              TableBase, static_cast<uint16_t>(PointerSize), PointerSize, Run});
          Candidate.SuppressibleRelocationSlots.reserve(Run);
          for (uint32_t Slot = 0; Slot < Run; ++Slot)
            Candidate.SuppressibleRelocationSlots.push_back(
                TableBase + uint64_t(Slot) * PointerSize);

          // Mirror budgetedJumpTableProofRoots exactly.  A rank-0 candidate
          // proves itself against the full persistent-root set; later
          // candidates may suppress only exact relocation slots from strictly
          // lower ranks.
          if (!consumeI386GOTOFFProposalEvidence(
                  PriorStrongJumpTableProposals.size()))
            return false;
          size_t StorageCount = 1;
          size_t SuppressibleSlotCount = Run;
          for (const auto &[Addr, Proposal] :
               PriorStrongJumpTableProposals) {
            if (Addr == ActiveJumpTableCandidateAddr ||
                (!ActiveJumpTableConsumerAudit &&
                 Proposal.ProofRank >= ActiveJumpTableCandidateProofRank))
              continue;
            if (Proposal.StorageRanges.size() >
                    std::numeric_limits<size_t>::max() - StorageCount ||
                Proposal.SuppressibleRelocationSlots.size() >
                    std::numeric_limits<size_t>::max() -
                        SuppressibleSlotCount)
              return consumeI386GOTOFFProposalEvidence(
                         std::numeric_limits<size_t>::max()),
                     false;
            StorageCount += Proposal.StorageRanges.size();
            SuppressibleSlotCount +=
                Proposal.SuppressibleRelocationSlots.size();
          }
          if (!ConsumeProduct(PersistentCFGRoots.size(), size_t{2}) ||
              !ConsumeProduct(
                  RelocatedInstructionAddressOccurrences.size(), size_t{2}) ||
              !ConsumeProduct(StorageCount, size_t{2}) ||
              !ConsumeProduct(SuppressibleSlotCount, size_t{4}) ||
              !consumeI386GOTOFFProposalEvidence(
                  ProtectedJumpTableRelocationSlots
                      ? ProtectedJumpTableRelocationSlots->size()
                      : 0) ||
              !ConsumeProduct(SuppressibleSlotCount, StorageCount) ||
              !ConsumeProduct(RelocationCFGRootSources.size(), size_t{2}))
            return false;
          if (StorageCount > std::numeric_limits<size_t>::max() - 2)
            return consumeI386GOTOFFProposalEvidence(
                       std::numeric_limits<size_t>::max()),
                   false;
          for (const auto &[Target, Sources] : RelocationCFGRootSources) {
            (void)Target;
            if (!ConsumeProduct(Sources.size(), StorageCount + 2))
              return false;
          }
          // Candidate.StorageRanges carries the exact boundary authenticated
          // above, so jumpTableProofRoots consumes it directly without
          // rebuilding the anchor/boundary inventory a second time.
          ProposalRoots = jumpTableProofRoots(Candidate, &DecodedAnchors);
        }
      }
      if (!ConsumeProduct(
              ProposalRootKeyWork,
              OrderedLookupWork(I386GOTOFFProposalRootCache.size() + 1)) ||
          !ConsumeProduct(ProposalRootKeyWork, 2) ||
          !consumeI386GOTOFFProposalEvidence(1))
        return false;
      Cached = I386GOTOFFProposalRootCache
                   .emplace(ProposalRootKey, std::move(ProposalRoots))
                   .first;
    }
    if (Cached->second) {
      // Source traversal plus destination nodes and their stage cleanup.
      if (!ConsumeProduct(Cached->second->size(), 3))
        return false;
      ActiveJumpTableProofRoots = *Cached->second;
    }
  }

  std::vector<va_t> ProofRootIdentity;
  if (ActiveJumpTableProofRoots) {
    // Set traversal, vector elements, and eventual key/vector destruction.
    if (!ConsumeProduct(ActiveJumpTableProofRoots->size(), 3)) {
      RestoreProofRoots();
      return false;
    }
    ProofRootIdentity.assign(ActiveJumpTableProofRoots->begin(),
                             ActiveJumpTableProofRoots->end());
  }
  const NdVar &BaseInput = Use.Inputs[BaseSide];
  I386GOTOFFModelReachCacheKey CacheKey = std::make_tuple(
      ActiveJumpTableCandidateAddr, Use.Addr, Use.Seq, BaseSide, TableBase,
      static_cast<uint8_t>(BaseInput.Space), BaseInput.Offset, BaseInput.Size,
      static_cast<uint8_t>(BaseInput.Provenance), BaseInput.AddressOwnerVA,
      std::move(ProofRootIdentity));
  I386GOTOFFAmbiguityReplayKey ReplayKey = std::make_tuple(
      ActiveJumpTableCandidateAddr, Use.Addr, Use.Seq, BaseSide, TableBase);
  const size_t CacheKeyWork = std::get<10>(CacheKey).size() + 11;
  constexpr size_t ReplayKeyWork = 5;
  auto PrepayKeySetInsert = [&](const auto &Set) {
    return ConsumeProduct(ReplayKeyWork, OrderedLookupWork(Set.size())) &&
           ConsumeProduct(ReplayKeyWork, 1) &&
           consumeI386GOTOFFProposalEvidence(1);
  };
  auto PublishCompletedAmbiguousQuery =
      [&](const I386GOTOFFModelReachResult &Result) {
        if (!Result.AmbiguousQueryIssued)
          return true;
        if (!PrepayKeySetInsert(StageReplayedI386GOTPCKeys))
          return false;
        if (Result.AmbiguousReach &&
            !PrepayKeySetInsert(CurrentI386GOTOFFAmbiguityKeys))
          return false;
        StageReplayedI386GOTPCKeys.insert(ReplayKey);
        if (Result.AmbiguousReach)
          CurrentI386GOTOFFAmbiguityKeys.insert(ReplayKey);
        return true;
      };
  if (!ConsumeProduct(CacheKeyWork,
                      OrderedLookupWork(I386GOTOFFModelReachCache.size()))) {
    RestoreProofRoots();
    return false;
  }
  const auto Cached = I386GOTOFFModelReachCache.find(CacheKey);
  if (Cached != I386GOTOFFModelReachCache.end()) {
    if (!PublishCompletedAmbiguousQuery(Cached->second)) {
      RestoreProofRoots();
      return false;
    }
    if (Cached->second.AmbiguousReach)
      I386GOTOFFAmbiguousModelReach = true;
    RestoreProofRoots();
    return Cached->second.Authenticated && !Cached->second.AmbiguousReach;
  }

  std::vector<JumpTableValueQuery> Queries;
  if (!consumeI386GOTOFFProposalEvidence(6)) {
    RestoreProofRoots();
    return false;
  }
  Queries.reserve(2);
  std::optional<size_t> AuthenticatedQuery;
  std::optional<size_t> AmbiguousQuery;
  if (!Alternatives.empty()) {
    AuthenticatedQuery = Queries.size();
    JumpTableValueQuery Query;
    Query.Candidate = Use.Inputs[BaseSide];
    Query.UseAddr = Use.Addr;
    Query.UseSeq = Use.Seq;
    Query.Alternatives = std::move(Alternatives);
    Queries.push_back(std::move(Query));
  }
  if (!AmbiguousAlternatives.empty()) {
    AmbiguousQuery = Queries.size();
    JumpTableValueQuery Query;
    Query.Candidate = Use.Inputs[BaseSide];
    Query.UseAddr = Use.Addr;
    Query.UseSeq = Use.Seq;
    Query.Alternatives = std::move(AmbiguousAlternatives);
    Query.Relation = JumpTableValueRelation::MayDepend;
    Queries.push_back(std::move(Query));
  }
  bool AnalysisComplete = false;
  std::vector<bool> QueryAnalysisComplete;
  I386GOTOFFGraphQueryIssuedForTesting = true;
  const std::vector<bool> Matches = tableValuesMatchAtUses(
      Queries, &AnalysisComplete, &QueryAnalysisComplete,
      /*CandidateBranchOverride=*/InvalidVA,
      /*CandidateTargetsOverride=*/nullptr,
      &I386GOTOFFProposalEvidenceRemaining);
  auto QueryComplete = [&](size_t Index) {
    return Index < Matches.size() && Index < QueryAnalysisComplete.size() &&
           QueryAnalysisComplete[Index];
  };
  const bool IssuedQueriesComplete =
      AnalysisComplete && Matches.size() == Queries.size() &&
      QueryAnalysisComplete.size() == Queries.size() &&
      std::all_of(QueryAnalysisComplete.begin(), QueryAnalysisComplete.end(),
                  [](bool Complete) { return Complete; });
  if (!IssuedQueriesComplete) {
    I386GOTOFFGraphQueryBudgetExhaustedForTesting |=
        I386GOTOFFProposalEvidenceRemaining == 0;
    // A graph/resource-incomplete query has no cacheable semantic result.
    // Mark the candidate stage incomplete transactionally; a later graph with
    // a fresh allowance must replay both authenticated and ambiguous paths.
    I386GOTOFFProposalEvidenceIncomplete = true;
    RestoreProofRoots();
    return false;
  }
  I386GOTOFFModelReachResult Result;
  Result.Authenticated =
      AuthenticatedQuery && QueryComplete(*AuthenticatedQuery) &&
      Matches[*AuthenticatedQuery];
  Result.AmbiguousQueryIssued = AmbiguousQuery.has_value();
  Result.AmbiguousReach =
      AmbiguousQuery && QueryComplete(*AmbiguousQuery) &&
      Matches[*AmbiguousQuery];
  // Prepay both the generation-local replay records and the cache's ordered
  // insertion/key ownership before publishing any of them.  CacheKey's root
  // vector is copied into each set but moved into the cache node.
  if (Result.AmbiguousQueryIssued) {
    if (!PrepayKeySetInsert(StageReplayedI386GOTPCKeys) ||
        (Result.AmbiguousReach &&
         !PrepayKeySetInsert(CurrentI386GOTOFFAmbiguityKeys))) {
      RestoreProofRoots();
      return false;
    }
  }
  if (!ConsumeProduct(
          CacheKeyWork,
          OrderedLookupWork(I386GOTOFFModelReachCache.size() + 1)) ||
      !ConsumeProduct(CacheKeyWork, 2) ||
      !consumeI386GOTOFFProposalEvidence(1)) {
    RestoreProofRoots();
    return false;
  }
  if (Result.AmbiguousQueryIssued)
    StageReplayedI386GOTPCKeys.insert(ReplayKey);
  if (Result.AmbiguousReach)
    CurrentI386GOTOFFAmbiguityKeys.insert(ReplayKey);
  if (Result.AmbiguousReach)
    I386GOTOFFAmbiguousModelReach = true;
  I386GOTOFFModelReachCache.emplace(std::move(CacheKey), Result);
  RestoreProofRoots();
  return Result.Authenticated && !Result.AmbiguousReach;
}

/// Resolve a LOAD address of the form INT_ADD(base, index*scale) into its
/// base register, requiring a genuine scaled index so plain pointer loads
/// are not mistaken for tables.
bool CFGBuilder::analyzeTableLoadAddr(
    const std::vector<LowOp> &Ops, int FromIdx, const NdVar &AddrV,
    uint64_t &BaseReg, uint64_t &IndexReg, bool &HasScaledIndex, uint64_t &Disp,
    va_t *AddrAddVA, NdVar *IndexValue, va_t *IndexUseAddr, int *IndexUseSeq,
    JumpTableValueOccurrence *AddressOccurrence,
    JumpTableFrameAddressUse *FrameRuntimeBase) const {
  Disp = 0;
  int AddIdx = reachingDefIdx(Ops, FromIdx, AddrV);
  // The effective address may be materialised in a register and copied to the
  // load operand (`lea base(,idx,8),%rN; mov %rN,%rM; jmp *(%rM)` — the
  // threaded/interleaved dispatch shape); follow the COPY chain (through both
  // temps and registers) to the defining INT_ADD.
  for (int Guard = 0; AddIdx >= 0 && Guard < limits::kMaxQuasiCopyDepth;
       ++Guard) {
    const LowOp &Transport = Ops[AddIdx];
    const bool PlainCopy =
        Transport.Opcode == NdOp::COPY && Transport.NumInputs >= 1;
    // i386 effective addresses are computed modulo the 32-bit guest pointer
    // width and then widened to NeverD's internal 64-bit VA container.  That
    // widening preserves the complete address coordinate; a narrower source
    // (or an arbitrary ZEXT in a 64-bit guest) does not.  Keep this exception
    // local to the LOAD-address owner rather than teaching legacy frame-slot
    // or value-provenance walkers that every extension is address preserving.
    const bool CanonicalGuestAddressWiden =
        Transport.Opcode == NdOp::INT_ZEXT && Transport.NumInputs >= 1 &&
        CurrentImg &&
        Transport.Inputs[0].Size == CurrentImg->getPointerSize() &&
        Transport.Output.Size >= Transport.Inputs[0].Size;
    if ((!PlainCopy && !CanonicalGuestAddressWiden) ||
        (!Transport.Inputs[0].isReg() && !Transport.Inputs[0].isTemp()))
      break;
    AddIdx = reachingDefIdx(Ops, AddIdx - 1, Transport.Inputs[0]);
  }
  if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddIdx].NumInputs < 2)
    return false;

  for (int Which = 0; Which < 2; ++Which) {
    NdVar CandidateValue;
    va_t CandidateUseAddr = InvalidVA;
    int CandidateUseSeq = -1;
    uint64_t Idx =
        scaledIndexReg(Ops, AddIdx - 1, Ops[AddIdx].Inputs[Which],
                       &CandidateValue, &CandidateUseAddr, &CandidateUseSeq);
    if (Idx == InvalidVA)
      continue;
    uint64_t Reg =
        traceToRegister(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
    if (Reg != InvalidVA) {
      BaseReg = Reg;
      IndexReg = Idx;
      HasScaledIndex = true;
      if (IndexValue)
        *IndexValue = CandidateValue;
      if (IndexUseAddr)
        *IndexUseAddr = CandidateUseAddr;
      if (IndexUseSeq)
        *IndexUseSeq = CandidateUseSeq;
      // The base+index combining add: clang -O0 on ARM folds the scaled index
      // into the base register here (`add rB,rB,idx,lsl#k`), so a caller that
      // needs the *base* constant must fold rB before this add executes, not at
      // the load (where rB already holds base+index).
      if (AddrAddVA)
        *AddrAddVA = Ops[AddIdx].Addr;
      if (AddressOccurrence)
        *AddressOccurrence = {Ops[AddIdx].Output, Ops[AddIdx].Addr,
                              Ops[AddIdx].Seq,
                              /*DefinedAtPoint=*/true};
      if (FrameRuntimeBase)
        *FrameRuntimeBase = {{Ops[AddIdx].Inputs[1 - Which], Ops[AddIdx].Addr,
                              Ops[AddIdx].Seq, /*DefinedAtPoint=*/false},
                             /*ByteAddend=*/0};
      return true;
    }
  }

  // i386 PIC GOTOFF form: addr = (base + index*scale) + disp, where the GOTOFF
  // displacement is folded into the load.  Peel the outer constant and recurse
  // into the inner `base + index*scale`.
  for (int Which = 0; Which < 2; ++Which) {
    if (!Ops[AddIdx].Inputs[Which].isConst())
      continue;
    uint64_t D = Ops[AddIdx].Inputs[Which].Offset;
    int InnerIdx =
        reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[1 - Which]);
    if (InnerIdx < 0 || Ops[InnerIdx].Opcode != NdOp::INT_ADD ||
        Ops[InnerIdx].NumInputs < 2)
      continue;
    const bool ExactGOTOFF = isExactI386GOTOFFInput(Ops[AddIdx], Which);
    for (int W2 = 0; W2 < 2; ++W2) {
      NdVar CandidateValue;
      va_t CandidateUseAddr = InvalidVA;
      int CandidateUseSeq = -1;
      uint64_t Idx =
          scaledIndexReg(Ops, InnerIdx - 1, Ops[InnerIdx].Inputs[W2],
                         &CandidateValue, &CandidateUseAddr, &CandidateUseSeq);
      if (Idx == InvalidVA)
        continue;
      uint64_t Reg =
          traceToRegister(Ops, InnerIdx - 1, Ops[InnerIdx].Inputs[1 - W2]);
      if (Reg != InvalidVA) {
        std::optional<int64_t> SignedDisp = signedFrameDelta(
            Ops[AddIdx].Inputs[Which], Ops[AddIdx].Output.Size);
        const bool ExactModelReaches =
            ExactGOTOFF && exactI386ModelZeroReaches(Ops[InnerIdx], 1 - W2, D);
        if (!SignedDisp && ExactModelReaches)
          SignedDisp = static_cast<int32_t>(D);
        if (!SignedDisp)
          continue;
        BaseReg = Reg;
        IndexReg = Idx;
        HasScaledIndex = true;
        Disp = D;
        if (IndexValue)
          *IndexValue = CandidateValue;
        if (IndexUseAddr)
          *IndexUseAddr = CandidateUseAddr;
        if (IndexUseSeq)
          *IndexUseSeq = CandidateUseSeq;
        if (AddressOccurrence)
          *AddressOccurrence = {Ops[AddIdx].Output, Ops[AddIdx].Addr,
                                Ops[AddIdx].Seq,
                                /*DefinedAtPoint=*/true};
        if (FrameRuntimeBase)
          *FrameRuntimeBase = {{Ops[InnerIdx].Inputs[1 - W2],
                                Ops[InnerIdx].Addr, Ops[InnerIdx].Seq,
                                /*DefinedAtPoint=*/false},
                               *SignedDisp};
        return true;
      }
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// CFG-aware, lane-aware guard/index provenance
//===----------------------------------------------------------------------===//

bool CFGBuilder::tableIndexMatchesValueAtUse(const NdVar &Candidate,
                                             va_t UseAddr, int UseSeq,
                                             const JumpTableInfo &Info,
                                             bool AllowZeroExtension,
                                             bool AllowSignExtension) const {
  std::vector<JumpTableValueOccurrence> Alternatives =
      Info.IndexValueAlternatives;
  if (Alternatives.empty())
    Alternatives.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                            Info.IndexUseSeq, Info.IndexValueDefinedAtUse});
  std::vector<bool> Results = tableValuesMatchAtUses(
      {{Candidate, UseAddr, UseSeq, std::move(Alternatives), AllowZeroExtension,
        AllowSignExtension}});
  return !Results.empty() && Results.front();
}

std::vector<bool> CFGBuilder::tableValuesMatchAtUses(
    const std::vector<JumpTableValueQuery> &Queries, bool *AnalysisComplete,
    std::vector<bool> *QueryAnalysisComplete, va_t CandidateBranchOverride,
    const std::vector<va_t> *CandidateTargetsOverride,
    size_t *GraphWorkBudget, size_t LocalMatchEvidenceLimit) const {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  if (QueryAnalysisComplete)
    QueryAnalysisComplete->clear();
  RequestedCompleteJumpTableProof = true;
  size_t ResultStorage = Queries.size();
  if (QueryAnalysisComplete) {
    if (Queries.size() >
        std::numeric_limits<size_t>::max() - ResultStorage) {
      if (GraphWorkBudget)
        *GraphWorkBudget = 0;
      return {};
    }
    ResultStorage += Queries.size();
  }
  // A budgeted query may return an empty batch on exhaustion; every fixed-point
  // caller treats a size mismatch as incomplete.  This lets us charge output
  // storage before allocating it instead of allocating an attacker-sized false
  // vector after the shared allowance has already run out.  Null-budget legacy
  // callers retain the established one-result-per-query shape.
  const size_t ResultFixedWork = QueryAnalysisComplete ? 4 : 2;
  if ((ResultStorage != 0 &&
       3 > std::numeric_limits<size_t>::max() / ResultStorage) ||
      ResultStorage * 3 >
          std::numeric_limits<size_t>::max() - ResultFixedWork) {
    if (GraphWorkBudget)
      *GraphWorkBudget = 0;
    return {};
  }
  if (!consumeResolverGraphWork(
          GraphWorkBudget, ResultStorage * 3 + ResultFixedWork))
    return {};
  std::vector<bool> Results(Queries.size(), false);
  if (QueryAnalysisComplete)
    QueryAnalysisComplete->assign(Queries.size(), false);
  if (!JumpTableProofContextComplete || !CurrentImg || Queries.empty())
    return Results;
  bool Complete = true;
  if (QueryAnalysisComplete)
    std::fill(QueryAnalysisComplete->begin(), QueryAnalysisComplete->end(),
              true);
  auto markIncomplete = [&](size_t QueryIndex) {
    Complete = false;
    if (QueryAnalysisComplete)
      (*QueryAnalysisComplete)[QueryIndex] = false;
  };

  std::vector<ResolverInsnSnapshot> Snapshot;
  if (!copyResolverInsnSnapshots(
          Insns, Snapshot,
          [&](va_t Addr, const auto &Rec) -> const std::vector<va_t> & {
            return CandidateTargetsOverride &&
                           Addr == CandidateBranchOverride
                       ? *CandidateTargetsOverride
                       : Rec.JumpTableTargets;
          },
          [&](size_t Amount) {
            return consumeResolverGraphWork(GraphWorkBudget, Amount);
          })) {
    if (QueryAnalysisComplete)
      std::fill(QueryAnalysisComplete->begin(), QueryAnalysisComplete->end(),
                false);
    return Results;
  }
  const std::set<va_t> &ProofRoots = ActiveJumpTableProofRoots
                                         ? *ActiveJumpTableProofRoots
                                         : PersistentCFGRoots;
  bool GraphComplete = false;
  const ResolverFlowGraph Graph = buildResolverFlowGraph(
      Snapshot, BlockStarts, ProofRoots, DiscoveredCodeRefSources,
      [&](va_t Address, const std::set<va_t> *ActiveOwners) {
        return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners,
                                                   GraphWorkBudget);
      },
      GraphWorkBudget, &GraphComplete);
  if (!GraphComplete) {
    if (QueryAnalysisComplete)
      std::fill(QueryAnalysisComplete->begin(), QueryAnalysisComplete->end(),
                false);
    return Results;
  }
  // Graph construction is only the first half of one evidence query.  Keep
  // charging the same candidate-local account while reconstructing frame,
  // memory and value state, and while matching/symbolizing the resulting DAG.
  // A null pointer preserves the established unmetered callers outside the
  // candidate-local fixed point; a supplied account never receives a fresh
  // post-graph allowance.
  bool EvidenceBudgetExhausted = false;
  auto consumeEvidence = [&](size_t Amount = 1) {
    if (!consumeResolverGraphWork(GraphWorkBudget, Amount)) {
      EvidenceBudgetExhausted = true;
      Complete = false;
      return false;
    }
    return true;
  };
  auto consumeEvidenceSum =
      [&](std::initializer_list<size_t> Terms) -> bool {
    const size_t Max = std::numeric_limits<size_t>::max();
    size_t Total = 0;
    for (size_t Term : Terms) {
      if (Term > Max - Total) {
        if (GraphWorkBudget)
          *GraphWorkBudget = 0;
        EvidenceBudgetExhausted = true;
        Complete = false;
        return false;
      }
      Total += Term;
    }
    return consumeEvidence(Total);
  };
  auto consumeEvidenceProduct = [&](size_t Count, size_t Cost) {
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count) {
      if (GraphWorkBudget)
        *GraphWorkBudget = 0;
      EvidenceBudgetExhausted = true;
      Complete = false;
      return false;
    }
    return consumeEvidence(Count * Cost);
  };
  auto namedResolverRoot =
      [&](uint16_t Size, std::string_view Prefix,
          std::initializer_list<uint64_t> Fields) -> ResolverValue {
    ResolverRootKey Root;
    return makeResolverRootKey(Root, Prefix, Fields, consumeEvidence,
                               &EvidenceBudgetExhausted)
               ? budgetedResolverRoot(Size, Root.view(), consumeEvidence)
               : ResolverValue{};
  };
  auto namedResolverTransform =
      [&](uint16_t Size, std::string_view Prefix,
          std::initializer_list<uint64_t> Fields,
          std::vector<ResolverValue> Inputs,
          std::optional<NdOp> Opcode = std::nullopt) -> ResolverValue {
    ResolverRootKey Root;
    return makeResolverRootKey(Root, Prefix, Fields, consumeEvidence,
                               &EvidenceBudgetExhausted)
               ? budgetedResolverTransform(
                      Size, Root.view(), std::move(Inputs), Opcode,
                      consumeEvidence)
                : ResolverValue{};
  };
  auto instructionIsGuarded = [&](va_t Addr) -> std::optional<bool> {
    if (!consumeEvidence(
            orderedSetLookupWork(Graph.InstructionGuards.size())))
      return std::nullopt;
    return Graph.InstructionGuards.count(Addr) != 0;
  };
  auto consumeProofPointLookup = [&] {
    constexpr size_t ProofPointKeyWork = 2;
    return consumeEvidenceProduct(
        ProofPointKeyWork, orderedSetLookupWork(Graph.PointToOp.size()));
  };
  auto consumeMemoLookup = [&](size_t KeyWork, size_t Count) {
    return consumeEvidenceProduct(KeyWork, orderedSetLookupWork(Count));
  };
  auto consumeMemoInsert = [&](size_t KeyWork, size_t Count) {
    // try_emplace performs its own ordered lookup after the preceding find.
    // Retain and eventually destroy every fixed-width key field, the mapped
    // optional state, and the tree node before allocating persistent memo
    // state.  The prepaid node lifetime also covers later clear/destruction.
    return consumeMemoLookup(KeyWork, Count) &&
           consumeEvidenceSum({KeyWork * 2, size_t{3}});
  };
  auto consumeImageInventoryProduct = [&](size_t Count, size_t Factor) {
    return consumeEvidenceProduct(Count, Factor);
  };
  auto consumeExecutableOwnerQuery = [&]() {
    // hasExecutableCodeOwnerAt resolves the mapped owner, exact section/code
    // identity, two import-stub spellings and the function-range inventories.
    // Pay the full worst-case helper traversal before entering it.
    return consumeImageInventoryProduct(CurrentImg->Segments.size(), 8) &&
           consumeImageInventoryProduct(CurrentImg->Sections.size(), 8) &&
           consumeImageInventoryProduct(CurrentImg->Imports.size(), 2) &&
           consumeImageInventoryProduct(CurrentImg->ImportStubRanges.size(),
                                        2) &&
           consumeEvidenceProduct(
               2, orderedSetLookupWork(
                      CurrentImg->ImportStubIndices.size())) &&
           consumeEvidenceProduct(
               2, orderedSetLookupWork(
                      CurrentImg->RuntimeFunctionAddrs.size())) &&
           consumeEvidence(orderedSetLookupWork(
               CurrentImg->VerifiedFunctionEntries.size())) &&
           consumeEvidence(CurrentImg->KnownCodeRanges.size()) &&
           consumeEvidence(CurrentImg->Symbols.size());
  };
  auto consumeObjectDataQuery = [&]() {
    // hasObjectDataProvenance performs a segment lookup, a section lookup and,
    // on Mach-O/fallback paths, another complete code/section classification.
    return consumeImageInventoryProduct(CurrentImg->Segments.size(), 4) &&
           consumeImageInventoryProduct(CurrentImg->Sections.size(), 4);
  };
  auto consumeRelocatedOwnerQuery = [&]() {
    // relocatedTargetBelongsToOwner scans both owner inventories and can issue
    // two executable-owner classifications for the selected owner.
    return consumeEvidenceSum(
               {CurrentImg->Segments.size(), CurrentImg->Sections.size()}) &&
           consumeExecutableOwnerQuery() && consumeExecutableOwnerQuery();
  };
  auto consumeSymbolNameLookup = [&](std::string_view Name) {
    if (!consumeEvidence(CurrentImg->Symbols.size()))
      return false;
    size_t SymbolBytes = 0;
    for (const Symbol &Sym : CurrentImg->Symbols) {
      if (Sym.Name.size() >
          std::numeric_limits<size_t>::max() - SymbolBytes)
        return consumeEvidence(std::numeric_limits<size_t>::max());
      SymbolBytes += Sym.Name.size();
    }
    return consumeEvidence(SymbolBytes) &&
           consumeEvidenceProduct(CurrentImg->Symbols.size(), Name.size());
  };
  if (!consumeEvidenceProduct(Graph.Blocks.size(), 3) || !consumeEvidence(2) ||
      !consumeEvidence(Graph.RootBlocks.size())) {
    if (QueryAnalysisComplete)
      std::fill(QueryAnalysisComplete->begin(), QueryAnalysisComplete->end(),
                false);
    return Results;
  }
  std::vector<bool> IsRootBlock(Graph.Blocks.size(), false);
  for (int Root : Graph.RootBlocks) {
    if (Root < 0 || Root >= static_cast<int>(IsRootBlock.size())) {
      Complete = false;
      if (QueryAnalysisComplete)
        std::fill(QueryAnalysisComplete->begin(), QueryAnalysisComplete->end(),
                  false);
      return Results;
    }
    IsRootBlock[Root] = true;
  }
  auto budgetedReachingDefIdx = [&](const std::vector<LowOp> &Ops,
                                    int FromIdx, const NdVar &Value) {
    for (int I = FromIdx; I >= 0; --I) {
      if (!consumeEvidence())
        return -1;
      const NdVar &Output = Ops[I].Output;
      if (Output.Space == Value.Space && Output.Offset == Value.Offset)
        return I;
    }
    return -1;
  };
  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
  const llvm::ArrayRef<uint64_t> IntParamRegs =
      TRI.integerParamRegs(CurrentImg->Format);
  const std::vector<TargetRegisterRange> CallPreservedRanges =
      TRI.callPreservedRanges(CurrentImg->Format);
  struct LaneView {
    VnodeSpace Space = VnodeSpace::CONST;
    uint64_t Container = InvalidVA;
    uint16_t ContainerSize = 0;
    uint16_t Begin = 0;
    uint16_t Size = 0;
    bool Valid = false;
  };
  auto viewOf = [&](const NdVar &V) -> LaneView {
    LaneView View;
    if (V.Size == 0 || (!V.isReg() && !V.isTemp()))
      return View;
    View.Space = V.Space;
    View.Size = V.Size;
    if (V.isTemp()) {
      View.Container = V.Offset;
      View.ContainerSize = V.Size;
      View.Valid = true;
      return View;
    }
    auto [WideOff, WideSize] = TRI.findWideReg(V.Offset, V.Size);
    int ByteOffset = TRI.subRegByteOffset(V.Offset, V.Size, WideOff, WideSize);
    if (ByteOffset < 0) {
      if (V.Offset < WideOff || V.Offset - WideOff > WideSize ||
          V.Size > WideSize - (V.Offset - WideOff))
        return View;
      ByteOffset = static_cast<int>(V.Offset - WideOff);
    }
    View.Container = WideOff;
    View.ContainerSize = WideSize;
    View.Begin = static_cast<uint16_t>(ByteOffset);
    View.Valid = true;
    return View;
  };
  auto sameContainer = [](const LaneView &A, const LaneView &B) {
    return A.Valid && B.Valid && A.Space == B.Space &&
           A.Container == B.Container;
  };

  using ValueKey = std::tuple<int, int, uint8_t, uint64_t, uint16_t>;
  using MemoryKey = std::tuple<int, int, uint64_t, int64_t, uint16_t>;
  // A disengaged memo value is the single prepaid Active state.  Reusing the
  // same map node for the completed result avoids allocating an Active-set node
  // and then a second memo node after the shared allowance has been consumed.
  std::map<ValueKey, std::optional<ResolverResult>> ValueMemo;
  std::map<MemoryKey, std::optional<ResolverResult>> MemoryMemo;

  std::function<ResolverResult(int, int, const NdVar &, unsigned)> resolveValue;
  std::function<ResolverResult(int, int, uint64_t, int64_t, uint16_t, unsigned)>
      resolveMemory;
  const JumpTableValueQuery *ActiveFrameMemoryQuery = nullptr;

  auto constantValue = [&](const NdVar &V) -> ResolverResult {
    if (!V.isConst() || V.Size == 0)
      return resolverInvalid();
    return resolverValue(budgetedResolverConstant(
        V.Offset, V.Size, V.Provenance, V.AddressOwnerVA, consumeEvidence));
  };
  auto resolveOperand = [&](int Block, int Before, const NdVar &V,
                            unsigned Depth) -> ResolverResult {
    return V.isConst() ? constantValue(V)
                       : resolveValue(Block, Before, V, Depth);
  };
  auto applyNumericOperandRole = [&](const LowOp &Use, unsigned InputIndex,
                                     ResolverResult Result) -> ResolverResult {
    if (Result.Kind != ResolverResultKind::Value ||
        !isNumericConstantOperand(Use.Opcode, InputIndex) ||
        !isRoleNeutralNumericOccurrence(Result.Value))
      return Result;
    // COPY deliberately transports an encoded immediate without deciding
    // whether its bits are a pointer.  Once that value reaches a numeric LowIR
    // operand, classify this use exactly as LiftState::emit and MedPropagation
    // do.  Clone only Unknown: loader-authenticated address/fragment roles are
    // immutable and therefore cannot impersonate a modulo reciprocal.
    return resolverValue(budgetedResolverConstant(
        Result.Value->Constant, Result.Value->Size,
        ConstantAddressProvenance::Scalar, InvalidVA, consumeEvidence));
  };

  auto relocatedLiteralValue = [&](va_t Slot, uint16_t Size) -> ResolverValue {
    // ARM ELF materializes a data/code address as
    //   ldr rN, [pc, #literal]
    //   add rN, pc, rN
    // where the literal carries R_ARM_REL32.  The loaded word is not itself a
    // pointer; it is a relocation-authenticated fragment whose owner is the
    // relocation symbol.  Preserve that owner so the exact ADD below can
    // complete the address without treating an arbitrary integer literal as
    // relocation evidence.
    if (!CurrentImg->isELF() || CurrentImg->Arch != Arch::ARM || Size != 4)
      return {};
    if (!consumeEvidence(CurrentImg->Segments.size()))
      return {};
    const Segment *SlotSegment = CurrentImg->getSegmentFor(Slot);
    if (!SlotSegment || !SlotSegment->isReadable() || SlotSegment->isWritable())
      return {};
    const RelocationEntry *Relocation = nullptr;
    for (const RelocationEntry &Candidate : CurrentImg->Relocations) {
      if (!consumeEvidence())
        return {};
      if (Candidate.Address == Slot &&
          Candidate.Type == llvm::ELF::R_ARM_REL32) {
        if (Relocation)
          return {};
        Relocation = &Candidate;
      }
    }
    if (!Relocation || Relocation->SymbolName.empty())
      return {};
    if (!consumeSymbolNameLookup(Relocation->SymbolName))
      return {};
    const Symbol *Target = CurrentImg->findSymbol(Relocation->SymbolName);
    if (!Target)
      return {};
    va_t Owner = InvalidVA;
    if (!consumeEvidenceSum(
            {CurrentImg->Segments.size(), CurrentImg->Sections.size()}))
      return {};
    if (const Section *TargetSection = CurrentImg->getSectionFor(Target->Addr))
      Owner = TargetSection->VA;
    else {
      if (!consumeEvidence(CurrentImg->Segments.size()))
        return {};
      if (const Segment *TargetSegment =
              CurrentImg->getSegmentFor(Target->Addr))
        Owner = TargetSegment->VA;
    }
    if (Owner == InvalidVA)
      return {};
    if (!consumeEvidence(CurrentImg->Segments.size()))
      return {};
    const uint8_t *Bytes = CurrentImg->readVA(Slot, Size);
    if (!Bytes)
      return {};
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, Bytes, sizeof(Encoded));
    return budgetedResolverConstant(
        Encoded, Size, ConstantAddressProvenance::AddressFragment, Owner,
        consumeEvidence);
  };

  auto projectDefinition = [&](ResolverValue Full, const LaneView &Output,
                               const LaneView &Query,
                               bool ZeroExtendingWrite) -> ResolverResult {
    if (!Full || !sameContainer(Output, Query))
      return resolverInvalid();
    const uint32_t OBegin = Output.Begin;
    const uint32_t OEnd = OBegin + Output.Size;
    const uint32_t QBegin = Query.Begin;
    const uint32_t QEnd = QBegin + Query.Size;
    if (OBegin <= QBegin && QEnd <= OEnd)
      return resolverValue(resolverSlice(
          Full, static_cast<uint16_t>(QBegin - OBegin), Query.Size,
          consumeEvidence, &EvidenceBudgetExhausted));
    if (!ZeroExtendingWrite)
      return resolverInvalid();
    // A W/E-register write defines the full X/R register.  Bytes above the
    // narrow output become zero; a full-width read is the explicit zero-
    // extension of the written value.  Arbitrary cross-boundary subviews are
    // rejected rather than reconstructed heuristically.
    if (QBegin >= OEnd && QEnd <= Output.ContainerSize)
      return resolverValue(budgetedResolverZero(Query.Size, consumeEvidence));
    if (OBegin == 0 && QBegin == 0 && QEnd == Output.ContainerSize)
      return resolverValue(resolverExtend(Full, Query.Size, false,
                                          consumeEvidence,
                                          &EvidenceBudgetExhausted));
    return resolverInvalid();
  };

  // Canonicalize every frame address to an offset from the incoming stack
  // pointer.  A physical SP/FP register number is not a memory identity: push,
  // pop, dynamic adjustment, and FP setup create different epochs of that
  // register.  This point-sensitive affine dataflow proves equal epochs across
  // CFG joins and rejects any ambiguous or non-affine update.
  enum class FrameResultKind : uint8_t { Invalid, Cycle, Value };
  struct FrameResult {
    FrameResultKind Kind = FrameResultKind::Invalid;
    int64_t Offset = 0;
  };
  auto frameInvalid = [] { return FrameResult{}; };
  auto frameCycle = [](int64_t Delta = 0) {
    return FrameResult{FrameResultKind::Cycle, Delta};
  };
  auto frameValue = [](int64_t Offset) {
    return FrameResult{FrameResultKind::Value, Offset};
  };
  using FrameKey = std::tuple<int, int, uint64_t>;
  std::map<FrameKey, std::optional<FrameResult>> FrameMemo;
  std::function<FrameResult(int, int, uint64_t, unsigned)> resolveFrameBase;
  std::function<FrameResult(int, int, const NdVar &, unsigned)> resolveFrameVar;

  auto mergeFrameResults = [&](llvm::ArrayRef<FrameResult> Incoming,
                               bool IgnoreTransparentCycles) {
    bool SawCycle = false;
    bool SawValue = false;
    int64_t Common = 0;
    int64_t CycleDelta = 0;
    for (const FrameResult &R : Incoming) {
      if (!consumeEvidence())
        return frameInvalid();
      if (R.Kind == FrameResultKind::Cycle) {
        if (!SawCycle) {
          CycleDelta = R.Offset;
          SawCycle = true;
        } else if (CycleDelta != R.Offset) {
          return frameInvalid();
        }
        continue;
      }
      if (R.Kind != FrameResultKind::Value)
        return frameInvalid();
      if (!SawValue) {
        Common = R.Offset;
        SawValue = true;
      } else if (Common != R.Offset) {
        return frameInvalid();
      }
    }
    if (!SawValue)
      return SawCycle ? frameCycle(CycleDelta) : frameInvalid();
    if (SawCycle && (!IgnoreTransparentCycles || CycleDelta != 0))
      return frameInvalid();
    return frameValue(Common);
  };

  auto adjustFrame = [&](FrameResult Base, const NdVar &Constant,
                         uint16_t ArithmeticSize, bool Subtract) {
    if (Base.Kind == FrameResultKind::Invalid)
      return frameInvalid();
    const std::optional<int64_t> Delta =
        signedFrameDelta(Constant, ArithmeticSize);
    if (!Delta)
      return frameInvalid();
    const std::optional<int64_t> Offset =
        checkedFrameOffset(Base.Offset, *Delta, Subtract);
    if (!Offset)
      return frameInvalid();
    // Preserve the affine delta while traversing a recursive frame cycle.  A
    // balanced push/pop path returns Cycle(0) and is transparent at the loop
    // header; any non-zero net adjustment is rejected by mergeFrameResults.
    return Base.Kind == FrameResultKind::Cycle ? frameCycle(*Offset)
                                               : frameValue(*Offset);
  };

  resolveFrameVar = [&](int Block, int Before, const NdVar &Value,
                        unsigned Depth) -> FrameResult {
    if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
      EvidenceBudgetExhausted = true;
      Complete = false;
      return frameInvalid();
    }
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0 || !CurrentImg ||
        Value.Size < CurrentImg->getPointerSize() ||
        ((!Value.isReg() && !Value.isTemp()) &&
         Value.Size != CurrentImg->getPointerSize()))
      return frameInvalid();
    if (!consumeEvidence())
      return frameInvalid();
    if (Value.isReg())
      return resolveFrameBase(Block, Before, Value.Offset, Depth);
    if (!Value.isTemp())
      return frameInvalid();
    const std::vector<LowOp> &Ops = Graph.Blocks[Block].Ops;
    const int DefIndex = budgetedReachingDefIdx(
        Ops, std::min(Before, static_cast<int>(Ops.size())) - 1, Value);
    if (DefIndex < 0)
      return frameInvalid();
    const LowOp &Def = Ops[DefIndex];
    FrameResult Result = frameInvalid();
    if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
      Result = resolveFrameVar(Block, DefIndex, Def.Inputs[0], Depth + 1);
    } else if (Def.Opcode == NdOp::INT_ZEXT && Def.NumInputs >= 1 &&
               CurrentImg &&
               Def.Inputs[0].Size == CurrentImg->getPointerSize() &&
               Def.Output.Size > Def.Inputs[0].Size) {
      Result = resolveFrameVar(Block, DefIndex, Def.Inputs[0], Depth + 1);
    } else if (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
               CurrentImg &&
               Def.Output.Size == CurrentImg->getPointerSize() &&
               Def.Inputs[0].Size > Def.Output.Size &&
               Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0) {
      const int WidenIndex =
          budgetedReachingDefIdx(Ops, DefIndex - 1, Def.Inputs[0]);
      if (WidenIndex >= 0) {
        const LowOp &Widen = Ops[WidenIndex];
        if (Widen.Opcode == NdOp::INT_ZEXT && Widen.NumInputs >= 1 &&
            Widen.Output == Def.Inputs[0] &&
            Widen.Inputs[0].Size == CurrentImg->getPointerSize() &&
            Widen.Output.Size > Widen.Inputs[0].Size)
          Result =
              resolveFrameVar(Block, WidenIndex, Widen.Inputs[0], Depth + 1);
      }
    } else if ((Def.Opcode == NdOp::INT_ADD ||
                Def.Opcode == NdOp::INT_SUB) &&
               Def.NumInputs >= 2) {
      if (Def.Inputs[1].isConst())
        Result = adjustFrame(
                             resolveFrameVar(Block, DefIndex, Def.Inputs[0],
                                             Depth + 1),
                             Def.Inputs[1], Def.Output.Size,
                             Def.Opcode == NdOp::INT_SUB);
      else if (Def.Opcode == NdOp::INT_ADD && Def.Inputs[0].isConst())
        Result = adjustFrame(
                             resolveFrameVar(Block, DefIndex, Def.Inputs[1],
                                             Depth + 1),
                             Def.Inputs[0], Def.Output.Size, false);
    }
    const std::optional<bool> Guarded = instructionIsGuarded(Def.Addr);
    if (!Guarded)
      return frameInvalid();
    if (*Guarded)
      Result = mergeFrameResults(
          {resolveFrameVar(Block, DefIndex, Value, Depth + 1), Result},
          /*IgnoreTransparentCycles=*/false);
    return Result;
  };

  resolveFrameBase = [&](int Block, int Before, uint64_t BaseReg,
                         unsigned Depth) -> FrameResult {
    if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
      EvidenceBudgetExhausted = true;
      Complete = false;
      return frameInvalid();
    }
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0)
      return frameInvalid();
    constexpr size_t FrameKeyWork = 3;
    FrameKey Key{Block, Before, BaseReg};
    if (!consumeMemoLookup(FrameKeyWork, FrameMemo.size()))
      return frameInvalid();
    auto MemoIt = FrameMemo.find(Key);
    if (MemoIt != FrameMemo.end())
      return MemoIt->second ? *MemoIt->second : frameCycle();
    if (!consumeMemoInsert(FrameKeyWork, FrameMemo.size()))
      return frameInvalid();
    MemoIt = FrameMemo.try_emplace(Key, std::nullopt).first;

    const ResolverFlowBlock &B = Graph.Blocks[Block];
    const LaneView Query = viewOf(NdVar::reg(
        BaseReg, static_cast<uint16_t>(CurrentImg->getPointerSize())));
    FrameResult Result = frameInvalid();
    bool Found = false;
    for (int I = std::min(Before, static_cast<int>(B.Ops.size())) - 1; I >= 0;
         --I) {
      if (!consumeEvidence()) {
        Found = true;
        break;
      }
      const LowOp &Def = B.Ops[I];
      if (Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL) {
        const bool QueryPreserved = [&] {
          if (TRI.isStackPointer(BaseReg))
            return true;
          if (!Query.Valid || Query.Begin > InvalidVA - Query.Container)
            return false;
          const uint64_t Begin = Query.Container + Query.Begin;
          if (Query.Size > InvalidVA - Begin)
            return false;
          const uint64_t End = Begin + Query.Size;
          if (!consumeEvidence(CallPreservedRanges.size()))
            return false;
          return std::any_of(CallPreservedRanges.begin(),
                             CallPreservedRanges.end(),
                             [&](const TargetRegisterRange &Range) {
                               if (Range.Bytes > InvalidVA - Range.Offset)
                                 return false;
                               return Begin >= Range.Offset &&
                                      End <= Range.Offset + Range.Bytes;
                             });
        }();
        if (!QueryPreserved) {
          Found = true;
          Result = frameInvalid();
          break;
        }
      }
      const LaneView Output = viewOf(Def.Output);
      if (!sameContainer(Output, Query))
        continue;
      const uint32_t OBegin = Output.Begin;
      const uint32_t OEnd = OBegin + Output.Size;
      const uint32_t QBegin = Query.Begin;
      const uint32_t QEnd = QBegin + Query.Size;
      if (!(OBegin < QEnd && QBegin < OEnd))
        continue;
      Found = true;
      // A partial write to SP/FP cannot preserve an affine frame identity.
      if (OBegin > QBegin || OEnd < QEnd) {
        Result = frameInvalid();
        break;
      }
      if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
        Result = resolveFrameVar(Block, I, Def.Inputs[0], Depth + 1);
      } else if (Def.Opcode == NdOp::INT_ZEXT && Def.NumInputs >= 1 &&
                 CurrentImg &&
                 Def.Inputs[0].Size == CurrentImg->getPointerSize() &&
                 Def.Output.Size > Def.Inputs[0].Size) {
        Result = resolveFrameVar(Block, I, Def.Inputs[0], Depth + 1);
      } else if (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
                 CurrentImg &&
                 Def.Output.Size == CurrentImg->getPointerSize() &&
                 Def.Inputs[0].Size > Def.Output.Size &&
                 Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0) {
        const int WidenIndex =
            budgetedReachingDefIdx(B.Ops, I - 1, Def.Inputs[0]);
        if (WidenIndex >= 0) {
          const LowOp &Widen = B.Ops[WidenIndex];
          if (Widen.Opcode == NdOp::INT_ZEXT && Widen.NumInputs >= 1 &&
              Widen.Output == Def.Inputs[0] &&
              Widen.Inputs[0].Size == CurrentImg->getPointerSize() &&
              Widen.Output.Size > Widen.Inputs[0].Size)
            Result =
                resolveFrameVar(Block, WidenIndex, Widen.Inputs[0], Depth + 1);
        }
      } else if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
                 Def.NumInputs >= 2) {
        if (Def.Inputs[1].isConst())
          Result = adjustFrame(
                               resolveFrameVar(Block, I, Def.Inputs[0],
                                               Depth + 1),
                               Def.Inputs[1], Def.Output.Size,
                               Def.Opcode == NdOp::INT_SUB);
        else if (Def.Opcode == NdOp::INT_ADD && Def.Inputs[0].isConst())
          Result = adjustFrame(
                               resolveFrameVar(Block, I, Def.Inputs[1],
                                               Depth + 1),
                               Def.Inputs[0], Def.Output.Size, false);
      }
      const std::optional<bool> Guarded = instructionIsGuarded(Def.Addr);
      if (!Guarded) {
        FrameMemo.erase(MemoIt);
        return frameInvalid();
      }
      if (*Guarded) {
        // A predicated frame-register write is a merge of the old and new
        // epochs.  Unless both paths prove the same canonical offset, the
        // frame identity is ambiguous and must fail closed.
        Result =
            mergeFrameResults(
                              {resolveFrameBase(Block, I, BaseReg, Depth + 1),
                               Result},
                              /*IgnoreTransparentCycles=*/false);
      }
      break;
    }

    if (!Found) {
      std::vector<FrameResult> Incoming;
      if (B.Preds.size() == std::numeric_limits<size_t>::max()) {
        consumeEvidence(std::numeric_limits<size_t>::max());
        FrameMemo.erase(MemoIt);
        return frameInvalid();
      }
      const size_t IncomingCapacity = B.Preds.size() + 1;
      if (!consumeEvidenceProduct(IncomingCapacity, 2) ||
          !consumeEvidence(2)) {
        FrameMemo.erase(MemoIt);
        return frameInvalid();
      }
      Incoming.reserve(IncomingCapacity);
      if (IsRootBlock[Block]) {
        if (!consumeEvidence()) {
          FrameMemo.erase(MemoIt);
          return frameInvalid();
        }
        if (B.Start == CurrentFuncEntry && BaseReg == TRI.StackPointer)
          Incoming.push_back(frameValue(0));
        else
          // A disconnected/address-taken root has an independent incoming
          // frame state.  It cannot borrow the function entry's spill slots.
          Incoming.push_back(frameInvalid());
      }
      for (int Pred : B.Preds) {
        if (!consumeEvidence()) {
          Incoming.clear();
          break;
        }
        FrameResult PredResult = resolveFrameBase(
            Pred, static_cast<int>(Graph.Blocks[Pred].Ops.size()), BaseReg,
            Depth + 1);
        if (!consumeEvidence()) {
          Incoming.clear();
          break;
        }
        Incoming.push_back(std::move(PredResult));
      }
      Result = Incoming.empty()
                   ? frameInvalid()
                   : mergeFrameResults(Incoming,
                                       /*IgnoreTransparentCycles=*/true);
    }

    if (Result.Kind == FrameResultKind::Cycle)
      FrameMemo.erase(MemoIt);
    else
      MemoIt->second = Result;
    return Result;
  };

  auto canonicalFrameSlotKey = [&](int Block, int FromIdx, const NdVar &Address,
                                   uint64_t &BaseReg, int64_t &Offset) {
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()))
      return false;
    // FromIdx names the last operation preceding this exact operand use.  The
    // shared frame resolver follows direct SP/FP values and derived GPR/TEMP
    // address expressions across CFG edges, while merging predicated writes
    // and stack-pointer epochs conservatively.
    NdVar GuestAddress = Address;
    if ((GuestAddress.isReg() || GuestAddress.isTemp()) && CurrentImg &&
        CurrentImg->getPointerSize() != 0 &&
        GuestAddress.Size > CurrentImg->getPointerSize())
      GuestAddress.Size = CurrentImg->getPointerSize();
    FrameResult AddressState =
        resolveFrameVar(Block, FromIdx + 1, GuestAddress, /*Depth=*/0);
    if (AddressState.Kind != FrameResultKind::Value)
      return false;
    BaseReg = TRI.StackPointer;
    Offset = AddressState.Offset;
    return true;
  };

  auto definitelyNonFrameAddress = [&](const std::vector<LowOp> &Ops,
                                       int Before, NdVar Address) {
    std::set<std::pair<VnodeSpace, uint64_t>> Seen;
    std::function<bool(NdVar, int, int)> Walk = [&](NdVar V, int From,
                                                    int Depth) -> bool {
      if (!consumeEvidence())
        return false;
      if (Depth >= limits::kMaxQuasiCopyDepth)
        return false;
      if (V.isConst())
        return isExactAddressProvenance(V.Provenance);
      if (V.isReg() && TRI.isFrameReg(V.Offset))
        return false;
      if (!V.isReg() && !V.isTemp())
        return false;
      constexpr size_t SeenKeyWork = 2;
      if (!consumeEvidenceProduct(
              SeenKeyWork, orderedSetLookupWork(Seen.size())) ||
          !consumeEvidenceSum({SeenKeyWork * 2, size_t{2}}))
        return false;
      if (!Seen.insert({V.Space, V.Offset}).second)
        return false;
      int D = budgetedReachingDefIdx(Ops, From, V);
      if (D < 0)
        return false;
      const LowOp &Def = Ops[D];
      switch (Def.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::SUBBYTES:
        return Def.NumInputs >= 1 && Walk(Def.Inputs[0], D - 1, Depth + 1);
      case NdOp::INT_ADD:
        if (Def.NumInputs < 2)
          return false;
        return (Walk(Def.Inputs[0], D - 1, Depth + 1) &&
                Def.Inputs[1].isConst() &&
                Def.Inputs[1].Provenance ==
                    ConstantAddressProvenance::Scalar) ||
               (Walk(Def.Inputs[1], D - 1, Depth + 1) &&
                Def.Inputs[0].isConst() &&
                Def.Inputs[0].Provenance == ConstantAddressProvenance::Scalar);
      case NdOp::INT_SUB:
        return Def.NumInputs >= 2 && Walk(Def.Inputs[0], D - 1, Depth + 1) &&
               Def.Inputs[1].isConst() &&
               Def.Inputs[1].Provenance == ConstantAddressProvenance::Scalar;
      case NdOp::SELECT:
        return Def.NumInputs >= 3 && Walk(Def.Inputs[1], D - 1, Depth + 1) &&
               Walk(Def.Inputs[2], D - 1, Depth + 1);
      default:
        return false;
      }
    };
    return Walk(Address, Before, 0);
  };

  auto isKnownMemsetCall = [&](const LowOp &Op) {
    if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
        !Op.Inputs[0].isConst())
      return false;
    const va_t Target = static_cast<va_t>(Op.Inputs[0].Offset);
    if (!consumeEvidence(CurrentImg->Imports.size()) ||
        !consumeEvidence(
            orderedSetLookupWork(CurrentImg->ImportStubIndices.size())))
      return false;
    if (const Import *Imp = CurrentImg->findImportAt(Target)) {
      if (!consumeEvidence(Imp->Name.size()))
        return false;
      if (libc::isMemSetName(Imp->Name))
        return true;
    }
    if (!consumeEvidence(CurrentImg->Symbols.size()))
      return false;
    if (const Symbol *Sym = CurrentImg->findSymbolAt(Target)) {
      if (!consumeEvidence(Sym->Name.size()))
        return false;
      if (libc::isMemSetName(Sym->Name))
        return true;
    }
    bool Found = false;
    for (const RelocationEntry &Relocation : CurrentImg->Relocations) {
      if (!consumeEvidence())
        return false;
      if (Relocation.Address != Op.Addr &&
          (Op.Addr == InvalidVA || Relocation.Address != Op.Addr + 1))
        continue;
      if (Relocation.SymbolName.empty())
        continue;
      if (!consumeEvidence(Relocation.SymbolName.size()))
        return false;
      if (!libc::isMemSetName(Relocation.SymbolName))
        continue;
      if (Found)
        return false;
      Found = true;
    }
    return Found;
  };

  resolveMemory = [&](int Block, int Before, uint64_t SlotBase,
                      int64_t SlotOffset, uint16_t Size,
                      unsigned Depth) -> ResolverResult {
    if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
      EvidenceBudgetExhausted = true;
      Complete = false;
      return resolverInvalid();
    }
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0 || Size == 0)
      return resolverInvalid();
    constexpr size_t MemoryKeyWork = 5;
    MemoryKey Key{Block, Before, SlotBase, SlotOffset, Size};
    if (!consumeMemoLookup(MemoryKeyWork, MemoryMemo.size()))
      return resolverInvalid();
    auto MemoIt = MemoryMemo.find(Key);
    if (MemoIt != MemoryMemo.end())
      return MemoIt->second ? *MemoIt->second : resolverCycle();
    if (!consumeMemoInsert(MemoryKeyWork, MemoryMemo.size()))
      return resolverInvalid();
    MemoIt = MemoryMemo.try_emplace(Key, std::nullopt).first;

    const ResolverFlowBlock &B = Graph.Blocks[Block];
    ResolverResult Result = resolverInvalid();
    bool Found = false;
    for (int I = std::min(Before, static_cast<int>(B.Ops.size())) - 1; I >= 0;
         --I) {
      if (!consumeEvidence()) {
        Found = true;
        break;
      }
      const LowOp &Op = B.Ops[I];
      // LowIR has no callee memory-effect summary.  A call (direct, indirect,
      // or predicated) can mutate a frame slot whose address escaped earlier;
      // an opaque side-effect intrinsic such as SVC/HVC/SMC has the same
      // contract.  Do not trace a reload through either to an older STORE.
      bool AuthenticatedMemcpy = false;
      if (ActiveFrameMemoryQuery) {
        if (!consumeEvidence(
                ActiveFrameMemoryQuery->AuthenticatedFrameMemcpyWriters
                    .size())) {
          Found = true;
          break;
        }
        AuthenticatedMemcpy = std::any_of(
            ActiveFrameMemoryQuery->AuthenticatedFrameMemcpyWriters.begin(),
            ActiveFrameMemoryQuery->AuthenticatedFrameMemcpyWriters.end(),
            [&](const JumpTableValueOccurrence &Writer) {
              return Writer.Addr == Op.Addr && Writer.Seq == Op.Seq;
            });
      }
      if (AuthenticatedMemcpy) {
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) {
          Found = true;
          Result = resolverInvalid();
          break;
        }
        const std::optional<bool> Guarded = instructionIsGuarded(Op.Addr);
        if (!Guarded) {
          Found = true;
          break;
        }
        if (*Guarded) {
          Found = true;
          Result = resolverInvalid();
          break;
        }
        Found = true;
        ResolverRootKey Root;
        if (!makeResolverRootKey(
                Root, "MCPY",
                {Op.Addr, static_cast<uint64_t>(Op.Seq),
                 static_cast<uint64_t>(SlotOffset), Size},
                consumeEvidence, &EvidenceBudgetExhausted)) {
          Result = resolverInvalid();
          break;
        }
        Result = resolverValue(
            budgetedResolverRoot(Size, Root.view(), consumeEvidence));
        break;
      }
      // A known memset is not an opaque whole-frame clobber when its exact
      // destination and byte count prove a disjoint frame interval at this
      // CALL point.  This is needed for clang's five-entry local computed-goto
      // staging shape: immutable entries are first spilled to a scratch range,
      // memset clears the final table range, and later LOAD/STORE pairs copy
      // scratch into that table.  Skipping the call by name alone would be
      // unsound, so guarded calls, stack ABIs, non-frame destinations,
      // non-scalar lengths, overflow, and any overlap all retain the ordinary
      // call barrier below.  A raw ownerless immediate may still carry
      // Unknown provenance in early LowIR; the known size_t argument position
      // gives that exact constant its numeric meaning.  Any relocation-backed
      // or owner-carrying address constant remains ineligible.
      bool ExactDisjointMemset = false;
      const bool KnownMemset = isKnownMemsetCall(Op);
      std::optional<bool> MemsetGuarded = false;
      if (KnownMemset)
        MemsetGuarded = instructionIsGuarded(Op.Addr);
      if (!MemsetGuarded) {
        Found = true;
        break;
      }
      if (KnownMemset && !*MemsetGuarded && IntParamRegs.size() >= 3 &&
          CurrentImg->getPointerSize() != 0) {
        const uint16_t PointerSize = CurrentImg->getPointerSize();
        const NdVar Destination = NdVar::reg(IntParamRegs[0], PointerSize);
        const NdVar Length = NdVar::reg(IntParamRegs[2], PointerSize);
        uint64_t DestinationBase = InvalidVA;
        int64_t DestinationOffset = 0;
        const ResolverResult LengthValue =
            resolveValue(Block, I, Length, Depth + 1);
        const bool HasDestination = canonicalFrameSlotKey(
            Block, I - 1, Destination, DestinationBase, DestinationOffset);
        const bool NumericLength =
            LengthValue.Kind == ResolverResultKind::Value &&
            LengthValue.Value &&
            LengthValue.Value->K == ResolverValueExpr::Kind::Constant &&
            (LengthValue.Value->Provenance ==
                 ConstantAddressProvenance::Scalar ||
             (LengthValue.Value->Provenance ==
                  ConstantAddressProvenance::Unknown &&
              LengthValue.Value->AddressOwnerVA == InvalidVA));
        if (HasDestination &&
            DestinationBase == SlotBase &&
            NumericLength &&
            LengthValue.Value->Size == PointerSize &&
            LengthValue.Value->Constant <=
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
          const int64_t ByteCount =
              static_cast<int64_t>(LengthValue.Value->Constant);
          const std::optional<int64_t> DestinationEnd = checkedFrameOffset(
              DestinationOffset, ByteCount, /*Subtract=*/false);
          const std::optional<int64_t> SlotEnd = checkedFrameOffset(
              SlotOffset, static_cast<int64_t>(Size), /*Subtract=*/false);
          ExactDisjointMemset =
              DestinationEnd && SlotEnd &&
              (*DestinationEnd <= SlotOffset || *SlotEnd <= DestinationOffset);
        }
      }
      if (ExactDisjointMemset)
        continue;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          intrinsicMayClobberFrameMemory(Op)) {
        Found = true;
        Result = resolverInvalid();
        break;
      }
      const bool IsStore = Op.Opcode == NdOp::STORE && Op.NumInputs >= 2;
      const bool IsAtomic =
          (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          Op.NumInputs >= 1;
      if (!IsStore && !IsAtomic)
        continue;
      const NdVar &Address =
          IsAtomic ? Op.Inputs[0]
                   : (Op.NumInputs >= 3 ? Op.Inputs[1] : Op.Inputs[0]);
      uint64_t StoreBase = InvalidVA;
      int64_t StoreOffset = 0;
      if (!canonicalFrameSlotKey(Block, I - 1, Address, StoreBase,
                                 StoreOffset)) {
        // An unknown pointer, a SELECT/PHI with any unproved arm, or another
        // unsupported address form may alias the queried frame slot.  Only an
        // occurrence-local exact non-frame address plus scalar arithmetic is
        // disjoint evidence; everything else kills the reaching spill.
        if (!definitelyNonFrameAddress(B.Ops, I - 1, Address)) {
          Found = true;
          Result = resolverInvalid();
          break;
        }
        continue;
      }
      // SP and FP are distinct register containers but may name the same
      // physical frame slot after a prologue adjustment.  Until this resolver
      // has a canonical cross-base frame key, a write through the other frame
      // base is a may-alias barrier rather than evidence that can be skipped.
      if (StoreBase != SlotBase) {
        Found = true;
        Result = resolverInvalid();
        break;
      }
      const NdVar &Stored =
          IsStore ? (Op.NumInputs >= 3 ? Op.Inputs[2] : Op.Inputs[1])
                  : Op.Output;
      const uint16_t StoredSize = Stored.Size;
      if (StoredSize == 0) {
        Found = true;
        Result = resolverInvalid();
        break;
      }
      const std::optional<int64_t> StoreEndOpt = checkedFrameOffset(
          StoreOffset, static_cast<int64_t>(StoredSize), false);
      const std::optional<int64_t> LoadEndOpt =
          checkedFrameOffset(SlotOffset, static_cast<int64_t>(Size), false);
      if (!StoreEndOpt || !LoadEndOpt) {
        Found = true;
        Result = resolverInvalid();
        break;
      }
      const int64_t StoreEnd = *StoreEndOpt;
      const int64_t LoadEnd = *LoadEndOpt;
      if (StoreEnd <= SlotOffset || LoadEnd <= StoreOffset)
        continue;
      Found = true;
      // Every atomic RMW is a memory definition.  Modeling XCHG/ADD/CMPXCHG
      // precisely is unnecessary for value-identity proof: an overlapping
      // update (predicated or unconditional) invalidates the older spill.
      if (IsAtomic) {
        Result = resolverInvalid();
        break;
      }
      if (StoreOffset > SlotOffset || StoreEnd < LoadEnd) {
        Result = resolverInvalid();
        break;
      }
      if (ActiveFrameMemoryQuery) {
        if (!consumeEvidence(
                ActiveFrameMemoryQuery->AuthenticatedFrameStoreWriters
                    .size())) {
          Result = resolverInvalid();
          break;
        }
        if (std::none_of(
                ActiveFrameMemoryQuery->AuthenticatedFrameStoreWriters.begin(),
                ActiveFrameMemoryQuery->AuthenticatedFrameStoreWriters.end(),
                [&](const JumpTableValueOccurrence &Writer) {
                  return Writer.Addr == Op.Addr && Writer.Seq == Op.Seq;
                })) {
          Result = resolverInvalid();
          break;
        }
      }
      ResolverResult StoredValue =
          resolveOperand(Block, I, Stored, Depth + 1);
      if (StoredValue.Kind != ResolverResultKind::Value) {
        // A cycle propagated through an actual STORE is a loop-carried memory
        // definition, not a transparent CFG back-edge.  Treating it as a raw
        // cycle would let an earlier spill survive a recurrence that may write
        // a different value on every iteration.
        Result = resolverInvalid();
        break;
      }
      Result = resolverValue(
          resolverSlice(StoredValue.Value,
                        static_cast<uint16_t>(SlotOffset - StoreOffset), Size,
                        consumeEvidence, &EvidenceBudgetExhausted));
      const std::optional<bool> Guarded = instructionIsGuarded(Op.Addr);
      if (!Guarded) {
        Result = resolverInvalid();
        break;
      }
      if (*Guarded) {
        ResolverResult Old = resolveMemory(Block, I, SlotBase, SlotOffset,
                                           Size, Depth + 1);
        if (!consumeEvidence(4)) {
          Result = resolverInvalid();
          break;
        }
        const std::array<ResolverResult, 2> MergeInputs = {Old, Result};
        Result = mergeResolverResults(MergeInputs, {}, false, consumeEvidence,
                                      &EvidenceBudgetExhausted);
      }
      break;
    }

    if (!Found) {
      std::vector<ResolverResult> Incoming;
      if (!consumeEvidenceProduct(B.Preds.size(), 2) ||
          !consumeEvidence(2)) {
        MemoryMemo.erase(MemoIt);
        return resolverInvalid();
      }
      Incoming.reserve(B.Preds.size());
      for (int Pred : B.Preds) {
        if (!consumeEvidence(2)) {
          Incoming.clear();
          break;
        }
        Incoming.push_back(
            resolveMemory(Pred, static_cast<int>(Graph.Blocks[Pred].Ops.size()),
                          SlotBase, SlotOffset, Size, Depth + 1));
      }
      if (Incoming.empty()) {
        Result = resolverInvalid();
      } else {
        ResolverRootKey MergeRoot;
        Result = makeResolverRootKey(
                     MergeRoot, "M",
                     {B.Start, SlotBase, static_cast<uint64_t>(SlotOffset)},
                     consumeEvidence, &EvidenceBudgetExhausted)
                     ? mergeResolverResults(
                           Incoming, MergeRoot.view(),
                           /*IgnoreTransparentCycles=*/true, consumeEvidence,
                           &EvidenceBudgetExhausted)
                     : resolverInvalid();
      }
    }
    if (Result.Kind == ResolverResultKind::Cycle)
      MemoryMemo.erase(MemoIt);
    else
      MemoIt->second = Result;
    return Result;
  };

  resolveValue = [&](int Block, int Before, const NdVar &V,
                     unsigned Depth) -> ResolverResult {
    if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
      EvidenceBudgetExhausted = true;
      Complete = false;
      return resolverInvalid();
    }
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0 || V.Size == 0 || (!V.isReg() && !V.isTemp()))
      return resolverInvalid();
    constexpr size_t ValueKeyWork = 5;
    ValueKey Key{Block, Before, static_cast<uint8_t>(V.Space), V.Offset,
                 V.Size};
    if (!consumeMemoLookup(ValueKeyWork, ValueMemo.size()))
      return resolverInvalid();
    auto MemoIt = ValueMemo.find(Key);
    if (MemoIt != ValueMemo.end())
      return MemoIt->second ? *MemoIt->second : resolverCycle();
    if (!consumeMemoInsert(ValueKeyWork, ValueMemo.size()))
      return resolverInvalid();
    MemoIt = ValueMemo.try_emplace(Key, std::nullopt).first;

    const ResolverFlowBlock &B = Graph.Blocks[Block];
    const LaneView Query = viewOf(V);
    ResolverResult Result = resolverInvalid();
    bool Found = false;
    for (int I = std::min(Before, static_cast<int>(B.Ops.size())) - 1; I >= 0;
         --I) {
      if (!consumeEvidence()) {
        Found = true;
        break;
      }
      const LowOp &Def = B.Ops[I];
      const LaneView Output = viewOf(Def.Output);
      if ((Def.Opcode == NdOp::CALL || Def.Opcode == NdOp::INDIR_CALL) &&
          V.isReg()) {
        const bool CallDefinesQuery = [&] {
          if (!sameContainer(Output, Query))
            return false;
          const uint32_t OBegin = Output.Begin;
          const uint32_t OEnd = OBegin + Output.Size;
          const uint32_t QBegin = Query.Begin;
          const uint32_t QEnd = QBegin + Query.Size;
          const bool ZeroWrite =
              Def.Output.isReg() &&
              TRI.writeZeroExtends(Def.Output.Offset, Def.Output.Size);
          return (OBegin < QEnd && QBegin < OEnd) ||
                 (ZeroWrite && QBegin >= OEnd &&
                  QEnd <= Output.ContainerSize) ||
                 (ZeroWrite && OBegin == 0 && QBegin == 0 &&
                  QEnd == Output.ContainerSize);
        }();
        const bool QueryPreserved = [&] {
          if (!Query.Valid || Query.Begin > InvalidVA - Query.Container)
            return false;
          const uint64_t Begin = Query.Container + Query.Begin;
          if (Query.Size > InvalidVA - Begin)
            return false;
          const uint64_t End = Begin + Query.Size;
          if (!consumeEvidence(CallPreservedRanges.size()))
            return false;
          return std::any_of(CallPreservedRanges.begin(),
                             CallPreservedRanges.end(),
                             [&](const TargetRegisterRange &Range) {
                               if (Range.Bytes > InvalidVA - Range.Offset)
                                 return false;
                               return Begin >= Range.Offset &&
                                      End <= Range.Offset + Range.Bytes;
                             });
        }();
        // CALL LowIR only names explicit results.  Every other caller-saved
        // byte lane is an implicit definition and therefore a hard reaching-
        // value barrier.  This also keeps a predicated call fail closed: the
        // taken path may clobber the lane even though the untaken path does
        // not.
        if (!CallDefinesQuery && !QueryPreserved) {
          Found = true;
          Result = resolverInvalid();
          break;
        }
      }
      if (!sameContainer(Output, Query))
        continue;
      const uint32_t OBegin = Output.Begin;
      const uint32_t OEnd = OBegin + Output.Size;
      const uint32_t QBegin = Query.Begin;
      const uint32_t QEnd = QBegin + Query.Size;
      const bool ZeroWrite =
          Def.Output.isReg() &&
          TRI.writeZeroExtends(Def.Output.Offset, Def.Output.Size);
      const bool Overlaps = OBegin < QEnd && QBegin < OEnd;
      const bool DefinesZeroLane =
          ZeroWrite && QBegin >= OEnd && QEnd <= Output.ContainerSize;
      const bool DefinesFullWide = ZeroWrite && OBegin == 0 && QBegin == 0 &&
                                   QEnd == Output.ContainerSize;
      if (!Overlaps && !DefinesZeroLane && !DefinesFullWide)
        continue;
      Found = true;

      ResolverValue Full;
      bool PreserveExactGuestPointerLane = false;
      if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
           Def.Opcode == NdOp::INT_SEXT) &&
          Def.NumInputs >= 1) {
        ResolverResult Input = applyNumericOperandRole(
            Def, 0, resolveOperand(Block, I, Def.Inputs[0], Depth + 1));
        if (Input.Kind == ResolverResultKind::Value) {
          PreserveExactGuestPointerLane =
              Def.Opcode == NdOp::INT_ZEXT && CurrentImg &&
              Def.Inputs[0].Size == CurrentImg->getPointerSize() &&
              Def.Output.Size > Def.Inputs[0].Size &&
              Query.Begin == Output.Begin && Query.Size == Def.Inputs[0].Size;
          if (PreserveExactGuestPointerLane)
            // x86-32 computes an effective address modulo the complete
            // 32-bit guest-pointer domain and then widens it to the internal
            // VA container.  Reading that same low guest lane is the original
            // value occurrence, including its relocation owner.  Routing the
            // constant through generic extend+slice would incorrectly demote
            // an exact GOTOFF address to AddressFragment.
            Full = Input.Value;
          else if (Def.Opcode == NdOp::INT_ZEXT)
            Full = resolverExtend(Input.Value, Def.Output.Size, false,
                                  consumeEvidence,
                                  &EvidenceBudgetExhausted);
          else if (Def.Opcode == NdOp::INT_SEXT)
            Full = resolverExtend(Input.Value, Def.Output.Size, true,
                                  consumeEvidence,
                                  &EvidenceBudgetExhausted);
          else if (Def.Inputs[0].Size == Def.Output.Size)
            Full = Input.Value;
          else if (Def.Inputs[0].Size < Def.Output.Size)
            Full = resolverExtend(Input.Value, Def.Output.Size, false,
                                  consumeEvidence,
                                  &EvidenceBudgetExhausted);
          else
            Full = resolverSlice(Input.Value, 0, Def.Output.Size,
                                 consumeEvidence,
                                 &EvidenceBudgetExhausted);
        } else if (Input.Kind == ResolverResultKind::Cycle &&
                   Def.Opcode == NdOp::COPY && Def.NumInputs >= 1 &&
                   Def.Inputs[0].Space == Def.Output.Space &&
                   Def.Inputs[0].Offset == Def.Output.Offset &&
                   Def.Inputs[0].Size == Def.Output.Size) {
          Result = resolverCycle();
        }
      } else if (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
                 Def.Inputs[1].isConst()) {
        if (Def.Inputs[1].Offset <= std::numeric_limits<uint16_t>::max()) {
          const uint16_t SliceOffset =
              static_cast<uint16_t>(Def.Inputs[1].Offset);
          NdVar InputView = Def.Inputs[0];
          const bool IsGuestPointerLane =
              SliceOffset == 0 && CurrentImg->getPointerSize() != 0 &&
              Def.Output.Size == CurrentImg->getPointerSize() &&
              InputView.Size > Def.Output.Size;
          // Resolve only the bytes SUBBYTES actually consumes.  This is
          // essential on i386, where LowIR holds an effective address in an
          // eight-byte physical container whose synthetic upper lane has no
          // guest meaning.  Low-lane modular arithmetic is independently
          // evaluated below, so no unknown high lane can erase an exact
          // relocation occurrence.
          if (IsGuestPointerLane)
            InputView.Size = Def.Output.Size;
          ResolverResult Input =
              resolveOperand(Block, I, InputView, Depth + 1);
          if (Input.Kind == ResolverResultKind::Value)
            Full = IsGuestPointerLane
                       ? Input.Value
                       : resolverSlice(Input.Value, SliceOffset,
                                       Def.Output.Size, consumeEvidence,
                                       &EvidenceBudgetExhausted);
        }
      } else if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
                 Def.NumInputs >= 2 && Def.Output.Size <= sizeof(uint64_t)) {
        uint16_t EvalSize = Def.Output.Size;
        NdVar LeftInput = Def.Inputs[0];
        NdVar RightInput = Def.Inputs[1];
        if (Query.Begin == Output.Begin && Query.Size < Def.Output.Size &&
            Query.Size == CurrentImg->getPointerSize()) {
          EvalSize = Query.Size;
          if (LeftInput.Size > EvalSize)
            LeftInput.Size = EvalSize;
          if (RightInput.Size > EvalSize)
            RightInput.Size = EvalSize;
        }
        ResolverResult Left = applyNumericOperandRole(
            Def, 0, resolveOperand(Block, I, LeftInput, Depth + 1));
        ResolverResult Right = applyNumericOperandRole(
            Def, 1, resolveOperand(Block, I, RightInput, Depth + 1));
        if (Left.Kind == ResolverResultKind::Value &&
            Right.Kind == ResolverResultKind::Value && Left.Value &&
            Right.Value) {
          auto IsSameWidthScalarZero = [&](const ResolverValue &Value) {
            return Value && Value->Size == EvalSize &&
                   Value->K == ResolverValueExpr::Kind::Constant &&
                   Value->Constant == 0 &&
                   Value->Provenance == ConstantAddressProvenance::Scalar;
          };
          // Preserve the exact value occurrence across an authenticated
          // model-zero add/sub.  i386 PIC materializes the GOT base through a
          // GOTPC relocation; the resolver above proves that exact chain is
          // scalar zero in the ET_REL image.  Treating x+0 as an opaque
          // transform breaks both the table-address role and LOAD-to-branch
          // role, while accepting an arbitrary numeric/address fragment here
          // would be unsound after relinking.  Requiring the same arithmetic
          // width and Scalar provenance keeps this a pure machine-semantic
          // identity rather than a provenance upgrade.
          if (Def.Opcode == NdOp::INT_ADD &&
              IsSameWidthScalarZero(Left.Value) &&
              Right.Value->Size == EvalSize) {
            Full = Right.Value;
          } else if (IsSameWidthScalarZero(Right.Value) &&
                     Left.Value->Size == EvalSize) {
            Full = Left.Value;
          } else if (Left.Value->K == ResolverValueExpr::Kind::Constant &&
                     Right.Value->K == ResolverValueExpr::Kind::Constant) {
            const uint64_t Value =
                Def.Opcode == NdOp::INT_ADD
                    ? Left.Value->Constant + Right.Value->Constant
                    : Left.Value->Constant - Right.Value->Constant;
            ConstantAddressProvenance Provenance =
                ConstantAddressProvenance::Scalar;
            uint64_t Owner = InvalidVA;
            const bool LeftAddress =
                isExactAddressProvenance(Left.Value->Provenance);
            const bool RightAddress =
                isExactAddressProvenance(Right.Value->Provenance);
            const bool LeftFragment =
                Left.Value->Provenance ==
                ConstantAddressProvenance::AddressFragment;
            const bool RightFragment =
                Right.Value->Provenance ==
                ConstantAddressProvenance::AddressFragment;
            const bool LeftScalar =
                Left.Value->Provenance == ConstantAddressProvenance::Scalar;
            const bool RightScalar =
                Right.Value->Provenance == ConstantAddressProvenance::Scalar;
            auto ExactI386ModelZeroOccurrence = [&]()
                -> const RelocatedInstructionScalarModelOccurrence * {
              if (!CurrentImg || !CurrentImg->isELF() ||
                  CurrentImg->Arch != Arch::X86 ||
                  CurrentImg->getPointerSize() != 4)
                return nullptr;
              if (!consumeEvidence(orderedSetLookupWork(Insns.size())))
                return nullptr;
              const auto InsnIt = Insns.find(Def.Addr);
              if (InsnIt == Insns.end() || InsnIt->second.Size == 0 ||
                  InsnIt->second.Size > InvalidVA - Def.Addr)
                return nullptr;
              const va_t End = Def.Addr + InsnIt->second.Size;
              const RelocatedInstructionScalarModelOccurrence *Exact = nullptr;
              for (const RelocatedInstructionScalarModelOccurrence &Model :
                   RelocatedInstructionScalarModelOccurrences) {
                if (!consumeEvidence())
                  return nullptr;
                if (Model.Model !=
                        RelocatedInstructionScalarModelOccurrence::ModelKind::
                            I386ELFGOTBaseZero ||
                    Model.InstructionAddr != Def.Addr ||
                    Model.OpSeq != Def.Seq || Model.OutputOpcode != Def.Opcode ||
                    Model.OutputWitness != Def.Output ||
                    Model.Width != EvalSize ||
                    Model.OutputWitness.Size != EvalSize ||
                    Model.FieldVA < Def.Addr || Model.FieldVA >= End)
                  continue;
                if (!consumeEvidence(orderedSetLookupWork(
                        CurrentImg->I386GOTPCFields.size())))
                  return nullptr;
                if (!CurrentImg->I386GOTPCFields.count(Model.FieldVA))
                  continue;
                if (Exact)
                  return nullptr;
                Exact = &Model;
              }
              return Exact;
            };
            auto OwnerContains = [&](uint64_t Candidate,
                                     uint64_t Owner) -> bool {
              if (Owner == InvalidVA)
                return false;
              if (!consumeEvidenceSum(
                      {CurrentImg->Segments.size(),
                       CurrentImg->Sections.size()}))
                return false;
              if (const Section *Sec = CurrentImg->getSectionFor(Owner))
                return Sec->Size <= InvalidVA - Sec->VA &&
                       Candidate >= Sec->VA && Candidate < Sec->VA + Sec->Size;
              if (!consumeEvidence(CurrentImg->Segments.size()))
                return false;
              if (const Segment *Seg = CurrentImg->getSegmentFor(Owner))
                return Seg->Size <= InvalidVA - Seg->VA &&
                       Candidate >= Seg->VA && Candidate < Seg->VA + Seg->Size;
              return false;
            };
            auto ExactOwnerProvenance = [&](uint64_t Candidate) {
              if (!consumeExecutableOwnerQuery())
                return ConstantAddressProvenance::Address;
              if (CurrentImg->hasExecutableCodeOwnerAt(Candidate))
                return ConstantAddressProvenance::CodeAddress;
              if (!consumeObjectDataQuery())
                return ConstantAddressProvenance::Address;
              if (CurrentImg->hasObjectDataProvenance(Candidate))
                return ConstantAddressProvenance::DataAddress;
              return ConstantAddressProvenance::Address;
            };
            const RelocatedInstructionScalarModelOccurrence *I386Model =
                ExactI386ModelZeroOccurrence();
            auto ExactAddressOutputOccurrence = [&]()
                -> const RelocatedInstructionAddressOccurrence * {
              const RelocatedInstructionAddressOccurrence *Exact = nullptr;
              for (const RelocatedInstructionAddressOccurrence &Occurrence :
                   RelocatedInstructionAddressOccurrences) {
                if (!consumeEvidence())
                  return nullptr;
                if (!Occurrence.DefinesOutput || Occurrence.OutputMayDepend ||
                    Occurrence.InstructionAddr != Def.Addr ||
                    Occurrence.OpSeq != Def.Seq ||
                    Occurrence.OutputOpcode != Def.Opcode ||
                    Occurrence.OutputWitness != Def.Output ||
                    Occurrence.TargetVA !=
                        (Value & resolverWidthMask(EvalSize)) ||
                    Occurrence.TargetOwnerVA == InvalidVA ||
                    !isExactAddressProvenance(Occurrence.Provenance))
                  continue;
                if (!consumeRelocatedOwnerQuery())
                  return nullptr;
                if (!CurrentImg->relocatedTargetBelongsToOwner(
                        Occurrence.TargetVA, Occurrence.TargetOwnerVA))
                  continue;
                if (Exact)
                  return nullptr;
                Exact = &Occurrence;
              }
              return Exact;
            };
            const RelocatedInstructionAddressOccurrence *ExactAddressOutput =
                ExactAddressOutputOccurrence();
            const bool I386ModelZero =
                Def.Opcode == NdOp::INT_ADD && CurrentImg->Arch == Arch::X86 &&
                (Value & resolverWidthMask(EvalSize)) == 0 &&
                I386Model != nullptr;
            if (ExactAddressOutput) {
              Provenance = ExactAddressOutput->Provenance;
              Owner = ExactAddressOutput->TargetOwnerVA;
            } else if (I386ModelZero && ((LeftAddress && RightScalar) ||
                                  (RightAddress && LeftScalar))) {
              // ET_REL i386 models _GLOBAL_OFFSET_TABLE_ at zero.  The exact
              // GOTPC relocation occurrence proves this cancellation; a
              // numerically identical address+scalar expression without that
              // relocation must remain an address fragment after relinking.
              Provenance = ConstantAddressProvenance::Scalar;
              Owner = InvalidVA;
            } else if (Def.Opcode == NdOp::INT_ADD && LeftAddress &&
                       RightScalar) {
              Provenance = Left.Value->Provenance;
              Owner = Left.Value->AddressOwnerVA;
            } else if (Def.Opcode == NdOp::INT_ADD && RightAddress &&
                       LeftScalar) {
              Provenance = Right.Value->Provenance;
              Owner = Right.Value->AddressOwnerVA;
            } else if (Def.Opcode == NdOp::INT_SUB && LeftAddress &&
                       RightScalar) {
              Provenance = Left.Value->Provenance;
              Owner = Left.Value->AddressOwnerVA;
            } else if (Def.Opcode == NdOp::INT_ADD &&
                       ((LeftFragment && RightScalar) ||
                        (RightFragment && LeftScalar))) {
              // AArch64 ADRP materializes a page address fragment; the paired
              // scalar low-12 ADD at this exact CFG occurrence completes the
              // runtime address.  It has no object owner until the loader/table
              // role validates the resulting VA below.
              Provenance = ConstantAddressProvenance::Address;
            } else if (Def.Opcode == NdOp::INT_SUB && LeftFragment &&
                       RightScalar) {
              Provenance = ConstantAddressProvenance::Address;
            } else if (Def.Opcode == NdOp::INT_ADD && LeftAddress &&
                       RightFragment &&
                       OwnerContains(Value & resolverWidthMask(EvalSize),
                                     Right.Value->AddressOwnerVA)) {
              // ARM R_ARM_REL32 literal + the exact architectural PC completes
              // the relocation symbol's address.  The fragment owner, not the
              // coincident numeric value, authenticates the result.
              Provenance =
                  ExactOwnerProvenance(Value & resolverWidthMask(EvalSize));
              Owner = Right.Value->AddressOwnerVA;
            } else if (Def.Opcode == NdOp::INT_ADD && RightAddress &&
                       LeftFragment &&
                       OwnerContains(Value & resolverWidthMask(EvalSize),
                                     Left.Value->AddressOwnerVA)) {
              Provenance =
                  ExactOwnerProvenance(Value & resolverWidthMask(EvalSize));
              Owner = Left.Value->AddressOwnerVA;
            } else if (LeftAddress || RightAddress) {
              // Address+address, scalar-address, or an address combined with an
              // untyped immediate is not an exact address occurrence.
              Provenance = ConstantAddressProvenance::AddressFragment;
            }
            std::optional<ResolverScalarModelOrigin> ScalarModelOrigin;
            if (I386ModelZero && Provenance ==
                                     ConstantAddressProvenance::Scalar)
              ScalarModelOrigin = ResolverScalarModelOrigin{
                  I386Model->Model, I386Model->FieldVA,
                  I386Model->InstructionAddr, I386Model->OpSeq,
                  I386Model->Width};
            Full = budgetedResolverConstant(
                Value, EvalSize, Provenance, Owner, consumeEvidence,
                std::move(ScalarModelOrigin));
          }
        }
      } else if (Def.Opcode == NdOp::SELECT && Def.NumInputs >= 3) {
        ResolverResult TrueValue =
            resolveOperand(Block, I, Def.Inputs[1], Depth + 1);
        ResolverResult FalseValue =
            resolveOperand(Block, I, Def.Inputs[2], Depth + 1);
        if (TrueValue.Kind == ResolverResultKind::Value &&
            FalseValue.Kind == ResolverResultKind::Value) {
          ResolverRootKey MergeRoot;
          if (!makeResolverRootKey(
                  MergeRoot, "Q",
                  {Def.Addr, static_cast<uint64_t>(Def.Seq)}, consumeEvidence,
                  &EvidenceBudgetExhausted)) {
            Result = resolverInvalid();
            break;
          }
          if (!consumeEvidence(4)) {
            Result = resolverInvalid();
            break;
          }
          const std::array<ResolverResult, 2> MergeInputs = {TrueValue,
                                                             FalseValue};
          ResolverResult Merged = mergeResolverResults(
              MergeInputs, MergeRoot.view(),
              /*IgnoreTransparentCycles=*/false, consumeEvidence,
              &EvidenceBudgetExhausted);
          if (Merged.Kind == ResolverResultKind::Value)
            Full = Merged.Value;
        }
      } else if (Def.Opcode == NdOp::LOAD && Def.NumInputs >= 1) {
        const NdVar &Address =
            Def.NumInputs >= 2 ? Def.Inputs[1] : Def.Inputs[0];
        ResolverResult AddressValue =
            resolveOperand(Block, I, Address, Depth + 1);
        if (AddressValue.Kind == ResolverResultKind::Value &&
            AddressValue.Value &&
            AddressValue.Value->K == ResolverValueExpr::Kind::Constant &&
            isExactAddressProvenance(AddressValue.Value->Provenance))
          Full = relocatedLiteralValue(AddressValue.Value->Constant,
                                       Def.Output.Size);
        uint64_t SlotBase = InvalidVA;
        int64_t SlotOffset = 0;
        const bool HasFrameSlot =
            !Full &&
            canonicalFrameSlotKey(Block, I - 1, Address, SlotBase, SlotOffset);
        if (HasFrameSlot) {
          ResolverResult Loaded =
              resolveMemory(Block, I, SlotBase, SlotOffset, Def.Output.Size,
                            Depth + 1);
          if (Loaded.Kind == ResolverResultKind::Value)
            Full = Loaded.Value;
          else
            // Even when the frame-memory origin is deliberately opaque (an
            // atomic/call/unknown-alias barrier), this particular LOAD still
            // defines one stable SSA-like occurrence.  Two later uses of that
            // same occurrence may be compared; a distinct reload gets a
            // distinct root and therefore cannot borrow stale guard evidence.
            Full = namedResolverRoot(
                Def.Output.Size, "D",
                {Def.Addr, static_cast<uint64_t>(Def.Seq),
                 static_cast<unsigned>(Def.Output.Space), Def.Output.Offset,
                 Def.Output.Size});
        } else if (!Full) {
          Full = namedResolverRoot(
              Def.Output.Size, "D",
              {Def.Addr, static_cast<uint64_t>(Def.Seq),
               static_cast<unsigned>(Def.Output.Space), Def.Output.Offset,
               Def.Output.Size});
        }
      }

      if (!Full && Def.NumInputs > 0) {
        // Preserve occurrence-level dependency even when this proof session
        // does not model the operation's value semantics.  Must-equality still
        // requires the exact same transform occurrence and input graph, while
        // MayDepend can see through an unmodelled ADD/SUB/shift/logic chain
        // instead of losing the edge behind an opaque output root.  Unknown
        // inputs receive occurrence-local roots, so this can conservatively
        // reject an incomplete domain but cannot manufacture a constant or
        // equate unrelated operations.
        std::vector<ResolverValue> Dependencies;
        if (consumeEvidenceProduct(Def.NumInputs, 2) && consumeEvidence(2)) {
          Dependencies.reserve(Def.NumInputs);
          for (int InputNo = 0; InputNo < Def.NumInputs; ++InputNo) {
            if (!consumeEvidence(2)) {
              Dependencies.clear();
              break;
            }
            ResolverResult Input = applyNumericOperandRole(
                Def, InputNo,
                resolveOperand(Block, I, Def.Inputs[InputNo], Depth + 1));
            if (Input.Kind == ResolverResultKind::Value && Input.Value) {
              Dependencies.push_back(Input.Value);
            } else {
              const uint16_t InputSize = Def.Inputs[InputNo].Size != 0
                                             ? Def.Inputs[InputNo].Size
                                             : Def.Output.Size;
              ResolverValue Unknown = namedResolverRoot(
                  InputSize, "U",
                  {Def.Addr, static_cast<uint64_t>(Def.Seq),
                   static_cast<uint64_t>(InputNo)});
              if (!Unknown) {
                Dependencies.clear();
                break;
              }
              Dependencies.push_back(std::move(Unknown));
            }
          }
        }
        if (EvidenceBudgetExhausted) {
          Result = resolverInvalid();
          break;
        }
        if (!Dependencies.empty())
          Full = namedResolverTransform(
              Def.Output.Size, "T",
              {static_cast<unsigned>(Def.Opcode), Def.Addr,
               static_cast<uint64_t>(Def.Seq)},
              std::move(Dependencies), Def.Opcode);
      }
      if (!Full)
        Full = namedResolverRoot(
            Def.Output.Size, "D",
            {Def.Addr, static_cast<uint64_t>(Def.Seq),
             static_cast<unsigned>(Def.Output.Space), Def.Output.Offset,
             Def.Output.Size});

      if (Full) {
        Result = PreserveExactGuestPointerLane
                     ? resolverValue(Full)
                     : projectDefinition(Full, Output, Query, ZeroWrite);
        if (Result.Kind != ResolverResultKind::Value && Overlaps &&
            !(OBegin <= QBegin && QEnd <= OEnd)) {
          // A partial-register definition creates a composite value: the
          // overlapping lane comes from this definition while every untouched
          // byte still comes from the value immediately before it.  Naming the
          // whole result as one opaque root loses the new lane's dependency;
          // e.g. `andb $1,%r10b` would make a later full-R10 table index look
          // unrelated to the mask and permit a relocation-run fallback.
          //
          // Keep the exact definition occurrence as a Transform over both
          // sources.  Must-equality consumers can still recognize two uses of
          // this same composite state, while MayDepend walks into the narrow
          // definition and therefore fails closed when the untouched wide
          // lane leaves the table domain incomplete.
          ResolverResult Old = resolveValue(Block, I, V, Depth + 1);
          std::vector<ResolverValue> Dependencies;
          if (!consumeEvidenceProduct(2, 2) || !consumeEvidence(2)) {
            Result = resolverInvalid();
            break;
          }
          Dependencies.reserve(2);
          if (Old.Kind == ResolverResultKind::Value && Old.Value) {
            if (!consumeEvidence()) {
              Result = resolverInvalid();
              break;
            }
            Dependencies.push_back(Old.Value);
          } else {
            ResolverValue OldRoot = namedResolverRoot(
                Query.Size, "POLD",
                {Def.Addr, static_cast<uint64_t>(Def.Seq),
                 static_cast<unsigned>(Query.Space), Query.Container,
                 Query.Begin, Query.Size});
            if (!OldRoot || !consumeEvidence()) {
              Result = resolverInvalid();
              break;
            }
            Dependencies.push_back(std::move(OldRoot));
          }
          if (!consumeEvidence()) {
            Result = resolverInvalid();
            break;
          }
          Dependencies.push_back(Full);
          Result = resolverValue(namedResolverTransform(
              Query.Size, "PT",
              {Def.Addr, static_cast<uint64_t>(Def.Seq), Output.Begin,
               Output.Size},
              std::move(Dependencies)));
        }
      } else if (Result.Kind != ResolverResultKind::Cycle) {
        // A partial-register write does not expose the untouched bytes as one
        // reconstructible expression, but it does create a new, stable lane
        // state.  Name that state by the exact definition occurrence so uses
        // after the write agree with each other while no use before it can.
        Result = resolverValue(namedResolverRoot(
            Query.Size, "S",
            {Def.Addr, static_cast<uint64_t>(Def.Seq),
             static_cast<unsigned>(Query.Space), Query.Container, Query.Begin,
             Query.Size}));
      }
      const std::optional<bool> Guarded = instructionIsGuarded(Def.Addr);
      if (!Guarded) {
        Result = resolverInvalid();
        break;
      }
      if (*Guarded) {
        ResolverResult Old = resolveValue(Block, I, V, Depth + 1);
        // Preserve both feasible values of a condition-executed definition.
        // Must-equality consumers below still require every arm to match, but
        // incomplete-domain checks can now ask whether *any* arm depends on a
        // predicated mask/offset producer.  Collapsing unequal arms to Invalid
        // erased exactly the unsafe path that the latter query must detect.
        ResolverRootKey MergeRoot;
        if (!makeResolverRootKey(
                MergeRoot, "G",
                {Def.Addr, static_cast<uint64_t>(Def.Seq)}, consumeEvidence,
                &EvidenceBudgetExhausted) ||
            !consumeEvidence(4)) {
          Result = resolverInvalid();
          break;
        }
        const std::array<ResolverResult, 2> MergeInputs = {Old, Result};
        Result = mergeResolverResults(
            MergeInputs, MergeRoot.view(),
            /*IgnoreTransparentCycles=*/false, consumeEvidence,
            &EvidenceBudgetExhausted);
      }
      break;
    }

    if (!Found) {
      std::vector<ResolverResult> Incoming;
      if (B.Preds.size() == std::numeric_limits<size_t>::max() ||
          !consumeEvidenceProduct(B.Preds.size() + 1, 2) ||
          !consumeEvidence(2)) {
        ValueMemo.erase(MemoIt);
        return resolverInvalid();
      }
      Incoming.reserve(B.Preds.size() + 1);
      // Each disconnected CFG root has a distinct incoming register state.
      // The canonical function entry also retains its initial state when a
      // loop back-edge targets it.
      if (V.isReg() && IsRootBlock[Block]) {
        ResolverValue EntryRoot = namedResolverRoot(
            V.Size, "L", {B.Start, V.Offset, V.Size});
        if (!EntryRoot || !consumeEvidence()) {
          Incoming.clear();
        } else {
          Incoming.push_back(resolverValue(std::move(EntryRoot)));
        }
      }
      for (int Pred : B.Preds) {
        if (!consumeEvidence(2)) {
          Incoming.clear();
          break;
        }
        Incoming.push_back(resolveValue(
            Pred, static_cast<int>(Graph.Blocks[Pred].Ops.size()), V,
            Depth + 1));
      }
      if (Incoming.empty()) {
        Result = resolverInvalid();
      } else {
        ResolverRootKey MergeRoot;
        Result = makeResolverRootKey(
                     MergeRoot, "PB",
                     {B.Start, static_cast<unsigned>(Query.Space),
                      Query.Container, Query.Begin},
                     consumeEvidence, &EvidenceBudgetExhausted)
                     ? mergeResolverResults(
                           Incoming, MergeRoot.view(),
                           /*IgnoreTransparentCycles=*/true, consumeEvidence,
                           &EvidenceBudgetExhausted)
                     : resolverInvalid();
      }
    }

    if (Result.Kind == ResolverResultKind::Cycle)
      ValueMemo.erase(MemoIt);
    else
      MemoIt->second = Result;
    return Result;
  };

  // All queries below share the same CFG snapshot and reaching-value/memory
  // memo tables.  This avoids rebuilding the whole proof graph for every
  // candidate/anchor pair in a large dispatch DAG.  One global work budget is
  // deliberately shared across the batch; exhaustion fails remaining queries
  // closed rather than turning attacker-controlled graph size into unbounded
  // analysis work.
  const size_t DefaultMatchEvidenceLimit =
      ActiveJumpTableConsumerAudit
          ? limits::kMaxJumpTableConsumerAuditMatchEvidenceWork
          : limits::kMaxJumpTableValueMatchEvidenceWork;
  size_t MatchBudget =
      GraphWorkBudget
          ? (LocalMatchEvidenceLimit != 0 ? LocalMatchEvidenceLimit
                                          : DefaultMatchEvidenceLimit)
          : limits::kMaxJumpTableEntries;
  bool MatchBudgetExhausted = false;
  bool SymbolBudgetExhausted = false;
  auto consumeMatchWork = [&](size_t Amount = 1) {
    if (Amount > MatchBudget) {
      MatchBudget = 0;
      MatchBudgetExhausted = true;
      Complete = false;
      return false;
    }
    if (!consumeEvidence(Amount)) {
      MatchBudget = 0;
      MatchBudgetExhausted = true;
      return false;
    }
    MatchBudget -= Amount;
    return true;
  };
  std::function<bool(const ResolverValue &, const ResolverValue &, unsigned)>
      sameResolverValueBudgeted = [&](const ResolverValue &A,
                                      const ResolverValue &B,
                                      unsigned Depth) {
        if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
          MatchBudgetExhausted = true;
          Complete = false;
          return false;
        }
        if (!consumeMatchWork())
          return false;
        if (A == B)
          return true;
        if (!A || !B || A->K != B->K || A->Size != B->Size ||
            A->SliceOffset != B->SliceOffset || A->Constant != B->Constant ||
            A->Provenance != B->Provenance ||
            A->AddressOwnerVA != B->AddressOwnerVA ||
            A->Root.size() != B->Root.size())
          return false;
        if (!budgetedResolverRootsEqual(A->Root, B->Root,
                                       consumeMatchWork) ||
            A->Opcode != B->Opcode || A->HasOpcode != B->HasOpcode ||
            A->ScalarModelOrigin != B->ScalarModelOrigin ||
            A->Inputs.size() != B->Inputs.size())
          return false;
        if (!sameResolverValueBudgeted(A->Input, B->Input, Depth + 1))
          return false;
        for (size_t I = 0; I < A->Inputs.size(); ++I)
          if (!sameResolverValueBudgeted(A->Inputs[I], B->Inputs[I],
                                         Depth + 1))
            return false;
        return true;
      };
  auto sameAllowedValue = [&](const ResolverValue &Value,
                              const ResolverValue &Allowed,
                              bool RequireExactAddressOwner) {
    if (sameResolverValueBudgeted(Value, Allowed, /*Depth=*/0))
      return true;
    if (MatchBudgetExhausted)
      return false;
    if (!Value || !Allowed || Value->K != ResolverValueExpr::Kind::Constant ||
        Allowed->K != ResolverValueExpr::Kind::Constant ||
        Value->Size != Allowed->Size || Value->Constant != Allowed->Constant ||
        !isExactAddressProvenance(Value->Provenance) ||
        !isExactAddressProvenance(Allowed->Provenance))
      return false;
    // A generic exact-address anchor intentionally has no object owner.  When
    // both sides do carry loader-authenticated owners they must agree.
    if (RequireExactAddressOwner)
      return Value->AddressOwnerVA == Allowed->AddressOwnerVA;
    return Value->AddressOwnerVA == InvalidVA ||
           Allowed->AddressOwnerVA == InvalidVA ||
           Value->AddressOwnerVA == Allowed->AddressOwnerVA;
  };

  // Exact modulo-recipe queries are attacker-shaped proposals collected from
  // one function.  Share one symbolization budget across their entire batch;
  // giving every proposal a fresh full allowance would restore a
  // proposal-count-times-expression-size work multiplier even without SAT.
  size_t ExactModuloRecipeWork =
      limits::kMaxJumpTableModuloRecipeSymbolEvidenceWork;
  auto provesUnsignedUpperBound = [&](const ResolverValue &Value,
                                      uint64_t Bound, bool &ProofComplete,
                                      bool ExactModuloRecipeOnly = false) {
    ProofComplete = false;
    if (!Value || Value->Size == 0 || Value->Size > sizeof(uint64_t) ||
        Bound == 0)
      return false;

    const uint32_t Width = uint32_t(Value->Size) * 8u;
    if (!ExactModuloRecipeOnly && Width < 64 &&
        Bound >= (uint64_t{1} << Width)) {
      ProofComplete = true;
      return true;
    }

    symbolic::SymContext Ctx;
    size_t LocalWork = limits::kMaxJumpTableEvidenceWork;
    size_t &Work =
        ExactModuloRecipeOnly ? ExactModuloRecipeWork : LocalWork;
    std::map<const ResolverValueExpr *, symbolic::SymRef> Memo;
    std::map<std::pair<std::string, uint32_t>, symbolic::SymRef> Variables;
    std::map<std::pair<std::string, size_t>, symbolic::SymRef> MergeSelectors;
    size_t MaxVariableKeyWork = 0;
    size_t MaxMergeSelectorKeyWork = 0;
    bool Exhausted = false;
    auto consumeSymbolWork = [&](size_t Amount = 1) {
      if (Amount > Work) {
        Work = 0;
        Exhausted = true;
        SymbolBudgetExhausted = true;
        return false;
      }
      if (!consumeEvidence(Amount)) {
        Work = 0;
        Exhausted = true;
        SymbolBudgetExhausted = true;
        return false;
      }
      Work -= Amount;
      return true;
    };
    auto consumeSymbolProduct = [&](size_t Count, size_t Cost) {
      if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count)
        return consumeSymbolWork(std::numeric_limits<size_t>::max());
      return consumeSymbolWork(Count * Cost);
    };
    auto consumeSymbolSum =
        [&](std::initializer_list<size_t> Terms) -> bool {
      size_t Total = 0;
      for (size_t Term : Terms) {
        if (Term > std::numeric_limits<size_t>::max() - Total)
          return consumeSymbolWork(std::numeric_limits<size_t>::max());
        Total += Term;
      }
      return consumeSymbolWork(Total);
    };
    auto stringKeyWork = [&](const std::string &Name)
        -> std::optional<size_t> {
      if (Name.size() > std::numeric_limits<size_t>::max() - 2) {
        consumeSymbolWork(std::numeric_limits<size_t>::max());
        return std::nullopt;
      }
      return Name.size() + 2;
    };
    auto consumeDynamicMapLookup = [&](size_t KeyWork, size_t MaxKeyWork,
                                       size_t Count) {
      return consumeSymbolProduct(std::max(KeyWork, MaxKeyWork),
                                  orderedSetLookupWork(Count));
    };
    auto consumeDynamicMapInsert = [&](size_t KeyWork, size_t MaxKeyWork,
                                       size_t Count) {
      return consumeDynamicMapLookup(KeyWork, MaxKeyWork, Count) &&
             consumeSymbolProduct(KeyWork, 2) &&
             consumeSymbolSum({size_t{3}});
    };
    auto unknownNamed = [&](const std::string &Name,
                            uint32_t Width) -> symbolic::SymRef {
      const std::optional<size_t> KeyWork = stringKeyWork(Name);
      if (!KeyWork || !consumeSymbolProduct(*KeyWork, 2))
        return {};
      auto Key = std::make_pair(Name, Width);
      if (!consumeDynamicMapLookup(*KeyWork, MaxVariableKeyWork,
                                   Variables.size()))
        return {};
      auto It = Variables.find(Key);
      if (It == Variables.end()) {
        // The context node is distinct from the ordered map's repeated
        // lookup, retained dynamic key/mapped value, node, and cleanup.
        if (!consumeSymbolWork() ||
            !consumeDynamicMapInsert(*KeyWork, MaxVariableKeyWork,
                                     Variables.size()))
          return {};
        const uint32_t KeyWidth = Key.second;
        It = Variables
                 .emplace(std::move(Key),
                          Ctx.mkFreshVar(KeyWidth, "jt_value"))
                 .first;
        MaxVariableKeyWork = std::max(MaxVariableKeyWork, *KeyWork);
      }
      return It->second;
    };
    auto unknown = [&](const ResolverValue &Node) -> symbolic::SymRef {
      if (!Node->Root.empty())
        return unknownNamed(Node->Root, uint32_t(Node->Size) * 8u);
      constexpr size_t OpaqueNameWork = 32;
      // Pay both the local dynamic string construction and its eventual
      // cleanup before the first allocation.  unknownNamed independently
      // owns the temporary/retained map-key lifetimes.
      if (!consumeSymbolProduct(OpaqueNameWork, 2))
        return {};
      const std::string Name =
          "opaque:" + std::to_string(reinterpret_cast<uintptr_t>(Node.get()));
      return unknownNamed(Name, uint32_t(Node->Size) * 8u);
    };

    std::function<symbolic::SymRef(const ResolverValue &, unsigned)> Symbolize =
        [&](const ResolverValue &Node, unsigned Depth) -> symbolic::SymRef {
      if (!Node || Node->Size == 0 || Node->Size > sizeof(uint64_t) ||
          Depth > limits::kMaxJumpTableGuardExpressionDepth) {
        Exhausted = true;
        SymbolBudgetExhausted = true;
        return {};
      }
      if (!consumeSymbolWork())
        return {};
      if (!consumeSymbolWork(orderedSetLookupWork(Memo.size())))
        return {};
      if (auto It = Memo.find(Node.get()); It != Memo.end())
        return It->second;
      const uint32_t NodeWidth = uint32_t(Node->Size) * 8u;
      symbolic::SymRef Result;
      switch (Node->K) {
      case ResolverValueExpr::Kind::Root:
        Result = unknown(Node);
        break;
      case ResolverValueExpr::Kind::Constant:
        if (!consumeSymbolWork())
          break;
        // Exact modulo structure is an integer-domain certificate.  A
        // loader-authenticated address whose numeric bits happen to equal a
        // reciprocal, divisor, or shift literal is not scalar arithmetic.
        if (!ExactModuloRecipeOnly ||
            Node->Provenance == ConstantAddressProvenance::Scalar)
          Result = Ctx.mkConst(NodeWidth, Node->Constant);
        break;
      case ResolverValueExpr::Kind::Zero:
        if (!consumeSymbolWork())
          break;
        Result = Ctx.mkZero(NodeWidth);
        break;
      case ResolverValueExpr::Kind::ZeroExtend:
      case ResolverValueExpr::Kind::SignExtend: {
        symbolic::SymRef Input = Symbolize(Node->Input, Depth + 1);
        if (!Input || Ctx.width(Input) > NodeWidth)
          break;
        if (!consumeSymbolWork())
          break;
        Result = Node->K == ResolverValueExpr::Kind::ZeroExtend
                     ? Ctx.mkZExt(Input, NodeWidth)
                     : Ctx.mkSExt(Input, NodeWidth);
        break;
      }
      case ResolverValueExpr::Kind::Slice: {
        symbolic::SymRef Input = Symbolize(Node->Input, Depth + 1);
        const uint64_t Low = uint64_t(Node->SliceOffset) * 8u;
        if (!Input || Low > Ctx.width(Input) ||
            NodeWidth > Ctx.width(Input) - Low)
          break;
        if (!consumeSymbolWork())
          break;
        Result = Ctx.mkExtract(Input, static_cast<uint32_t>(Low), NodeWidth);
        break;
      }
      case ResolverValueExpr::Kind::Merge: {
        if (Node->Inputs.empty())
          break;
        // Inspect a uniform extension before collapsing a named merge to an
        // arbitrary block-entry value.  The resolver can represent the same
        // predecessor lane once at its machine width and once after every arm
        // applies the same zext/sext.  Treating those named merges as unrelated
        // SymVar32/SymVar64 roots destroys the dividend identity of an exact
        // modulo recipe.  Hoisting preserves the real relation
        //   merge(ext(x_i)) == ext(merge(x_i))
        // without inspecting or constraining any predecessor value.
        if (!consumeSymbolWork(Node->Inputs.size()) ||
            !Node->Inputs.front())
          break;
        const ResolverValueExpr::Kind FirstKind = Node->Inputs.front()->K;
        const bool HoistExtension =
            (FirstKind == ResolverValueExpr::Kind::ZeroExtend ||
             FirstKind == ResolverValueExpr::Kind::SignExtend) &&
            Node->Inputs.front()->Input &&
            std::all_of(Node->Inputs.begin(), Node->Inputs.end(),
                        [&](const ResolverValue &Arm) {
                          return Arm && Arm->K == FirstKind && Arm->Input &&
                                 Arm->Size == Node->Size &&
                                 Arm->Input->Size ==
                                     Node->Inputs.front()->Input->Size;
                        });
        std::optional<ResolverValueExpr::Kind> EnvelopeKind;
        uint16_t EnvelopeInputSize = 0;
        std::function<bool(const ResolverValue &, unsigned)>
            CollectUniformExtensionEnvelope =
                [&](const ResolverValue &Arm, unsigned EnvelopeDepth) {
                  if (!Arm || Arm->Size != Node->Size ||
                      EnvelopeDepth >
                          limits::kMaxJumpTableGuardExpressionDepth ||
                      !consumeSymbolWork())
                    return false;
                  if ((Arm->K == ResolverValueExpr::Kind::ZeroExtend ||
                       Arm->K == ResolverValueExpr::Kind::SignExtend) &&
                      Arm->Input && Arm->Input->Size < Arm->Size) {
                    if (!EnvelopeKind) {
                      EnvelopeKind = Arm->K;
                      EnvelopeInputSize = Arm->Input->Size;
                      return true;
                    }
                    return *EnvelopeKind == Arm->K &&
                           EnvelopeInputSize == Arm->Input->Size;
                  }
                  if (Arm->K != ResolverValueExpr::Kind::Merge ||
                      Arm->Inputs.empty() ||
                      !consumeSymbolWork(Arm->Inputs.size()))
                    return false;
                  return std::all_of(
                      Arm->Inputs.begin(), Arm->Inputs.end(),
                      [&](const ResolverValue &Nested) {
                        return CollectUniformExtensionEnvelope(
                            Nested, EnvelopeDepth + 1);
                      });
                };
        const bool ExactHoistExtension =
            ExactModuloRecipeOnly && !Node->Root.empty() &&
            std::all_of(Node->Inputs.begin(), Node->Inputs.end(),
                        [&](const ResolverValue &Arm) {
                          return CollectUniformExtensionEnvelope(Arm, 0);
                        }) &&
            EnvelopeKind.has_value();
        // An exact modulo-recipe theorem needs the algebra surrounding a CFG
        // value, not an expansion of every loop iteration that can feed it.
        // A named merge root identifies one block-entry lane in this immutable
        // graph snapshot.  Model that whole incoming value as one arbitrary
        // bit-vector: the quotient and back-subtract must still consume the
        // same root, while a sibling merge, local rewrite, or differently
        // named lane remains independent.  This also keeps final fixed-point
        // replay invariant under the addition of authenticated backedges.
        if (ExactModuloRecipeOnly && !Node->Root.empty()) {
          if (!ExactHoistExtension) {
            Result = unknown(Node);
            break;
          }
          symbolic::SymRef Input = unknownNamed(
              Node->Root, uint32_t(EnvelopeInputSize) * 8u);
          if (!Input || !consumeSymbolWork())
            break;
          Result = *EnvelopeKind == ResolverValueExpr::Kind::ZeroExtend
                       ? Ctx.mkZExt(Input, NodeWidth)
                       : Ctx.mkSExt(Input, NodeWidth);
          break;
        }
        auto SymbolizeArm = [&](const ResolverValue &Arm) {
          return Symbolize(HoistExtension ? Arm->Input : Arm, Depth + 1);
        };
        Result = SymbolizeArm(Node->Inputs.back());
        for (size_t I = Node->Inputs.size() - 1; Result && I > 0; --I) {
          if (!consumeSymbolWork()) {
            Result = {};
            break;
          }
          symbolic::SymRef Arm = SymbolizeArm(Node->Inputs[I - 1]);
          if (!Arm || Ctx.width(Arm) != Ctx.width(Result)) {
            Result = {};
            break;
          }
          // Repeated queries at different use points can reconstruct distinct
          // ResolverValueExpr objects for the same CFG predecessor join.  The
          // merge root names that join and lane; use one selector per arm so
          // the quotient and remainder retain their real path correlation.
          // Freshening every reconstruction treats one predecessor choice as
          // independent choices and makes a valid same-dividend modulo recipe
          // look unrelated.  An empty root is defensive-only and remains
          // pointer-local rather than correlating unrelated anonymous merges.
          std::string MergeRoot;
          if (!Node->Root.empty()) {
            const std::optional<size_t> RootWork =
                stringKeyWork(Node->Root);
            if (!RootWork || !consumeSymbolProduct(*RootWork, 2)) {
              Result = {};
              break;
            }
            MergeRoot = Node->Root;
          } else {
            constexpr size_t AnonymousMergeNameWork = 32;
            if (!consumeSymbolProduct(AnonymousMergeNameWork, 2)) {
              Result = {};
              break;
            }
            MergeRoot = "anon:" +
                        std::to_string(reinterpret_cast<uintptr_t>(Node.get()));
          }
          const std::optional<size_t> KeyWork = stringKeyWork(MergeRoot);
          if (!KeyWork || !consumeSymbolProduct(*KeyWork, 2)) {
            Result = {};
            break;
          }
          auto Key = std::make_pair(std::move(MergeRoot), I - 1);
          if (!consumeDynamicMapLookup(*KeyWork, MaxMergeSelectorKeyWork,
                                       MergeSelectors.size())) {
            Result = {};
            break;
          }
          auto It = MergeSelectors.find(Key);
          if (It == MergeSelectors.end()) {
            // Prepay the context variable and the complete dynamic-key map
            // insertion before allocating either retained object.
            if (!consumeSymbolWork() ||
                !consumeDynamicMapInsert(*KeyWork,
                                         MaxMergeSelectorKeyWork,
                                         MergeSelectors.size())) {
              Result = {};
              break;
            }
            It = MergeSelectors
                     .emplace(std::move(Key),
                              Ctx.mkFreshVar(1, "jt_merge"))
                     .first;
            MaxMergeSelectorKeyWork =
                std::max(MaxMergeSelectorKeyWork, *KeyWork);
          }
          if (!consumeSymbolWork()) {
            Result = {};
            break;
          }
          Result = Ctx.mkIte(It->second, Arm, Result);
        }
        // Extension distributes over a predecessor-select exactly.  The
        // resolver represents it arm-wise so each arm retains provenance;
        // canonicalize it back to zext/sext(merge) for bit-vector reasoning,
        // but only when every arm performs the same extension from the same
        // width.  Mixed or partially extended merges remain untouched.
        if (Result && HoistExtension) {
          if (!consumeSymbolWork()) {
            Result = {};
            break;
          }
          Result = FirstKind == ResolverValueExpr::Kind::ZeroExtend
                       ? Ctx.mkZExt(Result, NodeWidth)
                       : Ctx.mkSExt(Result, NodeWidth);
        }
        break;
      }
      case ResolverValueExpr::Kind::Transform: {
        if (!Node->HasOpcode) {
          // An unmodelled partial-lane or memory transform may produce any
          // value of its output width.  A fresh bit-vector is conservative:
          // it can only prevent a finite-domain proof, never manufacture one.
          Result = unknown(Node);
          break;
        }
        std::vector<symbolic::SymRef> Inputs;
        if (!consumeSymbolProduct(Node->Inputs.size(), 2) ||
            !consumeSymbolWork(2))
          break;
        Inputs.reserve(Node->Inputs.size());
        for (const ResolverValue &Input : Node->Inputs) {
          if (!consumeSymbolWork()) {
            Inputs.clear();
            break;
          }
          symbolic::SymRef Symbolic = Symbolize(Input, Depth + 1);
          if (!Symbolic) {
            Inputs.clear();
            break;
          }
          if (!consumeSymbolWork()) {
            Inputs.clear();
            break;
          }
          Inputs.push_back(Symbolic);
        }
        if (!Inputs.empty()) {
          const size_t InputCount = Node->Inputs.size();
          if (InputCount >
                  (std::numeric_limits<size_t>::max() - 10) / 5 ||
              !consumeSymbolWork(InputCount * 5 + 10))
            break;
          std::optional<symbolic::SymRef> Operation =
              symbolizeJumpTableIntegerOperation(Ctx, Node->Opcode, Node->Size,
                                                 Inputs);
          if (Operation)
            Result = *Operation;
        }
        if (!Result)
          Result = unknown(Node);
        break;
      }
      }
      if (Result) {
        const size_t Lookup = orderedSetLookupWork(Memo.size());
        if (Lookup > std::numeric_limits<size_t>::max() - 5 ||
            !consumeSymbolWork(Lookup + 5))
          return {};
        Memo.try_emplace(Node.get(), Result);
      }
      return Result;
    };

    symbolic::SymRef Index = Symbolize(Value, 0);
    if (!Index || Exhausted || Ctx.width(Index) != Width) {
      if (ExactModuloRecipeOnly && !Exhausted)
        ProofComplete = true;
      return false;
    }
    auto makeSolverOptions = [] {
      solver::SolverOptions Options;
      Options.BuildModel = false;
      Options.Sat.MaxConflicts =
          limits::kMaxJumpTableGuardSolverConflicts;
      Options.Sat.MaxPropagations =
          limits::kMaxJumpTableGuardSolverPropagations;
      Options.Sat.MaxWatchVisits =
          limits::kMaxJumpTableGuardSolverWatchVisits;
      Options.Blast.MaxWidth = 64;
      Options.Blast.MaxGates = limits::kMaxJumpTableGuardSolverGates;
      return Options;
    };
    if (provesExactUnsignedModuloRecipe(Ctx, Index, Bound,
                                        consumeSymbolWork, &Exhausted)) {
      ProofComplete = true;
      return true;
    }
    SymbolBudgetExhausted |= Exhausted;
    if (ExactModuloRecipeOnly) {
      // A fully symbolized expression that does not match an exact unsigned
      // modulo recipe is a completed negative structural result.  In
      // particular, do not ask SAT whether the unrelated expression merely
      // happens to fit below the proposed table capacity.
      ProofComplete = !Exhausted;
      return false;
    }
    if (!consumeSymbolWork(3))
      return false;
    symbolic::SymRef Counterexample =
        Ctx.mkNot(Ctx.mkUlt(Index, Ctx.mkConst(Width, Bound)));
    const solver::SatResult Result =
        solver::checkSat(Ctx, Counterexample, nullptr, makeSolverOptions());
    ProofComplete = Result != solver::SatResult::Unknown;
    return Result == solver::SatResult::Unsat;
  };

  for (size_t QueryIndex = 0; QueryIndex < Queries.size(); ++QueryIndex) {
    if (!consumeEvidence()) {
      markIncomplete(QueryIndex);
      continue;
    }
    const JumpTableValueQuery &Query = Queries[QueryIndex];
    if (Query.Candidate.Size == 0 ||
        (Query.Relation != JumpTableValueRelation::UnsignedLessThan &&
         Query.Relation !=
             JumpTableValueRelation::ExactUnsignedModuloRecipe &&
         Query.Relation !=
             JumpTableValueRelation::AuthenticatedFrameMemory &&
         Query.Alternatives.empty()))
      continue;

    if (Query.Relation ==
        JumpTableValueRelation::SameCanonicalFrameAddress) {
      if (Query.Alternatives.size() != 1 ||
          Query.Alternatives.front().DefinedAtPoint)
        continue;
      if (!consumeProofPointLookup() || !consumeProofPointLookup()) {
        markIncomplete(QueryIndex);
        continue;
      }
      auto CandidatePoint = Graph.PointToOp.find({Query.UseAddr, Query.UseSeq});
      auto AlternativePoint = Graph.PointToOp.find(
          {Query.Alternatives.front().Addr, Query.Alternatives.front().Seq});
      if (CandidatePoint == Graph.PointToOp.end() ||
          AlternativePoint == Graph.PointToOp.end())
        continue;
      uint64_t CandidateBase = InvalidVA;
      uint64_t AlternativeBase = InvalidVA;
      int64_t CandidateOffset = 0;
      int64_t AlternativeOffset = 0;
      const auto [CandidateBlock, CandidateOp] = CandidatePoint->second;
      const auto [AlternativeBlock, AlternativeOp] = AlternativePoint->second;
      const bool CandidateFrame = canonicalFrameSlotKey(
          CandidateBlock, CandidateOp - 1, Query.Candidate, CandidateBase,
          CandidateOffset);
      const bool AlternativeFrame = canonicalFrameSlotKey(
          AlternativeBlock, AlternativeOp - 1,
          Query.Alternatives.front().Value, AlternativeBase,
          AlternativeOffset);
      if (!CandidateFrame || !AlternativeFrame)
        continue;
      const std::optional<int64_t> AdjustedCandidate = checkedFrameOffset(
          CandidateOffset, Query.FrameByteAddend, /*Subtract=*/false);
      const std::optional<int64_t> AdjustedAlternative = checkedFrameOffset(
          AlternativeOffset, Query.AlternativeFrameByteAddend,
          /*Subtract=*/false);
      Results[QueryIndex] =
          AdjustedCandidate && AdjustedAlternative &&
          CandidateBase == AlternativeBase &&
          *AdjustedCandidate == *AdjustedAlternative;
      if (EvidenceBudgetExhausted)
        markIncomplete(QueryIndex);
      continue;
    }

    ResolverResult CandidateValue;
    if (Query.Relation == JumpTableValueRelation::AuthenticatedFrameMemory ||
        Query.Relation == JumpTableValueRelation::FrameMemoryMatches) {
      if (Query.FrameMemorySize == 0)
        continue;
      if (!consumeProofPointLookup()) {
        markIncomplete(QueryIndex);
        continue;
      }
      auto CandidatePoint = Graph.PointToOp.find({Query.UseAddr, Query.UseSeq});
      if (CandidatePoint == Graph.PointToOp.end())
        continue;
      const auto [CandidateBlock, CandidateOp] = CandidatePoint->second;
      auto AddressPoint = CandidatePoint;
      if (Query.FrameAddressUseAddr != InvalidVA &&
          Query.FrameAddressUseSeq >= 0) {
        if (!consumeProofPointLookup()) {
          markIncomplete(QueryIndex);
          continue;
        }
        AddressPoint = Graph.PointToOp.find(
            {Query.FrameAddressUseAddr, Query.FrameAddressUseSeq});
        if (AddressPoint == Graph.PointToOp.end())
          continue;
      }
      const auto [AddressBlock, AddressOp] = AddressPoint->second;
      uint64_t SlotBase = InvalidVA;
      int64_t SlotOffset = 0;
      if (!canonicalFrameSlotKey(AddressBlock, AddressOp - 1,
                                 Query.Candidate, SlotBase, SlotOffset))
        continue;
      const std::optional<int64_t> AdjustedOffset = checkedFrameOffset(
          SlotOffset, Query.FrameByteAddend, /*Subtract=*/false);
      if (!AdjustedOffset)
        continue;
      MemoryMemo.clear();
      ActiveFrameMemoryQuery =
          Query.Relation == JumpTableValueRelation::AuthenticatedFrameMemory
              ? &Query
              : nullptr;
      CandidateValue = resolveMemory(CandidateBlock, CandidateOp, SlotBase,
                                     *AdjustedOffset, Query.FrameMemorySize,
                                     /*Depth=*/0);
      ActiveFrameMemoryQuery = nullptr;
      MemoryMemo.clear();
      if (Query.Relation ==
          JumpTableValueRelation::AuthenticatedFrameMemory) {
        Results[QueryIndex] =
            CandidateValue.Kind == ResolverResultKind::Value;
        if (EvidenceBudgetExhausted)
          markIncomplete(QueryIndex);
        continue;
      }
      if (Query.AlternativeFrameValueOffsets.size() !=
          Query.Alternatives.size())
        continue;
    } else if (Query.Candidate.isConst()) {
      CandidateValue = constantValue(Query.Candidate);
    } else if (Query.Candidate.isReg() || Query.Candidate.isTemp()) {
      if (!consumeProofPointLookup()) {
        markIncomplete(QueryIndex);
        continue;
      }
      auto CandidatePoint = Graph.PointToOp.find({Query.UseAddr, Query.UseSeq});
      if (CandidatePoint == Graph.PointToOp.end())
        continue;
      auto [CandidateBlock, CandidateBefore] = CandidatePoint->second;
      if (Query.Relation ==
          JumpTableValueRelation::ExactUnsignedModuloRecipe) {
        // This relation is defined only for an exact output occurrence.  The
        // ordinary query convention resolves inputs immediately before the
        // named op; advance exactly one operation after verifying that the
        // point really defines the requested lane.
        if (CandidateBlock < 0 ||
            CandidateBlock >= static_cast<int>(Graph.Blocks.size()) ||
            CandidateBefore < 0 ||
            CandidateBefore >= static_cast<int>(
                                   Graph.Blocks[CandidateBlock].Ops.size()))
          continue;
        const LowOp &Definition =
            Graph.Blocks[CandidateBlock].Ops[CandidateBefore];
        if (Definition.Output.Space != Query.Candidate.Space ||
            Definition.Output.Offset != Query.Candidate.Offset ||
            Definition.Output.Size != Query.Candidate.Size ||
            Definition.Addr != Query.UseAddr ||
            Definition.Seq != Query.UseSeq)
          continue;
        const std::optional<bool> Guarded =
            instructionIsGuarded(Definition.Addr);
        if (!Guarded) {
          markIncomplete(QueryIndex);
          continue;
        }
        if (*Guarded)
          continue;
        ++CandidateBefore;
      }
      CandidateValue =
          resolveValue(CandidateBlock, CandidateBefore, Query.Candidate,
                       /*Depth=*/0);
    } else {
      continue;
    }
    if (EvidenceBudgetExhausted) {
      markIncomplete(QueryIndex);
      continue;
    }
    if (CandidateValue.Kind != ResolverResultKind::Value) {
      if (Query.Relation == JumpTableValueRelation::MayDepend ||
          Query.Relation == JumpTableValueRelation::UnsignedLessThan) {
        markIncomplete(QueryIndex);
      }
      continue;
    }

    if (Query.Relation ==
        JumpTableValueRelation::ExactUnsignedModuloRecipe) {
      bool ProofComplete = false;
      Results[QueryIndex] = provesUnsignedUpperBound(
          CandidateValue.Value, Query.UnsignedUpperBound, ProofComplete,
          /*ExactModuloRecipeOnly=*/true);
      if (!ProofComplete) {
        markIncomplete(QueryIndex);
      }
      continue;
    }

    if (Query.Relation == JumpTableValueRelation::UnsignedLessThan) {
      bool ProofComplete = false;
      Results[QueryIndex] = provesUnsignedUpperBound(
          CandidateValue.Value, Query.UnsignedUpperBound, ProofComplete);
      if (!ProofComplete) {
        markIncomplete(QueryIndex);
      }
      continue;
    }

    std::vector<ResolverValue> AllowedValues;
    if (!consumeEvidenceProduct(Query.Alternatives.size(), 2) ||
        !consumeEvidence(2)) {
      markIncomplete(QueryIndex);
      continue;
    }
    AllowedValues.reserve(Query.Alternatives.size());
    for (const JumpTableValueOccurrence &Anchor : Query.Alternatives) {
      if (!consumeEvidence())
        break;
      ResolverResult IndexValue;
      if (Anchor.Value.isConst()) {
        IndexValue = constantValue(Anchor.Value);
      } else if (Anchor.Value.isReg() || Anchor.Value.isTemp()) {
        if (!consumeProofPointLookup()) {
          markIncomplete(QueryIndex);
          break;
        }
        auto IndexPoint = Graph.PointToOp.find({Anchor.Addr, Anchor.Seq});
        // Alternatives are a union of authenticated producers.  A producer
        // pruned from this candidate's proof graph cannot reach the queried
        // use and is therefore irrelevant; it must not poison every other
        // reachable alternative.  The candidate itself is still a must-value
        // over every feasible predecessor below, so dropping an unreachable
        // anchor cannot turn an ambiguous path into a match.
        if (IndexPoint == Graph.PointToOp.end())
          continue;
        auto [IndexBlock, IndexBefore] = IndexPoint->second;
        if (Anchor.DefinedAtPoint)
          ++IndexBefore;
        IndexValue = resolveValue(IndexBlock, IndexBefore, Anchor.Value,
                                  /*Depth=*/0);
      }
      if (IndexValue.Kind != ResolverResultKind::Value) {
        if (Query.Relation == JumpTableValueRelation::MayDepend) {
          markIncomplete(QueryIndex);
        }
        continue;
      }
      if (Query.Relation == JumpTableValueRelation::FrameMemoryMatches) {
        const size_t AlternativeIndex =
            static_cast<size_t>(&Anchor - Query.Alternatives.data());
        const uint16_t SliceOffset =
            Query.AlternativeFrameValueOffsets[AlternativeIndex];
        if (!IndexValue.Value ||
            SliceOffset > IndexValue.Value->Size ||
            Query.FrameMemorySize > IndexValue.Value->Size - SliceOffset)
          continue;
        IndexValue = resolverValue(resolverSlice(
            IndexValue.Value, SliceOffset, Query.FrameMemorySize,
            consumeEvidence, &EvidenceBudgetExhausted));
      }
      if (!consumeEvidence())
        break;
      AllowedValues.push_back(IndexValue.Value);
    }
    if (EvidenceBudgetExhausted) {
      markIncomplete(QueryIndex);
      continue;
    }
    if (AllowedValues.empty())
      continue;

    enum class MatchState : uint8_t { Active, No, Yes };
    std::map<const ResolverValueExpr *, MatchState> MatchMemo;
    auto consumeMatchMemoLookup = [&] {
      return consumeMatchWork(orderedSetLookupWork(MatchMemo.size()));
    };
    auto consumeMatchMemoInsert = [&] {
      const size_t Lookup = orderedSetLookupWork(MatchMemo.size());
      if (Lookup > std::numeric_limits<size_t>::max() - 5)
        return consumeMatchWork(std::numeric_limits<size_t>::max());
      return consumeMatchWork(Lookup + 5);
    };
    bool QueryBudgetExhausted = false;
    std::function<bool(const ResolverValue &, unsigned)> MatchesAllowed =
        [&](const ResolverValue &Value, unsigned Depth) {
          if (!Value)
            return false;
          if (Depth > limits::kMaxJumpTableGuardExpressionDepth) {
            QueryBudgetExhausted = true;
            MatchBudgetExhausted = true;
            Complete = false;
            return false;
          }
          if (!consumeMatchWork() || !consumeMatchMemoLookup()) {
            QueryBudgetExhausted = true;
            return false;
          }
          auto MemoIt = MatchMemo.find(Value.get());
          if (MemoIt != MatchMemo.end()) {
            if (MemoIt->second == MatchState::Active) {
              QueryBudgetExhausted = true;
              Complete = false;
              return false;
            }
            return MemoIt->second == MatchState::Yes;
          }
          if (!consumeMatchMemoInsert()) {
            QueryBudgetExhausted = true;
            return false;
          }
          MemoIt = MatchMemo.try_emplace(Value.get(), MatchState::Active).first;
          bool Matches = false;
          for (const ResolverValue &Allowed : AllowedValues) {
            if (!consumeMatchWork()) {
              QueryBudgetExhausted = true;
              break;
            }
            if (sameAllowedValue(Value, Allowed,
                                 Query.RequireExactAddressOwner) ||
                (Query.AllowZeroExtension && Value->Size < Allowed->Size &&
                 sameAllowedValue(resolverExtend(Value, Allowed->Size, false,
                                                  consumeMatchWork,
                                                  &MatchBudgetExhausted),
                                  Allowed, Query.RequireExactAddressOwner)) ||
                (Query.AllowZeroExtension && Allowed->Size < Value->Size &&
                 sameAllowedValue(Value,
                                  resolverExtend(Allowed, Value->Size, false,
                                                 consumeMatchWork,
                                                 &MatchBudgetExhausted),
                                  Query.RequireExactAddressOwner)) ||
                (Query.AllowSignExtension && Value->Size < Allowed->Size &&
                 sameAllowedValue(resolverExtend(Value, Allowed->Size, true,
                                                  consumeMatchWork,
                                                  &MatchBudgetExhausted),
                                  Allowed, Query.RequireExactAddressOwner)) ||
                (Query.AllowSignExtension && Allowed->Size < Value->Size &&
                 sameAllowedValue(Value,
                                  resolverExtend(Allowed, Value->Size, true,
                                                 consumeMatchWork,
                                                 &MatchBudgetExhausted),
                                  Query.RequireExactAddressOwner)) ||
                (Query.AllowZeroExtension &&
                 resolverNumericOccurrenceExtensionMatches(
                     Value, Allowed, /*Signed=*/false,
                     Query.RequireExactAddressOwner, sameAllowedValue,
                     consumeMatchWork, &MatchBudgetExhausted)) ||
                (Query.AllowSignExtension &&
                 resolverNumericOccurrenceExtensionMatches(
                     Value, Allowed, /*Signed=*/true,
                     Query.RequireExactAddressOwner, sameAllowedValue,
                     consumeMatchWork, &MatchBudgetExhausted))) {
              Matches = true;
              break;
            }
          }
          if (!Matches && Query.Relation == JumpTableValueRelation::MayDepend) {
            if (Value->Input)
              Matches = MatchesAllowed(Value->Input, Depth + 1);
            if (!Matches && !Value->Inputs.empty())
              Matches = std::any_of(Value->Inputs.begin(), Value->Inputs.end(),
                                    [&](const ResolverValue &Arm) {
                                      return MatchesAllowed(Arm, Depth + 1);
                                    });
          } else if (!Matches && Value->K == ResolverValueExpr::Kind::Merge &&
                     !Value->Inputs.empty()) {
            Matches = std::all_of(
                Value->Inputs.begin(), Value->Inputs.end(),
                [&](const ResolverValue &Arm) {
                  return MatchesAllowed(Arm, Depth + 1);
                });
          }
          MemoIt->second = Matches ? MatchState::Yes : MatchState::No;
          return Matches;
        };
    Results[QueryIndex] = MatchesAllowed(CandidateValue.Value, /*Depth=*/0);
    if (QueryBudgetExhausted || MatchBudgetExhausted ||
        EvidenceBudgetExhausted) {
      markIncomplete(QueryIndex);
    }
  }
  // A shared allowance belongs to the complete query transaction.  Do not
  // expose an order-dependent successful prefix when graph/value/match or
  // symbolic work runs out in a later query.
  if (EvidenceBudgetExhausted || MatchBudgetExhausted ||
      SymbolBudgetExhausted) {
    std::fill(Results.begin(), Results.end(), false);
    if (QueryAnalysisComplete)
      std::fill(QueryAnalysisComplete->begin(),
                QueryAnalysisComplete->end(), false);
    Complete = false;
  }
  if (AnalysisComplete)
    *AnalysisComplete = Complete;
  return Results;
}

bool relativeTargetTransformUsesPointerWidth(NdOp Opcode,
                                             uint16_t DynamicInputSize,
                                             uint16_t OtherInputSize,
                                             uint16_t OutputSize,
                                             uint16_t PointerSize) {
  if (PointerSize == 0 || DynamicInputSize == 0 || OutputSize == 0)
    return false;
  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return DynamicInputSize < PointerSize && OutputSize == PointerSize;
  case NdOp::INT_MULT:
  case NdOp::INT_LEFT:
    return DynamicInputSize == PointerSize && OutputSize == PointerSize;
  case NdOp::INT_ADD:
    return DynamicInputSize == PointerSize && OtherInputSize == PointerSize &&
           OutputSize == PointerSize;
  default:
    return false;
  }
}

bool CFGBuilder::branchTargetDependsOnTableLoad(
    const InsnRecord &Rec, const JumpTableInfo &Info,
    size_t *AggregateEvidenceBudget, bool *AnalysisComplete) const {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  RequestedCompleteJumpTableProof = true;
  if (!JumpTableProofContextComplete || !CurrentImg)
    return false;

  bool Complete = true;
  struct CompletionPublisher {
    bool *Output;
    const bool &Complete;
    ~CompletionPublisher() {
      if (Output)
        *Output = Complete;
    }
  } PublishCompletion{AnalysisComplete, Complete};
  if (Info.TargetLoads.empty())
    return false;

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  auto consumeWork = [&](size_t Amount = 1) {
    if (!consumeResolverGraphWork(AggregateEvidenceBudget, Amount)) {
      Complete = false;
      return false;
    }
    return true;
  };
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (AggregateEvidenceBudget && Count != 0 &&
        Cost > std::numeric_limits<size_t>::max() / Count) {
      *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    return consumeWork(Count * Cost);
  };
  auto ensureAppendCapacity = [&](auto &Values, size_t Additional = 1) {
    const size_t Max = std::numeric_limits<size_t>::max();
    if (Additional > Max - Values.size()) {
      if (AggregateEvidenceBudget)
        *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    const size_t Required = Values.size() + Additional;
    if (Required <= Values.capacity())
      return true;
    const size_t Doubled = Values.capacity() == 0
                               ? size_t{1}
                               : (Values.capacity() > Max / 2
                                      ? Max
                                      : Values.capacity() * 2);
    const size_t NewCapacity = std::max(Required, Doubled);
    if (NewCapacity == Max) {
      if (AggregateEvidenceBudget)
        *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    if (!consumeProduct(NewCapacity, 2) || !consumeWork(Values.size()))
      return false;
    Values.reserve(NewCapacity);
    return true;
  };
  const size_t MaxProofQueries =
      AggregateEvidenceBudget
          ? limits::kMaxJumpTableRoleMatchEvidenceWork
          : limits::kMaxJumpTableEntries;

  const LowOp *IndirectBranch = nullptr;
  for (const LowOp &Op : Rec.Ops) {
    if (!consumeWork())
      return false;
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1)
      IndirectBranch = &Op;
  }
  if (!IndirectBranch || IndirectBranch->Inputs[0].isConst())
    return false;

  enum TransformState : uint8_t {
    TargetRaw = 0,
    TargetExtended = 1u << 0,
    TargetScaled = 1u << 1,
    TargetAnchored = 1u << 2,
  };
  struct DerivedOccurrence {
    JumpTableValueOccurrence Occurrence;
    uint8_t State = TargetRaw;
  };
  if (!consumeWork(2))
    return false;
  std::vector<DerivedOccurrence> Derived;
  const uint16_t PointerSize = CurrentImg->getPointerSize();
  if (PointerSize == 0)
    return false;
  for (const JumpTableValueOccurrence &Occurrence : Info.TargetLoads) {
    if (!ensureAppendCapacity(Derived) || !consumeWork())
      return false;
    if (Occurrence.Addr == InvalidVA || Occurrence.Seq < 0 ||
        (!Occurrence.Value.isReg() && !Occurrence.Value.isTemp()) ||
        Occurrence.Value.Size == 0 || !Occurrence.DefinedAtPoint)
      return false;
    const LowOp *TargetLoad = nullptr;
    for (const auto &[Addr, Insn] : Insns) {
      (void)Addr;
      if (!consumeWork())
        return false;
      for (const LowOp &Op : Insn.Ops) {
        if (!consumeWork())
          return false;
        if (Op.Addr == Occurrence.Addr && Op.Seq == Occurrence.Seq &&
            Op.Opcode == NdOp::LOAD && sameVar(Op.Output, Occurrence.Value)) {
          TargetLoad = &Op;
          break;
        }
      }
    }
    if (!TargetLoad || TargetLoad->Output.Size != Info.EntrySize ||
        TargetLoad->Output.Size > PointerSize)
      return false;
    // An absolute code-pointer table already yields the final pointer and may
    // not be reinterpreted as a relative offset.  Conversely, a relative or
    // compact table begins with an entry-sized offset whose declared
    // extension/scale/anchor transform must be observed before publication.
    if (!Info.IsRelative && TargetLoad->Output.Size != PointerSize)
      return false;
    if (!consumeWork())
      return false;
    Derived.push_back({{TargetLoad->Output, TargetLoad->Addr, TargetLoad->Seq,
                        /*DefinedAtPoint=*/true},
                       TargetRaw});
  }

  uint8_t RequiredState = TargetRaw;
  if (Info.IsRelative) {
    if (Info.EntrySize < PointerSize)
      RequiredState |= TargetExtended;
    if (Info.EntryScale > 1)
      RequiredState |= TargetScaled;
    RequiredState |= TargetAnchored;
  }
  const va_t ExpectedAnchor =
      Info.HasTargetBase ? Info.TargetBase : Info.BaseAddr;

  auto alternativesFor = [&](uint8_t State)
      -> std::optional<std::vector<JumpTableValueOccurrence>> {
    if (!consumeWork(2))
      return std::nullopt;
    std::vector<JumpTableValueOccurrence> Alternatives;
    for (const DerivedOccurrence &D : Derived) {
      if (!consumeWork())
        return std::nullopt;
      if (D.State == State) {
        if (!ensureAppendCapacity(Alternatives) || !consumeWork())
          return std::nullopt;
        Alternatives.push_back(D.Occurrence);
      }
    }
    return Alternatives;
  };
  auto alreadyDerived = [&](const LowOp &Op,
                            uint8_t State) -> std::optional<bool> {
    for (const DerivedOccurrence &D : Derived) {
      if (!consumeWork())
        return std::nullopt;
      if (D.State == State && D.Occurrence.Addr == Op.Addr &&
          D.Occurrence.Seq == Op.Seq &&
          sameVar(D.Occurrence.Value, Op.Output))
        return true;
    }
    return false;
  };

  struct PendingTransform {
    const LowOp *Op = nullptr;
    uint8_t ResultState = TargetRaw;
    std::vector<size_t> QueryIndices;
  };

  // Grow exact transform states, batching every point-sensitive value query in
  // a pass through one resolver session.  State transitions encode the table
  // role: absolute pointers never accept anchor/scale arithmetic; relative
  // entries must perform the declared extension, optional scale, and exactly
  // one target-anchor add before they can reach INDIR_BR.
  bool TransformFixedPoint = false;
  for (int Pass = 0; Pass < limits::kMaxSliceDepth; ++Pass) {
    if (!consumeWork(4))
      return false;
    std::vector<JumpTableValueQuery> Queries;
    std::vector<PendingTransform> Pending;
    auto addQuery = [&](const NdVar &Candidate, const LowOp &Use,
                        llvm::ArrayRef<JumpTableValueOccurrence> Alternatives)
        -> std::optional<size_t> {
      if (Queries.size() >= MaxProofQueries) {
        Complete = false;
        return std::nullopt;
      }
      // JumpTableValueQuery owns four vector objects.  Pay their fixed
      // lifetimes, the retained outer element, and the exact alternatives
      // buffer/elements before either vector can allocate.
      if (!consumeWork(9) || !consumeProduct(Alternatives.size(), 3) ||
          !ensureAppendCapacity(Queries))
        return std::nullopt;
      const size_t Index = Queries.size();
      JumpTableValueQuery Query;
      Query.Candidate = Candidate;
      Query.UseAddr = Use.Addr;
      Query.UseSeq = Use.Seq;
      Query.Alternatives.reserve(Alternatives.size());
      Query.Alternatives.insert(Query.Alternatives.end(), Alternatives.begin(),
                                Alternatives.end());
      Queries.push_back(std::move(Query));
      return Index;
    };
    auto addPending = [&](const LowOp &Op, uint8_t State,
                          llvm::ArrayRef<size_t> QueryIndices) {
      const std::optional<bool> Already = alreadyDerived(Op, State);
      if (!Already)
        return false;
      if (*Already)
        return true;
      // PendingTransform owns one nested vector.  Prepay its fixed lifetime,
      // exact buffer/elements, the retained outer element, and the outer
      // capacity transition before construction.
      if (!consumeWork(3) || !consumeProduct(QueryIndices.size(), 3) ||
          !ensureAppendCapacity(Pending))
        return false;
      PendingTransform Candidate;
      Candidate.Op = &Op;
      Candidate.ResultState = State;
      Candidate.QueryIndices.reserve(QueryIndices.size());
      Candidate.QueryIndices.insert(Candidate.QueryIndices.end(),
                                    QueryIndices.begin(), QueryIndices.end());
      Pending.push_back(std::move(Candidate));
      return true;
    };

    for (const auto &[Addr, Insn] : Insns) {
      (void)Addr;
      if (!consumeWork())
        return false;
      if (Insn.IsInstructionGuard)
        continue; // a predicated write is not a necessary target source
      for (const LowOp &Op : Insn.Ops) {
        if (!consumeWork())
          return false;
        if ((!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size == 0)
          continue;
        for (uint8_t State = TargetRaw; State <= RequiredState; ++State) {
          if (!consumeWork())
            return false;
          auto Alternatives = alternativesFor(State);
          if (!Alternatives)
            return false;
          if (Alternatives->empty())
            continue;
          switch (Op.Opcode) {
          case NdOp::COPY:
            if (Op.NumInputs >= 1) {
              uint8_t Next = State;
              if (Op.Inputs[0].Size < Op.Output.Size) {
                if (!Info.IsRelative || Info.IsSigned ||
                    !(RequiredState & TargetExtended) ||
                    (State & (TargetScaled | TargetAnchored)) ||
                    Op.Output.Size > PointerSize)
                  break;
                // Some lifters materialize compact values through an
                // architecture-register width before widening to the guest
                // pointer (e.g. u8 -> w32 -> x64).  Preserve that intermediate
                // value in the raw state; only the complete pointer-width
                // result satisfies TargetExtended.
                if (relativeTargetTransformUsesPointerWidth(
                        Op.Opcode, Op.Inputs[0].Size, 0, Op.Output.Size,
                        PointerSize))
                  Next |= TargetExtended;
              } else if (Op.Inputs[0].Size != Op.Output.Size) {
                break;
              }
              auto Query = addQuery(Op.Inputs[0], Op, *Alternatives);
              if (!Query ||
                  !addPending(Op, Next, llvm::ArrayRef<size_t>(&*Query, 1)))
                return false;
            }
            break;
          case NdOp::INT_ZEXT:
          case NdOp::INT_SEXT:
            if (Op.NumInputs >= 1 && Info.IsRelative &&
                (RequiredState & TargetExtended) &&
                !(State & (TargetScaled | TargetAnchored)) &&
                Op.Inputs[0].Size < Op.Output.Size &&
                Op.Output.Size <= PointerSize &&
                ((Info.IsSigned && Op.Opcode == NdOp::INT_SEXT) ||
                 (!Info.IsSigned && Op.Opcode == NdOp::INT_ZEXT)))
              {
                auto Query = addQuery(Op.Inputs[0], Op, *Alternatives);
                if (!Query ||
                    !addPending(
                        Op,
                        State | (relativeTargetTransformUsesPointerWidth(
                                     Op.Opcode, Op.Inputs[0].Size, 0,
                                     Op.Output.Size, PointerSize)
                                     ? TargetExtended
                                     : TargetRaw),
                        llvm::ArrayRef<size_t>(&*Query, 1)))
                  return false;
              }
            break;
          case NdOp::SUBBYTES:
            if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                Op.Inputs[1].Offset == 0 &&
                Op.Inputs[0].Size == Op.Output.Size) {
              auto Query = addQuery(Op.Inputs[0], Op, *Alternatives);
              if (!Query ||
                  !addPending(Op, State, llvm::ArrayRef<size_t>(&*Query, 1)))
                return false;
            }
            break;
          case NdOp::INT_MULT:
            if (Op.NumInputs >= 2 && Info.IsRelative && Info.EntryScale > 1 &&
                !(State & TargetScaled) && !(State & TargetAnchored) &&
                (!(RequiredState & TargetExtended) || (State & TargetExtended)))
              for (int Side = 0; Side < 2; ++Side)
                if (Op.Inputs[1 - Side].isConst() &&
                    Op.Inputs[1 - Side].Offset == Info.EntryScale &&
                    relativeTargetTransformUsesPointerWidth(
                        Op.Opcode, Op.Inputs[Side].Size,
                        Op.Inputs[1 - Side].Size, Op.Output.Size, PointerSize))
                  {
                    auto Query =
                        addQuery(Op.Inputs[Side], Op, *Alternatives);
                    if (!Query ||
                        !addPending(Op, State | TargetScaled,
                                    llvm::ArrayRef<size_t>(&*Query, 1)))
                      return false;
                  }
            break;
          case NdOp::INT_LEFT:
            if (Op.NumInputs >= 2 && Info.IsRelative && Info.EntryScale > 1 &&
                !(State & TargetScaled) && !(State & TargetAnchored) &&
                (!(RequiredState & TargetExtended) ||
                 (State & TargetExtended)) &&
                Op.Inputs[1].isConst() && Op.Inputs[1].Offset < 64 &&
                (uint64_t{1} << Op.Inputs[1].Offset) == Info.EntryScale &&
                relativeTargetTransformUsesPointerWidth(
                    Op.Opcode, Op.Inputs[0].Size, Op.Inputs[1].Size,
                    Op.Output.Size, PointerSize))
              {
                auto Query = addQuery(Op.Inputs[0], Op, *Alternatives);
                if (!Query ||
                    !addPending(Op, State | TargetScaled,
                                llvm::ArrayRef<size_t>(&*Query, 1)))
                  return false;
              }
            break;
          case NdOp::INT_ADD:
            if (Op.NumInputs >= 2 && Info.IsRelative &&
                !(State & TargetAnchored) &&
                (!(RequiredState & TargetExtended) ||
                 (State & TargetExtended)) &&
                (!(RequiredState & TargetScaled) || (State & TargetScaled))) {
              for (int Side = 0; Side < 2; ++Side) {
                if (!relativeTargetTransformUsesPointerWidth(
                        Op.Opcode, Op.Inputs[Side].Size,
                        Op.Inputs[1 - Side].Size, Op.Output.Size, PointerSize))
                  continue;
                std::array<size_t, 2> QueryIndices{};
                size_t QueryCount = 0;
                auto Dynamic =
                    addQuery(Op.Inputs[Side], Op, *Alternatives);
                if (!Dynamic)
                  return false;
                QueryIndices[QueryCount++] = *Dynamic;
                const NdVar &Anchor = Op.Inputs[1 - Side];
                if (Anchor.isConst()) {
                  if (!isExactAddressProvenance(Anchor.Provenance) ||
                      static_cast<va_t>(Anchor.Offset) != ExpectedAnchor)
                    continue;
                } else if (Anchor.isReg() || Anchor.isTemp()) {
                  const std::array<JumpTableValueOccurrence, 1>
                      AnchorAlternative = {
                          JumpTableValueOccurrence{
                              NdVar::address(ExpectedAnchor, Anchor.Size),
                              InvalidVA, -1, /*DefinedAtPoint=*/false}};
                  auto AnchorQuery = addQuery(Anchor, Op, AnchorAlternative);
                  if (!AnchorQuery)
                    return false;
                  QueryIndices[QueryCount++] = *AnchorQuery;
                } else {
                  continue;
                }
                if (!addPending(Op, State | TargetAnchored,
                                llvm::ArrayRef<size_t>(QueryIndices.data(),
                                                       QueryCount)))
                  return false;
              }
            }
            break;
          case NdOp::SELECT:
            if (Op.NumInputs >= 3) {
              std::array<size_t, 2> QueryIndices{};
              auto TrueQuery = addQuery(Op.Inputs[1], Op, *Alternatives);
              auto FalseQuery = addQuery(Op.Inputs[2], Op, *Alternatives);
              if (!TrueQuery || !FalseQuery)
                return false;
              QueryIndices = {*TrueQuery, *FalseQuery};
              if (!addPending(Op, State, QueryIndices))
                return false;
            }
            break;
          default:
            break;
          }
        }
      }
    }

    if (Queries.empty()) {
      TransformFixedPoint = true;
      break;
    }
    bool TransformQueriesComplete = true;
    const std::vector<bool> QueryResults = tableValuesMatchAtUses(
        Queries, &TransformQueriesComplete, nullptr, InvalidVA, nullptr,
        AggregateEvidenceBudget,
        limits::kMaxJumpTableRoleMatchEvidenceWork);
    if (!TransformQueriesComplete || QueryResults.size() != Queries.size()) {
      Complete = false;
      return false;
    }
    bool Changed = false;
    for (const PendingTransform &P : Pending) {
      if (!consumeWork())
        return false;
      bool AllQueriesMatched = P.Op != nullptr;
      for (size_t I : P.QueryIndices) {
        if (!consumeWork())
          return false;
        if (I >= QueryResults.size() || !QueryResults[I]) {
          AllQueriesMatched = false;
          break;
        }
      }
      if (!AllQueriesMatched)
        continue;
      const std::optional<bool> Already = alreadyDerived(*P.Op, P.ResultState);
      if (!Already)
        return false;
      if (*Already)
        continue;
      if (!ensureAppendCapacity(Derived) || !consumeWork())
        return false;
      Derived.push_back({{P.Op->Output, P.Op->Addr, P.Op->Seq,
                          /*DefinedAtPoint=*/true},
                         P.ResultState});
      Changed = true;
    }
    if (!Changed) {
      TransformFixedPoint = true;
      break;
    }
  }
  if (!TransformFixedPoint) {
    Complete = false;
    return false;
  }

  auto FinalAlternatives = alternativesFor(RequiredState);
  if (!FinalAlternatives || FinalAlternatives->empty() ||
      IndirectBranch->Inputs[0].Size != PointerSize)
    return false;
  for (const JumpTableValueOccurrence &Alternative : *FinalAlternatives) {
    if (!consumeWork())
      return false;
    if (Alternative.Value.Size != PointerSize)
      return false;
  }
  // FinalQueries and its one JumpTableValueQuery own five vector objects in
  // total.  The alternatives buffer transfers from FinalAlternatives, so only
  // fixed lifetimes, the retained query element, and outer capacity are new.
  if (!consumeWork(11))
    return false;
  std::vector<JumpTableValueQuery> FinalQueries;
  if (!ensureAppendCapacity(FinalQueries))
    return false;
  JumpTableValueQuery FinalQuery;
  FinalQuery.Candidate = IndirectBranch->Inputs[0];
  FinalQuery.UseAddr = IndirectBranch->Addr;
  FinalQuery.UseSeq = IndirectBranch->Seq;
  FinalQuery.Alternatives = std::move(*FinalAlternatives);
  FinalQueries.push_back(std::move(FinalQuery));
  bool FinalQueryComplete = true;
  const std::vector<bool> Final = tableValuesMatchAtUses(
      FinalQueries, &FinalQueryComplete, nullptr, InvalidVA, nullptr,
      AggregateEvidenceBudget, limits::kMaxJumpTableRoleMatchEvidenceWork);
  if (!FinalQueryComplete || Final.size() != FinalQueries.size()) {
    Complete = false;
    return false;
  }
  return !Final.empty() && Final.front();
}

bool CFGBuilder::tableLoadAddressesMatchRole(
    JumpTableInfo &Info, size_t *AggregateEvidenceBudget,
    bool *AnalysisComplete) const {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  RequestedCompleteJumpTableProof = true;
  if (!JumpTableProofContextComplete || !CurrentImg)
    return false;

  bool Complete = true;
  struct CompletionPublisher {
    bool *Output;
    const bool &Complete;
    ~CompletionPublisher() {
      if (Output)
        *Output = Complete;
    }
  } PublishCompletion{AnalysisComplete, Complete};
  if (Info.LoadRoles.empty())
    return false;

  auto consumeWork = [&](size_t Amount = 1) {
    if (!consumeResolverGraphWork(AggregateEvidenceBudget, Amount)) {
      Complete = false;
      return false;
    }
    return true;
  };
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (AggregateEvidenceBudget && Count != 0 &&
        Cost > std::numeric_limits<size_t>::max() / Count) {
      *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    return consumeWork(Count * Cost);
  };
  auto consumeSum = [&](std::initializer_list<size_t> Terms) {
    size_t Total = 0;
    for (size_t Term : Terms) {
      if (Term > std::numeric_limits<size_t>::max() - Total) {
        if (AggregateEvidenceBudget)
          *AggregateEvidenceBudget = 0;
        Complete = false;
        return false;
      }
      Total += Term;
    }
    return consumeWork(Total);
  };
  auto consumeExecutableOwnerQuery = [&]() {
    return consumeProduct(CurrentImg->Segments.size(), 8) &&
           consumeProduct(CurrentImg->Sections.size(), 8) &&
           consumeProduct(CurrentImg->Imports.size(), 2) &&
           consumeProduct(CurrentImg->ImportStubRanges.size(), 2) &&
           consumeProduct(
               2, orderedSetLookupWork(
                      CurrentImg->ImportStubIndices.size())) &&
           consumeProduct(
               2, orderedSetLookupWork(
                      CurrentImg->RuntimeFunctionAddrs.size())) &&
           consumeWork(orderedSetLookupWork(
               CurrentImg->VerifiedFunctionEntries.size())) &&
           consumeWork(CurrentImg->KnownCodeRanges.size()) &&
           consumeWork(CurrentImg->Symbols.size());
  };
  auto consumeObjectDataQuery = [&]() {
    return consumeProduct(CurrentImg->Segments.size(), 4) &&
           consumeProduct(CurrentImg->Sections.size(), 4);
  };
  auto consumeRelocatedOwnerQuery = [&]() {
    return consumeSum(
               {CurrentImg->Segments.size(), CurrentImg->Sections.size()}) &&
           consumeExecutableOwnerQuery() && consumeExecutableOwnerQuery();
  };
  auto consumeImmutableDataSpanQuery = [&]() {
    return consumeProduct(CurrentImg->Segments.size(), 15) &&
           consumeProduct(CurrentImg->Sections.size(), 12) &&
           consumeExecutableOwnerQuery() && consumeExecutableOwnerQuery();
  };
  auto chargeInsnInventoryScan = [&]() {
    if (!consumeWork(Insns.size()))
      return false;
    for (const auto &[Addr, Rec] : Insns) {
      (void)Addr;
      if (!consumeWork(Rec.Ops.size()))
        return false;
    }
    return true;
  };
  auto ensureAppendCapacity = [&](auto &Values, size_t Additional = 1) {
    const size_t Max = std::numeric_limits<size_t>::max();
    if (Additional > Max - Values.size()) {
      if (AggregateEvidenceBudget)
        *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    const size_t Required = Values.size() + Additional;
    if (Required <= Values.capacity())
      return true;
    const size_t Doubled = Values.capacity() == 0
                               ? size_t{1}
                               : (Values.capacity() > Max / 2
                                      ? Max
                                      : Values.capacity() * 2);
    const size_t NewCapacity = std::max(Required, Doubled);
    if (NewCapacity == Max) {
      if (AggregateEvidenceBudget)
        *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    if (!consumeProduct(NewCapacity, 2) || !consumeWork(Values.size()))
      return false;
    Values.reserve(NewCapacity);
    return true;
  };
  auto consumeSortWork = [&](size_t Count) {
    size_t Levels = 0;
    for (size_t Width = Count; Width > 1; Width = Width / 2 + Width % 2)
      ++Levels;
    if (Levels > std::numeric_limits<size_t>::max() - 3) {
      if (AggregateEvidenceBudget)
        *AggregateEvidenceBudget = 0;
      Complete = false;
      return false;
    }
    return consumeProduct(Count, Levels + 3);
  };

  // The role proof refines occurrence metadata.  Work only on prepaid local
  // copies and publish them after every graph/value/address certificate has
  // succeeded; exhaustion must not leave a reachable-role prefix or a cleared
  // composite selector in the caller's candidate.
  if (!consumeProduct(Info.LoadRoles.size(), 3) || !consumeWork(2))
    return false;
  for (const JumpTableLoadRole &Role : Info.LoadRoles) {
    if (!consumeWork() || !consumeProduct(Role.AllowedBases.size(), 3) ||
        !consumeProduct(Role.Indices.size(), 3) ||
        !consumeProduct(Role.FrameStorage.Initializers.size(), 3) ||
        !consumeWork(6))
      return false;
    for (const JumpTableFrameInitializerChunk &Initializer :
         Role.FrameStorage.Initializers)
      if (!consumeWork() ||
          !consumeProduct(Initializer.StaticSources.size(), 3) ||
          !consumeWork(2))
        return false;
  }
  if (!consumeProduct(Info.TargetLoads.size(), 3) ||
      !consumeProduct(Info.IndexValueAlternatives.size(), 3) ||
      !consumeWork(4))
    return false;
  std::vector<JumpTableLoadRole> WorkingLoadRoles = Info.LoadRoles;
  std::vector<JumpTableValueOccurrence> WorkingTargetLoads = Info.TargetLoads;
  std::vector<JumpTableValueOccurrence> WorkingIndexValueAlternatives =
      Info.IndexValueAlternatives;
  NdVar WorkingIndexValueAtUse = Info.IndexValueAtUse;
  va_t WorkingIndexUseAddr = Info.IndexUseAddr;
  int WorkingIndexUseSeq = Info.IndexUseSeq;
  bool WorkingIndexValueDefinedAtUse = Info.IndexValueDefinedAtUse;
  va_t WorkingTableLoadAddr = Info.TableLoadAddr;
  int WorkingTableLoadSeq = Info.TableLoadSeq;

  // A shared -O0 computed-goto dispatch is discovered in two monotone CFG
  // rounds.  Before its table edges are published, only the entry goto site's
  // LOAD is reachable; the LOADs in label blocks become reachable after those
  // labels are installed as successors.  Requiring every lexically discovered
  // role in the bootstrap round creates a circular proof obligation.  Keep
  // only roles present in this candidate's current proof graph.  The next
  // multi-stage round rediscovers all roles and revalidates them after the new
  // edges are present, so an invalid case-path role cannot survive the fixed
  // point.  Composite tables are indivisible and retain their dedicated all-
  // role proof.
  if (!Info.TwoLevelIndex && !Info.TwoTableSelect &&
      WorkingLoadRoles.size() > 1) {
    std::vector<ResolverInsnSnapshot> Snapshot;
    if (!copyResolverInsnSnapshots(
            Insns, Snapshot,
            [](va_t, const auto &Rec) -> const std::vector<va_t> & {
              return Rec.JumpTableTargets;
            },
            [&](size_t Amount) { return consumeWork(Amount); }))
      return false;
    const std::set<va_t> &ProofRoots = ActiveJumpTableProofRoots
                                           ? *ActiveJumpTableProofRoots
                                           : PersistentCFGRoots;
    bool GraphComplete = false;
    const ResolverFlowGraph Graph = buildResolverFlowGraph(
        Snapshot, BlockStarts, ProofRoots, DiscoveredCodeRefSources,
        [&](va_t Address, const std::set<va_t> *ActiveOwners) {
          return resolvedJumpTableOwnsStorageAddress(
              Address, ActiveOwners, AggregateEvidenceBudget);
        },
        AggregateEvidenceBudget, &GraphComplete);
    if (!GraphComplete) {
      Complete = false;
      return false;
    }
    constexpr size_t ProofPointKeyWork = 2;
    if (!consumeProduct(
            WorkingLoadRoles.size(),
            ProofPointKeyWork *
                orderedSetLookupWork(Graph.PointToOp.size())))
      return false;
    auto IsReachable = [&](const JumpTableValueOccurrence &Occurrence) {
      return Occurrence.Addr != InvalidVA && Occurrence.Seq >= 0 &&
             Graph.PointToOp.count({Occurrence.Addr, Occurrence.Seq});
    };

    std::vector<JumpTableLoadRole> ReachableRoles;
    if (!consumeProduct(WorkingLoadRoles.size(), 2) || !consumeWork(2))
      return false;
    ReachableRoles.reserve(WorkingLoadRoles.size());
    for (const JumpTableLoadRole &Role : WorkingLoadRoles) {
      if (!consumeWork())
        return false;
      if (IsReachable(Role.Load)) {
        if (!consumeWork() ||
            !consumeProduct(Role.AllowedBases.size(), 3) ||
            !consumeProduct(Role.Indices.size(), 3) ||
            !consumeProduct(Role.FrameStorage.Initializers.size(), 3) ||
            !consumeWork(6))
          return false;
        for (const JumpTableFrameInitializerChunk &Initializer :
             Role.FrameStorage.Initializers)
          if (!consumeWork() ||
              !consumeProduct(Initializer.StaticSources.size(), 3) ||
              !consumeWork(2))
            return false;
        ReachableRoles.push_back(Role);
      }
    }
    if (ReachableRoles.empty())
      return false;

    if (ReachableRoles.size() != WorkingLoadRoles.size()) {
      WorkingLoadRoles = std::move(ReachableRoles);
      std::set<std::pair<va_t, int>> ReachableLoads;
      std::vector<JumpTableValueOccurrence> ReachableIndices;
      size_t ReachableIndexUpperBound = 0;
      for (const JumpTableLoadRole &Role : WorkingLoadRoles) {
        if (!consumeWork() ||
            Role.Indices.size() >
                std::numeric_limits<size_t>::max() -
                    ReachableIndexUpperBound) {
          if (AggregateEvidenceBudget)
            *AggregateEvidenceBudget = 0;
          Complete = false;
          return false;
        }
        ReachableIndexUpperBound += Role.Indices.size();
      }
      if (!consumeProduct(ReachableIndexUpperBound, 2) || !consumeWork(2))
        return false;
      ReachableIndices.reserve(ReachableIndexUpperBound);
      for (const JumpTableLoadRole &Role : WorkingLoadRoles) {
        if (!consumeWork() || !consumeWork(Role.Indices.size()))
          return false;
        if (!consumeProduct(ProofPointKeyWork,
                            orderedSetLookupWork(ReachableLoads.size())) ||
            !consumeWork(ProofPointKeyWork * 2 + 3))
          return false;
        ReachableLoads.emplace(Role.Load.Addr, Role.Load.Seq);
        for (const JumpTableValueOccurrence &Index : Role.Indices) {
          if (!consumeWork(ReachableIndices.size()))
            return false;
          if (std::find(ReachableIndices.begin(), ReachableIndices.end(),
                        Index) == ReachableIndices.end()) {
            if (!consumeWork())
              return false;
            ReachableIndices.push_back(Index);
          }
        }
      }
      if (!consumeWork(WorkingTargetLoads.size()) ||
          !consumeWork(WorkingTargetLoads.size()) ||
          !consumeProduct(
              WorkingTargetLoads.size(),
              ProofPointKeyWork *
                  orderedSetLookupWork(ReachableLoads.size())))
        return false;
      WorkingTargetLoads.erase(
          std::remove_if(WorkingTargetLoads.begin(), WorkingTargetLoads.end(),
                         [&](const JumpTableValueOccurrence &Load) {
                           return !ReachableLoads.count({Load.Addr, Load.Seq});
                         }),
          WorkingTargetLoads.end());
      if (WorkingTargetLoads.empty() || ReachableIndices.empty())
        return false;
      WorkingIndexValueAlternatives = std::move(ReachableIndices);
      const JumpTableValueOccurrence &Index =
          WorkingIndexValueAlternatives.front();
      WorkingIndexValueAtUse = Index.Value;
      WorkingIndexUseAddr = Index.Addr;
      WorkingIndexUseSeq = Index.Seq;
      WorkingIndexValueDefinedAtUse = Index.DefinedAtPoint;
      WorkingTableLoadAddr = WorkingLoadRoles.front().Load.Addr;
      WorkingTableLoadSeq = WorkingLoadRoles.front().Load.Seq;
    }
  }

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  auto occurrenceFor = [](const LowOp &Op) {
    return JumpTableValueOccurrence{Op.Output, Op.Addr, Op.Seq,
                                    /*DefinedAtPoint=*/true};
  };
  const uint16_t GuestPointerSize = CurrentImg->getPointerSize();
  auto guestAddressView = [&](const NdVar &Value) {
    NdVar View = Value;
    if ((View.isReg() || View.isTemp()) && GuestPointerSize != 0 &&
        View.Size > GuestPointerSize)
      View.Size = GuestPointerSize;
    return View;
  };
  auto producesCanonicalBoolean = [](NdOp Opcode) {
    switch (Opcode) {
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
    case NdOp::BOOL_NOT:
    case NdOp::FLOAT_EQUAL:
    case NdOp::FLOAT_NOTEQUAL:
    case NdOp::FLOAT_LESS:
    case NdOp::FLOAT_LESSEQUAL:
    case NdOp::FLOAT_ISNAN:
      return true;
    default:
      return false;
    }
  };

  // INT_NEG2 yields an all-zero/all-one selection mask only when its input is
  // a canonical boolean.  Merely proving that the complementary INT_NOT uses
  // the same input is insufficient: for an arbitrary integer C, -C and ~(-C)
  // splice two base addresses bitwise and can form a third pointer.  Name all
  // exact boolean-producing occurrences up front; the batch reaching-value
  // query below then proves the mask input comes from one of them on every
  // feasible path.  A widening COPY/ZEXT remains boolean, sign extension or
  // truncation does not.
  std::vector<JumpTableValueOccurrence> BooleanAlternatives;
  std::vector<const LowOp *> BooleanCombiners;
  if (!consumeWork(4) || !chargeInsnInventoryScan())
    return false;
  for (const auto &[Addr, Insn] : Insns) {
    (void)Addr;
    if (Insn.IsInstructionGuard)
      continue;
    for (const LowOp &Op : Insn.Ops) {
      if (Op.Output.Size == 0)
        continue;
      const bool IsAndOne =
          Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2 &&
          ((Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 1) ||
           (Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 1));
      if (producesCanonicalBoolean(Op.Opcode) || IsAndOne) {
        if (!ensureAppendCapacity(BooleanAlternatives) || !consumeWork())
          return false;
        BooleanAlternatives.push_back(occurrenceFor(Op));
      } else if ((Op.Opcode == NdOp::BOOL_AND ||
                  Op.Opcode == NdOp::BOOL_OR ||
                  Op.Opcode == NdOp::BOOL_XOR) &&
                 Op.NumInputs >= 2) {
        if (!ensureAppendCapacity(BooleanCombiners) || !consumeWork())
          return false;
        BooleanCombiners.push_back(&Op);
      }
    }
  }

  const size_t MaxProofQueries =
      AggregateEvidenceBudget
          ? limits::kMaxJumpTableRoleMatchEvidenceWork
          : limits::kMaxJumpTableEntries;
  auto pushQuery =
      [&](std::vector<JumpTableValueQuery> &Queries, const NdVar &Candidate,
          const LowOp &Use,
          llvm::ArrayRef<JumpTableValueOccurrence> Alternatives,
          bool AllowZeroExtension = false,
          bool AllowSignExtension = false) -> std::optional<size_t> {
    if (Queries.size() >= MaxProofQueries) {
      Complete = false;
      return std::nullopt;
    }
    if (!ensureAppendCapacity(Queries) || !consumeWork() ||
        !consumeProduct(Alternatives.size(), 3) || !consumeWork(8))
      return std::nullopt;
    const size_t Index = Queries.size();
    JumpTableValueQuery Query;
    Query.Candidate = Candidate;
    Query.UseAddr = Use.Addr;
    Query.UseSeq = Use.Seq;
    Query.Alternatives.reserve(Alternatives.size());
    Query.Alternatives.insert(Query.Alternatives.end(), Alternatives.begin(),
                              Alternatives.end());
    Query.AllowZeroExtension = AllowZeroExtension;
    Query.AllowSignExtension = AllowSignExtension;
    Queries.push_back(std::move(Query));
    return Index;
  };

  // BOOL_AND/OR/XOR are bitwise in the production emitter.  They preserve a
  // canonical boolean domain only when every input is already canonical, so
  // grow their output set with an occurrence-sensitive must fixed point rather
  // than trusting the opcode name.  COPY/ZEXT/SELECT/CFG merges are handled by
  // the shared reaching-value matcher itself.
  std::set<std::pair<va_t, int>> ProvenBooleanCombiners;
  bool BooleanFixedPoint = BooleanCombiners.empty();
  for (unsigned Round = 0;
       Round < limits::kMaxQuasiCopyDepth && !BooleanCombiners.empty();
       ++Round) {
    struct PendingBoolean {
      const LowOp *Op = nullptr;
      std::vector<size_t> QueryIndices;
    };
    std::vector<JumpTableValueQuery> Queries;
    std::vector<PendingBoolean> Pending;
    if (!consumeWork(4))
      return false;
    for (const LowOp *Op : BooleanCombiners) {
      if (!consumeWork())
        return false;
      if (!Op)
        continue;
      constexpr size_t BooleanCombinerKeyWork = 2;
      if (!consumeProduct(
              BooleanCombinerKeyWork,
              orderedSetLookupWork(ProvenBooleanCombiners.size())))
        return false;
      if (ProvenBooleanCombiners.count({Op->Addr, Op->Seq}))
        continue;
      if (!consumeWork(2))
        return false;
      PendingBoolean Candidate{Op, {}};
      bool InputsCanBeBoolean = true;
      for (unsigned I = 0; I < 2; ++I) {
        const NdVar &Input = Op->Inputs[I];
        if (Input.isConst()) {
          if (Input.Offset > 1)
            InputsCanBeBoolean = false;
          continue;
        }
        if (BooleanAlternatives.empty()) {
          InputsCanBeBoolean = false;
          continue;
        }
        auto Query = pushQuery(Queries, Input, *Op, BooleanAlternatives,
                               /*AllowZeroExtension=*/true,
                               /*AllowSignExtension=*/false);
        if (!Query || !ensureAppendCapacity(Candidate.QueryIndices) ||
            !consumeWork())
          return false;
        Candidate.QueryIndices.push_back(*Query);
      }
      if (InputsCanBeBoolean) {
        if (!ensureAppendCapacity(Pending) || !consumeWork())
          return false;
        Pending.push_back(std::move(Candidate));
      }
    }
    bool QueriesComplete = true;
    std::vector<bool> Results;
    if (!Queries.empty()) {
      Results = tableValuesMatchAtUses(
          Queries, &QueriesComplete, nullptr, InvalidVA, nullptr,
          AggregateEvidenceBudget,
          limits::kMaxJumpTableRoleMatchEvidenceWork);
      if (!QueriesComplete || Results.size() != Queries.size()) {
        Complete = false;
        return false;
      }
    }
    bool Changed = false;
    for (const PendingBoolean &Candidate : Pending) {
      bool AllInputsMatch = Candidate.Op != nullptr;
      for (size_t I : Candidate.QueryIndices) {
        if (!consumeWork())
          return false;
        if (I >= Results.size() || !Results[I]) {
          AllInputsMatch = false;
          break;
        }
      }
      if (!AllInputsMatch)
        continue;
      constexpr size_t BooleanCombinerKeyWork = 2;
      if (!consumeProduct(
              BooleanCombinerKeyWork,
              orderedSetLookupWork(ProvenBooleanCombiners.size())) ||
          !consumeWork(BooleanCombinerKeyWork * 2 + 2))
        return false;
      ProvenBooleanCombiners.insert({Candidate.Op->Addr, Candidate.Op->Seq});
      if (!ensureAppendCapacity(BooleanAlternatives) || !consumeWork())
        return false;
      BooleanAlternatives.push_back(occurrenceFor(*Candidate.Op));
      Changed = true;
    }
    if (!Changed) {
      BooleanFixedPoint = true;
      break;
    }
  }
  if (!BooleanFixedPoint) {
    Complete = false;
    return false;
  }

  struct RoleState {
    JumpTableLoadRole *Role = nullptr;
    const LowOp *Load = nullptr;
    const LowOp *Select = nullptr;
    const LowOp *Blend = nullptr;
    const LowOp *PositiveAnd = nullptr;
    const LowOp *NegativeAnd = nullptr;
    const LowOp *PositiveMask = nullptr;
    const LowOp *NegativeMask = nullptr;
    std::vector<size_t> SelectQueries;
    std::vector<JumpTableValueOccurrence> DynamicAlternatives;
  };
  std::vector<RoleState> Roles;
  if (!consumeWork(2) || !consumeProduct(WorkingLoadRoles.size(), 2))
    return false;
  Roles.reserve(WorkingLoadRoles.size());
  for (JumpTableLoadRole &Role : WorkingLoadRoles) {
    if (!consumeWork())
      return false;
    if (Role.LoadWidth == 0 || Role.AddressScale == 0 ||
        Role.AllowedBases.empty() || Role.Indices.empty() ||
        Role.Load.Addr == InvalidVA || Role.Load.Seq < 0 ||
        !Role.Load.DefinedAtPoint)
      return false;
    const LowOp *Load = nullptr;
    const LowOp *Select = nullptr;
    const LowOp *Blend = nullptr;
    const LowOp *PositiveAnd = nullptr;
    const LowOp *NegativeAnd = nullptr;
    const LowOp *PositiveMask = nullptr;
    const LowOp *NegativeMask = nullptr;
    if (!chargeInsnInventoryScan())
      return false;
    for (const auto &[Addr, Insn] : Insns) {
      (void)Addr;
      for (const LowOp &Op : Insn.Ops) {
        if (Op.Opcode == NdOp::LOAD && Op.Addr == Role.Load.Addr &&
            Op.Seq == Role.Load.Seq && sameVar(Op.Output, Role.Load.Value))
          Load = &Op;
        if (Role.HasBaseSelect && Op.Opcode == NdOp::SELECT &&
            Op.Addr == Role.SelectedBase.Addr &&
            Op.Seq == Role.SelectedBase.Seq &&
            sameVar(Op.Output, Role.SelectedBase.Value))
          Select = &Op;
        if (Role.HasBaseMaskBlend && Op.Opcode == NdOp::INT_OR &&
            Op.Addr == Role.SelectedBase.Addr &&
            Op.Seq == Role.SelectedBase.Seq &&
            sameVar(Op.Output, Role.SelectedBase.Value))
          Blend = &Op;
        if (Role.HasBaseMaskBlend && Op.Opcode == NdOp::INT_AND &&
            Op.Addr == Role.PositiveBlendArm.Addr &&
            Op.Seq == Role.PositiveBlendArm.Seq &&
            sameVar(Op.Output, Role.PositiveBlendArm.Value))
          PositiveAnd = &Op;
        if (Role.HasBaseMaskBlend && Op.Opcode == NdOp::INT_AND &&
            Op.Addr == Role.NegativeBlendArm.Addr &&
            Op.Seq == Role.NegativeBlendArm.Seq &&
            sameVar(Op.Output, Role.NegativeBlendArm.Value))
          NegativeAnd = &Op;
        if (Role.HasBaseMaskBlend && Op.Opcode == NdOp::INT_NEG2 &&
            Op.Addr == Role.PositiveMask.Addr &&
            Op.Seq == Role.PositiveMask.Seq &&
            sameVar(Op.Output, Role.PositiveMask.Value))
          PositiveMask = &Op;
        if (Role.HasBaseMaskBlend && Op.Opcode == NdOp::INT_NOT &&
            Op.Addr == Role.NegativeMask.Addr &&
            Op.Seq == Role.NegativeMask.Seq &&
            sameVar(Op.Output, Role.NegativeMask.Value))
          NegativeMask = &Op;
      }
    }
    if (!Load || Load->NumInputs < 1 || Load->Output.Size != Role.LoadWidth)
      return false;
    const NdVar &LoadAddress =
        Load->Inputs[Load->NumInputs >= 2 ? 1 : 0];
    const JumpTableFrameStorageRole &FrameStorage = Role.FrameStorage;
    const bool HasFrameStorage =
        FrameStorage.RuntimeBase.Use.Value.Size != 0;
    if (HasFrameStorage) {
      if (FrameStorage.RuntimeBase.Use.DefinedAtPoint ||
          FrameStorage.RuntimeBase.Use.Addr == InvalidVA ||
          FrameStorage.RuntimeBase.Use.Seq < 0 ||
          !FrameStorage.CompleteAddress.DefinedAtPoint ||
          FrameStorage.CompleteAddress.Addr == InvalidVA ||
          FrameStorage.CompleteAddress.Seq < 0 ||
          FrameStorage.Initializers.empty() ||
          guestAddressView(FrameStorage.CompleteAddress.Value).Size !=
              guestAddressView(LoadAddress).Size)
        return false;
      for (const JumpTableFrameInitializerChunk &Initializer :
           FrameStorage.Initializers) {
        if (!consumeWork())
          return false;
        if (Initializer.Destination.Use.Value.Size == 0 ||
            Initializer.Destination.Use.DefinedAtPoint ||
            Initializer.Destination.Use.Addr == InvalidVA ||
            Initializer.Destination.Use.Seq < 0 ||
            Initializer.Writer.Addr == InvalidVA ||
            Initializer.Writer.Seq < 0 || Initializer.ByteCount == 0)
          return false;
      }
    } else if (FrameStorage.CompleteAddress.Value.Size != 0 ||
               !FrameStorage.Initializers.empty()) {
      return false;
    }
    if (Role.HasBaseSelect && Role.HasBaseMaskBlend)
      return false;
    if (Role.HasBaseSelect) {
      if (!consumeWork(Role.AllowedBases.size()) ||
          !consumeWork(Role.AllowedBases.size()))
        return false;
      if (!Select || Select->NumInputs < 3 ||
          !Role.SelectedBase.DefinedAtPoint ||
          Role.SelectCondition.DefinedAtPoint ||
          Role.SelectCondition.Addr != Select->Addr ||
          Role.SelectCondition.Seq != Select->Seq ||
          !sameVar(Role.SelectCondition.Value, Select->Inputs[0]) ||
          Role.TrueBase == Role.FalseBase ||
          std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                    Role.TrueBase) == Role.AllowedBases.end() ||
          std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                    Role.FalseBase) == Role.AllowedBases.end())
        return false;
    } else if (Role.HasBaseMaskBlend) {
      if (!Blend || !PositiveAnd || !NegativeAnd || !PositiveMask ||
          !NegativeMask || Blend->NumInputs < 2 || PositiveAnd->NumInputs < 2 ||
          NegativeAnd->NumInputs < 2 || PositiveMask->NumInputs < 1 ||
          NegativeMask->NumInputs < 1 || Role.PositiveBlendInputSide > 1 ||
          Role.PositiveBaseInputSide > 1 || Role.NegativeBaseInputSide > 1 ||
          !Role.SelectedBase.DefinedAtPoint ||
          !Role.PositiveBlendArm.DefinedAtPoint ||
          !Role.NegativeBlendArm.DefinedAtPoint ||
          !Role.PositiveMask.DefinedAtPoint ||
          !Role.NegativeMask.DefinedAtPoint ||
          Role.SelectCondition.DefinedAtPoint ||
          Role.SelectCondition.Addr != PositiveMask->Addr ||
          Role.SelectCondition.Seq != PositiveMask->Seq ||
          !sameVar(Role.SelectCondition.Value, PositiveMask->Inputs[0]) ||
          Role.TrueBase == Role.FalseBase)
        return false;
    }
    if ((Role.HasBaseSelect || Role.HasBaseMaskBlend) && Info.TwoTableSelect &&
        Info.TwoTableHiPositive != (Role.TrueBase > Role.FalseBase))
      return false;
    if (!ensureAppendCapacity(Roles) || !consumeWork(5))
      return false;
    Roles.push_back({&Role,
                     Load,
                     Select,
                     Blend,
                     PositiveAnd,
                     NegativeAnd,
                     PositiveMask,
                     NegativeMask,
                     {},
                     {}});
  }

  auto baseAlternatives = [&](const std::vector<va_t> &Bases, uint16_t Size)
      -> std::optional<std::vector<JumpTableValueOccurrence>> {
    std::vector<JumpTableValueOccurrence> Alternatives;
    // One pass inspects the authenticated owners and one retained occurrence
    // is allocated for each of them.
    if (!consumeProduct(Bases.size(), 3) || !consumeWork(2))
      return std::nullopt;
    Alternatives.reserve(Bases.size());
    for (va_t Base : Bases)
      Alternatives.push_back({NdVar::address(Base, Size), InvalidVA, -1,
                              /*DefinedAtPoint=*/false});
    return Alternatives;
  };
  auto pushBaseQuery =
      [&](std::vector<JumpTableValueQuery> &Queries, const NdVar &Candidate,
          const LowOp &Use, const std::vector<va_t> &Bases, uint16_t Size,
          bool AllowZeroExtension = false,
          bool AllowSignExtension = false) -> std::optional<size_t> {
    auto Alternatives = baseAlternatives(Bases, Size);
    if (!Alternatives)
      return std::nullopt;
    return pushQuery(Queries, Candidate, Use, *Alternatives,
                     AllowZeroExtension, AllowSignExtension);
  };
  auto pushSingleBaseQuery =
      [&](std::vector<JumpTableValueQuery> &Queries, const NdVar &Candidate,
          const LowOp &Use, va_t Base, uint16_t Size) {
    const JumpTableValueOccurrence Alternative{
        NdVar::address(Base, Size), InvalidVA, -1,
        /*DefinedAtPoint=*/false};
    return pushQuery(
        Queries, Candidate, Use,
        llvm::ArrayRef<JumpTableValueOccurrence>(Alternative));
  };
  auto pushIndex = [&](std::vector<size_t> &Indices, size_t Index) {
    if (!ensureAppendCapacity(Indices) || !consumeWork())
      return false;
    Indices.push_back(Index);
    return true;
  };

  // Phase 1: authenticate every scaled-index occurrence once.  The previous
  // implementation paired every ADD with every scale op separately for every
  // LOAD site, which both multiplied graph queries and rejected ordinary
  // shared computed-goto dispatches by exhausting the pair budget.  Here a
  // scale definition is proved independently, then reused as an allowed value
  // occurrence by all address expressions for that role.
  struct ScaleProof {
    size_t RoleIndex = 0;
    const LowOp *Scale = nullptr;
    size_t QueryIndex = 0;
  };
  std::vector<JumpTableValueQuery> ScaleQueries;
  std::vector<ScaleProof> ScaleProofs;
  if (!consumeWork(4))
    return false;
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    if (!consumeWork())
      return false;
    RoleState &State = Roles[RoleIndex];
    const JumpTableLoadRole &Role = *State.Role;
    if (Role.AddressScale == 1) {
      if (!consumeProduct(Role.Indices.size(), 3))
        return false;
      State.DynamicAlternatives.reserve(Role.Indices.size());
      State.DynamicAlternatives.insert(State.DynamicAlternatives.end(),
                                       Role.Indices.begin(),
                                       Role.Indices.end());
      continue;
    }
    if (!chargeInsnInventoryScan())
      return false;
    for (const auto &[Addr, Insn] : Insns) {
      (void)Addr;
      if (Insn.IsInstructionGuard)
        continue;
      for (const LowOp &Scale : Insn.Ops) {
        if ((!Scale.Output.isReg() && !Scale.Output.isTemp()) ||
            Scale.NumInputs < 2 ||
            (Scale.Opcode != NdOp::INT_MULT && Scale.Opcode != NdOp::INT_LEFT))
          continue;
        int IndexSide = -1;
        if (Scale.Opcode == NdOp::INT_MULT) {
          if (Scale.Inputs[0].isConst() &&
              Scale.Inputs[0].Offset == Role.AddressScale)
            IndexSide = 1;
          else if (Scale.Inputs[1].isConst() &&
                   Scale.Inputs[1].Offset == Role.AddressScale)
            IndexSide = 0;
        } else if (Scale.Inputs[1].isConst() && Scale.Inputs[1].Offset < 64 &&
                   (uint64_t{1} << Scale.Inputs[1].Offset) ==
                       Role.AddressScale) {
          IndexSide = 0;
        }
        if (IndexSide < 0)
          continue;
        const NdVar IndexValue = guestAddressView(Scale.Inputs[IndexSide]);
        if (!consumeWork(Role.Indices.size()))
          return false;
        const bool IsExactRecordedUse = std::any_of(
            Role.Indices.begin(), Role.Indices.end(),
            [&](const JumpTableValueOccurrence &Index) {
              return !Index.DefinedAtPoint && Index.Addr == Scale.Addr &&
                     Index.Seq == Scale.Seq &&
                     sameVar(guestAddressView(Index.Value), IndexValue);
            });
        if (IsExactRecordedUse) {
          if (!ensureAppendCapacity(State.DynamicAlternatives) ||
              !consumeWork())
            return false;
          State.DynamicAlternatives.push_back({guestAddressView(Scale.Output),
                                               Scale.Addr, Scale.Seq,
                                               /*DefinedAtPoint=*/true});
          continue;
        }
        auto Query =
            pushQuery(ScaleQueries, IndexValue, Scale, Role.Indices,
                      Role.AllowZeroExtension, Role.AllowSignExtension);
        if (!Query)
          return false;
        if (!ensureAppendCapacity(ScaleProofs) || !consumeWork())
          return false;
        ScaleProofs.push_back({RoleIndex, &Scale, *Query});
      }
    }
  }
  if (!ScaleQueries.empty()) {
    bool ScaleQueriesComplete = true;
    const std::vector<bool> Results = tableValuesMatchAtUses(
        ScaleQueries, &ScaleQueriesComplete, nullptr, InvalidVA, nullptr,
        AggregateEvidenceBudget,
        limits::kMaxJumpTableRoleMatchEvidenceWork);
    if (!ScaleQueriesComplete || Results.size() != ScaleQueries.size()) {
      Complete = false;
      return false;
    }
    for (const ScaleProof &Proof : ScaleProofs) {
      if (!consumeWork())
        return false;
      if (Proof.QueryIndex < Results.size() && Results[Proof.QueryIndex]) {
        if (!ensureAppendCapacity(
                Roles[Proof.RoleIndex].DynamicAlternatives) ||
            !consumeWork())
          return false;
        Roles[Proof.RoleIndex].DynamicAlternatives.push_back(
            {guestAddressView(Proof.Scale->Output), Proof.Scale->Addr,
             Proof.Scale->Seq, /*DefinedAtPoint=*/true});
      }
    }
  }
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    if (!consumeWork())
      return false;
    const RoleState &State = Roles[RoleIndex];
    if (State.DynamicAlternatives.empty()) {
      return false;
    }
  }

  // Phase 2: authenticate complete base-plus-index address definitions.  Each
  // candidate ADD carries two exact value proofs; no physical-register or
  // lexical-nearest definition is trusted.
  struct AddressProof {
    size_t RoleIndex = 0;
    const LowOp *Add = nullptr;
    JumpTableValueOccurrence DynamicIndex;
    std::vector<size_t> QueryIndices;
  };
  auto localCopyChainMatchesUse = [&](const JumpTableValueOccurrence &Source,
                                      const NdVar &UseValue, const LowOp &Use) {
    if (!Source.DefinedAtPoint || Source.Addr != Use.Addr || Source.Seq < 0 ||
        Source.Seq >= Use.Seq)
      return false;
    if (!consumeWork(orderedSetLookupWork(Insns.size())))
      return false;
    auto InsnIt = Insns.find(Use.Addr);
    if (InsnIt == Insns.end() || InsnIt->second.IsInstructionGuard)
      return false;
    if (!consumeWork() || !consumeWork(InsnIt->second.Ops.size()))
      return false;
    std::vector<NdVar> Equivalent;
    if (!consumeWork(2))
      return false;
    if (!ensureAppendCapacity(Equivalent) || !consumeWork())
      return false;
    Equivalent.push_back(guestAddressView(Source.Value));
    auto overlaps = [](const NdVar &A, const NdVar &B) {
      if (A.Space != B.Space || A.Size == 0 || B.Size == 0)
        return false;
      const uint64_t AEnd = A.Offset + A.Size;
      const uint64_t BEnd = B.Offset + B.Size;
      if (AEnd < A.Offset || BEnd < B.Offset)
        return true;
      return A.Offset < BEnd && B.Offset < AEnd;
    };
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (Op.Seq <= Source.Seq)
        continue;
      if (Op.Seq >= Use.Seq)
        break;
      const NdVar Output = guestAddressView(Op.Output);
      if (!consumeWork(Equivalent.size()))
        return false;
      const bool CopiesEquivalent =
          Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          Output.Size == guestAddressView(Op.Inputs[0]).Size &&
          std::any_of(Equivalent.begin(), Equivalent.end(),
                      [&](const NdVar &Value) {
                        return sameVar(Value, guestAddressView(Op.Inputs[0]));
                      });
      if (!consumeWork(Equivalent.size()))
        return false;
      Equivalent.erase(std::remove_if(Equivalent.begin(), Equivalent.end(),
                                      [&](const NdVar &Value) {
                                        return overlaps(Value, Output);
                                      }),
                       Equivalent.end());
      if (!consumeWork(Equivalent.size()))
        return false;
      if (CopiesEquivalent &&
          std::none_of(Equivalent.begin(), Equivalent.end(),
                       [&](const NdVar &Value) {
                         return sameVar(Value, Output);
                       })) {
        if (!ensureAppendCapacity(Equivalent) || !consumeWork())
          return false;
        Equivalent.push_back(Output);
      }
    }
    const NdVar GuestUse = guestAddressView(UseValue);
    if (!consumeWork(Equivalent.size()))
      return false;
    return std::any_of(
        Equivalent.begin(), Equivalent.end(),
        [&](const NdVar &Value) { return sameVar(Value, GuestUse); });
  };
  std::vector<JumpTableValueQuery> AddressQueries;
  std::vector<AddressProof> AddressProofs;
  if (!consumeWork(4))
    return false;
  auto exactOpAt = [&](va_t Addr, int Seq) -> const LowOp * {
    if (!consumeWork(orderedSetLookupWork(Insns.size())))
      return nullptr;
    auto InsnIt = Insns.find(Addr);
    if (InsnIt == Insns.end() || InsnIt->second.IsInstructionGuard)
      return nullptr;
    const LowOp *Found = nullptr;
    if (!consumeWork(InsnIt->second.Ops.size()))
      return nullptr;
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (Op.Addr != Addr || Op.Seq != Seq)
        continue;
      if (Found)
        return nullptr;
      Found = &Op;
    }
    return Found;
  };
  auto proofRegistersOverlap = [](const NdVar &Left, const NdVar &Right) {
    if (!Left.isReg() || !Right.isReg() || Left.Size == 0 || Right.Size == 0)
      return false;
    const uint64_t LeftEnd = Left.Offset + Left.Size;
    const uint64_t RightEnd = Right.Offset + Right.Size;
    return Left.Offset < RightEnd && Right.Offset < LeftEnd;
  };
  auto proofValueClobbers = [&](const NdVar &Output, const NdVar &Value) {
    return Output.Size != 0 &&
           (Output == Value || proofRegistersOverlap(Output, Value));
  };
  auto exactRelocationFreeAddressProof =
      [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
    if (!CurrentImg ||
        Occurrence.Authority != RelocatedInstructionAddressProofKind::
                                    AArch64RelocationFreeDataDereference)
      return false;
    const uint16_t PointerSize = CurrentImg->getPointerSize();
    if (CurrentImg->Arch != Arch::AArch64 || CurrentImg->IsRelocatable ||
        PointerSize == 0 || PointerSize > sizeof(va_t) ||
        Occurrence.FieldVA != InvalidVA || Occurrence.Width != PointerSize ||
        !Occurrence.DefinesOutput || Occurrence.OutputMayDepend ||
        Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
        Occurrence.TargetVA == InvalidVA ||
        Occurrence.TargetOwnerVA == InvalidVA ||
        Occurrence.ArithmeticProof.empty() ||
        Occurrence.ArithmeticProof.size() > 32)
      return false;
    if (!consumeWork(orderedSetLookupWork(Insns.size())))
      return false;
    const auto RootRec = Insns.find(Occurrence.SeedInstructionAddr);
    const LowOp *Root =
        exactOpAt(Occurrence.SeedInstructionAddr, Occurrence.SeedOpSeq);
    if (RootRec == Insns.end() || !Root || RootRec->second.Ops.size() != 1 ||
        RootRec->second.Size == 0 ||
        RootRec->second.Size > InvalidVA - RootRec->second.Addr ||
        RootRec->second.IsBranch || RootRec->second.IsCall ||
        RootRec->second.IsRet || RootRec->second.IsOpaqueTerminator ||
        RootRec->second.IsResumableTerminator ||
        Root->Opcode != Occurrence.SeedOpcode || Root->Opcode != NdOp::COPY ||
        Root->NumInputs != 1 || Root->Inputs[0] != Occurrence.SeedInputWitness ||
        Root->Output != Occurrence.SeedOutputWitness ||
        !Root->Output.isReg() || Root->Output.Size != PointerSize ||
        !Root->Inputs[0].isConst() || Root->Inputs[0].Size != PointerSize ||
        Root->Inputs[0].Provenance !=
            ConstantAddressProvenance::AddressFragment)
      return false;

    const unsigned PointerBits = static_cast<unsigned>(PointerSize) * 8;
    const uint64_t PointerMask =
        PointerBits == 64 ? std::numeric_limits<uint64_t>::max()
                          : (uint64_t{1} << PointerBits) - 1;
    auto canonicalScalar =
        [&](const LowOp &Op,
            const RelocatedInstructionAddressArithmeticStep &Step)
        -> std::optional<uint64_t> {
      if (!Step.ScalarInputWitness.isConst() ||
          Step.ScalarInputWitness.Provenance !=
              ConstantAddressProvenance::Scalar)
        return std::nullopt;
      if (Step.ScalarInputWitness.Size == PointerSize)
        return Step.ScalarInputWitness.Offset & PointerMask;
      if (PointerSize != 8 || Step.ScalarInputWitness.Size != 4 ||
          Step.BaseInputIndex != 0)
        return std::nullopt;
      if (!consumeWork(CurrentImg->Segments.size()))
        return std::nullopt;
      const uint8_t *Bytes = CurrentImg->readVA(Op.Addr, sizeof(uint32_t));
      if (!Bytes)
        return std::nullopt;
      const uint32_t Word = readLE<uint32_t>(Bytes);
      if ((Word & 0x1f000000u) != 0x11000000u ||
          (Word & 0x80000000u) == 0 || (Word & 0x20000000u) != 0 ||
          (((Word & 0x40000000u) != 0) !=
           (Op.Opcode == NdOp::INT_SUB)))
        return std::nullopt;
      const uint64_t Encoded = uint64_t((Word >> 10) & 0xfffu)
                               << (((Word >> 22) & 1u) ? 12 : 0);
      return Encoded == Step.ScalarInputWitness.Offset
                 ? std::optional<uint64_t>(Encoded)
                 : std::nullopt;
    };
    NdVar Current = Root->Output;
    va_t Address = Root->Inputs[0].Offset;
    va_t ExpectedAddress = RootRec->second.Addr + RootRec->second.Size;
    for (const RelocatedInstructionAddressArithmeticStep &Step :
         Occurrence.ArithmeticProof) {
      if (!consumeWork())
        return false;
      if (!consumeWork(orderedSetLookupWork(Insns.size())))
        return false;
      const auto Rec = Insns.find(Step.InstructionAddr);
      const LowOp *Op = exactOpAt(Step.InstructionAddr, Step.OpSeq);
      if (Rec == Insns.end() || !Op || Rec->second.Addr != ExpectedAddress ||
          Rec->second.Size == 0 ||
          Rec->second.Size > InvalidVA - Rec->second.Addr ||
          Rec->second.IsBranch || Rec->second.IsCall || Rec->second.IsRet ||
          Rec->second.IsOpaqueTerminator ||
          Rec->second.IsResumableTerminator ||
          (Step.Opcode != NdOp::INT_ADD && Step.Opcode != NdOp::INT_SUB) ||
          Op->Opcode != Step.Opcode || Op->NumInputs != 2 ||
          Step.BaseInputIndex > 1 ||
          (Op->Opcode == NdOp::INT_SUB && Step.BaseInputIndex != 0) ||
          Op->Inputs[Step.BaseInputIndex] != Step.BaseInputWitness ||
          Op->Inputs[1 - Step.BaseInputIndex] != Step.ScalarInputWitness ||
          Op->Output != Step.OutputWitness ||
          (!Op->Output.isReg() && !Op->Output.isTemp()) ||
          Op->Output.Size != PointerSize ||
          Step.BaseInputWitness.Size != PointerSize)
        return false;
      if (!consumeWork(orderedSetLookupWork(
              CurrentImg->InstructionAddressMaterializations.size())))
        return false;
      if (CurrentImg->InstructionAddressMaterializations.count(Op->Addr))
        return false;
      if (!consumeWork(Rec->second.Ops.size()))
        return false;

      // AArch64 unsigned-offset memory operations lower as an
      // instruction-local COPY/ADD/LOAD chain. Reconstruct only full-width COPY
      // aliases into the authenticated arithmetic input. P-code may preserve
      // the architectural register while also materializing a bookkeeping
      // temporary, so a COPY does not consume its source. Any unrelated memory
      // effect or alias clobber fails closed.
      std::vector<NdVar> BaseAliases;
      if (!consumeWork(2))
        return false;
      if (!ensureAppendCapacity(BaseAliases) || !consumeWork())
        return false;
      BaseAliases.push_back(Current);
      bool SawArithmetic = false;
      for (const LowOp &Other : Rec->second.Ops) {
        if (&Other == Op) {
          if (!consumeWork(BaseAliases.size()) ||
              std::find(BaseAliases.begin(), BaseAliases.end(),
                        Step.BaseInputWitness) == BaseAliases.end())
            return false;
          SawArithmetic = true;
          continue;
        }
        const LowMemoryOperandView Memory = lowMemoryOperands(Other);
        if (!SawArithmetic) {
          if (Memory.Complete)
            return false;
          if (!consumeWork(BaseAliases.size()))
            return false;
          const bool CopiesAlias =
              Other.Opcode == NdOp::COPY && Other.NumInputs == 1 &&
              (Other.Output.isReg() || Other.Output.isTemp()) &&
              Other.Output.Size == PointerSize &&
              std::find(BaseAliases.begin(), BaseAliases.end(),
                        Other.Inputs[0]) != BaseAliases.end();
          if (CopiesAlias) {
            if (!consumeWork(BaseAliases.size()))
              return false;
            if (std::find(BaseAliases.begin(), BaseAliases.end(),
                          Other.Output) == BaseAliases.end()) {
              if (!ensureAppendCapacity(BaseAliases) || !consumeWork())
                return false;
              BaseAliases.push_back(Other.Output);
            }
            continue;
          }
          if (!consumeWork(BaseAliases.size()) ||
              std::any_of(BaseAliases.begin(), BaseAliases.end(),
                          [&](const NdVar &Alias) {
                            return proofValueClobbers(Other.Output, Alias);
                          }) ||
              proofValueClobbers(Other.Output, Op->Output))
            return false;
          continue;
        }

        const bool SameRecordFinalDereference =
            &Step == &Occurrence.ArithmeticProof.back() &&
            Occurrence.DereferenceInstructionAddr == Step.InstructionAddr;
        if (!SameRecordFinalDereference &&
            (Memory.Complete || proofValueClobbers(Other.Output, Current) ||
             proofValueClobbers(Other.Output, Op->Output)))
          return false;
      }
      if (!SawArithmetic)
        return false;
      const std::optional<uint64_t> Delta = canonicalScalar(*Op, Step);
      if (!Delta || Address > PointerMask ||
          (Op->Opcode == NdOp::INT_ADD && *Delta > PointerMask - Address) ||
          (Op->Opcode == NdOp::INT_SUB && *Delta > Address))
        return false;
      Address = Op->Opcode == NdOp::INT_ADD ? Address + *Delta
                                            : Address - *Delta;
      Current = Op->Output;
      ExpectedAddress =
          (&Step == &Occurrence.ArithmeticProof.back() &&
           Occurrence.DereferenceInstructionAddr == Step.InstructionAddr)
              ? Rec->second.Addr
              : Rec->second.Addr + Rec->second.Size;
    }
    const RelocatedInstructionAddressArithmeticStep &Final =
        Occurrence.ArithmeticProof.back();
    if (Occurrence.InstructionAddr != Final.InstructionAddr ||
        Occurrence.OpSeq != Final.OpSeq ||
        Occurrence.OutputOpcode != Final.Opcode ||
        Occurrence.OutputWitness != Final.OutputWitness ||
        Occurrence.TargetVA != Address)
      return false;

    if (!consumeWork(orderedSetLookupWork(Insns.size())))
      return false;
    const auto DereferenceRec =
        Insns.find(Occurrence.DereferenceInstructionAddr);
    const LowOp *Dereference = exactOpAt(
        Occurrence.DereferenceInstructionAddr, Occurrence.DereferenceOpSeq);
    if (DereferenceRec == Insns.end() || !Dereference ||
        DereferenceRec->second.Addr != ExpectedAddress ||
        DereferenceRec->second.IsBranch || DereferenceRec->second.IsCall ||
        DereferenceRec->second.IsRet ||
        DereferenceRec->second.IsOpaqueTerminator ||
        DereferenceRec->second.IsResumableTerminator ||
        Dereference->Opcode != Occurrence.DereferenceOpcode ||
        (Dereference->Opcode != NdOp::LOAD &&
         Dereference->Opcode != NdOp::STORE))
      return false;
    NdVar DereferenceAddress = Current;
    bool SawDereference = false;
    bool SawFinalArithmetic =
        DereferenceRec->second.Addr != Final.InstructionAddr;
    if (!consumeWork(DereferenceRec->second.Ops.size()))
      return false;
    for (const LowOp &Op : DereferenceRec->second.Ops) {
      if (!SawFinalArithmetic) {
        if (Op.Addr == Final.InstructionAddr && Op.Seq == Final.OpSeq) {
          SawFinalArithmetic = true;
          continue;
        }
        continue;
      }
      const LowMemoryOperandView CandidateMemory = lowMemoryOperands(Op);
      if (&Op == Dereference) {
        if (!CandidateMemory.Complete || !CandidateMemory.Address ||
            *CandidateMemory.Address != DereferenceAddress || SawDereference ||
            proofValueClobbers(Op.Output, Current))
          return false;
        SawDereference = true;
        continue;
      }
      if (CandidateMemory.Complete || proofValueClobbers(Op.Output, Current))
        return false;
      if (!SawDereference) {
        if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
            Op.Inputs[0] != DereferenceAddress ||
            (!Op.Output.isReg() && !Op.Output.isTemp()) ||
            Op.Output.Size != PointerSize ||
            (Op.Output.isReg() &&
             proofRegistersOverlap(Op.Output, Current)))
          return false;
        DereferenceAddress = Op.Output;
      }
    }
    const LowMemoryOperandView Memory = lowMemoryOperands(*Dereference);
    if (!SawFinalArithmetic || !SawDereference || !Memory.Complete ||
        !Memory.Address ||
        DereferenceAddress != Occurrence.DereferenceAddressWitness ||
        *Memory.Address != DereferenceAddress ||
        Memory.AccessSize != Occurrence.DereferenceAccessSize ||
        Memory.AccessSize == 0 ||
        Memory.AccessSize - 1 > InvalidVA - Address)
      return false;
    const va_t Last = Address + Memory.AccessSize - 1;
    if (!consumeObjectDataQuery() ||
        !CurrentImg->hasObjectDataProvenance(Address) ||
        !consumeObjectDataQuery() ||
        !CurrentImg->hasObjectDataProvenance(Last) ||
        !consumeExecutableOwnerQuery() ||
        CurrentImg->hasExecutableCodeOwnerAt(Address) ||
        !consumeExecutableOwnerQuery() ||
        CurrentImg->hasExecutableCodeOwnerAt(Last) ||
        !consumeProduct(CurrentImg->Segments.size(), 2) ||
        !consumeWork(CurrentImg->Sections.size()) ||
        isRuntimeWritableAddress(*CurrentImg, Address) ||
        !consumeProduct(CurrentImg->Segments.size(), 2) ||
        !consumeWork(CurrentImg->Sections.size()) ||
        isRuntimeWritableAddress(*CurrentImg, Last) ||
        !consumeRelocatedOwnerQuery() ||
        !CurrentImg->relocatedTargetBelongsToOwner(
            Address, Occurrence.TargetOwnerVA))
      return false;
    if (!consumeProduct(CurrentImg->Segments.size(), 2) ||
        !consumeProduct(CurrentImg->Sections.size(), 2))
      return false;
    const Section *StartSection = CurrentImg->getSectionFor(Address);
    const Section *LastSection = CurrentImg->getSectionFor(Last);
    if (StartSection || LastSection)
      return StartSection && StartSection == LastSection &&
             StartSection->VA == Occurrence.TargetOwnerVA;
    if (!consumeProduct(CurrentImg->Segments.size(), 2))
      return false;
    const Segment *StartSegment = CurrentImg->getSegmentFor(Address);
    const Segment *LastSegment = CurrentImg->getSegmentFor(Last);
    return StartSegment && StartSegment == LastSegment &&
           StartSegment->VA == Occurrence.TargetOwnerVA;
  };
  auto exactAddressProducer =
      [&](const JumpTableValueOccurrence &Producer, va_t FieldVA,
          va_t ProducerTargetVA, va_t StaticAddress,
          ConstantAddressProvenance Provenance, va_t OwnerVA) {
        if (Provenance != ConstantAddressProvenance::DataAddress ||
            OwnerVA == InvalidVA ||
            ProducerTargetVA == InvalidVA || StaticAddress == InvalidVA ||
            Producer.Addr == InvalidVA ||
            Producer.Seq < 0)
          return false;
        if (!consumeRelocatedOwnerQuery() ||
            !CurrentImg->relocatedTargetBelongsToOwner(ProducerTargetVA,
                                                       OwnerVA) ||
            !consumeRelocatedOwnerQuery() ||
            !CurrentImg->relocatedTargetBelongsToOwner(StaticAddress, OwnerVA))
          return false;
        const LowOp *Op = exactOpAt(Producer.Addr, Producer.Seq);
        if (!Op)
          return false;
        const RelocatedInstructionAddressOccurrence *Exact = nullptr;
        if (!consumeWork(RelocatedInstructionAddressOccurrences.size()))
          return false;
        for (const RelocatedInstructionAddressOccurrence &Occurrence :
             RelocatedInstructionAddressOccurrences) {
          if (Occurrence.InstructionAddr != Producer.Addr ||
              Occurrence.OpSeq != Producer.Seq ||
              Occurrence.FieldVA != FieldVA ||
              Occurrence.TargetVA != ProducerTargetVA ||
              Occurrence.TargetOwnerVA != OwnerVA ||
              Occurrence.Provenance != Provenance ||
              Occurrence.OutputMayDepend)
            continue;
          const bool LoaderAuthority =
              Occurrence.Authority ==
                  RelocatedInstructionAddressProofKind::LoaderField &&
              FieldVA != InvalidVA;
          const bool RelocationFreeAuthority =
              FieldVA == InvalidVA && exactRelocationFreeAddressProof(Occurrence);
          if (!LoaderAuthority && !RelocationFreeAuthority)
            continue;
          bool Matches = false;
          if (Occurrence.DefinesOutput) {
            Matches = Producer.DefinedAtPoint &&
                      Op->Opcode == Occurrence.OutputOpcode &&
                      Op->Output == Occurrence.OutputWitness &&
                      Producer.Value == Op->Output;
          } else if (!Producer.DefinedAtPoint &&
                     Occurrence.InputIndex >= 0 &&
                     Occurrence.InputIndex < Op->NumInputs) {
            Matches = Op->Inputs[Occurrence.InputIndex] == Producer.Value &&
                      Producer.Value.isConst() &&
                      Producer.Value.Offset == ProducerTargetVA &&
                      Producer.Value.Provenance == Provenance &&
                      Producer.Value.AddressOwnerVA == OwnerVA;
          }
          if (!Matches)
            continue;
          if (Exact)
            return false;
          Exact = &Occurrence;
        }
        return Exact != nullptr;
      };
  auto exactScalarProducer = [&](const JumpTableValueOccurrence &Producer,
                                 uint64_t Expected) {
    if (Producer.Addr == InvalidVA || Producer.Seq < 0)
      return false;
    const LowOp *Op = exactOpAt(Producer.Addr, Producer.Seq);
    if (!Op)
      return false;
    const NdVar *Literal = nullptr;
    int LiteralInput = -1;
    if (Producer.DefinedAtPoint) {
      if (Op->Opcode != NdOp::COPY || Op->NumInputs < 1 ||
          Op->Output != Producer.Value)
        return false;
      Literal = &Op->Inputs[0];
      LiteralInput = 0;
    } else {
      if (!consumeWork(Op->NumInputs))
        return false;
      for (int Input = 0; Input < Op->NumInputs; ++Input)
        if (Op->Inputs[Input] == Producer.Value) {
          if (Literal)
            return false;
          Literal = &Op->Inputs[Input];
          LiteralInput = Input;
        }
    }
    if (!Literal || !Literal->isConst() || Literal->Size == 0 ||
        Literal->Size > sizeof(uint64_t) ||
        (Literal->Provenance != ConstantAddressProvenance::Unknown &&
         Literal->Provenance != ConstantAddressProvenance::Scalar))
      return false;
    const unsigned Bits = static_cast<unsigned>(Literal->Size) * 8;
    const uint64_t Mask = Bits == 64 ? std::numeric_limits<uint64_t>::max()
                                     : (uint64_t{1} << Bits) - 1;
    if ((Literal->Offset & Mask) != Expected)
      return false;
    if (!consumeWork(RelocatedInstructionAddressOccurrences.size()))
      return false;
    return std::none_of(
        RelocatedInstructionAddressOccurrences.begin(),
        RelocatedInstructionAddressOccurrences.end(),
        [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
          return Occurrence.InstructionAddr == Op->Addr &&
                 Occurrence.OpSeq == Op->Seq &&
                 (Occurrence.DefinesOutput ||
                  Occurrence.InputIndex == LiteralInput);
        });
  };
  auto isAuthenticatedMemcpyCall = [&](const LowOp &Op) {
    if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
        !Op.Inputs[0].isConst())
      return false;
    const va_t Target = static_cast<va_t>(Op.Inputs[0].Offset);
    if (!consumeWork(CurrentImg->Imports.size()) ||
        !consumeWork(
            orderedSetLookupWork(CurrentImg->ImportStubIndices.size())))
      return false;
    if (const Import *Imp = CurrentImg->findImportAt(Target)) {
      if (!consumeWork(Imp->Name.size()))
        return false;
      if (libc::isMemCopyName(Imp->Name))
        return true;
    }
    if (!consumeWork(CurrentImg->Symbols.size()))
      return false;
    if (const Symbol *Sym = CurrentImg->findSymbolAt(Target)) {
      if (!consumeWork(Sym->Name.size()))
        return false;
      if (libc::isMemCopyName(Sym->Name))
        return true;
    }
    if (!consumeWork(CurrentImg->Relocations.size()))
      return false;
    for (const RelocationEntry &Relocation : CurrentImg->Relocations) {
      if ((Relocation.Address != Op.Addr &&
           (Op.Addr == InvalidVA || Relocation.Address != Op.Addr + 1)) ||
          Relocation.SymbolName.empty())
        continue;
      if (!consumeWork(Relocation.SymbolName.size()))
        return false;
      if (libc::isMemCopyName(Relocation.SymbolName))
        return true;
    }
    return false;
  };
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    if (!consumeWork())
      return false;
    RoleState &State = Roles[RoleIndex];
    const JumpTableLoadRole &Role = *State.Role;
    const JumpTableFrameStorageRole &FrameStorage = Role.FrameStorage;
    if (FrameStorage.RuntimeBase.Use.Value.Size != 0) {
      const JumpTableValueOccurrence &RuntimeUse =
          FrameStorage.RuntimeBase.Use;
      const LowOp *RuntimeAdd = exactOpAt(RuntimeUse.Addr, RuntimeUse.Seq);
      const JumpTableValueOccurrence &CompleteAddress =
          FrameStorage.CompleteAddress;
      const LowOp *CompleteAdd =
          exactOpAt(CompleteAddress.Addr, CompleteAddress.Seq);
      if (!RuntimeAdd || !CompleteAdd ||
          RuntimeAdd->Opcode != NdOp::INT_ADD || RuntimeAdd->NumInputs < 2 ||
          !sameVar(guestAddressView(CompleteAdd->Output),
                   guestAddressView(CompleteAddress.Value)))
        return false;

      int BaseSide = -1;
      for (int Side = 0; Side < 2; ++Side)
        if (sameVar(guestAddressView(RuntimeAdd->Inputs[Side]),
                    guestAddressView(RuntimeUse.Value))) {
          if (BaseSide >= 0)
            return false;
          BaseSide = Side;
        }
      if (BaseSide < 0)
        return false;
      const NdVar DynamicValue =
          guestAddressView(RuntimeAdd->Inputs[1 - BaseSide]);
      if (!consumeWork(State.DynamicAlternatives.size()))
        return false;
      const bool LocallyAuthenticatedIndex = std::any_of(
          State.DynamicAlternatives.begin(), State.DynamicAlternatives.end(),
          [&](const JumpTableValueOccurrence &Alternative) {
            return localCopyChainMatchesUse(Alternative, DynamicValue,
                                            *RuntimeAdd);
          });
      std::vector<size_t> ProofQueries;
      if (!consumeWork(2))
        return false;
      if (!LocallyAuthenticatedIndex) {
        auto IndexQuery =
            pushQuery(AddressQueries, DynamicValue, *RuntimeAdd,
                      State.DynamicAlternatives);
        if (!IndexQuery)
          return false;
        if (!pushIndex(ProofQueries, *IndexQuery))
          return false;
      }

      const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
      const llvm::ArrayRef<uint64_t> IntParamRegs =
          TRI.integerParamRegs(CurrentImg->Format);
      const uint16_t PointerSize = CurrentImg->getPointerSize();
      std::vector<JumpTableValueOccurrence> StoreWriters;
      std::vector<JumpTableValueOccurrence> MemcpyWriters;
      if (!consumeWork(4))
        return false;
      size_t SourcePieceCount = 0;
      for (const JumpTableFrameInitializerChunk &Initializer :
           FrameStorage.Initializers) {
        if (!consumeWork())
          return false;
        const JumpTableFrameAddressUse &Destination =
            Initializer.Destination;
        const LowOp *DestinationUse =
            exactOpAt(Destination.Use.Addr, Destination.Use.Seq);
        if (!DestinationUse)
          return false;
        bool ExplicitUse = false;
        if (!consumeWork(DestinationUse->NumInputs))
          return false;
        for (int InputIndex = 0;
             InputIndex < DestinationUse->NumInputs; ++InputIndex)
          ExplicitUse |= sameVar(
              guestAddressView(DestinationUse->Inputs[InputIndex]),
              guestAddressView(Destination.Use.Value));
        const bool ImplicitFirstCallArgument =
            DestinationUse->Opcode == NdOp::CALL &&
            Destination.Use.Value.isReg() && !IntParamRegs.empty() &&
            Destination.Use.Value.Offset == IntParamRegs.front();
        if (!ExplicitUse && !ImplicitFirstCallArgument)
          return false;
        auto FrameQuery = pushQuery(
            AddressQueries, guestAddressView(RuntimeUse.Value), *RuntimeAdd,
            llvm::ArrayRef<JumpTableValueOccurrence>(Destination.Use));
        if (!FrameQuery)
          return false;
        JumpTableValueQuery &Query = AddressQueries[*FrameQuery];
        Query.Relation =
            JumpTableValueRelation::SameCanonicalFrameAddress;
        Query.FrameByteAddend = FrameStorage.RuntimeBase.ByteAddend;
        Query.AlternativeFrameByteAddend = Destination.ByteAddend;
        if (!pushIndex(ProofQueries, *FrameQuery))
          return false;

        const LowOp *Writer =
            exactOpAt(Initializer.Writer.Addr, Initializer.Writer.Seq);
        if (!Writer)
          return false;
        if (Initializer.IsMemcpy) {
          if (!isAuthenticatedMemcpyCall(*Writer) ||
              Initializer.StoredValue.Value.Size != 0 ||
              Initializer.SourceAddress.Value.Size == 0 ||
              Initializer.Length.Value.Size == 0 ||
              Initializer.Length.Value.Size > sizeof(uint64_t) ||
              !exactScalarProducer(Initializer.LengthProducer,
                                   Initializer.ByteCount) ||
              Initializer.StaticSourceAddress == InvalidVA ||
              !exactAddressProducer(
                  Initializer.StaticSourceProducer,
                  Initializer.StaticSourceFieldVA,
                  Initializer.StaticSourceProducerTargetVA,
                  Initializer.StaticSourceAddress,
                  Initializer.StaticSourceProvenance,
                  Initializer.StaticSourceOwnerVA) ||
              !Initializer.StaticSources.empty())
            return false;
          if (!consumeImmutableDataSpanQuery() ||
              !exactImmutableDataSpanOwner(
                  *CurrentImg, Initializer.StaticSourceAddress,
                  Initializer.ByteCount, Initializer.StaticSourceOwnerVA))
            return false;
          const unsigned LengthBits =
              static_cast<unsigned>(Initializer.Length.Value.Size) * 8;
          if (LengthBits < 64 &&
              Initializer.ByteCount >= (uint64_t{1} << LengthBits))
            return false;
          const LowOp *SourceUse = exactOpAt(
              Initializer.SourceAddress.Addr,
              Initializer.SourceAddress.Seq);
          const LowOp *LengthUse =
              exactOpAt(Initializer.Length.Addr, Initializer.Length.Seq);
          if (!SourceUse || !LengthUse)
            return false;
          const JumpTableValueOccurrence SourceAlternative{
              NdVar::dataAddress(Initializer.StaticSourceAddress,
                                 Initializer.SourceAddress.Value.Size,
                                 Initializer.StaticSourceOwnerVA),
              InvalidVA, -1, /*DefinedAtPoint=*/false};
          auto SourceQuery = pushQuery(
              AddressQueries, Initializer.SourceAddress.Value, *SourceUse,
              llvm::ArrayRef<JumpTableValueOccurrence>(SourceAlternative));
          auto LengthQuery = pushQuery(
              AddressQueries, Initializer.Length.Value, *LengthUse,
              llvm::ArrayRef<JumpTableValueOccurrence>(
                  Initializer.LengthProducer),
              /*AllowZeroExtension=*/true);
          if (!SourceQuery || !LengthQuery)
            return false;
          AddressQueries[*SourceQuery].RequireExactAddressOwner = true;
          // Register ABIs expose the memcpy operands as implicit register uses
          // at the exact CALL point.  A stack ABI instead records the STORE
          // value uses; bind all three outgoing cells to this CALL's current
          // SP epoch with all-path reaching-memory equality.
          const bool RegisterArguments =
              Destination.Use.Addr == Writer->Addr &&
              Destination.Use.Seq == Writer->Seq &&
              Initializer.SourceAddress.Addr == Writer->Addr &&
              Initializer.SourceAddress.Seq == Writer->Seq &&
              Initializer.Length.Addr == Writer->Addr &&
              Initializer.Length.Seq == Writer->Seq;
          if (RegisterArguments) {
            if (IntParamRegs.size() < 3 || !Destination.Use.Value.isReg() ||
                !Initializer.SourceAddress.Value.isReg() ||
                !Initializer.Length.Value.isReg() ||
                Destination.Use.Value.Offset != IntParamRegs[0] ||
                Initializer.SourceAddress.Value.Offset != IntParamRegs[1] ||
                Initializer.Length.Value.Offset != IntParamRegs[2])
              return false;
          } else {
            if (CurrentImg->Arch != Arch::X86 ||
                DestinationUse->Opcode != NdOp::STORE ||
                SourceUse->Opcode != NdOp::STORE ||
                LengthUse->Opcode != NdOp::STORE)
              return false;
            if (PointerSize == 0 || PointerSize > sizeof(uint64_t) ||
                !TRI.isStackPointer(TRI.StackPointer))
              return false;
            const std::array<JumpTableValueOccurrence, 3> Arguments = {
                Destination.Use, Initializer.SourceAddress,
                Initializer.Length};
            for (size_t ArgumentIndex = 0;
                 ArgumentIndex < Arguments.size(); ++ArgumentIndex) {
              if (ArgumentIndex >
                  static_cast<size_t>(
                      std::numeric_limits<int64_t>::max()) /
                      PointerSize)
                return false;
              auto ArgumentQuery = pushQuery(
                  AddressQueries,
                  NdVar::reg(TRI.StackPointer, PointerSize), *Writer,
                  llvm::ArrayRef<JumpTableValueOccurrence>(
                      Arguments[ArgumentIndex]));
              if (!ArgumentQuery)
                return false;
              JumpTableValueQuery &Arg = AddressQueries[*ArgumentQuery];
              Arg.Relation = JumpTableValueRelation::FrameMemoryMatches;
              Arg.FrameByteAddend = static_cast<int64_t>(ArgumentIndex) *
                                     static_cast<int64_t>(PointerSize);
              Arg.FrameMemorySize = PointerSize;
              if (!consumeWork(5))
                return false;
              Arg.AlternativeFrameValueOffsets = {0};
              if (!pushIndex(ProofQueries, *ArgumentQuery))
                return false;
            }
          }
          if (!pushIndex(ProofQueries, *SourceQuery) ||
              !pushIndex(ProofQueries, *LengthQuery))
            return false;
          if (!ensureAppendCapacity(MemcpyWriters) || !consumeWork())
            return false;
          MemcpyWriters.push_back(Initializer.Writer);
          continue;
        }

        if (Writer->Opcode != NdOp::STORE || Writer->NumInputs < 2 ||
            Initializer.StoredValue.Value.Size == 0 ||
            Initializer.ByteCount != Initializer.StoredValue.Value.Size ||
            !sameVar(Initializer.Destination.Use.Value, Writer->Inputs[0]) ||
            !sameVar(Initializer.StoredValue.Value, Writer->Inputs[1]) ||
            Initializer.StaticSourceAddress == InvalidVA ||
            Initializer.StaticSources.empty())
          return false;
        if (SourcePieceCount > MaxProofQueries ||
            Initializer.StaticSources.size() >
                MaxProofQueries - SourcePieceCount) {
          Complete = false;
          return false;
        }
        SourcePieceCount += Initializer.StaticSources.size();
        std::vector<std::pair<va_t, va_t>> SourceRanges;
        if (!consumeWork(2))
          return false;
        for (const auto &Source : Initializer.StaticSources) {
          if (!consumeWork())
            return false;
          const LowOp *SourceLoad = exactOpAt(Source.Value.Addr,
                                              Source.Value.Seq);
          const bool ExactSourceProducer = exactAddressProducer(
              Source.StaticAddressProducer, Source.StaticAddressFieldVA,
              Source.StaticAddressProducerTargetVA, Source.StaticAddress,
              Source.StaticAddressProvenance,
              Source.StaticAddressOwnerVA);
          if (!SourceLoad || SourceLoad->Opcode != NdOp::LOAD ||
              SourceLoad->NumInputs < 1 ||
              !sameVar(SourceLoad->Output, Source.Value.Value) ||
              Source.ByteCount == 0 ||
              Source.ByteCount != Source.Value.Value.Size ||
              Source.StaticAddress == InvalidVA ||
              Source.StaticAddressProvenance !=
                  ConstantAddressProvenance::DataAddress ||
              Source.StaticAddressOwnerVA == InvalidVA ||
              !ExactSourceProducer ||
              Source.StaticAddress > InvalidVA - Source.ByteCount)
            return false;
          if (!consumeImmutableDataSpanQuery() ||
              !exactImmutableDataSpanOwner(
                  *CurrentImg, Source.StaticAddress, Source.ByteCount,
                  Source.StaticAddressOwnerVA) ||
              !consumeRelocatedOwnerQuery() ||
              !CurrentImg->relocatedTargetBelongsToOwner(
                  Source.StaticAddress, Source.StaticAddressOwnerVA))
            return false;
          const NdVar &SourceAddress =
              SourceLoad->Inputs[SourceLoad->NumInputs >= 2 ? 1 : 0];
          if (!sameVar(SourceAddress, Source.Address.Value) ||
              Source.Address.DefinedAtPoint ||
              Source.Address.Addr != SourceLoad->Addr ||
              Source.Address.Seq != SourceLoad->Seq)
            return false;
          const JumpTableValueOccurrence SourceAddressAlternative{
              NdVar::dataAddress(Source.StaticAddress, SourceAddress.Size,
                                 Source.StaticAddressOwnerVA),
              InvalidVA, -1, /*DefinedAtPoint=*/false};
          auto AddressQuery = pushQuery(
              AddressQueries, SourceAddress, *SourceLoad,
              llvm::ArrayRef<JumpTableValueOccurrence>(
                  SourceAddressAlternative));
          auto DependencyQuery = pushQuery(
              AddressQueries, Initializer.StoredValue.Value, *Writer,
              llvm::ArrayRef<JumpTableValueOccurrence>(Source.Value));
          if (!AddressQuery || !DependencyQuery)
            return false;
          AddressQueries[*AddressQuery].RequireExactAddressOwner = true;
          AddressQueries[*DependencyQuery].Relation =
              JumpTableValueRelation::MayDepend;
          if (!pushIndex(ProofQueries, *AddressQuery) ||
              !pushIndex(ProofQueries, *DependencyQuery))
            return false;
          if (!ensureAppendCapacity(SourceRanges) || !consumeWork())
            return false;
          SourceRanges.emplace_back(Source.StaticAddress,
                                    Source.StaticAddress + Source.ByteCount);
        }
        if (!consumeSortWork(SourceRanges.size()))
          return false;
        std::sort(SourceRanges.begin(), SourceRanges.end());
        va_t Expected = Initializer.StaticSourceAddress;
        if (Initializer.ByteCount > InvalidVA - Expected)
          return false;
        const va_t ExpectedEnd = Expected + Initializer.ByteCount;
        for (const auto &[Begin, End] : SourceRanges) {
          if (!consumeWork())
            return false;
          if (Begin != Expected || End <= Begin || End > ExpectedEnd)
            return false;
          Expected = End;
        }
        if (Expected != ExpectedEnd)
          return false;
        if (!ensureAppendCapacity(StoreWriters) || !consumeWork())
          return false;
        StoreWriters.push_back(Initializer.Writer);
      }
      if (StoreWriters.size() >
          std::numeric_limits<size_t>::max() - MemcpyWriters.size()) {
        Complete = false;
        return false;
      }
      const size_t WriterCount = StoreWriters.size() + MemcpyWriters.size();
      if (Info.PhysicalCapacity == 0 || Info.EntrySize == 0 ||
          WriterCount == 0)
        return false;
      const uint64_t PhysicalStride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      if (PhysicalStride < Info.EntrySize)
        return false;
      if (WriterCount > MaxProofQueries ||
          Info.PhysicalCapacity >
              MaxProofQueries / WriterCount ||
          Info.PhysicalCapacity > MaxProofQueries ||
          AddressQueries.size() > MaxProofQueries - Info.PhysicalCapacity) {
        Complete = false;
        return false;
      }
      for (uint32_t Slot = 0; Slot < Info.PhysicalCapacity; ++Slot) {
        if (!consumeWork())
          return false;
        if (Slot != 0 && PhysicalStride >
                             static_cast<uint64_t>(
                                 std::numeric_limits<int64_t>::max()) /
                                 Slot)
          return false;
        const uint64_t SlotDelta = uint64_t{Slot} * PhysicalStride;
        if (SlotDelta >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
          return false;
        const std::optional<int64_t> FrameDelta = checkedFrameOffset(
            FrameStorage.RuntimeBase.ByteAddend,
            static_cast<int64_t>(SlotDelta), /*Subtract=*/false);
        if (!FrameDelta)
          return false;
        auto MemoryQuery = pushQuery(
            AddressQueries, guestAddressView(RuntimeUse.Value), *State.Load,
            llvm::ArrayRef<JumpTableValueOccurrence>());
        if (!MemoryQuery)
          return false;
        JumpTableValueQuery &Memory = AddressQueries[*MemoryQuery];
        Memory.Relation =
            JumpTableValueRelation::AuthenticatedFrameMemory;
        Memory.FrameByteAddend = *FrameDelta;
        Memory.FrameAddressUseAddr = RuntimeUse.Addr;
        Memory.FrameAddressUseSeq = RuntimeUse.Seq;
        Memory.FrameMemorySize = Info.EntrySize;
        if (!consumeProduct(StoreWriters.size(), 3) || !consumeWork(2) ||
            !consumeProduct(MemcpyWriters.size(), 3) || !consumeWork(2))
          return false;
        Memory.AuthenticatedFrameStoreWriters = StoreWriters;
        Memory.AuthenticatedFrameMemcpyWriters = MemcpyWriters;
        if (!pushIndex(ProofQueries, *MemoryQuery))
          return false;
      }

      JumpTableValueOccurrence InnerAddress{
          guestAddressView(RuntimeAdd->Output), RuntimeAdd->Addr,
          RuntimeAdd->Seq, /*DefinedAtPoint=*/true};
      const bool CompleteIsInner =
          CompleteAdd == RuntimeAdd &&
          sameVar(guestAddressView(CompleteAdd->Output),
                  InnerAddress.Value);
      if (CompleteIsInner) {
        if (FrameStorage.RuntimeBase.ByteAddend != 0)
          return false;
      } else {
        if ((CompleteAdd->Opcode != NdOp::INT_ADD &&
             CompleteAdd->Opcode != NdOp::INT_SUB) ||
            CompleteAdd->NumInputs < 2)
          return false;
        int ValueSide = -1;
        int ConstantSide = -1;
        if (CompleteAdd->Opcode == NdOp::INT_SUB) {
          if (!CompleteAdd->Inputs[1].isConst())
            return false;
          ValueSide = 0;
          ConstantSide = 1;
        } else {
          for (int Side = 0; Side < 2; ++Side)
            if (CompleteAdd->Inputs[Side].isConst()) {
              if (ConstantSide >= 0)
                return false;
              ConstantSide = Side;
              ValueSide = 1 - Side;
            }
        }
        if (ValueSide < 0 || ConstantSide < 0 ||
            CompleteAdd->Inputs[ConstantSide].Provenance !=
                ConstantAddressProvenance::Scalar)
          return false;
        const std::optional<int64_t> RawDelta = signedFrameDelta(
            CompleteAdd->Inputs[ConstantSide], CompleteAdd->Output.Size);
        if (!RawDelta)
          return false;
        const std::optional<int64_t> EffectiveDelta = checkedFrameOffset(
            0, *RawDelta, CompleteAdd->Opcode == NdOp::INT_SUB);
        if (!EffectiveDelta ||
            *EffectiveDelta != FrameStorage.RuntimeBase.ByteAddend)
          return false;
        const NdVar FinalBase =
            guestAddressView(CompleteAdd->Inputs[ValueSide]);
        if (!localCopyChainMatchesUse(InnerAddress, FinalBase, *CompleteAdd)) {
          auto FinalBaseQuery = pushQuery(AddressQueries, FinalBase,
                                          *CompleteAdd,
                                          llvm::ArrayRef<
                                              JumpTableValueOccurrence>(
                                              InnerAddress));
          if (!FinalBaseQuery)
            return false;
          if (!pushIndex(ProofQueries, *FinalBaseQuery))
            return false;
        }
      }
      if (!ensureAppendCapacity(AddressProofs) || !consumeWork(3))
        return false;
      AddressProofs.push_back(
          {RoleIndex,
           CompleteAdd,
           {DynamicValue, RuntimeAdd->Addr, RuntimeAdd->Seq,
            /*DefinedAtPoint=*/false},
           std::move(ProofQueries)});
      continue;
    }
    if (Role.HasBaseSelect) {
      auto TrueQuery = pushSingleBaseQuery(
          AddressQueries, State.Select->Inputs[1], *State.Select,
          Role.TrueBase, State.Select->Inputs[1].Size);
      auto FalseQuery = pushSingleBaseQuery(
          AddressQueries, State.Select->Inputs[2], *State.Select,
          Role.FalseBase, State.Select->Inputs[2].Size);
      if (!TrueQuery || !FalseQuery)
        return false;
      if (!ensureAppendCapacity(State.SelectQueries, 2) || !consumeWork(2))
        return false;
      State.SelectQueries = {*TrueQuery, *FalseQuery};
    } else if (Role.HasBaseMaskBlend) {
      const int PositiveOrSide = Role.PositiveBlendInputSide;
      const int NegativeOrSide = 1 - PositiveOrSide;
      const int PositiveBaseSide = Role.PositiveBaseInputSide;
      const int NegativeBaseSide = Role.NegativeBaseInputSide;
      auto PositiveOr =
          pushQuery(AddressQueries, State.Blend->Inputs[PositiveOrSide],
                    *State.Blend,
                    llvm::ArrayRef<JumpTableValueOccurrence>(
                        Role.PositiveBlendArm));
      auto NegativeOr =
          pushQuery(AddressQueries, State.Blend->Inputs[NegativeOrSide],
                    *State.Blend,
                    llvm::ArrayRef<JumpTableValueOccurrence>(
                        Role.NegativeBlendArm));
      auto PositiveBase = pushSingleBaseQuery(
          AddressQueries, State.PositiveAnd->Inputs[PositiveBaseSide],
          *State.PositiveAnd, Role.TrueBase,
          State.PositiveAnd->Inputs[PositiveBaseSide].Size);
      auto PositiveMask = pushQuery(
          AddressQueries, State.PositiveAnd->Inputs[1 - PositiveBaseSide],
          *State.PositiveAnd,
          llvm::ArrayRef<JumpTableValueOccurrence>(Role.PositiveMask));
      auto NegativeBase = pushSingleBaseQuery(
          AddressQueries, State.NegativeAnd->Inputs[NegativeBaseSide],
          *State.NegativeAnd, Role.FalseBase,
          State.NegativeAnd->Inputs[NegativeBaseSide].Size);
      auto NegativeMask = pushQuery(
          AddressQueries, State.NegativeAnd->Inputs[1 - NegativeBaseSide],
          *State.NegativeAnd,
          llvm::ArrayRef<JumpTableValueOccurrence>(Role.NegativeMask));
      auto Complement = pushQuery(AddressQueries, State.NegativeMask->Inputs[0],
                                  *State.NegativeMask,
                                  llvm::ArrayRef<JumpTableValueOccurrence>(
                                      Role.PositiveMask));
      auto BooleanCondition =
          BooleanAlternatives.empty()
              ? std::optional<size_t>{}
              : pushQuery(AddressQueries, State.PositiveMask->Inputs[0],
                          *State.PositiveMask, BooleanAlternatives,
                          /*AllowZeroExtension=*/true,
                          /*AllowSignExtension=*/false);
      if (!PositiveOr || !NegativeOr || !PositiveBase || !PositiveMask ||
          !NegativeBase || !NegativeMask || !Complement || !BooleanCondition)
        return false;
      if (!ensureAppendCapacity(State.SelectQueries, 8) || !consumeWork(8))
        return false;
      State.SelectQueries = {*PositiveOr,   *NegativeOr,      *PositiveBase,
                             *PositiveMask, *NegativeBase,    *NegativeMask,
                             *Complement,   *BooleanCondition};
    }
    const NdVar &LoadAddress =
        State.Load->Inputs[State.Load->NumInputs >= 2 ? 1 : 0];
    const NdVar GuestLoadAddress = guestAddressView(LoadAddress);
    if (!chargeInsnInventoryScan())
      return false;
    for (const auto &[Addr, Insn] : Insns) {
      (void)Addr;
      if (Insn.IsInstructionGuard)
        continue;
      for (const LowOp &Add : Insn.Ops) {
        if (Add.Opcode != NdOp::INT_ADD || Add.NumInputs < 2 ||
            (!Add.Output.isReg() && !Add.Output.isTemp()) ||
            guestAddressView(Add.Output).Size != GuestLoadAddress.Size)
          continue;
        for (int BaseSide = 0; BaseSide < 2; ++BaseSide) {
          const NdVar &RawBaseValue = Add.Inputs[BaseSide];
          const NdVar &RawDynamicValue = Add.Inputs[1 - BaseSide];
          // x86 LowIR uses the wide physical register container for address
          // arithmetic even in a 32-bit guest.  Only the low guest-pointer
          // lane participates in the effective address; requiring the
          // synthetic high lane to match a 32-bit SELECT/index occurrence
          // rejects valid i386 GOTOFF/CMOV tables.  Keep the exact occurrence
          // and lane proof, but query the architectural address view.
          const NdVar BaseValue = guestAddressView(RawBaseValue);
          const NdVar DynamicValue = guestAddressView(RawDynamicValue);
          if (!consumeWork(Role.AllowedBases.size()))
            return false;
          if (!Role.HasBaseSelect && !Role.HasBaseMaskBlend &&
              BaseValue.isConst() &&
              std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                        static_cast<va_t>(BaseValue.Offset)) ==
                  Role.AllowedBases.end())
            continue;
          if (!consumeProduct(State.SelectQueries.size(), 3) ||
              !consumeWork(2))
            return false;
          std::vector<size_t> ProofQueries;
          ProofQueries.reserve(State.SelectQueries.size());
          ProofQueries.insert(ProofQueries.end(), State.SelectQueries.begin(),
                              State.SelectQueries.end());
          const bool HasBaseMerge = Role.HasBaseSelect || Role.HasBaseMaskBlend;
          auto BaseQuery = HasBaseMerge
                               ? pushQuery(AddressQueries, BaseValue, Add,
                                           llvm::ArrayRef<
                                               JumpTableValueOccurrence>(
                                               Role.SelectedBase))
                               : pushBaseQuery(AddressQueries, BaseValue, Add,
                                               Role.AllowedBases,
                                               BaseValue.Size);
          if (!consumeWork(State.DynamicAlternatives.size()))
            return false;
          const bool LocallyAuthenticatedIndex = std::any_of(
              State.DynamicAlternatives.begin(),
              State.DynamicAlternatives.end(),
              [&](const JumpTableValueOccurrence &Alternative) {
                return localCopyChainMatchesUse(Alternative, DynamicValue, Add);
              });
          std::optional<size_t> IndexQuery;
          if (!LocallyAuthenticatedIndex)
            IndexQuery = pushQuery(AddressQueries, DynamicValue, Add,
                                   State.DynamicAlternatives);
          if (!BaseQuery || (!LocallyAuthenticatedIndex && !IndexQuery))
            return false;
          if (!pushIndex(ProofQueries, *BaseQuery))
            return false;
          if (IndexQuery) {
            if (!pushIndex(ProofQueries, *IndexQuery))
              return false;
          }
          if (!ensureAppendCapacity(AddressProofs) || !consumeWork(3))
            return false;
          AddressProofs.push_back({RoleIndex,
                                   &Add,
                                   {DynamicValue, Add.Addr, Add.Seq,
                                    /*DefinedAtPoint=*/false},
                                   std::move(ProofQueries)});
        }
      }
    }
  }
  if (!consumeProduct(Roles.size(), 5) || !consumeWork(2) ||
      !consumeProduct(Roles.size(), 5) || !consumeWork(2))
    return false;
  std::vector<std::vector<JumpTableValueOccurrence>> AddressAlternatives(
      Roles.size());
  std::vector<std::vector<JumpTableValueOccurrence>> AddressIndexAlternatives(
      Roles.size());
  bool AddressQueriesComplete = true;
  std::vector<bool> AddressResults;
  if (!AddressQueries.empty()) {
    AddressResults = tableValuesMatchAtUses(
        AddressQueries, &AddressQueriesComplete, nullptr, InvalidVA, nullptr,
        AggregateEvidenceBudget,
        limits::kMaxJumpTableRoleMatchEvidenceWork);
    if (!AddressQueriesComplete ||
        AddressResults.size() != AddressQueries.size()) {
      Complete = false;
      return false;
    }
  }
  for (const AddressProof &Proof : AddressProofs) {
    if (!consumeWork())
      return false;
    bool AllQueriesMatch =
        Proof.Add && Proof.RoleIndex < AddressAlternatives.size();
    for (size_t I : Proof.QueryIndices) {
      if (!consumeWork())
        return false;
      if (I >= AddressResults.size() || !AddressResults[I]) {
        AllQueriesMatch = false;
        break;
      }
    }
    if (!AllQueriesMatch)
      continue;
    JumpTableValueOccurrence Occurrence{guestAddressView(Proof.Add->Output),
                                        Proof.Add->Addr, Proof.Add->Seq,
                                        /*DefinedAtPoint=*/true};
    auto &Alternatives = AddressAlternatives[Proof.RoleIndex];
    if (!consumeWork(Alternatives.size()))
      return false;
    if (std::none_of(Alternatives.begin(), Alternatives.end(),
                     [&](const JumpTableValueOccurrence &Existing) {
                       return Existing.Addr == Occurrence.Addr &&
                              Existing.Seq == Occurrence.Seq &&
                              sameVar(Existing.Value, Occurrence.Value);
                     })) {
      if (!ensureAppendCapacity(Alternatives) || !consumeWork())
        return false;
      Alternatives.push_back(Occurrence);
    }
    auto &IndexAlternatives = AddressIndexAlternatives[Proof.RoleIndex];
    if (!consumeWork(IndexAlternatives.size()))
      return false;
    if (std::none_of(IndexAlternatives.begin(), IndexAlternatives.end(),
                     [&](const JumpTableValueOccurrence &Existing) {
                       return Existing.Addr == Proof.DynamicIndex.Addr &&
                              Existing.Seq == Proof.DynamicIndex.Seq &&
                              sameVar(Existing.Value, Proof.DynamicIndex.Value);
                     })) {
      if (!ensureAppendCapacity(IndexAlternatives) || !consumeWork())
        return false;
      IndexAlternatives.push_back(Proof.DynamicIndex);
    }
  }
  for (size_t I = 0; I < AddressAlternatives.size(); ++I) {
    if (!consumeWork())
      return false;
    if (AddressAlternatives[I].empty()) {
      return false;
    }
  }

  // A composite SelectOffset plan names the dynamic operand of one exact
  // address ADD.  The all-path LOAD proof intentionally accepts multiple ADD
  // alternatives for shared/diamond dispatches, but no single one of those
  // Med SSA inputs necessarily dominates the final branch.  Authenticate the
  // detector-recorded byte coordinate only when one address definition and
  // one dynamic input survived the role proof; otherwise clear it so
  // extraction publishes no composite plan and both backends fail closed.
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    if (!consumeWork())
      return false;
    JumpTableLoadRole &Role = *Roles[RoleIndex].Role;
    if (Role.AddressIndex.Value.Size == 0)
      continue;
    const auto &Addresses = AddressAlternatives[RoleIndex];
    const auto &Indices = AddressIndexAlternatives[RoleIndex];
    const bool UniqueAndMatching =
        Addresses.size() == 1 && Indices.size() == 1 &&
        Indices.front().Addr == Role.AddressIndex.Addr &&
        Indices.front().Seq == Role.AddressIndex.Seq &&
        sameVar(Indices.front().Value,
                guestAddressView(Role.AddressIndex.Value));
    if (UniqueAndMatching)
      Role.AddressIndex = Indices.front();
    else
      Role.AddressIndex = {};
  }

  // Phase 3: prove that the exact address used by every authenticated LOAD is
  // one of the complete role expressions above.  Passing all alternatives in
  // one query preserves PHI/shared-predecessor merges instead of requiring one
  // arbitrarily selected ADD to dominate every path.
  std::vector<JumpTableValueQuery> LoadQueries;
  std::vector<size_t> LoadQueryRoles;
  if (!consumeWork(4) || !consumeProduct(Roles.size(), 3) ||
      !consumeWork(2))
    return false;
  std::vector<bool> LoadMatches(Roles.size(), false);
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    if (!consumeWork())
      return false;
    const RoleState &State = Roles[RoleIndex];
    const NdVar &LoadAddress =
        State.Load->Inputs[State.Load->NumInputs >= 2 ? 1 : 0];
    if (!consumeWork(AddressAlternatives[RoleIndex].size()))
      return false;
    if (std::any_of(AddressAlternatives[RoleIndex].begin(),
                    AddressAlternatives[RoleIndex].end(),
                    [&](const JumpTableValueOccurrence &Alternative) {
                      return localCopyChainMatchesUse(Alternative, LoadAddress,
                                                      *State.Load);
                    })) {
      LoadMatches[RoleIndex] = true;
      continue;
    }
    if (!pushQuery(LoadQueries, guestAddressView(LoadAddress), *State.Load,
                   AddressAlternatives[RoleIndex]))
      return false;
    if (!pushIndex(LoadQueryRoles, RoleIndex))
      return false;
  }
  bool LoadQueriesComplete = true;
  std::vector<bool> LoadResults;
  if (!LoadQueries.empty()) {
    LoadResults = tableValuesMatchAtUses(
        LoadQueries, &LoadQueriesComplete, nullptr, InvalidVA, nullptr,
        AggregateEvidenceBudget,
        limits::kMaxJumpTableRoleMatchEvidenceWork);
    if (!LoadQueriesComplete || LoadResults.size() != LoadQueries.size()) {
      Complete = false;
      return false;
    }
  }
  if (LoadResults.size() != LoadQueryRoles.size()) {
    Complete = false;
    return false;
  }
  for (size_t I = 0; I < LoadResults.size(); ++I) {
    if (!consumeWork())
      return false;
    if (LoadResults[I])
      LoadMatches[LoadQueryRoles[I]] = true;
  }
  if (!consumeWork(LoadMatches.size()))
    return false;
  if (!std::all_of(LoadMatches.begin(), LoadMatches.end(),
                   [](bool Matched) { return Matched; }))
    return false;

  // Publish relocation-consumer exemptions transactionally from the frame
  // roles that survived reachability pruning and every address/memory proof.
  // A stale source LOAD from a discarded site must not suppress its code roots.
  std::vector<JumpTableValueOccurrence> StorageConsumers;
  if (!consumeWork(2))
    return false;
  for (const RoleState &State : Roles) {
    if (!consumeWork())
      return false;
    for (const JumpTableFrameInitializerChunk &Initializer :
         State.Role->FrameStorage.Initializers) {
      if (!consumeWork())
        return false;
      if (Initializer.IsMemcpy) {
        // The exact CALL source occurrence has already passed source-owner,
        // length, destination-interval, and reaching-value proofs above.  It
        // is therefore a storage consumer just like a direct initializer
        // LOAD; without publishing it, final escape suppression mistakes the
        // memcpy source argument for an observable whole-object escape.
        if (!ensureAppendCapacity(StorageConsumers) || !consumeWork())
          return false;
        StorageConsumers.push_back(Initializer.SourceAddress);
      } else {
        for (const auto &Source : Initializer.StaticSources) {
          if (!consumeWork() || !ensureAppendCapacity(StorageConsumers) ||
              !consumeWork())
            return false;
          StorageConsumers.push_back(Source.Address);
        }
      }
    }
  }
  if (!consumeSortWork(StorageConsumers.size()))
    return false;
  std::sort(StorageConsumers.begin(), StorageConsumers.end(),
            [](const JumpTableValueOccurrence &A,
               const JumpTableValueOccurrence &B) {
              return std::tie(A.Addr, A.Seq, A.Value.Space, A.Value.Offset,
                              A.Value.Size) <
                     std::tie(B.Addr, B.Seq, B.Value.Space, B.Value.Offset,
                              B.Value.Size);
            });
  if (!consumeWork(StorageConsumers.size()))
    return false;
  StorageConsumers.erase(
      std::unique(StorageConsumers.begin(), StorageConsumers.end()),
      StorageConsumers.end());
  if (!Complete)
    return false;
  Info.LoadRoles = std::move(WorkingLoadRoles);
  Info.TargetLoads = std::move(WorkingTargetLoads);
  Info.IndexValueAlternatives = std::move(WorkingIndexValueAlternatives);
  Info.IndexValueAtUse = WorkingIndexValueAtUse;
  Info.IndexUseAddr = WorkingIndexUseAddr;
  Info.IndexUseSeq = WorkingIndexUseSeq;
  Info.IndexValueDefinedAtUse = WorkingIndexValueDefinedAtUse;
  Info.TableLoadAddr = WorkingTableLoadAddr;
  Info.TableLoadSeq = WorkingTableLoadSeq;
  Info.AuthenticatedStorageConsumers = std::move(StorageConsumers);
  return true;
}

std::set<va_t> CFGBuilder::candidateReachableInstructions(
    const InsnRecord &Candidate, const std::vector<va_t> &CandidateTargets,
    const std::set<va_t> &Roots,
    const std::vector<JumpTableStorageRange> &CandidateStorage,
    size_t *GraphWorkBudget, bool *AnalysisComplete) const {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  std::vector<ResolverInsnSnapshot> Snapshot;
  if (!copyResolverInsnSnapshots(
          Insns, Snapshot,
          [&](va_t Addr, const auto &Rec) -> const std::vector<va_t> & {
            return Addr == Candidate.Addr ? CandidateTargets
                                          : Rec.JumpTableTargets;
          },
          [&](size_t Amount) {
            return consumeResolverGraphWork(GraphWorkBudget, Amount);
          }))
    return {};

  bool GraphComplete = false;
  const ResolverFlowGraph Graph = buildResolverFlowGraph(
      Snapshot, BlockStarts, Roots, DiscoveredCodeRefSources,
      [&](va_t Address, const std::set<va_t> *ActiveOwners) {
        if (ActiveOwners) {
          if (!consumeResolverGraphWork(
                  GraphWorkBudget, orderedSetLookupWork(ActiveOwners->size())))
            return std::optional<bool>{};
        }
        if (ActiveOwners && ActiveOwners->count(Candidate.Addr)) {
          for (const JumpTableStorageRange &Range : CandidateStorage) {
            if (!consumeResolverGraphWork(GraphWorkBudget))
              return std::optional<bool>{};
            if (Range.ownsStorageAddress(Address))
              return std::optional<bool>{true};
          }
        }
        return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners,
                                                   GraphWorkBudget);
      },
      GraphWorkBudget, &GraphComplete);
  if (!GraphComplete)
    return {};

  std::set<va_t> Reachable;
  const size_t ReachableLookupWork =
      orderedSetLookupWork(Graph.InsnToBlock.size());
  if (GraphWorkBudget &&
      (ReachableLookupWork > std::numeric_limits<size_t>::max() - 4 ||
       (!Graph.InsnToBlock.empty() &&
        ReachableLookupWork + 4 >
            std::numeric_limits<size_t>::max() / Graph.InsnToBlock.size()))) {
    *GraphWorkBudget = 0;
    return {};
  }
  if (!consumeResolverGraphWork(GraphWorkBudget,
                                GraphWorkBudget ? Graph.InsnToBlock.size() *
                                                      (ReachableLookupWork + 4)
                                                : 0))
    return {};
  for (const auto &[Addr, Block] : Graph.InsnToBlock) {
    (void)Block;
    Reachable.insert(Addr);
  }
  if (AnalysisComplete)
    *AnalysisComplete = true;
  return Reachable;
}

std::vector<std::optional<bool>>
CFGBuilder::tableLoadConditionValues(llvm::ArrayRef<va_t> BranchAddrs,
                                     const JumpTableInfo &Info,
                                     bool *AnalysisComplete,
                                     size_t *GraphWorkBudget) const {
  if (AnalysisComplete)
    *AnalysisComplete = false;
  RequestedCompleteJumpTableProof = true;

  size_t LegacyWorkBudget = limits::kMaxJumpTableEvidenceWork;
  size_t *EvidenceBudget =
      GraphWorkBudget ? GraphWorkBudget : &LegacyWorkBudget;
  bool Complete = true;
  auto consumeWork = [&](size_t Amount = 1) {
    if (!consumeResolverGraphWork(EvidenceBudget, Amount)) {
      Complete = false;
      return false;
    }
    return true;
  };
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count) {
      *EvidenceBudget = 0;
      Complete = false;
      return false;
    }
    return consumeWork(Count * Cost);
  };
  const size_t MaxQueries =
      GraphWorkBudget ? limits::kMaxJumpTableValueMatchEvidenceWork
                      : limits::kMaxJumpTableEntries;
  if (BranchAddrs.size() > MaxQueries ||
      !consumeProduct(BranchAddrs.size(), 3) || !consumeWork(2)) {
    if (GraphWorkBudget)
      *GraphWorkBudget = 0;
    return {};
  }
  std::vector<std::optional<bool>> Results(BranchAddrs.size());
  if (!JumpTableProofContextComplete || Info.TableLoadAddr == InvalidVA)
    return Results;
  if (BranchAddrs.empty()) {
    if (AnalysisComplete)
      *AnalysisComplete = true;
    return Results;
  }

  std::vector<ResolverInsnSnapshot> Snapshot;
  if (!copyResolverInsnSnapshots(
          Insns, Snapshot,
          [](va_t, const auto &Rec) -> const std::vector<va_t> & {
            return Rec.JumpTableTargets;
          }, consumeWork))
    return Results;

  std::map<va_t, const ResolverInsnSnapshot *> SnapshotByAddr;
  for (const ResolverInsnSnapshot &S : Snapshot) {
    if (!consumeWork() ||
        !consumeWork(orderedSetLookupWork(SnapshotByAddr.size())))
      return Results;
    auto Position = SnapshotByAddr.lower_bound(S.Addr);
    if (Position == SnapshotByAddr.end() || Position->first != S.Addr) {
      if (!consumeWork(5))
        return Results;
      SnapshotByAddr.emplace_hint(Position, S.Addr, &S);
    }
  }
  const std::set<va_t> &ProofRoots = ActiveJumpTableProofRoots
                                         ? *ActiveJumpTableProofRoots
                                         : PersistentCFGRoots;
  bool GraphComplete = false;
  const ResolverFlowGraph Graph = buildResolverFlowGraph(
      Snapshot, BlockStarts, ProofRoots, DiscoveredCodeRefSources,
      [&](va_t Address, const std::set<va_t> *ActiveOwners) {
        return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners,
                                                   EvidenceBudget);
      },
      EvidenceBudget, &GraphComplete);
  if (!GraphComplete) {
    Complete = false;
    return Results;
  }
  if (!consumeWork(orderedSetLookupWork(Graph.InsnToBlock.size())))
    return Results;
  auto LI = Graph.InsnToBlock.find(Info.TableLoadAddr);
  if (LI == Graph.InsnToBlock.end()) {
    if (AnalysisComplete)
      *AnalysisComplete = Complete;
    return Results;
  }
  const int LoadBlock = LI->second;

  auto reachable = [&](llvm::ArrayRef<int> Starts, int Target,
                       int Excluded) -> std::optional<bool> {
    std::list<int> Work;
    for (int Start : Starts) {
      if (!consumeWork(4))
        return std::nullopt;
      Work.push_back(Start);
    }
    std::set<int> Seen;
    while (!Work.empty()) {
      if (!consumeWork())
        return std::nullopt;
      int B = Work.back();
      Work.pop_back();
      if (B == Excluded)
        continue;
      if (!consumeWork(orderedSetLookupWork(Seen.size())))
        return std::nullopt;
      auto SeenPosition = Seen.lower_bound(B);
      if (SeenPosition != Seen.end() && *SeenPosition == B)
        continue;
      if (!consumeWork(3))
        return std::nullopt;
      Seen.emplace_hint(SeenPosition, B);
      if (B == Target)
        return true;
      if (B < 0 || B >= static_cast<int>(Graph.Blocks.size())) {
        Complete = false;
        return std::nullopt;
      }
      for (int S : Graph.Blocks[B].Succs) {
        if (!consumeWork() || !consumeWork(3))
          return std::nullopt;
        Work.push_back(S);
      }
    }
    return false;
  };

  const std::vector<int> &Roots = Graph.RootBlocks;
  bool BlockLookupComplete = true;
  auto blockFor = [&](va_t Addr) -> int {
    if (!consumeWork(orderedSetLookupWork(Graph.InsnToBlock.size()))) {
      BlockLookupComplete = false;
      return -1;
    }
    auto It = Graph.InsnToBlock.find(Addr);
    return It == Graph.InsnToBlock.end() ? -1 : It->second;
  };

  const std::optional<bool> RootReaches =
      Roots.empty() ? std::optional<bool>{false}
                    : reachable(Roots, LoadBlock, -1);
  if (!RootReaches) {
    Complete = false;
    return Results;
  }
  if (!*RootReaches) {
    if (AnalysisComplete)
      *AnalysisComplete = Complete;
    return Results;
  }
  for (size_t QueryIndex = 0; QueryIndex < BranchAddrs.size(); ++QueryIndex) {
    if (!consumeWork())
      return Results;
    const va_t BranchAddr = BranchAddrs[QueryIndex];
    if (!consumeWork(orderedSetLookupWork(SnapshotByAddr.size())) ||
        !consumeWork(orderedSetLookupWork(Graph.InsnToBlock.size())))
      return Results;
    auto SnapshotIt = SnapshotByAddr.find(BranchAddr);
    auto BI = Graph.InsnToBlock.find(BranchAddr);
    if (BranchAddr == InvalidVA || SnapshotIt == SnapshotByAddr.end() ||
        BI == Graph.InsnToBlock.end())
      continue;
    const ResolverInsnSnapshot *BranchSnapshot = SnapshotIt->second;
    if (!BranchSnapshot->IsBranch || !BranchSnapshot->IsCond)
      continue;
    const int GuardBlock = BI->second;
    const std::optional<bool> ReachesWithoutGuard =
        GuardBlock == LoadBlock
            ? std::optional<bool>{true}
            : reachable(Roots, LoadBlock, GuardBlock);
    if (!ReachesWithoutGuard) {
      Complete = false;
      return Results;
    }
    if (*ReachesWithoutGuard)
      continue;

    const ResolverFlowBlock &Guard = Graph.Blocks[GuardBlock];
    if (Guard.ExternalSuccs >
        std::numeric_limits<size_t>::max() - Guard.Succs.size()) {
      Complete = false;
      return Results;
    }
    const size_t TotalSuccs = Guard.Succs.size() + Guard.ExternalSuccs;
    // A predicated terminal effect has one published CFG successor (the skip
    // edge); executing RETURN/INDIR_BR exits the local graph instead of
    // contributing a second successor node.  Validate that terminal shape
    // below before accepting it as the missing edge.
    if (TotalSuccs < 2 && !BranchSnapshot->IsInstructionGuard)
      continue;

    // Use the actual LowIR condition edge, not InsnRecord::BranchTarget.  ARM
    // lowers a conditional guest branch as COND_BR fallthrough,!cond followed
    // by BRANCH guest_target in the same instruction record.
    va_t TrueTarget = InvalidVA;
    va_t FalseTarget = BranchSnapshot->Addr + BranchSnapshot->Size;
    bool SawCondition = false;
    bool GuardedTerminalEffect = false;
    if (!consumeWork(BranchSnapshot->Ops.size()))
      return Results;
    for (const LowOp &Op : BranchSnapshot->Ops) {
      if (Op.Addr != BranchAddr)
        continue;
      if (Op.Opcode == NdOp::COND_BR && Op.NumInputs >= 2 &&
          Op.Inputs[0].isConst()) {
        TrueTarget = Op.Inputs[0].Offset;
        SawCondition = true;
      } else if (SawCondition && Op.Opcode == NdOp::BRANCH &&
                 Op.NumInputs >= 1 && Op.Inputs[0].isConst()) {
        FalseTarget = Op.Inputs[0].Offset;
        break;
      } else if (SawCondition && BranchSnapshot->IsInstructionGuard &&
                 (Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::INDIR_BR)) {
        // ARM/Thumb predicate a return/indirect branch by branching over the
        // terminal effect to the next instruction.  The skip edge may reach
        // the table; executing the effect cannot.  Predicated LOAD/STORE/CALL
        // records are not terminal and therefore remain ineligible guards.
        GuardedTerminalEffect = true;
      }
    }
    if (TrueTarget == InvalidVA)
      continue;
    if (BranchSnapshot->IsInstructionGuard && !GuardedTerminalEffect)
      continue;
    const int TrueBlock = blockFor(TrueTarget);
    if (!BlockLookupComplete)
      return Results;
    const int FalseBlock = blockFor(FalseTarget);
    if (!BlockLookupComplete)
      return Results;
    std::optional<bool> TrueReaches = false;
    if (TrueBlock >= 0) {
      const std::array<int, 1> Starts = {TrueBlock};
      TrueReaches = reachable(Starts, LoadBlock, GuardBlock);
    }
    std::optional<bool> FalseReaches = false;
    if (!GuardedTerminalEffect && FalseBlock >= 0) {
      const std::array<int, 1> Starts = {FalseBlock};
      FalseReaches = reachable(Starts, LoadBlock, GuardBlock);
    }
    if (!TrueReaches || !FalseReaches) {
      Complete = false;
      return Results;
    }
    if (*TrueReaches != *FalseReaches)
      Results[QueryIndex] = *TrueReaches;
  }
  if (AnalysisComplete)
    *AnalysisComplete = Complete;
  return Results;
}

std::optional<bool>
CFGBuilder::tableLoadConditionValue(va_t BranchAddr,
                                    const JumpTableInfo &Info,
                                    size_t *GraphWorkBudget) const {
  bool Complete = false;
  const std::vector<std::optional<bool>> Results =
      tableLoadConditionValues({BranchAddr}, Info, &Complete,
                               GraphWorkBudget);
  return Complete && Results.size() == 1 ? Results.front() : std::nullopt;
}

bool CFGBuilder::branchControlsTableLoad(va_t BranchAddr,
                                         const JumpTableInfo &Info,
                                         size_t *GraphWorkBudget) const {
  return tableLoadConditionValue(BranchAddr, Info, GraphWorkBudget)
      .has_value();
}

/// Resolve an address nd-var to a stack/frame slot key (base = SP/FP register
/// plus a constant byte offset).  Returns false for any non-SP/FP base or a
/// scaled-index address, so store-to-load forwarding never crosses heap/global
/// memory (which would be unsound).
bool frameSlotKey(const std::vector<LowOp> &Ops, int FromIdx, NdVar AddrV,
                  const TargetRegInfo &TRI, uint64_t &BaseReg, int64_t &Off) {
  Off = 0;
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (AddrV.isReg()) {
      if (!TRI.isFrameReg(AddrV.Offset))
        return false;
      BaseReg = AddrV.Offset;
      return true;
    }
    if (!AddrV.isTemp())
      return false;
    int D = reachingDefIdx(Ops, FromIdx, AddrV);
    if (D < 0)
      return false;
    const LowOp &A = Ops[D];
    if (A.Opcode == NdOp::COPY && A.NumInputs >= 1) {
      AddrV = A.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    if (A.Opcode == NdOp::INT_ADD && A.NumInputs >= 2) {
      int CW = A.Inputs[1].isConst() ? 1 : (A.Inputs[0].isConst() ? 0 : -1);
      if (CW < 0)
        return false;
      if (scaledIndexReg(Ops, D - 1, A.Inputs[1 - CW]) != InvalidVA)
        return false;
      const std::optional<int64_t> Delta =
          signedFrameDelta(A.Inputs[CW], A.Output.Size);
      if (!Delta)
        return false;
      const std::optional<int64_t> Next =
          checkedFrameOffset(Off, *Delta, false);
      if (!Next)
        return false;
      Off = *Next;
      AddrV = A.Inputs[1 - CW];
      FromIdx = D - 1;
      continue;
    }
    if (A.Opcode == NdOp::INT_SUB && A.NumInputs >= 2 &&
        A.Inputs[1].isConst()) {
      const std::optional<int64_t> Delta =
          signedFrameDelta(A.Inputs[1], A.Output.Size);
      if (!Delta)
        return false;
      const std::optional<int64_t> Next = checkedFrameOffset(Off, *Delta, true);
      if (!Next)
        return false;
      Off = *Next;
      AddrV = A.Inputs[0];
      FromIdx = D - 1;
      continue;
    }
    return false;
  }
  return false;
}

} // namespace neverd
