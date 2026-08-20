//===- ObjectModel.cpp - Destination capacity and heap-object sizing ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ObjectModel.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <tuple>

using namespace neverd;
using namespace neverd::safety;

namespace {

using ValueKey = std::tuple<uint8_t, int, int>;

ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

uint64_t byteSize(const TypeRef &T) {
  if (!T)
    return 0;
  switch (T->Kind) {
  case NdTypeKind::Array:
    return T->ElemType ? byteSize(T->ElemType) * T->ArrayCount : 0;
  default:
    return T->Size;
  }
}

struct DefIndex {
  llvm::DenseMap<ValueKey, std::pair<int, int>> OpDef;

  explicit DefIndex(const MedFunc &F) {
    for (int Bi = 0; Bi < static_cast<int>(F.Blocks.size()); ++Bi) {
      const MedBlock &B = F.Blocks[Bi];
      for (int Oi = 0; Oi < static_cast<int>(B.Ops.size()); ++Oi) {
        const MedOp &O = B.Ops[Oi];
        if (!O.Output.isConst() && O.Output.Size > 0)
          OpDef[keyOf(O.Output)] = {Bi, Oi};
      }
    }
  }
};

class Resolver {
public:
  Resolver(const AnalysisInput &In, const SinkCatalog &Cat, const MedFunc &F)
      : In(In), Cat(Cat), F(F), Defs(F) {}

  DestObject resolve(const MedVar &Dst) {
    DestObject R;

    // A heap allocation with a known size gives an exact capacity.
    if (auto Cap = allocCapacity(Dst, 0)) {
      R.Region = ObjectRegion::Heap;
      R.Capacity = *Cap;
      R.Detail = "allocation site";
      return R;
    }

    // A stack-relative destination: prefer a debug-declared array size, then a
    // sound frame upper bound.
    Active.clear();
    if (auto Off = stackOffset(Dst, 0)) {
      R.Region = ObjectRegion::Stack;
      R.StackOffset = *Off;
    if (In.Dbg) {
      if (auto Var = In.Dbg->resolveVariable(F.Entry, *Off);
          Var && Var->Type && Var->Type->Kind == NdTypeKind::Array) {
        R.Capacity = byteSize(Var->Type);
        R.Detail = "declared array";
        return R;
      }
    }
    for (const MedTypedLocal &L : F.TypedLocals)
      if (L.StackOff == *Off && L.Type) {
        uint64_t Sz = byteSize(L.Type);
        if (Sz) {
          R.Capacity = Sz;
          R.Detail = "typed local";
          return R;
        }
      }
    if (*Off < 0) {
      uint64_t Bound = static_cast<uint64_t>(-*Off);
      if (F.FrameSize > 0)
        Bound = std::min(Bound, static_cast<uint64_t>(F.FrameSize));
      R.Capacity = Bound;
      R.Detail = "stack frame bound";
    }
      return R;
    }

    return R; // unknown destination -> capacity stays unset.
  }

private:
  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const MedFunc &F;
  DefIndex Defs;
  llvm::DenseSet<ValueKey> Active;

  const MedOp *defOp(const MedVar &V, int &BlkOut, int &OpOut) const {
    auto It = Defs.OpDef.find(keyOf(V));
    if (It == Defs.OpDef.end())
      return nullptr;
    BlkOut = It->second.first;
    OpOut = It->second.second;
    return &F.Blocks[BlkOut].Ops[OpOut];
  }

  std::optional<uint64_t> allocCapacity(const MedVar &V, int Depth) {
    if (Depth > 32 || V.isConst())
      return std::nullopt;
    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(V, Blk, Oi);
    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return Op->NumInputs >= 1 ? allocCapacity(Op->Inputs[0], Depth + 1)
                                : std::nullopt;
    case NdOp::CALL:
    case NdOp::INDIR_CALL: {
      const MedCallInfo *CI = F.findCall(F.Blocks[Blk].Id, Oi);
      if (!CI)
        return std::nullopt;
      const SinkEntry *E = Cat.matchSink(CI->TargetName);
      if (!E || E->Kind != SinkKind::Alloc)
        return std::nullopt;
      std::string Norm = SinkCatalog::normalize(CI->TargetName);
      auto constArg = [&](int Idx) -> std::optional<uint64_t> {
        if (Idx < 0 || Idx >= static_cast<int>(CI->Args.size()))
          return std::nullopt;
        if (!CI->Args[Idx].isConst())
          return std::nullopt;
        return CI->Args[Idx].ConstVal;
      };
      if (Norm == "calloc") {
        auto Count = constArg(E->SrcArg);
        auto Size = constArg(E->LenArg);
        if (Count && Size)
          return *Count * *Size;
        return std::nullopt;
      }
      return constArg(E->LenArg);
    }
    default:
      return std::nullopt;
    }
  }

  // Signed offset of a pointer from the incoming stack pointer, or nullopt when
  // the value is not a stack address this walk can prove.
  std::optional<int64_t> stackOffset(const MedVar &V, int Depth) {
    if (Depth > 64 || V.isConst())
      return std::nullopt;

    ValueKey K = keyOf(V);
    if (!Active.insert(K).second)
      return std::nullopt;
    struct Pop {
      llvm::DenseSet<ValueKey> &S;
      ValueKey K;
      ~Pop() { S.erase(K); }
    } Guard{Active, K};

    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(V, Blk, Oi);

    if (In.StackRegsKnown && V.Kind == MedVar::Reg &&
        V.RegOff == In.StackPointerReg) {
      if (Op && (Op->Opcode == NdOp::INT_SUB || Op->Opcode == NdOp::INT_ADD))
        return affine(*Op, Depth);
      return 0; // the incoming stack pointer itself.
    }
    if (In.StackRegsKnown && V.Kind == MedVar::Reg &&
        V.RegOff == In.FramePointerReg) {
      if (Op && (Op->Opcode == NdOp::INT_SUB || Op->Opcode == NdOp::INT_ADD))
        return affine(*Op, Depth);
      return std::nullopt; // an incoming frame pointer is the caller's frame.
    }

    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      return Op->NumInputs >= 1 ? stackOffset(Op->Inputs[0], Depth + 1)
                                : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return affine(*Op, Depth);
    default:
      return std::nullopt;
    }
  }

  std::optional<int64_t> affine(const MedOp &Op, int Depth) {
    if (Op.NumInputs < 2)
      return std::nullopt;
    const MedVar &A = Op.Inputs[0];
    const MedVar &B = Op.Inputs[1];
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (B.isConst()) {
      if (auto Base = stackOffset(A, Depth + 1))
        return Sub ? *Base - static_cast<int64_t>(B.ConstVal)
                   : *Base + static_cast<int64_t>(B.ConstVal);
      return std::nullopt;
    }
    if (A.isConst() && !Sub) {
      if (auto Base = stackOffset(B, Depth + 1))
        return *Base + static_cast<int64_t>(A.ConstVal);
    }
    return std::nullopt;
  }
};

} // namespace

DestObject neverd::safety::resolveDestination(const AnalysisInput &In,
                                              const SinkCatalog &Cat,
                                              const MedFunc &F,
                                              size_t CallInfoIndex,
                                              int DstArgIndex) {
  DestObject R;
  if (DstArgIndex < 0 || CallInfoIndex >= F.CallInfos.size())
    return R;
  const MedCallInfo &CI = F.CallInfos[CallInfoIndex];
  if (DstArgIndex >= static_cast<int>(CI.Args.size()))
    return R;
  Resolver Res(In, Cat, F);
  return Res.resolve(CI.Args[DstArgIndex]);
}
