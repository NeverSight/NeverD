//===- X86LegacyExecution.h - Protected legacy execution -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original-byte segment steps and IDT event entry over owned paged RAM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_X86LEGACYEXECUTION_H
#define NEVERD_TRANSLATE_X86_X86LEGACYEXECUTION_H

#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/translate/X86/X86LegacySegmentExecution.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::translate {

struct X86LegacyExecutionState {
  X86LegacySegmentState Core;
  uint32_t EIP = 0;
  uint32_t EFlags = 2;
  uint32_t CR2 = 0;
  uint32_t IDTRBase = 0;
  uint16_t IDTRLimit = 0;
  X86LegacySegmentCache TR;
  bool STIBlocked = false;
  bool SSBlocked = false;
  bool NMIBlocked = false;
};

using X86LegacyEventKind = X86EventKind;

struct X86LegacyEvent {
  uint32_t SavedEIP = 0;
  uint32_t ErrorCode = 0;
  uint32_t FaultAddress = 0;
  uint8_t Vector = 0;
  X86LegacyEventKind Kind = X86LegacyEventKind::Fault;
};

/// One CPU, ordinary RAM, 16/32-bit gates, qword accessed writes, offset-first
/// TSS reads and ordered frame pushes. A 16-bit target stack preserves old
/// ESP high bits. Consecutive MOV/POP SS does not extend an incoming shadow.
/// Target IP-limit validation follows the frame stores. Recursive #DF retains
/// the failed fault's saved RF image; its saved PC/caches use the original
/// state. These #DF choices bind otherwise architecturally undefined program
/// state. The producer owns event selection, APIC/debug state and machine-check
/// recovery. Task gates, VM86 and CET need separate bindings.
enum class X86LegacyEventProfile : uint8_t { OrderedFrame };

/// IRET reads FLAGS/CS/IP, RETF reads CS/IP; descriptor accessed writes precede
/// outer SS/SP reads and late target-limit checks. CS/SS accesses cover only
/// their selector word, independently of the frame slot width. Ordinary RAM,
/// one CPU.
enum class X86LegacyReturnProfile : uint8_t { OrderedFrame };

/// Confined ordinary RAM. POP memory uses incremented ESP, with SS.B-sized
/// stack advancement preserving high ESP on a word wrap.
enum class X86LegacyScalarProfile : uint8_t { OrderedRAM };

/// Both choices check stack access before the target limit, retaining page
/// effects on a later fault. Only StoreBeforeTargetLimit publishes the return
/// address before that check. Architectural registers restart in either case.
enum class X86LegacyCallProfile : uint8_t {
  StoreBeforeTargetLimit,
  CheckBeforeTargetLimit,
};

struct X86LegacyExecutionOptions {
  X86LegacySegmentExecutionOptions Segments;
  std::optional<X86LegacyEventProfile> Events;
  /// Required for NMI eligibility under STI when no other blocker decides it;
  /// does not affect MOV/POP SS's NMI inhibition.
  std::optional<bool> NMIBlockedBySTI;
  std::optional<X86LegacyReturnProfile> Returns;
  /// Explicit value of architecturally undefined AF after AND/OR/XOR/TEST.
  /// Required only for these instructions; faults preserve incoming flags.
  std::optional<bool> LogicalAuxiliaryCarry;
  std::optional<X86LegacyScalarProfile> Scalars;
  std::optional<X86LegacyCallProfile> Calls;
};

enum class X86LegacyExecutionStatus : uint8_t {
  Completed,
  EventDelivered,
  EventDeferred,
  CodeChanged,
  BackingFault,
  Shutdown,
};

struct X86LegacyExecutionResult {
  X86LegacyExecutionStatus Status = X86LegacyExecutionStatus::Completed;
  bool InstructionAttempted = false;
  bool InstructionCompleted = false;
  bool EventAccepted = false;
  uint8_t DeliveredVector = 0;
  unsigned DeliveryAttempts = 0;
  std::optional<X86MemoryException> Exception;
  std::optional<GuestMemoryFault> BackingFault;
  std::vector<SelfModificationExit> SelfModifications;
};

/// Persistent shadows determine eligibility before any table access. Fault
/// delivery retains CR2, NMI blocking, descriptor writes and earlier frame
/// stores, including on a subsequent #DF/shutdown. Registers/cache/EIP commit
/// together only after a complete entry. No handler instructions are fetched.
llvm::Expected<X86LegacyExecutionResult>
deliverX86LegacyEvent(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                      const X86LegacyEvent &Event,
                      const X86LegacyExecutionOptions &Options);

/// Authenticates original bytes through CS and paging, then executes the
/// segment-transition owner, the explicit ordinary-RAM scalar owner, or
/// NOP/CLI/STI/INT/INT3/INTO/INT1/UD2/IRET/RETF. Scalars include
/// MOV/MOVSX/MOVZX, LEA, ADD/ADC/SUB/SBB/CMP, AND/OR/XOR/TEST, INC/DEC/NEG/NOT,
/// PUSH/POP and near CALL/JMP/Jcc/RET. Register immediates retain their unbound
/// interface; other scalars require Scalars, CALL requires Calls, and logical
/// flags require an AF choice. On an architectural fault, enters the IDT with
/// the original EIP. On
/// success, commits fallthrough/branch state, clears RF and advances SS/STI
/// inhibition. Changed code returns CodeChanged at the first mismatch.
/// Unimplemented or malformed instructions return Error, never a synthesized
/// #UD or NOP. TF stepping and hardware debug event production are not supplied
/// here. Returns stop at their destination, preserve RF restored by IRETD, and
/// end SS/STI inhibition. IRET unblocks NMI after fetch, including on a later
/// fault; RETF preserves NMI blocking.
llvm::Expected<X86LegacyExecutionResult>
executeX86LegacyInstruction(X86LegacyExecutionState &State,
                            X86PagedMemory &Memory,
                            llvm::ArrayRef<uint8_t> Instruction,
                            const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86LEGACYEXECUTION_H
