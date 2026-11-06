//===- X64PagedExecution.h - Faultable generated steps ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute generated scalar and admitted legacy SSE steps, segment loads and
/// returns over explicit paged RAM.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEXECUTION_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEXECUTION_H

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"
#include "neverd/analysis/arch/x86_64/X64PagedPortIO.h"
#include "neverd/analysis/arch/x86_64/X64PagedSource.h"
#include "neverd/ir/X86/X86DebugRules.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace neverd::analysis {

using X64PagedBody = uint64_t (*)(void *State, void *Exchange);

/// Elementwise repeated memory strings with destination-first CMPS reads.
/// Addr32 LODS entry has distinct
/// observed RCX-high behavior on the qualified hybrid core types, including
/// zero counts and first-element faults. Neither
/// policy is inferred from the host running the generated source.
enum class X64PagedRepeatProfile : uint8_t {
  ClearLodsCountHigh,
  PreserveLodsCountHigh,
};

/// MOVUPS/MOVDQU #AC behavior is processor dependent. The embedding supplies
/// the target's behavior, independently of the scalar #AC/#PF priority.
enum class X64PagedSseAlignment : uint8_t {
  NoCheck,
  Qword,
  FullOperand,
};

/// Explicit nontransactional debug bank. The selected profile admits either
/// inactive DR7 or canonical execution comparators, with DEBUGCTL zero.
/// Data/I/O watchpoints, GD and BTF need their own producer. This bank is
/// disjoint from V6 state and guest RAM and outlives its execution context.
/// External event producers own their own DR6 causes.
struct X64PagedDebugState {
  uint64_t DR6 = 0xffff0ff0;
  uint64_t DR7 = 0x400;
  uint64_t DebugCTL = 0;
  std::array<uint64_t, 4> DR{};
};

struct X64PagedExecutionOptions {
  /// Required when CPL3 alignment checking is active. Absence refuses the
  /// entry before fetch or RAM effects instead of inferring a host CPU policy.
  std::optional<X64PagedAlignmentPriority> AlignmentPriority;
  /// Required before executing IRET's return-frame accesses. Authentication
  /// and fetch still precede this instruction-specific runtime binding.
  std::optional<X64PagedIretAccessOrder> IretAccessOrder;
  /// Required before RETF accesses its frame. Includes the target processor's
  /// order of compatibility-IP truncation and canonical-address validation.
  std::optional<X64PagedFarReturnProfile> FarReturnProfile;
  /// Required before LSS/LFS/LGS reads either pointer field or descriptor.
  std::optional<X64PagedFarPointerProfile> FarPointerProfile;
  /// Required before selector MOV/PUSH/POP effects. MOV SS also requires the
  /// generated scheduler's persistent interruptibility cells.
  std::optional<X64PagedSegmentMoveProfile> SegmentMoveProfile;
  /// Each completed string element consumes one attempt; zero count consumes
  /// one completion attempt. Earlier elements survive later faults/stops.
  std::optional<X64PagedRepeatProfile> RepeatProfile;
  /// Required even for a zero-count port string. No host ports are accessed.
  std::optional<X64PagedPortIOBinding> PortIO;
  /// Required only for MOVUPS/MOVDQU memory operands with CPL3/AM/AC active.
  /// Register forms and supervisor operands do not require this binding.
  std::optional<X64PagedSseAlignment> SseAlignment;
  X64PagedDebugState *Debug = nullptr;
  X86InstructionDebugProfile DebugProfile =
      X86InstructionDebugProfile::InstructionStep;
};

struct X64PagedExecutionResult {
  uint64_t Exit = uint64_t(X64ContinuationExitV1::InvalidState);
  bool InstructionAttempted = false;
  /// A completed instruction remains complete if its subsequent #DB entry
  /// fails. Software INT requires successful entry; intermediate REP is false.
  bool InstructionCompleted = false;
  /// One irreversible device transfer, also retained on an INS guest fault.
  bool DeviceTransferCompleted = false;
  /// Bytes committed by this INS element, including a prefix on a later #PF.
  uint8_t PortInputBytesWritten = 0;
  std::optional<translate::X86MemoryException> Exception;
  std::optional<translate::GuestMemoryFault> BackingFault;
  std::optional<X64PagedEventResult> Delivery;
};

