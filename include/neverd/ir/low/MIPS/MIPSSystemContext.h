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
  uint32_t HWREna = 0;
  std::optional<uint32_t> Status;
  std::optional<MIPSHardwareObservation> Hardware;
  bool ExecutionHazardPending = false;
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSSYSTEMCONTEXT_H
