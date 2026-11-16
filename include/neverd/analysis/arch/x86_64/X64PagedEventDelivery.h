//===- X64PagedEventDelivery.h - Paged events and returns ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run the shared V6 event semantics over owned paged RAM, including faults
/// during delivery and returns, double faults and architectural shutdown.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H

#include "neverd/analysis/arch/x86_64/X64ContinuationEvent.h"
#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/X86/X86DebugRules.h"
#include "neverd/ir/X86/X86SegmentDescriptorProfile.h"
#include "neverd/translate/X86/X86EventDeliveryOptions.h"
#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace neverd::analysis {

/// Runtime-owned bank, disjoint from continuation state and guest RAM.
using X64PagedDebugState = X86InstructionDebugState<uint64_t>;

/// Canonical-address checks precede either order. Concurrent data-fault
/// priority is processor-specific, not specified by the boundary-event table.
using X64PagedAlignmentPriority = translate::X86AlignmentPriority;

/// Bound processor observation for competing IRET stack faults. The qualified
/// order reads FLAGS, CS, IP, then SS and SP, and updates accessed descriptors
/// as qwords. CS/SS slots read only the selector word. This is not inferred
/// from the host or the ISA pseudocode. FlagsFirst reports #GP(selector) for
/// a noncanonical descriptor start or tail. FlagsFirstTailGPZero uses #GP(0)
/// when only the tail is noncanonical. Both validate the complete descriptor
/// range before page-table accesses.
enum class X64PagedIretAccessOrder : uint8_t {
  FlagsFirst,
  FlagsFirstTailGPZero,
};
/// All profiles read CS then IP, validate and update the code descriptor,
/// then read outer SS and SP. SelectorFirst checks the original saved IP for
/// canonicality; SelectorFirstTruncatedIP truncates a compatibility target
/// first. The qualified hybrid Intel host exhibits both behaviors on different
/// core types, so this is an explicit target binding, never a host inference.
/// The two original profiles report #GP(selector) for any noncanonical
/// descriptor byte. SelectorFirstTailGPZero preserves the original-IP order
/// and uses #GP(0) when only the descriptor tail is noncanonical. Every profile
/// validates the complete descriptor range before its page-table accesses.
enum class X64PagedFarReturnProfile : uint8_t {
  SelectorFirst,
  SelectorFirstTruncatedIP,
  SelectorFirstTailGPZero,
};

/// Bound order for LSS/LFS/LGS: read offset, then selector, then validate and
/// update the descriptor as a qword. Descriptor reads precede RPL checks.
/// Qualified on both local hybrid core types, never inferred from the host.
/// OffsetFirst reports #GP(selector) for any noncanonical byte of the
/// eight-byte descriptor; OffsetFirstTailGPZero reports #GP(0) when only the
/// tail is noncanonical. Both check the whole range before descriptor page
/// walks and report #GP(selector) for a noncanonical start.
enum class X64PagedFarPointerProfile : uint8_t {
  OffsetFirst,
  OffsetFirstTailGPZero,
};

/// Selector word, descriptor validation, then conditional qword accessed
/// update. POP reads two bytes and publishes its two/eight-byte stack increment
/// only with the cache. PUSH stores the full two/eight-byte zero-extended
/// selector before publishing its stack decrement, without reading a
/// descriptor. The scheduler starts MOV-SS inhibition unless an
/// incoming MOV-SS window is already active; STI does not prevent that start.
/// Processor observations require explicit target qualification.
/// SelectorFirst reports #GP(selector) for any noncanonical byte of the
/// eight-byte descriptor; SelectorFirstTailGPZero reports #GP(0) when only the
/// tail is noncanonical. Both check the whole range before descriptor page
/// walks and report #GP(selector) for a noncanonical start.
enum class X64PagedSegmentMoveProfile : uint8_t {
  SelectorFirst,
  SelectorFirstTailGPZero,
};

/// Default event descriptor binding: a noncanonical start reports
/// #GP(selector | EXT); a noncanonical tail reports #GP(EXT). The alternative
/// includes the selector at either boundary. Both check the entire eight-byte
/// range before paging. This is an explicit target choice, never host
/// detection.
inline constexpr auto kX64PagedEventDescriptorCanonicality =
    X86DescriptorCanonicalityProfile::SelectorForStart;

enum class X64PagedEventStatus : uint8_t {
  Delivered,
  Deferred,
  InvalidState,
  BackingFault,
  Shutdown,
  BudgetExhausted,
};

/// Pending event at a physical backing or work-budget stop, including a
/// secondary fault. Keep the architectural state and existing RAM effects;
/// repair backing if necessary and resume this record instead of re-executing
/// the instruction or reselecting the original event. An accepted event keeps
/// completed reads and skips completed writes: frame writes can alias earlier
/// descriptor/TSS inputs. A work-budget stop retains the next unaccepted event.
struct X64PagedEventContinuation {
  struct Access {
    uint64_t Address = 0;
    uint64_t Value = 0;
    uint8_t Bytes = 0;
    bool Write = false;
    /// A completed event descriptor read/modify/write. Requires Write, eight
    /// bytes and Value.A=1. Value is its committed image, also used by the
    /// hidden cache; replay must neither reload nor rewrite guest memory.
    bool DescriptorAccessed = false;
  };
  X64ContinuationEventV1 Event;
  uint64_t FaultPC = 0;
  std::optional<translate::X86MemoryException> LastException;
  llvm::SmallVector<Access> CompletedAccesses;
  uint8_t DataBreakpoints = 0;
  bool SoftwareCompleted = false;
  /// False at a work-budget boundary before the next event is accepted.
  /// Such a record has no completed accesses or speculative data matches.
  bool EventAccepted = true;
  /// Retained for every secondary event, including an unaccepted budget stop.
  X86DescriptorCanonicalityProfile DescriptorCanonicality =
      kX64PagedEventDescriptorCanonicality;
};

struct X64PagedEventResult {
  X64PagedEventStatus Status = X64PagedEventStatus::InvalidState;
  /// The original event passed admission and inhibition before its first IDT
  /// access. A secondary fault does not put that external event back in a
  /// queue.
  bool InputAccepted = false;
  /// Software entry committed even if a subsequent data #DB entry stops.
  bool SoftwareCompleted = false;
  uint8_t DeliveredVector = 0;
  unsigned DeliveryAttempts = 0;
  std::optional<translate::X86MemoryException> LastException;
  std::optional<translate::GuestMemoryFault> BackingFault;
  std::optional<X64PagedEventContinuation> Continuation;
};

/// Legacy IA-32e IDT entry with explicit RAM and control-register bindings.
/// Uses the same descriptor/frame semantics as continuation source emission.
/// System table accesses use implicit supervisor privilege; frame accesses use
/// the handler privilege. CR2 is published whenever #PF is detected, including
/// a fault while delivering #DF. Already completed descriptor/page-table/frame
/// RAM writes remain visible. Shutdown is reported, never implemented as reset.
/// An A=0 code descriptor uses one prepared eight-byte read/modify/write;
/// its post-translation image supplies the loaded cache. A=1 performs no write.
/// Options bounds entry attempts per call. BudgetExhausted preserves a
/// continuation without inventing a guest exception or executing the handler.
/// For Software events, State.PC identifies the original INT/INT3 while
/// Event.SavedPC supplies its successful fallthrough. Delivery faults restart
/// State.PC, not that fallthrough; the numeric vector does not imply an error
/// code or a fault exception class.
/// SavedPC is frame data even when it exceeds the interrupted CS limit or is
/// noncanonical. Handler targets retain their independent architectural checks.
///
/// This entry stops before executing the handler. It does not provide legacy
/// i386 task gates, FRED/CET entry, VMX event routing, SMM or SGX AEX. A #DF
/// frame selects the original saved PC from the architecturally undefined
/// state; its presence is not a proof that the interrupted program can be
/// resumed.
llvm::Expected<X64PagedEventResult>
deliverX64PagedEvent(X64ContinuationStateV6 &State,
                     translate::X86PagedMemory &Memory,
                     const X64ContinuationEventV1 &Event, uint32_t MXCSRMask,
                     X64PagedDebugState *Debug = nullptr,
                     X86InstructionDebugProfile DebugProfile =
                         X86InstructionDebugProfile::InstructionStep,
                     const translate::X86EventDeliveryOptions &Options = {},
                     X86DescriptorCanonicalityProfile DescriptorCanonicality =
                         kX64PagedEventDescriptorCanonicality);

/// Resume uses the descriptor binding retained in Continuation. A new work
/// budget does not replace the already accepted sequence's processor profile.
llvm::Expected<X64PagedEventResult>
resumeX64PagedEvent(X64ContinuationStateV6 &State,
                    translate::X86PagedMemory &Memory,
                    const X64PagedEventContinuation &Continuation,
                    uint32_t MXCSRMask, X64PagedDebugState *Debug = nullptr,
                    X86InstructionDebugProfile DebugProfile =
                        X86InstructionDebugProfile::InstructionStep,
                    const translate::X86EventDeliveryOptions &Options = {});

struct X64PagedInstructionTransitionResult {
  bool Completed = false;
  /// Completion, an architectural fault, or a retained instruction effect
  /// consumes an attempt. Page-table A/D effects alone do not. An ordinary
  /// operand or descriptor backing stop before instruction effects does not.
  bool InstructionAttempted = false;
  std::optional<translate::X86MemoryException> Exception;
  std::optional<translate::GuestMemoryFault> BackingFault;
};

