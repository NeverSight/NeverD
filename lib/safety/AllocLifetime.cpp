//===- AllocLifetime.cpp - Heap allocation lifetime audit ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/AllocLifetime.h"

#include "CopySemantics.h"
#include "SourceSemantics.h"
#include "StackSlotFlow.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/SinkScanner.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <tuple>

using namespace neverd;
using namespace neverd::safety;

namespace {

using ValueKey = std::tuple<uint8_t, int, int>;

ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

bool isStringLengthCall(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_CALL_TRAIT(NAME, IS_LENGTH, IS_BOUNDED)                         \
  .Case(NAME, IS_LENGTH != 0)
#include "neverd/safety/SafetyCallTraits.inc"
#undef SAFETY_CALL_TRAIT
      .Default(false);
}

std::optional<uint64_t> unsignedConstant(const MedVar &Value) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t))
    return std::nullopt;
  const unsigned Bits = static_cast<unsigned>(Value.Size) * 8;
  const uint64_t Mask = Bits == 64 ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t{1} << Bits) - 1;
  return Value.ConstVal & Mask;
}

bool mayForwardPointerInput(NdOp Op, unsigned Input) {
  switch (Op) {
  case NdOp::COPY:
  case NdOp::CAST:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::SUBBYTES:
  case NdOp::INT_SUB:
    return Input == 0;
  case NdOp::INT_ADD:
    return Input < 2;
  case NdOp::SELECT:
    return Input == 1 || Input == 2;
  default:
    return false;
  }
}

bool mustForwardPointerValue(const MedOp &Op,
                             const llvm::DenseSet<ValueKey> &Aliases) {
  auto aliases = [&](unsigned Input) {
    return Input < Op.NumInputs && !Op.Inputs[Input].isConst() &&
           Aliases.count(keyOf(Op.Inputs[Input]));
  };
  auto sameSize = [&](unsigned Input) {
    return Input < Op.NumInputs && Op.Output.Size == Op.Inputs[Input].Size;
  };
  auto isZero = [&](unsigned Input) {
    return Input < Op.NumInputs && Op.Inputs[Input].isConst() &&
           Op.Inputs[Input].ConstVal == 0;
  };

  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::CAST:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return aliases(0) && sameSize(0);
  case NdOp::SUBBYTES:
    return aliases(0) && sameSize(0) && isZero(1);
  case NdOp::INT_ADD:
    return (aliases(0) && sameSize(0) && isZero(1)) ||
           (isZero(0) && aliases(1) && sameSize(1));
  case NdOp::INT_SUB:
    return aliases(0) && sameSize(0) && isZero(1);
  case NdOp::SELECT:
    return aliases(1) && sameSize(1) && aliases(2) && sameSize(2);
  default:
    return false;
  }
}

// A per-function memory model that resolves stack-slot addresses and records
// the spill/reload traffic, so a handle kept in a local slot is still tracked
// across an unoptimised binary's stores and loads.
struct MemModel {
  const MedFunc &F;
  uint64_t SP = 0, FP = 0;
  bool Have = false;
  llvm::DenseMap<ValueKey, std::pair<int, int>> OpDef;
  llvm::DenseMap<ValueKey, std::pair<int, int>> PhiDef;

  MemModel(const MedFunc &Fn, const AnalysisInput &In) : F(Fn) {
    SP = In.StackPointerReg;
    FP = In.FramePointerReg;
    Have = In.StackRegsKnown;
    for (int Bi = 0; Bi < static_cast<int>(F.Blocks.size()); ++Bi) {
      for (int Pi = 0; Pi < static_cast<int>(F.Blocks[Bi].Phis.size()); ++Pi) {
        const PhiNode &Phi = F.Blocks[Bi].Phis[Pi];
        if (!Phi.Output.isConst() && Phi.Output.Size > 0)
          PhiDef[keyOf(Phi.Output)] = {Bi, Pi};
      }
      for (int Oi = 0; Oi < static_cast<int>(F.Blocks[Bi].Ops.size()); ++Oi) {
        const MedOp &O = F.Blocks[Bi].Ops[Oi];
        if (!O.Output.isConst() && O.Output.Size > 0)
          OpDef[keyOf(O.Output)] = {Bi, Oi};
      }
    }
  }

  const MedOp *def(const MedVar &V) const {
    auto It = OpDef.find(keyOf(V));
    return It == OpDef.end()
               ? nullptr
               : &F.Blocks[It->second.first].Ops[It->second.second];
  }

  std::optional<int64_t> stackOffset(const MedVar &V) const {
    llvm::DenseSet<ValueKey> Seen;
    return stackOffsetRec(V, 0, Seen);
  }

