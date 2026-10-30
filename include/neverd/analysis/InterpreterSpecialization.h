//===- InterpreterSpecialization.h - LowIR partial evaluation ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H
#define NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H

#include "neverd/analysis/InterpreterEntryAlignment.h"
#include "neverd/analysis/InterpreterMachineStateProfile.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/LowPreservedState.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/symbolic/SymState.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::analysis {

struct SpecializationCursor {
  va_t Address = 0;
  InstructionMode Mode = InstructionMode::Default;

  bool operator==(const SpecializationCursor &) const = default;
};

/// Provider certificate for the omitted physical effects of a native near
/// CALL/RETURN. The instruction uses the configured eight-byte frame register
/// as its stack pointer. CALL pushes its exact fallthrough address. Internal
/// RETURN consumes one address and may apply authenticated unsigned-16-bit
/// cleanup; outer returns admit no extra cleanup. Generic LowIR CALL/RETURN
/// operands alone do not establish this machine-level contract.
/// A memory CALL retains a temporary-only effective-address prefix, one final
/// ordinary eight-byte LOAD, and an INDIR_CALL of that loaded temporary. The
/// entire target evaluation precedes the physical return-address push; a
/// legacy constant-slot INDIR_CALL is not a loaded target certificate.
enum class SpecializationNativeStackControl : uint8_t { None, Call, Return };

struct SpecializationTerminalImport {
  uint32_t Version = 1;
  va_t Slot = 0;
  std::string Module;
  std::string Symbol;
  bool operator==(const SpecializationTerminalImport &) const = default;
};

enum class SpecializationX64MachineExitKind : uint8_t {
  Call,
  Jump,
  Return,
  Trap
};

/// An explicit end of a machine region, after this exact native transfer.
/// The residual retains its real target and physical stack effects. No callee,
/// continuation, ordinary return ABI or whole-function coverage is implied.
/// A Trap exit ends the region at a declared resumable trap (INT3/INT1): the
/// trap effect is retained and control leaves the region to an unmodeled
/// external continuation (the native exception mechanism). No target, callee,
/// return ABI or continuation is implied.
struct SpecializationX64MachineExit {
  uint32_t Version = 1;
  va_t Address = 0;
  SpecializationX64MachineExitKind Kind =
      SpecializationX64MachineExitKind::Return;
  bool operator==(const SpecializationX64MachineExit &) const = default;
};

/// Caller-owned original x64 code at a runtime address. The complete range is
/// executable, readable, nonfaulting and immutable across every possible writer
/// for the whole supported execution. This is an explicit environment domain,
/// not a claim that endpoint snapshots establish temporal immutability.
struct SpecializationX64CodeRegion {
  uint32_t Version = 1;
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  std::string Evidence;
  bool operator==(const SpecializationX64CodeRegion &) const = default;
};

/// Exact ordinary named primary-PE IAT binding, stable throughout execution.
/// The loader authenticates the static slot/name; the caller supplies the live
/// target contract. Only the complete eight-byte pointer read is certified.
struct SpecializationX64ImportBinding {
  uint32_t Version = 1;
  va_t Slot = 0;
  std::string Module;
  std::string Symbol;
  va_t Target = 0;
  std::string Evidence;
  bool operator==(const SpecializationX64ImportBinding &) const = default;
};

struct SpecializationRawEnvironment;

/// Authenticated static identity only, never a runtime IAT value. The provider
/// binds the complete PE snapshot and the caller's explicit terminal domain.
struct SpecializationTerminalImportEvidence {
  SpecializationTerminalImport Binding;
  std::string FixedImageDigest;
};

/// A complete, strictly lifted guest instruction. The provider owns decoding,
/// mapping, relocation, and instruction-level exception checks. A missing or
/// unsupported instruction must return an Error, never an empty approximation.
/// A named ProfileProjection can instead retain exact native bytes for its
/// explicit environment owner. Empty Ops in that case is an unexecuted native
/// instruction, never a NOP or a complete architectural sidecar.
struct SpecializationInstruction {
  std::vector<LowOp> Ops;
  LowInstructionBoundary Origin;
  SpecializationCursor Fallthrough;
  SpecializationNativeStackControl NativeStackControl =
      SpecializationNativeStackControl::None;
  /// Architecture-owned evidence for this exact instruction's Ops. Missing
  /// coverage is not evidence that every output is defined.
  LowInstructionUndefinedEffects UndefinedEffects;
  /// Owned original instruction bytes, independent of decoder buffer reuse.
  std::vector<uint8_t> NativeBytes;
  /// Original native classification, including call-to-fallthrough sequences
  /// lowered to explicit stack operations with no remaining LowIR CALL.
  /// Unlike NativeStackControl, this does not authorize omitted stack effects.
  bool IsNativeCall = false;
  InterpreterProfileProjection ProfileProjection =
      InterpreterProfileProjection::None;
  /// Optional separate architectural bank preservation evidence.
  LowInstructionPreservedState PreservedState;
  std::optional<SpecializationTerminalImportEvidence> TerminalImport;
};

/// An exact non-faulting ordinary read whose bytes remain immutable throughout
/// every supported execution. This is a provider certificate, not a snapshot
/// sampled from writable memory. The returned byte count must equal the
/// request.
struct SpecializationImmutableRead {
  std::vector<uint8_t> Bytes;
  std::string Evidence;
};

/// Architecture-owned meaning of one complete scalar query instruction.
/// Selectors contain every modeled input; results are defined environment
/// inputs, independent between occurrences. Bindings zero-extend a result to
/// its scalar destination, including instruction-local output carriers. The
/// original query remains observable; this does not make its effects ignorable
/// or establish any state preservation outside the separate native audits.
struct SpecializationScalarQuery {
  struct Binding {
    NdVar Destination;
    uint16_t Result = 0;
  };
  std::string Identity;
  uint32_t SemanticsVersion = 0;
  std::vector<NdVar> Selectors;
  std::vector<uint16_t> ResultBits;
  std::vector<Binding> Bindings;
};

class SpecializationProvider {
public:
  virtual ~SpecializationProvider() = default;
  /// Nonempty raw declarations require a complete owned prepared domain.
  /// Default absence prevents existing providers from silently ignoring them.
  virtual std::shared_ptr<const SpecializationRawEnvironment>
  rawEnvironment() const {
    return {};
  }
  virtual llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) = 0;
  virtual std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) {
    return std::nullopt;
  }
  /// Describe only complete, supported LowIR query bundles. Candidate labels
  /// are not native-byte evidence. Native decoding and effect/preservation
  /// audits remain separate obligations on original instructions.
  virtual std::optional<SpecializationScalarQuery>
  scalarQuery(llvm::ArrayRef<LowOp> Operations) {
    return std::nullopt;
  }
};

struct SpecializationConstant {
  NdVar Location;
  uint64_t Value = 0;
};

struct SpecializationFrameSlot {
  /// Displacement from the entry value of FrameBaseRegister, never from its
  /// current value after a stack adjustment. Negative offsets are ordinary.
  int64_t Offset = 0;
  uint16_t Bytes = 0;
};

struct SpecializationFrameExclusion {
  /// Absolute, nonempty, nonwrapping half-open range [Begin, End).
  uint64_t Begin = 0;
  uint64_t End = 0;
};

struct SpecializationEntryFrameBounds {
  /// Caller-declared, nonempty, nonwrapping entry-relative range [Begin, End).
  /// This constrains the physical entry root; it grants no accessibility or
  /// initialization evidence for memory operations.
  int64_t Begin = 0;
  int64_t End = 0;
  /// Explicit environment preconditions: the whole frame misses each absolute
  /// range. Never inferred from observations, address provenance or file bytes.
  /// Duplicate and overlapping exclusions are valid redundant constraints.
  std::vector<SpecializationFrameExclusion> ExcludedAddressRanges;
};

struct SpecializationEntryObject {
  /// Complete physical entry register holding this object's arbitrary root.
  symbolic::SymRegisterRange Register;
  /// Nonempty, nonwrapping half-open range relative to that entry value.
  /// This grants no accessibility, initialization or loaded-pointer facts.
  int64_t Begin = 0;
  int64_t End = 0;
};

enum class SpecializationUndefinedModel : uint8_t {
  SelectedLowIR,
  /// Checked raw x64 regions only. Complete scalar sidecars supply independent
  /// architectural bit productions before control proofs. The residual keeps
  /// those productions; ordinary source and native-proof APIs refuse them.
  FreshScalarV1,
};

