//===- LowIRRefinement.h - Selected-value program refinement ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LOWIRREFINEMENT_H
#define NEVERD_ANALYSIS_LOWIRREFINEMENT_H

#include "neverd/analysis/LowIRUndefinedIndependence.h"

#include "llvm/Support/Error.h"

namespace neverd::analysis {

/// An explicit, constructive choice at each original undefined producer.
/// Neither policy describes the undefined-bit choice of a physical processor.
enum class LowIRRefinementWitness : uint8_t {
  /// Select the bits already computed by ordinary deterministic lifting.
  /// An unbound instruction temporary cannot supply this witness.
  LiftedBits,
  /// Select zero only for the active undefined bits; retain every other bit.
  ZeroBits,
};

struct LowIRRefinementLimits {
  /// Shared execution/query/observation budgets across both programs and the
  /// final relation. Input graph metadata limits apply to each graph.
  LowIRIndependenceLimits Execution;
  uint64_t MaxTerminalPairs = 65536;
};

enum class LowIRRefinementStatus : uint8_t {
  Proved,
  /// The supplied witness and, if present, loop plan do not establish the
  /// requested relation. A rejected plan need not imply differing outputs.
  /// This does not disprove refinement under a different witness.
  Different,
  Unsupported,
  Invalid,
  BudgetExceeded,
  InfeasibleEntry,
  ContractViolation,
};

enum class LowIRRefinementScope : uint8_t {
  CompleteFiniteLowIRPaths,
  CompleteFiniteNativeToLowIRPaths,
  InductiveLowIRLoops,
  InductiveNativeToLowIRLoops,
};

enum class LowIRLoopSpace : uint8_t {
  Register,
  Frame,
  SystemFlags,
  /// Declared storage on the selected LowIR side, defined by a checked prefix.
  /// Native state and the shared function entry have no such storage.
  FunctionTemporary,
};
enum class LowIRLoopSide : uint8_t {
  Entry,
  Original,
  Candidate,
  /// The checked, captured prefix of this cutpoint. Requires UseEntryPrefix;
  /// these are fixed prefix values, never fresh induction parameters.
  OriginalPrefix,
  CandidatePrefix,
};

struct LowIRLoopLocation {
  LowIRLoopSpace Space = LowIRLoopSpace::Register;
  /// Register or function-temporary byte offset; for Frame, the
  /// two's-complement entry-root offset. SystemFlags requires Offset == 0 and
  /// Bytes == 8 and a native profile.
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
};

struct LowIRLoopInput {
  LowIRLoopSide Side = LowIRLoopSide::Entry;
  LowIRLoopLocation Location;
  NdVar Temporary;
};

struct LowIRLoopAssignment {
  LowIRLoopLocation Location;
  NdVar Value;
};

/// A current-state selector, not an entry assumption or invariant. Only
/// Register, entry-root-relative Frame and SystemFlags locations are allowed.
/// Mask is nonzero and fits the location; Value has no bits outside Mask.
struct LowIRLoopGuard {
  LowIRLoopLocation Location;
  uint64_t Mask = 0, Value = 0;
};

/// A candidate state template, never an assumed invariant. Start with the real
/// shared entry state (or the checked prefix selected below), then apply the
/// assignments separately to each program.
/// Entry inputs read that snapshot. Other inputs are arbitrary induction
/// parameters whose projections back from the constructed state are checked.
/// Expressions are side-effect-free scalar LowIR over constants and bound
/// temporaries, in a namespace separate from either program. Predicate must be
/// a canonical byte Boolean. Rank is a nonempty unsigned lexicographic tuple.
struct LowIRLoopCutpoint {
  /// LowIR block entries, or an original native instruction entry. Each side's
  /// addresses are unique unless every cut sharing one has explicit guards.
  /// A segment stops before executing its next selected cut.
  va_t OriginalAddress = 0;
  va_t CandidateAddress = 0;
  /// Use the first feasible paired entry-prefix arrival as the template base
  /// instead of the function entry state. By default its path predicate is
  /// retained and proved on every arrival. A cut behind earlier cuts is reached
  /// by a separate bounded replay from the real entry. That replay establishes
  /// only a feasible paired witness; complete entry/transition coverage is
  /// still required. No abstract state is invented for an unreached cut.
  /// Defined function-temporary bytes retain this prefix's expressions unless
  /// explicitly assigned. Temporary inputs and assignments require declared,
  /// prefix-defined bytes on their own side; they cannot initialize storage.
  /// Every arrival checks exact definedness and all values, and arbitrary
  /// temporary inputs must pass the same projection checks as other state.
  bool UseEntryPrefix = false;
  std::vector<LowIRLoopInput> Inputs;
  std::vector<LowOp> Expressions;
  std::vector<LowIRLoopAssignment> OriginalState, CandidateState;
  NdVar Predicate = NdVar::scalar(1, 1);
  std::vector<NdVar> Rank;
  /// Requires UseEntryPrefix. Treat the captured state expressions as total,
  /// untrusted template functions outside the witness's path domain, without
  /// assuming its path predicate. This does not replay the prefix on that
  /// larger domain: all real arrivals must match the full template, and the
  /// expanded induction domain must pass projection, coverage, transition,
  /// observation and strict rank checks. A feasible paired prefix is required.
  bool GeneralizeEntryPrefix = false;
  /// Each side selects this cut when all masked equalities hold on its current
  /// state. Empty lists select unconditionally. At a shared address, feasible
  /// selections must be disjoint; unmatched states continue normal execution.
  /// The reconstructed induction state must prove its own selector before a
  /// segment starts. Complete entry/transition coverage is still mandatory.
  std::vector<LowIRLoopGuard> OriginalGuards, CandidateGuards;
};

struct LowIRLoopRefinementPlan {
  /// Covers every cycle needed by this proof. A missed cycle exhausts finite
  /// segment exploration and cannot be treated as an inductive edge.
  std::vector<LowIRLoopCutpoint> Cutpoints;
};

/// Propose equality between induction inputs at two full-width locations.
/// Both bindings are retained; the refinement checker must validate their
/// projections and every real arrival at the paired cutpoint.
struct LowIRLoopInputPair {
  LowIRLoopLocation Original, Candidate;
};

struct LowIRLoopCutpointPair {
  va_t OriginalAddress = 0, CandidateAddress = 0;
  std::vector<LowIRLoopInputPair> SharedInputs;
};

/// Combine two independently proposed self-relation plans into one untrusted
/// original/candidate proposal. Pairings must cover each plan exactly once.
/// Each input plan uses identical original/candidate addresses, assignments
/// and guards. Addresses remain unique even when guarded. Inputs use
/// Entry/Original/OriginalPrefix, with nonoverlapping temporary
/// definitions with exact-width uses. Prefix policies must agree on
/// UseEntryPrefix; GeneralizeEntryPrefix is enabled if either proposal requires
/// it.
///
/// Temporaries and input bindings are renamed independently. Equality
/// predicates couple explicitly paired Original inputs without removing
/// either projection. Candidate-side prefix inputs remain candidate-side
/// snapshots. Both predicates are retained, with the original plan's rank.
/// No solver runs here, no invariant is assumed, and no certificate is
/// produced. The complete original/candidate checker remains mandatory,
/// including input domain, state/frame equality, coverage, termination and
/// observation checks. MaxMetadata bounds the total input cuts, bindings,
/// expressions, assignments, ranking components and requested pairs before
/// construction allocates copies, including both sides' guard entries.
llvm::Expected<LowIRLoopRefinementPlan>
pairLowIRLoopRefinementPlans(const LowIRLoopRefinementPlan &Original,
                             const LowIRLoopRefinementPlan &Candidate,
                             llvm::ArrayRef<LowIRLoopCutpointPair> Pairings,
                             uint64_t MaxMetadata = 65536);

enum class LowIRLoopInferenceStatus : uint8_t {
  Inferred,
  Unsupported,
  Invalid,
  BudgetExceeded,
};

struct LowIRLoopInferenceLimits {
  /// Shared across prefix exploration, widening and rank queries. Inference
  /// has its own explicit budget; it never increases the subsequent proof's.
  LowIRIndependenceLimits Execution;
  uint32_t MaxCutpointAttempts = 16;
  uint32_t MaxWideningRounds = 8;
  uint32_t MaxRankCandidates = 128;
  /// Extra analysis for filtered branch arms and native context selectors:
  /// graph lookups, common-path sets, literal-bit scans and comparisons.
  uint64_t MaxCutSelectionWork = 262144;
};

struct LowIRLoopInferenceResult {
  LowIRLoopInferenceStatus Status = LowIRLoopInferenceStatus::Unsupported;
  /// An untrusted proposal, not a refinement certificate. Original addresses
  /// initially equal candidate addresses; a native caller must map them and
  /// check the complete original/candidate relation.
  std::optional<LowIRLoopRefinementPlan> Plan;
  std::string Diagnostic;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  int OpSeq = -1;
  uint64_t Operations = 0;
  /// Charged query requests, including completed entailment reuse.
  uint32_t SolverQueries = 0;
  /// Completed model-free entailments reused within this inference session.
  /// These requests remain included in SolverQueries and its shared limit.
  uint32_t EntailmentCacheHits = 0;
  uint64_t ScheduledPaths = 0;
  /// Cumulative traversal work, also bounded by Execution.MaxSymbolicNodes.
  uint64_t PredicateNodes = 0;
  uint32_t CutpointAttempts = 0;
  uint32_t WideningRounds = 0;
  uint32_t RankCandidates = 0;
  /// Optional selector work, including failed attempts. Alignment charges
  /// this against its remaining MaxSearchWork as well as the stage limit.
  uint64_t CutSelectionWork = 0;

