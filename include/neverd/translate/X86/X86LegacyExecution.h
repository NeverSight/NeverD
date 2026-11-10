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

#include "neverd/ir/X86/X86DebugRules.h"
#include "neverd/ir/X86/X86ExceptionDelivery.h"
#include "neverd/ir/X86/X86MpxContext.h"
#include "neverd/translate/X86/X86EventDeliveryOptions.h"
#include "neverd/translate/X86/X86LegacySegmentExecution.h"
#include "neverd/translate/X86/X86PortIO.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::translate {

namespace detail {
struct X86LegacyTaskContinuation;
struct X86LegacyTransferContinuation;
}

/// Low 32-bit architectural inputs for non-IA-32e SYSENTER/SYSEXIT.
struct X86LegacySysenterState {
  uint32_t CS = 0;
  uint32_t ESP = 0;
  uint32_t EIP = 0;
};

using X86LegacyEventKind = X86EventKind;

struct X86LegacyEvent {
  uint32_t SavedEIP = 0;
  uint32_t ErrorCode = 0;
  uint32_t FaultAddress = 0;
  uint8_t Vector = 0;
  X86LegacyEventKind Kind = X86LegacyEventKind::Fault;
  /// Only INT imm8 uses the VM86 IOPL check and VME redirection map.
  bool SoftwareINT = false;
};

/// Pending event retained across backing or work-budget stops. Repair backing
/// if necessary before resume; retain architectural state and RAM effects and
/// do not begin another instruction or accept another external event.
/// Completed inputs are retained and writes are not replayed: a partial frame
/// can already have overwritten the descriptor or TSS that selected it.
struct X86LegacyEventContinuation {
  struct Access {
    uint32_t Address = 0;
    uint8_t Bytes = 0;
    bool Write = false;
    uint64_t Value = 0;
  };
  X86LegacyEvent Event;
  uint32_t FaultEIP = 0;
  uint32_t SavedFlags = 0;
  std::optional<X86MemoryException> LastException;
  uint8_t DataBreakpoints = 0;
  bool SoftwareCompleted = false;
  llvm::SmallVector<Access> CompletedAccesses;
};

/// PUSHA/POPA/ENTER progress across a physical-backing stop. Retain this
/// state, the CPU/profile bindings and RAM; repair only unavailable backing
/// before calling the same instruction again. Completed accesses are not
/// repeated, even if an earlier push overwrote its own input or code bytes.
/// Architectural faults clear this continuation and restart at the original
/// PC after event delivery. POPA's already loaded registers remain visible.
struct X86LegacyStackContinuation {
  struct Access {
    uint32_t Offset = 0;
    uint32_t Value = 0;
    bool Write = false;
  };
  X86LegacySegmentState Entry;
  std::array<uint32_t, 8> ExpectedGPR{};
  uint32_t EIP = 0;
  uint32_t EFlags = 0;
  llvm::SmallVector<uint8_t> Instruction;
  llvm::SmallVector<Access> CompletedAccesses;
  uint8_t DataBreakpoints = 0;
};

/// MPX ordinary-RAM progress across a backing stop. Retain CPU/profile state
/// and RAM, repair only backing, and retry the same bytes. A completed BDE
/// read must not be repeated: later page walks can alias its storage. No
/// operand store or bounds result is published before the whole operand
/// succeeds. Architectural faults discard this continuation before IDT entry.
struct X86LegacyMpxContinuation {
  X86LegacySegmentState Entry;
  X86PagingState Controls;
  std::optional<X86MpxContext> Mpx;
  uint32_t EIP = 0;
  uint32_t EFlags = 0;
  llvm::SmallVector<uint8_t> Instruction;
  struct DirectoryRead {
    uint32_t Address;
    uint32_t Value;
  };
  std::optional<DirectoryRead> Directory;
  uint8_t DataBreakpoints = 0;
};

