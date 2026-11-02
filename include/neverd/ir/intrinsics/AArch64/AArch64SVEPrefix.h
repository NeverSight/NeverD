//===- AArch64SVEPrefix.h - MOVPRFX/CPA pair admission --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate a raw MOVPRFX and its adjacent CPA consumer before admitting the
/// prefix's separate dataflow. A branch into the consumer skips that copy.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEPREFIX_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEPREFIX_H

#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace neverd::aarch64_sve {

#define NEVERD_A64_SVE_PREFIX_VALUE(Type, Name, Value)                         \
  inline constexpr Type Name = Value;
#define NEVERD_A64_SVE_PREFIX_TEXT(Name, Value)                                \
  inline constexpr char Name[] = Value;
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.def"

enum class PrefixForm {
#define NEVERD_A64_SVE_PREFIX_FORM(Name, ...) Name,
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.def"
};

struct PrefixOperation {
  Intrinsic Id;
  PrefixForm Form;
  unsigned Count;
  unsigned Vectors;
  unsigned Predicates;
};

inline std::optional<PrefixOperation> prefixOperation(Intrinsic Id) {
  switch (Id) {
#define NEVERD_A64_SVE_PREFIX_OPERATION(Id, Name, Form, Count, Vectors,        \
                                        Predicates)                            \
  case Intrinsic::Id:                                                          \
    return PrefixOperation{Intrinsic::Id, PrefixForm::Form, Count, Vectors,    \
                           Predicates};
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.def"
  default:
    return std::nullopt;
  }
}

struct CPAPrefix {
  PrefixForm Form;
  unsigned RawCount;
  unsigned Destination;
  unsigned Source;
  unsigned Predicate;
};

inline std::optional<CPAPrefix> cpaPrefix(uint32_t Word,
                                          std::optional<uint32_t> FollowingWord,
                                          uint64_t Address) {
  if (!FollowingWord || Address % kPrefixInstructionBytes ||
      Address > std::numeric_limits<uint64_t>::max() - kPrefixInstructionBytes)
    return std::nullopt;
  std::optional<CPAPrefix> Prefix;
#define NEVERD_A64_SVE_PREFIX_FORM(Name, Mask, Encoding, RawCount)             \
  if ((Word & Mask) == Encoding)                                               \
    Prefix = CPAPrefix{PrefixForm::Name, RawCount, Word & kCPARegisterMask,    \
                       (Word >> kCPASecondShift) & kCPARegisterMask,           \
                       (Word >> kCPAPredicateShift) & kCPAPredicateMask};
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.def"
  if (!Prefix)
    return std::nullopt;

  const uint32_t Consumer = *FollowingWord;
  for (const auto &Operation : kCPAOperations) {
    if ((Consumer & Operation.Mask) != Operation.Encoding)
      continue;
    const unsigned Destination = Consumer & kCPARegisterMask;
    const unsigned Second = (Consumer >> kCPASecondShift) & kCPARegisterMask;
    if (Operation.Form == CPAForm::Binary ||
        Destination != Prefix->Destination || Destination == Second)
      return std::nullopt;
    if (Operation.Form == CPAForm::Multiply) {
      if (Prefix->Form != PrefixForm::Unpredicated ||
          Destination == ((Consumer >> kCPAThirdShift) & kCPARegisterMask))
        return std::nullopt;
    } else if (Prefix->Form != PrefixForm::Unpredicated &&
               Prefix->Predicate !=
                   ((Consumer >> kCPAPredicateShift) & kCPAPredicateMask)) {
      return std::nullopt;
    }
    return Prefix;
  }
  // Other destructive SVE classes need their own complete consumer contract.
  return std::nullopt;
}

} // namespace neverd::aarch64_sve

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64SVEPREFIX_H
