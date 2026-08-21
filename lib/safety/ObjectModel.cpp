//===- ObjectModel.cpp - Destination capacity and heap-object sizing ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ObjectModel.h"

#include "StackSlotFlow.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/safety/SinkScanner.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

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

std::optional<uint64_t> byteSizeRec(const TypeRef &T,
                                    llvm::DenseSet<const NdType *> &Active) {
  if (!T)
    return std::nullopt;
  switch (T->Kind) {
  case NdTypeKind::Array: {
    if (!T->ElemType || !Active.insert(T.get()).second)
      return std::nullopt;
    struct Pop {
      llvm::DenseSet<const NdType *> &Set;
      const NdType *Type;
      ~Pop() { Set.erase(Type); }
    } Guard{Active, T.get()};
    std::optional<uint64_t> Element = byteSizeRec(T->ElemType, Active);
    if (!Element ||
        (T->ArrayCount != 0 &&
         *Element > std::numeric_limits<uint64_t>::max() / T->ArrayCount))
      return std::nullopt;
    return *Element * T->ArrayCount;
  }
  default:
    return T->Size;
  }
}

std::optional<uint64_t> byteSize(const TypeRef &T) {
  llvm::DenseSet<const NdType *> Active;
  return byteSizeRec(T, Active);
}

struct DefIndex {
  llvm::DenseMap<ValueKey, std::pair<int, int>> OpDef;
  llvm::DenseMap<ValueKey, std::pair<int, int>> PhiDef;

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
      R.CapacityExact = true;
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
        auto useArray = [&](const std::optional<VariableSym> &Var) {
          if (!Var || !Var->Type || Var->Type->Kind != NdTypeKind::Array)
            return false;
          std::optional<uint64_t> Size = byteSize(Var->Type);
          if (!Size || *Size == 0)
            return false;
          R.Capacity = *Size;
          R.CapacityExact = true;
          R.Detail = "declared array";
          return true;
        };
        if (auto Base = frameBaseOffset())
          if (auto Relative = detail::checkedStackOffset(*Off, *Base, true))
            if (useArray(In.Dbg->resolveVariable(F.Entry, *Relative)))
              return R;
        if (useArray(In.Dbg->resolveVariable(F.Entry, *Off)))
          return R;
        if (F.FrameSize > 0 && *Off <= 0 &&
            *Off >= -static_cast<int64_t>(F.FrameSize)) {
          const int64_t Adjusted = *Off + F.FrameSize;
          if (useArray(In.Dbg->resolveStackPointerVariable(F.Entry, Adjusted)))
            return R;
        }
      }
      for (const MedTypedLocal &L : F.TypedLocals)
        if (L.StackOff == *Off && L.Type && L.Type->Kind == NdTypeKind::Array) {
          std::optional<uint64_t> Size = byteSize(L.Type);
          if (Size && *Size) {
            R.Capacity = *Size;
            R.CapacityExact = true;
            R.Detail = "typed local";
            return R;
          }
        }
      if (*Off < 0) {
        uint64_t Bound = uint64_t{0} - static_cast<uint64_t>(*Off);
        if (F.FrameSize > 0)
          Bound = std::min(Bound, static_cast<uint64_t>(F.FrameSize));
        R.Capacity = Bound;
        R.CapacityExact = false;
        R.Detail = "stack frame bound";
      }
      return R;
    }

    if (std::optional<DestObject> Global = globalObject(Dst))
      return *Global;

    return R; // unknown destination -> capacity stays unset.
  }

