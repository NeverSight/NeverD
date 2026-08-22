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
#include <functional>
#include <limits>
#include <map>
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

enum class StringDuplicationKind : uint8_t {
  None,
  NullTerminated,
  WideNullTerminated,
  Counted
};

StringDuplicationKind stringDuplicationKind(llvm::StringRef Name) {
  return llvm::StringSwitch<StringDuplicationKind>(SinkCatalog::normalize(Name))
      .Case("strdup", StringDuplicationKind::NullTerminated)
      .Case("wcsdup", StringDuplicationKind::WideNullTerminated)
      .Case("strndup", StringDuplicationKind::Counted)
      .Default(StringDuplicationKind::None);
}

std::optional<uint64_t>
minimumStringDuplicationReadBytes(StringDuplicationKind Kind,
                                  BinaryFormat Format) {
  if (Kind != StringDuplicationKind::WideNullTerminated)
    return 1;
  switch (Format) {
  case BinaryFormat::COFF:
    return 2;
  case BinaryFormat::ELF:
  case BinaryFormat::MachO:
    return 4;
  default:
    return std::nullopt;
  }
}

std::optional<uint64_t> unsignedConstant(const MedVar &Value) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t) ||
      isAddressProvenance(Value.Provenance))
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
           Op.Inputs[Input].ConstVal == 0 &&
           !isAddressProvenance(Op.Inputs[Input].Provenance);
  };

  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::CAST:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Op.NumInputs == 1 && aliases(0) && sameSize(0);
  case NdOp::SUBBYTES:
    return Op.NumInputs == 2 && aliases(0) && sameSize(0) && isZero(1);
  case NdOp::INT_ADD:
    return Op.NumInputs == 2 && ((aliases(0) && sameSize(0) && isZero(1)) ||
                                 (isZero(0) && aliases(1) && sameSize(1)));
  case NdOp::INT_SUB:
    return Op.NumInputs == 2 && aliases(0) && sameSize(0) && isZero(1);
  case NdOp::SELECT:
    return Op.NumInputs == 3 && aliases(1) && sameSize(1) && aliases(2) &&
           sameSize(2);
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
  static constexpr unsigned MaxProofSteps = 4096;

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

  std::optional<int64_t> stackOffsetThroughReloads(const MedVar &V) const {
    llvm::DenseSet<ValueKey> Active;
    std::map<ValueKey, std::optional<int64_t>> Cache;
    unsigned Remaining = MaxProofSteps;
    bool Incomplete = false;
    std::function<std::optional<int64_t>(const MedVar &)> Resolve =
        [&](const MedVar &Current) -> std::optional<int64_t> {
      if (Current.isConst())
        return std::nullopt;
      const ValueKey Key = keyOf(Current);
      if (auto It = Cache.find(Key); It != Cache.end())
        return It->second;
      if (std::optional<int64_t> Direct = stackOffset(Current)) {
        Cache[Key] = Direct;
        return Direct;
      }
      if (Remaining == 0 || !Active.insert(Key).second) {
        Incomplete = true;
        return std::nullopt;
      }
      struct PopActive {
        llvm::DenseSet<ValueKey> &Set;
        ValueKey Key;
        ~PopActive() { Set.erase(Key); }
      } Guard{Active, Key};
      --Remaining;

      auto finish = [&](std::optional<int64_t> Result) {
        if (!Incomplete)
          Cache[Key] = Result;
        return Result;
      };

      auto merge = [&](std::optional<int64_t> &Result,
                       const MedVar &Candidate) {
        std::optional<int64_t> Offset = Resolve(Candidate);
        if (!Offset || (Result && *Result != *Offset))
          return false;
        Result = Offset;
        return true;
      };

      std::optional<int64_t> Result;
      if (auto It = PhiDef.find(Key); It != PhiDef.end()) {
        const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
        if (Phi.Args.empty())
          return finish(std::nullopt);
        for (const auto &[Pred, Arg] : Phi.Args) {
          (void)Pred;
          if (!merge(Result, Arg))
            return finish(std::nullopt);
        }
        return finish(Result);
      }

      auto It = OpDef.find(Key);
      if (It == OpDef.end())
        return finish(std::nullopt);
      const MedBlock &Block = F.Blocks[It->second.first];
      const int OpIdx = It->second.second;
      const MedOp &Op = Block.Ops[OpIdx];
      auto sameSize = [&](unsigned Input) {
        return Input < Op.NumInputs && Op.Output.Size == Op.Inputs[Input].Size;
      };
      auto isZero = [&](unsigned Input) {
        return Input < Op.NumInputs && Op.Inputs[Input].isConst() &&
               Op.Inputs[Input].ConstVal == 0 &&
               !isAddressProvenance(Op.Inputs[Input].Provenance);
      };
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::CAST:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        if (Op.NumInputs == 1 && sameSize(0))
          Result = Resolve(Op.Inputs[0]);
        break;
      case NdOp::SUBBYTES:
        if (Op.NumInputs == 2 && sameSize(0) && isZero(1))
          Result = Resolve(Op.Inputs[0]);
        break;
      case NdOp::INT_ADD:
        if (Op.NumInputs == 2 && sameSize(0) && isZero(1))
          Result = Resolve(Op.Inputs[0]);
        else if (Op.NumInputs == 2 && isZero(0) && sameSize(1))
          Result = Resolve(Op.Inputs[1]);
        break;
      case NdOp::INT_SUB:
        if (Op.NumInputs == 2 && sameSize(0) && isZero(1))
          Result = Resolve(Op.Inputs[0]);
        break;
      case NdOp::SELECT:
        if (Op.NumInputs >= 3 && sameSize(1) && sameSize(2) &&
            merge(Result, Op.Inputs[1]) && merge(Result, Op.Inputs[2]))
          break;
        Result.reset();
        break;
      case NdOp::LOAD: {
        detail::ReachingStackValues Reaching = reachingLoad(Block, OpIdx, Op);
        if (!Reaching.Complete || Reaching.Values.empty())
          break;
        for (const MedVar &Stored : Reaching.Values)
          if (!merge(Result, Stored)) {
            Result.reset();
            break;
          }
        break;
      }
      default:
        break;
      }
      return finish(Result);
    };
    return Resolve(V);
  }

  bool mayBeStackAddressThroughReloads(const MedVar &V) const {
    llvm::DenseMap<ValueKey, bool> Cache;
    llvm::DenseSet<ValueKey> Active;
    unsigned Remaining = MaxProofSteps;
    bool Incomplete = false;

    std::function<bool(const MedVar &, int)> MayBe = [&](const MedVar &Current,
                                                         int Depth) {
      if (!Have || Current.isConst())
        return false;
      const ValueKey Key = keyOf(Current);
      if (auto It = Cache.find(Key); It != Cache.end())
        return It->second;
      if (stackOffset(Current)) {
        Cache[Key] = true;
        return true;
      }
      if (Depth > 64 || Remaining == 0) {
        Incomplete = true;
        return true;
      }
      if (!Active.insert(Key).second) {
        Incomplete = true;
        return true;
      }
      struct PopActive {
        llvm::DenseSet<ValueKey> &Set;
        ValueKey Key;
        ~PopActive() { Set.erase(Key); }
      } Guard{Active, Key};
      --Remaining;

      bool Result = false;
      if (auto It = PhiDef.find(Key); It != PhiDef.end()) {
        const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
        for (const auto &[Pred, Arg] : Phi.Args) {
          (void)Pred;
          if (MayBe(Arg, Depth + 1)) {
            Result = true;
            break;
          }
        }
      } else if (auto It = OpDef.find(Key); It != OpDef.end()) {
        const MedBlock &Block = F.Blocks[It->second.first];
        const int OpIdx = It->second.second;
        const MedOp &Op = Block.Ops[OpIdx];
        if (Op.Opcode == NdOp::LOAD) {
          detail::ReachingStackValues Reaching = reachingLoad(Block, OpIdx, Op);
          if (!Reaching.Complete) {
            Incomplete = true;
            Result = true;
          } else {
            for (const MedVar &Stored : Reaching.Values)
              if (MayBe(Stored, Depth + 1)) {
                Result = true;
                break;
              }
          }
        } else {
          unsigned Begin = 0;
          unsigned End = Op.NumInputs;
          switch (Op.Opcode) {
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
            End = 0;
            break;
          }
          for (unsigned I = Begin; I < End; ++I)
            if (MayBe(Op.Inputs[I], Depth + 1)) {
              Result = true;
              break;
            }
        }
      }
      if (!Incomplete)
        Cache[Key] = Result;
      return Result;
    };
    return MayBe(V, 0);
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
      return Op->NumInputs == 1 && Op->Output.Size > 0 &&
                     Op->Output.Size == Op->Inputs[0].Size
                 ? stackOffsetRec(Op->Inputs[0], Depth + 1, Seen)
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
    if (Op.NumInputs != 2 || Op.Output.Size == 0)
      return std::nullopt;
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (Op.Inputs[1].isConst()) {
      if (Op.Inputs[0].Size != Op.Output.Size ||
          Op.Inputs[1].Size > Op.Output.Size)
        return std::nullopt;
      auto Delta = detail::signedStackConstant(Op.Inputs[1]);
      if (auto B = stackOffsetRec(Op.Inputs[0], Depth + 1, Seen); B && Delta)
        return detail::checkedStackOffset(*B, *Delta, Sub);
    } else if (Op.Inputs[0].isConst() && !Sub) {
      if (Op.Inputs[1].Size != Op.Output.Size ||
          Op.Inputs[0].Size > Op.Output.Size)
        return std::nullopt;
      auto Delta = detail::signedStackConstant(Op.Inputs[0]);
      if (auto B = stackOffsetRec(Op.Inputs[1], Depth + 1, Seen); B && Delta)
        return detail::checkedStackOffset(*B, *Delta, false);
    }
    return std::nullopt;
  }

  bool mayBeStackAddress(const MedVar &Root) const {
    llvm::DenseMap<ValueKey, bool> Cache;
    llvm::DenseSet<ValueKey> Active;
    unsigned Remaining = MaxProofSteps;
    bool Incomplete = false;

    std::function<bool(const MedVar &, int)> Prove = [&](const MedVar &V,
                                                         int Depth) {
      if (!Have || V.isConst())
        return false;
      const ValueKey Key = keyOf(V);
      if (auto It = Cache.find(Key); It != Cache.end())
        return It->second;
      if (Depth > 64 || Remaining == 0) {
        Incomplete = true;
        return true;
      }
      if (!Active.insert(Key).second) {
        Incomplete = true;
        return true;
      }
      struct PopActive {
        llvm::DenseSet<ValueKey> &Set;
        ValueKey Key;
        ~PopActive() { Set.erase(Key); }
      } Guard{Active, Key};
      --Remaining;

      bool Result = false;
      if (V.Kind == MedVar::Reg && (V.RegOff == SP || V.RegOff == FP)) {
        Result = true;
      } else if (auto It = PhiDef.find(Key); It != PhiDef.end()) {
        const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
        for (const auto &[Pred, Arg] : Phi.Args) {
          (void)Pred;
          if (Prove(Arg, Depth + 1)) {
            Result = true;
            break;
          }
        }
      } else if (const MedOp *Op = def(V)) {
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
          End = 0;
          break;
        }
        for (unsigned I = Begin; I < End; ++I)
          if (Prove(Op->Inputs[I], Depth + 1)) {
            Result = true;
            break;
          }
      }
      if (!Incomplete)
        Cache[Key] = Result;
      return Result;
    };
    return Prove(Root, 0);
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
    auto MayBeFrame = [&](const MedVar &V) { return mayBeStackAddress(V); };
    auto AliasesWholeFrame = [&](const MedVar &V) {
      if (!Have)
        return false;
      auto FindOp = [&](const MedVar &Current) { return def(Current); };
      auto FindPhi = [&](const MedVar &Current) -> const PhiNode * {
        auto It = PhiDef.find(keyOf(Current));
        return It == PhiDef.end()
                   ? nullptr
                   : &F.Blocks[It->second.first].Phis[It->second.second];
      };
      return detail::aliasesWholeFrame(V, SP, FP, FindOp, FindPhi);
    };
    return detail::reachingStackValues(F, B.Id, OpIdx, *Off, Op.Output.Size,
                                       Resolve, MayBeFrame, AliasesWholeFrame);
  }

  detail::ReachingStackValues reachingRead(const MedBlock &B, int OpIdx,
                                           const MedVar &Addr,
                                           uint16_t Size) const {
    if (Size == 0)
      return {};
    std::optional<int64_t> Off = stackOffsetThroughReloads(Addr);
    if (!Off)
      return {};
    auto Resolve = [&](const MedVar &V) {
      return stackOffsetThroughReloads(V);
    };
    auto MayBeFrame = [&](const MedVar &V) {
      return mayBeStackAddressThroughReloads(V);
    };
    auto AliasesWholeFrame = [&](const MedVar &V) {
      if (!Have)
        return false;
      auto FindOp = [&](const MedVar &Current) { return def(Current); };
      auto FindPhi = [&](const MedVar &Current) -> const PhiNode * {
        auto It = PhiDef.find(keyOf(Current));
        return It == PhiDef.end()
                   ? nullptr
                   : &F.Blocks[It->second.first].Phis[It->second.second];
      };
      return detail::aliasesWholeFrame(V, SP, FP, FindOp, FindPhi);
    };
    return detail::reachingStackValues(F, B.Id, OpIdx, *Off, Size, Resolve,
                                       MayBeFrame, AliasesWholeFrame,
                                       /*RequireMemoryRead=*/false,
                                       /*InvalidateEscapedAddresses=*/false);
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
            AllAlias &= Aliases && Arg.Size == Phi.Output.Size;
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
    if (ABlock == BBlock) {
      if (BOp > AOp)
        return true;
      auto It = IdToIndex.find(ABlock);
      if (It == IdToIndex.end())
        return false;
      for (int Succ : F.Blocks[It->second].Succs)
        if (blockReaches(Succ, BBlock))
          return true;
      return false;
    }
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
  llvm::DenseSet<int> MayFreeParam;
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
    if (!SummaryFixpointComplete && !In.MedFuncs->empty()) {
      const MedFunc &F = In.MedFuncs->front();
      Finding Fn;
      Fn.Origin = Track::Audit;
      Fn.Class = VulnClass::HeapLeak;
      Fn.TheVerdict = Verdict::Unknown;
      Fn.TheConfidence = Confidence::Low;
      Fn.Function = F.Name;
      Fn.FuncEntry = F.Entry;
      Fn.Name = "interprocedural lifetime summary";
      Fn.Sink = Fn.Name;
      Fn.Detail = "interprocedural heap lifetime summaries did not converge";
      Fn.BudgetHit = true;
      All.push_back(std::move(Fn));
      return All;
    }
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
  bool SummaryFixpointComplete = true;

  std::string callName(const MedCallInfo &CI) const {
    return resolveCallName(In, CI);
  }

  va_t canonicalCallTarget(const MedCallInfo &CI) const {
    if (!In.Img)
      return CI.TargetAddr;
    return normalizeCodeAddress(CI.TargetAddr, In.Img->Arch, In.Img->Mode);
  }

  const MedFunc *internalCallee(const MedCallInfo &CI) const {
    if (CI.IsIndirect)
      return nullptr;
    const MedFunc *Callee = In.findMedFunc(canonicalCallTarget(CI));
    if (!Callee)
      return Callee;
    if (classifyNameSource(In, CI.TargetAddr, CI.TargetName, CI.IsIndirect) ==
        NameSource::Import)
      return nullptr;
    if (CI.TargetAddr != 0)
      return Callee;
    if (CI.TargetName.empty() || isSynthesizedFuncName(CI.TargetName))
      return nullptr;

    const std::string TargetName = SinkCatalog::normalize(callName(CI));
    const auto MatchesTarget = [&](llvm::StringRef Candidate) {
      return !Candidate.empty() &&
             SinkCatalog::normalize(Candidate) == TargetName;
    };
    if (!MatchesTarget(Callee->Name) && !MatchesTarget(Callee->DebugName))
      return nullptr;
    return Callee;
  }

  const Summary *callSummary(const MedCallInfo &CI) const {
    const MedFunc *Callee = internalCallee(CI);
    if (!Callee)
      return nullptr;
    auto It = Summaries.find(Callee->Entry);
    return It == Summaries.end() ? nullptr : &It->second;
  }

  const SinkEntry *catalogSink(const MedCallInfo &CI) const {
    return Cat.matchSink(callName(CI));
  }

  bool argumentHasPointerWidth(const MedCallInfo &CI, int ArgIndex) const {
    return detail::callArgumentHasTargetPointerWidth(In.Img, CI, ArgIndex);
  }

  bool valueHasPointerWidth(const MedVar &Value) const {
    return detail::hasTargetPointerWidth(In.Img, Value);
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
    if (E->Kind == SinkKind::Realloc && E->Name == "reallocf")
      return E->HandleArg;
    return -1;
  }

  bool catalogFreeMayFail(const MedCallInfo &CI) const {
    const SinkEntry *E = catalogSink(CI);
    return E && E->Kind == SinkKind::Free && E->ReleaseMayFail;
  }

  bool freeArgMayFail(const MedCallInfo &CI, int ArgIndex) const {
    if (ArgIndex < 0)
      return false;
    if (ArgIndex == catalogFreeArg(CI))
      return catalogFreeMayFail(CI);
    if (const Summary *S = callSummary(CI))
      return S->MayFreeParam.count(ArgIndex) != 0;
    return false;
  }

  llvm::SmallVector<int, 4> freeArgsOf(const MedCallInfo &CI) const {
    llvm::SmallVector<int, 4> Args;
    int FA = catalogFreeArg(CI);
    if (FA >= 0) {
      Args.push_back(FA);
      return Args;
    }
    if (const Summary *S = callSummary(CI)) {
      for (int Param : S->FreesParam)
        Args.push_back(Param);
      for (int Param : S->MayFreeParam)
        if (std::find(Args.begin(), Args.end(), Param) == Args.end())
          Args.push_back(Param);
    }
    return Args;
  }

  bool allocates(const MedCallInfo &CI) const {
    if (const SinkEntry *E = catalogSink(CI))
      if (E->Kind == SinkKind::Alloc || E->Kind == SinkKind::Realloc)
        return true;
    if (const Summary *S = callSummary(CI); S && S->ReturnsHeap)
      return true;
    return false;
  }

  static bool sameSummary(const Summary &A, const Summary &B) {
    if (A.ReturnsHeap != B.ReturnsHeap ||
        A.FreesParam.size() != B.FreesParam.size() ||
        A.MayFreeParam.size() != B.MayFreeParam.size() ||
        A.EscapesParam.size() != B.EscapesParam.size() ||
        A.UnknownParam.size() != B.UnknownParam.size())
      return false;
    for (int P : A.FreesParam)
      if (!B.FreesParam.count(P))
        return false;
    for (int P : A.MayFreeParam)
      if (!B.MayFreeParam.count(P))
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
      return argumentHasPointerWidth(CI, ArgIndex) ? EscapeState::No
                                                   : EscapeState::Unknown;

    if (const MedFunc *Internal = internalCallee(CI)) {
      if (!argumentHasPointerWidth(CI, ArgIndex))
        return EscapeState::Unknown;
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
          if (!argumentHasPointerWidth(CI, FA)) {
            HasPotentialFree |= Alias.count(ArgKey) != 0;
            continue;
          }
          if (MustAlias.count(ArgKey)) {
            if (freeArgMayFail(CI, FA))
              HasPotentialFree = true;
            else
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
      else if (!DefiniteFrees.empty() || HasPotentialFree) {
        S.MayFreeParam.insert(PI);
        S.UnknownParam.insert(PI);
      }
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
                if (valueHasPointerWidth(Op.Inputs[I]))
                  DefiniteEscapes.push_back({B.Id, OI});
                else
                  HasPotentialEscape = true;
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
            if (MustAlias.count(StoredKey)) {
              if (valueHasPointerWidth(Val))
                DefiniteEscapes.push_back({B.Id, OI});
              else
                HasPotentialEscape = true;
            } else
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
      if (!Alloc)
        if (const Summary *CalleeSummary = callSummary(CI))
          Alloc = CalleeSummary->ReturnsHeap;
      if (!Alloc)
        continue;
      const MedOp *Op = opAt(F, CI.BlockId, CI.OpIdx);
      if (!Op || Op->Output.isConst() || Op->Output.Size == 0 ||
          !valueHasPointerWidth(Op->Output))
        continue;
      llvm::DenseSet<ValueKey> MustAlias = Mem.mustAliasClosure(Op->Output);
      std::vector<FreeEvent> ReturnEvents;
      bool HasIncompleteReturn = false;
      for (const MedBlock &B : F.Blocks)
        for (int OI = 0; OI < static_cast<int>(B.Ops.size()); ++OI) {
          const MedOp &O = B.Ops[OI];
          if (O.Opcode != NdOp::RETURN)
            continue;
          for (unsigned Input = 0; Input < O.NumInputs; ++Input)
            if (!O.Inputs[Input].isConst() &&
                MustAlias.count(keyOf(O.Inputs[Input]))) {
              if (valueHasPointerWidth(O.Inputs[Input]))
                ReturnEvents.push_back({B.Id, OI});
              else
                HasIncompleteReturn = true;
              break;
            }
        }
      std::vector<FreeEvent> DefiniteFrees;
      bool HasIncompleteFree = false;
      for (const MedCallInfo &FC : F.CallInfos)
        for (int FreeArg : freeArgsOf(FC)) {
          if (FreeArg < 0 || FreeArg >= static_cast<int>(FC.Args.size()) ||
              FC.Args[FreeArg].isConst())
            continue;
          const bool AliasesAllocation =
              MustAlias.count(keyOf(FC.Args[FreeArg])) != 0;
          if (!argumentHasPointerWidth(FC, FreeArg)) {
            HasIncompleteFree |= AliasesAllocation;
            continue;
          }
          if (AliasesAllocation && !freeArgMayFail(FC, FreeArg)) {
            DefiniteFrees.push_back({FC.BlockId, FC.OpIdx});
            break;
          }
        }
      if (!HasIncompleteReturn && !HasIncompleteFree &&
          reachesAnyEventWithoutBlocker(F, CI.BlockId, CI.OpIdx, ReturnEvents,
                                        DefiniteFrees))
        S.ReturnsHeap = true;
    }
    return S;
  }

  void buildSummaries() {
    if (!In.MedFuncs)
      return;

    llvm::DenseMap<va_t, const MedFunc *> Functions;
    llvm::DenseMap<va_t, llvm::SmallVector<va_t, 4>> Callers;
    std::deque<va_t> Work;
    llvm::DenseSet<va_t> Queued;
    size_t SummaryStateUnits = In.MedFuncs->size();

    for (const MedFunc &F : *In.MedFuncs) {
      Functions[F.Entry] = &F;
      Work.push_back(F.Entry);
      Queued.insert(F.Entry);
      SummaryStateUnits += F.Params.size() * 5;
    }
    for (const MedFunc &F : *In.MedFuncs)
      for (const MedCallInfo &CI : F.CallInfos) {
        const MedFunc *Callee = internalCallee(CI);
        if (!Callee)
          continue;
        Callers[Callee->Entry].push_back(F.Entry);
      }

    // Each summary has one return fact plus a finite set of per-parameter
    // lifetime facts.  The input-scaled cap is only a termination guard for a
    // malformed or unexpectedly non-monotone summary cycle; exhausting it is
    // reported as UNKNOWN rather than publishing a partial fixed point.
    const size_t MaxChanges = std::max<size_t>(1024, SummaryStateUnits * 8 + 1);
    size_t Changes = 0;

    while (!Work.empty()) {
      const va_t Entry = Work.front();
      Work.pop_front();
      Queued.erase(Entry);
      auto FunctionIt = Functions.find(Entry);
      if (FunctionIt == Functions.end())
        continue;

      Summary S = summarize(*FunctionIt->second);
      auto It = Summaries.find(Entry);
      if (It != Summaries.end() && sameSummary(It->second, S))
        continue;
      Summaries[Entry] = std::move(S);
      if (++Changes > MaxChanges) {
        SummaryFixpointComplete = false;
        break;
      }

      auto CallerIt = Callers.find(Entry);
      if (CallerIt == Callers.end())
        continue;
      for (va_t Caller : CallerIt->second)
        if (Queued.insert(Caller).second)
          Work.push_back(Caller);
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
                               bool *ReachedEvent = nullptr,
                               bool FollowExceptional = true) const {
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
      if (FollowExceptional && ExpandedExceptional.insert(BlkId).second)
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
                        const std::vector<FreeEvent> &Frees,
                        bool FollowExceptional = true) const {
    return reachesExitWithoutEvent(F, Alloc.BlockId, Alloc.OpIdx, Frees,
                                   nullptr, FollowExceptional);
  }

  bool hasUnresolvedLifecycleCall(const MedFunc &F, const CFG &G,
                                  const MedCallInfo &Alloc) const {
    for (const MedCallInfo &CI : F.CallInfos) {
      if (!G.after(Alloc.BlockId, Alloc.OpIdx, CI.BlockId, CI.OpIdx))
        continue;
      if (const SinkEntry *E = catalogSink(CI))
        if (E->Kind == SinkKind::Realloc &&
            (E->HandleArg < 0 ||
             E->HandleArg >= static_cast<int>(CI.Args.size()) ||
             !argumentHasPointerWidth(CI, E->HandleArg)))
          return true;
      for (int FreeArg : freeArgsOf(CI))
        if (FreeArg < 0 || FreeArg >= static_cast<int>(CI.Args.size()) ||
            !argumentHasPointerWidth(CI, FreeArg))
          return true;
      if (CI.Args.empty() && catalogSink(CI) == nullptr && !internalCallee(CI))
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
            isStringLengthCall(Name) || internalCallee(*Call))
          ++Count;
      }
    }
    return Count;
  }

  bool corroborate(Finding &Fn) {
    const bool ConfirmUnsafe = Fn.TheVerdict == Verdict::Unsafe;
    const bool FilterUnknownLeakPath =
        Fn.TheVerdict == Verdict::Unknown && Fn.RequireReturnedPath &&
        Fn.RequireNonNullCallVA != 0 && !Fn.RequiredPathEvents.empty();
    if (!ConfirmUnsafe && !FilterUnknownLeakPath)
      return true;
    const LowFunc *LF = In.findLowFunc(Fn.FuncEntry);
    auto failClosed = [&](llvm::StringRef Reason, bool Budget = false) {
      if (ConfirmUnsafe) {
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail = "symbolic corroboration did not establish the candidate";
      }
      Fn.BudgetHit = Budget;
      Fn.Corroboration = Reason.str();
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
      if (ConfirmUnsafe)
        Fn.TheConfidence = Confidence::High;
      Fn.Corroboration =
          ConfirmUnsafe
              ? "candidate event sequence is feasible on a symbolic path"
              : "a necessary unresolved-lifetime path is feasible";
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
      if (!valueHasPointerWidth(AllocOp->Output)) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail = "allocation result has incompatible target pointer width";
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
      std::vector<FreeEvent> FallibleFrees;
      bool HasPotentialFree = false;
      for (const MedCallInfo &FC : F.CallInfos) {
        for (int FA : freeArgsOf(FC)) {
          if (FA < 0 || FA >= static_cast<int>(FC.Args.size()))
            continue;
          if (!argumentHasPointerWidth(FC, FA)) {
            HasPotentialFree = true;
            continue;
          }
          const ValueKey ArgKey = keyOf(FC.Args[FA]);
          if (MustAlias.count(ArgKey)) {
            FreeEvent Event{FC.BlockId,    FC.OpIdx,     callVA(F, FC),
                            FC.TargetAddr, callName(FC), FC.IsIndirect};
            if (freeArgMayFail(FC, FA)) {
              FallibleFrees.push_back(std::move(Event));
              HasPotentialFree = true;
            } else {
              Frees.push_back(std::move(Event));
            }
            break;
          }
          HasPotentialFree |= Alias.count(ArgKey) != 0;
        }
      }

      const EscapeState Escape = escapes(F, Mem, Alias, MustAlias, CI);
      const bool UnresolvedLifecycle =
          HasPotentialFree || hasUnresolvedLifecycleCall(F, G, CI);

      bool ReportedDouble = false;
      for (size_t A = 0; A < Frees.size() && !ReportedDouble; ++A)
        for (size_t B = 0; B < Frees.size(); ++B) {
          const std::vector<FreeEvent> Target = {Frees[B]};
          const bool NormalPath = reachesAnyEventWithoutBlocker(
              F, Frees[A].BlockId, Frees[A].OpIdx, Target, AllocationSite,
              /*FollowExceptional=*/false);
          const bool ExceptionalPath =
              !NormalPath &&
              reachesAnyEventWithoutBlocker(F, Frees[A].BlockId, Frees[A].OpIdx,
                                            Target, AllocationSite,
                                            /*FollowExceptional=*/true);
          if (NormalPath || ExceptionalPath) {
            Finding Fn = baseFinding(F, VulnClass::DoubleFree, Frees[B].VA,
                                     Frees[B].Name, Frees[B].TargetAddr,
                                     Frees[B].IsIndirect);
            if (ExceptionalPath) {
              Fn.TheVerdict = Verdict::Unknown;
              Fn.TheConfidence = Confidence::Low;
              Fn.Detail = "a second release is reachable only through "
                          "exceptional control flow";
            } else {
              Fn.TheConfidence = Confidence::High;
              Fn.Detail = "handle released twice on a path";
              Fn.RequiredPathEvents = {
                  {CI.BlockId, CI.OpIdx},
                  {Frees[A].BlockId, Frees[A].OpIdx},
                  {Frees[B].BlockId, Frees[B].OpIdx},
              };
              Fn.RequireNonNullCallVA = AllocVA;
            }
            Out.push_back(std::move(Fn));
            ReportedDouble = true;
            break;
          }
        }

      if (!ReportedDouble && !FallibleFrees.empty()) {
        struct ReleaseRef {
          const FreeEvent *Event = nullptr;
          bool MayFail = false;
        };
        std::vector<ReleaseRef> Releases;
        Releases.reserve(Frees.size() + FallibleFrees.size());
        for (const FreeEvent &Fr : Frees)
          Releases.push_back({&Fr, false});
        for (const FreeEvent &Fr : FallibleFrees)
          Releases.push_back({&Fr, true});
        for (const ReleaseRef &First : Releases) {
          if (ReportedDouble)
            break;
          for (const ReleaseRef &Second : Releases) {
            if (!First.MayFail && !Second.MayFail)
              continue;
            const std::vector<FreeEvent> Target = {*Second.Event};
            if (!reachesAnyEventWithoutBlocker(
                    F, First.Event->BlockId, First.Event->OpIdx, Target,
                    AllocationSite, /*FollowExceptional=*/true))
              continue;
            Finding Fn = baseFinding(
                F, VulnClass::DoubleFree, Second.Event->VA, Second.Event->Name,
                Second.Event->TargetAddr, Second.Event->IsIndirect);
            Fn.TheVerdict = Verdict::Unknown;
            Fn.TheConfidence = Confidence::Low;
            Fn.Detail = "a conditional release may have occurred before a "
                        "later release";
            Out.push_back(std::move(Fn));
            ReportedDouble = true;
            break;
          }
        }
      }

      CollectedUses Uses = collectUses(F, Alias, MustAlias, Frees);
      bool ReportedUAF = false;
      for (const FreeEvent &Fr : Frees) {
        if (ReportedUAF)
          break;
        for (const UseEvent &U : Uses.Definite) {
          const std::vector<FreeEvent> Target = {{U.BlockId, U.OpIdx}};
          const bool NormalPath = reachesAnyEventWithoutBlocker(
              F, Fr.BlockId, Fr.OpIdx, Target, AllocationSite,
              /*FollowExceptional=*/false);
          const bool ExceptionalPath =
              !NormalPath &&
              reachesAnyEventWithoutBlocker(F, Fr.BlockId, Fr.OpIdx, Target,
                                            AllocationSite,
                                            /*FollowExceptional=*/true);
          if (NormalPath || ExceptionalPath) {
            Finding Fn = baseFinding(F, VulnClass::UseAfterFree, U.VA, Fr.Name,
                                     Fr.TargetAddr, Fr.IsIndirect);
            if (ExceptionalPath) {
              Fn.TheVerdict = Verdict::Unknown;
              Fn.TheConfidence = Confidence::Low;
              Fn.Detail = "a post-release use is reachable only through "
                          "exceptional control flow";
            } else {
              Fn.TheConfidence = Confidence::High;
              Fn.Detail = "handle used after it was released";
              Fn.RequiredPathEvents = {
                  {CI.BlockId, CI.OpIdx},
                  {Fr.BlockId, Fr.OpIdx},
                  {U.BlockId, U.OpIdx},
              };
              Fn.RequireNonNullCallVA = AllocVA;
            }
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
                                               /*FollowExceptional=*/true))
              continue;
            Finding Fn = baseFinding(F, VulnClass::UseAfterFree, U.VA, U.Name,
                                     U.TargetAddr, U.IsIndirect);
            Fn.TheVerdict = Verdict::Unknown;
            Fn.TheConfidence = Confidence::Low;
            Fn.Detail =
                U.Name == "memory_access"
                    ? "a derived address may access a handle after it "
                      "was released"
                    : "callee may access a handle after it was released";
            Out.push_back(std::move(Fn));
            ReportedPossibleUAF = true;
            break;
          }
          if (ReportedPossibleUAF)
            break;
        }

      if (!ReportedUAF)
        for (const FreeEvent &Fr : FallibleFrees) {
          bool Found = false;
          auto reportPossibleUse = [&](const UseEvent &U) {
            const std::vector<FreeEvent> Target = {{U.BlockId, U.OpIdx}};
            if (!reachesAnyEventWithoutBlocker(F, Fr.BlockId, Fr.OpIdx, Target,
                                               AllocationSite,
                                               /*FollowExceptional=*/true))
              return false;
            Finding Fn = baseFinding(F, VulnClass::UseAfterFree, U.VA, Fr.Name,
                                     Fr.TargetAddr, Fr.IsIndirect);
            Fn.TheVerdict = Verdict::Unknown;
            Fn.TheConfidence = Confidence::Low;
            Fn.Detail = "a conditional release may have occurred before this "
                        "handle use";
            Out.push_back(std::move(Fn));
            return true;
          };
          for (const UseEvent &U : Uses.Definite)
            if ((Found = reportPossibleUse(U)))
              break;
          if (!Found)
            for (const UseEvent &U : Uses.Possible)
              if ((Found = reportPossibleUse(U)))
                break;
          if (Found) {
            ReportedUAF = true;
            break;
          }
        }

      const bool MayLeakOnNormalPath =
          shouldReportLeak(F, CI, Frees, /*FollowExceptional=*/false);
      const bool MayLeakOnlyOnExceptionalPath =
          !MayLeakOnNormalPath &&
          shouldReportLeak(F, CI, Frees, /*FollowExceptional=*/true);
      const bool MayLeak = MayLeakOnNormalPath || MayLeakOnlyOnExceptionalPath;
      if (MayLeak && (Escape == EscapeState::Unknown || UnresolvedLifecycle)) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail =
            UnresolvedLifecycle
                ? (!FallibleFrees.empty()
                       ? "heap lifetime depends on a conditional release"
                       : "heap lifetime depends on a call with unrecovered "
                         "arguments")
                : "heap handle reaches a call without a retention summary";
        if (MayLeakOnNormalPath) {
          Fn.RequiredPathEvents = {{CI.BlockId, CI.OpIdx}};
          for (const FreeEvent &Fr : Frees)
            Fn.ForbiddenPathEvents.push_back({Fr.BlockId, Fr.OpIdx});
          Fn.RequireReturnedPath = true;
          Fn.RequireNonNullCallVA = AllocVA;
        }
        Out.push_back(std::move(Fn));
      } else if (MayLeak && Escape == EscapeState::No) {
        Finding Fn = baseFinding(F, VulnClass::HeapLeak, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        if (MayLeakOnlyOnExceptionalPath) {
          Fn.TheVerdict = Verdict::Unknown;
          Fn.TheConfidence = Confidence::Low;
          Fn.Detail = "an unreleased allocation is reachable only through "
                      "exceptional control flow";
        } else {
          Fn.TheConfidence = Confidence::Medium;
          Fn.Detail = "allocation neither released nor escaped on every path";
          Fn.RequiredPathEvents = {{CI.BlockId, CI.OpIdx}};
          for (const FreeEvent &Fr : Frees)
            Fn.ForbiddenPathEvents.push_back({Fr.BlockId, Fr.OpIdx});
          Fn.RequireReturnedPath = true;
          Fn.RequireNonNullCallVA = AllocVA;
        }
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
        const std::optional<int64_t> Offset =
            Mem.stackOffsetThroughReloads(*Addr);
        // Positive entry-SP offsets are caller-owned argument/ABI storage.
        // This audit only claims local frame bytes below the entry SP.
        if (!Offset || *Offset >= 0)
          continue;
        const detail::ReachingStackValues Reaching =
            Mem.reachingRead(B, Oi, *Addr, Op.Output.Size);
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

    auto auditCallStackSource = [&](const MedCallInfo &CI, int SourceArg,
                                    std::optional<uint64_t> Bytes,
                                    llvm::StringRef UnknownDetail,
                                    llvm::StringRef DefiniteDetail,
                                    llvm::StringRef PossibleDetail) {
      if (SourceArg < 0 || SourceArg >= static_cast<int>(CI.Args.size()))
        return;
      const MedOp *CallOp = opAt(F, CI.BlockId, CI.OpIdx);
      if (!CallOp ||
          (CallOp->Opcode != NdOp::CALL && CallOp->Opcode != NdOp::INDIR_CALL))
        return;
      const MedBlock *Block = nullptr;
      for (const MedBlock &Candidate : F.Blocks)
        if (Candidate.Id == CI.BlockId) {
          Block = &Candidate;
          break;
        }
      if (!Block)
        return;
      const MedVar &Source = CI.Args[SourceArg];
      const std::optional<int64_t> Offset =
          Mem.stackOffsetThroughReloads(Source);
      if (!Offset || *Offset >= 0)
        return;
      if (!Bytes || *Bytes > std::numeric_limits<uint16_t>::max()) {
        Finding Fn = baseFinding(F, VulnClass::UninitializedRead, callVA(F, CI),
                                 callName(CI), CI.TargetAddr, CI.IsIndirect);
        Fn.ArgIndex = SourceArg;
        Fn.TheVerdict = Verdict::Unknown;
        Fn.TheConfidence = Confidence::Low;
        Fn.Detail = UnknownDetail.str();
        Out.push_back(std::move(Fn));
        return;
      }
      const uint16_t ReadSize = static_cast<uint16_t>(*Bytes);
      const detail::ReachingStackValues Reaching =
          Mem.reachingRead(*Block, CI.OpIdx, Source, ReadSize);
      if (!Reaching.Reachable ||
          (!Reaching.MayBeUninitialized && !Reaching.HasUnknownWrites))
        return;

      const bool Definite =
          !Reaching.HasUnknownWrites && Reaching.Values.empty();
      Finding Fn = baseFinding(F, VulnClass::UninitializedRead, callVA(F, CI),
                               callName(CI), CI.TargetAddr, CI.IsIndirect);
      Fn.ArgIndex = SourceArg;
      Fn.TheVerdict = Definite ? Verdict::Unsafe : Verdict::Unknown;
      Fn.TheConfidence = Definite ? Confidence::Medium : Confidence::Low;
      Fn.Detail = (Definite ? DefiniteDetail : PossibleDetail).str();
      if (Definite)
        Fn.RequiredPathEvents = {{CI.BlockId, CI.OpIdx}};
      Out.push_back(std::move(Fn));
    };

    for (const MedCallInfo &CI : F.CallInfos) {
      const StringDuplicationKind Kind = stringDuplicationKind(callName(CI));
      if (Kind == StringDuplicationKind::None)
        continue;
      std::optional<uint64_t> Bytes = minimumStringDuplicationReadBytes(
          Kind, In.Img ? In.Img->Format : BinaryFormat::Unknown);
      if (Kind == StringDuplicationKind::Counted) {
        if (CI.Args.size() <= 1)
          Bytes = std::nullopt;
        else if (std::optional<uint64_t> Limit = unsignedConstant(CI.Args[1])) {
          if (*Limit == 0)
            continue;
        } else {
          Bytes = std::nullopt;
        }
      }
      auditCallStackSource(
          CI, 0, Bytes,
          "string duplication source has a runtime length that may read "
          "uninitialized local stack bytes",
          "string duplication reads a local stack byte before initialization",
          "string duplication may read a local stack byte before "
          "initialization on a control-flow path");
    }

    for (const MedCallInfo &CI : F.CallInfos) {
      const SinkEntry *E = catalogSink(CI);
      if (!E || E->Kind != SinkKind::Copy || E->SrcArg < 0 || E->LenArg < 0 ||
          E->SrcArg >= static_cast<int>(CI.Args.size()) ||
          E->LenArg >= static_cast<int>(CI.Args.size()))
        continue;
      if (!detail::isExactCopySourceRead(callName(CI)))
        continue;
      std::optional<uint64_t> Count = unsignedConstant(CI.Args[E->LenArg]);
      if (Count && *Count == 0)
        continue;
      const std::optional<uint64_t> Bytes =
          Count ? detail::exactCopyReadBytes(
                      callName(CI),
                      In.Img ? In.Img->Format : BinaryFormat::Unknown, *Count)
                : std::nullopt;
      if (Count && E->CapArg >= 0 &&
          E->CapArg < static_cast<int>(CI.Args.size()))
        if (std::optional<uint64_t> Capacity =
                unsignedConstant(CI.Args[E->CapArg]);
            Capacity &&
            detail::fortifiedCountedAccessIsRejected(
                callName(CI), In.Img ? In.Img->Format : BinaryFormat::Unknown,
                *Count, *Capacity))
          continue;
      auditCallStackSource(
          CI, E->SrcArg, Bytes,
          !Count ? "copy source has a runtime length that may extend past "
                   "initialized local stack bytes"
                 : "copy source byte count exceeds the range of the local "
                   "stack initialization audit",
          "copy source reads local stack bytes before any full-width "
          "initialization",
          "copy source may read local stack bytes before initialization on a "
          "control-flow path");
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
        if (!argumentHasPointerWidth(CI, ArgIndex))
          return CallUse::Possible;
        if (E->UnboundedWrite && ArgIndex == E->DstArg &&
            Cat.matchSource(callName(CI)))
          return CallUse::Possible;
        if (E->LenArg >= 0 && E->LenArg < static_cast<int>(CI.Args.size()) &&
            E->CapArg >= 0 && E->CapArg < static_cast<int>(CI.Args.size()))
          if (std::optional<uint64_t> Count =
                  unsignedConstant(CI.Args[E->LenArg]);
              Count)
            if (std::optional<uint64_t> Capacity =
                    unsignedConstant(CI.Args[E->CapArg]);
                Capacity && detail::fortifiedCountedAccessIsRejected(
                                callName(CI),
                                In.Img ? In.Img->Format : BinaryFormat::Unknown,
                                *Count, *Capacity))
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
        if ((ArgIndex == E->FmtArg || ArgIndex == E->DstArg) &&
            !argumentHasPointerWidth(CI, ArgIndex))
          return CallUse::Possible;
        if (E->LenArg >= 0 && E->LenArg < static_cast<int>(CI.Args.size()) &&
            E->CapArg >= 0 && E->CapArg < static_cast<int>(CI.Args.size()))
          if (std::optional<uint64_t> Limit =
                  unsignedConstant(CI.Args[E->LenArg]);
              Limit)
            if (std::optional<uint64_t> Capacity =
                    unsignedConstant(CI.Args[E->CapArg]);
                Capacity && detail::fortifiedCountedAccessIsRejected(
                                callName(CI),
                                In.Img ? In.Img->Format : BinaryFormat::Unknown,
                                *Limit, *Capacity))
              return CallUse::None;
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
      case SinkKind::Alloc: {
        if (E->HandleArg >= 0 && ArgIndex == E->HandleArg)
          return argumentHasPointerWidth(CI, ArgIndex) ? CallUse::Definite
                                                       : CallUse::Possible;
        if (ArgIndex == 0 &&
            stringDuplicationKind(callName(CI)) !=
                StringDuplicationKind::None &&
            !argumentHasPointerWidth(CI, ArgIndex))
          return CallUse::Possible;
        if (ArgIndex != 0)
          return CallUse::None;
        const StringDuplicationKind DupKind =
            stringDuplicationKind(callName(CI));
        if (DupKind == StringDuplicationKind::NullTerminated ||
            DupKind == StringDuplicationKind::WideNullTerminated)
          return CallUse::Definite;
        if (DupKind == StringDuplicationKind::Counted) {
          if (CI.Args.size() <= 1)
            return CallUse::Possible;
          if (std::optional<uint64_t> Limit = unsignedConstant(CI.Args[1]))
            return *Limit == 0 ? CallUse::None : CallUse::Definite;
          return CallUse::Possible;
        }
        return CallUse::None;
      }
      case SinkKind::Realloc:
        return ArgIndex == E->HandleArg
                   ? (argumentHasPointerWidth(CI, ArgIndex) ? CallUse::Definite
                                                            : CallUse::Possible)
                   : CallUse::None;
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
      return ArgIndex < static_cast<int>(FixedCount)
                 ? (argumentHasPointerWidth(CI, ArgIndex) ? CallUse::Definite
                                                          : CallUse::Possible)
                 : CallUse::Possible;
    }
    if (const SourceEntry *Source = Cat.matchSource(Name)) {
      if (ArgIndex != Source->OutArg)
        return CallUse::Possible;
      const std::string Normalized = SinkCatalog::normalize(Name);
      auto countedOutputUse = [&](int CountArg) {
        if (CountArg < 0 || CountArg >= static_cast<int>(CI.Args.size()))
          return CallUse::Possible;
        if (std::optional<uint64_t> Count = unsignedConstant(CI.Args[CountArg]))
          if (*Count == 0)
            return CallUse::None;
        return CallUse::Possible;
      };
      if (Normalized == "read" || Normalized == "pread" ||
          Normalized == "read_chk" || Normalized == "pread_chk" ||
          Normalized == "recv" || Normalized == "recvfrom" ||
          Normalized == "recv_chk" || Normalized == "recvfrom_chk" ||
          Normalized == "ReadFile" || Normalized == "GetEnvironmentVariableA" ||
          Normalized == "GetEnvironmentVariableW")
        return countedOutputUse(2);
      if (Normalized == "fgets" || Normalized == "fgets_unlocked")
        return countedOutputUse(1);
      if (Normalized == "fgets_chk" || Normalized == "fgets_unlocked_chk")
        return countedOutputUse(2);
      if (Normalized == "fread" || Normalized == "fread_unlocked" ||
          Normalized == "fread_chk" || Normalized == "fread_unlocked_chk") {
        const bool Fortified = Normalized.ends_with("_chk");
        const int ElementSizeArg = Fortified ? 2 : 1;
        const int ElementCountArg = Fortified ? 3 : 2;
        if (ElementCountArg >= static_cast<int>(CI.Args.size()))
          return CallUse::Possible;
        const std::optional<uint64_t> ElementSize =
            unsignedConstant(CI.Args[ElementSizeArg]);
        const std::optional<uint64_t> ElementCount =
            unsignedConstant(CI.Args[ElementCountArg]);
        if ((ElementSize && *ElementSize == 0) ||
            (ElementCount && *ElementCount == 0))
          return CallUse::None;
        return CallUse::Possible;
      }
      // External input APIs may fail or produce no output. Without a call
      // result predicate, reaching the call proves only a possible access.
      return CallUse::Possible;
    }
    if (isStringLengthCall(Name))
      return ArgIndex == 0
                 ? (argumentHasPointerWidth(CI, ArgIndex) ? CallUse::Definite
                                                          : CallUse::Possible)
                 : CallUse::None;
    return CallUse::Possible;
  }

  CollectedUses collectUses(const MedFunc &F,
                            const llvm::DenseSet<ValueKey> &Alias,
                            const llvm::DenseSet<ValueKey> &MustAlias,
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
          if (!Addr || Addr->isConst())
            continue;
          const ValueKey AddrKey = keyOf(*Addr);
          if (!Alias.count(AddrKey))
            continue;
          UseEvent Event{B.Id, Oi, Op.Addr, 0, "memory_access", false};
          (MustAlias.count(AddrKey) && valueHasPointerWidth(*Addr)
               ? Uses.Definite
               : Uses.Possible)
              .push_back(std::move(Event));
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
        if (ArgUse == CallUse::Definite && MustAlias.count(keyOf(CI.Args[I]))) {
          Use = CallUse::Definite;
          break;
        }
        if (ArgUse != CallUse::None)
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
                      const llvm::DenseSet<ValueKey> &MustAlias,
                      const MedCallInfo &Alloc) const {
    bool Unknown = false;
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops) {
        if (Op.Opcode == NdOp::RETURN && returnsValue(F)) {
          for (unsigned I = 0; I < Op.NumInputs; ++I) {
            if (Op.Inputs[I].isConst())
              continue;
            const ValueKey InputKey = keyOf(Op.Inputs[I]);
            if (!Alias.count(InputKey))
              continue;
            if (MustAlias.count(InputKey)) {
              if (valueHasPointerWidth(Op.Inputs[I]))
                return EscapeState::Yes;
              Unknown = true;
              continue;
            }
            Unknown = true;
          }
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
          const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
          const MedVar &Val = Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
          // Spilling the handle into its own stack frame is not an escape; only
          // a write through a non-stack address publishes it.
          if (Val.isConst() || Mem.stackOffset(Addr))
            continue;
          const ValueKey StoredKey = keyOf(Val);
          if (!Alias.count(StoredKey))
            continue;
          if (MustAlias.count(StoredKey)) {
            if (valueHasPointerWidth(Val))
              return EscapeState::Yes;
            Unknown = true;
            continue;
          }
          Unknown = true;
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
          if (MustAlias.count(keyOf(CI.Args[I])))
            return EscapeState::Yes;
          Unknown = true;
          break;
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
