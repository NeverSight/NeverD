//===- ArgSlicer.cpp - Classify a sink argument by its provenance ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ArgSlicer.h"

#include "SourceSemantics.h"
#include "StackSlotFlow.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/safety/SinkScanner.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

using namespace neverd;
using namespace neverd::safety;

namespace {

using ValueKey = std::tuple<uint8_t, int, int>;
using ConstantSourceKey =
    std::tuple<uint64_t, uint16_t, ConstantAddressProvenance, uint64_t>;

ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

ConstantSourceKey constantSourceKey(const MedVar &V) {
  return {V.ConstVal, V.Size, V.Provenance, V.AddressOwnerVA};
}

bool isNumericConstant(const MedVar &V) {
  return V.isConst() && V.Size > 0 && V.Size <= sizeof(uint64_t) &&
         !isAddressProvenance(V.Provenance);
}

uint64_t numericConstantValue(const MedVar &V) {
  const uint32_t BitWidth = uint32_t(V.Size) * 8;
  if (BitWidth >= 64)
    return V.ConstVal;
  return V.ConstVal & ((uint64_t(1) << BitWidth) - 1);
}

bool isExactValue(const MedVar &A, const MedVar &B) {
  return A == B && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag;
}

bool isLengthReturn(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_CALL_TRAIT(NAME, IS_LENGTH, IS_BOUNDED)                         \
  .Case(NAME, IS_LENGTH != 0)
#include "neverd/safety/SafetyCallTraits.inc"
#undef SAFETY_CALL_TRAIT
      .Default(false);
}

bool isCappedLengthReturn(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
      .Cases({"strnlen", "strnlen_s", "wcsnlen"}, true)
      .Default(false);
}

bool isMainLike(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_ENTRY_NAME(NAME) .Case(NAME, true)
#include "neverd/safety/SafetyEntryNames.inc"
#undef SAFETY_ENTRY_NAME
      .Default(false);
}

// Locates the op or phi that defines each SSA value, so a use can be walked
// back to its definition in one hop.
struct DefIndex {
  llvm::DenseMap<ValueKey, std::pair<int, int>> OpDef;  ///< value -> (blk,op).
  llvm::DenseMap<ValueKey, std::pair<int, int>> PhiDef; ///< value -> (blk,phi).

  explicit DefIndex(const MedFunc &F) {
    for (int Bi = 0; Bi < static_cast<int>(F.Blocks.size()); ++Bi) {
      const MedBlock &B = F.Blocks[Bi];
      for (int Pi = 0; Pi < static_cast<int>(B.Phis.size()); ++Pi)
        if (!B.Phis[Pi].Output.isConst() && B.Phis[Pi].Output.Size > 0)
          PhiDef[keyOf(B.Phis[Pi].Output)] = {Bi, Pi};
      for (int Oi = 0; Oi < static_cast<int>(B.Ops.size()); ++Oi) {
        const MedOp &O = B.Ops[Oi];
        if (!O.Output.isConst() && O.Output.Size > 0)
          OpDef[keyOf(O.Output)] = {Bi, Oi};
      }
    }
  }
};

class Slicer {
public:
  Slicer(const AnalysisInput &In, const SinkCatalog &Cat, const MedFunc &F,
         size_t SinkCallInfoIndex)
      : In(In), Cat(Cat), F(F), Defs(F), SinkCallInfoIndex(SinkCallInfoIndex) {
    indexSourceOutputs(SinkCallInfoIndex);
  }

  ArgClassification run(const MedVar &Arg) {
    ArgClassification R;
    resetClassificationProof();
    const MedCallInfo &Sink = F.CallInfos[SinkCallInfoIndex];
    if (std::optional<std::string> Source =
            pointeeSourceBefore(Arg, Sink.BlockId, Sink.OpIdx)) {
      R.Flow = ArgFlow::Tainted;
      R.TaintSource = *Source;
      R.Reason = "reaches external input " + *Source;
    } else {
      R.Flow = classify(Arg, 0, R);
    }
    resetUpperBoundProof();
    R.UpperBound = upperBound(Arg, 0);
    if (R.ConstValue)
      R.UpperBound = R.ConstValue;
    if (R.Flow == ArgFlow::Bounded && !R.UpperBound)
      R.Flow = ArgFlow::Unknown;
    return R;
  }

private:
  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const MedFunc &F;
  DefIndex Defs;
  size_t SinkCallInfoIndex = 0;
  llvm::DenseMap<ValueKey, ArgFlow> ClassificationCache;
  llvm::DenseSet<ValueKey> ClassificationActive;
  llvm::DenseMap<ValueKey, std::optional<uint64_t>> UpperBoundCache;
  llvm::DenseSet<ValueKey> UpperBoundActive;
  unsigned ClassificationProofBudget = 0;
  unsigned UpperBoundProofBudget = 0;
  bool ClassificationProofIncomplete = false;
  bool UpperBoundProofIncomplete = false;
  llvm::DenseMap<ValueKey, std::string> TaintedOutputValues;
  std::map<ConstantSourceKey, std::string> TaintedOutputConstants;
  struct IndexedSourceOutput {
    MedVar Value;
    size_t CallInfoIndex = 0;
    std::string Name;
  };
  std::vector<IndexedSourceOutput> IndexedSourceOutputs;
  std::vector<IndexedSourceOutput> IndexedPointeeOutputs;