/// Debug bank shared with the paged x64 owner, with 32-bit address wrap.
using X86LegacyDebugState = X86InstructionDebugState<uint32_t>;

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
  /// Shared bounds/check/table and branch bank, rebound to live CPL/OSXSAVE.
  /// Absence proves nothing; CR4.OSXSAVE=0 can prove MPX is disabled.
  std::optional<X86MpxContext> Mpx;
  std::optional<X86LegacyEventContinuation> PendingEvent;
  std::optional<X86LegacyDebugState> Debug;
  std::optional<X86LegacyStackContinuation> PendingStack;
  std::optional<X86LegacyMpxContinuation> PendingMpx;
  /// HLT has completed at CPL0; EIP names its following instruction. No
  /// instruction may fetch until an eligible IRQ, NMI or debug trap is
  /// accepted. Event entry clears this before touching the IDT, including
  /// failed entry. SMI/INIT/RESET require their separate machine-state owner.
  bool Halted = false;
  /// The task owner retains private publication/replay state across a backing
  /// stop or a fault after loading the incoming task. Preserve this with the
  /// CPU and RAM; only that owner may resume or retire it.
  std::shared_ptr<const detail::X86LegacyTaskContinuation> PendingTask;
  /// Return and VM86 far-transfer progress across unavailable backing. Retain
  /// this opaque state with the CPU, bindings and original instruction bytes;
  /// repair only backing. Completed RAM accesses are never repeated.
  std::shared_ptr<const detail::X86LegacyTransferContinuation> PendingTransfer;
};

/// One CPU, ordinary RAM, 16/32-bit gates, qword accessed writes, offset-first
/// TSS reads and ordered frame pushes. A 16-bit target stack preserves old
/// ESP high bits. Consecutive MOV/POP SS does not extend an incoming shadow.
/// Protected target IP checks follow frame stores; VM86 exit checks first.
/// Recursive #DF retains
/// the failed fault's saved RF image; its saved PC/caches use the original
/// state. These #DF choices bind otherwise architecturally undefined program
/// state. The producer owns event selection, APIC/debug state and machine-check
/// recovery. Task gates use the additional Tasks binding; VM86 uses
/// Virtual8086. CET needs its separate execution-mode owner.
enum class X86LegacyEventProfile : uint8_t { OrderedFrame };

/// IRET reads FLAGS/CS/IP, RETF reads CS/IP; descriptor accessed writes precede
/// outer SS/SP reads and late target-limit checks. CS/SS accesses cover only
/// their selector word, independently of the frame slot width. Ordinary RAM,
/// one CPU. Host backing stops retain private frame/descriptor access progress
/// and IRET NMI unblocking; resumption does not repeat an instruction attempt.
enum class X86LegacyReturnProfile : uint8_t { OrderedFrame };

/// Intel VM86 execution with VME word IRET and low-word IRETD entry IP.
/// Ordinary RAM; interrupt redirection reads the TSS/IVT as supervisor and
/// pushes words on the user stack, with independent slot wrap/faults. CALL
/// checks its target after both pushes. VIF/VIP conflicts use either VME or
/// PVI, independently of IOPL. These CPU choices need explicit target binding.
enum class X86LegacyVM86Profile : uint8_t { IntelVME };

/// Confined ordinary RAM. POP memory uses incremented ESP, with SS.B-sized
/// stack advancement preserving high ESP on a word wrap. PUSHA/ENTER stores
/// and POPA register loads publish in order; ESP and ENTER's EBP commit only
/// after completion. POPA reads and discards the saved SP slot. ENTER's frame
/// value uses operand-sized entry-SP subtraction, independently of SS.B.
/// ENTER checks an operand-sized write at its final SP
/// without storing data. Host backing stops retain PendingStack.
enum class X86LegacyScalarProfile : uint8_t { OrderedRAM };

/// Read-only descriptor queries in protected mode. The table image supplies
/// LAR's undefined bits 19:16; no query checks presence or sets accessed.
enum class X86LegacyDescriptorProfile : uint8_t { TableImage };

/// One CPU with exclusive ordinary RAM. LTR checks the descriptor before a
/// whole-qword busy read/modify/write. Null LLDT keeps hidden unusable fields
/// opaque; SLDT/STR dword register results zero-extend the selector.
enum class X86LegacySystemSelectorProfile : uint8_t { ExclusiveTable };