  std::optional<int64_t> stackOffsetRec(const MedVar &V, int Depth,
                                        llvm::DenseSet<ValueKey> &Seen) const {
    if (!Have || V.isConst() || Depth > 48 || !Seen.insert(keyOf(V)).second)
      return std::nullopt;
    const MedOp *Op = def(V);
    if (V.Kind == MedVar::Reg && (V.RegOff == SP || V.RegOff == FP)) {
      if (Op && (Op->Opcode == NdOp::INT_ADD || Op->Opcode == NdOp::INT_SUB))
        return affine(*Op, Depth, Seen);
      return V.RegOff == SP ? std::optional<int64_t>(0) : std::nullopt;
    }
    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      return Op->NumInputs >= 1 ? stackOffsetRec(Op->Inputs[0], Depth + 1, Seen)
                                : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return affine(*Op, Depth, Seen);
    default:
      return std::nullopt;
    }
  }

  std::optional<int64_t> affine(const MedOp &Op, int Depth,
                                llvm::DenseSet<ValueKey> &Seen) const {
    if (Op.NumInputs < 2)
      return std::nullopt;
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (Op.Inputs[1].isConst()) {
      auto Delta = detail::signedStackConstant(Op.Inputs[1]);
      if (auto B = stackOffsetRec(Op.Inputs[0], Depth + 1, Seen); B && Delta)
        return detail::checkedStackOffset(*B, *Delta, Sub);
    } else if (Op.Inputs[0].isConst() && !Sub) {
      auto Delta = detail::signedStackConstant(Op.Inputs[0]);
      if (auto B = stackOffsetRec(Op.Inputs[1], Depth + 1, Seen); B && Delta)
        return detail::checkedStackOffset(*B, *Delta, false);
    }
    return std::nullopt;
  }

  bool mayBeStackAddress(const MedVar &V, int Depth,
                         llvm::DenseSet<ValueKey> &Seen) const {
    if (!Have || V.isConst() || Depth > 64 || !Seen.insert(keyOf(V)).second)
      return false;
    if (V.Kind == MedVar::Reg && (V.RegOff == SP || V.RegOff == FP))
      return true;
    if (auto It = PhiDef.find(keyOf(V)); It != PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        llvm::DenseSet<ValueKey> BranchSeen = Seen;
        if (mayBeStackAddress(Arg, Depth + 1, BranchSeen))
          return true;
      }
      return false;
    }
    const MedOp *Op = def(V);
    if (!Op)
      return false;
    unsigned Begin = 0;
    unsigned End = Op->NumInputs;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
      End = std::min<unsigned>(End, 1);
      break;
    case NdOp::SELECT:
      Begin = std::min<unsigned>(1, End);
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      break;
    default:
      return false;
    }
    for (unsigned I = Begin; I < End; ++I) {
      llvm::DenseSet<ValueKey> BranchSeen = Seen;
      if (mayBeStackAddress(Op->Inputs[I], Depth + 1, BranchSeen))
        return true;
    }
    return false;
  }

  detail::ReachingStackValues reachingLoad(const MedBlock &B, int OpIdx,
                                           const MedOp &Op) const {
    if (!detail::isStackMemoryRead(Op.Opcode) || Op.NumInputs == 0 ||
        Op.Output.Size == 0)
      return {};
    const MedVar *Addr = detail::memoryAddress(Op);
    if (!Addr)
      return {};
    std::optional<int64_t> Off = stackOffset(*Addr);
    if (!Off)
      return {};
    auto Resolve = [&](const MedVar &V) { return stackOffset(V); };
    auto MayBeFrame = [&](const MedVar &V) {
      llvm::DenseSet<ValueKey> Seen;
      return mayBeStackAddress(V, 0, Seen);
    };
    return detail::reachingStackValues(F, B.Id, OpIdx, *Off, Op.Output.Size,
                                       Resolve, MayBeFrame);
  }

  detail::ReachingStackValues reachingRead(const MedBlock &B, int OpIdx,
                                           const MedVar &Addr,
                                           uint16_t Size) const {
    if (Size == 0)
      return {};
    std::optional<int64_t> Off = stackOffset(Addr);
    if (!Off)
      return {};
    auto Resolve = [&](const MedVar &V) { return stackOffset(V); };
    auto MayBeFrame = [&](const MedVar &V) {
      llvm::DenseSet<ValueKey> Seen;
      return mayBeStackAddress(V, 0, Seen);
    };
    return detail::reachingStackValues(F, B.Id, OpIdx, *Off, Size, Resolve,
                                       MayBeFrame,
                                       /*RequireMemoryRead=*/false);
  }

  llvm::DenseSet<ValueKey> aliasClosureImpl(const MedVar &Seed,
                                            bool RequireExactAlias) const {
    llvm::DenseSet<ValueKey> Set;
    Set.insert(keyOf(Seed));
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const MedBlock &B : F.Blocks) {
        for (const PhiNode &Phi : B.Phis) {
          if (Phi.Output.isConst() || Phi.Args.empty())
            continue;
          bool AnyAlias = false;
          bool AllAlias = true;
          for (const auto &[Pred, Arg] : Phi.Args) {
            (void)Pred;
            const bool Aliases = !Arg.isConst() && Set.count(keyOf(Arg));
            AnyAlias |= Aliases;
            AllAlias &= Aliases;
          }
          if ((RequireExactAlias ? AllAlias : AnyAlias) &&
              Set.insert(keyOf(Phi.Output)).second)
            Changed = true;
        }
        for (const MedOp &Op : B.Ops) {
          if (Op.Output.isConst() || Op.Output.Size == 0)
            continue;
          if (RequireExactAlias) {
            if (mustForwardPointerValue(Op, Set) &&
                Set.insert(keyOf(Op.Output)).second)
              Changed = true;
            continue;
          }
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (mayForwardPointerInput(Op.Opcode, I) &&
                !Op.Inputs[I].isConst() && Set.count(keyOf(Op.Inputs[I])) &&
                Set.insert(keyOf(Op.Output)).second)
              Changed = true;
        }
      }
      for (const MedBlock &B : F.Blocks)
        for (int Oi = 0; Oi < static_cast<int>(B.Ops.size()); ++Oi) {
          const MedOp &Op = B.Ops[Oi];
          if (!detail::isStackMemoryRead(Op.Opcode) || Op.Output.isConst() ||
              Op.Output.Size == 0)
            continue;
          detail::ReachingStackValues Reaching = reachingLoad(B, Oi, Op);
          if (!Reaching.Reachable)
            continue;
          bool AnyAlias = false;
          bool AllAlias = Reaching.Complete && !Reaching.Values.empty();
          for (const MedVar &Source : Reaching.Values)
            if (!Source.isConst() && Set.count(keyOf(Source)))
              AnyAlias = true;
            else
              AllAlias = false;
          if ((RequireExactAlias ? AllAlias : AnyAlias) &&
              Set.insert(keyOf(Op.Output)).second)
            Changed = true;
        }
    }
    return Set;
  }

  // Every value that may alias the seed, by SSA forwarding and by
  // spill/reload through a stack slot.
  llvm::DenseSet<ValueKey> aliasClosure(const MedVar &Seed) const {
    return aliasClosureImpl(Seed, /*RequireExactAlias=*/false);
  }

  // Values that are guaranteed to derive from the seed on every PHI arm.
  llvm::DenseSet<ValueKey> mustAliasClosure(const MedVar &Seed) const {
    return aliasClosureImpl(Seed, /*RequireExactAlias=*/true);
  }
};

struct CFG {
  const MedFunc &F;
  llvm::DenseMap<int, int> IdToIndex;

  explicit CFG(const MedFunc &Fn) : F(Fn) {
    for (int I = 0; I < static_cast<int>(F.Blocks.size()); ++I)
      IdToIndex[F.Blocks[I].Id] = I;
  }

  bool blockReaches(int FromId, int ToId) const {
    if (FromId == ToId)
      return true;
    llvm::DenseSet<int> Seen;
    std::deque<int> Work{FromId};
    while (!Work.empty()) {
      int Cur = Work.front();
      Work.pop_front();
      auto It = IdToIndex.find(Cur);
      if (It == IdToIndex.end())
        continue;
      for (int Succ : F.Blocks[It->second].Succs) {
        if (Succ == ToId)
          return true;
        if (Seen.insert(Succ).second)
          Work.push_back(Succ);
      }
    }
    return false;
  }

  bool after(int ABlock, int AOp, int BBlock, int BOp) const {
    if (ABlock == BBlock)
      return BOp > AOp;
    return blockReaches(ABlock, BBlock);
  }
};

const MedOp *opAt(const MedFunc &F, int BlockId, int OpIdx) {
  for (const MedBlock &B : F.Blocks)
    if (B.Id == BlockId)
      return (OpIdx >= 0 && OpIdx < static_cast<int>(B.Ops.size()))
                 ? &B.Ops[OpIdx]
                 : nullptr;
  return nullptr;
}

va_t callVA(const MedFunc &F, const MedCallInfo &CI) {
  if (const MedOp *Op = opAt(F, CI.BlockId, CI.OpIdx))
    return Op->Addr;
  return 0;
}

struct Summary {
  bool ReturnsHeap = false;
  llvm::DenseSet<int> FreesParam;
  llvm::DenseSet<int> EscapesParam;
  llvm::DenseSet<int> UnknownParam;
};

enum class EscapeState : uint8_t { No, Yes, Unknown };

struct FreeEvent {
  int BlockId = -1;
  int OpIdx = -1;
  va_t VA = 0;
  va_t TargetAddr = 0;
  std::string Name;
  bool IsIndirect = false;
};

struct UseEvent {
  int BlockId = -1;
  int OpIdx = -1;
  va_t VA = 0;
  va_t TargetAddr = 0;
  std::string Name;
  bool IsIndirect = false;
};