  static constexpr unsigned MaxProofSteps = 4096;

  void resetClassificationProof() {
    ClassificationCache.clear();
    ClassificationActive.clear();
    ClassificationProofBudget = MaxProofSteps;
    ClassificationProofIncomplete = false;
  }

  void resetUpperBoundProof() {
    UpperBoundCache.clear();
    UpperBoundActive.clear();
    UpperBoundProofBudget = MaxProofSteps;
    UpperBoundProofIncomplete = false;
  }

  const MedOp *defOp(const MedVar &V) const {
    auto It = Defs.OpDef.find(keyOf(V));
    return It == Defs.OpDef.end()
               ? nullptr
               : &F.Blocks[It->second.first].Ops[It->second.second];
  }

  // Signed offset of a pointer from the incoming stack pointer, or nullopt.
  std::optional<int64_t> stackOffset(const MedVar &V, int Depth,
                                     llvm::DenseSet<ValueKey> &Seen) const {
    if (!In.StackRegsKnown || V.isConst() || Depth > 48)
      return std::nullopt;
    if (!Seen.insert(keyOf(V)).second)
      return std::nullopt;

    const MedOp *Op = defOp(V);
    if (V.Kind == MedVar::Reg && V.RegOff == In.StackPointerReg) {
      if (Op && (Op->Opcode == NdOp::INT_ADD || Op->Opcode == NdOp::INT_SUB))
        return affineOffset(*Op, Depth, Seen);
      return 0;
    }
    if (V.Kind == MedVar::Reg && V.RegOff == In.FramePointerReg) {
      if (Op && (Op->Opcode == NdOp::INT_ADD || Op->Opcode == NdOp::INT_SUB))
        return affineOffset(*Op, Depth, Seen);
      return std::nullopt;
    }
    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      return Op->NumInputs == 1 && Op->Output.Size > 0 &&
                     Op->Output.Size == Op->Inputs[0].Size
                 ? stackOffset(Op->Inputs[0], Depth + 1, Seen)
                 : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return affineOffset(*Op, Depth, Seen);
    default:
      return std::nullopt;
    }
  }

  std::optional<int64_t> affineOffset(const MedOp &Op, int Depth,
                                      llvm::DenseSet<ValueKey> &Seen) const {
    if (Op.NumInputs != 2 || Op.Output.Size == 0)
      return std::nullopt;
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (Op.Inputs[1].isConst()) {
      if (Op.Inputs[0].Size != Op.Output.Size ||
          Op.Inputs[1].Size > Op.Output.Size)
        return std::nullopt;
      auto Delta = detail::signedStackConstant(Op.Inputs[1]);
      if (auto Base = stackOffset(Op.Inputs[0], Depth + 1, Seen); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, Sub);
    } else if (Op.Inputs[0].isConst() && !Sub) {
      if (Op.Inputs[1].Size != Op.Output.Size ||
          Op.Inputs[0].Size > Op.Output.Size)
        return std::nullopt;
      auto Delta = detail::signedStackConstant(Op.Inputs[0]);
      if (auto Base = stackOffset(Op.Inputs[1], Depth + 1, Seen); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, false);
    }
    return std::nullopt;
  }

  bool mayBeStackAddressImpl(const MedVar &V, int Depth,
                             llvm::DenseMap<ValueKey, bool> &Cache,
                             llvm::DenseSet<ValueKey> &Active,
                             unsigned &Remaining, bool &Incomplete) const {
    if (!In.StackRegsKnown || V.isConst())
      return false;
    const ValueKey Key = keyOf(V);
    if (auto It = Cache.find(Key); It != Cache.end())
      return It->second;
    if (V.Kind == MedVar::Reg &&
        (V.RegOff == In.StackPointerReg || V.RegOff == In.FramePointerReg)) {
      Cache[Key] = true;
      return true;
    }
    if (Depth > 64 || Remaining == 0 || !Active.insert(Key).second) {
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
    if (auto It = Defs.PhiDef.find(Key); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        if (mayBeStackAddressImpl(Arg, Depth + 1, Cache, Active, Remaining,
                                  Incomplete)) {
          Result = true;
          break;
        }
      }
    } else if (const MedOp *Op = defOp(V)) {
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
        if (mayBeStackAddressImpl(Op->Inputs[I], Depth + 1, Cache, Active,
                                  Remaining, Incomplete)) {
          Result = true;
          break;
        }
    }
    if (!Incomplete)
      Cache[Key] = Result;
    return Result;
  }

