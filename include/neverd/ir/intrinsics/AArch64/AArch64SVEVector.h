//===- AArch64SVEVector.h - SVE vector representation ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Representation-neutral contracts for the supported SVE vector producers
/// and consumers. Elements describe operations, never the stored Z value.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEVECTOR_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEVECTOR_H

#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::aarch64_sve {

#define NEVERD_A64_SVE_VECTOR_VALUE(Type, Name, Value)                         \
  inline constexpr Type Name = Value;
#define NEVERD_A64_SVE_VECTOR_TEXT(Name, Text)                                 \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.def"

struct VectorOperation {
  unsigned OperandCount;
  unsigned ElementIndex;
  bool ProducesVector;
  unsigned VectorInputs;
  unsigned PredicateInputs;

  bool consumesVector(unsigned Index) const {
    return Index < OperandCount && (VectorInputs & (1u << Index));
  }
  bool consumesPredicate(unsigned Index) const {
    return Index < OperandCount && (PredicateInputs & (1u << Index));
  }
  bool consumesState(unsigned Index) const {
    return consumesVector(Index) || consumesPredicate(Index);
  }
};

inline std::optional<VectorOperation> vectorOperation(Intrinsic Id) {
  if (const auto CPA = cpaOperation(Id)) {
    const auto Roles = cpaRoles(CPA->Form);
    return VectorOperation{Roles.Count + 1, Roles.Count, true, Roles.Vectors,
                           Roles.Predicates};
  }
  if (const auto Prefix = prefixOperation(Id))
    return VectorOperation{Prefix->Count + 1, Prefix->Count, true,
                           Prefix->Vectors, Prefix->Predicates};
  switch (Id) {
#define NEVERD_A64_SVE_VECTOR_OPERATION(Id, Count, Element, Produces, Vectors, \
                                        Predicates)                            \
  case Intrinsic::Id:                                                          \
    return VectorOperation{Count, Element, Produces, Vectors, Predicates};
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.def"
  default:
    return std::nullopt;
  }
}

struct VectorOperand {
  unsigned Bytes = 0;
  std::optional<uint64_t> Constant;
};

struct VectorShape {
  unsigned NumOperands = 0;
  unsigned OutputBytes = 0;
  std::array<VectorOperand, kVectorMaxOperands> Operands;
};

inline bool isValidVectorShape(Intrinsic Id, const VectorShape &Shape) {
  const auto Operation = vectorOperation(Id);
  if (!Operation || Shape.NumOperands != Operation->OperandCount)
    return false;
  const auto &Ops = Shape.Operands;
  const auto &Element = Ops[Operation->ElementIndex];
  if (Element.Bytes != kElementOperandBytes || !Element.Constant ||
      !isValidElementBytes(*Element.Constant))
    return false;
  for (unsigned I = 0; I != Shape.NumOperands; ++I) {
    if (Operation->consumesState(I) && Ops[I].Constant)
      return false;
    if (Operation->consumesVector(I) && Ops[I].Bytes != kVectorStorageBytes)
      return false;
    if (Operation->consumesPredicate(I) &&
        Ops[I].Bytes != kPredicateStorageBytes)
      return false;
  }
  if (cpaOperation(Id) || prefixOperation(Id))
    return Shape.OutputBytes == kVectorStorageBytes &&
           *Element.Constant == kCPAElementBytes;
  if (Operation->ProducesVector) {
    if (Shape.OutputBytes != kVectorStorageBytes)
      return false;
    for (unsigned I = 0; I != Operation->ElementIndex; ++I)
      if (!isValidElementBytes(Ops[I].Bytes))
        return false;
    return true;
  }
  if (Ops[0].Bytes != kPredicateStorageBytes ||
      Ops[kVectorInputIndex].Bytes != kVectorStorageBytes || Ops[0].Constant ||
      Ops[kVectorInputIndex].Constant)
    return false;
  if (Id == Intrinsic::A64_SveLastb)
    return isValidElementBytes(Shape.OutputBytes) &&
           Shape.OutputBytes >= *Element.Constant;
  const auto &Store = Ops.back();
  return Shape.OutputBytes == 0 &&
         Ops[kVectorAddressInputIndex].Bytes == kVectorPointerBytes &&
         Store.Bytes == kElementOperandBytes && Store.Constant &&
         isValidElementBytes(*Store.Constant) &&
         *Store.Constant <= *Element.Constant &&
         (Id != Intrinsic::A64_SveStnt1 ||
          *Store.Constant == *Element.Constant);
}

} // namespace neverd::aarch64_sve

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEVECTOR_H