/// SMSW's architecturally undefined high word of a dword register result
/// follows CR0[31:16]. Required only for that form, independently of host CPU.
enum class X86LegacyMachineStatusProfile : uint8_t { FullCR0 };

/// CR4 capability bits not owned by X86PagingFeatures: VME/PVI/TSD/DE,
/// MCE/PGE/PCE, OSFXSR/OSXMMEXCPT/UMIP, VMXE/SMXE, FSGSBASE/PCIDE/OSXSAVE,
/// KL/CET/UINTR/LASS/LAM. Reserved bits are not CPU capabilities.
inline constexpr uint32_t kX86LegacyOtherCR4FeatureMask = 0x1a8f6fcf;

/// Intel, outside VMX/SMX/SMM/transactions, with exclusive ordinary RAM and
/// fresh page walks. Arithmetic flags are preserved; CR0 ignores reserved low
/// bits and fixes ET=1; ignored CR3 bits retain the source image. PAE validates
/// reserved bits only in present PDPTEs. Paging capabilities come solely from
/// X86PagingFeatures. OtherCR4Features binds the remaining supported CR4 bits,
/// independently of the host. Valid requests for unbound memory/mode domains
/// refuse instead of inventing #GP. No real/IA-32e transfer is implied.
struct X86LegacyControlRegisterBinding {
  uint32_t OtherCR4Features = 0;
};

/// One ordinary-RAM element per step, with destination-first CMPS reads.
/// Incomplete comparisons retain entry flags; completed elements retain their
/// memory/register effects across faults and producer-selected IRQ/NMI entry.
/// Original instruction bytes remain immutable until completion or an event.
/// Fast-string timing, MMIO and external writers are outside this profile.
enum class X86LegacyStringProfile : uint8_t { ElementwiseDestinationFirst };

/// One CPU with exclusive access to the entire ordinary-RAM image for each
/// step. No concurrent CPU, DMA, MMIO, split-lock or bus-lock trap producer.
/// Locked operands use one prepared read/write plan, including failed
/// compare-exchange. This is not a shared-memory synchronization primitive.
enum class X86LegacyAtomicProfile : uint8_t { ExclusiveRAM };

/// One CPU, exclusive ordinary RAM, whole BNDMOV/BDE/BTE operands. Page A/D
/// effects survive faults; bounds loads and stores publish only on complete
/// access. BNDMOV uses eight-byte alignment when #AC is enabled. BDE/BTE
/// addresses are linear with a flat DS base; DS permission/limit checks apply
/// to BTE operands. BNDLDX BTE faults forget only its destination bounds.
/// Non-flat DS tables, MMIO and transactional aborts need other bindings.
enum class X86LegacyMpxProfile : uint8_t { WholeOperandRAM };

inline constexpr uint32_t kX86LegacyArithmeticFlagsMask = 0x8d5;
inline constexpr uint32_t kX86LegacyShiftUndefinedFlagsMask = 0x811;
inline constexpr uint32_t kX86LegacyBitTestUndefinedFlagsMask = 0x894;
inline constexpr uint32_t kX86LegacyBitScanChoiceFlagsMask = 0x895;
inline constexpr uint32_t kX86LegacyBitCountUndefinedFlagsMask = 0x894;
inline constexpr uint32_t kX86LegacyBMIUndefinedFlagsMask = 0x14;
inline constexpr uint32_t kX86LegacyBEXTRUndefinedFlagsMask = 0x94;

/// Producer-selected instruction availability, independently of host CPUID.
/// Missing POPCNT/ADX or VEX BMI1/BMI2 delivers #UD. Missing LZCNT/BMI1
/// selects BSR/BSF for LZCNT/TZCNT. Other ISA banks are not implied.
struct X86LegacyIntegerExtensions {
  bool POPCNT = false;
  bool LZCNT = false;
  bool BMI1 = false;
  bool ADX = false;
  bool BMI2 = false;
};

/// Explicit CPU choices for BSF/BSR's zero-source result and non-ZF flags.
/// Current Intel documentation specifies preservation of the zero result;
/// older architectural contracts leave it undefined. The producer binds the
/// selected target, never the executing host. Nonzero indices and ZF are
/// always computed from the source, independently of this image.
struct X86LegacyBitScanChoices {
  uint32_t ZeroResult = 0;
  uint32_t Flags = 0;
};

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

/// Protected far CALL/JMP, ordinary RAM and one CPU. Descriptor accessed
/// writes and ordered frame stores survive a later transfer fault. Call gates
/// use their own width, independently of CS.D, SS.B and the operand prefix.
/// Task switching, VM86 and CET require separate state bindings.
enum class X86LegacyFarTransferProfile : uint8_t { OrderedFrame };

/// Intel SEP with explicit MSRs, no CET/FRED and no debug-event producer.
/// Transfers install fixed caches without reading descriptors or either stack.
enum class X86LegacyFastSystemProfile : uint8_t { IntelSEP };

/// One CPU, exclusive ordinary RAM, outgoing TSS stores before incoming state
/// reads, then LDT/CS/SS/data descriptor loads. This specifies an access order;
/// it does not infer a physical processor's microcode ordering. Guest TSS
/// residency is checked before stores. Invalid changed PAE roots publish the
/// new task, a cleared PDPTE bank and #TS(0). Requires a debug bank and an
/// explicit Segments.FullLimitChoice before task effects.
enum class X86LegacyTaskProfile : uint8_t { OrderedTSS };

struct X86LegacyTaskBinding {
  X86LegacyTaskProfile Profile = X86LegacyTaskProfile::OrderedTSS;
  /// A word TSS does not preserve GPR upper halves. Bind their arbitrary
  /// incoming values explicitly; dword tasks do not consume this choice.
  std::optional<std::array<uint16_t, 8>> WordGPRHigh;
};

/// Shared legacy/x64 instruction-step and execute-comparator profiles. MOV/POP
/// SS suppresses its own TF trap and the following instruction breakpoint.
/// DebugRegisters additionally owns MOV-DR/GD with a bound frame policy.
/// Watchpoints additionally owns data/I/O causes and delayed SS state. Task
/// traps require Tasks; branch stepping requires BranchWatchpoints.
using X86LegacyDebugProfile = X86InstructionDebugProfile;

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
  std::optional<X86LegacyTaskBinding> Tasks;
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
  /// OF/SF/AF/PF after successful BT/BTS/BTR/BTC. CF is computed; ZF is kept.
  std::optional<uint32_t> BitTestUndefinedFlags;
  /// Required for BSF/BSR; Flags may contain only CF/PF/AF/SF/OF.
  /// Faults retain the original destination and complete flag image.
  std::optional<X86LegacyBitScanChoices> BitScanChoices;
  /// Required for explicit LOCK and implicit-lock memory XCHG.
  std::optional<X86LegacyAtomicProfile> Atomics;
  /// Successful decimal-adjust undefined flags: AAA/AAS OF/SF/ZF/PF,
  /// DAA/DAS OF, AAM/AAD OF/AF/CF. No other bits are permitted. AAM base
  /// zero delivers #DE before publishing AX or flags; AAD base zero is valid.
  std::optional<uint32_t> DecimalUndefinedFlags;
  std::optional<X86LegacyStringProfile> Strings;
  /// Explicit memory-string debug grouping, shared with paged x64. Required
  /// for Watchpoints plus REP even at zero count; {} selects one element.
  std::optional<X86RepeatDebugBinding> RepeatDebug;
  std::optional<X86PortIOBinding> PortIO;
  std::optional<X86LegacyFarTransferProfile> FarTransfers;
  std::optional<X86LegacyDebugProfile> Debug;
  std::optional<X86GeneralDetectFrame> GeneralDetectFrame;
  X86EventDeliveryOptions EventDelivery;
  /// Required for POPCNT/LZCNT/TZCNT/ADCX/ADOX and BMI, even when unavailable.
  std::optional<X86LegacyIntegerExtensions> IntegerExtensions;
  /// OF/SF/AF/PF after successful LZCNT/TZCNT with the feature enabled.
  /// CF/ZF are computed. Feature-disabled aliases use BitScanChoices instead.
  std::optional<uint32_t> BitCountUndefinedFlags;
  /// AF/PF for ANDN/BLSI/BLSMSK/BLSR/BZHI, plus SF for BEXTR. Required only
  /// when the corresponding feature is enabled; defined flags are forbidden.
  /// MULX/PDEP/PEXT/RORX/SARX/SHLX/SHRX preserve flags and need no image.
  std::optional<uint32_t> BMIUndefinedFlags;
  /// Required for LAR/LSL/VERR/VERW, including invalid selectors.
  std::optional<X86LegacyDescriptorProfile> DescriptorQueries;
  std::optional<X86LegacySystemSelectorProfile> SystemSelectors;
  std::optional<X86LegacyMachineStatusProfile> MachineStatus;
  std::optional<X86LegacyControlRegisterBinding> ControlRegisters;
  std::optional<X86LegacyMpxProfile> MpxMemory;
  std::optional<X86LegacyVM86Profile> Virtual8086;
};