/// One confined CPU and owned ordinary RAM, without external writes while a
/// step executes. Prepared stores are committed only after the complete
/// instruction succeeds. IRET/RETF retain ordered descriptor writes, and IRET
/// also retains NMI unblocking on failure. LSS/LFS/LGS stage both destinations
/// until their pointer reads, descriptor checks and accessed writes succeed.
/// Selector MOV/PUSH/POP share those checks and stage stack updates too.
/// Memory string repeats commit one element and its restart state per step.
/// Unfinished comparisons retain entry flags. Port transfers use a separate
/// synchronous device binding; gather/XSAVE still need progress bindings.
/// Generated bodies/validators and original bytes belong to the
/// same source export; passing unrelated or arbitrary function pointers
/// violates this ABI.
class X64PagedExecution final {
public:
  explicit X64PagedExecution(translate::X86PagedMemory &Memory,
                             const X64PagedExecutionOptions &Options = {})
      : Memory(Memory), Options(Options) {}

  /// Shared admission before producer polling or instruction fetch. Neither
  /// the generated validator nor this check touches guest RAM.
  llvm::Expected<bool> validate(X64ContinuationStateV6 &State, uint64_t Address,
                                X64PagedBody Validate,
                                unsigned PagingAddressBits,
                                bool InstructionDebug = false);

  llvm::Expected<X64PagedExecutionResult>
  deliver(X64ContinuationStateV6 &State, const X64ContinuationEventV1 &Event,
          uint32_t MXCSRMask);

  /// Resume lastResult().Delivery->Continuation after repairing backing.
  /// The instruction/cause and any external producer acceptance stay consumed.
  llvm::Expected<X64PagedExecutionResult>
  resume(X64ContinuationStateV6 &State, X64PagedEventContinuation Continuation,
         uint32_t MXCSRMask);

  llvm::Expected<X64PagedExecutionResult>
  execute(X64ContinuationStateV6 &State, uint64_t Address,
          llvm::ArrayRef<uint8_t> Bytes, X64PagedBody Body,
          X64PagedBody Validate, uint32_t MXCSRMask, unsigned PagingAddressBits,
          std::optional<bool> DebugSSBlocked = std::nullopt);

  const X64PagedExecutionResult &lastResult() const { return Last; }
  const std::string &lastError() const { return Error; }
  /// C bridge retains diagnostic ownership in the supplied execution context.
  void recordError(llvm::Error Failure);

private:
  llvm::Error
  deliverEvent(X64ContinuationStateV6 &State,
               const X64ContinuationEventV1 &Event, uint32_t MXCSRMask,
               const X64PagedEventContinuation *Continuation = nullptr);
  llvm::Error deliverDebug(X64ContinuationStateV6 &State, bool SingleStep,
                           uint32_t MXCSRMask, uint8_t Breakpoints = 0);
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
/// Debug exports use distinct bridges so an older runtime cannot silently
/// omit a trap. The scheduler supplies its current MOV-SS shadow on each step.
extern "C" uint64_t neverd_x64_paged_debug_step_v1(
    uint8_t *Context, uint8_t *State, uint64_t Address, const uint8_t *Bytes,
    uint64_t Size, X64PagedBody Body, X64PagedBody Validate, uint64_t MXCSRMask,
    uint64_t PagingAddressBits, uint64_t SSBlocked);
extern "C" uint64_t
neverd_x64_paged_debug_validate_v1(uint8_t *Context, uint8_t *State,
                                   uint64_t Address, X64PagedBody Validate,
                                   uint64_t PagingAddressBits);
extern "C" uint64_t neverd_x64_paged_event_v1(uint8_t *Context, uint8_t *State,
                                              const uint8_t *Event,
                                              uint64_t MXCSRMask);
extern "C" uint64_t neverd_x64_paged_progress_v1(uint8_t *Context);

} // namespace neverd::analysis
#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEXECUTION_H