struct SpecializationOptions {
  /// Preserve incoming machine flags as symbolic machine-state inputs rather
  /// than claiming they are supplied by an ordinary source-language ABI.
  /// With NormalNonfaultingExecution, the raw residual also admits exact
  /// 16-byte COPY, bitwise operations, multiplication and ordinary LOAD/STORE,
  /// scalar-to-16-byte integer extensions, one-byte equality results and
  /// constant byte slices back to scalars. These retain their full width;
  /// scalar native-proof and source interfaces keep their own value contracts.
  /// The same sequential nonfaulting raw domain admits default-space CMPXCHG
  /// with a 64-bit address, 1/2/4/8-byte values and sequential consistency.
  /// Its complete ordered operation remains in the residual; source wrapping
  /// and independent native/LLVM proofs still reject ordered memory.
  bool ExplicitMachineState = false;
  /// Optional execution-profile certificates; neither is valid without the
  /// explicit machine-state interface. The binary provider owns their use.
  bool NormalNonfaultingExecution = false;
  bool X64CetDisabled = false;
  /// Retain scalar FS/GS reads with arbitrary 64-bit long-mode bases, fixed
  /// throughout supported execution unless X64LegacyGSState is declared.
  /// Requires ExplicitMachineState and
  /// NormalNonfaultingExecution. Bases may alias each other, the frame or any
  /// other address; this grants no operating-system layout or nonalias facts.
  /// Only the raw residual API supports this environment contract. V1 source
  /// wrapping and native proof APIs do not have authenticated base inputs.
  bool X64SegmentedReads = false;
  /// Retain ordinary scalar FS/GS stores with a 64-bit offset and arbitrary
  /// long-mode bases, fixed unless X64LegacyGSState is declared. Requires
  /// explicit nonfaulting machine state.
  /// Full linear addresses may alias any memory, including live return cells;
  /// offset provenance grants no frame or separation fact. This raw-residual
  /// contract is independent of X64SegmentedReads. Source and native proof
  /// interfaces without authenticated base inputs refuse it.
  bool X64SegmentedWrites = false;
  /// Raw-only SWAPGS state under an explicit kernel flags profile, nonfaulting
  /// machine state and little-endian x64. The caller declares CR4.FRED = 0
  /// throughout execution. Active GS.base and IA32_KERNEL_GS_BASE are arbitrary
  /// 64-bit inputs; only modeled SWAPGS exchanges them. FS stays fixed. These
  /// inputs may alias any memory; no observed base or separation is inferred.
  /// Segmented accesses still require their separate read/write options.
  /// Source and native proof interfaces without this state contract refuse it.
  bool X64LegacyGSState = false;
  /// Retain CR8 reads/writes with a symbolic four-bit task-priority state.
  /// Requires explicit nonfaulting machine state and a kernel flags profile.
  /// Every retained memory access must be ordinary memory without device side
  /// effects or APIC/TPR aliases; only modeled MOV-to-CR8 writes may change the
  /// priority. These are caller preconditions, never inferred from addresses.
  /// This raw-residual contract supplies no scheduling, native proof or source
  /// ABI. Possible reserved-bit writes are refused rather than truncated.
  bool X64CR8State = false;
  /// Raw-only MXCSR loads and scalar state reads/writes under explicit
  /// nonfaulting machine state. The caller declares the CPU's writable mask:
  /// 0xffbf without DAZ, or 0xffff with DAZ. Entry state is arbitrary within
  /// that mask, never a default control word. Every load/write must prove its
  /// reserved bits clear. Memory stores and other floating-point operations
  /// remain unsupported; source and native proofs lack this state contract.
  std::optional<uint32_t> X64MXCSRMask;
  /// Raw-only environment domain: these exact ordinary named PE imports have
  /// stable loader bindings, and their callees neither return nor reenter this
  /// residual graph via unwind, callbacks, context restoration or other means.
  /// No return value, side effect or pointer value is assumed. Requires
  /// explicit nonfaulting CET-disabled x64 state. Only canonical RIP-relative
  /// memory calls are admitted. Static import names alone do not prove this
  /// contract. Consumers must retain and validate ExternalExits with the raw
  /// residual; native proof and source interfaces reject even unused
  /// declarations.
  std::vector<SpecializationTerminalImport> X64TerminalImports;
  /// Explicit raw-region boundaries. Requires the raw environment receipt and
  /// excludes RequireRestoredFrameAtReturn and X64TerminalImports. All other
  /// transfers retain their ordinary exhaustive-analysis obligations.
  std::vector<SpecializationX64MachineExit> X64MachineExits;
  /// Raw-only supplemental code domain. Requires explicit nonfaulting,
  /// CET-disabled x64 state and an eight-byte entry RSP root. Ordinary domains
  /// require restored returns; X64MachineExits declares machine regions. The
  /// binary adapter requires a complete fixed primary PE and disjoint ranges,
  /// including its unmapped holes. It uses exactly the supplied preconditions,
  /// without inserting the ordinary adapter's implicit external-store domain.
  /// Writable external data remains unknown. Stable code/imports cover the
  /// entire execution under the existing sequential, no-concurrent-mutation
  /// contract. Consumers must retain and validate RawEnvironment, including
  /// unused declarations; source and native proof interfaces refuse this API.
  std::vector<SpecializationX64CodeRegion> X64CodeRegions;
  std::vector<SpecializationX64ImportBinding> X64ImportBindings;
  /// Optional native-proof flag environment. None of the booleans above
  /// implies this CPL/IOPL/flags contract. The binary proof requires the same
  /// value in its observation contract; ordinary recovery remains conservative.
  std::optional<InterpreterMachineStateProfile> X64FlagsProfile;
  SpecializationUndefinedModel UndefinedModel =
      SpecializationUndefinedModel::SelectedLowIR;
  /// Demand-driven precision refinement for unresolved control. Discovered
  /// register/frame ranges retain exhaustive finite relations. Repeatedly
  /// unresolved memory dependencies may additionally separate proven constant
  /// contexts, under the same global bounds; no input value is assumed.
  bool DiscoverControlState = false;
  /// Explicit context-key ranges, also included in bounded joint relations.
  /// Discovery may add further ranges. Unknown is valid, not an assumed zero.
  /// Other constant bytes join by intersection, so changing business values
  /// do not unroll a loop forever.
  std::vector<symbolic::SymRegisterRange> ControlRegisters;
  /// Optional entry-relative frame identity. This enables affine pointers,
  /// complete affine pointer spills, and constant frame-byte propagation.
  /// Bounded alignment refinements cover every entry-root residue; they do
  /// not assume ABI alignment or private/non-aliasing memory. The root must
  /// be an eight-byte register.
  std::optional<symbolic::SymRegisterRange> FrameBaseRegister;
  /// Optional environment precondition, never inferred from a concrete run.
  /// Requires FrameBaseRegister. Native proof APIs require the same bounds in
  /// their explicit frame contract. Absence keeps the full modular root domain.
  std::optional<SpecializationEntryFrameBounds> EntryFrameBounds;
  /// Explicit raw-domain preconditions: each complete object range misses
  /// EntryFrameBounds. The entry pointer remains symbolic. Objects may alias
  /// each other, and their roots may equal the frame root when the declared
  /// ranges are disjoint. No store exemption is implied: enable
  /// ProveStoreFrameSeparation to prove each complete access independently.
  /// Requires distinct complete x64 GPRs, excluding the frame register and
  /// overlapping EntryConstants. Ordinary source/native consumers refuse.
  std::vector<SpecializationEntryObject> EntryFrameExcludedObjects;
  /// Optional congruence domain for the entry frame root. Requires explicit
  /// machine state and an RSP FrameBaseRegister. Raw residual consumers enforce
  /// it; machine source rejects other roots before guest effects. Native proof
  /// APIs require the identical explicit alignment in their frame contract.
  std::optional<InterpreterEntryAlignment> EntryFrameAlignment;
  /// Context hints only: missing bytes remain unknown. Other frame facts join
  /// by intersection, so changing spilled business values do not unroll loops.
  std::vector<SpecializationFrameSlot> ControlFrameSlots;
  /// An outer RETURN requires the original frame-base value and no writes to
  /// its entry [0, 8) control slot. With a provider native-stack certificate,
  /// non-entry RETURN instead loads a proved exact destination and advances
  /// the stack, including certified internal imm16 cleanup. Outer cleanup
  /// and uncertified return dispatch remain unsupported.
  bool RequireRestoredFrameAtReturn = false;
  /// Opt in to additional solver queries proving that an unknown-origin
  /// memory write's complete extent misses the entry return slot. Also preserve
  /// live native return cells, saved frame addresses and current scalar frame
  /// values across non-frame, 64-bit-address stores only when each complete
  /// cell is proved disjoint under the current path. This is a precision
  /// strategy, not a nonalias assumption. Requires an active restored-frame
  /// check. Unsafe provenance and all unproved memory alias invalidation
  /// remain; proofs and preservation share global budgets. Includes the
  /// complete CMPXCHG write phase, even when comparison fails. Disabled by
  /// default so existing strategies keep their costs.
  bool ProveStoreReturnSlotSeparation = false;
  /// Prove each complete non-frame write disjoint from EntryFrameBounds under
  /// the current path and explicit frame domain before preserving its bytes.
  /// Requires a frame root and bounds. This grants no external provenance or
  /// unchecked store exemption. Unknown proofs fail closed; unproved writes
  /// retain normal alias invalidation. Applies to the complete CMPXCHG phase.
  /// Also prove separation of currently materialized absolute-memory cells
  /// from symbolic writes. Preserved constants can cross nodes by intersection;
  /// no initial memory value or global nonalias premise is introduced.
  bool ProveStoreFrameSeparation = false;
  /// Explicit source-ABI precondition: every external-origin store target
  /// range, including computed external addresses, is disjoint from the entry
  /// control slot. Frame-derived or unknown-origin addresses are never
  /// exempted by this precondition.
  /// Applies to the full CMPXCHG write phase, including comparison failure.
  bool ExternalStoresPreserveEntryReturnSlot = false;
  /// Unchecked source precondition: every external-origin STORE's entire
  /// extent is disjoint from EntryFrameBounds. Requires explicit machine state
  /// and an RSP frame root. This does not constrain external LOADs or aliasing
  /// between external pointers. Frame-derived/unknown-origin stores retain
  /// normal alias invalidation. Native proof APIs currently reject this domain.
  /// Includes the complete CMPXCHG write phase, even when comparison fails.
  bool ExternalStoresDisjointEntryFrame = false;
  /// Physical-register preconditions supplied by the caller, not values
  /// inferred from a run. Instruction-local lifter temporaries are not ABI
  /// inputs and cannot be bound here.
  std::vector<SpecializationConstant> EntryConstants;
  llvm::endianness ByteOrder = llvm::endianness::little;
  /// Binary-provider preparation budgets for authenticating fixed image
  /// bytes and loader metadata. Exhaustion is distinct from unsupported
  /// semantics; callers may increase these explicitly and retry.
  uint64_t MaxImagePreparationBytes = 64 * 1024 * 1024;
  uint64_t MaxImagePreparationRecords = 65536;
  /// Independent raw-result integrity work limits, supplied by the expected
  /// caller options, never enlarged by a stored receipt. V1 reserves eight
  /// validation/serialization passes before walking graph/evidence payloads.
  uint64_t MaxRawEnvironmentResultBytes = 64 * 1024 * 1024;
  uint64_t MaxRawEnvironmentResultRecords = 1048576;
  uint32_t MaxNodes = 4096;
  uint32_t MaxContextsPerAddress = 64;
  uint64_t MaxOperations = 262144;
  uint32_t MaxNodeEvaluations = 16384;
  uint32_t MaxIndirectTargets = 16;
  /// Keep symbolic state across this many proved-singleton control transfers
  /// per node before ordinary projection. Zero preserves existing boundaries.
  /// Multiple targets retain the CFG; all proofs and operations remain charged.
  /// Chaining can duplicate loop origins and limit automatic cutpoint
  /// inference.
  uint32_t MaxChainedTransfers = 0;
  /// End an ordinary chain before revisiting a native destination (address
  /// and decode mode). This can preserve runtime loops and reduce unrolling,
  /// but projection can lose correlations required to resolve later control.
  /// Demand replay still follows the committed instruction occurrences.
  bool StopChainingAtRepeatedDestination = false;
  /// Bounds distinct native return slots retained in one context. Native
  /// returns are physical control transfers, not assumed LIFO function exits.
  uint32_t MaxNativeReturnSlots = 64;
  /// Maximum exhaustive value sets used for immutable addresses and joint
  /// control-state projection. A partial solver enumeration is never a fact.
  uint32_t MaxImmutableReadAddresses = 16;
  uint32_t MaxControlTuples = 32;
  uint32_t MaxControlFields = 16;
  /// Refinement restarts the graph from entry. Global node, operation,
  /// evaluation, and solver-query budgets are cumulative across restarts and
  /// backward dependency replay after a failed attempt;
  /// these additionally bound discovery itself. Per-address contexts, active
  /// native return slots, fields, and tuples remain per-attempt structural
  /// limits. Context promotion adds no guest-memory reads: finite-value proof
  /// alone would not establish the accessibility of a new load.
  uint32_t MaxControlRefinements = 16;
  /// Includes dependency DAG visits and observed-predecessor traversal. Replay
  /// carries only candidate bit demands; publication needs a fresh fixed point.
  uint64_t MaxDiscoveryVisits = 65536;
  /// Global check count, per-query encoding/search limits, and per-node DAG
  /// bound. These limits also apply in builds without the optional Z3 backend;
  /// specialization uses the always-available built-in bitvector solver.
  uint64_t MaxSolverQueries = 4096;
  uint64_t MaxSolverGates = 262144;
  uint64_t MaxSolverConflicts = 10000;
  uint64_t MaxSolverPropagations = 1000000;
  uint64_t MaxSolverWatchVisits = 10000000;
  uint64_t MaxSymbolicNodes = 262144;
  /// Optional per-(address, mode) visit cap within an ordinary chain. Count its
  /// initial cursor and chained transfer destinations, not sequential
  /// fallthrough. Zero adds no cap. The legacy stop-at-repeat option
  /// takes precedence and still permits only one visit. Reaching either this
  /// cap or MaxChainedTransfers uses normal edge projection, never truncation.
  /// Dependency replay follows committed occurrences without applying the cap.
  uint32_t MaxChainedVisitsPerDestination = 0;

