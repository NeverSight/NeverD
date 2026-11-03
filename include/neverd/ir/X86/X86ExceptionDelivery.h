//===- X86ExceptionDelivery.h - Nested exception classification -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Intel IDT exception pairing shared by legacy and IA-32e execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86EXCEPTIONDELIVERY_H
#define NEVERD_IR_X86_X86EXCEPTIONDELIVERY_H

#include <cstdint>

namespace neverd {

enum class X86EventKind : uint8_t {
  Fault,
  Trap,
  Interrupt,
  NMI,
  Software,
  InstructionBreakpoint,
  Abort,
};

inline constexpr unsigned kX86FaultVectors[]{0,  1,  5,  6,  7,  10, 11,
                                             12, 13, 14, 16, 17, 19, 20};
inline constexpr unsigned kX86TrapVectors[]{1, 3, 4};
inline constexpr unsigned kX86AbortVectors[]{8, 18};
inline constexpr unsigned kX86ErrorCodeVectors[]{8, 10, 11, 12, 13, 14, 17};

constexpr bool x86EventHasErrorCode(X86EventKind Kind, uint8_t Vector) {
  if (Kind != X86EventKind::Fault && Kind != X86EventKind::Abort)
    return false;
  for (auto V : kX86ErrorCodeVectors)
    if (V == Vector)
      return true;
  return false;
}

constexpr bool x86ContributoryException(uint8_t Vector) {
  return Vector == 0 || (Vector >= 10 && Vector <= 13) || Vector == 21;
}

constexpr bool x86PageClassException(uint8_t Vector) {
  return Vector == 14 || Vector == 20;
}

constexpr bool x86DoubleFaultPair(uint8_t First, uint8_t Second) {
  return (x86ContributoryException(First) &&
          x86ContributoryException(Second)) ||
         (x86PageClassException(First) &&
          (x86ContributoryException(Second) || x86PageClassException(Second)));
}

/// Protected virtual-interrupt conflict is checked before fetching an opcode.
constexpr bool x86PendingVirtualInterrupt(uint8_t CPL, uint64_t CR4,
                                          uint64_t Flags) {
  return CPL == 3 && (CR4 & 2) && (Flags & 0x180000) == 0x180000;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X86EXCEPTIONDELIVERY_H
