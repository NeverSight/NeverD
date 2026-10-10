//===- X86PreservedState.h - Closed x64 opaque state set --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86PRESERVEDSTATE_H
#define NEVERD_LIFT_X86PRESERVEDSTATE_H

#include "neverd/ir/low/LowPreservedState.h"
#include "neverd/lift/X86Regs.h"

#include <array>

namespace neverd::x86reg {

struct OpaqueStateRange {
  enum class Space : uint8_t { Register, MXCSR };
  Space Location;
  uint64_t Offset;
  uint16_t FirstBit, BitCount;
};

/// Exact LegacyIntegerOpaqueV1 definition. MXCSR is named separately; its
/// Offset is zero, not a register-file offset. All ranges are conditional on
/// architectural availability. No reset value or entry constant is implied.
inline constexpr std::array<OpaqueStateRange, 50> LegacyIntegerOpaqueV1 = [] {
  std::array<OpaqueStateRange, 50> R{};
  for (unsigned I = 0; I != 32; ++I)
    R[I] = {OpaqueStateRange::Space::Register, vectorReg(I), 0, 512};
  for (unsigned I = 0; I != 16; ++I)
    R[32 + I] = {OpaqueStateRange::Space::Register, extendedGeneralReg(I), 0,
                 64};
  R[48] = {OpaqueStateRange::Space::Register, FPU_CW, 0, 16};
  R[49] = {OpaqueStateRange::Space::MXCSR, 0, 0, 16};
  return R;
}();

} // namespace neverd::x86reg

#endif