  /// Optional exact MBA fallback for incomplete frame-offset proofs. Zero
  /// disables it. Each attempt charges uncached readability-prefix nodes,
  /// operand edges and constant limbs plus MBA measurement units to both
  /// MaxOperations and this ceiling. Internal traversals, rebuilding and sample
  /// defect guards are not separately metered; this is not a wall-time or
  /// peak-memory bound. MaxSymbolicNodes is checked again before publishing any
  /// offset theorem. A changed exact expression may retry the ordinary frame
  /// proof once under the original predicate and cumulative solver-query limit.
  uint64_t MaxFrameOffsetNormalizationWorkPerProof = 0;

  /// Native recovery contract, independent of the build host. X64 retains
  /// the existing API default. Targets without an adapter fail explicitly;
  /// changing this field alone does not enable native recovery for that ISA.
  Arch Architecture = Arch::X64;

  bool hasRawEnvironment() const {
    return !X64CodeRegions.empty() || !X64ImportBindings.empty() ||
           !X64MachineExits.empty() || !EntryFrameExcludedObjects.empty() ||
           UndefinedModel != SpecializationUndefinedModel::SelectedLowIR;
  }

  uint32_t chainedVisitLimit() const {
    if (!MaxChainedTransfers)
      return 0;
    return StopChainingAtRepeatedDestination ? 1
                                             : MaxChainedVisitsPerDestination;
  }
};

