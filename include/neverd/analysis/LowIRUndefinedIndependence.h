//===- LowIRUndefinedIndependence.h - Bounded relational proof ----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LOWIRUNDEFINEDINDEPENDENCE_H
#define NEVERD_ANALYSIS_LOWIRUNDEFINEDINDEPENDENCE_H

#include "neverd/analysis/InterpreterEntryAlignment.h"
#include "neverd/analysis/InterpreterMachineStateProfile.h"
#include "neverd/ir/low/LowPreservedState.h"
#include "neverd/ir/low/LowUndefinedEffects.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymState.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::analysis {

/// Bind architecture evidence to one complete instruction, never just a PC.
struct LowIRUndefinedInstruction {
  int BlockId = -1;
  LowInstructionBoundary Boundary;
  LowInstructionUndefinedEffects Effects;
};

struct LowIRIndependenceConstant {
  NdVar Location;
  uint64_t Value = 0;
};

struct LowIRIndependenceAddressRange {
  /// Absolute, nonempty, nonwrapping half-open address range [Begin, End).
  uint64_t Begin = 0;
  uint64_t End = 0;
};

struct LowIRIndependenceFrame {
  /// The current exact-frame model uses a 64-bit entry address register.
  symbolic::SymRegisterRange RootRegister;
  /// Accessible, nonwrapping bytes relative to the entry root: [Begin, End).
  /// This is an explicit nonfaulting environment contract, not an alias fact
  /// inferred from executing one input. Only this mutable region is supported.
  int64_t Begin = 0;
  int64_t End = 0;
  /// The whole accessible frame must be disjoint from every excluded absolute
  /// range. This restricts the symbolic entry root, not just constant roots.
  /// Duplicate and overlapping exclusions are valid redundant constraints.
  std::vector<LowIRIndependenceAddressRange> ExcludedAddressRanges;
  /// Optional caller-declared congruence of the shared entry root. The proof
  /// intersects this predicate with bounds, exclusions and entry constants;
  /// it never rewrites a root to make an incompatible entry satisfy it.
  /// Absence admits every residue. This supplies no memory or ABI evidence.
  std::optional<InterpreterEntryAlignment> EntryAlignment;
};

struct LowIRIndependenceFrameRange {
  /// Byte range relative to the shared entry frame root.
  int64_t Offset = 0;
  uint16_t Bytes = 0;
};

enum class LowIRNativePreservationQuantifier : uint8_t {
  /// Every execution admitted by the refinement's named undefined witness.
  /// Two selected-witness results cannot establish AllUndefinedChoices.
  SelectedWitness,
  AllUndefinedChoices,
};

struct LowIRNativePreservationRequirement {
  LowPreservedStateSet StateSet = LowPreservedStateSet::LegacyIntegerOpaqueV1;
  LowIRNativePreservationQuantifier Quantifier =
      LowIRNativePreservationQuantifier::AllUndefinedChoices;
};

/// Original native execution only, not candidate LowIR or an ordinary ABI.
/// Issued only after complete execution/relation checking. ExecutionDigest
/// binds every checked original fact to its executed span, physical expansion
/// and segment; Instructions counts executed visits, not collected entries.
struct LowIRNativePreservationCertificate {
  LowPreservedStateSet StateSet = LowPreservedStateSet::None;
  uint32_t SemanticsVersion = 1;
  LowIRNativePreservationQuantifier Quantifier =
      LowIRNativePreservationQuantifier::SelectedWitness;
  uint64_t Instructions = 0;
  std::string ExecutionDigest;
};