  bool inferred() const {
    return Status == LowIRLoopInferenceStatus::Inferred && Plan.has_value();
  }
};

/// Propose a loop template by symbolic prefix exploration and bounded
/// widening in the same executor used by refinement. Searches guarded loop
/// bodies and CFG backedge targets, retaining fixed prefix bits and unsigned
/// bounds. Nested cycles use a feedback cutpoint set, pruned prefix bounds,
/// observed unit counters and inferred phases in unsigned lexicographic ranks.
/// For multiple cuts, each failed counter tuple also tries a leading constant
/// phase, charged as a separate rank attempt. It can cover sequential counter
/// resets only when nonincreasing phase constraints have no positive cycle.
/// Every feasible transition still proves the complete tuple decreases.
/// Unit recurrences can update a byte-aligned lane while preserving all other
/// bits. Multi-cut templates propose endpoint exclusions for such lanes only
/// after proving them on saved and current arrivals; widening prunes failures
/// without reseeding them. Full-word ranks and observations remain unchanged.
/// Every cycle must cross a selected cut. Unreachable prefixes, unmatched
/// control and rank families outside this bounded search remain unsupported.
/// Single-cut coverage checks include every originally reachable block and
/// consume cutpoint attempts before symbolic execution. Scalar counters with
/// unit progress on every returning edge retain priority. Up to eight observed
/// unit-counter tuple proposals then alternate with broader scalar hypotheses.
/// After the remaining scalar hypotheses, tuple search resumes without replay
/// on the complete stable template. Order is independent of budget caps;
/// every attempt shares the same cumulative inference budgets.
/// Single-cut search may retry another eligible cut after a generalized
/// template violates a contract. Real-prefix failures, malformed inputs,
/// unsupported execution and shared-budget exhaustion still stop the search.
/// A nonempty EligibleCutpoints restricts only the search, not execution or
/// the admitted input domain. No native/source equivalence follows from an
/// inferred plan; pass it to the original/candidate checker before use.
LowIRLoopInferenceResult
inferLowIRLoopRefinementPlan(const LowFunc &Candidate,
                             const LowIRIndependenceContract &Contract,
                             const LowIRLoopInferenceLimits &Limits = {},
                             llvm::ArrayRef<va_t> EligibleCutpoints = {});

struct LowIRRefinementProducer {
  /// Unique instruction visit and sidecar index, including revisited loops.
  uint64_t InstructionVisit = 0;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  uint64_t EffectIndex = 0;
};

struct LowIRRefinementCertificate {
  LowIRRefinementScope Scope = LowIRRefinementScope::CompleteFiniteLowIRPaths;
  std::string InputDigest;
  std::string OriginalDigest;
  std::string CandidateDigest;
  LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits;
  LowIRIndependenceContract Contract;
  LowIRRefinementLimits Limits;
  std::vector<LowIRUndefinedInstruction> OriginalInstructions;
  std::vector<LowIRRefinementProducer> Producers;
  std::vector<LowIRNativeFlagTransition> NativeFlagTransitions;
  std::vector<LowIRNativeProfileProjection> NativeProfileProjections;
  std::vector<LowIRNativeAuditBoundary> NativeAuditBoundaries;
  /// Present only for inductive scopes. Binds the exact templates, predicates,
  /// projections and rankings that were checked, including their limits.
  std::optional<LowIRLoopRefinementPlan> LoopPlan;
  /// Covers original native executions under Witness, never arbitrary
  /// architecture-allowed undefined choices or candidate-native state.
  std::optional<LowIRNativePreservationCertificate> NativePreservation;
};

struct LowIRRefinementResult {
  LowIRRefinementStatus Status = LowIRRefinementStatus::Invalid;
  std::optional<LowIRRefinementCertificate> Certificate;
  std::string Diagnostic;
  int BlockId = -1;
  va_t InstructionAddress = 0;
  int OpSeq = -1;
  uint64_t Operations = 0;
  uint64_t Instructions = 0;
  /// Complete terminal paths in finite scopes; terminal segments in inductive
  /// scopes. The latter count is not the number of full program executions.
  uint32_t OriginalPaths = 0;
  uint32_t CandidatePaths = 0;
  uint32_t BlockVisits = 0;
  uint32_t Producers = 0;
  uint32_t SolverQueries = 0;
  uint64_t Observations = 0;
  /// Endpoint pairs, including cutpoint arrivals in inductive scopes.
  uint64_t TerminalPairs = 0;
  uint64_t OriginalCutpoints = 0;
  uint64_t CandidateCutpoints = 0;
  uint64_t LoopInitiations = 0;
  uint64_t LoopTransitions = 0;
  uint64_t RankingChecks = 0;