/// Provider-owned declaration snapshot. Its hash establishes integrity against
/// independently expected Options, not authenticity of a live environment.
struct SpecializationRawEnvironment {
  uint32_t Version = 1;
  SpecializationOptions Declaration;
  va_t PrimaryImageBegin = 0;
  va_t PrimaryImageEnd = 0;
  std::string PrimaryImageDigest;
  std::string DomainDigest;
};

struct SpecializationRawEnvironmentReceipt {
  uint32_t Version = 1;
  std::shared_ptr<const SpecializationRawEnvironment> Domain;
  std::string GraphDigest;
  std::string EvidenceDigest;
  std::string IdentityDigest;
};

enum class SpecializationStatus : uint8_t {
  Complete,
  InvalidInput,
  Unsupported,
  UnresolvedControl,
  BudgetExceeded,
};

struct SpecializationOrigin {
  va_t ResidualAddress = 0;
  LowInstructionBoundary NativeInstruction;
};

struct SpecializationReadWitness {
  va_t InstructionAddress = 0;
  int OpSeq = 0;
  va_t Address = 0;
  std::vector<uint8_t> Bytes;
  std::string Evidence;
};

/// An exact native transfer or trap ending the declared analysis region. A
/// terminal import retains its physical CALL; MachineExit additionally admits
/// declared CALL/JMP/RET and resumable trap boundaries. Transfers retain the
/// actual next PC and physical stack effects in the final INDIR_BR span. A
/// trap retains its authenticated final intrinsic without a continuation
/// contract. No ordinary successor or callee state summary is implied.
/// Synthetic residual labels never supply native return addresses.
struct SpecializationExternalExit {
  uint32_t Version = 1;
  va_t ResidualAddress = 0;
  SpecializationInstruction Original;
  NdVar ExpansionScratch;
  std::string OriginalOperationDigest;
  std::string ExpandedOperationDigest;
  std::string ResidualOperationDigest;
  /// Integrity binding for identity, domain, original evidence and occurrence.
  /// This does not authenticate a changed image or the live loader binding.
  std::string IdentityDigest;
  /// Present only for an explicitly declared raw machine-region boundary.
  /// The original need not be an import or a CALL. This is a machine transfer,
  /// never a callee-effect summary or an implicit ordinary function return.
  std::optional<SpecializationX64MachineExit> MachineExit;
};

