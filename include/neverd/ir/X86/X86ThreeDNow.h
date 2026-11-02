//===- X86ThreeDNow.h - Owned packed MMX instruction contracts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native operations retain processor-specific arithmetic and the MMX/x87
/// transition. A packed value alone is not a complete instruction contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X86THREEDNOW_H
#define NEVERD_IR_X86_X86THREEDNOW_H

#include <cstdint>
#include <optional>

namespace neverd {
inline constexpr uint32_t X87ContextThreeDNow = 1U << 27;
inline constexpr uint32_t X87ContextStackReference = 1U << 28;
// The x87 original-ModRM field carries the trailing 3DNow opcode instead:
// MMX instructions do not update the x87 instruction/data pointers or FOP.
inline constexpr unsigned X87ContextThreeDNowOpcodeShift = 19;

struct X86ThreeDNowInstruction {
  const char *Name;
  bool ReadsDestination;
  bool Enhanced;
};

inline std::optional<X86ThreeDNowInstruction>
x86ThreeDNowInstruction(unsigned Opcode) {
  switch (Opcode) {
#define NEVERD_3DNOW(Name, Opcode, Reads, Enhanced)                            \
  case Opcode:                                                                 \
    return X86ThreeDNowInstruction{#Name, Reads, Enhanced};
#include "neverd/ir/X86/X86ThreeDNow.def"
#undef NEVERD_3DNOW
  default:
    return std::nullopt;
  }
}

constexpr unsigned x87ContextThreeDNowOpcode(uint32_t Selector) {
  return (Selector >> X87ContextThreeDNowOpcodeShift) & 0xff;
}
} // namespace neverd

#endif // NEVERD_IR_X86_X86THREEDNOW_H
