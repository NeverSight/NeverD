//===- X86Interrupts.h - Architectural x86 interrupt vectors ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86INTERRUPTS_H
#define NEVERD_IR_INTRINSICS_X86_X86INTERRUPTS_H

#include <cstdint>

namespace neverd {

/// The x86 interrupt vectors NeverD gives a meaning (X86Interrupts.def).
enum class X86Interrupt : uint8_t {
#define X86_INTERRUPT(ID, VECTOR, NO_RETURN) ID = VECTOR,
#include "neverd/ir/intrinsics/X86/X86Interrupts.def"
};

/// True when `int Vector` names \p Interrupt; `int` reads only the low byte.
constexpr bool isX86Interrupt(uint64_t Vector, X86Interrupt Interrupt) {
  return (Vector & 0xFF) == static_cast<uint8_t>(Interrupt);
}

/// True when `int Vector` never returns to the next instruction.
constexpr bool isX86NoReturnInterrupt(uint64_t Vector) {
#define X86_INTERRUPT(ID, VECTOR, NO_RETURN)                                   \
  if (NO_RETURN && isX86Interrupt(Vector, X86Interrupt::ID))                   \
    return true;
#include "neverd/ir/intrinsics/X86/X86Interrupts.def"
  return false;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86INTERRUPTS_H