struct CollectedUses {
  std::vector<UseEvent> Definite;
  std::vector<UseEvent> Possible;
};

struct EventCursor {
  size_t PathIndex = 0;
  int BlockId = -1;
  int OpIdx = -1;
  bool Valid = false;
};

bool containsEventsInOrder(const symbolic::SymPath &Path,
                           llvm::ArrayRef<FindingEvent> Events,
                           EventCursor &Cursor) {
  Cursor = {};
  for (const FindingEvent &Event : Events) {
    bool Found = false;
    const size_t Start = Cursor.Valid ? Cursor.PathIndex : 0;
    for (size_t I = Start; I < Path.Blocks.size(); ++I) {
      if (Path.Blocks[I] != Event.BlockId)
        continue;
      if (Cursor.Valid && I == Cursor.PathIndex &&
          Event.BlockId == Cursor.BlockId && Event.OpIdx <= Cursor.OpIdx)
        continue;
      Cursor = {I, Event.BlockId, Event.OpIdx, true};
      Found = true;
      break;
    }
    if (!Found)
      return false;
  }
  return true;
}

bool containsForbiddenAfter(const symbolic::SymPath &Path,
                            llvm::ArrayRef<FindingEvent> Events,
                            const EventCursor &Cursor) {
  const size_t Start = Cursor.Valid ? Cursor.PathIndex : 0;
  for (const FindingEvent &Event : Events)
    for (size_t I = Start; I < Path.Blocks.size(); ++I)
      if (Path.Blocks[I] == Event.BlockId &&
          (!Cursor.Valid || I > Cursor.PathIndex ||
           Event.BlockId != Cursor.BlockId || Event.OpIdx > Cursor.OpIdx))
        return true;
  return false;
}

class Auditor {
public:
  Auditor(const AnalysisInput &In, const SinkCatalog &Cat,
          const SafetyBudgets &Budgets, bool IncludeStackReads)
      : In(In), Cat(Cat), Budgets(Budgets),
        IncludeStackReads(IncludeStackReads) {
    buildSummaries();
  }

  std::vector<Finding> run() {
    std::vector<Finding> All;
    if (!In.MedFuncs)
      return All;
    for (const MedFunc &F : *In.MedFuncs)
      auditFunction(F, All);
    std::vector<Finding> Kept;
    Kept.reserve(All.size());
    for (Finding &Fn : All)
      if (corroborate(Fn))
        Kept.push_back(std::move(Fn));
    return Kept;
  }

private:
  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const SafetyBudgets &Budgets;
  bool IncludeStackReads = false;
  llvm::DenseMap<va_t, Summary> Summaries;

  std::string callName(const MedCallInfo &CI) const {
    return resolveCallName(In, CI);
  }

  const SinkEntry *catalogSink(const MedCallInfo &CI) const {
    return Cat.matchSink(callName(CI));
  }

  bool returnsValue(const MedFunc &F) const {
    if (In.Dbg)
      if (auto Sym = In.Dbg->resolveFunction(F.Entry); Sym && Sym->ReturnType)
        return Sym->ReturnType->Kind != NdTypeKind::Void;
    return !F.ReturnType || F.ReturnType->Kind != NdTypeKind::Void;
  }

  int catalogFreeArg(const MedCallInfo &CI) const {
    const SinkEntry *E = catalogSink(CI);
    if (!E)
      return -1;
    if (E->Kind == SinkKind::Free)
      return E->HandleArg;
    return -1;
  }

  llvm::SmallVector<int, 4> freeArgsOf(const MedCallInfo &CI) const {
    llvm::SmallVector<int, 4> Args;
    int FA = catalogFreeArg(CI);
    if (FA >= 0) {
      Args.push_back(FA);
      return Args;
    }
    if (!CI.IsIndirect) {
      auto It = Summaries.find(CI.TargetAddr);
      if (It != Summaries.end())
        for (int Param : It->second.FreesParam)
          Args.push_back(Param);
    }
    return Args;
  }

  bool allocates(const MedCallInfo &CI) const {
    if (const SinkEntry *E = catalogSink(CI))
      if (E->Kind == SinkKind::Alloc || E->Kind == SinkKind::Realloc)
        return true;
    if (!CI.IsIndirect) {
      auto It = Summaries.find(CI.TargetAddr);
      if (It != Summaries.end() && It->second.ReturnsHeap)
        return true;
    }
    return false;
  }

  static bool sameSummary(const Summary &A, const Summary &B) {
    if (A.ReturnsHeap != B.ReturnsHeap ||
        A.FreesParam.size() != B.FreesParam.size() ||
        A.EscapesParam.size() != B.EscapesParam.size() ||
        A.UnknownParam.size() != B.UnknownParam.size())
      return false;
    for (int P : A.FreesParam)
      if (!B.FreesParam.count(P))
        return false;
    for (int P : A.EscapesParam)
      if (!B.EscapesParam.count(P))
        return false;
    for (int P : A.UnknownParam)
      if (!B.UnknownParam.count(P))
        return false;
    return true;
  }

  EscapeState callArgEscape(const MedCallInfo &CI, int ArgIndex) const {
    if (ArgIndex < 0 || ArgIndex >= static_cast<int>(CI.Args.size()))
      return EscapeState::Unknown;
    if (const SinkEntry *E = catalogSink(CI))
      if (E->Kind == SinkKind::Realloc && ArgIndex == E->HandleArg)
        return EscapeState::Unknown;
    if (ArgIndex == catalogFreeArg(CI))
      return EscapeState::No;

    if (!CI.IsIndirect)
      if (const MedFunc *Internal = In.findMedFunc(CI.TargetAddr)) {
        auto It = Summaries.find(Internal->Entry);
        if (It == Summaries.end())
          return EscapeState::Unknown;
        if (It->second.EscapesParam.count(ArgIndex))
          return EscapeState::Yes;
        if (It->second.UnknownParam.count(ArgIndex))
          return EscapeState::Unknown;
        return EscapeState::No;
      }

    // Catalogued runtime operations have a bounded, non-retaining contract.
    // Unknown and indirect callees remain explicitly unresolved.
    return catalogSink(CI) ? EscapeState::No : EscapeState::Unknown;
  }

