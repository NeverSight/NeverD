//===- InterpreterMachineSourceABI.h - Recovery source layouts -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit source storage choices, independent of recovery and proof domains.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_INTERPRETERMACHINESOURCEABI_H
#define NEVERD_ANALYSIS_ARCH_X86_64_INTERPRETERMACHINESOURCEABI_H

#include <cstdint>
#include <optional>

namespace neverd::analysis {

enum class InterpreterMachineSourceABIKind : uint32_t {
  ScalarV1 = 0,
  QueriesV1 = 1,
  QueriesXMMV2 = 2,
};

struct InterpreterMachineSourceABIInfo {
  const char *Name;
  uint64_t Bytes;
};

/// Unknown enum values have no source layout. Sizes come from the owning
/// machine-state structures, never from a CLI or SDK's parallel definition.
std::optional<InterpreterMachineSourceABIInfo>
interpreterMachineSourceABIInfo(InterpreterMachineSourceABIKind Kind);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_INTERPRETERMACHINESOURCEABI_H