enum class X86LegacyExecutionStatus : uint8_t {
  Completed,
  EventDelivered,
  EventDeferred,
  CodeChanged,
  BackingFault,
  Shutdown,
  /// One REP element committed; EIP still names the original instruction.
  RepeatContinue,
  DeviceUnavailable,
  /// Resume PendingEvent; this is a host work boundary, not a CPU exception.
  EventBudgetExhausted,
  /// HLT completed, or the CPU is still waiting without another attempt.
  /// Consult InstructionAttempted/Completed to distinguish those cases.
  Halted,
  /// A task has published a destination outside the protected executor's
  /// mode contract. Retain PendingTask (and any PendingEvent); no instruction
  /// or event completion is implied, and this is not a guest exception.
  ModeUnavailable,
};

struct X86LegacyExecutionResult {
  X86LegacyExecutionStatus Status = X86LegacyExecutionStatus::Completed;
  bool InstructionAttempted = false;
  bool InstructionCompleted = false;
  bool EventAccepted = false;
  bool SoftwareCompleted = false;
  bool DeviceTransferCompleted = false;
  uint8_t PortInputBytesWritten = 0;
  uint8_t DeliveredVector = 0;
  unsigned DeliveryAttempts = 0;
  std::optional<X86MemoryException> Exception;
  std::optional<GuestMemoryFault> BackingFault;
  std::vector<SelfModificationExit> SelfModifications;
};

/// Authenticate a complete instruction extent with the executor's own byte
/// grammar. This performs no accesses and binds no runtime state or profiles.
/// An accepted encoding can still require an explicit execution option or
/// raise an owned architectural fault. Used by original-byte source export.
llvm::Error
validateX86LegacyInstructionEncoding(llvm::ArrayRef<uint8_t> Instruction,
                                     bool Code32);

/// Persistent shadows determine eligibility before any table access. Fault
/// delivery retains CR2, NMI blocking, descriptor writes and earlier frame
/// stores, including on a subsequent #DF/shutdown. Registers/cache/EIP commit
/// together only after a complete entry. No handler instructions are fetched.
llvm::Expected<X86LegacyExecutionResult>
deliverX86LegacyEvent(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                      const X86LegacyEvent &Event,
                      const X86LegacyExecutionOptions &Options);

/// Resume a previously accepted event without refetching or repeating its
/// triggering instruction/device effects. The result counts only this call's
/// delivery attempts; EventAccepted and device-completion fields remain false.
llvm::Expected<X86LegacyExecutionResult>
resumeX86LegacyEvent(X86LegacyExecutionState &State, X86PagedMemory &Memory,
                     const X86LegacyExecutionOptions &Options);