  bool mayBeStackAddress(const MedVar &V) const {
    llvm::DenseMap<ValueKey, bool> Cache;
    llvm::DenseSet<ValueKey> Active;
    unsigned Remaining = MaxProofSteps;
    bool Incomplete = false;
    return mayBeStackAddressImpl(V, 0, Cache, Active, Remaining, Incomplete);
  }

  detail::ReachingStackValues reachingLoad(const MedBlock &B, int OpIdx,
                                           const MedOp &Op) const {
    if (Op.Opcode != NdOp::LOAD || Op.NumInputs == 0)
      return {};
    const MedVar &Addr = Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
    llvm::DenseSet<ValueKey> Seen;
    std::optional<int64_t> Off = stackOffset(Addr, 0, Seen);
    if (!Off)
      return {};
    auto Resolve = [&](const MedVar &V) {
      llvm::DenseSet<ValueKey> ResolveSeen;
      return stackOffset(V, 0, ResolveSeen);
    };
    auto MayBeFrame = [&](const MedVar &V) { return mayBeStackAddress(V); };
    auto AliasesWholeFrame = [&](const MedVar &V) {
      if (!In.StackRegsKnown)
        return false;
      auto FindOp = [&](const MedVar &Current) { return defOp(Current); };
      auto FindPhi = [&](const MedVar &Current) -> const PhiNode * {
        auto It = Defs.PhiDef.find(keyOf(Current));
        return It == Defs.PhiDef.end()
                   ? nullptr
                   : &F.Blocks[It->second.first].Phis[It->second.second];
      };
      return detail::aliasesWholeFrame(V, In.StackPointerReg,
                                       In.FramePointerReg, FindOp, FindPhi);
    };
    return detail::reachingStackValues(F, B.Id, OpIdx, *Off, Op.Output.Size,
                                       Resolve, MayBeFrame, AliasesWholeFrame);
  }

  const MedBlock *blockById(int BlockId) const {
    for (const MedBlock &B : F.Blocks)
      if (B.Id == BlockId)
        return &B;
    return nullptr;
  }

  bool sourceMayPrecede(const MedCallInfo &Source, int TargetBlockId,
                        int OpIdx) const {
    if (Source.BlockId == TargetBlockId && Source.OpIdx < OpIdx)
      return true;

    const MedBlock *SourceBlock = blockById(Source.BlockId);
    if (!SourceBlock)
      return false;
    llvm::SmallVector<int, 8> Worklist(SourceBlock->Succs.begin(),
                                       SourceBlock->Succs.end());
    llvm::DenseSet<int> Seen;
    while (!Worklist.empty()) {
      const int CurBlockId = Worklist.pop_back_val();
      if (CurBlockId == TargetBlockId)
        return true;
      if (!Seen.insert(CurBlockId).second)
        continue;
      const MedBlock *Block = blockById(CurBlockId);
      if (!Block)
        continue;
      Worklist.append(Block->Succs.begin(), Block->Succs.end());
    }
    return false;
  }

  bool sourceMayPrecedeSink(const MedCallInfo &Source,
                            const MedCallInfo &Sink) const {
    return sourceMayPrecede(Source, Sink.BlockId, Sink.OpIdx);
  }

  bool addIndexedOutput(std::vector<IndexedSourceOutput> &Outputs,
                        size_t CallInfoIndex, int ArgIndex,
                        llvm::StringRef Name) {
    if (CallInfoIndex >= F.CallInfos.size())
      return false;
    const MedCallInfo &CI = F.CallInfos[CallInfoIndex];
    if (ArgIndex < 0 || ArgIndex >= static_cast<int>(CI.Args.size()))
      return false;
    const MedVar &Value = CI.Args[ArgIndex];
    if (Value.isConst()) {
      const bool ExactDataAddress =
          Value.Provenance == ConstantAddressProvenance::DataAddress ||
          Value.Provenance == ConstantAddressProvenance::Address;
      const bool AuthenticatedZero =
          Value.ConstVal == 0 &&
          Value.Provenance == ConstantAddressProvenance::DataAddress &&
          Value.AddressOwnerVA != InvalidVA;
      if (!ExactDataAddress || (Value.ConstVal == 0 && !AuthenticatedZero))
        return false;
    }
    for (const IndexedSourceOutput &Output : Outputs)
      if (Output.CallInfoIndex == CallInfoIndex && Output.Value == Value)
        return false;
    Outputs.push_back({Value, CallInfoIndex, SinkCatalog::normalize(Name)});
    return true;
  }

