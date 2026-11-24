//===- AArch64CPA.h - Checked pointer arithmetic contracts -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scalar checked pointer arithmetic retains architectural control state and
/// poisoning behavior. It is not ordinary integer arithmetic.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_AARCH64_AARCH64CPA_H
#define NEVERD_IR_INTRINSICS_AARCH64_AARCH64CPA_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd {

#define NEVERD_A64_CPA_VALUE(Type, Name, Value)                                \
  inline constexpr Type Name = Value;
#define NEVERD_A64_CPA_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.def"

/// Result of PointerCheckAtEL with effective CPTA enabled. ProductOverflow
/// contributes only when the caller has also enabled the multiply check.
inline uint64_t aarch64CheckedPointerResult(uint64_t Base, uint64_t Result,
                                            bool ProductOverflow = false) {
  const bool PreviouslyDetected = bool(Base & kAddressMemorySignBit) !=
                                  bool(Base & kAddressMemoryPoisonBit);
  if (!ProductOverflow && !PreviouslyDetected &&
      !((Result ^ Base) & kAddressMemoryTagMask))
    return Result;
  return (Result & ~(kAddressMemoryPreservedMask | kAddressMemoryPoisonBit)) |
         (Base & kAddressMemoryPreservedMask) |
         (Base & kAddressMemorySignBit ? 0 : kAddressMemoryPoisonBit);
}

struct AArch64CPAOperation {
  Intrinsic Id;
  unsigned Sources;
  uint32_t Mask;
  uint32_t Encoding;
  const char *MachineAsm;

  const char *llvmConstraints() const {
    return Sources == 2 ? kAArch64CPAConstraints
                        : kAArch64CPAMultiplyConstraints;
  }
};

inline constexpr AArch64CPAOperation kAArch64CPAOperations[] = {
#define NEVERD_A64_CPA(Id, Insn, Mnemonic, Sources, Mask, Encoding,            \
                       MachineAsm)                                             \
  {Intrinsic::Id, Sources, Mask, Encoding, MachineAsm},
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.def"
};

inline std::optional<AArch64CPAOperation> aarch64CPAOperation(Intrinsic Id) {
  for (const auto &Operation : kAArch64CPAOperations)
    if (Operation.Id == Id)
      return Operation;
  return std::nullopt;
}

inline bool aarch64CPAShapeIsValid(Intrinsic Id, unsigned NumInputs,
                                   unsigned OutputBytes, unsigned FirstBytes,
                                   unsigned SecondBytes, unsigned ThirdBytes) {
  const auto Operation = aarch64CPAOperation(Id);
  return Operation && NumInputs == Operation->Sources + 1 &&
         OutputBytes == kAArch64CPAPointerBytes &&
         FirstBytes == kAArch64CPAPointerBytes &&
         SecondBytes == kAArch64CPAPointerBytes &&
         (Operation->Sources == 2 || ThirdBytes == kAArch64CPAPointerBytes);
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_AARCH64_AARCH64CPA_H