struct LowIRIndependenceContract {
  /// Native-only opt-in. Initializes canonical shared entry flags and always
  /// observes final system flags, independently of ReturnRegisters and frame
  /// observations. The static LowIR API refuses a nonempty profile.
  std::optional<InterpreterMachineStateProfile> X64FlagsProfile;
  std::vector<LowIRIndependenceConstant> EntryConstants;
  std::optional<LowIRIndependenceFrame> Frame;
  std::vector<symbolic::SymRegisterRange> ReturnRegisters;
  /// Each execution must restore these one-to-eight-byte register ranges to
  /// their shared entry values at every RETURN, after EntryConstants apply.
  /// Overlaps within this list are invalid; ReturnRegisters may overlap it.
  std::vector<symbolic::SymRegisterRange> PreservedRegisters;
  /// Each execution must restore these entry-frame bytes at every RETURN.
  /// Requires Frame; ranges must be nonempty, contained in Frame and mutually
  /// disjoint. This obligation applies even when written-byte observation is
  /// disabled. Temporarily modifying and then restoring a range is allowed.
  std::vector<LowIRIndependenceFrameRange> PreservedFrameRanges;
  bool ObserveWrittenFrameBytes = true;
  llvm::endianness ByteOrder = llvm::endianness::little;
  /// Native proofs only. Retain strictly lifted instructions with
  /// Missing effect coverage as explicit refusal boundaries. Success requires
  /// proving them unreachable; reaching one never executes its LowIR. Static
  /// LowIR APIs without a native provider reject this option. Every inductive
  /// segment rechecks unreachability over its complete domain. In selected-
  /// value refinement, unreachability is relative to the declared witness.
  bool RetainUnauditedNativeBoundaries = false;
  /// Finite native proofs only. Different instruction entry addresses may
  /// cover the same immutable bytes when every overlapping byte agrees.
  /// Each entry still requires its own complete boundary and semantic
  /// evidence. Static LowIR and inductive loop APIs reject this option.
  bool AllowOverlappingNativeInstructions = false;
  /// Native proofs only. Collect conditional successors only after
  /// their existing paired-control and feasibility checks. Every feasible
  /// destination still needs complete byte and semantic evidence; skipped
  /// arms have no instruction-inventory claim. The default eagerly audits
  /// both arms. Every inductive segment checks its entire domain. Static
  /// LowIR APIs without a native provider reject this option.
  bool DeferNativeConditionalEdges = false;
  /// Native-only opt-in; absent preserves the existing contract. Strict finite
  /// independence can cover all undefined choices. Selected refinement,
  /// including induction, refuses an AllUndefinedChoices request. Static
  /// LowIR cannot establish architectural effects omitted from its input.
  std::optional<LowIRNativePreservationRequirement> NativePreservedState;
};

struct LowIRIndependenceLimits {
  uint64_t MaxOperations = 65536;
  /// Bounds both input metadata and dynamically visited instructions, including
  /// instructions whose lifted operation spans are empty and frame exclusions.
  uint64_t MaxInstructions = 65536;
  /// Total path states scheduled, including straight-line block successors.
  /// Native traces group up to 32 contiguous instructions per scheduled block;
  /// instruction, operation and input-metadata budgets remain independent.
  uint32_t MaxPaths = 256;
  uint32_t MaxBlockVisits = 4096;
  uint32_t MaxProducers = 4096;
  uint32_t MaxFrameBytes = 4096;
  uint32_t MaxSolverQueries = 4096;
  /// Complete native target sets; solver enumeration requires a final
  /// no-more-values query. Partial target sets never authorize a proof.
  uint32_t MaxIndirectTargets = 16;
  /// Complete native immutable-load address sets. Every candidate needs
  /// immutable bytes and frame nonalias evidence; partial sets prove nothing.
  uint32_t MaxImmutableLoadAddresses = 16;
  /// Bounds checked equalities and the total bytes snapshotted for return
  /// preservation contracts. Each preserved item is checked in both executions.
  uint64_t MaxObservations = 65536;
  uint64_t MaxSymbolicNodes = 262144;
  solver::SolverOptions Solver = [] {
    solver::SolverOptions Value;
    Value.Blast.MaxGates = 262144;
    Value.Sat.MaxConflicts = 10000;
    Value.Sat.MaxPropagations = 1000000;
    Value.Sat.MaxWatchVisits = 10000000;
    return Value;
  }();
  /// With overlapping native entries enabled, bounds the total instruction
  /// byte evidence. Each newly fetched entry charges its full size, including
  /// bytes shared with earlier entries. Immutable reads retain their separate
  /// evidence budget. Exhaustion never authorizes a partial proof.
  uint64_t MaxNativeInstructionBytes = 1048576;
};

enum class LowIRIndependenceStatus : uint8_t {
  Proved,
  Dependent,
  Unsupported,
  Invalid,
  BudgetExceeded,
  InfeasibleEntry,
  ContractViolation,
};