struct SpecializationResult {
  SpecializationStatus Status = SpecializationStatus::InvalidInput;
  /// Populated only on Complete, within the declared domain and boundaries.
  /// Explicit machine regions do not imply whole-function reconstruction.
  /// Incomplete exploration is never a replacement.
  LowFunc Residual;
  std::vector<SpecializationOrigin> Origins;
  std::vector<SpecializationReadWitness> Reads;
  std::vector<SpecializationExternalExit> ExternalExits;
  std::optional<SpecializationRawEnvironmentReceipt> RawEnvironment;
  std::string Diagnostic;
  /// Includes the additional pure operations generated for certified reads
  /// and finite dispatch, plus optional frame-normalization strategy units;
  /// fixed-point reevaluations and normalization retries are charged again.
  uint64_t EvaluatedOperations = 0;
  uint32_t NodeEvaluations = 0;
  uint32_t Contexts = 0;
  uint64_t SolverQueries = 0;
  uint32_t RelationalWidenings = 0;
  uint32_t DiscoveredControlFields = 0;
  uint32_t DiscoveredContextFields = 0;
  uint32_t ControlRefinements = 0;
  uint64_t DiscoveryVisits = 0;

  bool complete() const { return Status == SpecializationStatus::Complete; }
};

/// Validate every declared external transfer or trap against its complete exit
/// receipt and the explicit domain. Stripping or altering a receipt invalidates
/// this interface; the LowFunc alone is not an ordinary call-free function.
/// Raw machine regions additionally require
/// validateSpecializationRawEnvironment; this exit-only check does not
/// authenticate their full graph or entry marker. As with the provider itself,
/// the fixed-image evidence is trusted input here; this does not reopen the
/// image or authenticate its current runtime binding.
llvm::Error
validateSpecializationExternalExits(const SpecializationResult &Result,
                                    const SpecializationOptions &Options);

