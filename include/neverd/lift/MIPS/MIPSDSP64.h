//===- MIPSDSP64.h - Legacy packed GPR instruction forms -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIFT_MIPS_MIPSDSP64_H
#define NEVERD_LIFT_MIPS_MIPSDSP64_H

#include <cstdint>
#include <optional>

namespace neverd::mips {
enum class DSP64Operands {
  Binary,
  Compare,
  Unary,
  Reduce,
  ShiftReg,
  Shift3,
  Shift4,
  Shift5,
  Imm8,
  Imm10,
  Accumulator,
  ShiftAccumulator,
  ShiftAccumulatorReg
};
struct DSP64Form {
  const char *Mnemonic;
  uint32_t OperandTemplate;
  std::optional<int8_t> LastImmediate;
};
constexpr std::optional<DSP64Form> dsp64PackedForm(uint32_t Word) {
  if (Word >> 26 != 31)
    return std::nullopt;
  DSP64Operands Layout;
  DSP64Form Form;
  switch (Word & 0x7ff) {
#define NEVERD_MIPS_DSP64(Minor, Text, Operands, Template)                     \
  case Minor:                                                                  \
    Layout = DSP64Operands::Operands;                                          \
    Form = {Text, 0x7c000000u | Template};                                     \
    break;
#include "neverd/lift/MIPS/MIPSDSP64Forms.def"
  default:
    return std::nullopt;
  }
  uint32_t Reserved = 0;
  switch (Layout) {
  case DSP64Operands::ShiftAccumulator:
    Reserved = (7u << 16) | (28u << 11);
    break;
  case DSP64Operands::ShiftAccumulatorReg:
    Reserved = (31u << 16) | (28u << 11);
    break;
  case DSP64Operands::Accumulator:
    Reserved = 28u << 11;
    break;
  case DSP64Operands::Compare:
    Reserved = 31u << 11;
    break;
  case DSP64Operands::Unary:
    Reserved = 31u << 21;
    break;
  case DSP64Operands::Reduce:
    Reserved = 31u << 16;
    break;
  case DSP64Operands::Imm8:
  case DSP64Operands::Shift3:
    Reserved = 24u << 21;
    break;
  case DSP64Operands::Shift4:
    Reserved = 16u << 21;
    break;
  default:
    break;
  }
  if (Word & Reserved)
    return std::nullopt;
  Form.OperandTemplate |= Word & 0x03fff800;
  if (Layout == DSP64Operands::ShiftAccumulator) {
    const unsigned Shift = (Word >> 19) & 127;
    Form.OperandTemplate =
        0x7c0006b8u | (Word & (3u << 11)) | ((Shift & 63) << 20);
    Form.LastImmediate = Shift < 64 ? int64_t(Shift) : int64_t(Shift) - 128;
  }
  return Form;
}
} // namespace neverd::mips
#endif // NEVERD_LIFT_MIPS_MIPSDSP64_H