  Summary summarize(const MedFunc &F) const {
    Summary S;
    MemModel Mem(F, In);
    for (int PI = 0; PI < static_cast<int>(F.Params.size()); ++PI) {
      if (F.Params[PI].isConst())
        continue;
      llvm::DenseSet<ValueKey> Alias = Mem.aliasClosure(F.Params[PI]);
      llvm::DenseSet<ValueKey> MustAlias = Mem.mustAliasClosure(F.Params[PI]);
      std::vector<FreeEvent> DefiniteFrees;
      bool HasPotentialFree = false;
      for (const MedCallInfo &CI : F.CallInfos) {
        for (int FA : freeArgsOf(CI)) {
          if (FA < 0 || FA >= static_cast<int>(CI.Args.size()) ||
              CI.Args[FA].isConst())
            continue;
          const ValueKey ArgKey = keyOf(CI.Args[FA]);
          if (MustAlias.count(ArgKey)) {
            DefiniteFrees.push_back({CI.BlockId, CI.OpIdx, callVA(F, CI),
                                     CI.TargetAddr, callName(CI),
                                     CI.IsIndirect});
            break;
          } else if (Alias.count(ArgKey))
            HasPotentialFree = true;
        }
      }

      if (eventsCoverEveryExit(F, DefiniteFrees))
        S.FreesParam.insert(PI);
      else if (!DefiniteFrees.empty() || HasPotentialFree)
        S.UnknownParam.insert(PI);
    }

    for (int PI = 0; PI < static_cast<int>(F.Params.size()); ++PI) {
      if (F.Params[PI].isConst())
        continue;
      llvm::DenseSet<ValueKey> Alias = Mem.aliasClosure(F.Params[PI]);
      llvm::DenseSet<ValueKey> MustAlias = Mem.mustAliasClosure(F.Params[PI]);
      std::vector<FreeEvent> DefiniteEscapes;
      bool HasPotentialEscape = false;
      for (const MedBlock &B : F.Blocks) {
        for (int OI = 0; OI < static_cast<int>(B.Ops.size()); ++OI) {
          const MedOp &Op = B.Ops[OI];
          if (Op.Opcode == NdOp::RETURN && returnsValue(F)) {
            for (unsigned I = 0; I < Op.NumInputs; ++I) {
              if (Op.Inputs[I].isConst())
                continue;
              const ValueKey InputKey = keyOf(Op.Inputs[I]);
              if (MustAlias.count(InputKey)) {
                DefiniteEscapes.push_back({B.Id, OI});
                break;
              }
              HasPotentialEscape |= Alias.count(InputKey) != 0;
            }
          } else if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
            const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
            const MedVar &Val = Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
            if (Val.isConst() || Mem.stackOffset(Addr))
              continue;
            const ValueKey StoredKey = keyOf(Val);
            if (MustAlias.count(StoredKey))
              DefiniteEscapes.push_back({B.Id, OI});
            else
              HasPotentialEscape |= Alias.count(StoredKey) != 0;
          }
        }
      }
      for (const MedCallInfo &CI : F.CallInfos) {
        bool CallDefinitelyEscapes = false;
        bool CallMayEscape = false;
        for (int AI = 0; AI < static_cast<int>(CI.Args.size()); ++AI) {
          if (CI.Args[AI].isConst())
            continue;
          const ValueKey ArgKey = keyOf(CI.Args[AI]);
          if (!Alias.count(ArgKey))
            continue;
          switch (callArgEscape(CI, AI)) {
          case EscapeState::Yes:
            if (MustAlias.count(ArgKey))
              CallDefinitelyEscapes = true;
            else
              CallMayEscape = true;
            break;
          case EscapeState::Unknown:
            CallMayEscape = true;
            break;
          case EscapeState::No:
            break;
          }
        }
        if (CallDefinitelyEscapes)
          DefiniteEscapes.push_back({CI.BlockId, CI.OpIdx});
        else
          HasPotentialEscape |= CallMayEscape;
      }
      if (eventsCoverEveryExit(F, DefiniteEscapes))
        S.EscapesParam.insert(PI);
      else if (!DefiniteEscapes.empty() || HasPotentialEscape)
        S.UnknownParam.insert(PI);
    }

    if (!returnsValue(F))
      return S;

