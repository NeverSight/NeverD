//===- ArgSlicer.cpp - Classify a sink argument by its provenance ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ArgSlicer.h"

#include "StackSlotFlow.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/safety/SinkScanner.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <algorithm>
#include <limits>
#include <tuple>

using namespace neverd;
using namespace neverd::safety;

namespace {

using ValueKey = std::tuple<uint8_t, int, int>;

ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
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

bool sourceReturnCarriesInput(llvm::StringRef Name) {
  const std::string Normalized = SinkCatalog::normalize(Name);
  return llvm::StringSwitch<bool>(Normalized)
      .Cases({"getenv", "secure_getenv", "read", "pread"}, true)
      .Cases({"recv", "recvfrom", "fgets", "fread", "gets"}, true)
      .Cases({"GetCommandLineA", "GetCommandLineW"}, true)
      .Cases({"GetEnvironmentVariableA", "GetEnvironmentVariableW"}, true)
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
      : In(In), Cat(Cat), F(F), Defs(F) {
    indexSourceOutputs(SinkCallInfoIndex);
  }

  ArgClassification run(const MedVar &Arg) {
    ArgClassification R;
    R.Flow = classify(Arg, 0, R);
    llvm::DenseSet<ValueKey> Seen;
    R.UpperBound = upperBound(Arg, 0, Seen);
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
  llvm::DenseSet<ValueKey> Active; ///< guards against SSA cycles.
  llvm::DenseMap<ValueKey, std::string> TaintedOutputValues;

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
      return Op->NumInputs >= 1 ? stackOffset(Op->Inputs[0], Depth + 1, Seen)
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
    if (Op.NumInputs < 2)
      return std::nullopt;
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (Op.Inputs[1].isConst()) {
      auto Delta = detail::signedStackConstant(Op.Inputs[1]);
      if (auto Base = stackOffset(Op.Inputs[0], Depth + 1, Seen); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, Sub);
    } else if (Op.Inputs[0].isConst() && !Sub) {
      auto Delta = detail::signedStackConstant(Op.Inputs[0]);
      if (auto Base = stackOffset(Op.Inputs[1], Depth + 1, Seen); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, false);
    }
    return std::nullopt;
  }

  bool mayBeStackAddress(const MedVar &V, int Depth,
                         llvm::DenseSet<ValueKey> &Seen) const {
    if (!In.StackRegsKnown || V.isConst() || Depth > 64 ||
        !Seen.insert(keyOf(V)).second)
      return false;
    if (V.Kind == MedVar::Reg &&
        (V.RegOff == In.StackPointerReg || V.RegOff == In.FramePointerReg))
      return true;
    if (auto It = Defs.PhiDef.find(keyOf(V)); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        llvm::DenseSet<ValueKey> BranchSeen = Seen;
        if (mayBeStackAddress(Arg, Depth + 1, BranchSeen))
          return true;
      }
      return false;
    }
    const MedOp *Op = defOp(V);
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
    auto MayBeFrame = [&](const MedVar &V) {
      llvm::DenseSet<ValueKey> FrameSeen;
      return mayBeStackAddress(V, 0, FrameSeen);
    };
    return detail::reachingStackValues(F, B.Id, OpIdx, *Off, Op.Output.Size,
                                       Resolve, MayBeFrame);
  }

  const MedBlock *blockById(int BlockId) const {
    for (const MedBlock &B : F.Blocks)
      if (B.Id == BlockId)
        return &B;
    return nullptr;
  }

  bool sourceMayPrecedeSink(const MedCallInfo &Source,
                            const MedCallInfo &Sink) const {
    if (Source.BlockId == Sink.BlockId && Source.OpIdx < Sink.OpIdx)
      return true;

    const MedBlock *SourceBlock = blockById(Source.BlockId);
    if (!SourceBlock)
      return false;
    llvm::SmallVector<int, 8> Worklist(SourceBlock->Succs.begin(),
                                       SourceBlock->Succs.end());
    llvm::DenseSet<int> Seen;
    while (!Worklist.empty()) {
      const int BlockId = Worklist.pop_back_val();
      if (BlockId == Sink.BlockId)
        return true;
      if (!Seen.insert(BlockId).second)
        continue;
      const MedBlock *Block = blockById(BlockId);
      if (!Block)
        continue;
      Worklist.append(Block->Succs.begin(), Block->Succs.end());
    }
    return false;
  }