  bool proved() const {
    return Status == LowIRRefinementStatus::Proved && Certificate.has_value();
  }
};

/// Compare a deterministic candidate against an original with explicit
/// architecture-undefined effects. For every admitted ordinary entry input,
/// the selected original execution and candidate must terminate and agree on
/// RETURN operands, requested registers and the union of written frame bytes.
/// Both must meet the preservation contract. Control structure may differ.
///
/// This constructs an ISA-allowed refinement witness, not independence from
/// all undefined choices, CPU-specific equality, or a native/source proof.
/// Candidate is semantic LowIR: no architecture sidecars are inferred for it.
/// Instruction boundaries and temporary lifetimes must still be valid.
/// Declared function temporaries start unbound and need a reaching definition
/// on each executed path; other temporaries remain instruction-local. This API
/// requires complete finite execution of loops within the shared budgets.
/// The inductive API can retain checked, fixed entry-prefix temporaries.
/// Every feasible
/// terminal pair and complete entry-domain coverage are checked. Incomplete
/// exploration, unknown aliases and unsupported operations refuse a
/// certificate. Hashes bind all inputs and limits; rerun the check to validate
/// changed inputs.
LowIRRefinementResult checkLowIRRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &Limits = {});

/// Prove entry initiation, every feasible segment successor, invariant
/// preservation, terminal observations and total termination. Both sides must
/// reach corresponding cuts or both return. Every cut-to-cut transition must
/// strictly decrease the same finite unsigned lexicographic rank (matching
/// component widths at all cuts). Finite segment exploration must cover its
/// entire domain; a successful sibling cannot hide an unfinished path.
///
/// The template checks all modified registers, the entire accessible frame,
/// and defined function-temporary bytes at cuts. Inductive terminal
/// observations include the entire frame when
/// written-byte observation is requested, a conservative strengthening that
/// retains writes made in earlier iterations. Templates, predicates and ranks
/// are explicit proof hints; they do not restrict the admitted entry domain.
/// No loop certificate is produced by the finite API or the strict
/// independence API. This still does not certify a C backend or physical CPU.
LowIRRefinementResult checkLowIRLoopRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopRefinementPlan &Plan,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &Limits = {});