/// Authenticates original bytes through CS and paging, then executes the
/// segment-transition owner, the explicit ordinary-RAM scalar owner, or
/// NOP/CLI/STI/HLT/INT/INT3/INTO/INT1/UD2/IRET/RETF. HLT retains a halt
/// state across calls; waiting performs no fetch and consumes no attempt.
/// Scalars include
/// MOV/MOVSX/MOVZX, LEA, ARPL, ADD/ADC/SUB/SBB/CMP, AND/OR/XOR/TEST,
/// INC/DEC/NEG/NOT, MUL/IMUL, DIV/IDIV, CBW/CWDE/CWD/CDQ, LAHF/SAHF,
/// CLC/STC/CMC/CLD/STD, BSWAP (dword), XLAT, LEAVE, SETcc/CMOVcc,
/// LOOP/LOOPcc/JCXZ/JECXZ, SHL/SAL/SHR/SAR, ROL/ROR/RCL/RCR, SHLD/SHRD,
/// BT/BTS/BTR/BTC, BSF/BSR, XCHG/XADD/CMPXCHG/CMPXCHG8B,
/// AAA/AAS/DAA/DAS and AAM/AAD with every immediate base,
/// PUSH/POP, PUSHF/POPF and near CALL/JMP/Jcc/RET.
/// Code32 MPX uses the shared bounds context; memory additionally requires
/// MpxMemory. PendingMpx retains host backing progress. Original near branch
/// BNDINIT commits only on success, including untaken Jcc, before debug entry.
/// FarTransfers adds direct and memory far CALL/JMP and 16/32-bit call gates,
/// including inner-ring stack switches and parameter copying. Transfers stop
/// before fetching the destination. Tasks additionally owns protected TSS
/// CALL/JMP, task gates and NT IRET. PendingTask retains pre/post-publication
/// memory progress; failures after publication belong to the incoming task.
/// VM86 destinations stop with ModeUnavailable and retained state.
/// The explicit Strings profile adds MOVS/STOS/LODS/CMPS/SCAS, with REP or
/// REPE/REPNE as appropriate, in both address and operand widths. Each call
/// commits at most one element; RepeatContinue retains EIP, sets RF, ends
/// SS/STI inhibition and leaves comparisons' entry flags available for event
/// delivery. The caller may deliver an event at that boundary or repeat the
/// same immutable bytes. Zero counts complete without operand access. LOCK
/// raises #UD after fetch; duplicate prefixes and F2 on non-comparisons refuse.
/// PortIO adds IN/OUT and single/REP INS/OUTS with explicit device effects.
/// DeviceUnavailable preserves indices/count/flags; an input fault may retain
/// a read and RAM fragments according to the bound profile. Pending event
/// delivery must be resumed before executing another instruction.
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
/// Valid memory LOCK forms and memory XCHG additionally require Atomics.
/// CMPXCHG/CMPXCHG8B check write permissions and write the original memory
/// value on comparison failure. CMPXCHG8B changes only ZF among the flags.
/// ARPL reads its word first and checks write permissions only when RPL must
/// increase. A failed conditional store preserves the original ZF.
/// The explicit FastSystem profile adds protected
/// SYSENTER/SYSEXIT with live Sysenter inputs and fixed CS/SS caches. Ignored
/// prefixes retain 32-bit targets; LOCK delivers #UD after original-byte fetch.
/// These transfers stop before fetching their destination. On an architectural
/// fault, enters the IDT with the original EIP. On
/// success, commits fallthrough/branch state, clears RF and advances SS/STI
/// inhibition. Changed code returns CodeChanged at the first mismatch.
/// Unimplemented or malformed instructions return Error, never a synthesized
/// #UD or NOP. The explicit Debug binding supplies instruction-entry TF
/// sampling and instruction #DB causes; data/I/O producers remain external.
/// Returns stop at their destination, preserve RF restored by IRETD, and
/// end SS/STI inhibition. IRET unblocks NMI after fetch, including on a later
/// fault; RETF preserves NMI blocking.
llvm::Expected<X86LegacyExecutionResult>
executeX86LegacyInstruction(X86LegacyExecutionState &State,
                            X86PagedMemory &Memory,
                            llvm::ArrayRef<uint8_t> Instruction,
                            const X86LegacyExecutionOptions &Options);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_X86LEGACYEXECUTION_H
