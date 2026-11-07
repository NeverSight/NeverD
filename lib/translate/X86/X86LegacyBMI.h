//===- X86LegacyBMI.h - Protected VEX integer instructions ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// BMI byte admission and register effects; scalar execution owns memory.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYBMI_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYBMI_H

#include "X86LegacyEncoding.h"

#include "neverd/translate/X86/X86LegacyExecution.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

enum class X86LegacyBMIOperation {
  AndNot,
  Extract,
  IsolateLowest,
  MaskLowest,
  ResetLowest,
  ZeroHigh,
  Multiply,
  Deposit,
  Gather,
  RotateRight,
  SignedShiftRight,
  ShiftLeft,
  ShiftRight,
};

struct X86LegacyBMIInstruction {
  X86LegacyBMIOperation Operation;
  X86LegacyAddress Source;
  unsigned Destination = 0, VRegister = 0;
  uint8_t Immediate = 0;

  bool enabled(const X86LegacyIntegerExtensions &Features) const;
  uint32_t undefinedFlagsMask() const;
};

/// R has consumed C4. The caller checks the complete instruction extent.
std::optional<X86LegacyBMIInstruction> decodeLegacyBMI(X86LegacyEncoding &R);

/// Source is the completed dword operand read. Capture inputs before either
/// MULX destination changes; its high half wins when the destinations alias.
void executeLegacyBMI(const X86LegacyBMIInstruction &F,
                      std::array<uint32_t, 8> &GPR, uint32_t &Flags,
                      uint32_t Source, uint32_t UndefinedFlags);

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYBMI_H
