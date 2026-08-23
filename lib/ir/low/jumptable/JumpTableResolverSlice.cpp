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
#include "neverd/solver/BitVectorSolver.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/DivisionByConstantInfo.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

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
  std::map<std::pair<va_t, int>, std::pair<int, int>> PointToOp;
  std::set<va_t> InstructionGuards;
  /// Durable and conditional block-entry roots that actually seeded the final
  /// pruned graph.  Dominance and live-in identity must use this set rather
  /// than graph indegree or PersistentCFGRoots alone.
  std::vector<int> RootBlocks;
};

static ResolverFlowGraph buildResolverFlowGraph(
    const std::vector<ResolverInsnSnapshot> &Insns,
    const std::set<va_t> &BlockStarts, const std::set<va_t> &PersistentRoots,
    const std::map<va_t, std::set<va_t>> &ConditionalCodeRefRoots,
    const std::function<bool(va_t, const std::set<va_t> *)> &IsTableStorage) {
  ResolverFlowGraph Graph;
  std::map<va_t, std::vector<const ResolverInsnSnapshot *>> Grouped;
  for (const ResolverInsnSnapshot &Insn : Insns) {
    auto BI = BlockStarts.upper_bound(Insn.Addr);
    if (BI == BlockStarts.begin())
      continue;
    --BI;
    Grouped[*BI].push_back(&Insn);
  }

  std::map<va_t, int> StartToBlock;
  for (const auto &[Start, Members] : Grouped) {
    const int Id = static_cast<int>(Graph.Blocks.size());
    StartToBlock[Start] = Id;
    ResolverFlowBlock Block;
    Block.Start = Start;
    for (const ResolverInsnSnapshot *Insn : Members) {
      Graph.InsnToBlock[Insn->Addr] = Id;
      if (Insn->IsInstructionGuard)
        Graph.InstructionGuards.insert(Insn->Addr);
      for (const LowOp &Op : Insn->Ops) {
        const int OpIndex = static_cast<int>(Block.Ops.size());
        Block.Ops.push_back(Op);
        Graph.PointToOp[{Op.Addr, Op.Seq}] = {Id, OpIndex};
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
    if (Target == InvalidVA)
      return;
    const int To = blockAtInsn(Target);
    if (To < 0) {
      if (CountsExternal)
        ++Graph.Blocks[From].ExternalSuccs;
      return;
    }
    auto &Succs = Graph.Blocks[From].Succs;
    if (std::find(Succs.begin(), Succs.end(), To) == Succs.end())
      Succs.push_back(To);
  };

  for (int B = 0; B < static_cast<int>(Graph.Blocks.size()); ++B) {
    ResolverFlowBlock &Block = Graph.Blocks[B];
    const ResolverInsnSnapshot *Rec = Block.LastInsn;
    if (!Rec)
      continue;
    const va_t Fall = Rec->Addr + Rec->Size;
    if (Rec->IsRet && Rec->IsCond && Rec->IsBranch) {
      addEdge(B, Rec->BranchTarget, true);
    } else if (Rec->IsRet) {
      // Terminal.
    } else if (Rec->IsBranch && Rec->IsIndirect) {
      if (Rec->IsCond)
        addEdge(B, Fall, true);
      for (va_t Target : Rec->JumpTableTargets)
        addEdge(B, Target, true);
    } else if (Rec->IsBranch && !Rec->IsIndirect && !Rec->IsCall) {
      if (Rec->IsCond)
        addEdge(B, Fall, true);
      addEdge(B, Rec->BranchTarget, true);
    } else if (!Rec->IsBranch || Rec->IsCall) {
      if (!Rec->IsNoReturnCall || Rec->IsCond)
        addEdge(B, Fall, false);
    }
  }

  for (int B = 0; B < static_cast<int>(Graph.Blocks.size()); ++B)
    for (int S : Graph.Blocks[B].Succs) {
      auto &Preds = Graph.Blocks[S].Preds;
      if (std::find(Preds.begin(), Preds.end(), B) == Preds.end())
        Preds.push_back(B);
    }

  // Decoded instructions are retained across fixed-point rounds so a target
  // can be re-admitted without decoding churn.  They are not all roots: once
  // a provisional table slot is removed, its old case block (and any backedge
  // it contains) must disappear from the proof graph as well as from LowFunc.
  // Seed only durable roots and follow the current successor relation, whose
  // indirect edges already reflect the latest JumpTableTargets.
  std::vector<bool> Reachable(Graph.Blocks.size(), false);
  std::queue<int> Worklist;
  auto Flood = [&] {
    while (!Worklist.empty()) {
      const int Block = Worklist.front();
      Worklist.pop();
      for (int Succ : Graph.Blocks[Block].Succs)
        if (Succ >= 0 && Succ < static_cast<int>(Graph.Blocks.size()) &&
            !Reachable[Succ]) {
          Reachable[Succ] = true;
          Worklist.push(Succ);
        }
    }
  };
  std::set<va_t> ActiveTableOwners;
  std::set<int> ActiveRootBlocks;
  for (;;) {
    std::fill(Reachable.begin(), Reachable.end(), false);
    Worklist = std::queue<int>();
    ActiveRootBlocks.clear();
    for (va_t Root : PersistentRoots) {
      auto It = StartToBlock.find(Root);
      if (It == StartToBlock.end())
        continue;
      ActiveRootBlocks.insert(It->second);
      if (!Reachable[It->second]) {
        Reachable[It->second] = true;
        Worklist.push(It->second);
      }
    }
    Flood();
    for (bool Added = true; Added;) {
      Added = false;
      for (const auto &[Target, Sources] : ConditionalCodeRefRoots) {
        if (IsTableStorage && IsTableStorage(Target, &ActiveTableOwners))
          continue;
        auto TargetBlock = StartToBlock.find(Target);
        if (TargetBlock == StartToBlock.end())
          continue;
        const bool HasReachableSource =
            std::any_of(Sources.begin(), Sources.end(), [&](va_t Source) {
              auto SourceBlock = Graph.InsnToBlock.find(Source);
              return SourceBlock != Graph.InsnToBlock.end() &&
                     Reachable[SourceBlock->second];
            });
        if (!HasReachableSource)
          continue;
        ActiveRootBlocks.insert(TargetBlock->second);
        if (Reachable[TargetBlock->second])
          continue;
        Reachable[TargetBlock->second] = true;
        Worklist.push(TargetBlock->second);
        Added = true;
      }
      Flood();
    }

    bool OwnerAdded = false;
    for (const ResolverInsnSnapshot &Insn : Insns) {
      if (Insn.JumpTableTargets.empty())
        continue;
      auto It = Graph.InsnToBlock.find(Insn.Addr);
      if (It != Graph.InsnToBlock.end() && Reachable[It->second])
        OwnerAdded |= ActiveTableOwners.insert(Insn.Addr).second;
    }
    if (!OwnerAdded)
      break;
  }
  Graph.RootBlocks.assign(ActiveRootBlocks.begin(), ActiveRootBlocks.end());

  if (std::find(Reachable.begin(), Reachable.end(), false) != Reachable.end()) {
    std::vector<int> OldToNew(Graph.Blocks.size(), -1);
    std::vector<ResolverFlowBlock> Pruned;
    Pruned.reserve(static_cast<size_t>(
        std::count(Reachable.begin(), Reachable.end(), true)));
    for (size_t Old = 0; Old < Graph.Blocks.size(); ++Old) {
      if (!Reachable[Old])
        continue;
      OldToNew[Old] = static_cast<int>(Pruned.size());
      Pruned.push_back(std::move(Graph.Blocks[Old]));
    }
    for (ResolverFlowBlock &Block : Pruned) {
      std::vector<int> Succs;
      Succs.reserve(Block.Succs.size());
      for (int Succ : Block.Succs)
        if (Succ >= 0 && Succ < static_cast<int>(OldToNew.size()) &&
            OldToNew[Succ] >= 0)
          Succs.push_back(OldToNew[Succ]);
      Block.Succs = std::move(Succs);
      Block.Preds.clear();
    }
    for (size_t Block = 0; Block < Pruned.size(); ++Block)
      for (int Succ : Pruned[Block].Succs)
        Pruned[Succ].Preds.push_back(static_cast<int>(Block));

    for (auto It = Graph.InsnToBlock.begin(); It != Graph.InsnToBlock.end();) {
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
      if (!Graph.InsnToBlock.count(*It))
        It = Graph.InstructionGuards.erase(It);
      else
        ++It;
    }
    std::vector<int> RemappedRoots;
    for (int Root : Graph.RootBlocks)
      if (Root >= 0 && Root < static_cast<int>(OldToNew.size()) &&
          OldToNew[Root] >= 0 &&
          std::find(RemappedRoots.begin(), RemappedRoots.end(),
                    OldToNew[Root]) == RemappedRoots.end())
        RemappedRoots.push_back(OldToNew[Root]);
    Graph.RootBlocks = std::move(RemappedRoots);
    Graph.Blocks = std::move(Pruned);
  }
  return Graph;
}

struct ResolverValueExpr;
using ResolverValue = std::shared_ptr<const ResolverValueExpr>;

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
  ResolverValue Input;
  std::vector<ResolverValue> Inputs;
};

static bool sameResolverValue(const ResolverValue &A, const ResolverValue &B) {
  if (A == B)
    return true;
  if (!A || !B || A->K != B->K || A->Size != B->Size ||
      A->SliceOffset != B->SliceOffset || A->Constant != B->Constant ||
      A->Provenance != B->Provenance ||
      A->AddressOwnerVA != B->AddressOwnerVA || A->Root != B->Root ||
      A->Opcode != B->Opcode || A->HasOpcode != B->HasOpcode ||
      A->Inputs.size() != B->Inputs.size())
    return false;
  if (!sameResolverValue(A->Input, B->Input))
    return false;
  for (size_t I = 0; I < A->Inputs.size(); ++I)
    if (!sameResolverValue(A->Inputs[I], B->Inputs[I]))
      return false;
  return true;
}

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
                                      uint64_t AddressOwnerVA = InvalidVA) {
  if (Size == 0 || Size > sizeof(uint64_t))
    return {};
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Constant;
  E->Size = Size;
  E->Constant = Value & resolverWidthMask(Size);
  E->Provenance = Provenance;
  E->AddressOwnerVA = AddressOwnerVA;
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

static ResolverValue resolverExtend(ResolverValue Input, uint16_t Size,
                                    bool Signed) {
  if (!Input || Input->Size == 0 || Size < Input->Size)
    return {};
  if (Size == Input->Size)
    return Input;
  if (Input->K == ResolverValueExpr::Kind::Zero)
    return resolverZero(Size);
  if (Input->K == ResolverValueExpr::Kind::Constant) {
    uint64_t Value = Input->Constant & resolverWidthMask(Input->Size);
    if (Signed && Input->Size < sizeof(uint64_t)) {
      const unsigned Bits = Input->Size * 8;
      const uint64_t Sign = uint64_t{1} << (Bits - 1);
      if (Value & Sign)
        Value |= ~resolverWidthMask(Input->Size);
    }
    return resolverConstant(Value, Size, Input->Provenance,
                            Input->AddressOwnerVA);
  }
  if (Input->K == ResolverValueExpr::Kind::Merge) {
    std::vector<ResolverValue> Inputs;
    Inputs.reserve(Input->Inputs.size());
    for (const ResolverValue &Arm : Input->Inputs) {
      ResolverValue Extended = resolverExtend(Arm, Size, Signed);
      if (!Extended)
        return {};
      Inputs.push_back(std::move(Extended));
    }
    return resolverMerge(Size, Input->Root, std::move(Inputs));
  }
  // Canonicalize consecutive extensions with the same signedness.  The
  // machine value of zext(zext(x, A), B) is zext(x, B), and likewise for two
  // sign extensions.  Keeping the redundant middle width in the symbolic
  // identity made an explicit `movzbl %al,%eax` followed by the architectural
  // EAX-to-RAX zero extension differ from a guard on AL even though an AH write
  // between them is provably non-overlapping.
  if (Input->Input &&
      ((!Signed && Input->K == ResolverValueExpr::Kind::ZeroExtend) ||
       (Signed && Input->K == ResolverValueExpr::Kind::SignExtend)))
    return resolverExtend(Input->Input, Size, Signed);
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = Signed ? ResolverValueExpr::Kind::SignExtend
                : ResolverValueExpr::Kind::ZeroExtend;
  E->Size = Size;
  E->Input = std::move(Input);
  return E;
}

static ResolverValue resolverSlice(ResolverValue Input, uint16_t Offset,
                                   uint16_t Size) {
  if (!Input || Size == 0 || uint32_t(Offset) + Size > Input->Size)
    return {};
  if (Offset == 0 && Size == Input->Size)
    return Input;
  if (Input->K == ResolverValueExpr::Kind::Zero)
    return resolverZero(Size);
  if (Input->K == ResolverValueExpr::Kind::Constant) {
    const unsigned Shift = Offset * 8;
    ConstantAddressProvenance Provenance = Input->Provenance;
    uint64_t Owner = Input->AddressOwnerVA;
    if (Offset != 0 || Size != Input->Size) {
      if (isAddressProvenance(Provenance))
        Provenance = ConstantAddressProvenance::AddressFragment;
      Owner = InvalidVA;
    }
    return resolverConstant(Input->Constant >> Shift, Size, Provenance, Owner);
  }
  if (Input->K == ResolverValueExpr::Kind::Merge) {
    std::vector<ResolverValue> Inputs;
    Inputs.reserve(Input->Inputs.size());
    for (const ResolverValue &Arm : Input->Inputs) {
      ResolverValue Sliced = resolverSlice(Arm, Offset, Size);
      if (!Sliced)
        return {};
      Inputs.push_back(std::move(Sliced));
    }
    return resolverMerge(Size, Input->Root, std::move(Inputs));
  }
  if (Offset == 0 && Input->Input &&
      (Input->K == ResolverValueExpr::Kind::ZeroExtend ||
       Input->K == ResolverValueExpr::Kind::SignExtend) &&
      Size == Input->Input->Size)
    return Input->Input;
  if (Input->K == ResolverValueExpr::Kind::Slice && Input->Input)
    return resolverSlice(Input->Input, Input->SliceOffset + Offset, Size);
  auto E = std::make_shared<ResolverValueExpr>();
  E->K = ResolverValueExpr::Kind::Slice;
  E->Size = Size;
  E->SliceOffset = Offset;
  E->Input = std::move(Input);
  return E;
}

/// Prove the complete unsigned remainder recipe emitted for division by a
/// constant.  This is deliberately a structural theorem over the expression
/// produced by the point-sensitive resolver, not a lexical recognition of a
/// multiply and shift: the exact same dividend expression must feed both the
/// LLVM-defined quotient recipe and the final `x - q*N` back-multiply.
/// Consequently a sibling quotient, a predicated reaching definition, or one
/// wrong magic/shift bit cannot authorize a table domain.
static bool provesLLVMUnsignedModuloRecipe(symbolic::SymContext &Ctx,
                                           symbolic::SymRef Index,
                                           uint64_t Divisor) {
  using symbolic::SymOp;
  using symbolic::SymRef;

  if (!Index || Divisor < limits::kMinJumpTableEntries)
    return false;

  // Table selectors are commonly widened to the internal address container
  // after the machine-width remainder.  Only zero extension preserves the
  // unsigned domain theorem; sign extension and arbitrary truncation do not.
  SymRef Remainder = Index;
  while (Ctx.op(Remainder) == SymOp::ZExt) {
    if (Ctx.numOperands(Remainder) != 1)
      return false;
    Remainder = Ctx.operand(Remainder, 0);
  }
  const uint32_t Width = Ctx.width(Remainder);
  if (Width < 2 || Width > 32 || Divisor >= (uint64_t{1} << Width)) {
    // The supported lowering uses a 2W-bit full product.  Keep the theorem's
    // bit-blast-free construction bounded until a production 64-bit modulo
    // dispatch provides a concrete 128-bit recipe test.
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
  auto shiftRight = [&](SymRef Value, uint32_t Amount) {
    if (Amount == 0)
      return Value;
    return Ctx.mkLShr(Value, Ctx.mkConst(Ctx.width(Value), Amount));
  };
  auto addUnique = [](std::vector<SymRef> &Values, SymRef Value) {
    if (Value && std::find(Values.begin(), Values.end(), Value) == Values.end())
      Values.push_back(Value);
  };

  struct QuotientForm {
    SymRef Narrow;
    SymRef Wide;
  };
  auto addQuotient = [&](std::vector<QuotientForm> &Forms, SymRef Narrow,
                         SymRef Wide) {
    if (!Narrow || !Wide || Ctx.width(Narrow) != Width ||
        Ctx.width(Wide) != WideWidth)
      return;
    if (std::none_of(Forms.begin(), Forms.end(), [&](const QuotientForm &F) {
          return F.Narrow == Narrow && F.Wide == Wide;
        }))
      Forms.push_back({Narrow, Wide});
  };

  // Iterate only nodes that belonged to the resolved selector expression.
  // Builders below intern derived comparison forms into the same context.
  const size_t ExpressionNodeCount = Ctx.numNodes();
  for (size_t NodeIndex = 0; NodeIndex < ExpressionNodeCount; ++NodeIndex) {
    SymRef Dividend(static_cast<uint32_t>(NodeIndex));
    if (Ctx.width(Dividend) != Width || Ctx.isConst(Dividend))
      continue;

    SymRef DivInput = shiftRight(Dividend, Magic.PreShift);
    SymRef WideInput = Ctx.mkZExt(DivInput, WideWidth);
    SymRef WideMagic = Ctx.mkConst(Magic.Magic.zextOrTrunc(WideWidth));
    SymRef FullProduct = Ctx.mkMul(WideMagic, WideInput);

    // Backends spell mulhi either as extract(full, W, W), or as a logical
    // shift followed by a low extract.  Keep both exact forms; no algebraic
    // approximation or numeric coincidence is accepted.
    std::vector<SymRef> HighForms;
    addUnique(HighForms, Ctx.mkExtract(FullProduct, Width, Width));
    addUnique(HighForms,
              Ctx.mkExtract(shiftRight(FullProduct, Width), 0, Width));

    std::vector<QuotientForm> Quotients;
    if (!Magic.IsAdd) {
      for (SymRef High : HighForms) {
        SymRef Narrow = shiftRight(High, Magic.PostShift);
        addQuotient(Quotients, Narrow, Ctx.mkZExt(Narrow, WideWidth));
      }
      // x86 commonly performs the post-shift directly on the full widened
      // product and keeps that W-bit quotient in a wide register until the
      // low-width back-multiply.
      SymRef Wide = shiftRight(FullProduct, Width + Magic.PostShift);
      addQuotient(Quotients, Ctx.mkExtract(Wide, 0, Width), Wide);
    } else {
      if (Magic.PreShift != 0)
        continue; // LLVM's IsAdd recipe and pre-shift are mutually exclusive.
      for (SymRef High : HighForms) {
        SymRef HalfDifference = shiftRight(Ctx.mkSub(DivInput, High), 1);
        SymRef Adjusted = Ctx.mkAdd(High, HalfDifference);
        SymRef Narrow = shiftRight(Adjusted, Magic.PostShift);
        addQuotient(Quotients, Narrow, Ctx.mkZExt(Narrow, WideWidth));
      }
    }

    auto lowWideProduct = [&](uint64_t Factor, SymRef WideQuotient) {
      SymRef Product = Ctx.mkMul(Ctx.mkConst(WideWidth, Factor), WideQuotient);
      return Ctx.mkExtract(Product, 0, Width);
    };
    auto matchesRemainder = [&](const QuotientForm &Q) {
      SymRef NarrowProduct = Ctx.mkMul(Ctx.mkConst(Width, Divisor), Q.Narrow);
      if (Ctx.mkSub(Dividend, NarrowProduct) == Remainder ||
          Ctx.mkSub(Dividend, lowWideProduct(Divisor, Q.Wide)) == Remainder)
        return true;

      // LEA often realizes q*(2^k-1) as q*2^k-q in the widened address
      // container.  Match that exact identity rather than treating an
      // arbitrary linear tree as a modulus.
      const uint64_t Above = llvm::PowerOf2Ceil(Divisor);
      if (Above > Divisor && Above - Divisor == 1) {
        SymRef Expected = Ctx.mkSub(Ctx.mkAdd(Dividend, Q.Narrow),
                                    lowWideProduct(Above, Q.Wide));
        if (Expected == Remainder)
          return true;
      }
      if (Divisor > 1 && llvm::isPowerOf2_64(Divisor - 1)) {
        const uint64_t Below = Divisor - 1;
        SymRef Expected = Ctx.mkSub(
            Dividend, Ctx.mkAdd(Q.Narrow, lowWideProduct(Below, Q.Wide)));
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
mergeResolverResults(const std::vector<ResolverResult> &Incoming,
                     const std::string &MergeRoot = {},
                     bool IgnoreTransparentCycles = false) {
  ResolverValue Common;
  std::vector<ResolverValue> Values;
  bool SawValue = false;
  bool SawCycle = false;
  for (const ResolverResult &R : Incoming) {
    if (R.Kind == ResolverResultKind::Cycle) {
      SawCycle = true;
      continue;
    }
    if (R.Kind != ResolverResultKind::Value || !R.Value)
      return resolverInvalid();
    if (!SawValue) {
      Common = R.Value;
      SawValue = true;
    }
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
  if (std::all_of(Values.begin(), Values.end(), [&](const ResolverValue &V) {
        return sameResolverValue(Common, V);
      }))
    return resolverValue(Common);
  if (MergeRoot.empty())
    return resolverInvalid();
  const uint16_t Size = Common ? Common->Size : 0;
  if (Size == 0 ||
      std::any_of(Values.begin(), Values.end(), [&](const ResolverValue &V) {
        return !V || V->Size != Size;
      }))
    return resolverInvalid();
  return resolverValue(resolverMerge(Size, MergeRoot, std::move(Values)));
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

/// Resolve a LOAD address of the form INT_ADD(base, index*scale) into its
/// base register, requiring a genuine scaled index so plain pointer loads
/// are not mistaken for tables.
bool CFGBuilder::analyzeTableLoadAddr(const std::vector<LowOp> &Ops,
                                      int FromIdx, const NdVar &AddrV,
                                      uint64_t &BaseReg, uint64_t &IndexReg,
                                      bool &HasScaledIndex, uint64_t &Disp,
                                      va_t *AddrAddVA, NdVar *IndexValue,
                                      va_t *IndexUseAddr,
                                      int *IndexUseSeq) const {
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
    const std::vector<JumpTableValueQuery> &Queries,
    bool *AnalysisComplete) const {
  std::vector<bool> Results(Queries.size(), false);
  if (AnalysisComplete)
    *AnalysisComplete = false;
  RequestedCompleteJumpTableProof = true;
  if (!JumpTableProofContextComplete || !CurrentImg || Queries.empty())
    return Results;
  bool Complete = true;

  std::vector<ResolverInsnSnapshot> Snapshot;
  Snapshot.reserve(Insns.size());
  for (const auto &[Addr, Rec] : Insns) {
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
    S.JumpTableTargets = Rec.JumpTableTargets;
    Snapshot.push_back(std::move(S));
  }
  const std::set<va_t> &ProofRoots = ActiveJumpTableProofRoots
                                         ? *ActiveJumpTableProofRoots
                                         : PersistentCFGRoots;
  const ResolverFlowGraph Graph = buildResolverFlowGraph(
      Snapshot, BlockStarts, ProofRoots, DiscoveredCodeRefSources,
      [&](va_t Address, const std::set<va_t> *ActiveOwners) {
        return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners);
      });
  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
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
  std::map<ValueKey, ResolverResult> ValueMemo;
  std::set<ValueKey> ActiveValues;
  std::map<MemoryKey, ResolverResult> MemoryMemo;
  std::set<MemoryKey> ActiveMemory;

  std::function<ResolverResult(int, int, const NdVar &)> resolveValue;
  std::function<ResolverResult(int, int, uint64_t, int64_t, uint16_t)>
      resolveMemory;

  auto constantValue = [](const NdVar &V) -> ResolverResult {
    if (!V.isConst() || V.Size == 0)
      return resolverInvalid();
    return resolverValue(
        resolverConstant(V.Offset, V.Size, V.Provenance, V.AddressOwnerVA));
  };
  auto resolveOperand = [&](int Block, int Before,
                            const NdVar &V) -> ResolverResult {
    return V.isConst() ? constantValue(V) : resolveValue(Block, Before, V);
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
    const Segment *SlotSegment = CurrentImg->getSegmentFor(Slot);
    if (!SlotSegment || !SlotSegment->isReadable() || SlotSegment->isWritable())
      return {};
    const RelocationEntry *Relocation = nullptr;
    for (const RelocationEntry &Candidate : CurrentImg->Relocations)
      if (Candidate.Address == Slot &&
          Candidate.Type == llvm::ELF::R_ARM_REL32) {
        if (Relocation)
          return {};
        Relocation = &Candidate;
      }
    if (!Relocation || Relocation->SymbolName.empty())
      return {};
    const Symbol *Target = CurrentImg->findSymbol(Relocation->SymbolName);
    if (!Target)
      return {};
    va_t Owner = InvalidVA;
    if (const Section *TargetSection = CurrentImg->getSectionFor(Target->Addr))
      Owner = TargetSection->VA;
    else if (const Segment *TargetSegment =
                 CurrentImg->getSegmentFor(Target->Addr))
      Owner = TargetSegment->VA;
    if (Owner == InvalidVA)
      return {};
    const uint8_t *Bytes = CurrentImg->readVA(Slot, Size);
    if (!Bytes)
      return {};
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, Bytes, sizeof(Encoded));
    return resolverConstant(Encoded, Size,
                            ConstantAddressProvenance::AddressFragment, Owner);
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
          Full, static_cast<uint16_t>(QBegin - OBegin), Query.Size));
    if (!ZeroExtendingWrite)
      return resolverInvalid();
    // A W/E-register write defines the full X/R register.  Bytes above the
    // narrow output become zero; a full-width read is the explicit zero-
    // extension of the written value.  Arbitrary cross-boundary subviews are
    // rejected rather than reconstructed heuristically.
    if (QBegin >= OEnd && QEnd <= Output.ContainerSize)
      return resolverValue(resolverZero(Query.Size));
    if (OBegin == 0 && QBegin == 0 && QEnd == Output.ContainerSize)
      return resolverValue(resolverExtend(Full, Query.Size, false));
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
  std::map<FrameKey, FrameResult> FrameMemo;
  std::set<FrameKey> ActiveFrames;
  std::function<FrameResult(int, int, uint64_t)> resolveFrameBase;
  std::function<FrameResult(int, int, const NdVar &)> resolveFrameVar;

  auto mergeFrameResults = [&](const std::vector<FrameResult> &Incoming,
                               bool IgnoreTransparentCycles) {
    bool SawCycle = false;
    bool SawValue = false;
    int64_t Common = 0;
    int64_t CycleDelta = 0;
    for (const FrameResult &R : Incoming) {
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

  resolveFrameVar = [&](int Block, int Before,
                        const NdVar &Value) -> FrameResult {
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0)
      return frameInvalid();
    if (Value.isReg())
      return TRI.isFrameReg(Value.Offset)
                 ? resolveFrameBase(Block, Before, Value.Offset)
                 : frameInvalid();
    if (!Value.isTemp())
      return frameInvalid();
    const std::vector<LowOp> &Ops = Graph.Blocks[Block].Ops;
    const int DefIndex = reachingDefIdx(
        Ops, std::min(Before, static_cast<int>(Ops.size())) - 1, Value);
    if (DefIndex < 0)
      return frameInvalid();
    const LowOp &Def = Ops[DefIndex];
    if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1)
      return resolveFrameVar(Block, DefIndex, Def.Inputs[0]);
    if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
        Def.NumInputs >= 2) {
      if (Def.Inputs[1].isConst())
        return adjustFrame(resolveFrameVar(Block, DefIndex, Def.Inputs[0]),
                           Def.Inputs[1], Def.Output.Size,
                           Def.Opcode == NdOp::INT_SUB);
      if (Def.Opcode == NdOp::INT_ADD && Def.Inputs[0].isConst())
        return adjustFrame(resolveFrameVar(Block, DefIndex, Def.Inputs[1]),
                           Def.Inputs[0], Def.Output.Size, false);
    }
    return frameInvalid();
  };

  resolveFrameBase = [&](int Block, int Before,
                         uint64_t BaseReg) -> FrameResult {
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0 || !TRI.isFrameReg(BaseReg))
      return frameInvalid();
    FrameKey Key{Block, Before, BaseReg};
    if (auto It = FrameMemo.find(Key); It != FrameMemo.end())
      return It->second;
    if (!ActiveFrames.insert(Key).second)
      return frameCycle();

    const ResolverFlowBlock &B = Graph.Blocks[Block];
    const LaneView Query = viewOf(NdVar::reg(
        BaseReg, static_cast<uint16_t>(CurrentImg->getPointerSize())));
    FrameResult Result = frameInvalid();
    bool Found = false;
    for (int I = std::min(Before, static_cast<int>(B.Ops.size())) - 1; I >= 0;
         --I) {
      const LowOp &Def = B.Ops[I];
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
        Result = resolveFrameVar(Block, I, Def.Inputs[0]);
      } else if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
                 Def.NumInputs >= 2) {
        if (Def.Inputs[1].isConst())
          Result = adjustFrame(resolveFrameVar(Block, I, Def.Inputs[0]),
                               Def.Inputs[1], Def.Output.Size,
                               Def.Opcode == NdOp::INT_SUB);
        else if (Def.Opcode == NdOp::INT_ADD && Def.Inputs[0].isConst())
          Result = adjustFrame(resolveFrameVar(Block, I, Def.Inputs[1]),
                               Def.Inputs[0], Def.Output.Size, false);
      }
      if (Graph.InstructionGuards.count(Def.Addr)) {
        // A predicated frame-register write is a merge of the old and new
        // epochs.  Unless both paths prove the same canonical offset, the
        // frame identity is ambiguous and must fail closed.
        Result =
            mergeFrameResults({resolveFrameBase(Block, I, BaseReg), Result},
                              /*IgnoreTransparentCycles=*/false);
      }
      break;
    }

    if (!Found) {
      std::vector<FrameResult> Incoming;
      Incoming.reserve(B.Preds.size() + 1);
      if (std::find(Graph.RootBlocks.begin(), Graph.RootBlocks.end(), Block) !=
          Graph.RootBlocks.end()) {
        if (B.Start == CurrentFuncEntry && BaseReg == TRI.StackPointer)
          Incoming.push_back(frameValue(0));
        else
          // A disconnected/address-taken root has an independent incoming
          // frame state.  It cannot borrow the function entry's spill slots.
          Incoming.push_back(frameInvalid());
      }
      for (int Pred : B.Preds)
        Incoming.push_back(resolveFrameBase(
            Pred, static_cast<int>(Graph.Blocks[Pred].Ops.size()), BaseReg));
      Result = Incoming.empty()
                   ? frameInvalid()
                   : mergeFrameResults(Incoming,
                                       /*IgnoreTransparentCycles=*/true);
    }

    ActiveFrames.erase(Key);
    if (Result.Kind != FrameResultKind::Cycle)
      FrameMemo[Key] = Result;
    return Result;
  };

  auto canonicalFrameSlotKey = [&](int Block, int FromIdx, const NdVar &Address,
                                   uint64_t &BaseReg, int64_t &Offset) {
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()))
      return false;
    NdVar FrameAddress = Address;
    int FrameFrom = FromIdx;
    // The x86-32 lifter performs the effective-address arithmetic in the
    // complete 32-bit guest domain, then widens that value to the common
    // internal VA width.  This widening preserves the frame epoch.  Peel it
    // only here, before the point-sensitive frame-base proof; a narrow ESP/WSP
    // write in a wider guest does not qualify because its input is smaller
    // than the architectural pointer width.
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (FrameAddress.isReg() && TRI.isFrameReg(FrameAddress.Offset))
        break;
      if (!FrameAddress.isReg() && !FrameAddress.isTemp())
        break;
      const int DefIdx =
          reachingDefIdx(Graph.Blocks[Block].Ops, FrameFrom, FrameAddress);
      if (DefIdx < 0)
        break;
      const LowOp &Def = Graph.Blocks[Block].Ops[DefIdx];
      if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
        FrameAddress = Def.Inputs[0];
        FrameFrom = DefIdx - 1;
        continue;
      }
      if (Def.Opcode == NdOp::INT_ZEXT && Def.NumInputs >= 1 && CurrentImg &&
          Def.Inputs[0].Size == CurrentImg->getPointerSize() &&
          Def.Output.Size > Def.Inputs[0].Size) {
        FrameAddress = Def.Inputs[0];
        FrameFrom = DefIdx - 1;
        continue;
      }
      break;
    }
    uint64_t PhysicalBase = InvalidVA;
    int64_t LocalOffset = 0;
    if (!frameSlotKey(Graph.Blocks[Block].Ops, FrameFrom, FrameAddress, TRI,
                      PhysicalBase, LocalOffset))
      return false;
    FrameResult BaseState =
        resolveFrameBase(Block, FrameFrom + 1, PhysicalBase);
    if (BaseState.Kind != FrameResultKind::Value)
      return false;
    const std::optional<int64_t> Canonical =
        checkedFrameOffset(BaseState.Offset, LocalOffset, false);
    if (!Canonical)
      return false;
    BaseReg = TRI.StackPointer;
    Offset = *Canonical;
    return true;
  };

  auto definitelyNonFrameAddress = [&](const std::vector<LowOp> &Ops,
                                       int Before, NdVar Address) {
    std::set<std::pair<VnodeSpace, uint64_t>> Seen;
    std::function<bool(NdVar, int, int)> Walk = [&](NdVar V, int From,
                                                    int Depth) -> bool {
      if (Depth >= limits::kMaxQuasiCopyDepth)
        return false;
      if (V.isConst())
        return isExactAddressProvenance(V.Provenance);
      if (V.isReg() && TRI.isFrameReg(V.Offset))
        return false;
      if (!V.isReg() && !V.isTemp())
        return false;
      if (!Seen.insert({V.Space, V.Offset}).second)
        return false;
      int D = reachingDefIdx(Ops, From, V);
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

  resolveMemory = [&](int Block, int Before, uint64_t SlotBase,
                      int64_t SlotOffset, uint16_t Size) -> ResolverResult {
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0 || Size == 0)
      return resolverInvalid();
    MemoryKey Key{Block, Before, SlotBase, SlotOffset, Size};
    if (auto It = MemoryMemo.find(Key); It != MemoryMemo.end())
      return It->second;
    if (!ActiveMemory.insert(Key).second)
      return resolverCycle();

    const ResolverFlowBlock &B = Graph.Blocks[Block];
    ResolverResult Result = resolverInvalid();
    bool Found = false;
    for (int I = std::min(Before, static_cast<int>(B.Ops.size())) - 1; I >= 0;
         --I) {
      const LowOp &Op = B.Ops[I];
      // LowIR has no callee memory-effect summary.  A call (direct, indirect,
      // or predicated) can mutate a frame slot whose address escaped earlier;
      // an opaque side-effect intrinsic such as SVC/HVC/SMC has the same
      // contract.  Do not trace a reload through either to an older STORE.
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
      ResolverResult StoredValue = resolveOperand(Block, I, Stored);
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
                        static_cast<uint16_t>(SlotOffset - StoreOffset), Size));
      if (Graph.InstructionGuards.count(Op.Addr)) {
        ResolverResult Old =
            resolveMemory(Block, I, SlotBase, SlotOffset, Size);
        Result = mergeResolverResults({Old, Result});
      }
      break;
    }

    if (!Found) {
      std::vector<ResolverResult> Incoming;
      Incoming.reserve(B.Preds.size());
      for (int Pred : B.Preds)
        Incoming.push_back(
            resolveMemory(Pred, static_cast<int>(Graph.Blocks[Pred].Ops.size()),
                          SlotBase, SlotOffset, Size));
      Result = Incoming.empty()
                   ? resolverInvalid()
                   : mergeResolverResults(Incoming,
                                          "M:" + std::to_string(B.Start) + ":" +
                                              std::to_string(SlotBase) + ":" +
                                              std::to_string(SlotOffset),
                                          /*IgnoreTransparentCycles=*/true);
    }
    ActiveMemory.erase(Key);
    if (Result.Kind != ResolverResultKind::Cycle)
      MemoryMemo[Key] = Result;
    return Result;
  };

  resolveValue = [&](int Block, int Before, const NdVar &V) -> ResolverResult {
    if (Block < 0 || Block >= static_cast<int>(Graph.Blocks.size()) ||
        Before < 0 || V.Size == 0 || (!V.isReg() && !V.isTemp()))
      return resolverInvalid();
    ValueKey Key{Block, Before, static_cast<uint8_t>(V.Space), V.Offset,
                 V.Size};
    if (auto It = ValueMemo.find(Key); It != ValueMemo.end())
      return It->second;
    if (!ActiveValues.insert(Key).second)
      return resolverCycle();

    const ResolverFlowBlock &B = Graph.Blocks[Block];
    const LaneView Query = viewOf(V);
    ResolverResult Result = resolverInvalid();
    bool Found = false;
    for (int I = std::min(Before, static_cast<int>(B.Ops.size())) - 1; I >= 0;
         --I) {
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
        ResolverResult Input = resolveOperand(Block, I, Def.Inputs[0]);
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
            Full = resolverExtend(Input.Value, Def.Output.Size, false);
          else if (Def.Opcode == NdOp::INT_SEXT)
            Full = resolverExtend(Input.Value, Def.Output.Size, true);
          else if (Def.Inputs[0].Size == Def.Output.Size)
            Full = Input.Value;
          else if (Def.Inputs[0].Size < Def.Output.Size)
            Full = resolverExtend(Input.Value, Def.Output.Size, false);
          else
            Full = resolverSlice(Input.Value, 0, Def.Output.Size);
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
          ResolverResult Input = resolveOperand(Block, I, InputView);
          if (Input.Kind == ResolverResultKind::Value)
            Full = IsGuestPointerLane ? Input.Value
                                      : resolverSlice(Input.Value, SliceOffset,
                                                      Def.Output.Size);
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
        ResolverResult Left = resolveOperand(Block, I, LeftInput);
        ResolverResult Right = resolveOperand(Block, I, RightInput);
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
            auto HasAppliedI386GOTPCInInstruction = [&]() {
              if (!CurrentImg->isELF() || CurrentImg->Arch != Arch::X86)
                return false;
              const auto InsnIt = Insns.find(Def.Addr);
              if (InsnIt == Insns.end() || InsnIt->second.Size == 0 ||
                  InsnIt->second.Size > InvalidVA - Def.Addr)
                return false;
              const va_t End = Def.Addr + InsnIt->second.Size;
              auto It = CurrentImg->I386GOTPCFields.lower_bound(Def.Addr);
              return It != CurrentImg->I386GOTPCFields.end() && It->first < End;
            };
            auto OwnerContains = [&](uint64_t Candidate,
                                     uint64_t Owner) -> bool {
              if (Owner == InvalidVA)
                return false;
              if (const Section *Sec = CurrentImg->getSectionFor(Owner))
                return Sec->Size <= InvalidVA - Sec->VA &&
                       Candidate >= Sec->VA && Candidate < Sec->VA + Sec->Size;
              if (const Segment *Seg = CurrentImg->getSegmentFor(Owner))
                return Seg->Size <= InvalidVA - Seg->VA &&
                       Candidate >= Seg->VA && Candidate < Seg->VA + Seg->Size;
              return false;
            };
            auto ExactOwnerProvenance = [&](uint64_t Candidate) {
              if (CurrentImg->hasExecutableCodeOwnerAt(Candidate))
                return ConstantAddressProvenance::CodeAddress;
              if (CurrentImg->hasObjectDataProvenance(Candidate))
                return ConstantAddressProvenance::DataAddress;
              return ConstantAddressProvenance::Address;
            };
            const bool I386ModelZero =
                Def.Opcode == NdOp::INT_ADD && CurrentImg->Arch == Arch::X86 &&
                (Value & resolverWidthMask(EvalSize)) == 0 &&
                HasAppliedI386GOTPCInInstruction();
            if (I386ModelZero && ((LeftAddress && RightScalar) ||
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
            } else if (Def.Opcode == NdOp::INT_ADD && LeftAddress &&
                       RightFragment && Right.Value->Constant == 0) {
              // i386's loader models GOT at zero.  Adding that exact model-zero
              // fragment to a GOTOFF-authenticated address preserves the latter
              // occurrence and owner; nonzero/ownerless fragments remain
              // incomplete.
              Provenance = Left.Value->Provenance;
              Owner = Left.Value->AddressOwnerVA;
            } else if (Def.Opcode == NdOp::INT_ADD && RightAddress &&
                       LeftFragment && Left.Value->Constant == 0) {
              Provenance = Right.Value->Provenance;
              Owner = Right.Value->AddressOwnerVA;
            } else if (LeftAddress || RightAddress) {
              // Address+address, scalar-address, or an address combined with an
              // untyped immediate is not an exact address occurrence.
              Provenance = ConstantAddressProvenance::AddressFragment;
            }
            Full = resolverConstant(Value, EvalSize, Provenance, Owner);
          }
        }
      } else if (Def.Opcode == NdOp::SELECT && Def.NumInputs >= 3) {
        ResolverResult TrueValue = resolveOperand(Block, I, Def.Inputs[1]);
        ResolverResult FalseValue = resolveOperand(Block, I, Def.Inputs[2]);
        if (TrueValue.Kind == ResolverResultKind::Value &&
            FalseValue.Kind == ResolverResultKind::Value) {
          ResolverResult Merged = mergeResolverResults(
              {TrueValue, FalseValue},
              "Q:" + std::to_string(Def.Addr) + ":" + std::to_string(Def.Seq));
          if (Merged.Kind == ResolverResultKind::Value)
            Full = Merged.Value;
        }
      } else if (Def.Opcode == NdOp::LOAD && Def.NumInputs >= 1) {
        const NdVar &Address =
            Def.NumInputs >= 2 ? Def.Inputs[1] : Def.Inputs[0];
        ResolverResult AddressValue = resolveOperand(Block, I, Address);
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
              resolveMemory(Block, I, SlotBase, SlotOffset, Def.Output.Size);
          if (Loaded.Kind == ResolverResultKind::Value)
            Full = Loaded.Value;
          else
            // Even when the frame-memory origin is deliberately opaque (an
            // atomic/call/unknown-alias barrier), this particular LOAD still
            // defines one stable SSA-like occurrence.  Two later uses of that
            // same occurrence may be compared; a distinct reload gets a
            // distinct root and therefore cannot borrow stale guard evidence.
            Full = resolverRoot(
                Def.Output.Size,
                "D:" + std::to_string(Def.Addr) + ":" +
                    std::to_string(Def.Seq) + ":" +
                    std::to_string(static_cast<unsigned>(Def.Output.Space)) +
                    ":" + std::to_string(Def.Output.Offset) + ":" +
                    std::to_string(Def.Output.Size));
        } else if (!Full) {
          Full = resolverRoot(
              Def.Output.Size,
              "D:" + std::to_string(Def.Addr) + ":" + std::to_string(Def.Seq) +
                  ":" +
                  std::to_string(static_cast<unsigned>(Def.Output.Space)) +
                  ":" + std::to_string(Def.Output.Offset) + ":" +
                  std::to_string(Def.Output.Size));
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
        Dependencies.reserve(Def.NumInputs);
        for (int InputNo = 0; InputNo < Def.NumInputs; ++InputNo) {
          ResolverResult Input = resolveOperand(Block, I, Def.Inputs[InputNo]);
          if (Input.Kind == ResolverResultKind::Value && Input.Value) {
            Dependencies.push_back(Input.Value);
          } else {
            const uint16_t InputSize = Def.Inputs[InputNo].Size != 0
                                           ? Def.Inputs[InputNo].Size
                                           : Def.Output.Size;
            Dependencies.push_back(
                resolverRoot(InputSize, "U:" + std::to_string(Def.Addr) + ":" +
                                            std::to_string(Def.Seq) + ":" +
                                            std::to_string(InputNo)));
          }
        }
        Full = resolverTransform(
            Def.Output.Size,
            "T:" + std::to_string(static_cast<unsigned>(Def.Opcode)) + ":" +
                std::to_string(Def.Addr) + ":" + std::to_string(Def.Seq),
            std::move(Dependencies), Def.Opcode);
      }
      if (!Full)
        Full = resolverRoot(
            Def.Output.Size,
            "D:" + std::to_string(Def.Addr) + ":" + std::to_string(Def.Seq) +
                ":" + std::to_string(static_cast<unsigned>(Def.Output.Space)) +
                ":" + std::to_string(Def.Output.Offset) + ":" +
                std::to_string(Def.Output.Size));

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
          ResolverResult Old = resolveValue(Block, I, V);
          std::vector<ResolverValue> Dependencies;
          Dependencies.reserve(2);
          if (Old.Kind == ResolverResultKind::Value && Old.Value)
            Dependencies.push_back(Old.Value);
          else
            Dependencies.push_back(resolverRoot(
                Query.Size,
                "POLD:" + std::to_string(Def.Addr) + ":" +
                    std::to_string(Def.Seq) + ":" +
                    std::to_string(static_cast<unsigned>(Query.Space)) + ":" +
                    std::to_string(Query.Container) + ":" +
                    std::to_string(Query.Begin) + ":" +
                    std::to_string(Query.Size)));
          Dependencies.push_back(Full);
          Result = resolverValue(resolverTransform(
              Query.Size,
              "P:" + std::to_string(Def.Addr) + ":" + std::to_string(Def.Seq) +
                  ":" + std::to_string(Output.Begin) + ":" +
                  std::to_string(Output.Size),
              std::move(Dependencies)));
        }
      } else if (Result.Kind != ResolverResultKind::Cycle) {
        // A partial-register write does not expose the untouched bytes as one
        // reconstructible expression, but it does create a new, stable lane
        // state.  Name that state by the exact definition occurrence so uses
        // after the write agree with each other while no use before it can.
        Result = resolverValue(resolverRoot(
            Query.Size, "S:" + std::to_string(Def.Addr) + ":" +
                            std::to_string(Def.Seq) + ":" +
                            std::to_string(static_cast<unsigned>(Query.Space)) +
                            ":" + std::to_string(Query.Container) + ":" +
                            std::to_string(Query.Begin) + ":" +
                            std::to_string(Query.Size)));
      }
      if (Graph.InstructionGuards.count(Def.Addr)) {
        ResolverResult Old = resolveValue(Block, I, V);
        // Preserve both feasible values of a condition-executed definition.
        // Must-equality consumers below still require every arm to match, but
        // incomplete-domain checks can now ask whether *any* arm depends on a
        // predicated mask/offset producer.  Collapsing unequal arms to Invalid
        // erased exactly the unsafe path that the latter query must detect.
        Result = mergeResolverResults({Old, Result},
                                      "G:" + std::to_string(Def.Addr) + ":" +
                                          std::to_string(Def.Seq));
      }
      break;
    }

    if (!Found) {
      std::vector<ResolverResult> Incoming;
      Incoming.reserve(B.Preds.size() + 1);
      // Each disconnected CFG root has a distinct incoming register state.
      // The canonical function entry also retains its initial state when a
      // loop back-edge targets it.
      if (V.isReg() &&
          std::find(Graph.RootBlocks.begin(), Graph.RootBlocks.end(), Block) !=
              Graph.RootBlocks.end()) {
        Incoming.push_back(resolverValue(
            resolverRoot(V.Size, "L:" + std::to_string(B.Start) + ":" +
                                     std::to_string(V.Offset) + ":" +
                                     std::to_string(V.Size))));
      }
      for (int Pred : B.Preds)
        Incoming.push_back(resolveValue(
            Pred, static_cast<int>(Graph.Blocks[Pred].Ops.size()), V));
      Result =
          Incoming.empty()
              ? resolverInvalid()
              : mergeResolverResults(
                    Incoming,
                    "P:" + std::to_string(B.Start) + ":" +
                        std::to_string(static_cast<unsigned>(Query.Space)) +
                        ":" + std::to_string(Query.Container) + ":" +
                        std::to_string(Query.Begin),
                    /*IgnoreTransparentCycles=*/true);
    }

    ActiveValues.erase(Key);
    if (Result.Kind != ResolverResultKind::Cycle)
      ValueMemo[Key] = Result;
    return Result;
  };

  // All queries below share the same CFG snapshot and reaching-value/memory
  // memo tables.  This avoids rebuilding the whole proof graph for every
  // candidate/anchor pair in a large dispatch DAG.  One global work budget is
  // deliberately shared across the batch; exhaustion fails remaining queries
  // closed rather than turning attacker-controlled graph size into unbounded
  // analysis work.
  size_t MatchBudget = limits::kMaxJumpTableEntries;
  auto sameAllowedValue = [](const ResolverValue &Value,
                             const ResolverValue &Allowed,
                             bool RequireExactAddressOwner) {
    if (sameResolverValue(Value, Allowed))
      return true;
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

  auto provesUnsignedUpperBound = [&](const ResolverValue &Value,
                                      uint64_t Bound, bool &ProofComplete) {
    ProofComplete = false;
    if (!Value || Value->Size == 0 || Value->Size > sizeof(uint64_t) ||
        Bound == 0)
      return false;

    const uint32_t Width = uint32_t(Value->Size) * 8u;
    if (Width < 64 && Bound >= (uint64_t{1} << Width)) {
      ProofComplete = true;
      return true;
    }

    symbolic::SymContext Ctx;
    size_t Work = limits::kMaxJumpTableEvidenceWork;
    std::map<const ResolverValueExpr *, symbolic::SymRef> Memo;
    std::map<std::pair<std::string, uint32_t>, symbolic::SymRef> Variables;
    std::map<std::pair<std::string, size_t>, symbolic::SymRef> MergeSelectors;
    bool Exhausted = false;
    auto unknown = [&](const ResolverValue &Node) -> symbolic::SymRef {
      std::string Name = Node->Root;
      if (Name.empty())
        Name =
            "opaque:" + std::to_string(reinterpret_cast<uintptr_t>(Node.get()));
      const auto Key =
          std::make_pair(std::move(Name), uint32_t(Node->Size) * 8u);
      auto [It, Inserted] = Variables.try_emplace(Key);
      if (Inserted)
        It->second = Ctx.mkFreshVar(Key.second, "jt_value");
      return It->second;
    };

    std::function<symbolic::SymRef(const ResolverValue &, unsigned)> Symbolize =
        [&](const ResolverValue &Node, unsigned Depth) -> symbolic::SymRef {
      if (!Node || Node->Size == 0 || Node->Size > sizeof(uint64_t) ||
          Depth > limits::kMaxJumpTableGuardExpressionDepth || Work == 0) {
        Exhausted = true;
        return {};
      }
      if (auto It = Memo.find(Node.get()); It != Memo.end())
        return It->second;
      --Work;
      const uint32_t NodeWidth = uint32_t(Node->Size) * 8u;
      symbolic::SymRef Result;
      switch (Node->K) {
      case ResolverValueExpr::Kind::Root:
        Result = unknown(Node);
        break;
      case ResolverValueExpr::Kind::Constant:
        Result = Ctx.mkConst(NodeWidth, Node->Constant);
        break;
      case ResolverValueExpr::Kind::Zero:
        Result = Ctx.mkZero(NodeWidth);
        break;
      case ResolverValueExpr::Kind::ZeroExtend:
      case ResolverValueExpr::Kind::SignExtend: {
        symbolic::SymRef Input = Symbolize(Node->Input, Depth + 1);
        if (!Input || Ctx.width(Input) > NodeWidth)
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
        Result = Ctx.mkExtract(Input, static_cast<uint32_t>(Low), NodeWidth);
        break;
      }
      case ResolverValueExpr::Kind::Merge: {
        if (Node->Inputs.empty())
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
        auto SymbolizeArm = [&](const ResolverValue &Arm) {
          return Symbolize(HoistExtension ? Arm->Input : Arm, Depth + 1);
        };
        Result = SymbolizeArm(Node->Inputs.back());
        for (size_t I = Node->Inputs.size() - 1; Result && I > 0; --I) {
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
          std::string MergeRoot = Node->Root;
          if (MergeRoot.empty())
            MergeRoot = "anon:" +
                        std::to_string(reinterpret_cast<uintptr_t>(Node.get()));
          const auto Key = std::make_pair(std::move(MergeRoot), I - 1);
          auto [It, Inserted] = MergeSelectors.try_emplace(Key);
          if (Inserted)
            It->second = Ctx.mkFreshVar(1, "jt_merge");
          Result = Ctx.mkIte(It->second, Arm, Result);
        }
        // Extension distributes over a predecessor-select exactly.  The
        // resolver represents it arm-wise so each arm retains provenance;
        // canonicalize it back to zext/sext(merge) for bit-vector reasoning,
        // but only when every arm performs the same extension from the same
        // width.  Mixed or partially extended merges remain untouched.
        if (Result && HoistExtension) {
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
        Inputs.reserve(Node->Inputs.size());
        for (const ResolverValue &Input : Node->Inputs) {
          symbolic::SymRef Symbolic = Symbolize(Input, Depth + 1);
          if (!Symbolic) {
            Inputs.clear();
            break;
          }
          Inputs.push_back(Symbolic);
        }
        if (!Inputs.empty()) {
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
      if (Result)
        Memo[Node.get()] = Result;
      return Result;
    };

    symbolic::SymRef Index = Symbolize(Value, 0);
    if (!Index || Exhausted || Ctx.width(Index) != Width)
      return false;
    if (provesLLVMUnsignedModuloRecipe(Ctx, Index, Bound)) {
      ProofComplete = true;
      return true;
    }
    symbolic::SymRef Counterexample =
        Ctx.mkNot(Ctx.mkUlt(Index, Ctx.mkConst(Width, Bound)));
    solver::SolverOptions Options;
    Options.BuildModel = false;
    Options.Sat.MaxConflicts = limits::kMaxJumpTableGuardSolverConflicts;
    Options.Sat.MaxPropagations = limits::kMaxJumpTableGuardSolverPropagations;
    Options.Sat.MaxWatchVisits = limits::kMaxJumpTableGuardSolverWatchVisits;
    Options.Blast.MaxWidth = 64;
    Options.Blast.MaxGates = limits::kMaxJumpTableGuardSolverGates;
    const solver::SatResult Result =
        solver::checkSat(Ctx, Counterexample, nullptr, Options);
    ProofComplete = Result != solver::SatResult::Unknown;
    return Result == solver::SatResult::Unsat;
  };

  for (size_t QueryIndex = 0; QueryIndex < Queries.size(); ++QueryIndex) {
    const JumpTableValueQuery &Query = Queries[QueryIndex];
    if (Query.Candidate.Size == 0 ||
        (Query.Relation != JumpTableValueRelation::UnsignedLessThan &&
         Query.Alternatives.empty()))
      continue;

    ResolverResult CandidateValue;
    if (Query.Candidate.isConst()) {
      CandidateValue = constantValue(Query.Candidate);
    } else if (Query.Candidate.isReg() || Query.Candidate.isTemp()) {
      auto CandidatePoint = Graph.PointToOp.find({Query.UseAddr, Query.UseSeq});
      if (CandidatePoint == Graph.PointToOp.end())
        continue;
      const auto [CandidateBlock, CandidateBefore] = CandidatePoint->second;
      CandidateValue =
          resolveValue(CandidateBlock, CandidateBefore, Query.Candidate);
    } else {
      continue;
    }
    if (CandidateValue.Kind != ResolverResultKind::Value) {
      if (Query.Relation == JumpTableValueRelation::MayDepend ||
          Query.Relation == JumpTableValueRelation::UnsignedLessThan)
        Complete = false;
      continue;
    }

    if (Query.Relation == JumpTableValueRelation::UnsignedLessThan) {
      bool ProofComplete = false;
      Results[QueryIndex] = provesUnsignedUpperBound(
          CandidateValue.Value, Query.UnsignedUpperBound, ProofComplete);
      if (!ProofComplete)
        Complete = false;
      continue;
    }

    std::vector<ResolverValue> AllowedValues;
    AllowedValues.reserve(Query.Alternatives.size());
    for (const JumpTableValueOccurrence &Anchor : Query.Alternatives) {
      ResolverResult IndexValue;
      if (Anchor.Value.isConst()) {
        IndexValue = constantValue(Anchor.Value);
      } else if (Anchor.Value.isReg() || Anchor.Value.isTemp()) {
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
        IndexValue = resolveValue(IndexBlock, IndexBefore, Anchor.Value);
      }
      if (IndexValue.Kind != ResolverResultKind::Value) {
        if (Query.Relation == JumpTableValueRelation::MayDepend)
          Complete = false;
        continue;
      }
      AllowedValues.push_back(IndexValue.Value);
    }
    if (AllowedValues.empty())
      continue;

    std::map<const ResolverValueExpr *, bool> MatchMemo;
    std::set<const ResolverValueExpr *> ActiveMatches;
    bool QueryBudgetExhausted = false;
    std::function<bool(const ResolverValue &)> MatchesAllowed =
        [&](const ResolverValue &Value) {
          if (!Value)
            return false;
          if (MatchBudget == 0) {
            QueryBudgetExhausted = true;
            return false;
          }
          if (auto It = MatchMemo.find(Value.get()); It != MatchMemo.end())
            return It->second;
          if (!ActiveMatches.insert(Value.get()).second)
            return false;
          --MatchBudget;
          bool Matches = false;
          for (const ResolverValue &Allowed : AllowedValues) {
            if (sameAllowedValue(Value, Allowed,
                                 Query.RequireExactAddressOwner) ||
                (Query.AllowZeroExtension && Value->Size < Allowed->Size &&
                 sameAllowedValue(resolverExtend(Value, Allowed->Size, false),
                                  Allowed, Query.RequireExactAddressOwner)) ||
                (Query.AllowZeroExtension && Allowed->Size < Value->Size &&
                 sameAllowedValue(Value,
                                  resolverExtend(Allowed, Value->Size, false),
                                  Query.RequireExactAddressOwner)) ||
                (Query.AllowSignExtension && Value->Size < Allowed->Size &&
                 sameAllowedValue(resolverExtend(Value, Allowed->Size, true),
                                  Allowed, Query.RequireExactAddressOwner)) ||
                (Query.AllowSignExtension && Allowed->Size < Value->Size &&
                 sameAllowedValue(Value,
                                  resolverExtend(Allowed, Value->Size, true),
                                  Query.RequireExactAddressOwner))) {
              Matches = true;
              break;
            }
          }
          if (!Matches && Query.Relation == JumpTableValueRelation::MayDepend) {
            if (Value->Input)
              Matches = MatchesAllowed(Value->Input);
            if (!Matches && !Value->Inputs.empty())
              Matches = std::any_of(Value->Inputs.begin(), Value->Inputs.end(),
                                    [&](const ResolverValue &Arm) {
                                      return MatchesAllowed(Arm);
                                    });
          } else if (!Matches && Value->K == ResolverValueExpr::Kind::Merge &&
                     !Value->Inputs.empty()) {
            Matches = std::all_of(
                Value->Inputs.begin(), Value->Inputs.end(),
                [&](const ResolverValue &Arm) { return MatchesAllowed(Arm); });
          }
          ActiveMatches.erase(Value.get());
          MatchMemo[Value.get()] = Matches;
          return Matches;
        };
    Results[QueryIndex] = MatchesAllowed(CandidateValue.Value);
    if (QueryBudgetExhausted)
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
    const InsnRecord &Rec, const JumpTableInfo &Info) const {
  RequestedCompleteJumpTableProof = true;
  if (!JumpTableProofContextComplete || !CurrentImg || Info.TargetLoads.empty())
    return false;

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };

  const LowOp *IndirectBranch = nullptr;
  for (const LowOp &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1)
      IndirectBranch = &Op;
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
  std::vector<DerivedOccurrence> Derived;
  const uint16_t PointerSize = CurrentImg->getPointerSize();
  if (PointerSize == 0)
    return false;
  for (const JumpTableValueOccurrence &Occurrence : Info.TargetLoads) {
    if (Occurrence.Addr == InvalidVA || Occurrence.Seq < 0 ||
        (!Occurrence.Value.isReg() && !Occurrence.Value.isTemp()) ||
        Occurrence.Value.Size == 0 || !Occurrence.DefinedAtPoint)
      return false;
    const LowOp *TargetLoad = nullptr;
    for (const auto &[Addr, Insn] : Insns)
      for (const LowOp &Op : Insn.Ops)
        if (Op.Addr == Occurrence.Addr && Op.Seq == Occurrence.Seq &&
            Op.Opcode == NdOp::LOAD && sameVar(Op.Output, Occurrence.Value)) {
          TargetLoad = &Op;
          break;
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

  auto alternativesFor = [&](uint8_t State) {
    std::vector<JumpTableValueOccurrence> Alternatives;
    for (const DerivedOccurrence &D : Derived)
      if (D.State == State)
        Alternatives.push_back(D.Occurrence);
    return Alternatives;
  };
  auto alreadyDerived = [&](const LowOp &Op, uint8_t State) {
    return std::any_of(
        Derived.begin(), Derived.end(), [&](const DerivedOccurrence &D) {
          return D.State == State && D.Occurrence.Addr == Op.Addr &&
                 D.Occurrence.Seq == Op.Seq &&
                 sameVar(D.Occurrence.Value, Op.Output);
        });
  };

  struct PendingTransform {
    const LowOp *Op = nullptr;
    uint8_t ResultState = TargetRaw;
    std::vector<size_t> QueryIndices;
  };

  auto exactAddressAlternative = [&](uint16_t Size) {
    return std::vector<JumpTableValueOccurrence>{
        {NdVar::address(ExpectedAnchor, Size), InvalidVA, -1,
         /*DefinedAtPoint=*/false}};
  };

  // Grow exact transform states, batching every point-sensitive value query in
  // a pass through one resolver session.  State transitions encode the table
  // role: absolute pointers never accept anchor/scale arithmetic; relative
  // entries must perform the declared extension, optional scale, and exactly
  // one target-anchor add before they can reach INDIR_BR.
  for (int Pass = 0; Pass < limits::kMaxSliceDepth; ++Pass) {
    std::vector<JumpTableValueQuery> Queries;
    std::vector<PendingTransform> Pending;
    auto addQuery = [&](const NdVar &Candidate, const LowOp &Use,
                        std::vector<JumpTableValueOccurrence> Alternatives) {
      const size_t Index = Queries.size();
      Queries.push_back({Candidate, Use.Addr, Use.Seq, std::move(Alternatives),
                         false, false});
      return Index;
    };
    auto addPending = [&](const LowOp &Op, uint8_t State,
                          std::vector<size_t> QueryIndices) {
      if (!alreadyDerived(Op, State))
        Pending.push_back({&Op, State, std::move(QueryIndices)});
    };

    for (const auto &[Addr, Insn] : Insns) {
      if (Insn.IsInstructionGuard)
        continue; // a predicated write is not a necessary target source
      for (const LowOp &Op : Insn.Ops) {
        if ((!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size == 0)
          continue;
        for (uint8_t State = TargetRaw; State <= RequiredState; ++State) {
          std::vector<JumpTableValueOccurrence> Alternatives =
              alternativesFor(State);
          if (Alternatives.empty())
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
              addPending(Op, Next, {addQuery(Op.Inputs[0], Op, Alternatives)});
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
              addPending(Op,
                         State | (relativeTargetTransformUsesPointerWidth(
                                      Op.Opcode, Op.Inputs[0].Size, 0,
                                      Op.Output.Size, PointerSize)
                                      ? TargetExtended
                                      : TargetRaw),
                         {addQuery(Op.Inputs[0], Op, Alternatives)});
            break;
          case NdOp::SUBBYTES:
            if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                Op.Inputs[1].Offset == 0 && Op.Inputs[0].Size == Op.Output.Size)
              addPending(Op, State, {addQuery(Op.Inputs[0], Op, Alternatives)});
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
                  addPending(Op, State | TargetScaled,
                             {addQuery(Op.Inputs[Side], Op, Alternatives)});
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
              addPending(Op, State | TargetScaled,
                         {addQuery(Op.Inputs[0], Op, Alternatives)});
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
                std::vector<size_t> Q = {
                    addQuery(Op.Inputs[Side], Op, Alternatives)};
                const NdVar &Anchor = Op.Inputs[1 - Side];
                if (Anchor.isConst()) {
                  if (!isExactAddressProvenance(Anchor.Provenance) ||
                      static_cast<va_t>(Anchor.Offset) != ExpectedAnchor)
                    continue;
                } else if (Anchor.isReg() || Anchor.isTemp()) {
                  Q.push_back(addQuery(Anchor, Op,
                                       exactAddressAlternative(Anchor.Size)));
                } else {
                  continue;
                }
                addPending(Op, State | TargetAnchored, std::move(Q));
              }
            }
            break;
          case NdOp::SELECT:
            if (Op.NumInputs >= 3)
              addPending(Op, State,
                         {addQuery(Op.Inputs[1], Op, Alternatives),
                          addQuery(Op.Inputs[2], Op, Alternatives)});
            break;
          default:
            break;
          }
        }
      }
    }

    if (Queries.empty())
      break;
    const std::vector<bool> QueryResults = tableValuesMatchAtUses(Queries);
    bool Changed = false;
    for (const PendingTransform &P : Pending) {
      if (!P.Op || !std::all_of(P.QueryIndices.begin(), P.QueryIndices.end(),
                                [&](size_t I) {
                                  return I < QueryResults.size() &&
                                         QueryResults[I];
                                }))
        continue;
      if (alreadyDerived(*P.Op, P.ResultState))
        continue;
      Derived.push_back({{P.Op->Output, P.Op->Addr, P.Op->Seq,
                          /*DefinedAtPoint=*/true},
                         P.ResultState});
      Changed = true;
    }
    if (!Changed)
      break;
  }

  std::vector<JumpTableValueOccurrence> FinalAlternatives =
      alternativesFor(RequiredState);
  if (FinalAlternatives.empty() ||
      IndirectBranch->Inputs[0].Size != PointerSize ||
      std::any_of(FinalAlternatives.begin(), FinalAlternatives.end(),
                  [&](const JumpTableValueOccurrence &Alternative) {
                    return Alternative.Value.Size != PointerSize;
                  }))
    return false;
  const std::vector<bool> Final = tableValuesMatchAtUses(
      {{IndirectBranch->Inputs[0], IndirectBranch->Addr, IndirectBranch->Seq,
        std::move(FinalAlternatives), false, false}});
  return !Final.empty() && Final.front();
}

bool CFGBuilder::tableLoadAddressesMatchRole(JumpTableInfo &Info) const {
  RequestedCompleteJumpTableProof = true;
  if (!JumpTableProofContextComplete || !CurrentImg || Info.LoadRoles.empty())
    return false;

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
      Info.LoadRoles.size() > 1) {
    std::vector<ResolverInsnSnapshot> Snapshot;
    Snapshot.reserve(Insns.size());
    for (const auto &[Addr, Rec] : Insns) {
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
      S.JumpTableTargets = Rec.JumpTableTargets;
      Snapshot.push_back(std::move(S));
    }
    const std::set<va_t> &ProofRoots = ActiveJumpTableProofRoots
                                           ? *ActiveJumpTableProofRoots
                                           : PersistentCFGRoots;
    const ResolverFlowGraph Graph = buildResolverFlowGraph(
        Snapshot, BlockStarts, ProofRoots, DiscoveredCodeRefSources,
        [&](va_t Address, const std::set<va_t> *ActiveOwners) {
          return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners);
        });
    auto IsReachable = [&](const JumpTableValueOccurrence &Occurrence) {
      return Occurrence.Addr != InvalidVA && Occurrence.Seq >= 0 &&
             Graph.PointToOp.count({Occurrence.Addr, Occurrence.Seq});
    };

    std::vector<JumpTableLoadRole> ReachableRoles;
    ReachableRoles.reserve(Info.LoadRoles.size());
    for (const JumpTableLoadRole &Role : Info.LoadRoles)
      if (IsReachable(Role.Load))
        ReachableRoles.push_back(Role);
    if (ReachableRoles.empty())
      return false;

    if (ReachableRoles.size() != Info.LoadRoles.size()) {
      Info.LoadRoles = std::move(ReachableRoles);
      std::set<std::pair<va_t, int>> ReachableLoads;
      std::vector<JumpTableValueOccurrence> ReachableIndices;
      for (const JumpTableLoadRole &Role : Info.LoadRoles) {
        ReachableLoads.emplace(Role.Load.Addr, Role.Load.Seq);
        for (const JumpTableValueOccurrence &Index : Role.Indices)
          if (std::find(ReachableIndices.begin(), ReachableIndices.end(),
                        Index) == ReachableIndices.end())
            ReachableIndices.push_back(Index);
      }
      Info.TargetLoads.erase(
          std::remove_if(Info.TargetLoads.begin(), Info.TargetLoads.end(),
                         [&](const JumpTableValueOccurrence &Load) {
                           return !ReachableLoads.count({Load.Addr, Load.Seq});
                         }),
          Info.TargetLoads.end());
      if (Info.TargetLoads.empty() || ReachableIndices.empty())
        return false;
      Info.IndexValueAlternatives = std::move(ReachableIndices);
      const JumpTableValueOccurrence &Index =
          Info.IndexValueAlternatives.front();
      Info.IndexValueAtUse = Index.Value;
      Info.IndexUseAddr = Index.Addr;
      Info.IndexUseSeq = Index.Seq;
      Info.IndexValueDefinedAtUse = Index.DefinedAtPoint;
      Info.TableLoadAddr = Info.LoadRoles.front().Load.Addr;
      Info.TableLoadSeq = Info.LoadRoles.front().Load.Seq;
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
  for (const auto &[Addr, Insn] : Insns) {
    if (Insn.IsInstructionGuard)
      continue;
    for (const LowOp &Op : Insn.Ops) {
      if (Op.Output.Size == 0)
        continue;
      const bool IsAndOne =
          Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2 &&
          ((Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 1) ||
           (Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 1));
      if (producesCanonicalBoolean(Op.Opcode) || IsAndOne)
        BooleanAlternatives.push_back(occurrenceFor(Op));
      else if ((Op.Opcode == NdOp::BOOL_AND || Op.Opcode == NdOp::BOOL_OR ||
                Op.Opcode == NdOp::BOOL_XOR) &&
               Op.NumInputs >= 2)
        BooleanCombiners.push_back(&Op);
    }
  }

  // BOOL_AND/OR/XOR are bitwise in the production emitter.  They preserve a
  // canonical boolean domain only when every input is already canonical, so
  // grow their output set with an occurrence-sensitive must fixed point rather
  // than trusting the opcode name.  COPY/ZEXT/SELECT/CFG merges are handled by
  // the shared reaching-value matcher itself.
  std::set<std::pair<va_t, int>> ProvenBooleanCombiners;
  for (unsigned Round = 0;
       Round < limits::kMaxQuasiCopyDepth && !BooleanCombiners.empty();
       ++Round) {
    struct PendingBoolean {
      const LowOp *Op = nullptr;
      std::vector<size_t> QueryIndices;
    };
    std::vector<JumpTableValueQuery> Queries;
    std::vector<PendingBoolean> Pending;
    for (const LowOp *Op : BooleanCombiners) {
      if (!Op || ProvenBooleanCombiners.count({Op->Addr, Op->Seq}))
        continue;
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
        Candidate.QueryIndices.push_back(Queries.size());
        Queries.push_back({Input, Op->Addr, Op->Seq, BooleanAlternatives,
                           /*AllowZeroExtension=*/true,
                           /*AllowSignExtension=*/false});
      }
      if (InputsCanBeBoolean)
        Pending.push_back(std::move(Candidate));
    }
    const std::vector<bool> Results = tableValuesMatchAtUses(Queries);
    bool Changed = false;
    for (const PendingBoolean &Candidate : Pending) {
      if (!Candidate.Op ||
          !std::all_of(
              Candidate.QueryIndices.begin(), Candidate.QueryIndices.end(),
              [&](size_t I) { return I < Results.size() && Results[I]; }))
        continue;
      ProvenBooleanCombiners.insert({Candidate.Op->Addr, Candidate.Op->Seq});
      BooleanAlternatives.push_back(occurrenceFor(*Candidate.Op));
      Changed = true;
    }
    if (!Changed)
      break;
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
  Roles.reserve(Info.LoadRoles.size());
  for (JumpTableLoadRole &Role : Info.LoadRoles) {
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
    for (const auto &[Addr, Insn] : Insns)
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
    if (!Load || Load->NumInputs < 1 || Load->Output.Size != Role.LoadWidth)
      return false;
    if (Role.HasBaseSelect && Role.HasBaseMaskBlend)
      return false;
    if (Role.HasBaseSelect) {
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

  auto baseAlternatives = [](const std::vector<va_t> &Bases, uint16_t Size) {
    std::vector<JumpTableValueOccurrence> Alternatives;
    Alternatives.reserve(Bases.size());
    for (va_t Base : Bases)
      Alternatives.push_back({NdVar::address(Base, Size), InvalidVA, -1,
                              /*DefinedAtPoint=*/false});
    return Alternatives;
  };

  constexpr size_t MaxProofQueries = limits::kMaxJumpTableEntries;
  auto pushQuery =
      [&](std::vector<JumpTableValueQuery> &Queries, const NdVar &Candidate,
          const LowOp &Use, std::vector<JumpTableValueOccurrence> Alternatives,
          bool AllowZeroExtension = false,
          bool AllowSignExtension = false) -> std::optional<size_t> {
    if (Queries.size() >= MaxProofQueries)
      return std::nullopt;
    const size_t Index = Queries.size();
    Queries.push_back({Candidate, Use.Addr, Use.Seq, std::move(Alternatives),
                       AllowZeroExtension, AllowSignExtension});
    return Index;
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
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    RoleState &State = Roles[RoleIndex];
    const JumpTableLoadRole &Role = *State.Role;
    if (Role.AddressScale == 1) {
      State.DynamicAlternatives = Role.Indices;
      continue;
    }
    for (const auto &[Addr, Insn] : Insns) {
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
        const bool IsExactRecordedUse = std::any_of(
            Role.Indices.begin(), Role.Indices.end(),
            [&](const JumpTableValueOccurrence &Index) {
              return !Index.DefinedAtPoint && Index.Addr == Scale.Addr &&
                     Index.Seq == Scale.Seq &&
                     sameVar(guestAddressView(Index.Value), IndexValue);
            });
        if (IsExactRecordedUse) {
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
        ScaleProofs.push_back({RoleIndex, &Scale, *Query});
      }
    }
  }
  if (!ScaleQueries.empty()) {
    const std::vector<bool> Results = tableValuesMatchAtUses(ScaleQueries);
    for (const ScaleProof &Proof : ScaleProofs) {
      if (Proof.QueryIndex < Results.size() && Results[Proof.QueryIndex])
        Roles[Proof.RoleIndex].DynamicAlternatives.push_back(
            {guestAddressView(Proof.Scale->Output), Proof.Scale->Addr,
             Proof.Scale->Seq, /*DefinedAtPoint=*/true});
    }
  }
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
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
    auto InsnIt = Insns.find(Use.Addr);
    if (InsnIt == Insns.end() || InsnIt->second.IsInstructionGuard)
      return false;
    std::vector<NdVar> Equivalent{guestAddressView(Source.Value)};
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
      const bool CopiesEquivalent =
          Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          Output.Size == guestAddressView(Op.Inputs[0]).Size &&
          std::any_of(Equivalent.begin(), Equivalent.end(),
                      [&](const NdVar &Value) {
                        return sameVar(Value, guestAddressView(Op.Inputs[0]));
                      });
      Equivalent.erase(std::remove_if(Equivalent.begin(), Equivalent.end(),
                                      [&](const NdVar &Value) {
                                        return overlaps(Value, Output);
                                      }),
                       Equivalent.end());
      if (CopiesEquivalent && std::none_of(Equivalent.begin(), Equivalent.end(),
                                           [&](const NdVar &Value) {
                                             return sameVar(Value, Output);
                                           }))
        Equivalent.push_back(Output);
    }
    const NdVar GuestUse = guestAddressView(UseValue);
    return std::any_of(
        Equivalent.begin(), Equivalent.end(),
        [&](const NdVar &Value) { return sameVar(Value, GuestUse); });
  };
  std::vector<JumpTableValueQuery> AddressQueries;
  std::vector<AddressProof> AddressProofs;
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    RoleState &State = Roles[RoleIndex];
    const JumpTableLoadRole &Role = *State.Role;
    if (Role.HasBaseSelect) {
      auto TrueQuery = pushQuery(
          AddressQueries, State.Select->Inputs[1], *State.Select,
          baseAlternatives({Role.TrueBase}, State.Select->Inputs[1].Size));
      auto FalseQuery = pushQuery(
          AddressQueries, State.Select->Inputs[2], *State.Select,
          baseAlternatives({Role.FalseBase}, State.Select->Inputs[2].Size));
      if (!TrueQuery || !FalseQuery)
        return false;
      State.SelectQueries = {*TrueQuery, *FalseQuery};
    } else if (Role.HasBaseMaskBlend) {
      const int PositiveOrSide = Role.PositiveBlendInputSide;
      const int NegativeOrSide = 1 - PositiveOrSide;
      const int PositiveBaseSide = Role.PositiveBaseInputSide;
      const int NegativeBaseSide = Role.NegativeBaseInputSide;
      auto PositiveOr =
          pushQuery(AddressQueries, State.Blend->Inputs[PositiveOrSide],
                    *State.Blend, {Role.PositiveBlendArm});
      auto NegativeOr =
          pushQuery(AddressQueries, State.Blend->Inputs[NegativeOrSide],
                    *State.Blend, {Role.NegativeBlendArm});
      auto PositiveBase = pushQuery(
          AddressQueries, State.PositiveAnd->Inputs[PositiveBaseSide],
          *State.PositiveAnd,
          baseAlternatives({Role.TrueBase},
                           State.PositiveAnd->Inputs[PositiveBaseSide].Size));
      auto PositiveMask = pushQuery(
          AddressQueries, State.PositiveAnd->Inputs[1 - PositiveBaseSide],
          *State.PositiveAnd, {Role.PositiveMask});
      auto NegativeBase = pushQuery(
          AddressQueries, State.NegativeAnd->Inputs[NegativeBaseSide],
          *State.NegativeAnd,
          baseAlternatives({Role.FalseBase},
                           State.NegativeAnd->Inputs[NegativeBaseSide].Size));
      auto NegativeMask = pushQuery(
          AddressQueries, State.NegativeAnd->Inputs[1 - NegativeBaseSide],
          *State.NegativeAnd, {Role.NegativeMask});
      auto Complement = pushQuery(AddressQueries, State.NegativeMask->Inputs[0],
                                  *State.NegativeMask, {Role.PositiveMask});
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
      State.SelectQueries = {*PositiveOr,   *NegativeOr,      *PositiveBase,
                             *PositiveMask, *NegativeBase,    *NegativeMask,
                             *Complement,   *BooleanCondition};
    }
    const NdVar &LoadAddress =
        State.Load->Inputs[State.Load->NumInputs >= 2 ? 1 : 0];
    const NdVar GuestLoadAddress = guestAddressView(LoadAddress);
    for (const auto &[Addr, Insn] : Insns) {
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
          if (!Role.HasBaseSelect && !Role.HasBaseMaskBlend &&
              BaseValue.isConst() &&
              std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                        static_cast<va_t>(BaseValue.Offset)) ==
                  Role.AllowedBases.end())
            continue;
          std::vector<size_t> ProofQueries = State.SelectQueries;
          const bool HasBaseMerge = Role.HasBaseSelect || Role.HasBaseMaskBlend;
          auto BaseQuery = HasBaseMerge
                               ? pushQuery(AddressQueries, BaseValue, Add,
                                           {Role.SelectedBase})
                               : pushQuery(AddressQueries, BaseValue, Add,
                                           baseAlternatives(Role.AllowedBases,
                                                            BaseValue.Size));
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
          ProofQueries.push_back(*BaseQuery);
          if (IndexQuery)
            ProofQueries.push_back(*IndexQuery);
          AddressProofs.push_back({RoleIndex,
                                   &Add,
                                   {DynamicValue, Add.Addr, Add.Seq,
                                    /*DefinedAtPoint=*/false},
                                   std::move(ProofQueries)});
        }
      }
    }
  }
  if (AddressQueries.empty() || AddressProofs.empty())
    return false;
  const std::vector<bool> AddressResults =
      tableValuesMatchAtUses(AddressQueries);
  std::vector<std::vector<JumpTableValueOccurrence>> AddressAlternatives(
      Roles.size());
  std::vector<std::vector<JumpTableValueOccurrence>> AddressIndexAlternatives(
      Roles.size());
  for (const AddressProof &Proof : AddressProofs) {
    if (!Proof.Add || Proof.RoleIndex >= AddressAlternatives.size() ||
        !std::all_of(Proof.QueryIndices.begin(), Proof.QueryIndices.end(),
                     [&](size_t I) {
                       return I < AddressResults.size() && AddressResults[I];
                     }))
      continue;
    JumpTableValueOccurrence Occurrence{guestAddressView(Proof.Add->Output),
                                        Proof.Add->Addr, Proof.Add->Seq,
                                        /*DefinedAtPoint=*/true};
    auto &Alternatives = AddressAlternatives[Proof.RoleIndex];
    if (std::none_of(Alternatives.begin(), Alternatives.end(),
                     [&](const JumpTableValueOccurrence &Existing) {
                       return Existing.Addr == Occurrence.Addr &&
                              Existing.Seq == Occurrence.Seq &&
                              sameVar(Existing.Value, Occurrence.Value);
                     }))
      Alternatives.push_back(Occurrence);
    auto &IndexAlternatives = AddressIndexAlternatives[Proof.RoleIndex];
    if (std::none_of(IndexAlternatives.begin(), IndexAlternatives.end(),
                     [&](const JumpTableValueOccurrence &Existing) {
                       return Existing.Addr == Proof.DynamicIndex.Addr &&
                              Existing.Seq == Proof.DynamicIndex.Seq &&
                              sameVar(Existing.Value, Proof.DynamicIndex.Value);
                     }))
      IndexAlternatives.push_back(Proof.DynamicIndex);
  }
  for (size_t I = 0; I < AddressAlternatives.size(); ++I)
    if (AddressAlternatives[I].empty()) {
      return false;
    }

  // A composite SelectOffset plan names the dynamic operand of one exact
  // address ADD.  The all-path LOAD proof intentionally accepts multiple ADD
  // alternatives for shared/diamond dispatches, but no single one of those
  // Med SSA inputs necessarily dominates the final branch.  Authenticate the
  // detector-recorded byte coordinate only when one address definition and
  // one dynamic input survived the role proof; otherwise clear it so
  // extraction publishes no composite plan and both backends fail closed.
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
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
  std::vector<bool> LoadMatches(Roles.size(), false);
  for (size_t RoleIndex = 0; RoleIndex < Roles.size(); ++RoleIndex) {
    const RoleState &State = Roles[RoleIndex];
    const NdVar &LoadAddress =
        State.Load->Inputs[State.Load->NumInputs >= 2 ? 1 : 0];
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
    LoadQueryRoles.push_back(RoleIndex);
  }
  const std::vector<bool> LoadResults = tableValuesMatchAtUses(LoadQueries);
  if (LoadResults.size() != LoadQueryRoles.size())
    return false;
  for (size_t I = 0; I < LoadResults.size(); ++I)
    if (LoadResults[I])
      LoadMatches[LoadQueryRoles[I]] = true;
  return std::all_of(LoadMatches.begin(), LoadMatches.end(),
                     [](bool Matched) { return Matched; });
}