  void indexSourceOutputs(size_t SinkCallInfoIndex) {
    if (SinkCallInfoIndex >= F.CallInfos.size())
      return;
    const MedCallInfo &Sink = F.CallInfos[SinkCallInfoIndex];
    for (const MedCallInfo &CI : F.CallInfos) {
      if (!sourceMayPrecedeSink(CI, Sink))
        continue;
      const std::string Name = resolveCallName(In, CI);
      const SourceEntry *Source = Cat.matchSource(Name);
      if (!Source || Source->OutArg < 0 ||
          Source->OutArg >= static_cast<int>(CI.Args.size()))
        continue;
      const MedVar &Out = CI.Args[Source->OutArg];
      if (!Out.isConst())
        TaintedOutputValues[keyOf(Out)] = SinkCatalog::normalize(Name);
    }
  }

  std::optional<uint64_t> clampBound(const MedOp &Op) const {
    if (Op.Opcode != NdOp::SELECT || Op.NumInputs < 3)
      return std::nullopt;
    const MedVar &Then = Op.Inputs[1];
    const MedVar &Else = Op.Inputs[2];
    if (!((Then.isConst() && !Else.isConst()) ||
          (Else.isConst() && !Then.isConst())))
      return std::nullopt;
    const MedVar &Bound = Then.isConst() ? Then : Else;
    const MedVar &Other = Then.isConst() ? Else : Then;
    const MedOp *Cond = defOp(Op.Inputs[0]);
    if (!Cond ||
        (Cond->Opcode != NdOp::INT_LESS &&
         Cond->Opcode != NdOp::INT_LESSEQUAL) ||
        Cond->NumInputs < 2)
      return std::nullopt;
    auto isOther = [&](const MedVar &V) {
      return V.Kind == Other.Kind && V.Id == Other.Id &&
             V.SSAVer == Other.SSAVer;
    };
    auto isBound = [&](const MedVar &V) {
      return V.isConst() && V.ConstVal == Bound.ConstVal;
    };
    const bool OtherBelowBound =
        isOther(Cond->Inputs[0]) && isBound(Cond->Inputs[1]);
    const bool BoundBelowOther =
        isBound(Cond->Inputs[0]) && isOther(Cond->Inputs[1]);
    if ((OtherBelowBound && isOther(Then) && isBound(Else)) ||
        (BoundBelowOther && isBound(Then) && isOther(Else)))
      return Bound.ConstVal;
    return std::nullopt;
  }

