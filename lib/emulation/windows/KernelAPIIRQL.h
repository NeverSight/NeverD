//===- KernelAPIIRQL.h - Kernel routine IRQL contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Maximum caller IRQL for the supported Windows x64 kernel API inventory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELAPIIRQL_H
#define NEVERD_EMULATION_KERNELAPIIRQL_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::emulation {

/// Return the routine's outer caller-IRQL ceiling, or an error for an unknown
/// spelling. Conditional restrictions (pool residency, mapping access mode,
/// wait timeout and Unicode debug formats) remain with their semantic owners.
/// Dispatcher routines return the Windows x64 HIGH_LEVEL ceiling; their
/// authoritative, narrower validation remains in KernelDispatcher.
llvm::Expected<uint8_t> maximumKernelIRQL(llvm::StringRef Name);

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELAPIIRQL_H