namespace detail {
struct X64PagedReturnContinuation;
struct X64PagedSegmentContinuation;
} // namespace detail
/// Immutable progress outside the V6 ABI. Retain state and existing RAM effects
/// when repairing physical backing; use resume rather than starting a return.
using X64PagedReturnContinuation =
    std::shared_ptr<const detail::X64PagedReturnContinuation>;

struct X64PagedReturnResult : X64PagedInstructionTransitionResult {
  X64PagedReturnContinuation Continuation;
};
using X64PagedInterruptReturnResult = X64PagedReturnResult;

/// Accepted segment-load accesses, including an empty prefix before the first
/// operand. Reads and page-walk effects alone leave its attempt unconsumed.
using X64PagedSegmentContinuation =
    std::shared_ptr<const detail::X64PagedSegmentContinuation>;
struct X64PagedSegmentResult : X64PagedInstructionTransitionResult {
  X64PagedSegmentContinuation Continuation;
};

/// Execute an authenticated 64-bit-mode IRET/IRETD/IRETQ after instruction
/// fetch. Descriptor/page-table writes and NMI unblocking survive a later
/// fault; the returned register/segment state commits only on success. Fault
/// delivery belongs to the instruction executor. Compatibility upper halves
/// use the same explicit witnesses as the generated no-fault return.
llvm::Expected<X64PagedInterruptReturnResult>
returnX64PagedInterrupt(X64ContinuationStateV6 &State,
                        translate::X86PagedMemory &Memory, uint16_t FieldBytes,
                        X64PagedAlignmentPriority AlignmentPriority,
                        X64PagedIretAccessOrder AccessOrder);

/// Execute an authenticated 64-bit-mode RETF, optionally releasing Discard
/// bytes from both stacks on an outer return. NMI inhibition is retained.
/// Ordered descriptor writes survive faults; register/cache state commits on
/// success. A physical backing failure is an embedding stop, not a guest #PF.
llvm::Expected<X64PagedReturnResult>
returnX64PagedFar(X64ContinuationStateV6 &State,
                  translate::X86PagedMemory &Memory, uint16_t FieldBytes,
                  uint16_t Discard, X64PagedAlignmentPriority AlignmentPriority,
                  X64PagedFarReturnProfile Profile);

/// Check retained CPU, control and feature values without accessing RAM.
/// Refusal leaves the immutable record usable with its original bindings.
llvm::Error
validateX64PagedReturnResume(const X64ContinuationStateV6 &State,
                             const translate::X86PagedMemory &Memory,
                             const X64PagedReturnContinuation &Continuation);

/// Continue ordered accesses without repeating successful reads or writes.
/// InstructionAttempted reports only a newly consumed attempt on this call.
llvm::Expected<X64PagedReturnResult>
resumeX64PagedReturn(X64ContinuationStateV6 &State,
                     translate::X86PagedMemory &Memory,
                     const X64PagedReturnContinuation &Continuation);

/// Execute an authenticated 64-bit-mode LSS/LFS/LGS. Explicit operands use
/// the current CPL; descriptors use implicit supervisor privilege. Both
/// destinations commit together. Fault delivery belongs to the executor.
llvm::Expected<X64PagedSegmentResult>
loadX64PagedFarPointer(X64ContinuationStateV6 &State,
                       translate::X86PagedMemory &Memory,
                       llvm::ArrayRef<uint8_t> Instruction,
                       X64PagedAlignmentPriority AlignmentPriority,
                       X64PagedFarPointerProfile Profile,
                       std::optional<X64NullSegmentBasePolicy> NullBasePolicy);

/// Execute an authenticated long-mode selector MOV/PUSH/POP over explicit
/// state. The caller owns MOV-SS interruptibility; generated source uses its
/// persistent scheduler. Fault delivery and instruction-budget accounting
/// belong to the executor, not this descriptor/operand transition.
llvm::Expected<X64PagedSegmentResult>
moveX64PagedSegment(X64ContinuationStateV6 &State,
                    translate::X86PagedMemory &Memory,
                    llvm::ArrayRef<uint8_t> Instruction,
                    X64PagedAlignmentPriority AlignmentPriority,
                    X64PagedSegmentMoveProfile Profile,
                    std::optional<X64NullSegmentBasePolicy> NullBasePolicy);

/// Check the complete retained state and paging bindings without RAM access.
/// A refused resume leaves the immutable continuation available for repair.
llvm::Error
validateX64PagedSegmentResume(const X64ContinuationStateV6 &State,
                              const translate::X86PagedMemory &Memory,
                              const X64PagedSegmentContinuation &Continuation);

/// Resume the original MOV/PUSH/POP/LxS, without fetching its bytes or reading
/// completed operands again. Only a newly consumed attempt is reported.
llvm::Expected<X64PagedSegmentResult>
resumeX64PagedSegment(X64ContinuationStateV6 &State,
                      translate::X86PagedMemory &Memory,
                      const X64PagedSegmentContinuation &Continuation);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H
