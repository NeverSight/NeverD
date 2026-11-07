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
  /// One REP element committed at the original PC; the region drivers consume
  /// an attempt and continue dispatch, allowing a budget stop or event poll.
  RepeatContinue = 21,
  /// No device effect or element completion; restore availability and retry.
  DeviceUnavailable = 22,
  /// Resume the retained event before executing another instruction.
  EventBudgetExhausted = 23,
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
/// Debug exports require the runtime that also owns authenticated MOV-DR.
inline constexpr const char *kX64PagedDebugAccessName =
    "neverd_x64_paged_debug_access_v1";
inline constexpr uint64_t kX64PagedStackAccess = 8;
/// Attach SSE preflight to the same single 16-byte operand callback.
inline constexpr uint64_t kX64PagedSseAccess = 32;
inline constexpr uint64_t kX64PagedRepeatSource = 1;
inline constexpr uint64_t kX64PagedRepeatDestination = 2;
inline constexpr uint64_t kX64PagedRepeatCompare = 4;
enum class X64PagedAccess : uint64_t {
  Load,
  Store,
  ReadModifyWrite,
  Fault,
  /// Address is the vector; the size argument transports the original next PC.
  SoftwareInterrupt,
  /// Address selects CLI (0) or STI (1); result is the authorized IF/VIF mask.
  InterruptFlagMask,
  /// Address is the authenticated IRET frame-field width; size is zero.
  InterruptReturn,
  /// Address is the RETF field width; size is the unsigned parameter discard.
  FarReturn,
  /// Original bytes come from the authenticated step. Address transports the
  /// source null-base policy: 0 unbound, 1 clear, 2 preserve; size is zero.
  /// Bit 3 remains the scalar stack-access flag, so new kinds start at 16.
  FarPointerLoad = 16,
  /// Selector MOV/PUSH/POP shares null-base policy and authenticated bytes.
  SegmentMove = 17,
  /// Require the REP profile before operands. Address is the index-use mask;
  /// size is the address width. Entry normalization survives guest faults.
  BeginRepeat = 18,
  /// Original bytes own scalar/string operands; address and size are zero.
  PortIO = 19,
  /// Legacy SSE register bitvectors; address and size are zero. Memory forms
  /// instead carry kX64PagedSseAccess on their existing operand callback.
  BeginSse = 20,
  /// ICEBP has completed at size (fallthrough); address is unused.
  DebugTrap = 21,
  /// Address transports the POPFQ image; result is its system-flags portion.
  PopFlags = 22,
  /// Authenticated original MOV-DR bytes are executed by the debug bank owner.
  DebugRegister = 23,
  /// Actual Jump predicate (0/1), independent of target equality. Size is zero.
  /// Bit 3 and bit 5 are access flags; neither may be part of a kind identity.
  /// Only debug v2 source bodies publish this observation.
  BranchDecision = 64,
};

struct X64PagedExchange {
  // Scalar bodies use only Value; legacy SSE uses the contiguous 16-byte
  // payload. The transaction pointer is never part of a generated value store.
  uint64_t Value = 0;
  uint64_t ValueHigh = 0;
  void *Transaction = nullptr;
};

SourceFunctionTypeHint x64PagedAccessSourceABI();

} // namespace neverd::analysis
#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDSOURCE_H
