//===- ArgSlicer.cpp - Classify a sink argument by its provenance ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ArgSlicer.h"

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"

#include <tuple>

using namespace neverd;
using namespace neverd::safety;

namespace {

using ValueKey = std::tuple<uint8_t, int, int>;

ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

// A call whose return value is a length no larger than an existing object, so a
// copy bounded by it does not depend on attacker-controlled size.
bool isBoundedReturn(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define SAFETY_CALL_TRAIT(NAME, IS_LENGTH, IS_BOUNDED)                         \
  .Case(NAME, IS_BOUNDED != 0)
#include "neverd/safety/SafetyCallTraits.inc"
#undef SAFETY_CALL_TRAIT
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
  Slicer(const AnalysisInput &In, const SinkCatalog &Cat, const MedFunc &F)
      : In(In), Cat(Cat), F(F), Defs(F) {
    indexStackStores();
  }

  ArgClassification run(const MedVar &Arg) {
    ArgClassification R;
    R.Flow = classify(Arg, 0, R);
    return R;
  }

private:
  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const MedFunc &F;
  DefIndex Defs;
  llvm::DenseSet<ValueKey> Active; ///< guards against SSA cycles.
  llvm::DenseSet<int64_t> ActiveSlots; ///< guards store/load slot cycles.
  /// Values written to each stack slot, so a spill/reload chain preserves
  /// provenance across an unoptimised binary's memory traffic.
  llvm::DenseMap<int64_t, llvm::SmallVector<MedVar, 2>> StoredByOffset;

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
      if (auto Base = stackOffset(Op.Inputs[0], Depth + 1, Seen))
        return Sub ? *Base - static_cast<int64_t>(Op.Inputs[1].ConstVal)
                   : *Base + static_cast<int64_t>(Op.Inputs[1].ConstVal);
    } else if (Op.Inputs[0].isConst() && !Sub) {
      if (auto Base = stackOffset(Op.Inputs[1], Depth + 1, Seen))
        return *Base + static_cast<int64_t>(Op.Inputs[0].ConstVal);
    }
    return std::nullopt;
  }

  void indexStackStores() {
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops) {
        if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
          continue;
        const MedVar &Addr = Op.Inputs[Op.NumInputs >= 3 ? 1 : 0];
        const MedVar &Val = Op.Inputs[Op.NumInputs >= 3 ? 2 : 1];
        llvm::DenseSet<ValueKey> Seen;
        if (auto Off = stackOffset(Addr, 0, Seen))
          StoredByOffset[*Off].push_back(Val);
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
      return ArgFlow::Unknown; // an unconstrained parameter is never assumed safe.
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
      return ArgFlow::Unknown; // no definition in this function -> conservative.

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
      llvm::StringRef Name = CI ? llvm::StringRef(CI->TargetName) : "";
      if (!Name.empty() && isBoundedReturn(Name)) {
        if (Top.Reason.empty())
          Top.Reason = ("bounded by " + stripLeadingUnderscores(Name)).str();
        return ArgFlow::Bounded;
      }
      if (CI && Cat.matchSource(CI->TargetName)) {
        if (Top.TaintSource.empty()) {
          Top.TaintSource = stripLeadingUnderscores(CI->TargetName).str();
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
      if (Op.NumInputs >= 3) {
        const MedVar &Then = Op.Inputs[1];
        const MedVar &Else = Op.Inputs[2];
        if ((Then.isConst() && !Else.isConst()) ||
            (Else.isConst() && !Then.isConst())) {
          const MedVar &Bound = Then.isConst() ? Then : Else;
          const MedVar &Other = Then.isConst() ? Else : Then;
          const MedOp *Cond = defOp(Op.Inputs[0]);
          if (Cond && (Cond->Opcode == NdOp::INT_LESS ||
                       Cond->Opcode == NdOp::INT_LESSEQUAL ||
                       Cond->Opcode == NdOp::INT_SLESS ||
                       Cond->Opcode == NdOp::INT_SLESSEQUAL) &&
              Cond->NumInputs >= 2) {
            auto isOther = [&](const MedVar &V) {
              return V.Kind == Other.Kind && V.Id == Other.Id &&
                     V.SSAVer == Other.SSAVer;
            };
            auto isBound = [&](const MedVar &V) {
              return V.isConst() && V.ConstVal == Bound.ConstVal;
            };
            if ((isOther(Cond->Inputs[0]) && isBound(Cond->Inputs[1])) ||
                (isOther(Cond->Inputs[1]) && isBound(Cond->Inputs[0]))) {
              if (Top.Reason.empty())
                Top.Reason = "clamped to a constant";
              return ArgFlow::Bounded;
            }
          }
        }
        return merge(classify(Then, Depth + 1, Top),
                     classify(Else, Depth + 1, Top));
      }
      return ArgFlow::Unknown;

    case NdOp::LOAD: {
      const MedVar &Addr = Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
      if (Addr.isConst()) {
        if (Top.Reason.empty())
          Top.Reason = "load from a constant address";
        return ArgFlow::Bounded;
      }
      llvm::DenseSet<ValueKey> Seen;
      auto Off = stackOffset(Addr, 0, Seen);
      if (!Off)
        return ArgFlow::Unknown;
      auto It = StoredByOffset.find(*Off);
      if (It == StoredByOffset.end() || It->second.empty())
        return ArgFlow::Unknown;
      if (!ActiveSlots.insert(*Off).second)
        return ArgFlow::Unknown;
      struct Pop {
        llvm::DenseSet<int64_t> &S;
        int64_t K;
        ~Pop() { S.erase(K); }
      } Guard{ActiveSlots, *Off};
      ArgFlow Acc = ArgFlow::Bounded;
      bool First = true;
      for (const MedVar &Val : It->second) {
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
  Slicer S(In, Cat, F);
  return S.run(CI.Args[ArgIndex]);
}
