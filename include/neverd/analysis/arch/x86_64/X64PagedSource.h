//===- X64PagedSource.h - Paged source ABI ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared source/runtime operand exchange and explicit imported-call ABI.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSOURCE_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSOURCE_H

#include "neverd/ir/SourceTypeHint.h"

#include <cstdint>

namespace neverd::analysis {

/// Continuation exits 0..16 retain their existing identity. AccessFault is an
/// internal body exit; the executor replaces it with delivered/shutdown/backing
/// status. The source body must only be called by this executor.
enum class X64PagedExit : uint64_t {
  AccessFault = 17,
  BackingFault = 18,
  Shutdown = 19,
  CodeChanged = 20,
};

/// Progress of the last runtime operation, independent of its final status.
/// A faulting instruction is charged even if subsequent entry hits a backing
/// failure. An accepted external event is consumed even on secondary faults.
inline constexpr uint64_t kX64PagedInstructionAttempted = 1;
inline constexpr uint64_t kX64PagedEventAccepted = 2;

// Synthetic source-only identity. Original instruction addresses cannot occupy
// the reserved final addresses used by continuation helpers.
inline constexpr uint64_t kX64PagedAccessAddress = UINT64_MAX - 1;
inline constexpr const char *kX64PagedAccessName = "neverd_x64_paged_access_v1";
inline constexpr uint64_t kX64PagedStackAccess = 8;
enum class X64PagedAccess : uint64_t {
  Load,
  Store,
  ReadModifyWrite,
  Fault,
  /// Address is the vector; the size argument transports the original next PC.
  SoftwareInterrupt,
  /// Address selects CLI (0) or STI (1); result is the authorized IF/VIF mask.
  InterruptFlagMask,
};

struct X64PagedExchange {
  uint64_t Value = 0;
  void *Transaction = nullptr;
};

SourceFunctionTypeHint x64PagedAccessSourceABI();

} // namespace neverd::analysis
#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSOURCE_H
