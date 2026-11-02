//===- NativeOperands.h - Typed decoded native operands ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Typed decoded native operands.
//===----------------------------------------------------------------------===//

#pragma once
#include "neverd/decode/Decoder.h"

#include <optional>

namespace neverd {
struct NativeImmediate {
  unsigned Operand = 0;
  uint64_t Value = 0;
  unsigned Bits = 0;
};
struct NativeRegister {
  std::string Name;
  bool Read = false, Write = false;
};
struct NativeOperands {
  std::vector<NativeImmediate> Immediates;
  std::vector<NativeRegister> Registers;
};
/// Typed decoded operands, before classic text folding. Immediate bits are
/// logical operand bits, not the encoded immediate field's byte count.
NativeOperands describeNativeOperands(const Decoder &, const DecodedInsn &,
                                      Arch, bool IncludeRegisters = true);
/// The immediate of \p Operands that a client's operand \p Index denotes.
/// Clients count operands by the commas outside brackets, braces and
/// parentheses of the decoder's operand text. That is the decoder's own order
/// on x86; ARM and AArch64 register lists and shifts take commas of their
/// own, so there the `#value` the operand spells selects the immediate.
std::optional<NativeImmediate> nativeImmediateAt(const NativeOperands &Operands,
                                                 const DecodedInsn &, Arch,
                                                 unsigned Index);
} // namespace neverd