/// Validate the complete raw result against independently expected caller
/// declarations. Requires Complete for a raw domain; authenticates its marker,
/// full graph, origins, reads and external exits. Any mutation requires a new
/// recovery. This is not native equivalence or proof of live binding stability.
llvm::Error
validateSpecializationRawEnvironment(const SpecializationResult &Result,
                                     const SpecializationOptions &Options);

/// Provider-neutral partial evaluation of strictly lifted integer/control
/// LowIR. Uncertified ordinary memory effects remain in the residual CFG.
/// Exhaustively covered, certified immutable reads may become pure selections.
/// Provider-certified native near CALL/RETURN uses physical stack semantics;
/// ordinary calls without that certificate are refused. Opaque semantics other
/// than retained x64 runtime flag snapshots and restores, ordered memory, and
/// unsupported instruction guards are refused.
/// Every temporary read must have a complete definition in the same lifted
/// native instruction; temporary offsets may be reused by later instructions.
/// A residual multi-target dispatch snapshots a temporary target into an
/// explicit function-local temporary before crossing instruction boundaries.
/// Flag snapshots are treated as unknown values for proof and may not certify
/// an external pointer or finite target on their own. A PUSHFQ snapshot needs
/// prior definitions for every modelled flag unless ExplicitMachineState
/// supplies their incoming values. Guest cursors retain all 64 address bits;
/// InvalidVA remains a sentinel. Synthetic labels identify clones in a checked
/// range disjoint from explored native instruction extents and the entry
/// anchor. Only CFG identities are renamed, never runtime data or native return
/// words. NativeInstruction preserves provenance without copying stale address-
/// occurrence certificates.
SpecializationResult
specializeInterpreter(SpecializationProvider &Provider,
                      SpecializationCursor Entry,
                      const SpecializationOptions &Options = {});

} // namespace neverd::analysis

#endif // NEVERD_ANALYSIS_INTERPRETERSPECIALIZATION_H