struct LowIRLoopAlignmentLimits {
  LowIRLoopInferenceLimits OriginalInference, CandidateInference;
  LowIRRefinementLimits Proof;
  /// Shared by every inference and proof, including failed attempts. Each
  /// invocation receives the lesser of its stage limit and the remainder.
  uint64_t MaxSolverQueries = 65536;
  /// Cumulative graph visits, optional cut-selection work, input comparisons
  /// and pairing metadata. Charged before constructing search containers;
  /// child executors keep their operation, path and symbolic-node limits.
  uint64_t MaxSearchWork = 262144;
  /// One shared pool for all retained original/candidate family plans; also
  /// bounds each individual pairing construction. Child inference keeps its
  /// own limits, so this is not a bound on all simultaneously live metadata.
  uint64_t MaxMetadata = 65536;
  /// Includes default, broad and filtered branch-arm candidate inference,
  /// then cyclic singletons. Empty or duplicate families consume an attempt.
  uint32_t MaxCandidateAttempts = 33;
  uint32_t MaxPairingAttempts = 128;
  uint32_t MaxCuts = 3;
  /// Optional direct-rank and rank-plus-frame proposals after each failed
  /// ordinary pairing. Zero disables them without changing legacy search.
  /// Each policy consumes this and MaxPairingAttempts before construction,
  /// including ineligible or duplicate proposals. Exhaustion skips optional
  /// work but allows later ordinary attempts within the remaining budgets.
  uint32_t MaxRankPairingAttempts = 0;
};