  bool addIndexedSourceOutput(size_t CallInfoIndex, int ArgIndex,
                              llvm::StringRef Name) {
    return addIndexedOutput(IndexedSourceOutputs, CallInfoIndex, ArgIndex,
                            Name);
  }

  bool addIndexedPointeeOutput(size_t CallInfoIndex, int ArgIndex,
                               llvm::StringRef Name) {
    return addIndexedOutput(IndexedPointeeOutputs, CallInfoIndex, ArgIndex,
                            Name);
  }

  std::optional<uint64_t>
  forwardPointeeOffset(const MedVar &Base, const MedVar &Current, int Depth,
                       llvm::DenseSet<ValueKey> &Seen) const {
    if (Base.Size == 0 || Current.Size != Base.Size || Depth > 48)
      return std::nullopt;
    if (isExactValue(Base, Current))
      return uint64_t(0);
    if (Base.isConst() && Current.isConst()) {
      if (!isExactAddressProvenance(Base.Provenance) ||
          Base.Provenance != Current.Provenance ||
          Base.AddressOwnerVA == InvalidVA ||
          Base.AddressOwnerVA != Current.AddressOwnerVA ||
          Current.ConstVal < Base.ConstVal)
        return std::nullopt;
      return Current.ConstVal - Base.ConstVal;
    }
    if (Current.isConst() || !Seen.insert(keyOf(Current)).second)
      return std::nullopt;
    const MedOp *Op = defOp(Current);
    if (!Op || Op->Output.Size != Current.Size)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      return Op->NumInputs == 1 && Op->Inputs[0].Size == Current.Size
                 ? forwardPointeeOffset(Base, Op->Inputs[0], Depth + 1, Seen)
                 : std::nullopt;
    case NdOp::INT_ADD:
      if (Op->NumInputs != 2)
        return std::nullopt;
      for (unsigned I = 0; I < 2; ++I) {
        const MedVar &Delta = Op->Inputs[I];
        const MedVar &Other = Op->Inputs[1 - I];
        if (!isNumericConstant(Delta) || Delta.Size > Current.Size ||
            Other.Size != Current.Size)
          continue;
        std::optional<uint64_t> Offset =
            forwardPointeeOffset(Base, Other, Depth + 1, Seen);
        const uint64_t DeltaValue = numericConstantValue(Delta);
        if (Offset && *Offset <= UINT64_MAX - DeltaValue)
          return *Offset + DeltaValue;
      }
      return std::nullopt;
    default:
      return std::nullopt;
    }
  }

  std::optional<std::string> pointeeSourceBefore(const MedVar &Address,
                                                 int BlockId, int OpIdx) const {
    for (const IndexedSourceOutput &Output : IndexedPointeeOutputs) {
      llvm::DenseSet<ValueKey> Seen;
      if (!forwardPointeeOffset(Output.Value, Address, 0, Seen))
        continue;
      const MedCallInfo &Source = F.CallInfos[Output.CallInfoIndex];
      if (sourceMayPrecede(Source, BlockId, OpIdx))
        return Output.Name;
    }
    return std::nullopt;
  }

  void publishSourceOutputsBefore(const MedCallInfo &Point) {
    TaintedOutputValues.clear();
    TaintedOutputConstants.clear();
    for (const IndexedSourceOutput &Output : IndexedSourceOutputs) {
      const MedCallInfo &Source = F.CallInfos[Output.CallInfoIndex];
      if (!sourceMayPrecedeSink(Source, Point))
        continue;
      if (Output.Value.isConst())
        TaintedOutputConstants[constantSourceKey(Output.Value)] = Output.Name;
      else
        TaintedOutputValues[keyOf(Output.Value)] = Output.Name;
    }
  }

  bool valueIsTaintedBefore(const MedVar &V, const MedCallInfo &Point) {
    if (pointeeSourceBefore(V, Point.BlockId, Point.OpIdx))
      return true;
    llvm::DenseMap<ValueKey, std::string> Saved =
        std::move(TaintedOutputValues);
    std::map<ConstantSourceKey, std::string> SavedConstants =
        std::move(TaintedOutputConstants);
    publishSourceOutputsBefore(Point);
    resetClassificationProof();
    ArgClassification Classification;
    const bool Tainted = classify(V, 0, Classification) == ArgFlow::Tainted;
    resetClassificationProof();
    TaintedOutputValues = std::move(Saved);
    TaintedOutputConstants = std::move(SavedConstants);
    return Tainted;
  }

  void indexSourceOutputs(size_t SinkCallInfoIndex) {
    if (SinkCallInfoIndex >= F.CallInfos.size())
      return;

    for (size_t I = 0; I < F.CallInfos.size(); ++I) {
      const MedCallInfo &CI = F.CallInfos[I];
      const std::string Name = resolveCallName(In, CI);
      const SourceEntry *Source = Cat.matchSource(Name);
      if (Source && Source->OutArg >= 0) {
        addIndexedSourceOutput(I, Source->OutArg, Name);
        addIndexedPointeeOutput(I, Source->OutArg, Name);
      }
    }

    for (size_t I = 0; I < F.CallInfos.size(); ++I) {
      const MedCallInfo &CI = F.CallInfos[I];
      const std::string Name = resolveCallName(In, CI);
      if (!Cat.matchSource(Name))
        continue;
      std::optional<detail::FormattedSourceOutputs> Outputs =
          detail::recoverFormattedSourceOutputs(In.Img, Name, CI.Args);
      if (!Outputs ||
          Outputs->Kind != detail::FormattedSourceKind::ExternalInput)
        continue;
      for (const detail::FormattedOutput &Output : Outputs->UnboundedTextArgs)
        addIndexedSourceOutput(I, Output.ArgIndex, Name);
      for (const detail::FormattedOutput &Output : Outputs->UnboundedTextArgs)
        addIndexedPointeeOutput(I, Output.ArgIndex, Name);
      for (const detail::BoundedTextOutput &Output : Outputs->BoundedTextArgs) {
        addIndexedSourceOutput(I, Output.ArgIndex, Name);
        addIndexedPointeeOutput(I, Output.ArgIndex, Name);
      }
      for (const detail::FormattedOutput &Output : Outputs->ScalarArgs)
        addIndexedPointeeOutput(I, Output.ArgIndex, Name);
    }

    for (size_t Pass = 0; Pass <= F.CallInfos.size(); ++Pass) {
      bool Changed = false;
      for (size_t I = 0; I < F.CallInfos.size(); ++I) {
        const MedCallInfo &CI = F.CallInfos[I];
        const std::string Name = resolveCallName(In, CI);
        if (!Cat.matchSource(Name))
          continue;
        std::optional<detail::FormattedSourceOutputs> Outputs =
            detail::recoverFormattedSourceOutputs(In.Img, Name, CI.Args);
        if (!Outputs ||
            Outputs->Kind != detail::FormattedSourceKind::DerivedInput ||
            Outputs->InputArg < 0 ||
            Outputs->InputArg >= static_cast<int>(CI.Args.size()) ||
            !valueIsTaintedBefore(CI.Args[Outputs->InputArg], CI))
          continue;
        for (const detail::FormattedOutput &Output :
             Outputs->UnboundedTextArgs) {
          Changed |= addIndexedSourceOutput(I, Output.ArgIndex, Name);
          Changed |= addIndexedPointeeOutput(I, Output.ArgIndex, Name);
        }
        for (const detail::BoundedTextOutput &Output :
             Outputs->BoundedTextArgs) {
          Changed |= addIndexedSourceOutput(I, Output.ArgIndex, Name);
          Changed |= addIndexedPointeeOutput(I, Output.ArgIndex, Name);
        }
        for (const detail::FormattedOutput &Output : Outputs->ScalarArgs)
          Changed |= addIndexedPointeeOutput(I, Output.ArgIndex, Name);
      }
      if (!Changed)
        break;
    }

    publishSourceOutputsBefore(F.CallInfos[SinkCallInfoIndex]);
  }

  std::optional<uint64_t> clampBound(const MedOp &Op) const {
    if (Op.Opcode != NdOp::SELECT || Op.NumInputs != 3 || Op.Output.Size == 0)
      return std::nullopt;
    const MedVar &Then = Op.Inputs[1];
    const MedVar &Else = Op.Inputs[2];
    if (!((isNumericConstant(Then) && !Else.isConst()) ||
          (isNumericConstant(Else) && !Then.isConst())))
      return std::nullopt;
    const MedVar &Bound = Then.isConst() ? Then : Else;
    const MedVar &Other = Then.isConst() ? Else : Then;
    if (Bound.Size != Other.Size || Other.Size != Op.Output.Size)
      return std::nullopt;
    const MedOp *Cond = defOp(Op.Inputs[0]);
    if (!Cond ||
        (Cond->Opcode != NdOp::INT_LESS &&
         Cond->Opcode != NdOp::INT_LESSEQUAL) ||
        Cond->NumInputs != 2)
      return std::nullopt;
    auto isOther = [&](const MedVar &V) { return isExactValue(V, Other); };
    auto isBound = [&](const MedVar &V) {
      return isNumericConstant(V) && isExactValue(V, Bound);
    };
    const bool OtherBelowBound =
        isOther(Cond->Inputs[0]) && isBound(Cond->Inputs[1]);
    const bool BoundBelowOther =
        isBound(Cond->Inputs[0]) && isOther(Cond->Inputs[1]);
    if ((OtherBelowBound && isOther(Then) && isBound(Else)) ||
        (BoundBelowOther && isBound(Then) && isOther(Else)))
      return numericConstantValue(Bound);
    return std::nullopt;
  }

  std::optional<uint64_t> upperBound(const MedVar &V, int Depth) {
    if (V.isConst())
      return isNumericConstant(V)
                 ? std::optional<uint64_t>(numericConstantValue(V))
                 : std::nullopt;

    const ValueKey Key = keyOf(V);
    if (auto It = UpperBoundCache.find(Key); It != UpperBoundCache.end())
      return It->second;
    if (Depth > 64 || UpperBoundProofBudget == 0 ||
        !UpperBoundActive.insert(Key).second) {
      UpperBoundProofIncomplete = true;
      return std::nullopt;
    }
    struct PopActive {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~PopActive() { Set.erase(Key); }
    } Guard{UpperBoundActive, Key};
    --UpperBoundProofBudget;

    auto finish = [&](std::optional<uint64_t> Result) {
      if (!UpperBoundProofIncomplete)
        UpperBoundCache[Key] = Result;
      return Result;
    };

    if (auto It = Defs.PhiDef.find(Key); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      std::optional<uint64_t> Max;
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        auto B = upperBound(Arg, Depth + 1);
        if (!B)
          return finish(std::nullopt);
        Max = Max ? std::max(*Max, *B) : *B;
      }
      return finish(Max);
    }

    const MedOp *Op = defOp(V);
    if (!Op)
      return finish(std::nullopt);
    std::optional<uint64_t> Result;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      Result = Op->NumInputs == 1 && Op->Inputs[0].Size == Op->Output.Size
                   ? upperBound(Op->Inputs[0], Depth + 1)
                   : std::nullopt;
      break;
    case NdOp::INT_ZEXT:
      Result = Op->NumInputs == 1 && Op->Inputs[0].Size <= Op->Output.Size
                   ? upperBound(Op->Inputs[0], Depth + 1)
                   : std::nullopt;
      break;
    case NdOp::SUBBYTES:
      Result = Op->NumInputs == 1 && Op->Output.Size > 0 &&
                       Op->Output.Size <= Op->Inputs[0].Size
                   ? upperBound(Op->Inputs[0], Depth + 1)
                   : std::nullopt;
      break;
    case NdOp::INT_AND:
      if (Op->NumInputs != 2 || Op->Output.Size == 0)
        break;
      for (unsigned I = 0; I < Op->NumInputs; ++I)
        if (isNumericConstant(Op->Inputs[I]) &&
            Op->Inputs[I].Size == Op->Output.Size &&
            Op->Inputs[1 - I].Size == Op->Output.Size) {
          Result = numericConstantValue(Op->Inputs[I]);
          break;
        }
      break;
    case NdOp::SELECT:
      if (auto Bound = clampBound(*Op)) {
        Result = Bound;
      } else if (Op->NumInputs >= 3) {
        auto Then = upperBound(Op->Inputs[1], Depth + 1);
        auto Else = upperBound(Op->Inputs[2], Depth + 1);
        if (Then && Else)
          Result = std::max(*Then, *Else);
      }
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_MULT: {
      if (Op->NumInputs < 2)
        break;
      auto A = upperBound(Op->Inputs[0], Depth + 1);
      auto B = upperBound(Op->Inputs[1], Depth + 1);
      if (!A || !B)
        break;
      if (Op->Opcode == NdOp::INT_ADD) {
        if (*A > std::numeric_limits<uint64_t>::max() - *B)
          break;
        Result = *A + *B;
        break;
      }
      if (*A != 0 && *B > std::numeric_limits<uint64_t>::max() / *A)
        break;
      Result = *A * *B;
      break;
    }
    case NdOp::CALL:
    case NdOp::INDIR_CALL: {
      auto It = Defs.OpDef.find(Key);
      if (It == Defs.OpDef.end())
        break;
      const MedBlock &B = F.Blocks[It->second.first];
      const MedCallInfo *CI = F.findCall(B.Id, It->second.second);
      if (!CI)
        break;
      const std::string Name = resolveCallName(In, *CI);
      if (!isCappedLengthReturn(Name) || CI->Args.size() < 2 ||
          !isNumericConstant(CI->Args[1]) ||
          CI->Args[1].Size != Op->Output.Size)
        break;
      Result = numericConstantValue(CI->Args[1]);
      break;
    }
    case NdOp::LOAD: {
      auto It = Defs.OpDef.find(Key);
      if (It == Defs.OpDef.end())
        break;
      const MedBlock &B = F.Blocks[It->second.first];
      detail::ReachingStackValues Reaching =
          reachingLoad(B, It->second.second, *Op);
      if (!Reaching.Complete)
        break;
      std::optional<uint64_t> Max;
      for (const MedVar &Value : Reaching.Values) {
        auto Bound = upperBound(Value, Depth + 1);
        if (!Bound)
          return finish(std::nullopt);
        Max = Max ? std::max(*Max, *Bound) : *Bound;
      }
      Result = Max;
      break;
    }
    default:
      break;
    }
    return finish(Result);
  }

  // Merge sub-results: taint must win (the sink must be explored), a lone
  // bounded set stays bounded, anything else is unknown.
  static ArgFlow merge(ArgFlow A, ArgFlow B) {
    if (A == ArgFlow::Tainted || B == ArgFlow::Tainted)
      return ArgFlow::Tainted;
    if (A == ArgFlow::Bounded && B == ArgFlow::Bounded)
      return ArgFlow::Bounded;
    return ArgFlow::Unknown;
  }

  ArgFlow classify(const MedVar &V, int Depth, ArgClassification &Top) {
    if (V.isConst()) {
      if (auto It = TaintedOutputConstants.find(constantSourceKey(V));
          It != TaintedOutputConstants.end()) {
        if (Top.TaintSource.empty()) {
          Top.TaintSource = It->second;
          Top.Reason = "reaches external input " + Top.TaintSource;
        }
        return ArgFlow::Tainted;
      }
      if (!isNumericConstant(V))
        return ArgFlow::Unknown;
      if (Depth == 0) {
        Top.ConstValue = numericConstantValue(V);
        Top.Reason = "constant argument";
      }
      return ArgFlow::Bounded;
    }
    if (auto It = TaintedOutputValues.find(keyOf(V));
        It != TaintedOutputValues.end()) {
      if (Top.TaintSource.empty()) {
        Top.TaintSource = It->second;
        Top.Reason = "reaches external input " + Top.TaintSource;
      }
      return ArgFlow::Tainted;
    }

    ValueKey K = keyOf(V);
    if (auto It = ClassificationCache.find(K); It != ClassificationCache.end())
      return It->second;
    if (Depth > 64 || ClassificationProofBudget == 0 ||
        !ClassificationActive.insert(K).second) {
      ClassificationProofIncomplete = true;
      return ArgFlow::Unknown;
    }
    struct PopActive {
      llvm::DenseSet<ValueKey> &S;
      ValueKey K;
      ~PopActive() { S.erase(K); }
    } Guard{ClassificationActive, K};
    --ClassificationProofBudget;

    auto finish = [&](ArgFlow Result) {
      if (!ClassificationProofIncomplete)
        ClassificationCache[K] = Result;
      return Result;
    };

    if (V.Kind == MedVar::Param) {
      if (isMainLike(F.Name)) {
        if (Top.TaintSource.empty()) {
          Top.TaintSource = "argv";
          Top.Reason = "reaches program arguments";
        }
        return finish(ArgFlow::Tainted);
      }
      return finish(ArgFlow::Unknown);
    }

    if (auto It = Defs.PhiDef.find(K); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      if (Phi.Args.empty())
        return finish(ArgFlow::Unknown);
      ArgFlow Acc = ArgFlow::Bounded;
      bool First = true;
      for (const auto &[Pred, Val] : Phi.Args) {
        ArgFlow Sub = classify(Val, Depth + 1, Top);
        Acc = First ? Sub : merge(Acc, Sub);
        First = false;
      }
      return finish(Acc);
    }

    auto It = Defs.OpDef.find(K);
    if (It == Defs.OpDef.end())
      return finish(ArgFlow::Unknown);

    const MedBlock &B = F.Blocks[It->second.first];
    const MedOp &Op = B.Ops[It->second.second];
    return finish(classifyOp(B, It->second.second, Op, Depth, Top));
  }

  ArgFlow classifyOp(const MedBlock &B, int OpIdx, const MedOp &Op, int Depth,
                     ArgClassification &Top) {
    switch (Op.Opcode) {
    case NdOp::CALL:
    case NdOp::INDIR_CALL: {
      const MedCallInfo *CI = F.findCall(B.Id, OpIdx);
      const std::string ResolvedName = CI ? resolveCallName(In, *CI) : "";
      llvm::StringRef Name = ResolvedName;
      if (CI && isCappedLengthReturn(Name) && CI->Args.size() >= 2 &&
          isNumericConstant(CI->Args[1]) &&
          CI->Args[1].Size == Op.Output.Size) {
        if (Top.Reason.empty())
          Top.Reason = ("bounded by " + stripLeadingUnderscores(Name)).str();
        return ArgFlow::Bounded;
      }
      if (CI && isLengthReturn(Name)) {
        if (!CI->Args.empty()) {
          ArgFlow Source = classify(CI->Args[0], Depth + 1, Top);
          return Source == ArgFlow::Tainted ? ArgFlow::Tainted
                                            : ArgFlow::Unknown;
        }
        return ArgFlow::Unknown;
      }
      const SourceEntry *Source = CI ? Cat.matchSource(Name) : nullptr;
      if (Source && Source->returnCarriesInput()) {
        if (Top.TaintSource.empty()) {
          Top.TaintSource = stripLeadingUnderscores(Name).str();
          Top.Reason = "reaches external input " + Top.TaintSource;
        }
        return ArgFlow::Tainted;
      }
      return ArgFlow::Unknown; // an unmodelled call result is not assumed safe.
    }

    // Pure forwarding: the value is a re-typed copy of a single input.
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::CAST:
      return Op.NumInputs >= 1 ? classify(Op.Inputs[0], Depth + 1, Top)
                               : ArgFlow::Unknown;

    case NdOp::SUBBYTES:
      // A low-lane extraction forwards its source; a shifted lane does not
      // change whether the value is bounded.
      return Op.NumInputs >= 1 ? classify(Op.Inputs[0], Depth + 1, Top)
                               : ArgFlow::Unknown;

    // A mask by a constant bounds the result regardless of the other operand.
    case NdOp::INT_AND:
      if (Op.NumInputs != 2 || Op.Output.Size == 0)
        return ArgFlow::Unknown;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (isNumericConstant(Op.Inputs[I]) &&
            Op.Inputs[I].Size == Op.Output.Size &&
            Op.Inputs[1 - I].Size == Op.Output.Size) {
          if (Top.Reason.empty())
            Top.Reason = "masked to a constant range";
          return ArgFlow::Bounded;
        }
      return combineInputs(Op, Depth, Top);

    // Arithmetic and selection preserve boundedness of their operands.
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_DIV:
    case NdOp::INT_SDIV:
    case NdOp::CONCAT:
      return combineInputs(Op, Depth, Top);

    case NdOp::SELECT:
      if (auto Bound = clampBound(Op)) {
        if (Top.Reason.empty())
          Top.Reason = "clamped to a constant";
        return ArgFlow::Bounded;
      }
      if (Op.NumInputs >= 3) {
        const MedVar &Then = Op.Inputs[1];
        const MedVar &Else = Op.Inputs[2];
        return merge(classify(Then, Depth + 1, Top),
                     classify(Else, Depth + 1, Top));
      }
      return ArgFlow::Unknown;

    case NdOp::LOAD: {
      if (Op.NumInputs > 0) {
        const MedVar &Addr = Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
        if (std::optional<std::string> Source =
                pointeeSourceBefore(Addr, B.Id, OpIdx)) {
          if (Top.TaintSource.empty()) {
            Top.TaintSource = *Source;
            Top.Reason = "loads external input from " + *Source;
          }
          return ArgFlow::Tainted;
        }
      }
      detail::ReachingStackValues Reaching = reachingLoad(B, OpIdx, Op);
      if (!Reaching.Complete)
        return ArgFlow::Unknown;
      ArgFlow Acc = ArgFlow::Bounded;
      bool First = true;
      for (const MedVar &Val : Reaching.Values) {
        ArgFlow Sub = classify(Val, Depth + 1, Top);
        Acc = First ? Sub : merge(Acc, Sub);
        First = false;
      }
      return Acc;
    }
    default:
      return ArgFlow::Unknown; // an unmodelled op stays unknown.
    }
  }

  ArgFlow combineInputs(const MedOp &Op, int Depth, ArgClassification &Top) {
    if (Op.NumInputs == 0)
      return ArgFlow::Unknown;
    ArgFlow Acc = ArgFlow::Bounded;
    bool First = true;
    for (unsigned I = 0; I < Op.NumInputs; ++I) {
      ArgFlow Sub = classify(Op.Inputs[I], Depth + 1, Top);
      Acc = First ? Sub : merge(Acc, Sub);
      First = false;
    }
    return Acc;
  }
};

} // namespace

ArgClassification neverd::safety::classifyArgument(const AnalysisInput &In,
                                                   const SinkCatalog &Cat,
                                                   const MedFunc &F,
                                                   size_t CallInfoIndex,
                                                   int ArgIndex) {
  ArgClassification R;
  if (CallInfoIndex >= F.CallInfos.size())
    return R;
  const MedCallInfo &CI = F.CallInfos[CallInfoIndex];
  if (ArgIndex < 0 || ArgIndex >= static_cast<int>(CI.Args.size())) {
    R.Reason = "argument not recovered";
    return R; // fail closed: an unrecovered argument stays UNKNOWN.
  }
  Slicer S(In, Cat, F, CallInfoIndex);
  return S.run(CI.Args[ArgIndex]);
}
