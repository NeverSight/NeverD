//===- MIPSSystemContext.h - Authenticated system observations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One-step MIPS32 observations. No interrupt delivery, CP0 timing or exception
/// entry is inferred. The caller authenticates privilege and register values.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H
#define NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H

#include <cstdint>
#include <optional>

namespace neverd {
struct MIPSHardwareObservation {
  uint8_t Index;
  uint32_t Value;
};
struct MIPSSystemContext {
  bool CP0Usable = false;
  bool ImplementedMT = false;
  uint32_t HWREna = 0;
#define NEVERD_MIPS_CP0(Index, Selector, Name, Writable, MT)                   \
  std::optional<uint32_t> Name;
#include "neverd/ir/intrinsics/MIPS/MIPSCP0.def"
  std::optional<MIPSHardwareObservation> Hardware;
  bool ExecutionHazardPending = false;
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H