enum class LowIRLoopAlignmentStatus : uint8_t {
  Proved,
  /// No relation found in this search family; not a proof of inequivalence.
  Unsupported,
  Invalid,
  BudgetExceeded,
};

struct LowIRLoopAlignmentResult {
  LowIRLoopAlignmentStatus Status = LowIRLoopAlignmentStatus::Unsupported;
  /// The last complete-checker result, using the caller's original records
  /// and witness unchanged. Only a successful fresh check contains a receipt.
  LowIRRefinementResult Refinement;
  std::string Diagnostic;
  std::string LastCandidateDiagnostic;
  uint64_t SolverQueries = 0;
  uint64_t SearchWork = 0;
  uint32_t CandidateAttempts = 0;
  uint32_t PairingAttempts = 0;
  uint32_t RankPairingAttempts = 0;

  bool proved() const {
    return Status == LowIRLoopAlignmentStatus::Proved && Refinement.proved();
  }
};

/// Search default, broad and filtered branch-arm self-plans on both sides,
/// first pairing the same families, then crossing them. The filtered family
/// omits branches whose paths all reconverge at a nonterminal node before any
/// DFS backedge boundary; exits and boundary successors remain in the test.
/// Branch-arm cuts preserve action phases; greedy additions cover remaining
/// cycles. Cache successes and failures under one retained-metadata budget;
/// optional families skip cuts duplicated by any already cached self-plan.
/// Then try individual cyclic candidate entries against available original
/// plans. Cut permutations are lazy; this does not enumerate every feedback
/// set or rank family. Standalone default inference is unchanged.
/// By default, only same-width frame inputs at identical offsets are proposed
/// equal. MaxRankPairingAttempts additionally matches direct Original inputs
/// at corresponding rank-tuple positions, alone and with compatible frame
/// bindings. Constant/expression ranks and width conversions are not matched.
/// Duplicate binding sets are skipped; affine relations remain explicit
/// pairing tasks. All proposals go through pairLowIRLoopRefinementPlans and
/// the complete checkLowIRLoopRefinement with the supplied audited records.
/// A failed proposal never proves inequivalence. Per-attempt exhaustion may
/// retry within the remaining total budgets; global exhaustion stops search.
/// This API neither certifies native lifting nor proves compiler correctness.
LowIRLoopAlignmentResult inferAndCheckLowIRLoopRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRLoopAlignmentLimits &Limits = {});

} // namespace neverd::analysis

#endif
