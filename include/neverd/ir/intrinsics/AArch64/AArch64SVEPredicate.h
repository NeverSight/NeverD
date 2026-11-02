//===- AArch64SVEPredicate.h - SVE predicate representation ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// P registers retain one bit per byte lane independently of the element
/// width of the operation that writes or consumes them.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEPREDICATE_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEPREDICATE_H

#include <cstdint>
#include <optional>

namespace neverd::aarch64_sve {

#define NEVERD_A64_SVE_PREDICATE_VALUE(Type, Name, Value)                      \
  inline constexpr Type Name = Value;
#define NEVERD_A64_SVE_PREDICATE_TEXT(Name, Text)                              \
  inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.def"

struct PtrueOperands {
  unsigned Predicate;
  unsigned Pattern;
  unsigned ElementBytes;
};

inline std::optional<PtrueOperands> decodePtrue(uint32_t Word) {
  if ((Word & kPtrueMask) != kPtrueEncoding)
    return std::nullopt;
  return PtrueOperands{
      Word & kPtrueRegisterMask,
      (Word >> kPtruePatternShift) & kPtruePatternMask,
      1U << ((Word >> kPtrueElementShift) & kPtrueElementMask)};
}

inline bool isValidElementBytes(uint64_t Bytes) {
  switch (Bytes) {
#define NEVERD_A64_SVE_PREDICATE_WIDTH(Bytes) case Bytes:
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.def"
    return true;
  default:
    return false;
  }
}

struct PtrueShape {
  unsigned NumOperands;
  unsigned OutputBytes;
  unsigned PatternBytes;
  unsigned ElementBytes;
  std::optional<uint64_t> Pattern;
  std::optional<uint64_t> Element;
};

inline bool isValidPtrueShape(const PtrueShape &Shape) {
  return Shape.NumOperands == kPtrueOperandCount &&
         Shape.OutputBytes == kPredicateStorageBytes &&
         Shape.PatternBytes == kPatternOperandBytes &&
         Shape.ElementBytes == kElementOperandBytes && Shape.Pattern &&
         *Shape.Pattern <= kPtruePatternMask && Shape.Element &&
         isValidElementBytes(*Shape.Element);
}

} // namespace neverd::aarch64_sve

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEPREDICATE_H
