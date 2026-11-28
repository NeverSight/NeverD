//===- IndexedReadOutput.h - Indexed read and return evidence ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_INDEXEDREADOUTPUT_H
#define NEVERD_LIB_SAFETY_INDEXEDREADOUTPUT_H

#include "StackSlotFlow.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/safety/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace neverd::safety::detail {

using ValueKey = std::tuple<uint8_t, int, int>;

inline ValueKey keyOf(const MedVar &V) {
  return {static_cast<uint8_t>(V.Kind), V.Id, V.SSAVer};
}

inline std::optional<uint64_t> unsignedConstant(const MedVar &Value) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t) ||
      isAddressProvenance(Value.Provenance))
    return std::nullopt;
  const unsigned Bits = static_cast<unsigned>(Value.Size) * 8;
  const uint64_t Mask = Bits == 64 ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t{1} << Bits) - 1;
  return Value.ConstVal & Mask;
}

// Match the exact SSA occurrence. Register id alone can denote different
// physical byte views, and a narrower occurrence cannot authenticate an
// address.
inline bool sameReadValue(const MedVar &A, const MedVar &B) {
  if (A.Kind == MedVar::Reg)
    return detail::sameRegisterOccurrence(A, B);
  return (A.Kind == MedVar::Temp || A.Kind == MedVar::Param) && A == B &&
         A.Size != 0 && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag;
}

