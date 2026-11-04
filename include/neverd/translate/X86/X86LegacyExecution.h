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

/// Low 32-bit architectural inputs for non-IA-32e SYSENTER/SYSEXIT.
struct X86LegacySysenterState {
  uint32_t CS = 0;
  uint32_t ESP = 0;
  uint32_t EIP = 0;
};

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
  X86LegacySysenterState Sysenter;
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

inline constexpr uint32_t kX86LegacyArithmeticFlagsMask = 0x8d5;
inline constexpr uint32_t kX86LegacyShiftUndefinedFlagsMask = 0x811;

/// SHLD/SHRD consume Result and all arithmetic Flags only for a word count
/// above 16 after masking. Otherwise only undefined AF/OF consume Flags;
/// a zero count preserves the complete operand and flags.
struct X86LegacyDoubleShiftUndefinedState {
  uint16_t Result = 0;
  uint32_t Flags = 0;
};

/// Explicit successful MUL/IMUL undefined-flag choices. Both clear AF and
/// derive SF/PF from the low product. ProductSignParity clears ZF;
/// ProductSignParityZero derives it from the low product. CF/OF always follow
/// architectural overflow. A processor model is not inferred from the host.
enum class X86LegacyMultiplyProfile : uint8_t {
  ProductSignParity,
  ProductSignParityZero,
};

/// Both choices check stack access before the target limit, retaining page
/// effects on a later fault. Only StoreBeforeTargetLimit publishes the return
/// address before that check. Architectural registers restart in either case.
enum class X86LegacyCallProfile : uint8_t {
  StoreBeforeTargetLimit,
  CheckBeforeTargetLimit,
};

/// Intel SEP with explicit MSRs, no CET/FRED and no debug-event producer.
/// Transfers install fixed caches without reading descriptors or either stack.
enum class X86LegacyFastSystemProfile : uint8_t { IntelSEP };

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
  std::optional<X86LegacyFastSystemProfile> FastSystem;
  /// Explicit CF/PF/AF/ZF/SF/OF image after successful DIV/IDIV. No other
  /// bits are permitted. Faults preserve the original arithmetic flags.
  std::optional<uint32_t> DivisionArithmeticFlags;
  std::optional<X86LegacyMultiplyProfile> Multiply;
  /// Explicit CF/AF/OF image for undefined shift/rotate flags only. Defined
  /// flags and zero-count preservation never consume these bits. No other
  /// bits are permitted; faults retain all original flags.
  std::optional<uint32_t> ShiftUndefinedFlags;
  /// Required for SHLD/SHRD. Flags may contain only arithmetic flag bits.
  /// Faults retain the original operand and flags; no host policy is inferred.
  std::optional<X86LegacyDoubleShiftUndefinedState> DoubleShiftUndefined;
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
/// MUL/IMUL, DIV/IDIV, CBW/CWDE/CWD/CDQ, LAHF/SAHF, CLC/STC/CMC/CLD/STD,
/// BSWAP (dword), XLAT, LEAVE, SETcc/CMOVcc, LOOP/LOOPcc/JCXZ/JECXZ,
/// SHL/SAL/SHR/SAR, ROL/ROR/RCL/RCR, SHLD/SHRD,
/// PUSH/POP and near CALL/JMP/Jcc/RET.
/// Register immediates
/// retain their unbound interface; other scalars require Scalars, CALL requires
/// Calls, and logical flags require an AF choice. Division requires an explicit
/// arithmetic-flags image and delivers #DE for zero divisors or an
/// unrepresentable quotient. Multiplication requires an explicit undefined-
/// flags profile and retains architectural CF/OF for full/truncated products.
/// Shifts and rotates require explicit undefined CF/AF/OF choices; defined
/// result/flag bits follow the masked count and independent operand width.
/// Double shifts use DoubleShiftUndefined: only a word count above 16 consumes
/// its result and full arithmetic-flags image; other counts retain all defined
/// bits. Count zero still checks a memory destination's write permissions.
/// The explicit FastSystem profile adds protected
/// SYSENTER/SYSEXIT with live Sysenter inputs and fixed CS/SS caches. Ignored
/// prefixes retain 32-bit targets; LOCK delivers #UD after original-byte fetch.
/// These transfers stop before fetching their destination. On an architectural
/// fault, enters the IDT with the original EIP. On
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
