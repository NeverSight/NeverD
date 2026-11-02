//===- X64PagedExecution.h - Faultable generated steps ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute a generated scalar step with an explicit paged-RAM transaction.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEXECUTION_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEXECUTION_H

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::analysis {

using X64PagedBody = uint64_t (*)(void *State, void *Exchange);

/// Canonical-address checks precede either order. Concurrent data-fault
/// priority is processor-specific, not specified by the boundary-event table.
enum class X64PagedAlignmentPriority : uint8_t { BeforePaging, AfterPaging };

struct X64PagedExecutionOptions {
  /// Required when CPL3 alignment checking is active. Absence refuses the
  /// entry before fetch or RAM effects instead of inferring a host CPU policy.
  std::optional<X64PagedAlignmentPriority> AlignmentPriority;
};

struct X64PagedExecutionResult {
  uint64_t Exit = uint64_t(X64ContinuationExitV1::InvalidState);
  bool InstructionAttempted = false;
  /// Includes successful software interrupt entry; a delivery fault is false.
  bool InstructionCompleted = false;
  std::optional<translate::X86MemoryException> Exception;
  std::optional<translate::GuestMemoryFault> BackingFault;
  std::optional<X64PagedEventResult> Delivery;
};

/// One confined CPU and owned ordinary RAM, without external writes while a
/// step executes. Prepared stores are committed only after the complete scalar
/// instruction succeeds. This is not a REP/gather/XSAVE partial-commit model.
/// Generated bodies/validators and original bytes belong to the same source
/// export; passing unrelated or arbitrary function pointers violates this ABI.
class X64PagedExecution final {
public:
  explicit X64PagedExecution(translate::X86PagedMemory &Memory,
                             const X64PagedExecutionOptions &Options = {})
      : Memory(Memory), Options(Options) {}

  /// Shared admission before producer polling or instruction fetch. Neither
  /// the generated validator nor this check touches guest RAM.
  llvm::Expected<bool> validate(X64ContinuationStateV6 &State, uint64_t Address,
                                X64PagedBody Validate,
                                unsigned PagingAddressBits);

  llvm::Expected<X64PagedExecutionResult>
  deliver(X64ContinuationStateV6 &State, const X64ContinuationEventV1 &Event,
          uint32_t MXCSRMask);

  llvm::Expected<X64PagedExecutionResult>
  execute(X64ContinuationStateV6 &State, uint64_t Address,
          llvm::ArrayRef<uint8_t> Bytes, X64PagedBody Body,
          X64PagedBody Validate, uint32_t MXCSRMask,
          unsigned PagingAddressBits);

  const X64PagedExecutionResult &lastResult() const { return Last; }
  const std::string &lastError() const { return Error; }
  /// C bridge retains diagnostic ownership in the supplied execution context.
  void recordError(llvm::Error Failure);

private:
  llvm::Error deliverEvent(X64ContinuationStateV6 &State,
                           const X64ContinuationEventV1 &Event,
                           uint32_t MXCSRMask);
  translate::X86PagedMemory &Memory;
  X64PagedExecutionOptions Options;
  X64PagedExecutionResult Last;
  std::string Error;
};

/// Source-only bridge. This is not part of NeverD's stable SDK C ABI.
extern "C" uint64_t neverd_x64_paged_access_v1(void *Exchange, uint64_t Address,
                                               uint64_t Bytes, uint64_t Flags);
extern "C" uint64_t
neverd_x64_paged_step_v1(uint8_t *Context, uint8_t *State, uint64_t Address,
                         const uint8_t *Bytes, uint64_t Size, X64PagedBody Body,
                         X64PagedBody Validate, uint64_t MXCSRMask,
                         uint64_t PagingAddressBits);
extern "C" uint64_t
neverd_x64_paged_validate_v1(uint8_t *Context, uint8_t *State, uint64_t Address,
                             X64PagedBody Validate, uint64_t PagingAddressBits);
extern "C" uint64_t neverd_x64_paged_event_v1(uint8_t *Context, uint8_t *State,
                                              const uint8_t *Event,
                                              uint64_t MXCSRMask);
extern "C" uint64_t neverd_x64_paged_progress_v1(uint8_t *Context);

} // namespace neverd::analysis
#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEXECUTION_H