// This whitelist is the transfer rule for a remembered write. Intrinsics,
// atomics, branches, calls and unknown effects end its scope.
inline bool preservesIndexedWrite(const MedOp &Op) {
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::CAST:
  case NdOp::SUBBYTES:
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_NOT:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::LOAD:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

// LowIR temporaries are reused between native instructions. Only follow the
// actual definition of this memory operand within its own instruction.
inline bool nativeIndexedLoad(const LowBlock &B, size_t At) {
  const LowOp &Load = B.Ops[At];
  const auto V = lowMemoryOperands(Load);
  if (!V.Address)
    return false;
  NdVar Address = *V.Address;
  for (unsigned Step = 0;
       At && Step < limits::safety::kIndexedReadMaxValueSteps; ++Step) {
    const LowOp &D = B.Ops[--At];
    if (D.Addr != Load.Addr)
      break;
    if (D.Output != Address)
      continue;
    if (D.Opcode == NdOp::COPY && D.NumInputs == 1) {
      Address = D.Inputs[0];
      continue;
    }
    if ((D.Opcode != NdOp::INT_ADD && D.Opcode != NdOp::INT_SUB) ||
        D.NumInputs != 2)
      break;
    if (!D.Inputs[0].isConst() && !D.Inputs[1].isConst())
      return true;
    if (D.Inputs[0].isConst() && D.Opcode == NdOp::INT_SUB)
      break;
    Address = D.Inputs[D.Inputs[0].isConst() ? 1 : 0];
  }
  return false;
}

struct IndexedEffects {
  int Last = -1, Previous = -1, BeforePrevious = -1, Barrier = -1;

  int lastStore() const { return Last > Barrier ? Last : -1; }

  void record(int Index, bool PlainStore) {
    BeforePrevious = Previous;
    Previous = Last;
    Last = Index;
    if (!PlainStore)
      Barrier = Index;
  }
};

struct IndexedReadBlock {
  const MedFunc &Func;
  const MedBlock &Block;
  const llvm::DenseMap<ValueKey, std::pair<int, int>> &Definitions;
  const llvm::DenseSet<ValueKey> &Ambiguous;

  const MedOp *definition(const MedVar &V, int &Before) const {
    if (V.isConst())
      return nullptr;
    const auto It = Definitions.find(keyOf(V));
    if (It == Definitions.end() || &Func.Blocks[It->second.first] != &Block ||
        It->second.second >= Before)
      return nullptr;
    const MedOp &D = Block.Ops[It->second.second];
    if (!sameReadValue(D.Output, V))
      return nullptr;
    Before = It->second.second;
    return &D;
  }

  struct Address {
    MedVar Base;
    int64_t Offset = 0;
    const MedOp *Index = nullptr;
    int Before = 0;
    bool Exact = true;
  };

  // Peel exact-width copies and fixed byte displacements. Stop at the first
  // runtime add/subtract so the caller can prove its index separately.
  std::optional<Address> address(MedVar V, int Before,
                                 bool Inventory = false) const {
    int64_t Offset = 0;
    bool Exact = true;
    for (unsigned I = 0; I < limits::safety::kIndexedReadMaxValueSteps; ++I) {
      const MedOp *D = definition(V, Before);
      if (!D)
        return Address{V, Offset, nullptr, Before, Exact};
      if (D->Opcode == NdOp::COPY && D->NumInputs == 1 &&
          D->Inputs[0].Size == V.Size) {
        if (sameReadValue(D->Inputs[0], V))
          return Address{V, Offset, nullptr, Before, Exact};
        V = D->Inputs[0];
        continue;
      }
      // A width-changing view may retain evidence of indexing without
      // preserving the address. Never use that history to prove a write.
      if (Inventory && (lowBytes(*D) || ((D->Opcode == NdOp::INT_ZEXT ||
                                          D->Opcode == NdOp::INT_SEXT) &&
                                         D->NumInputs == 1))) {
        Exact = false;
        V = D->Inputs[0];
        continue;
      }
      const bool Sub = D->Opcode == NdOp::INT_SUB;
      if ((!Sub && D->Opcode != NdOp::INT_ADD) || D->NumInputs != 2)
        return Address{V, Offset, nullptr, Before, Exact};
      const MedVar *Base = &D->Inputs[0], *Delta = &D->Inputs[1];
      if (!Sub && Base->isConst())
        std::swap(Base, Delta);
      if (!Delta->isConst())
        return Address{V, Offset, D, Before, Exact};
      const auto Fixed = detail::signedStackConstant(*Delta);
      if (!Fixed || Base->Size != V.Size || Delta->Size != V.Size)
        return std::nullopt;
      const auto Next = detail::checkedStackOffset(Offset, *Fixed, Sub);
      if (!Next)
        return std::nullopt;
      Offset = *Next;
      V = *Base;
    }
    return std::nullopt;
  }

  bool sameAddress(const MedVar &A, int AtA, const MedVar &B, int AtB) const {
    if (A.Size != B.Size)
      return false;
    if (sameReadValue(A, B))
      return !Ambiguous.count(keyOf(A));
    const auto Left = address(A, AtA), Right = address(B, AtB);
    return Left && Right && Left->Offset == Right->Offset &&
           sameReadValue(Left->Base, Right->Base) &&
           !Ambiguous.count(keyOf(Left->Base));
  }

  // The replacement store can rule out a data path through memory. Its
  // returned address must also be independent of the read being classified.
  std::optional<int> pointerBaseDefinition(const MedVar &V, int At) const {
    const auto Ptr = address(V, At);
    if (!Ptr || Ambiguous.count(keyOf(Ptr->Base)))
      return std::nullopt;
    const auto It = Definitions.find(keyOf(Ptr->Base));
    if (It == Definitions.end()) {
      if (Ptr->Base.Kind == MedVar::Reg || Ptr->Base.Kind == MedVar::Param)
        return -1;
      return std::nullopt;
    }
    const auto [Bi, Oi] = It->second;
    const MedBlock &DefBlock = Func.Blocks[Bi];
    if (!sameReadValue(DefBlock.Ops[Oi].Output, Ptr->Base))
      return std::nullopt;
    return &DefBlock == &Block ? Oi : -1;
  }

  static bool lowBytes(const MedOp &D) {
    return D.Opcode == NdOp::SUBBYTES && D.NumInputs == 2 &&
           unsignedConstant(D.Inputs[1]) == 0 &&
           D.Output.Size <= D.Inputs[0].Size;
  }

  static bool copyOrExtend(const MedOp &D) {
    if (D.NumInputs != 1)
      return false;
    if (D.Opcode == NdOp::COPY || D.Opcode == NdOp::CAST)
      return D.Output.Size == D.Inputs[0].Size;
    return D.Opcode == NdOp::INT_ZEXT && D.Output.Size >= D.Inputs[0].Size;
  }

  struct Range {
    int64_t Min, Max;
  };

  std::optional<Range> indexRange(MedVar V, int Before) const {
    bool SameWidth = true;
    for (unsigned I = 0; I < limits::safety::kIndexedReadMaxValueSteps; ++I) {
      if (!V.Size || V.Size > sizeof(uint64_t))
        break;
      const MedOp *D = definition(V, Before);
      if (!D)
        break;
      if ((D->Opcode == NdOp::INT_AND || D->Opcode == NdOp::INT_OR) &&
          D->NumInputs == 2 && D->Inputs[0].Size == V.Size &&
          D->Inputs[1].Size == V.Size) {
        const MedVar &C = D->Inputs[D->Inputs[1].isConst() ? 1 : 0];
        if (D->Opcode == NdOp::INT_AND) {
          if (const auto M = unsignedConstant(C);
              M && *M <= uint64_t(std::numeric_limits<int64_t>::max()))
            return Range{0, static_cast<int64_t>(*M)};
        } else if (SameWidth) {
          // A set sign bit and fixed high bits bound a backwards index.
          // Widening or truncating the OR result changes that interpretation.
          if (const auto M = detail::signedStackConstant(C); M && *M < 0)
            return Range{*M, -1};
        }
        break;
      }
      if (!copyOrExtend(*D) && !lowBytes(*D))
        break;
      SameWidth &= D->Inputs[0].Size == V.Size;
      V = D->Inputs[0];
    }
    return std::nullopt;
  }

  bool written(const Address &Read, uint16_t Bytes, int StoreIdx) const {
    if (!Read.Exact || !Read.Index || StoreIdx < 0)
      return false;
    const MedOp &Store = Block.Ops[StoreIdx];
    const MedVar *Dest = detail::memoryAddress(Store);
    const MedVar *Value = detail::storedValue(Store);
    if (Store.MemoryOrdering != NdMemoryOrdering::None || !Dest || !Value ||
        !Value->isConst() || !Bytes || Value->Size < Bytes ||
        Value->Size > sizeof(Value->ConstVal) || Dest->Size != Read.Base.Size)
      return false;
    const auto Origin = address(*Dest, StoreIdx);
    if (!Origin)
      return false;
    const MedOp &D = *Read.Index;
    const bool Sub = D.Opcode == NdOp::INT_SUB;
    for (unsigned J = 0; J < (Sub ? 1u : 2u); ++J) {
      const MedVar &Index = D.Inputs[1 - J];
      if (D.Inputs[J].Size != Dest->Size || Index.Size != Dest->Size)
        continue;
      const auto Base = address(D.Inputs[J], Read.Before);
      if (!Base || !sameReadValue(Base->Base, Origin->Base) ||
          Ambiguous.count(keyOf(Base->Base)))
        continue;
      auto Bias =
          detail::checkedStackOffset(Base->Offset, Origin->Offset, true);
      if (Bias)
        Bias = detail::checkedStackOffset(*Bias, Read.Offset, false);
      const uint64_t Limit = Value->Size - Bytes;
      const auto R = indexRange(Index, Read.Before);
      if (!Bias || !R)
        continue;
      const auto First =
          detail::checkedStackOffset(*Bias, Sub ? R->Max : R->Min, Sub);
      const auto Last =
          detail::checkedStackOffset(*Bias, Sub ? R->Min : R->Max, Sub);
      if (First && Last && *First >= 0 && *Last >= *First &&
          uint64_t(*Last) <= Limit)
        return true;
    }
    return false;
  }

  // Identify a whole loaded value transported in the low bytes of the final
  // return operand or final store. Truncation can turn this proof into unknown.
  const MedOp *loadOrigin(MedVar V, int Before, uint16_t Available) const {
    for (unsigned I = 0; I < limits::safety::kIndexedReadMaxValueSteps; ++I) {
      Available = std::min(Available, V.Size);
      const MedOp *D = definition(V, Before);
      if (!D)
        return nullptr;
      if (D->Opcode == NdOp::LOAD)
        return V.Size && V.Size <= Available ? D : nullptr;
      if (!copyOrExtend(*D) && !lowBytes(*D))
        return nullptr;
      V = D->Inputs[0];
    }
    return nullptr;
  }
};

} // namespace neverd::safety::detail

#endif // NEVERD_LIB_SAFETY_INDEXEDREADOUTPUT_H
