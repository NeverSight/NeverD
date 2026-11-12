//===- MIPSDSP.h - DSP control field contracts ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Interpret selector groups independently of the target memory byte order.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_INTRINSICS_MIPS_MIPSDSP_H
#define NEVERD_IR_INTRINSICS_MIPS_MIPSDSP_H

#include <cstdint>
#include <optional>

namespace neverd::mips {

enum class DSPInstruction {
#define NEVERD_MIPS_DSP_INSTRUCTION(Name, Mask, Value) Name,
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.def"
};

constexpr std::optional<DSPInstruction> dspInstruction(uint32_t Word) {
#define NEVERD_MIPS_DSP_INSTRUCTION(Name, Mask, Value)                         \
  if ((Word & Mask) == Value)                                                  \
    return DSPInstruction::Name;
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.def"
  return std::nullopt;
}

constexpr uint32_t dspControlMask(unsigned Selector, bool Wide = false) {
  uint32_t Result = 0;
#define NEVERD_MIPS_DSP_FIELD(Name, Select, Bits)                              \
  if (Selector & Select)                                                       \
    Result |= Bits;
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.def"
  if (Wide && (Selector & 1))
    Result |= 1u << 6;
  if (Wide && (Selector & 16))
    Result |= 0xf0000000u;
  return Result;
}

inline constexpr uint32_t DSPControlBits = dspControlMask(63);
inline constexpr uint32_t DSPByteOverflow = uint32_t{1} << 20;

} // namespace neverd::mips

#endif // NEVERD_IR_INTRINSICS_MIPS_MIPSDSP_H
