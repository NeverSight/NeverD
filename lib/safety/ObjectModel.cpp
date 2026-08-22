//===- ObjectModel.cpp - Destination capacity and heap-object sizing ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/ObjectModel.h"

#include "SourceSemantics.h"
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
    if (!detail::hasTargetPointerWidth(In.Img, Dst))
      return R;

    // A dynamic allocation with a known size gives an exact capacity while
    // preserving whether the object lives on the heap or stack.
    resetAllocationProof();
    if (auto Allocation = allocationObject(Dst, 0)) {
      R.Region = Allocation->Region;
      R.Capacity = Allocation->Capacity;
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

    resetConstantProof();
    resetExactDataIdentityProof();
    if (std::optional<DestObject> Global = globalObject(Dst))
      return *Global;

    return R; // unknown destination -> capacity stays unset.
  }

private:
  struct AllocationObject {
    ObjectRegion Region = ObjectRegion::Unknown;
    uint64_t Capacity = 0;
  };

  struct ExactDataIdentity {
    uint64_t Address = 0;
    uint64_t OwnerVA = InvalidVA;
  };

  const AnalysisInput &In;
  const SinkCatalog &Cat;
  const MedFunc &F;
  DefIndex Defs;
  llvm::DenseSet<ValueKey> Active;
  llvm::DenseMap<ValueKey, std::optional<uint64_t>> ConstantCache;
  llvm::DenseSet<ValueKey> ConstantActive;
  llvm::DenseMap<ValueKey, std::optional<ExactDataIdentity>>
      ExactDataIdentityCache;
  llvm::DenseSet<ValueKey> ExactDataIdentityActive;
  llvm::DenseMap<ValueKey, std::optional<AllocationObject>> AllocationCache;
  llvm::DenseSet<ValueKey> AllocationActive;
  llvm::DenseMap<ValueKey, bool> StackAddressCache;
  llvm::DenseSet<ValueKey> StackAddressActive;
  unsigned ConstantProofBudget = 0;
  unsigned ExactDataIdentityProofBudget = 0;
  unsigned AllocationProofBudget = 0;
  unsigned StackAddressProofBudget = 0;
  bool ConstantProofIncomplete = false;
  bool ExactDataIdentityProofIncomplete = false;
  bool AllocationProofIncomplete = false;
  bool StackAddressProofIncomplete = false;

  // Memoization makes ordinary queries linear in the reachable SSA graph.
  // The cap is a final fail-closed guard for malformed, exceptionally large
  // graphs; incomplete walks are never published into the negative caches.
  static constexpr unsigned MaxProofSteps = 4096;

  void resetConstantProof() {
    ConstantCache.clear();
    ConstantActive.clear();
    ConstantProofBudget = MaxProofSteps;
    ConstantProofIncomplete = false;
  }

  void resetExactDataIdentityProof() {
    ExactDataIdentityCache.clear();
    ExactDataIdentityActive.clear();
    ExactDataIdentityProofBudget = MaxProofSteps;
    ExactDataIdentityProofIncomplete = false;
  }

  void resetAllocationProof() {
    AllocationCache.clear();
    AllocationActive.clear();
    AllocationProofBudget = MaxProofSteps;
    AllocationProofIncomplete = false;
    resetConstantProof();
  }

  void resetStackAddressProof() {
    StackAddressCache.clear();
    StackAddressActive.clear();
    StackAddressProofBudget = MaxProofSteps;
    StackAddressProofIncomplete = false;
  }

  std::optional<ExactDataIdentity> exactDataIdentity(const MedVar &V,
                                                     int Depth) {
    if (V.isConst()) {
      if (V.Size == 0 || V.Size > sizeof(uint64_t) ||
          V.Provenance != ConstantAddressProvenance::DataAddress ||
          V.AddressOwnerVA == InvalidVA)
        return std::nullopt;
      return ExactDataIdentity{truncateToSize(V.ConstVal, V.Size),
                               V.AddressOwnerVA};
    }
    const ValueKey Key = keyOf(V);
    if (auto It = ExactDataIdentityCache.find(Key);
        It != ExactDataIdentityCache.end())
      return It->second;
    if (Depth > 32 || ExactDataIdentityProofBudget == 0 ||
        !ExactDataIdentityActive.insert(Key).second) {
      ExactDataIdentityProofIncomplete = true;
      return std::nullopt;
    }
    struct Pop {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~Pop() { Set.erase(Key); }
    } Guard{ExactDataIdentityActive, Key};
    --ExactDataIdentityProofBudget;

    auto finish = [&](std::optional<ExactDataIdentity> Result) {
      if (!ExactDataIdentityProofIncomplete)
        ExactDataIdentityCache[Key] = Result;
      return Result;
    };

    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(V, Blk, Oi);
    if (!Op || Op->Output.Size == 0 || Op->Output.Size > sizeof(uint64_t))
      return finish(std::nullopt);
    if (Op->NumInputs == 1 && Op->Output.Size == Op->Inputs[0].Size &&
        (Op->Opcode == NdOp::COPY || Op->Opcode == NdOp::CAST))
      return finish(exactDataIdentity(Op->Inputs[0], Depth + 1));

    if ((Op->Opcode != NdOp::INT_ADD && Op->Opcode != NdOp::INT_SUB) ||
        Op->NumInputs != 2)
      return finish(std::nullopt);

    auto scalarConstant =
        [&](const MedVar &Candidate) -> std::optional<uint64_t> {
      llvm::DenseMap<ValueKey, std::optional<uint64_t>> ScalarCache;
      llvm::DenseSet<ValueKey> ScalarActive;
      unsigned Remaining = MaxProofSteps;
      bool Incomplete = false;
      std::optional<uint64_t> Result = scalarConstantValue(
          Candidate, 0, ScalarCache, ScalarActive, Remaining, Incomplete);
      return Incomplete ? std::nullopt : Result;
    };
    auto adjustedIdentity =
        [&](const MedVar &Base, const MedVar &Offset,
            bool Subtract) -> std::optional<ExactDataIdentity> {
      if (Base.Size != Op->Output.Size || Offset.Size == 0 ||
          Offset.Size > Op->Output.Size)
        return std::nullopt;
      std::optional<ExactDataIdentity> Identity =
          exactDataIdentity(Base, Depth + 1);
      std::optional<uint64_t> Scalar = scalarConstant(Offset);
      if (!Identity || !Scalar)
        return std::nullopt;
      const uint64_t Address =
          Subtract ? Identity->Address - *Scalar : Identity->Address + *Scalar;
      Identity->Address = truncateToSize(Address, Op->Output.Size);
      return Identity;
    };

    if (Op->Opcode == NdOp::INT_SUB)
      return finish(adjustedIdentity(Op->Inputs[0], Op->Inputs[1], true));
    if (auto Identity = adjustedIdentity(Op->Inputs[0], Op->Inputs[1], false))
      return finish(Identity);
    return finish(adjustedIdentity(Op->Inputs[1], Op->Inputs[0], false));
  }

  std::optional<uint64_t>
  scalarConstantValue(const MedVar &V, unsigned Depth,
                      llvm::DenseMap<ValueKey, std::optional<uint64_t>> &Cache,
                      llvm::DenseSet<ValueKey> &Active, unsigned &Remaining,
                      bool &Incomplete) const {
    if (V.Size == 0 || V.Size > sizeof(uint64_t))
      return std::nullopt;
    if (V.isConst()) {
      if (V.Provenance != ConstantAddressProvenance::Scalar)
        return std::nullopt;
      return truncateToSize(V.ConstVal, V.Size);
    }

    const ValueKey Key = keyOf(V);
    if (auto It = Cache.find(Key); It != Cache.end())
      return It->second;
    if (Depth > 64 || Remaining == 0 || !Active.insert(Key).second) {
      Incomplete = true;
      return std::nullopt;
    }
    struct Pop {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~Pop() { Set.erase(Key); }
    } Guard{Active, Key};
    --Remaining;

    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(V, Blk, Oi);
    std::optional<uint64_t> Result;
    if (!Op || Op->Output.Size == 0 || Op->Output.Size > sizeof(uint64_t)) {
      if (!Incomplete)
        Cache[Key] = Result;
      return Result;
    }
    auto input = [&](unsigned Index) -> std::optional<uint64_t> {
      if (Index >= Op->NumInputs)
        return std::nullopt;
      return scalarConstantValue(Op->Inputs[Index], Depth + 1, Cache, Active,
                                 Remaining, Incomplete);
    };
    auto finish = [&](uint64_t Value) {
      return std::optional<uint64_t>(truncateToSize(Value, Op->Output.Size));
    };

    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      if (Op->NumInputs == 1 && Op->Inputs[0].Size == Op->Output.Size)
        Result = input(0);
      break;
    case NdOp::INT_ZEXT:
      if (Op->NumInputs == 1 && Op->Inputs[0].Size > 0 &&
          Op->Inputs[0].Size <= Op->Output.Size)
        if (auto A = input(0))
          Result = finish(*A);
      break;
    case NdOp::INT_SEXT:
      if (Op->NumInputs == 1 && Op->Inputs[0].Size > 0 &&
          Op->Inputs[0].Size <= Op->Output.Size)
        if (auto A = input(0)) {
          const unsigned Bits = Op->Inputs[0].Size * 8;
          uint64_t Extended = *A;
          if (Bits < 64 && (Extended & (uint64_t{1} << (Bits - 1))))
            Extended |= ~((uint64_t{1} << Bits) - 1);
          Result = finish(Extended);
        }
      break;
    case NdOp::SUBBYTES:
      if (Op->NumInputs == 2 && Op->Inputs[0].Size >= Op->Output.Size)
        if (auto A = input(0))
          if (auto Offset = input(1); Offset && *Offset < sizeof(uint64_t))
            Result = finish(*A >> (*Offset * 8));
      break;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR: {
      if (Op->NumInputs != 2 || Op->Inputs[0].Size != Op->Output.Size ||
          Op->Inputs[1].Size != Op->Output.Size)
        break;
      auto A = input(0);
      auto B = input(1);
      if (!A || !B)
        break;
      switch (Op->Opcode) {
      case NdOp::INT_ADD:
        Result = finish(*A + *B);
        break;
      case NdOp::INT_SUB:
        Result = finish(*A - *B);
        break;
      case NdOp::INT_MULT:
        Result = finish(*A * *B);
        break;
      case NdOp::INT_AND:
        Result = finish(*A & *B);
        break;
      case NdOp::INT_OR:
        Result = finish(*A | *B);
        break;
      case NdOp::INT_XOR:
        Result = finish(*A ^ *B);
        break;
      default:
        break;
      }
      break;
    }
    default:
      break;
    }
    if (!Incomplete)
      Cache[Key] = Result;
    return Result;
  }

  std::optional<DestObject> globalObject(const MedVar &V) {
    if (!In.Img)
      return std::nullopt;
    std::optional<ExactDataIdentity> Identity = exactDataIdentity(V, 0);
    if (ExactDataIdentityProofIncomplete)
      Identity.reset();
    std::optional<uint64_t> Address =
        Identity ? std::optional<uint64_t>(Identity->Address)
                 : constantValue(V, 0);
    if (!Address)
      return std::nullopt;
    if (*Address == 0 && !Identity)
      return std::nullopt;

    const Segment *Seg = nullptr;
    const Section *Sec = nullptr;
    uint64_t OwnerBegin = 0;
    uint64_t OwnerEnd = 0;
    if (Identity) {
      Seg = In.Img->getSegmentFor(Identity->OwnerVA);
      if (!Seg || !Seg->isWritable() || Seg->Size > InvalidVA - Seg->VA)
        return std::nullopt;
      const uint64_t SegmentEnd = Seg->VA + Seg->Size;
      Sec = In.Img->getSectionFor(Identity->OwnerVA);
      if (Sec) {
        if (!Sec->isWritable() || Sec->Size == 0 ||
            Sec->Size > InvalidVA - Sec->VA)
          return std::nullopt;
        OwnerBegin = Sec->VA;
        OwnerEnd = std::min<uint64_t>(SegmentEnd, Sec->VA + Sec->Size);
      } else {
        if (In.Img->segmentHasReadableSectionMetadata(*Seg))
          return std::nullopt;
        OwnerBegin = Seg->VA;
        OwnerEnd = SegmentEnd;
      }
      if (*Address < OwnerBegin || *Address > OwnerEnd)
        return std::nullopt;
    } else {
      Seg = In.Img->getSegmentFor(*Address);
      if (!Seg || !Seg->isWritable() || Seg->Size > InvalidVA - Seg->VA)
        return std::nullopt;
      const uint64_t SegmentEnd = Seg->VA + Seg->Size;
      OwnerBegin = Seg->VA;
      OwnerEnd = SegmentEnd;
      Sec = In.Img->getSectionFor(*Address);
      if (Sec) {
        if (!Sec->isWritable() || Sec->Size == 0 ||
            Sec->Size > InvalidVA - Sec->VA)
          return std::nullopt;
        OwnerBegin = Sec->VA;
        OwnerEnd = std::min<uint64_t>(OwnerEnd, Sec->VA + Sec->Size);
      } else if (In.Img->segmentHasReadableSectionMetadata(*Seg)) {
        return std::nullopt;
      }
    }

    std::optional<uint64_t> ExactCapacity;
    bool ConflictingSymbols = false;
    bool HasOnePastSymbolAtAddress = false;
    for (const Symbol &Sym : In.Img->Symbols) {
      if (Sym.IsFunc || Sym.Size == 0 || Sym.Addr < OwnerBegin ||
          Sym.Size > InvalidVA - Sym.Addr)
        continue;
      const uint64_t SymbolEnd = Sym.Addr + Sym.Size;
      if (SymbolEnd > OwnerEnd)
        continue;
      // A section/run owner cannot distinguish `&A[sizeof A]` from `&B`
      // when adjacent sized symbols share the same numeric boundary.  Keep
      // the mapped bound, but never publish B's size as an exact capacity
      // unless a future occurrence identity names the symbol itself.
      if (Sym.Addr < *Address && SymbolEnd == *Address)
        HasOnePastSymbolAtAddress = true;
      if (Sym.Addr > *Address)
        continue;
      if (*Address >= SymbolEnd)
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
    if (ExactCapacity && !ConflictingSymbols && !HasOnePastSymbolAtAddress) {
      Result.Capacity = *ExactCapacity;
      Result.CapacityExact = true;
      Result.Detail = "sized data symbol";
      return Result;
    }

    if (*Address > OwnerEnd)
      return std::nullopt;
    Result.Capacity = OwnerEnd - *Address;
    Result.CapacityExact = Identity && *Address == OwnerEnd;
    Result.Detail = Result.CapacityExact ? "relocation owner boundary"
                                         : "mapped global bound";
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

  std::optional<uint64_t> constantValue(const MedVar &V, int Depth) {
    if (V.isConst()) {
      if (V.Size == 0 || V.Size > sizeof(uint64_t) ||
          isAddressProvenance(V.Provenance))
        return std::nullopt;
      return truncateToSize(V.ConstVal, V.Size);
    }

    const ValueKey Key = keyOf(V);
    if (auto It = ConstantCache.find(Key); It != ConstantCache.end())
      return It->second;
    if (Depth > 32) {
      ConstantProofIncomplete = true;
      return std::nullopt;
    }

    if (!ConstantActive.insert(Key).second) {
      ConstantProofIncomplete = true;
      return std::nullopt;
    }
    struct Pop {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~Pop() { Set.erase(Key); }
    } Guard{ConstantActive, Key};
    if (ConstantProofBudget == 0) {
      ConstantProofIncomplete = true;
      return std::nullopt;
    }
    --ConstantProofBudget;

    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(V, Blk, Oi);
    std::optional<uint64_t> Result;
    if (Op && Op->Output.Size > 0 && Op->Output.Size <= sizeof(uint64_t)) {
      auto input = [&](unsigned Index) -> std::optional<uint64_t> {
        if (Index >= Op->NumInputs)
          return std::nullopt;
        return constantValue(Op->Inputs[Index], Depth + 1);
      };
      auto finish = [&](uint64_t Value) {
        return std::optional<uint64_t>(truncateToSize(Value, Op->Output.Size));
      };

      switch (Op->Opcode) {
      case NdOp::COPY:
      case NdOp::CAST:
        if (Op->NumInputs == 1 && Op->Inputs[0].Size == Op->Output.Size)
          if (auto A = input(0))
            Result = finish(*A);
        break;
      case NdOp::INT_ZEXT:
        if (Op->NumInputs == 1 && Op->Inputs[0].Size > 0 &&
            Op->Inputs[0].Size <= Op->Output.Size)
          if (auto A = input(0))
            Result = finish(*A);
        break;
      case NdOp::INT_SEXT:
        if (Op->NumInputs == 1 && Op->Inputs[0].Size > 0 &&
            Op->Inputs[0].Size <= Op->Output.Size)
          if (auto A = input(0)) {
            const unsigned Bits = Op->Inputs[0].Size * 8;
            uint64_t Extended = *A;
            if (Bits < 64 && (Extended & (uint64_t{1} << (Bits - 1))))
              Extended |= ~((uint64_t{1} << Bits) - 1);
            Result = finish(Extended);
          }
        break;
      case NdOp::SUBBYTES:
        if (Op->NumInputs == 2 && Op->Inputs[0].Size >= Op->Output.Size)
          if (auto A = input(0)) {
            auto Offset = input(1);
            if (Offset && *Offset < sizeof(uint64_t))
              Result = finish(*A >> (*Offset * 8));
          }
        break;
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_MULT:
      case NdOp::INT_AND:
      case NdOp::INT_OR:
      case NdOp::INT_XOR: {
        if (Op->NumInputs != 2 || Op->Inputs[0].Size != Op->Output.Size ||
            Op->Inputs[1].Size != Op->Output.Size)
          break;
        auto A = input(0);
        auto B = input(1);
        if (!A || !B)
          break;
        switch (Op->Opcode) {
        case NdOp::INT_ADD:
          Result = finish(*A + *B);
          break;
        case NdOp::INT_SUB:
          Result = finish(*A - *B);
          break;
        case NdOp::INT_MULT:
          Result = finish(*A * *B);
          break;
        case NdOp::INT_AND:
          Result = finish(*A & *B);
          break;
        case NdOp::INT_OR:
          Result = finish(*A | *B);
          break;
        case NdOp::INT_XOR:
          Result = finish(*A ^ *B);
          break;
        default:
          break;
        }
        break;
      }
      default:
        break;
      }
    }
    if (!ConstantProofIncomplete)
      ConstantCache[Key] = Result;
    return Result;
  }

  std::optional<AllocationObject> allocationObject(const MedVar &V, int Depth) {
    if (V.isConst())
      return std::nullopt;

    const ValueKey Key = keyOf(V);
    if (auto It = AllocationCache.find(Key); It != AllocationCache.end())
      return It->second;
    if (Depth > 32) {
      AllocationProofIncomplete = true;
      return std::nullopt;
    }

    if (!AllocationActive.insert(Key).second) {
      AllocationProofIncomplete = true;
      return std::nullopt;
    }
    struct Pop {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~Pop() { Set.erase(Key); }
    } Guard{AllocationActive, Key};
    if (AllocationProofBudget == 0) {
      AllocationProofIncomplete = true;
      return std::nullopt;
    }
    --AllocationProofBudget;

    std::optional<AllocationObject> Result = allocationObjectImpl(V, Depth);
    if (AllocationProofIncomplete || ConstantProofIncomplete)
      return std::nullopt;
    AllocationCache[Key] = Result;
    return Result;
  }

  std::optional<AllocationObject> allocationObjectImpl(const MedVar &V,
                                                       int Depth) {

    MedVar Current = V;
    llvm::DenseSet<ValueKey> ForwardSeen;
    while (!Current.isConst()) {
      if (!ForwardSeen.insert(keyOf(Current)).second)
        return std::nullopt;
      int ForwardBlock = 0, ForwardOp = 0;
      const MedOp *Forwarded = defOp(Current, ForwardBlock, ForwardOp);
      if (!Forwarded || Forwarded->NumInputs != 1 ||
          (Forwarded->Opcode != NdOp::COPY && Forwarded->Opcode != NdOp::CAST &&
           Forwarded->Opcode != NdOp::INT_ZEXT &&
           Forwarded->Opcode != NdOp::INT_SEXT))
        break;
      if (Forwarded->Output.Size == 0 ||
          Forwarded->Output.Size != Forwarded->Inputs[0].Size)
        return std::nullopt;
      Current = Forwarded->Inputs[0];
    }
    if (Current.isConst())
      return std::nullopt;

    int Blk = 0, Oi = 0;
    const MedOp *Op = defOp(Current, Blk, Oi);
    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::INT_ADD: {
      if (Op->NumInputs != 2 || Op->Output.Size == 0)
        return std::nullopt;
      std::optional<AllocationObject> Base;
      std::optional<uint64_t> Offset;
      for (unsigned I = 0; I < 2; ++I) {
        if (Op->Inputs[I].Size == Op->Output.Size)
          if (auto Candidate = allocationObject(Op->Inputs[I], Depth + 1)) {
            if (Base)
              return std::nullopt;
            Base = Candidate;
            continue;
          }
        if (Op->Inputs[I].Size == 0 || Op->Inputs[I].Size > Op->Output.Size)
          return std::nullopt;
        std::optional<uint64_t> Constant = constantValue(Op->Inputs[I], 0);
        if (!Constant || Offset)
          return std::nullopt;
        Offset = Constant;
      }
      if (!Base || !Offset || *Offset > Base->Capacity)
        return std::nullopt;
      Base->Capacity -= *Offset;
      return Base;
    }
    case NdOp::CALL:
    case NdOp::INDIR_CALL: {
      const MedCallInfo *CI = F.findCall(F.Blocks[Blk].Id, Oi);
      if (!CI)
        return std::nullopt;
      const std::string Name = resolveCallName(In, *CI);
      const SinkEntry *E = Cat.matchSink(Name);
      if (!E || debugSinkSummaryConflicts(In, *CI, *E))
        return std::nullopt;
      ObjectRegion Region = ObjectRegion::Unknown;
      switch (E->Kind) {
      case SinkKind::Alloc:
        if (E->HandleArg >= 0)
          return std::nullopt;
        Region = ObjectRegion::Heap;
        break;
      case SinkKind::StackAlloc:
        if (E->HandleArg >= 0)
          return std::nullopt;
        Region = ObjectRegion::Stack;
        break;
      case SinkKind::Realloc:
        Region = ObjectRegion::Heap;
        break;
      default:
        return std::nullopt;
      }
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
        if (!detail::callArgumentHasTargetSizeCarrierWidth(In.Img, *CI, Idx))
          return std::nullopt;
        return constantValue(CI->Args[Idx], 0);
      };
      if (Norm == "calloc") {
        auto Count = constArg(E->SrcArg);
        auto Size = constArg(E->LenArg);
        if (Count && Size && (*Count == 0 || *Size <= MaxObjectSize / *Count))
          return AllocationObject{Region, *Count * *Size};
        return std::nullopt;
      }
      std::optional<uint64_t> Size = constArg(E->LenArg);
      return Size && *Size <= MaxObjectSize
                 ? std::optional<AllocationObject>(
                       AllocationObject{Region, *Size})
                 : std::nullopt;
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
      resetStackAddressProof();
      auto MayBeFrame = [&](const MedVar &V) {
        return mayBeStackAddress(V, 0);
      };
      auto AliasesWholeFrame = [&](const MedVar &V) {
        if (!In.StackRegsKnown)
          return false;
        auto FindOp = [&](const MedVar &Current) {
          int DefBlock = 0;
          int DefOp = 0;
          return defOp(Current, DefBlock, DefOp);
        };
        auto FindPhi = [&](const MedVar &Current) -> const PhiNode * {
          auto It = Defs.PhiDef.find(keyOf(Current));
          return It == Defs.PhiDef.end()
                     ? nullptr
                     : &F.Blocks[It->second.first].Phis[It->second.second];
        };
        return detail::aliasesWholeFrame(V, In.StackPointerReg,
                                         In.FramePointerReg, FindOp, FindPhi);
      };
      detail::ReachingStackValues Reaching = detail::reachingStackValues(
          F, F.Blocks[Blk].Id, Oi, *Off, Op->Output.Size, Resolve, MayBeFrame,
          AliasesWholeFrame);
      if (!Reaching.Complete)
        return std::nullopt;
      std::optional<AllocationObject> Allocation;
      for (const MedVar &Stored : Reaching.Values) {
        auto Candidate = allocationObject(Stored, Depth + 1);
        if (!Candidate ||
            (Allocation && (Allocation->Region != Candidate->Region ||
                            Allocation->Capacity != Candidate->Capacity)))
          return std::nullopt;
        Allocation = Candidate;
      }
      return Allocation;
    }
    default:
      return std::nullopt;
    }
  }

  bool mayBeStackAddress(const MedVar &V, int Depth) {
    if (!In.StackRegsKnown || V.isConst())
      return false;

    const ValueKey Key = keyOf(V);
    if (auto It = StackAddressCache.find(Key); It != StackAddressCache.end())
      return It->second;
    if (Depth > 64) {
      StackAddressProofIncomplete = true;
      return true;
    }

    if (!StackAddressActive.insert(Key).second) {
      StackAddressProofIncomplete = true;
      return true;
    }
    struct Pop {
      llvm::DenseSet<ValueKey> &Set;
      ValueKey Key;
      ~Pop() { Set.erase(Key); }
    } Guard{StackAddressActive, Key};
    if (StackAddressProofBudget == 0) {
      StackAddressProofIncomplete = true;
      return true;
    }
    --StackAddressProofBudget;

    bool Result = false;
    if (V.Kind == MedVar::Reg &&
        (V.RegOff == In.StackPointerReg || V.RegOff == In.FramePointerReg))
      Result = true;
    else if (auto It = Defs.PhiDef.find(Key); It != Defs.PhiDef.end()) {
      const PhiNode &Phi = F.Blocks[It->second.first].Phis[It->second.second];
      for (const auto &[Pred, Arg] : Phi.Args) {
        (void)Pred;
        if (mayBeStackAddress(Arg, Depth + 1)) {
          Result = true;
          break;
        }
      }
    } else if (auto It = Defs.OpDef.find(Key); It != Defs.OpDef.end()) {
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
        End = 0;
        break;
      }
      for (unsigned I = Begin; I < End; ++I)
        if (mayBeStackAddress(Op.Inputs[I], Depth + 1)) {
          Result = true;
          break;
        }
    }
    if (!StackAddressProofIncomplete)
      StackAddressCache[Key] = Result;
    return Result;
  }

  std::optional<int64_t> frameBaseOffset() {
    if (!In.StackRegsKnown)
      return std::nullopt;
    std::optional<int64_t> Result;
    for (const MedBlock &B : F.Blocks) {
      for (const MedOp &Op : B.Ops) {
        if (Op.Output.Kind != MedVar::Reg ||
            Op.Output.RegOff != In.FramePointerReg)
          continue;
        Active.clear();
        const std::optional<int64_t> Off = stackOffset(Op.Output, 0);
        if (!Off)
          continue;
        if (Result && *Result != *Off)
          return std::nullopt;
        Result = Off;
      }
    }
    return Result;
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
            Op->NumInputs == 1 && Op->Output.Size > 0 &&
            Op->Output.Size == Op->Inputs[0].Size)
          return stackOffset(Op->Inputs[0], Depth + 1);
      }
      return std::nullopt; // an incoming frame pointer is the caller's frame.
    }

    if (!Op)
      return std::nullopt;
    switch (Op->Opcode) {
    case NdOp::COPY:
    case NdOp::CAST:
      return Op->NumInputs == 1 && Op->Output.Size > 0 &&
                     Op->Output.Size == Op->Inputs[0].Size
                 ? stackOffset(Op->Inputs[0], Depth + 1)
                 : std::nullopt;
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      return affine(*Op, Depth);
    default:
      return std::nullopt;
    }
  }

  std::optional<int64_t> affine(const MedOp &Op, int Depth) {
    if (Op.NumInputs != 2 || Op.Output.Size == 0)
      return std::nullopt;
    const MedVar &A = Op.Inputs[0];
    const MedVar &B = Op.Inputs[1];
    const bool Sub = Op.Opcode == NdOp::INT_SUB;
    if (B.isConst()) {
      if (A.Size != Op.Output.Size || B.Size > Op.Output.Size)
        return std::nullopt;
      auto Delta = detail::signedStackConstant(B);
      if (auto Base = stackOffset(A, Depth + 1); Base && Delta)
        return detail::checkedStackOffset(*Base, *Delta, Sub);
      return std::nullopt;
    }
    if (A.isConst() && !Sub) {
      if (B.Size != Op.Output.Size || A.Size > Op.Output.Size)
        return std::nullopt;
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