  std::optional<uint64_t> upperBound(const MedVar &V, int Depth,
                                     llvm::DenseSet<ValueKey> &Seen) const {
    if (V.isConst())
      return V.ConstVal;
    if (Depth > 64 || !Seen.insert(keyOf(V)).second)
      return std::nullopt;

    if (auto It = Defs.PhiDef.find(keyOf(V)); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      std::optional<uint64_t> Max;
      for (const auto &[Pred, Arg] : Phi.Args) {
        llvm::DenseSet<ValueKey> BranchSeen = Seen;
        auto B = upperBound(Arg, Depth + 1, BranchSeen);
        if (!B)
          return std::nullopt;
        Max = Max ? std::max(*Max, *B) : *B;
      }
      return Max;
    }

    const MedOp *Op = defOp(V);
    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::CAST:
    case NdOp::SUBBYTES:
      return Op->NumInputs >= 1 ? upperBound(Op->Inputs[0], Depth + 1, Seen)
                                : std::nullopt;
    case NdOp::INT_AND:
      for (unsigned I = 0; I < Op->NumInputs; ++I)
        if (Op->Inputs[I].isConst())
          return Op->Inputs[I].ConstVal;
      return std::nullopt;
    case NdOp::SELECT:
      if (auto Bound = clampBound(*Op))
        return Bound;
      if (Op->NumInputs >= 3) {
        llvm::DenseSet<ValueKey> ThenSeen = Seen;
        llvm::DenseSet<ValueKey> ElseSeen = Seen;
        auto Then = upperBound(Op->Inputs[1], Depth + 1, ThenSeen);
        auto Else = upperBound(Op->Inputs[2], Depth + 1, ElseSeen);
        if (Then && Else)
          return std::max(*Then, *Else);
      }
      return std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_MULT: {
      if (Op->NumInputs < 2)
        return std::nullopt;
      llvm::DenseSet<ValueKey> ASeen = Seen;
      llvm::DenseSet<ValueKey> BSeen = Seen;
      auto A = upperBound(Op->Inputs[0], Depth + 1, ASeen);
      auto B = upperBound(Op->Inputs[1], Depth + 1, BSeen);
      if (!A || !B)
        return std::nullopt;
      if (Op->Opcode == NdOp::INT_ADD) {
        if (*A > std::numeric_limits<uint64_t>::max() - *B)
          return std::nullopt;
        return *A + *B;
      }
      if (*A != 0 && *B > std::numeric_limits<uint64_t>::max() / *A)
        return std::nullopt;
      return *A * *B;
    }
    case NdOp::CALL:
    case NdOp::INDIR_CALL: {
      auto It = Defs.OpDef.find(keyOf(V));
      if (It == Defs.OpDef.end())
        return std::nullopt;
      const MedBlock &B = F.Blocks[It->second.first];
      const MedCallInfo *CI = F.findCall(B.Id, It->second.second);
      if (!CI)
        return std::nullopt;
      const std::string Name = resolveCallName(In, *CI);
      if (!isCappedLengthReturn(Name) || CI->Args.size() < 2 ||
          !CI->Args[1].isConst())
        return std::nullopt;
      return CI->Args[1].ConstVal;
    }
    case NdOp::LOAD: {
      auto It = Defs.OpDef.find(keyOf(V));
      if (It == Defs.OpDef.end())
        return std::nullopt;
      const MedBlock &B = F.Blocks[It->second.first];
      detail::ReachingStackValues Reaching =
          reachingLoad(B, It->second.second, *Op);
      if (!Reaching.Complete)
        return std::nullopt;
      std::optional<uint64_t> Max;
      for (const MedVar &Value : Reaching.Values) {
        llvm::DenseSet<ValueKey> BranchSeen = Seen;
        auto Bound = upperBound(Value, Depth + 1, BranchSeen);
        if (!Bound)
          return std::nullopt;
        Max = Max ? std::max(*Max, *Bound) : *Bound;
      }
      return Max;
    }
    default:
      return std::nullopt;
    }
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
      if (Depth == 0) {
        Top.ConstValue = V.ConstVal;
        Top.Reason = "constant argument";
      }
      return ArgFlow::Bounded;
    }
    if (Depth > 64)
      return ArgFlow::Unknown;

    if (auto It = TaintedOutputValues.find(keyOf(V));
        It != TaintedOutputValues.end()) {
      if (Top.TaintSource.empty()) {
        Top.TaintSource = It->second;
        Top.Reason = "reaches external input " + Top.TaintSource;
      }
      return ArgFlow::Tainted;
    }

    ValueKey K = keyOf(V);
    if (!Active.insert(K).second)
      return ArgFlow::Unknown; // already on the current path (a loop phi).
    struct Pop {
      llvm::DenseSet<ValueKey> &S;
      ValueKey K;
      ~Pop() { S.erase(K); }
    } Guard{Active, K};

    if (V.Kind == MedVar::Param) {
      if (isMainLike(F.Name)) {
        if (Top.TaintSource.empty()) {
          Top.TaintSource = "argv";
          Top.Reason = "reaches program arguments";
        }
        return ArgFlow::Tainted;
      }
      return ArgFlow::Unknown; // an unconstrained parameter is never assumed
                               // safe.
    }

    if (auto It = Defs.PhiDef.find(K); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      if (Phi.Args.empty())
        return ArgFlow::Unknown;
      ArgFlow Acc = ArgFlow::Bounded;
      bool First = true;
      for (const auto &[Pred, Val] : Phi.Args) {
        ArgFlow Sub = classify(Val, Depth + 1, Top);
        Acc = First ? Sub : merge(Acc, Sub);
        First = false;
      }
      return Acc;
    }

    auto It = Defs.OpDef.find(K);
    if (It == Defs.OpDef.end())
      return ArgFlow::Unknown; // no definition in this function ->
                               // conservative.

    const MedBlock &B = F.Blocks[It->second.first];
    const MedOp &Op = B.Ops[It->second.second];
    return classifyOp(B, It->second.second, Op, Depth, Top);
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
          CI->Args[1].isConst()) {
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
      if (CI && Cat.matchSource(Name) && sourceReturnCarriesInput(Name)) {
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
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst()) {
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