std::set<va_t> CFGBuilder::candidateReachableInstructions(
    const InsnRecord &Candidate, const std::vector<va_t> &CandidateTargets,
    const std::set<va_t> &Roots,
    const std::vector<JumpTableStorageRange> &CandidateStorage) const {
  std::vector<ResolverInsnSnapshot> Snapshot;
  Snapshot.reserve(Insns.size());
  for (const auto &[Addr, Rec] : Insns) {
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
    S.JumpTableTargets =
        Addr == Candidate.Addr ? CandidateTargets : Rec.JumpTableTargets;
    Snapshot.push_back(std::move(S));
  }

  const ResolverFlowGraph Graph = buildResolverFlowGraph(
      Snapshot, BlockStarts, Roots, DiscoveredCodeRefSources,
      [&](va_t Address, const std::set<va_t> *ActiveOwners) {
        if (ActiveOwners && ActiveOwners->count(Candidate.Addr) &&
            std::any_of(CandidateStorage.begin(), CandidateStorage.end(),
                        [&](const JumpTableStorageRange &Range) {
                          return Range.ownsStorageAddress(Address);
                        }))
          return true;
        return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners);
      });

  std::set<va_t> Reachable;
  for (const auto &[Addr, Block] : Graph.InsnToBlock) {
    (void)Block;
    Reachable.insert(Addr);
  }
  return Reachable;
}

