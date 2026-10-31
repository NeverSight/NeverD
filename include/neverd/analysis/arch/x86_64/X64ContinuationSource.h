//===- X64ContinuationSource.h - Explicit native continuation C -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Versioned instruction-step source interface for physical x64 continuations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSOURCE_H
#define NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSOURCE_H

#include "neverd/analysis/Limits.h"
#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/X86/X86SvmContext.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowUndefinedEffects.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {
struct BinaryImage;
namespace analysis {

/// Instruction-step source interface, separate from ordinary function C and
/// the interpreter recovery ABI. GPRs use the x86 encoding order. Vector
/// containers retain all bytes, including upper lanes untouched by legacy SSE.
/// The environment owns other architectural banks (MSRs, descriptor tables,
/// etc.); a NativeInstruction request must be handled with that environment.
struct alignas(16) X64ContinuationStateV1 {
  uint64_t GPR[16] = {};
  uint64_t RFlags = 2;
  uint64_t PC = 0;
  uint64_t FSBase = 0;
  uint64_t GSBase = 0;
  uint8_t ZMM[32][64] = {};
  /// Fresh, caller-selected architectural arbitrary values for this dynamic
  /// instruction. Each sidecar effect consumes one slot, including conditional
  /// effects. All copies of its produced value share that value. Reusing these
  /// slots selects a subset of permitted executions, not all undefined choices.
  uint64_t Undefined[8] = {};
};

/// A separate layout; V1 callers never provide or acquire these extra banks.
struct alignas(16) X64ContinuationStateV2 {
  X64ContinuationStateV1 Core;
  uint64_t CR8 = 0;
  uint64_t KernelGSBase = 0;
  uint32_t MXCSR = 0;
  uint32_t Reserved = 0;
  uint64_t ReservedTail = 0;
};

/// V3 retains the V2 storage layout. Its separate profile and symbol prefix
/// permit AC in Core.RFlags; invoking a V2 step still rejects that state.
using X64ContinuationStateV3 = X64ContinuationStateV2;

/// V4 owns a separate CR2 cell. Earlier layouts and profiles never read it.
struct alignas(16) X64ContinuationStateV4 {
  X64ContinuationStateV2 Core;
  uint64_t CR2 = 0;
  uint64_t Reserved = 0;
};

/// Visible selector and hidden descriptor cache. Limit is the effective byte
/// limit after granularity expansion. Attributes use the descriptor access
/// byte in bits 0..7 and AVL/L/DB/G in bits 12..15; bits 8..11 are zero.
struct X64ContinuationSegment {
  uint64_t Base = 0;
  uint32_t Limit = 0;
  uint16_t Selector = 0;
  uint16_t Attributes = 0;
};

/// V5 records the explicit result of SYSRET/SYSRETQ, including hidden CS/SS
/// caches. Other segment banks remain environment-owned and unchanged.
struct alignas(16) X64ContinuationStateV5 {
  X64ContinuationStateV4 Core;
  uint64_t STAR = 0;
  X64ContinuationSegment CS;
  X64ContinuationSegment SS;
  uint64_t Reserved = 0;
};

struct X64ContinuationDescriptorTable {
  uint64_t Base = 0;
  uint16_t Limit = 0;
  uint16_t Reserved = 0;
  uint32_t ReservedTail = 0;
};

/// FS/GS bases retain their single owner in StateV1, including when a return
/// invalidates a selector. Only the remaining descriptor fields live here.
struct X64ContinuationSegmentMetadata {
  uint32_t Limit = 0;
  uint16_t Selector = 0;
  uint16_t Attributes = 0;
};

/// V6 owns descriptor and interruptibility state for far/interrupt returns and
/// software interrupts. Unusable bits correspond to CS, SS, DS, ES, FS, GS,
/// LDTR, TR in that order. For an unusable segment, its selector and unusable
/// bit are meaningful; its cached limit/attributes and non-FS/GS base are not
/// architectural outputs. Invalidating a segment retains those ignored bytes.
/// This does not assert a processor-specific hidden-cache pattern for a null
/// selector.
struct alignas(16) X64ContinuationStateV6 {
  X64ContinuationStateV5 Core;
  X64ContinuationDescriptorTable GDTR;
  X64ContinuationDescriptorTable IDTR;
  X64ContinuationSegment LDTR;
  X64ContinuationSegment TR;
  X64ContinuationSegment DS;
  X64ContinuationSegment ES;
  X64ContinuationSegmentMetadata FS;
  X64ContinuationSegmentMetadata GS;
  uint8_t Unusable = 0;
  uint8_t NMIBlocked = 0;
  uint8_t Reserved[14] = {};
};

/// V7 owns the live Intel fast-system-call MSRs. CS retains all 32 readable
/// bits; its upper 32 bits are zero. EIP/ESP hold complete canonical addresses.
struct alignas(16) X64ContinuationStateV7 {
  X64ContinuationStateV6 Core;
  uint64_t SysenterCS = 0;
  uint64_t SysenterESP = 0;
  uint64_t SysenterEIP = 0;
  uint64_t Reserved = 0;
};

/// V8 retains V7's banks and owns UIF and the validity of cache-line
/// monitoring. The monitor's address/range and pending user-interrupt state
/// remain owned by the environment. Once MonitorActive is zero its old range is
/// inactive.
struct alignas(16) X64ContinuationStateV8 {
  X64ContinuationStateV7 Core;
  uint8_t UIF = 0;
  uint8_t MonitorActive = 0;
  uint8_t Reserved[14] = {};
};

/// V9 retains earlier storage and owns FRED stack level and event outputs.
/// Only FredConfig[1:0] (CSL) changes; other configuration bits stay intact.
/// PendingDebug records a pending single-step trap, not its delivery.
struct alignas(16) X64ContinuationStateV9 {
  X64ContinuationStateV8 Core;
  uint64_t FredConfig = 0;
  uint8_t STIBlocked = 0;
  uint8_t PendingDebug = 0;
  uint8_t Reserved[6] = {};
};

/// V10 extends V4, not the Intel event/return profiles. The explicit vector
/// cells and MXCSR are authoritative; SVM.GuestXState owns x87 and is also the
/// native exchange image. SVM.HostXState is compiler scratch. Other SVM cells
/// are overwritten from the instruction state and are not additional inputs.
struct alignas(16) X64ContinuationStateV10 {
  X64ContinuationStateV4 Core;
  X86SvmContext SVM;
};

/// V11 extends V4 with the complete long-mode bounds bank and supervisor MPX
/// configuration. Upper bounds retain their stored (complemented) form.
/// MPXSupported/OSXSAVE are Boolean snapshots; transactions are not admitted.
struct alignas(16) X64ContinuationStateV11 {
  X64ContinuationStateV4 Core;
  uint64_t Bounds[4][2] = {};
  uint64_t BNDCFGS = 0;
  uint64_t BNDSTATUS = 0;
  uint64_t XCR0 = 0;
  uint8_t MPXSupported = 0;
  uint8_t OSXSAVE = 0;
  uint8_t InTransaction = 0;
  uint8_t Reserved[5] = {};
};

enum class X64ContinuationExitV1 : uint64_t {
  Continue = 0,
  Call = 1,
  Jump = 2,
  Return = 3,
  /// Nothing in this instruction has executed. PC still identifies its exact
  /// bytes in the manifest. This is an explicit unsupported execution boundary,
  /// not successful emulation, an ordinary return, or permission to skip it.
  NativeInstruction = 4,
  /// State, a control write, or a division precondition violates the invocation
  /// contract; no state is changed. This rejects an invocation, not an emulated
  /// CPU fault.
  InvalidState = 5,
  /// The explicit state now belongs to the returned privilege context. Stop
  /// the supervisor dispatcher; this is not an ordinary function return.
  PrivilegeReturn = 6,
  /// A synchronous exception has installed its handler context. Stop before
  /// executing the handler; this is not a native execution request.
  Exception = 7,
  /// A fast system call installed its supervisor context. Stop before the
  /// handler executes. Values 8/9 belong to the public bounded driver.
  PrivilegeEnter = 10,
  /// UIRET installed its same-privilege target, flags, stack and UIF. Stop
  /// before target execution or pending user-interrupt delivery.
  UserInterruptReturn = 11,
  /// FRED installed its target state. Stop before executing the target or
  /// delivering a pending debug exception or newly unblocked interrupt.
  FredReturn = 12,
  /// Native VMRUN completed through VMEXIT. State.PC is its fallthrough. Stop
  /// before further host instructions; the VMM owns exit handling and GIF.
  SvmExit = 13,
  /// MPX raised #BR before instruction completion. BNDSTATUS is published,
  /// PC remains on the faulting instruction and no handler has executed.
  /// The bounded driver counts this attempted step once and stops.
  BoundsFault = 14,
};

enum class X64ContinuationProfile : uint8_t {
  Unspecified,
  /// CPL=0, IOPL=0, CET disabled, no asynchronous events/privilege transitions,
  /// canonical KernelX64NoFaultV1 flags and successful ordinary RAM accesses.
  /// Execution is sequential, with no concurrent memory writers or MMIO.
  /// Code retains the exact fixed image mapping and is immutable for execution.
  /// In particular, every executed byte must match the returned instruction
  /// manifest. This is an explicit snapshot binding, not a theorem about PE
  /// loader writes or initialized Windows state. Data reads are never folded.
  /// State storage is accessible and disjoint from all guest accesses. FS/GS
  /// bases are arbitrary and may alias each other or the physical guest stack.
  /// The host is little-endian with 64-bit pointers. Faulting executions and
  /// NativeInstruction requests are outside this source execution contract.
  /// Flat addr64 REP MOVS/STOS retain their runtime count, direction and full
  /// register completion through the owned native intrinsic on an x64 host.
  KernelNoFaultV1,
  /// V1's domain plus an x64 host, legacy GS (CR4.FRED=0), explicit CR8/MXCSR
  /// state and no APIC/TPR memory aliases or external changes to these banks.
  /// MXCSRMask declares the effective writable bits. V2 uses StateV2 and a
  /// distinct generated symbol prefix; it is not an extension of V1 storage.
  KernelControlsNoFaultV2,
  /// V2's controls plus CPUID SMAP support and a live AC flag. Accesses must
  /// remain nonfaulting under the actual page permissions and AC value.
  /// STAC/CLAC update guest flags, never host flags. No page-table, SMAP-fault
  /// or privilege-transition model is implied. V1/V2 admission is unchanged.
  KernelSmapNoFaultV3,
  /// V3 plus the explicit fault-address register. Under the nonfaulting,
  /// no-asynchronous-event domain only MOV CR2 changes it. No page-fault,
  /// paging, TLB or CR3 model is implied. Uses StateV4 and a distinct prefix.
  KernelFaultAddressNoFaultV4,
  /// V4 plus explicit STAR and CS/SS caches, EFER.SCE=1 and a declared paging
  /// address width. SYSRET/SYSRETQ produce the full compatibility/64-bit user
  /// return context and exit; they do not execute user code or change the host
  /// privilege level. SYSRET zero-extends ECX into RIP and preserves ESP. Its
  /// eight arbitrary slots choose the legacy GPR high halves in encoding order;
  /// R8-R15 remain preserved. SYSRETQ preserves every complete GPR.
  /// SYSRET accepts RF/IOPL/VIF/VIP on entry; TF remains excluded because
  /// debug-trap delivery is unowned. R11 supplies the defined output flags.
  /// RDMSR/WRMSR of IA32_STAR use this same live STAR cell. Writes require
  /// a zero low 32-bit reserved field; other values refuse without effects.
  /// Selector fields need not be canonical addresses. No host MSR is changed.
  /// Initial CS must describe CPL0 64-bit code. CET/FRED and asynchronous
  /// events remain disabled. Earlier state layouts are never extended.
  KernelReturnNoFaultV5,
  /// V5 plus live GDT/LDT/IDT/TSS RAM and explicit segment caches. Canonical
  /// 16/32/64-bit IRET and RETF (with or without imm16) may return to 64-bit or
  /// compatibility code at CPL0..3. The raw saved RIP must be canonical;
  /// compatibility targets use its low 32 bits within the CS limit. Their
  /// eight conditional slots choose legacy GPR high halves, including RSP.
  /// Target SS.B controls stack restoration/cleanup; 16-bit stacks retain
  /// original ESP[31:16]. RETF preserves NMI blocking and applies unsigned
  /// cleanup; IRET unblocks NMIs and pops all five fields even at the same CPL.
  /// Word IRET retains AC/ID/VIF/VIP and clears RF. Wider IRET restores RF
  /// from the frame; RETF clears it. INT3 and canonical INT
  /// imm8 enter a CPL0 64-bit interrupt or trap gate, with any IST. They exit
  /// before target execution. NMI blocking is explicit; no NMI is delivered.
  /// CET/FRED, faulting execution, MMIO, concurrent writers and asynchronous
  /// events remain outside the domain. Descriptor accessed-bit writes may alias
  /// the frame or TSS and retain their instruction-local ordering. No host
  /// privilege transfer is made. Segment caches must describe an
  /// architecturally valid entry context. Software interrupts accept TF/RF, all
  /// IOPL values and VIF/VIP in the input flags. IRET/RETF accept these flags
  /// except TF; other steps retain the earlier entry-flags domain.
  /// IRET may produce TF but exits before the
  /// target-instruction effects. LSS/LFS/LGS use Intel 16/32/64-bit operand
  /// rules and own selector loads, descriptor caches and coupled GPR writes.
  /// LSS accepts null SS at CPL0; unlike MOV/POP SS, it does not inhibit
  /// subsequent interrupts or debug events. Null FS/GS loads require an
  /// explicit base policy below. Compatibility-mode loads remain outside this
  /// profile.
  KernelEventsNoFaultV6,
  /// V6 plus Intel SYSENTER/SYSEXIT with SEP support and live SYSENTER MSRs.
  /// SYSENTER accepts a valid CPL0..3 entry and installs CPL0; other
  /// instructions retain the CPL0 entry domain. Canonical SYSENTER/SYSEXIT
  /// also accept flat 16/32-bit compatibility entry with the full instruction
  /// within CS.limit. SYSEXITQ and other steps still require 64-bit entry.
  /// SYSEXIT returns to CPL3 in
  /// compatibility or 64-bit mode. Both exit before executing their target.
  /// Both accept RF/IOPL/VIF/VIP, require TF=0 and clear RF on execution.
  /// SYSENTER additionally clears IF (VM is not a valid long-mode input).
  /// Compatibility returns use the same eight arbitrary GPR high halves as
  /// SYSRET. CET/FRED, faults, asynchronous events and AMD long-mode execution
  /// remain outside this domain. Earlier state layouts are unchanged.
  KernelFastSystemNoFaultV7,
  /// V7 plus enabled UINTR outside an enclave/transaction, with CET/FRED and
  /// asynchronous delivery disabled. UIRET/CLUI/STUI/TESTUI accept CPL0..3
  /// 64-bit entry; other steps retain their earlier CPL domain. Stack reads
  /// must succeed under the actual alignment/page permissions. UIRET exits
  /// before its target executes, preserving every segment and NMI blocking.
  /// CLUI/STUI/TESTUI clear RF and require TF=0 (no debug-trap delivery).
  /// UiretRestoresUIF must declare CPUID.07H.01H:EDX[17].
  UserInterruptNoFaultV8,
  /// Intel FRED enabled at CPL0 in 64-bit mode; CET and RTM disabled,
  /// outside VMX non-root execution.
  /// Only canonical ERETS/ERETU are admitted. Other instructions request
  /// native execution; this does not inherit the earlier FRED-disabled domains.
  /// ERETS returns to 64-bit mode; ERETU may return to compatibility mode.
  /// Compatibility choices are profile-owned, conditional on the live target
  /// mode; ERETU's explicitly restored RIP/RSP high halves stay zero.
  /// Successful ordinary frame/descriptor RAM accesses,
  /// no asynchronous delivery and no pending debug trap at entry. Event
  /// outputs are explicit and the step always stops before their delivery.
  /// PagingAddressBits and MXCSRMask are required. SYSENTER and UIRET feature
  /// bindings are not accepted. Earlier storage layouts remain unchanged.
  FredReturnNoFaultV9,
  /// V4 plus native CPL0 AMD SVM with unchanged XCR0=7. Canonical VMRUN and
  /// addr32 VMRUN transport all fourteen software-managed GPRs, x87, sixteen
  /// XMM/YMM registers, MXCSR and the completed CR2. RAX/RSP/RFLAGS are the
  /// restored host values (RF clears); guest counterparts remain in the VMCB.
  /// SVM buffers are distinct 64-byte-aligned, 832-byte standard XSAVE images,
  /// outside guest-writable RAM. Their reserved headers must be initialized.
  /// The native VMM owns VMCB/VM_HSAVE_PA, GIF, intercepts, debug registers,
  /// supervisor state and mappings. FS/GS/KernelGSBase and CR8 must match the
  /// explicit cells and remain unchanged by the guest; all other guest system
  /// changes must preserve the compiler's host ABI. Generated code, stack,
  /// state and buffers remain mapped and disjoint from guest writes and the
  /// hardware-written VMCB/VM_HSAVE_PA pages.
  /// No nested SVM, SEV-ES, SMM, CET, asynchronous events or faults. A VMEXIT,
  /// including VMEXIT_INVALID, stops the driver at status 13. This profile
  /// does not inherit Intel return domains or authorize an ordinary C ABI.
  NativeSvmNoFaultV10,
  /// V4 plus explicit CPL0 MPX state. All seven qualified long-mode BND
  /// instructions and original near-branch initialization are modeled with
  /// successful sequential ordinary RAM accesses. Disabled MPX performs no
  /// bounds or memory effects. #BR stops before exception delivery; page,
  /// segment, alignment faults, transactions and asynchronous events are
  /// outside this domain. Host MPX is disabled or has BNDPRESERVE set so
  /// compiler-created branches cannot initialize the host bounds bank.
  /// The explicit projection does not execute host MPX instructions.
  MpxNoFaultV11,
};

/// Bind the original processor's null FS/GS load behavior. No vendor-wide
/// default is inferred: AMD behavior also depends on processor capabilities.
/// The selector becomes unusable; its ignored limit/attribute bytes are kept.
/// FS/GS base remains architectural and follows this declared policy.
enum class X64NullSegmentBasePolicy : uint8_t { Clear, Preserve };

struct X64ContinuationSourceOptions {
  X64ContinuationProfile Profile = X64ContinuationProfile::Unspecified;
  std::optional<uint32_t> MXCSRMask;
  /// V5..V9 require 48 or 57 according to active paging, independently of
  /// the processor's MSR canonical width. V1..V4 and V10/V11 do not accept it.
  std::optional<unsigned> PagingAddressBits;
  /// V6 and later. Bind the actual original CPU's null LFS/LGS base transition.
  /// Without this declaration those loads reject before any state/RAM write
  /// or descriptor access. This is not a claim that all AMD CPUs preserve it.
  std::optional<X64NullSegmentBasePolicy> NullSegmentBasePolicy;
  /// V7/V8 require the original Intel CPU's 48- or 57-bit MSR canonical width,
  /// independently of active paging. If segment-base MSRs are also bound,
  /// their declared width must agree. Earlier profiles do not accept this.
  std::optional<unsigned> SysenterMSRAddressBits;
  /// Required only by V8. False means UIRET sets UIF; true means it restores
  /// UIF from bit 1 of the saved flags without changing actual RFLAGS bit 1.
  std::optional<bool> UiretRestoresUIF;
  /// Opt in to the executing x64 host's TSC and IA32_TSC_AUX, including its
  /// actual timestamp access/ordering behavior. The caller must bind this
  /// native environment to the source run and ensure RDTSC/RDTSCP support and
  /// nonfaulting access. No clock or processor-identity translation is made.
  bool NativeTimestampReads = false;
  /// Retain PREFETCHW on the executing x64 host. The invocation supplies its
  /// feature support and the same cacheable memory environment; the hint is
  /// emitted at the original address, including live FS/GS base addition.
  bool NativeWritePrefetch = false;
  /// Model IA32_FS_BASE, IA32_GS_BASE and IA32_KERNEL_GS_BASE through the
  /// existing state cells. The declared processor checks 48- or 57-bit
  /// canonical addresses on WRMSR, independently of the active paging depth.
  /// Other selectors need their own binding (V5/V6 already bind STAR).
  /// No host segment base is
  /// changed; all source memory continues to use the explicit live bases.
  std::optional<unsigned> SegmentBaseMSRAddressBits;
  /// Execute writes to IA32_SPEC_CTRL and IA32_PRED_CMD on the native x64
  /// supervisor processor. The caller supplies feature support, valid values
  /// and the actual speculation-control environment. Every write is retained,
  /// including repeated identical writes. No other native MSR is admitted.
  bool NativeSpeculationControlMsrs = false;
  /// Execute SGDT, memory VERW and register LSL against the native CPL0
  /// descriptor environment. GDTR/LDTR and their live table memory belong to
  /// that environment; selectors and memory operands remain runtime inputs.
  /// VERW retains its actual microarchitectural buffer-clearing behavior.
  bool NativeDescriptorQueries = false;
  /// Execute MOV to/from CR3 on the native supervisor CPU, including its
  /// paging, PCID and TLB effects. Both address spaces must map generated
  /// code, host stack and state storage to the same accessible RAM. Guest
  /// addresses use the actual mappings; writes and later accesses must be
  /// nonfaulting. This binds the native paging environment, not a CR3 cell.
  bool NativePageTableAccess = false;
  /// Bind a returning KVM ABI: RAX number/result, RBX/RCX/RDX/RSI arguments.
  /// Calls preserve every other represented register and flags, retain the
  /// compiler execution environment, and access the same mapped guest RAM.
  /// Services depend only on those five inputs and guest RAM; extra caller
  /// context dependencies or state changes are excluded. This does not infer
  /// KVM from an opcode or CPUID.
  bool NativeKvmHypercalls = false;
  uint64_t MaxInstructions = limits::analysis::kX64ContinuationMaxInstructions;
  uint64_t MaxOperations = limits::analysis::kX64ContinuationMaxOperations;
};

/// A profile-owned choice of a legacy GPR's upper 32 bits on a successful
/// return to compatibility mode. This is not a native LowIR sidecar: the live
/// return descriptor chooses the mode inside the continuation implementation.
struct X64ContinuationCompatibilityChoice {
  uint8_t Slot = 0;
  NdVar Output;
};

struct X64ContinuationInstructionSource {
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  unsigned StateVersion = 1;
  uint64_t StateBytes = sizeof(X64ContinuationStateV1);
  /// One complete native instruction, with an explicit state pointer and exit
  /// status. CALL/RET use the original stack and actual next PC. No argument
  /// count, callee preservation, private frame or return-slot proof is assumed.
  LowFunc Function;
  SourceFunctionTypeHint SourceABI;
  /// The arbitrary-value slot order and original instruction-local timing.
  /// Empty for a native request, where no source instruction has executed.
  std::vector<LowUndefinedEffect> UndefinedEffects;
  /// Apply only after a successful profile transition with new CS.L=0.
  /// Slots retain x86 GPR encoding order; explicitly restored registers may
  /// have no choice. No original instruction-local operation index is implied.
  std::vector<X64ContinuationCompatibilityChoice> CompatibilityChoices;
  bool RequiresNativeExecution = false;
  std::string Diagnostic;
};

/// Strictly relift every requested original instruction through the binary
/// provider. Addresses select a region, not proof of whole-function coverage.
/// The caller dispatches on State.PC after each step and may reenter any listed
/// address with fully changed state. Missing addresses remain outside the
/// region. Unsupported decoding/lifting fails; supported instructions outside
/// this version's state lowering remain explicit NativeInstruction requests.
/// Never publish these units as ordinary function C or count requests as
/// reconstructed native semantics. Both C backends consume the same LowIR and
/// explicit SourceABI; emission uses no BinaryImage address rebasing.
/// Emit ordinary memory as byte copies (UseUnalignedPointers=false) or through
/// the C backends' aligned(1), may_alias types, including state accesses.
llvm::Expected<std::vector<X64ContinuationInstructionSource>>
buildX64ContinuationSource(const BinaryImage &Image,
                           llvm::ArrayRef<va_t> Addresses,
                           const X64ContinuationSourceOptions &Options);

} // namespace analysis
} // namespace neverd

#endif // NEVERD_ANALYSIS_ARCH_X86_64_X64CONTINUATIONSOURCE_H