    for (const MedCallInfo &CI : F.CallInfos) {
      bool Alloc = false;
      if (const SinkEntry *E = catalogSink(CI)) {
        if (E->Kind == SinkKind::Alloc && E->HandleArg >= 0)
          continue;
        Alloc = E->Kind == SinkKind::Alloc || E->Kind == SinkKind::Realloc;
      }
      if (!Alloc && !CI.IsIndirect) {
        auto It = Summaries.find(CI.TargetAddr);
        Alloc = It != Summaries.end() && It->second.ReturnsHeap;
      }
      if (!Alloc)
        continue;
      const MedOp *Op = opAt(F, CI.BlockId, CI.OpIdx);
      if (!Op || Op->Output.isConst() || Op->Output.Size == 0)
        continue;
      llvm::DenseSet<ValueKey> Alias = Mem.aliasClosure(Op->Output);
      llvm::DenseSet<ValueKey> MustAlias = Mem.mustAliasClosure(Op->Output);
      std::vector<FreeEvent> ReturnEvents;
      for (const MedBlock &B : F.Blocks)
        for (int OI = 0; OI < static_cast<int>(B.Ops.size()); ++OI) {
          const MedOp &O = B.Ops[OI];
          if (O.Opcode != NdOp::RETURN)
            continue;
          for (unsigned Input = 0; Input < O.NumInputs; ++Input)
            if (!O.Inputs[Input].isConst() &&
                Alias.count(keyOf(O.Inputs[Input]))) {
              ReturnEvents.push_back({B.Id, OI});
              break;
            }
        }
      std::vector<FreeEvent> DefiniteFrees;
      for (const MedCallInfo &FC : F.CallInfos)
        for (int FreeArg : freeArgsOf(FC)) {
          if (FreeArg < 0 || FreeArg >= static_cast<int>(FC.Args.size()) ||
              FC.Args[FreeArg].isConst())
            continue;
          if (MustAlias.count(keyOf(FC.Args[FreeArg]))) {
            DefiniteFrees.push_back({FC.BlockId, FC.OpIdx});
            break;
          }
        }
      if (reachesAnyEventWithoutBlocker(F, CI.BlockId, CI.OpIdx, ReturnEvents,
                                        DefiniteFrees))
        S.ReturnsHeap = true;
    }
    return S;
  }

  void buildSummaries() {
    if (!In.MedFuncs)
      return;
    bool Changed = true;
    for (int Guard = 0; Changed && Guard < 32; ++Guard) {
      Changed = false;
      for (const MedFunc &F : *In.MedFuncs) {
        Summary S = summarize(F);
        auto It = Summaries.find(F.Entry);
        if (It == Summaries.end() || !sameSummary(It->second, S)) {
          Summaries[F.Entry] = std::move(S);
          Changed = true;
        }
      }
    }
  }

  bool isEventSite(int BlockId, int OpIdx,
                   const std::vector<FreeEvent> &Events) const {
    for (const FreeEvent &Event : Events)
      if (Event.BlockId == BlockId && Event.OpIdx == OpIdx)
        return true;
    return false;
  }

  bool reachesExitWithoutEvent(const MedFunc &F, int StartBlock, int StartOp,
                               const std::vector<FreeEvent> &Events,
                               bool *ReachedEvent = nullptr) const {
    llvm::DenseSet<ValueKey> SeenPts;
    llvm::DenseSet<int> ExpandedExceptional;
    std::deque<std::pair<int, int>> Work;
    auto enqueue = [&](int B, int O) {
      if (SeenPts.insert(ValueKey{0, B, O}).second)
        Work.emplace_back(B, O);
    };
    enqueue(StartBlock, StartOp + 1);
    while (!Work.empty()) {
      auto [BlkId, OpIdx] = Work.front();
      Work.pop_front();
      const MedBlock *Blk = nullptr;
      for (const MedBlock &B : F.Blocks)
        if (B.Id == BlkId) {
          Blk = &B;
          break;
        }
      if (!Blk)
        continue;
      if (ExpandedExceptional.insert(BlkId).second)
        for (const ExceptionalEdge &Edge : Blk->ExceptionalSuccs) {
          if (Edge.BlockId < 0)
            return true;
          enqueue(Edge.BlockId, 0);
        }
      if (OpIdx >= static_cast<int>(Blk->Ops.size())) {
        if (Blk->Succs.empty())
          return true;
        for (int S : Blk->Succs)
          enqueue(S, 0);
        continue;
      }
      if (isEventSite(BlkId, OpIdx, Events)) {
        if (ReachedEvent)
          *ReachedEvent = true;
        continue;
      }
      if (Blk->Ops[OpIdx].Opcode == NdOp::RETURN)
        return true;
      enqueue(BlkId, OpIdx + 1);
    }
    return false;
  }

  bool eventsCoverEveryExit(const MedFunc &F,
                            const std::vector<FreeEvent> &Events) const {
    if (F.Blocks.empty() || Events.empty())
      return false;
    int EntryBlock = F.Blocks.front().Id;
    for (const MedBlock &B : F.Blocks)
      if (B.StartAddr == F.Entry) {
        EntryBlock = B.Id;
        break;
      }
    bool ReachedEvent = false;
    const bool HasUncoveredExit =
        reachesExitWithoutEvent(F, EntryBlock, -1, Events, &ReachedEvent);
    return ReachedEvent && !HasUncoveredExit;
  }

  bool reachesAnyEventWithoutBlocker(const MedFunc &F, int StartBlock,
                                     int StartOp,
                                     const std::vector<FreeEvent> &Targets,
                                     const std::vector<FreeEvent> &Blockers,
                                     bool FollowExceptional = true) const {
    if (Targets.empty())
      return false;
    llvm::DenseSet<ValueKey> SeenPts;
    llvm::DenseSet<int> ExpandedExceptional;
    std::deque<std::pair<int, int>> Work;
    auto enqueue = [&](int BlockId, int OpIdx) {
      if (SeenPts.insert(ValueKey{0, BlockId, OpIdx}).second)
        Work.emplace_back(BlockId, OpIdx);
    };
    enqueue(StartBlock, StartOp + 1);
    while (!Work.empty()) {
      const auto [BlockId, OpIdx] = Work.front();
      Work.pop_front();
      const MedBlock *Block = nullptr;
      for (const MedBlock &Candidate : F.Blocks)
        if (Candidate.Id == BlockId) {
          Block = &Candidate;
          break;
        }
      if (!Block)
        continue;
      if (FollowExceptional && ExpandedExceptional.insert(BlockId).second)
        for (const ExceptionalEdge &Edge : Block->ExceptionalSuccs)
          if (Edge.BlockId >= 0)
            enqueue(Edge.BlockId, 0);
      if (OpIdx >= static_cast<int>(Block->Ops.size())) {
        for (int Succ : Block->Succs)
          enqueue(Succ, 0);
        continue;
      }
      if (isEventSite(BlockId, OpIdx, Blockers))
        continue;
      if (isEventSite(BlockId, OpIdx, Targets))
        return true;
      if (Block->Ops[OpIdx].Opcode != NdOp::RETURN)
        enqueue(BlockId, OpIdx + 1);
    }
    return false;
  }

  bool shouldReportLeak(const MedFunc &F, const MedCallInfo &Alloc,
                        const std::vector<FreeEvent> &Frees) const {
    if (Frees.empty())
      return true;
    return reachesExitWithoutEvent(F, Alloc.BlockId, Alloc.OpIdx, Frees);
  }

  bool hasUnresolvedLifecycleCall(const MedFunc &F, const CFG &G,
                                  const MedCallInfo &Alloc) const {
    for (const MedCallInfo &CI : F.CallInfos) {
      if (!G.after(Alloc.BlockId, Alloc.OpIdx, CI.BlockId, CI.OpIdx))
        continue;
      if (const SinkEntry *E = catalogSink(CI))
        if (E->Kind == SinkKind::Realloc &&
            (E->HandleArg < 0 ||
             E->HandleArg >= static_cast<int>(CI.Args.size())))
          return true;
      for (int FreeArg : freeArgsOf(CI))
        if (FreeArg < 0 || FreeArg >= static_cast<int>(CI.Args.size()))
          return true;
      if (CI.Args.empty() && catalogSink(CI) == nullptr &&
          (CI.IsIndirect || !In.findMedFunc(CI.TargetAddr)))
        return true;
    }
    return false;
  }

  unsigned summarizedCallsOnPath(const MedFunc &F, const LowFunc &LF,
                                 const symbolic::SymPath &Path) const {
    unsigned Count = 0;
    for (int BlockId : Path.Blocks) {
      const LowBlock *Block = nullptr;
      for (const LowBlock &Candidate : LF.Blocks)
        if (Candidate.Id == BlockId) {
          Block = &Candidate;
          break;
        }
      if (!Block)
        continue;
      for (const LowOp &Op : Block->Ops) {
        if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
          continue;
        const MedCallInfo *Call = nullptr;
        for (const MedCallInfo &CI : F.CallInfos)
          if (callVA(F, CI) == Op.Addr) {
            Call = &CI;
            break;
          }
        if (!Call)
          continue;
        const std::string Name = callName(*Call);
        if (catalogSink(*Call) || Cat.matchSource(Name) ||
            isStringLengthCall(Name) ||
            (!Call->IsIndirect && In.findMedFunc(Call->TargetAddr)))
          ++Count;
      }
    }
    return Count;
  }

  bool corroborate(Finding &Fn) {
    if (Fn.TheVerdict != Verdict::Unsafe)
      return true;
    const LowFunc *LF = In.findLowFunc(Fn.FuncEntry);
    auto failClosed = [&](llvm::StringRef Reason, bool Budget = false) {
      Fn.TheVerdict = Verdict::Unknown;
      Fn.TheConfidence = Confidence::Low;
      Fn.BudgetHit = Budget;
      Fn.Corroboration = Reason.str();
      Fn.Detail = "symbolic corroboration did not establish the candidate";
    };
    if (!LF || LF->Blocks.empty()) {
      failClosed("no LowIR path was available for corroboration");
      return true;
    }
    if (!LF->hasCompleteInstructionLift()) {
      failClosed("the containing function was not lifted completely");
      return true;
    }
    symbolic::SymContext Ctx;
    symbolic::ExploreOptions Opts;
    Opts.MaxPaths = Budgets.MaxPaths ? Budgets.MaxPaths : 64;
    Opts.MaxSteps = Budgets.MaxSteps ? Budgets.MaxSteps : (1u << 16);
    Opts.MaxLoopIterations = Budgets.MaxLoop ? Budgets.MaxLoop : 3;
    if (In.Img) {
      const TargetRegInfo &TRI = getTargetRegInfo(In.Img->Arch);
      const uint16_t Bytes = TRI.PointerSize ? TRI.PointerSize : 8;
      Opts.TrackedCallResultRegister =
          symbolic::SymRegisterRange{TRI.IntReturnReg, Bytes};
    }
    symbolic::SymExploration Ex =
        symbolic::explorePathsDetailed(Ctx, *LF, Opts);
    const MedFunc *Med = In.findMedFunc(Fn.FuncEntry);
    bool SawUnmodelled = false;
    bool SolverUnknown = false;
    for (const symbolic::SymPath &P : Ex.Paths) {
      if (Fn.RequireReturnedPath &&
          P.Outcome != symbolic::PathOutcome::Returned)
        continue;
      EventCursor Cursor;
      if (!containsEventsInOrder(P, Fn.RequiredPathEvents, Cursor))
        continue;
      if (containsForbiddenAfter(P, Fn.ForbiddenPathEvents, Cursor))
        continue;
      const unsigned SummarizedCalls =
          Med ? summarizedCallsOnPath(*Med, *LF, P) : 0;
      if (P.OpaqueOps != 0 || P.CallHavocs > SummarizedCalls) {
        SawUnmodelled = true;
        continue;
      }
      symbolic::SymRef Pred = P.predicate(Ctx);
      if (Fn.RequireNonNullCallVA) {
        symbolic::SymRef CallResult;
        unsigned Matches = 0;
        for (const symbolic::SymCallResult &Result : P.CallResults)
          if (Result.CallVA == Fn.RequireNonNullCallVA) {
            CallResult = Result.Value;
            ++Matches;
          }
        if (Matches != 1 || !CallResult.isValid()) {
          SawUnmodelled = true;
          continue;
        }
        symbolic::SymRef NonNull =
            Ctx.mkNe(CallResult, Ctx.mkZero(Ctx.width(CallResult)));
        symbolic::SymRef Parts[] = {Pred, NonNull};
        Pred = Ctx.mkAnd(Parts);
      }
      solver::SolverOptions SolverOpts;
      if (Budgets.SolverConflicts)
        SolverOpts.Sat.MaxConflicts = Budgets.SolverConflicts;
      const solver::SatResult Sat =
          solver::checkSat(Ctx, Pred, nullptr, SolverOpts);
      if (Sat == solver::SatResult::Unsat)
        continue;
      if (Sat != solver::SatResult::Sat) {
        SolverUnknown = true;
        continue;
      }
      Fn.TheConfidence = Confidence::High;
      Fn.Corroboration =
          "candidate event sequence is feasible on a symbolic path";
      if (!Ctx.isConstOnes(Pred))
        Fn.Constraints = Ctx.toString(Pred);
      return true;
    }
    if (SawUnmodelled)
      failClosed("a candidate path contained an unmodelled operation");
    else if (SolverUnknown)
      failClosed("the solver could not establish candidate feasibility", true);
    else if (!Ex.Complete)
      failClosed("symbolic exploration ended before all candidate paths", true);
    else
      return false;
    return true;
  }

  Finding baseFinding(const MedFunc &F, VulnClass Class, va_t VA,
                      const std::string &Name, va_t CalleeAddr,
                      bool IsIndirect) const {
    Finding Fn;
    Fn.Origin = Track::Audit;
    Fn.Class = Class;
    Fn.TheVerdict = Verdict::Unsafe;
    Fn.Function = F.Name;
    Fn.FuncEntry = F.Entry;
    Fn.Name = Name;
    Fn.Sink = Name;
    Fn.CallVA = VA;
    Fn.Source = classifyNameSource(In, CalleeAddr, Name, IsIndirect);
    if (In.Dbg)
      if (auto Loc = In.Dbg->sourceLocation(VA); Loc && !Loc->File.empty())
        Fn.SourceLoc = Loc->File + ":" + std::to_string(Loc->Line);
    return Fn;
  }

  void auditFunction(const MedFunc &F, std::vector<Finding> &Out) {
    CFG G(F);
    MemModel Mem(F, In);

    if (IncludeStackReads)
      auditUninitializedStackReads(F, Mem, Out);

    for (size_t I = 0; I < F.CallInfos.size(); ++I) {
      const MedCallInfo &CI = F.CallInfos[I];
      if (!allocates(CI))
        continue;
      if (const SinkEntry *E = catalogSink(CI);
          E && E->Kind == SinkKind::Alloc && E->HandleArg >= 0) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail = "allocation output handle was not recovered";
        Out.push_back(std::move(Fn));
        continue;
      }
      const MedOp *AllocOp = opAt(F, CI.BlockId, CI.OpIdx);
      if (!AllocOp || AllocOp->Output.isConst() || AllocOp->Output.Size == 0) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail = "allocation result was not recovered";
        Out.push_back(std::move(Fn));
        continue;
      }

      llvm::DenseSet<ValueKey> Alias = Mem.aliasClosure(AllocOp->Output);
      llvm::DenseSet<ValueKey> MustAlias =
          Mem.mustAliasClosure(AllocOp->Output);
      const va_t AllocVA = callVA(F, CI);
      const std::vector<FreeEvent> AllocationSite = {
          {CI.BlockId, CI.OpIdx, AllocVA, CI.TargetAddr, callName(CI),
           CI.IsIndirect}};

      std::vector<FreeEvent> Frees;
      bool HasPotentialFree = false;
      for (const MedCallInfo &FC : F.CallInfos) {
        for (int FA : freeArgsOf(FC)) {
          if (FA < 0 || FA >= static_cast<int>(FC.Args.size()))
            continue;
          const ValueKey ArgKey = keyOf(FC.Args[FA]);
          if (MustAlias.count(ArgKey)) {
            Frees.push_back({FC.BlockId, FC.OpIdx, callVA(F, FC), FC.TargetAddr,
                             callName(FC), FC.IsIndirect});
            break;
          }
          HasPotentialFree |= Alias.count(ArgKey) != 0;
        }
      }

      const EscapeState Escape = escapes(F, Mem, Alias, CI);
      const bool UnresolvedLifecycle =
          HasPotentialFree || hasUnresolvedLifecycleCall(F, G, CI);

      bool ReportedDouble = false;
      for (size_t A = 0; A < Frees.size() && !ReportedDouble; ++A)
        for (size_t B = 0; B < Frees.size(); ++B) {
          const std::vector<FreeEvent> Target = {Frees[B]};
          if (reachesAnyEventWithoutBlocker(F, Frees[A].BlockId, Frees[A].OpIdx,
                                            Target, AllocationSite,
                                            /*FollowExceptional=*/false)) {
            Finding Fn = baseFinding(F, VulnClass::DoubleFree, Frees[B].VA,
                                     Frees[B].Name, Frees[B].TargetAddr,
                                     Frees[B].IsIndirect);
            Fn.TheConfidence = Confidence::High;
            Fn.Detail = "handle released twice on a path";
            Fn.RequiredPathEvents = {
                {CI.BlockId, CI.OpIdx},
                {Frees[A].BlockId, Frees[A].OpIdx},
                {Frees[B].BlockId, Frees[B].OpIdx},
            };
            Fn.RequireNonNullCallVA = AllocVA;
            Out.push_back(std::move(Fn));
            ReportedDouble = true;
            break;
          }
        }

      CollectedUses Uses = collectUses(F, Alias, Frees);
      bool ReportedUAF = false;
      for (const FreeEvent &Fr : Frees) {
        if (ReportedUAF)
          break;
        for (const UseEvent &U : Uses.Definite) {
          const std::vector<FreeEvent> Target = {{U.BlockId, U.OpIdx}};
          if (reachesAnyEventWithoutBlocker(F, Fr.BlockId, Fr.OpIdx, Target,
                                            AllocationSite,
                                            /*FollowExceptional=*/false)) {
            Finding Fn = baseFinding(F, VulnClass::UseAfterFree, U.VA, Fr.Name,
                                     Fr.TargetAddr, Fr.IsIndirect);
            Fn.TheConfidence = Confidence::High;
            Fn.Detail = "handle used after it was released";
            Fn.RequiredPathEvents = {
                {CI.BlockId, CI.OpIdx},
                {Fr.BlockId, Fr.OpIdx},
                {U.BlockId, U.OpIdx},
            };
            Fn.RequireNonNullCallVA = AllocVA;
            Out.push_back(std::move(Fn));
            ReportedUAF = true;
            break;
          }
        }
      }
      if (!ReportedUAF)
        for (const FreeEvent &Fr : Frees) {
          bool ReportedPossibleUAF = false;
          for (const UseEvent &U : Uses.Possible) {
            const std::vector<FreeEvent> Target = {{U.BlockId, U.OpIdx}};
            if (!reachesAnyEventWithoutBlocker(F, Fr.BlockId, Fr.OpIdx, Target,
                                               AllocationSite,
                                               /*FollowExceptional=*/false))
              continue;
            Finding Fn = baseFinding(F, VulnClass::UseAfterFree, U.VA, U.Name,
                                     U.TargetAddr, U.IsIndirect);
            Fn.TheVerdict = Verdict::Unknown;
            Fn.TheConfidence = Confidence::Low;
            Fn.Detail = "callee may access a handle after it was released";
            Out.push_back(std::move(Fn));
            ReportedPossibleUAF = true;
            break;
          }
          if (ReportedPossibleUAF)
            break;
        }

      const bool MayLeak = shouldReportLeak(F, CI, Frees);
      if (MayLeak && (Escape == EscapeState::Unknown || UnresolvedLifecycle)) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail =
            UnresolvedLifecycle
                ? "heap lifetime depends on a call with unrecovered arguments"
                : "heap handle reaches a call without a retention summary";
        Out.push_back(std::move(Fn));
      } else if (MayLeak && Escape == EscapeState::No) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.TheConfidence = Confidence::Medium;
        Fn.Detail = "allocation neither released nor escaped on every path";
        Fn.RequiredPathEvents = {{CI.BlockId, CI.OpIdx}};
        for (const FreeEvent &Fr : Frees)
          Fn.ForbiddenPathEvents.push_back({Fr.BlockId, Fr.OpIdx});
        Fn.RequireReturnedPath = true;
        Fn.RequireNonNullCallVA = AllocVA;
        Out.push_back(std::move(Fn));
      }
    }
  }

  void auditUninitializedStackReads(const MedFunc &F, const MemModel &Mem,
                                    std::vector<Finding> &Out) const {
    for (const MedBlock &B : F.Blocks)
      for (int Oi = 0; Oi < static_cast<int>(B.Ops.size()); ++Oi) {
        const MedOp &Op = B.Ops[Oi];
        if (!detail::isStackMemoryRead(Op.Opcode) || Op.Output.Size == 0)
          continue;
        const MedVar *Addr = detail::memoryAddress(Op);
        if (!Addr)
          continue;
        const std::optional<int64_t> Offset = Mem.stackOffset(*Addr);
        // Positive entry-SP offsets are caller-owned argument/ABI storage.
        // This audit only claims local frame bytes below the entry SP.
        if (!Offset || *Offset >= 0)
          continue;
        const detail::ReachingStackValues Reaching =
            Mem.reachingLoad(B, Oi, Op);
        if (!Reaching.Reachable ||
            (!Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites))
          continue;

        const bool Definite =
            !Reaching.HasUnknownWrites && Reaching.Values.empty();
        Finding Fn;
        Fn.Origin = Track::Audit;
        Fn.Class = VulnClass::UninitializedRead;
        Fn.TheVerdict = Definite ? Verdict::Unsafe : Verdict::Unknown;
        Fn.TheConfidence = Definite ? Confidence::Medium : Confidence::Low;
        Fn.Function = F.Name;
        Fn.FuncEntry = F.Entry;
        Fn.Name = Op.Opcode == NdOp::LOAD ? "stack_load" : "stack_atomic";
        Fn.Sink = Fn.Name;
        Fn.CallVA = Op.Addr;
        Fn.Source = NameSource::Synthetic;
        Fn.Flow = ArgFlow::Unknown;
        Fn.Detail = Definite ? "local stack slot is read before any full-width "
                               "initialization"
                             : "local stack slot may be read before "
                               "initialization on a control-flow path";
        if (Definite)
          Fn.RequiredPathEvents = {{B.Id, Oi}};
        if (In.Dbg)
          if (auto Loc = In.Dbg->sourceLocation(Op.Addr);
              Loc && !Loc->File.empty())
            Fn.SourceLoc = Loc->File + ":" + std::to_string(Loc->Line);
        Out.push_back(std::move(Fn));
      }

    for (const MedCallInfo &CI : F.CallInfos) {
      const SinkEntry *E = catalogSink(CI);
      if (!E || E->Kind != SinkKind::Copy || E->SrcArg < 0 || E->LenArg < 0 ||
          E->SrcArg >= static_cast<int>(CI.Args.size()) ||
          E->LenArg >= static_cast<int>(CI.Args.size()))
        continue;
      std::optional<uint64_t> Count = unsignedConstant(CI.Args[E->LenArg]);
      if (!Count)
        continue;
      const std::optional<uint64_t> Bytes = detail::exactCopyReadBytes(
          callName(CI), In.Img ? In.Img->Format : BinaryFormat::Unknown,
          *Count);
      if (!Bytes || *Bytes == 0)
        continue;
      const MedOp *CallOp = opAt(F, CI.BlockId, CI.OpIdx);
      if (!CallOp ||
          (CallOp->Opcode != NdOp::CALL && CallOp->Opcode != NdOp::INDIR_CALL))
        continue;
      const MedBlock *Block = nullptr;
      for (const MedBlock &Candidate : F.Blocks)
        if (Candidate.Id == CI.BlockId) {
          Block = &Candidate;
          break;
        }
      if (!Block)
        continue;
      const MedVar &Source = CI.Args[E->SrcArg];
      const std::optional<int64_t> Offset = Mem.stackOffset(Source);
      if (!Offset || *Offset >= 0)
        continue;
      const uint16_t ReadSize = static_cast<uint16_t>(
          std::min<uint64_t>(*Bytes, std::numeric_limits<uint16_t>::max()));
      const detail::ReachingStackValues Reaching =
          Mem.reachingRead(*Block, CI.OpIdx, Source, ReadSize);
      if (!Reaching.Reachable ||
          (!Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites))
        continue;

      const bool Definite =
          !Reaching.HasUnknownWrites && Reaching.Values.empty();
      Finding Fn = baseFinding(F, VulnClass::UninitializedRead, callVA(F, CI),
                               callName(CI), CI.TargetAddr, CI.IsIndirect);
      Fn.ArgIndex = E->SrcArg;
      Fn.TheVerdict = Definite ? Verdict::Unsafe : Verdict::Unknown;
      Fn.TheConfidence = Definite ? Confidence::Medium : Confidence::Low;
      Fn.Detail = Definite ? "copy source reads local stack bytes before any "
                             "full-width initialization"
                           : "copy source may read local stack bytes before "
                             "initialization on a control-flow path";
      if (Definite)
        Fn.RequiredPathEvents = {{CI.BlockId, CI.OpIdx}};
      Out.push_back(std::move(Fn));
    }
  }

  enum class CallUse : uint8_t { None, Definite, Possible };

  CallUse callUse(const MedCallInfo &CI, int ArgIndex) const {
    if (ArgIndex < 0 || ArgIndex >= static_cast<int>(CI.Args.size()))
      return CallUse::Possible;
    if (const SinkEntry *E = catalogSink(CI)) {
      switch (E->Kind) {
      case SinkKind::Copy:
        if (ArgIndex != E->DstArg && ArgIndex != E->SrcArg)
          return CallUse::None;
        if (!detail::copyAccessRequiresPositiveCount(
                callName(CI), /*IsDestination=*/ArgIndex == E->DstArg))
          return CallUse::Definite;
        if (E->LenArg < 0 || E->LenArg >= static_cast<int>(CI.Args.size()))
          return CallUse::Possible;
        if (std::optional<uint64_t> Count =
                unsignedConstant(CI.Args[E->LenArg]))
          return *Count == 0 ? CallUse::None : CallUse::Definite;
        return CallUse::Possible;
      case SinkKind::Format:
        if (ArgIndex == E->FmtArg)
          return CallUse::Definite;
        if (ArgIndex == E->DstArg) {
          if (E->LenArg < 0)
            return CallUse::Definite;
          if (E->LenArg >= static_cast<int>(CI.Args.size()))
            return CallUse::Possible;
          if (std::optional<uint64_t> Limit =
                  unsignedConstant(CI.Args[E->LenArg]))
            return *Limit == 0 ? CallUse::None : CallUse::Definite;
          return CallUse::Possible;
        }
        if (ArgIndex == E->LenArg || ArgIndex == E->CapArg)
          return CallUse::None;
        return CallUse::Possible;
      case SinkKind::Alloc:
        return E->HandleArg >= 0 && ArgIndex == E->HandleArg ? CallUse::Definite
                                                             : CallUse::None;
      case SinkKind::Realloc:
        return ArgIndex == E->HandleArg ? CallUse::Definite : CallUse::None;
      case SinkKind::Free:
      case SinkKind::StackAlloc:
        return CallUse::None;
      case SinkKind::Exec:
      case SinkKind::Source:
        return CallUse::Possible;
      }
    }
    const std::string Name = callName(CI);
    if (std::optional<
            std::pair<std::string, safety::detail::FormattedSourceKind>>
            Formatted = safety::detail::formattedSourceName(Name)) {
      const unsigned FixedCount = libc::varArgFixedCount(Formatted->first);
      return ArgIndex < static_cast<int>(FixedCount) ? CallUse::Definite
                                                     : CallUse::Possible;
    }
    if (const SourceEntry *Source = Cat.matchSource(Name))
      return ArgIndex == Source->OutArg ? CallUse::Definite : CallUse::Possible;
    if (isStringLengthCall(Name))
      return ArgIndex == 0 ? CallUse::Definite : CallUse::None;
    return CallUse::Possible;
  }

  CollectedUses collectUses(const MedFunc &F,
                            const llvm::DenseSet<ValueKey> &Alias,
                            const std::vector<FreeEvent> &Frees) const {
    auto isFree = [&](int BlockId, int OpIdx) {
      for (const FreeEvent &Fr : Frees)
        if (Fr.BlockId == BlockId && Fr.OpIdx == OpIdx)
          return true;
      return false;
    };

    CollectedUses Uses;
    for (const MedBlock &B : F.Blocks)
      for (int Oi = 0; Oi < static_cast<int>(B.Ops.size()); ++Oi) {
        const MedOp &Op = B.Ops[Oi];
        if (Op.Opcode == NdOp::LOAD || detail::isStackMemoryWrite(Op.Opcode)) {
          const MedVar *Addr = detail::memoryAddress(Op);
          if (Addr && !Addr->isConst() && Alias.count(keyOf(*Addr)))
            Uses.Definite.push_back({B.Id, Oi, Op.Addr});
        }
      }
    for (const MedCallInfo &CI : F.CallInfos) {
      if (isFree(CI.BlockId, CI.OpIdx))
        continue;
      llvm::DenseSet<int> FreeArgs;
      for (int FA : freeArgsOf(CI))
        FreeArgs.insert(FA);
      CallUse Use = CallUse::None;
      for (int I = 0; I < static_cast<int>(CI.Args.size()); ++I) {
        if (FreeArgs.count(I) || CI.Args[I].isConst())
          continue;
        if (!Alias.count(keyOf(CI.Args[I])))
          continue;
        const CallUse ArgUse = callUse(CI, I);
        if (ArgUse == CallUse::Definite) {
          Use = CallUse::Definite;
          break;
        }
        if (ArgUse == CallUse::Possible)
          Use = CallUse::Possible;
      }
      if (Use == CallUse::None)
        continue;
      UseEvent Event{CI.BlockId,    CI.OpIdx,     callVA(F, CI),
                     CI.TargetAddr, callName(CI), CI.IsIndirect};
      (Use == CallUse::Definite ? Uses.Definite : Uses.Possible)
          .push_back(std::move(Event));
    }
    return Uses;
  }

  EscapeState escapes(const MedFunc &F, const MemModel &Mem,
                      const llvm::DenseSet<ValueKey> &Alias,
                      const MedCallInfo &Alloc) const {
    bool Unknown = false;
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops) {
        if (Op.Opcode == NdOp::RETURN && returnsValue(F)) {
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (!Op.Inputs[I].isConst() && Alias.count(keyOf(Op.Inputs[I])))
              return EscapeState::Yes;
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
          const MedVar &Val = Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
          // Spilling the handle into its own stack frame is not an escape; only
          // a write through a non-stack address publishes it.
          if (!Val.isConst() && Alias.count(keyOf(Val)) &&
              !Mem.stackOffset(Addr))
            return EscapeState::Yes;
        }
      }
    for (const MedCallInfo &CI : F.CallInfos) {
      if (CI.BlockId == Alloc.BlockId && CI.OpIdx == Alloc.OpIdx)
        continue;
      llvm::DenseSet<int> FreeArgs;
      for (int FA : freeArgsOf(CI))
        FreeArgs.insert(FA);
      for (int I = 0; I < static_cast<int>(CI.Args.size()); ++I) {
        if (FreeArgs.count(I) || CI.Args[I].isConst())
          continue;
        if (!Alias.count(keyOf(CI.Args[I])))
          continue;
        switch (callArgEscape(CI, I)) {
        case EscapeState::Yes:
          return EscapeState::Yes;
        case EscapeState::Unknown:
          Unknown = true;
          break;
        case EscapeState::No:
          break;
        }
      }
    }
    return Unknown ? EscapeState::Unknown : EscapeState::No;
  }
};

} // namespace

std::vector<Finding> neverd::safety::auditHeap(const AnalysisInput &In,
                                               const SinkCatalog &Cat,
                                               const SafetyBudgets &Budgets) {
  Auditor A(In, Cat, Budgets, /*IncludeStackReads=*/false);
  return A.run();
}

std::vector<Finding> neverd::safety::auditMemory(const AnalysisInput &In,
                                                 const SinkCatalog &Cat,
                                                 const SafetyBudgets &Budgets) {
  Auditor A(In, Cat, Budgets, /*IncludeStackReads=*/true);
  return A.run();
}