std::vector<std::optional<bool>>
CFGBuilder::tableLoadConditionValues(llvm::ArrayRef<va_t> BranchAddrs,
                                     const JumpTableInfo &Info,
                                     bool *AnalysisComplete) const {
  std::vector<std::optional<bool>> Results(BranchAddrs.size());
  if (AnalysisComplete)
    *AnalysisComplete = false;
  RequestedCompleteJumpTableProof = true;
  if (!JumpTableProofContextComplete || Info.TableLoadAddr == InvalidVA)
    return Results;
  if (BranchAddrs.empty()) {
    if (AnalysisComplete)
      *AnalysisComplete = true;
    return Results;
  }

  size_t WorkBudget = limits::kMaxJumpTableEvidenceWork;
  bool Complete = true;
  auto consumeWork = [&](size_t Amount = 1) {
    if (Amount > WorkBudget) {
      Complete = false;
      WorkBudget = 0;
      return false;
    }
    WorkBudget -= Amount;
    return true;
  };

  std::vector<ResolverInsnSnapshot> Snapshot;
  Snapshot.reserve(Insns.size());
  for (const auto &[Addr, Rec] : Insns) {
    if (!consumeWork(1 + Rec.Ops.size()))
      return Results;
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
    S.JumpTableTargets = Rec.JumpTableTargets;
    Snapshot.push_back(std::move(S));
  }

  std::map<va_t, const ResolverInsnSnapshot *> SnapshotByAddr;
  for (const ResolverInsnSnapshot &S : Snapshot)
    SnapshotByAddr.emplace(S.Addr, &S);
  const std::set<va_t> &ProofRoots = ActiveJumpTableProofRoots
                                         ? *ActiveJumpTableProofRoots
                                         : PersistentCFGRoots;
  const ResolverFlowGraph Graph = buildResolverFlowGraph(
      Snapshot, BlockStarts, ProofRoots, DiscoveredCodeRefSources,
      [&](va_t Address, const std::set<va_t> *ActiveOwners) {
        return resolvedJumpTableOwnsStorageAddress(Address, ActiveOwners);
      });
  auto LI = Graph.InsnToBlock.find(Info.TableLoadAddr);
  if (LI == Graph.InsnToBlock.end()) {
    if (AnalysisComplete)
      *AnalysisComplete = Complete;
    return Results;
  }
  const int LoadBlock = LI->second;

  auto reachable = [&](const std::vector<int> &Starts, int Target,
                       int Excluded) {
    std::vector<int> Work = Starts;
    std::set<int> Seen;
    while (!Work.empty()) {
      if (!consumeWork())
        return false;
      int B = Work.back();
      Work.pop_back();
      if (B == Excluded || !Seen.insert(B).second)
        continue;
      if (B == Target)
        return true;
      for (int S : Graph.Blocks[B].Succs)
        Work.push_back(S);
    }
    return false;
  };

  const std::vector<int> &Roots = Graph.RootBlocks;
  auto blockFor = [&](va_t Addr) -> int {
    auto It = Graph.InsnToBlock.find(Addr);
    return It == Graph.InsnToBlock.end() ? -1 : It->second;
  };

  if (Roots.empty() || !reachable(Roots, LoadBlock, -1)) {
    if (AnalysisComplete)
      *AnalysisComplete = Complete;
    return Results;
  }
  for (size_t QueryIndex = 0; QueryIndex < BranchAddrs.size(); ++QueryIndex) {
    if (!Complete)
      break;
    const va_t BranchAddr = BranchAddrs[QueryIndex];
    auto SnapshotIt = SnapshotByAddr.find(BranchAddr);
    auto BI = Graph.InsnToBlock.find(BranchAddr);
    if (BranchAddr == InvalidVA || SnapshotIt == SnapshotByAddr.end() ||
        BI == Graph.InsnToBlock.end())
      continue;
    const ResolverInsnSnapshot *BranchSnapshot = SnapshotIt->second;
    if (!BranchSnapshot->IsBranch || !BranchSnapshot->IsCond)
      continue;
    const int GuardBlock = BI->second;
    if (GuardBlock == LoadBlock || reachable(Roots, LoadBlock, GuardBlock))
      continue;

    const ResolverFlowBlock &Guard = Graph.Blocks[GuardBlock];
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
    for (const LowOp &Op : BranchSnapshot->Ops) {
      if (!consumeWork())
        break;
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
    if (!Complete || TrueTarget == InvalidVA)
      continue;
    if (BranchSnapshot->IsInstructionGuard && !GuardedTerminalEffect)
      continue;
    const int TrueBlock = blockFor(TrueTarget);
    const int FalseBlock = blockFor(FalseTarget);
    const bool TrueReaches =
        TrueBlock >= 0 && reachable({TrueBlock}, LoadBlock, GuardBlock);
    const bool FalseReaches = !GuardedTerminalEffect && FalseBlock >= 0 &&
                              reachable({FalseBlock}, LoadBlock, GuardBlock);
    if (Complete && TrueReaches != FalseReaches)
      Results[QueryIndex] = TrueReaches;
  }
  if (AnalysisComplete)
    *AnalysisComplete = Complete;
  return Results;
}

std::optional<bool>
CFGBuilder::tableLoadConditionValue(va_t BranchAddr,
                                    const JumpTableInfo &Info) const {
  bool Complete = false;
  const std::vector<std::optional<bool>> Results =
      tableLoadConditionValues({BranchAddr}, Info, &Complete);
  return Complete && Results.size() == 1 ? Results.front() : std::nullopt;
}

bool CFGBuilder::branchControlsTableLoad(va_t BranchAddr,
                                         const JumpTableInfo &Info) const {
  return tableLoadConditionValue(BranchAddr, Info).has_value();
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