enum class LowIRIndependenceScope : uint8_t {
  CompleteAcyclicLowIR,
  /// Original native instructions with physical near-call/return expansion.
  /// Loops require complete finite unrolling, not an invariant assumption.
  /// Every feasible path must finish; an exhausted prefix proves nothing.
  CompleteFiniteNativePaths,
};

struct LowIRNativeFlagTransition {
  int BlockId = -1;
  va_t InstructionAddress = 0;
  int OpSeq = -1;
  uint32_t SemanticsVersion = 0;
  /// Digest of the exact shared scalar transition in its isolated input/output
  /// namespace. Original intrinsics remain bound by the instruction evidence.
  std::string OperationDigest;
};

struct LowIRNativeProfileProjection {
  /// -1 records an original profile-dependent trap that must stay unreachable;
  /// otherwise this identifies a visited block in the finite native trace.
  int BlockId = -1;
  va_t InstructionAddress = 0;
  InterpreterProfileProjection Kind = InterpreterProfileProjection::None;
};

enum class LowIRNativeAuditBoundaryKind : uint8_t {
  MissingUndefinedOutputs = 1,
  MissingPreservedState = 2,
  MissingUndefinedOutputsAndPreservedState = 3,
};

/// A retained refusal boundary, never an assertion of instruction semantics.
/// The enclosing native certificate also retains the exact original bytes and
/// operations. No claim is made about uncollected bytes after this boundary.
struct LowIRNativeAuditBoundary {
  LowIRNativeAuditBoundaryKind Kind =
      LowIRNativeAuditBoundaryKind::MissingUndefinedOutputs;
  uint32_t SemanticsVersion = 1;
  LowInstructionBoundary Boundary;
  std::string NativeBytesDigest;
  std::string OperationDigest;
};

struct LowIRIndependenceCertificate {
  LowIRIndependenceScope Scope = LowIRIndependenceScope::CompleteAcyclicLowIR;
  /// SHA256 of the checked operations, CFG, boundaries, sidecars, contract and
  /// proof limits. Recheck the supplied inputs before reusing a certificate;
  /// this digest detects stale inputs, not an untrusted producer of proofs.
  /// Native scope binds the expanded execution trace; recheck its original
  /// image through the native driver rather than the static LowIR API.
  std::string InputDigest;
  std::vector<LowIRUndefinedInstruction> Instructions;
  LowIRIndependenceContract Contract;
  LowIRIndependenceLimits Limits;
  std::vector<LowIRNativeFlagTransition> NativeFlagTransitions;
  std::vector<LowIRNativeProfileProjection> NativeProfileProjections;
  std::vector<LowIRNativeAuditBoundary> NativeAuditBoundaries;
  std::optional<LowIRNativePreservationCertificate> NativePreservation;
};

struct LowIRIndependenceResult {
  LowIRIndependenceStatus Status = LowIRIndependenceStatus::Invalid;
  std::optional<LowIRIndependenceCertificate> Certificate;
  std::string Diagnostic;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  int OpSeq = -1;
  uint64_t Operations = 0;
  uint64_t Instructions = 0;
  uint32_t Paths = 0;
  uint32_t BlockVisits = 0;
  uint32_t Producers = 0;
  uint32_t SolverQueries = 0;
  uint64_t Observations = 0;

  bool proved() const { return Status == LowIRIndependenceStatus::Proved; }
};

/// Prove that all control predicates, memory addresses, RETURN operands and
/// requested final observations are independent of architecture-arbitrary
/// choices. Ordinary entry inputs are shared; each effect creates independent
/// left/right choices that stay correlated with their own copies and spills.
/// Preserved locations additionally must equal their shared entry snapshot in
/// each execution at every RETURN; pairwise independence alone is insufficient.
///
/// Starts at Function.Entry; additional declared entry roots are unsupported.
/// The driver checks a branch before constraining its path. Calls, opaque
/// operations, reachable cycles, unknown aliases, unsupported modes and missing
/// architecture coverage refuse proof. Path exhaustion is never success.
/// This certifies the supplied LowIR/effects only, not native lifting, source
/// translation, termination of arbitrary loops, or a processor's undefined-bit
/// choice. It must not be applied only after uncertified control pruning.
LowIRIndependenceResult checkLowIRUndefinedIndependence(
    const LowFunc &Function,
    llvm::ArrayRef<LowIRUndefinedInstruction> Instructions,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits = {});

} // namespace neverd::analysis

#endif