private:
  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const MedFunc &F;
  DefIndex Defs;
  llvm::DenseSet<ValueKey> Active;

  std::optional<DestObject> globalObject(const MedVar &V) const {
    if (!In.Img)
      return std::nullopt;
    llvm::DenseSet<ValueKey> Seen;
    std::optional<uint64_t> Address = constantValue(V, 0, Seen);
    if (!Address)
      return std::nullopt;
    const bool IsOwnedZero = *Address == 0 && V.isConst() &&
                             isDataAddressProvenance(V.Provenance) &&
                             V.AddressOwnerVA != InvalidVA;
    if (*Address == 0 && !IsOwnedZero)
      return std::nullopt;

    const Segment *Seg = In.Img->getSegmentFor(*Address);
    if (!Seg || !Seg->isWritable() || Seg->Size > InvalidVA - Seg->VA)
      return std::nullopt;
    if (IsOwnedZero) {
      const Segment *OwnerSeg = In.Img->getSegmentFor(V.AddressOwnerVA);
      const Section *TargetSec = In.Img->getSectionFor(*Address);
      const Section *OwnerSec = In.Img->getSectionFor(V.AddressOwnerVA);
      if (OwnerSeg != Seg || ((TargetSec || OwnerSec) && TargetSec != OwnerSec))
        return std::nullopt;
    }
    const uint64_t SegmentEnd = Seg->VA + Seg->Size;
    uint64_t OwnerBegin = Seg->VA;
    uint64_t OwnerEnd = SegmentEnd;
    if (const Section *Sec = In.Img->getSectionFor(*Address)) {
      if (!Sec->isWritable() || Sec->Size == 0 ||
          Sec->Size > InvalidVA - Sec->VA)
        return std::nullopt;
      OwnerBegin = Sec->VA;
      OwnerEnd = std::min<uint64_t>(OwnerEnd, Sec->VA + Sec->Size);
    } else if (In.Img->segmentHasReadableSectionMetadata(*Seg)) {
      return std::nullopt;
    }

    std::optional<uint64_t> ExactCapacity;
    bool ConflictingSymbols = false;
    for (const Symbol &Sym : In.Img->Symbols) {
      if (Sym.IsFunc || Sym.Size == 0 || Sym.Addr < OwnerBegin ||
          Sym.Addr > *Address || Sym.Size > InvalidVA - Sym.Addr)
        continue;
      const uint64_t SymbolEnd = Sym.Addr + Sym.Size;
      if (*Address >= SymbolEnd || SymbolEnd > OwnerEnd)
        continue;
      const uint64_t Capacity = SymbolEnd - *Address;
      if (ExactCapacity && *ExactCapacity != Capacity) {
        ConflictingSymbols = true;
        break;
      }
      ExactCapacity = Capacity;
    }

    DestObject Result;
    Result.Region = ObjectRegion::Global;
    if (ExactCapacity && !ConflictingSymbols) {
      Result.Capacity = *ExactCapacity;
      Result.CapacityExact = true;
      Result.Detail = "sized data symbol";
      return Result;
    }

    if (*Address > OwnerEnd)
      return std::nullopt;
    Result.Capacity = OwnerEnd - *Address;
    Result.CapacityExact = false;
    Result.Detail = "mapped global bound";
    return Result;
  }

  const MedOp *defOp(const MedVar &V, int &BlkOut, int &OpOut) const {
    auto It = Defs.OpDef.find(keyOf(V));
    if (It == Defs.OpDef.end())
      return nullptr;
    BlkOut = It->second.first;
    OpOut = It->second.second;
    return &F.Blocks[BlkOut].Ops[OpOut];
  }

  static uint64_t truncateToSize(uint64_t Value, uint16_t Size) {
    if (Size == 0 || Size >= sizeof(uint64_t))
      return Value;
    const unsigned Bits = static_cast<unsigned>(Size) * 8;
    return Value & ((uint64_t{1} << Bits) - 1);
  }

  std::optional<uint64_t> constantValue(const MedVar &V, int Depth,
                                        llvm::DenseSet<ValueKey> &Seen) const {
    if (V.isConst())
      return truncateToSize(V.ConstVal, V.Size);
    if (Depth > 32 || !Seen.insert(keyOf(V)).second)
      return std::nullopt;
    struct Pop {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~Pop() { Set.erase(Key); }
    } Guard{Seen, keyOf(V)};

    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(V, Blk, Oi);
    if (!Op)
      return std::nullopt;

    auto input = [&](unsigned Index) -> std::optional<uint64_t> {
      if (Index >= Op->NumInputs)
        return std::nullopt;
      return constantValue(Op->Inputs[Index], Depth + 1, Seen);
    };
    auto finish = [&](uint64_t Value) {
      return std::optional<uint64_t>(truncateToSize(Value, Op->Output.Size));
    };

    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
    case NdOp::INT_ZEXT:
      if (auto A = input(0))
        return finish(*A);
      return std::nullopt;
    case NdOp::INT_SEXT:
      if (auto A = input(0)) {
        const unsigned Bits = Op->Inputs[0].Size * 8;
        uint64_t Extended = *A;
        if (Bits > 0 && Bits < 64 && (Extended & (uint64_t{1} << (Bits - 1))))
          Extended |= ~((uint64_t{1} << Bits) - 1);
        return finish(Extended);
      }
      return std::nullopt;
    case NdOp::SUBBYTES:
      if (auto A = input(0)) {
        auto Offset = input(1);
        if (Offset && *Offset < sizeof(uint64_t))
          return finish(*A >> (*Offset * 8));
      }
      return std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR: {
      auto A = input(0);
      auto B = input(1);
      if (!A || !B)
        return std::nullopt;
      switch (Op->Opcode) {
      case NdOp::INT_ADD:
        return finish(*A + *B);
      case NdOp::INT_SUB:
        return finish(*A - *B);
      case NdOp::INT_MULT:
        return finish(*A * *B);
      case NdOp::INT_AND:
        return finish(*A & *B);
      case NdOp::INT_OR:
        return finish(*A | *B);
      case NdOp::INT_XOR:
        return finish(*A ^ *B);
      default:
        return std::nullopt;
      }
    }
    default:
      return std::nullopt;
    }
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
      const std::string Name = resolveCallName(In, *CI);
      const SinkEntry *E = Cat.matchSink(Name);
      if (!E ||
          (E->Kind != SinkKind::Alloc && E->Kind != SinkKind::StackAlloc) ||
          E->HandleArg >= 0)
        return std::nullopt;
      if (Op->Output.Size == 0 || Op->Output.Size > sizeof(uint64_t))
        return std::nullopt;
      const unsigned PointerBits = static_cast<unsigned>(Op->Output.Size) * 8;
      const uint64_t MaxObjectSize = PointerBits == 64
                                         ? std::numeric_limits<uint64_t>::max()
                                         : (uint64_t{1} << PointerBits) - 1;
      std::string Norm = SinkCatalog::normalize(Name);
      auto constArg = [&](int Idx) -> std::optional<uint64_t> {
        if (Idx < 0 || Idx >= static_cast<int>(CI->Args.size()))
          return std::nullopt;
        llvm::DenseSet<ValueKey> Seen;
        return constantValue(CI->Args[Idx], 0, Seen);
      };
      if (Norm == "calloc") {
        auto Count = constArg(E->SrcArg);
        auto Size = constArg(E->LenArg);
        if (Count && Size && (*Count == 0 || *Size <= MaxObjectSize / *Count))
          return *Count * *Size;
        return std::nullopt;
      }
      std::optional<uint64_t> Size = constArg(E->LenArg);
      return Size && *Size <= MaxObjectSize ? Size : std::nullopt;
    }
    case NdOp::LOAD: {
      if (Op->NumInputs == 0)
        return std::nullopt;
      const MedVar &Addr = Op->Inputs[Op->NumInputs >= 2 ? 1 : 0];
      Active.clear();
      auto Off = stackOffset(Addr, 0);
      if (!Off)
        return std::nullopt;
      auto Resolve = [&](const MedVar &V) {
        Active.clear();
        return stackOffset(V, 0);
      };
      auto MayBeFrame = [&](const MedVar &V) {
        llvm::DenseSet<ValueKey> Seen;
        return mayBeStackAddress(V, 0, Seen);
      };
      detail::ReachingStackValues Reaching = detail::reachingStackValues(
          F, F.Blocks[Blk].Id, Oi, *Off, Op->Output.Size, Resolve, MayBeFrame);
      if (!Reaching.Complete)
        return std::nullopt;
      std::optional<uint64_t> Capacity;
      for (const MedVar &Stored : Reaching.Values) {
        auto Candidate = allocCapacity(Stored, Depth + 1);
        if (!Candidate || (Capacity && *Capacity != *Candidate))
          return std::nullopt;
        Capacity = Candidate;
      }
      return Capacity;
    }
    default:
      return std::nullopt;
    }
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
    auto It = Defs.OpDef.find(keyOf(V));
    if (It == Defs.OpDef.end())
      return false;
    const MedOp &Op = F.Blocks[It->second.first].Ops[It->second.second];
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
      return false;
    }
    for (unsigned I = Begin; I < End; ++I) {
      llvm::DenseSet<ValueKey> BranchSeen = Seen;
      if (mayBeStackAddress(Op.Inputs[I], Depth + 1, BranchSeen))
        return true;
    }
    return false;
  }

  std::optional<int64_t> frameBaseOffset() {
    if (!In.StackRegsKnown)
      return std::nullopt;
    for (const MedBlock &B : F.Blocks) {
      for (const MedOp &Op : B.Ops) {
        if (Op.Output.Kind != MedVar::Reg ||
            Op.Output.RegOff != In.FramePointerReg)
          continue;
        Active.clear();
        if (auto Off = stackOffset(Op.Output, 0))
          return Off;
      }
    }
    return std::nullopt;
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
      if (Op) {
        if (Op->Opcode == NdOp::INT_SUB || Op->Opcode == NdOp::INT_ADD)
          return affine(*Op, Depth);
        if ((Op->Opcode == NdOp::COPY || Op->Opcode == NdOp::CAST) &&
            Op->NumInputs >= 1)
          return stackOffset(Op->Inputs[0], Depth + 1);
      }
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
      auto Delta = detail::signedStackConstant(B);
      if (auto Base = stackOffset(A, Depth + 1); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, Sub);
      return std::nullopt;
    }
    if (A.isConst() && !Sub) {
      auto Delta = detail::signedStackConstant(A);
      if (auto Base = stackOffset(B, Depth + 1); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, false);
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
