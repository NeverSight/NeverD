//===- PowerPCVMXRegisters.h - VMX register storage ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_POWERPC_POWERPCVMXREGISTERS_H
#define NEVERD_LIFT_POWERPC_POWERPCVMXREGISTERS_H

#include <cstdint>

namespace neverd::ppc {
#define NEVERD_PPC_VMX_FEATURE(Name) inline constexpr char VMXFeature[] = Name;
#define NEVERD_PPC_VMX_VALUE(Name, Value)                                      \
  inline constexpr unsigned Name = Value;
#include "neverd/lift/PowerPC/PowerPCVMXRegisters.def"

constexpr uint64_t vr(unsigned Index) {
  return VectorBase + VectorBytes * Index;
}
} // namespace neverd::ppc

#endif // NEVERD_LIFT_POWERPC_POWERPCVMXREGISTERS_H
