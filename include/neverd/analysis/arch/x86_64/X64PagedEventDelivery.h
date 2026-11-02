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
#include "neverd/translate/X86/X86PagedMemory.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::analysis {

/// Canonical-address checks precede either order. Concurrent data-fault
/// priority is processor-specific, not specified by the boundary-event table.
enum class X64PagedAlignmentPriority : uint8_t { BeforePaging, AfterPaging };

/// Bound processor observation for competing IRET stack faults. The qualified
/// order reads FLAGS, CS, IP, then SS and SP, and updates accessed descriptors
/// as qwords. This is not inferred from the host or the ISA pseudocode.
enum class X64PagedIretAccessOrder : uint8_t { FlagsFirst };
/// Both profiles read CS then IP, validate and update the code descriptor,
/// then read outer SS and SP. SelectorFirst checks the original saved IP for
/// canonicality; SelectorFirstTruncatedIP truncates a compatibility target
/// first. The qualified hybrid Intel host exhibits both behaviors on different
/// core types, so this is an explicit target binding, never a host inference.
enum class X64PagedFarReturnProfile : uint8_t {
  SelectorFirst,
  SelectorFirstTruncatedIP,
};

/// Bound order for LSS/LFS/LGS: read offset, then selector, then validate and
/// update the descriptor as a qword. Descriptor reads precede RPL checks.
/// Qualified on both local hybrid core types, never inferred from the host.
enum class X64PagedFarPointerProfile : uint8_t { OffsetFirst };

/// Selector word, descriptor validation, then conditional qword accessed
/// update. POP reads two bytes and publishes its two/eight-byte stack increment
/// only with the cache. The scheduler starts MOV-SS inhibition unless an
/// incoming MOV-SS window is already active; STI does not prevent that start.
/// Processor observations require explicit target qualification.
enum class X64PagedSegmentMoveProfile : uint8_t { SelectorFirst };

enum class X64PagedEventStatus : uint8_t {
  Delivered,
  Deferred,
  InvalidState,
  BackingFault,
  Shutdown,
};

struct X64PagedEventResult {
  X64PagedEventStatus Status = X64PagedEventStatus::InvalidState;
  /// The original event passed admission and inhibition before its first IDT
  /// access. A secondary fault does not put that external event back in a
  /// queue.
  bool InputAccepted = false;
  uint8_t DeliveredVector = 0;
  unsigned DeliveryAttempts = 0;
  std::optional<translate::X86MemoryException> LastException;
  std::optional<translate::GuestMemoryFault> BackingFault;
};

/// Legacy IA-32e IDT entry with explicit RAM and control-register bindings.
/// Uses the same descriptor/frame semantics as continuation source emission.
/// System table accesses use implicit supervisor privilege; frame accesses use
/// the handler privilege. CR2 is published whenever #PF is detected, including
/// a fault while delivering #DF. Already completed descriptor/page-table/frame
/// RAM writes remain visible. Shutdown is reported, never implemented as reset.
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
                     const X64ContinuationEventV1 &Event, uint32_t MXCSRMask);

struct X64PagedInstructionTransitionResult {
  bool Completed = false;
  /// Completion, an architectural fault, or a retained instruction effect
  /// consumes an attempt. Page-table A/D effects alone do not. An ordinary
  /// operand or descriptor backing stop before instruction effects does not.
  bool InstructionAttempted = false;
  std::optional<translate::X86MemoryException> Exception;
  std::optional<translate::GuestMemoryFault> BackingFault;
};

using X64PagedReturnResult = X64PagedInstructionTransitionResult;
using X64PagedInterruptReturnResult = X64PagedReturnResult;

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

/// Execute an authenticated 64-bit-mode LSS/LFS/LGS. Explicit operands use
/// the current CPL; descriptors use implicit supervisor privilege. Both
/// destinations commit together. Fault delivery belongs to the executor.
llvm::Expected<X64PagedInstructionTransitionResult>
loadX64PagedFarPointer(X64ContinuationStateV6 &State,
                       translate::X86PagedMemory &Memory,
                       llvm::ArrayRef<uint8_t> Instruction,
                       X64PagedAlignmentPriority AlignmentPriority,
                       X64PagedFarPointerProfile Profile,
                       std::optional<X64NullSegmentBasePolicy> NullBasePolicy);

/// Execute an authenticated long-mode selector MOV/POP over explicit state.
/// The caller owns MOV-SS interruptibility; generated source uses its
/// persistent scheduler. Fault delivery and instruction-budget accounting
/// belong to the executor, not this descriptor/operand transition.
llvm::Expected<X64PagedInstructionTransitionResult>
moveX64PagedSegment(X64ContinuationStateV6 &State,
                    translate::X86PagedMemory &Memory,
                    llvm::ArrayRef<uint8_t> Instruction,
                    X64PagedAlignmentPriority AlignmentPriority,
                    X64PagedSegmentMoveProfile Profile,
                    std::optional<X64NullSegmentBasePolicy> NullBasePolicy);

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64PAGEDEVENTDELIVERY_H
