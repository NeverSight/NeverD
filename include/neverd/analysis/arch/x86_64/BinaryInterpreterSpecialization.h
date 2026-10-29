//===- BinaryInterpreterSpecialization.h - Image adapter --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BINARYINTERPRETERSPECIALIZATION_H
#define NEVERD_ANALYSIS_BINARYINTERPRETERSPECIALIZATION_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/analysis/LowIRRefinement.h"
#include "neverd/analysis/LowIRUndefinedIndependence.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::analysis {

/// Specialize a linked x64 interpreter at the image's mapped addresses.
/// The execution contract fixes mappings and permissions, excludes concurrent
/// code/data mutation and external calls, and requires bytecode reads to come
/// from immutable file-backed mappings. This is source recovery, not a binary
/// replacement or an exception/unwind equivalence certificate.
/// Complete PE inputs require loader-owned preferred-base snapshot evidence:
/// full DIR64 fields and ordinary import write footprints are validated;
/// IAT reads, additional loader writers and changed mappings are refused.
/// MaxImagePreparationBytes/Records bound that authentication and return
/// BudgetExceeded on exhaustion. This does not prove ASLR or initialization
/// equivalence. Native relation checks use the same fresh image evidence.
/// Ordinary ABI return behavior is required: every external-origin store's
/// target range, including addresses computed from external integers, is
/// disjoint from the entry return-address slot. This is an environment
/// precondition, not a consequence of absent frame provenance. Root-derived
/// writes must prove disjointness; stack pivots and return dispatch are
/// refused in the default mode. ExplicitMachineState admits provider-certified
/// near calls and exact returns; NormalNonfaultingExecution excludes exception
/// dispatch rather than claiming its equivalence. X64CetDisabled certifies
/// RDSSP destination preservation, never arbitrary CET instruction support.
/// Optional EntryFrameBounds restrict the physical entry root to a nonwrapping
/// interval; they do not authorize memory accesses. Native relation checks
/// require exactly matching bounds in their explicit frame contract.
SpecializationResult
specializeBinaryInterpreter(const BinaryImage &Image, va_t Entry,
                            const SpecializationOptions &Options = {});

/// Evidence for complete original native execution paths, before recovery.
/// This is an undefined-state independence certificate, not a proof of the
/// lifter, a processor's undefined-bit choice, or native-to-source equivalence.
struct BinaryUndefinedIndependenceCertificate {
  /// Binds exact native bytes, their immutable mappings, execution profile and
  /// the LowIR certificate. Re-run the check to validate changed inputs; a hash
  /// alone is not a proof supplied by an untrusted party.
  std::string InputDigest;
  LowIRIndependenceCertificate LowIR;
  std::vector<SpecializationInstruction> Instructions;
  /// Exact provider-certified immutable reads, also bound to their mappings.
  std::vector<SpecializationReadWitness> Reads;
};

struct BinaryUndefinedIndependenceResult {
  LowIRIndependenceResult Proof;
  std::optional<BinaryUndefinedIndependenceCertificate> Certificate;

  bool proved() const { return Proof.proved() && Certificate.has_value(); }
};

/// By default collect both arms of original direct branches before pruning.
/// The explicit DeferNativeConditionalEdges contract instead
/// collects conditional successors after paired-control equality and their
/// feasibility checks. An infeasible arm has no instruction-inventory claim;
/// every feasible arrival retains all byte, semantic and resource checks.
/// Physical near calls capture their target before pushing the continuation;
/// internal returns load their actual stack target. Indirect control requires
/// paired target independence and a complete bounded target set. Every feasible
/// path must finish; direct and indirect loops require a complete finite
/// unrolling within the budgets, never a prefix or an assumed invariant.
/// Missing bytes refuse proof. Overlapping instructions are refused by default;
/// AllowOverlappingNativeInstructions permits independently checked entries
/// only when all instruction and immutable-read byte evidence agrees, within
/// the cumulative byte budget. Incomplete architecture evidence also refuses
/// by default. The explicit
/// RetainUnauditedNativeBoundaries contract instead retains strictly lifted
/// Missing instructions as bound refusal frontiers: each must be unreachable,
/// and their uncollected successors are outside the claimed byte inventory.
/// Exact INT3/UD2 evidence may be retained with its Missing
/// undefined-effect coverage only when no feasible execution reaches the trap.
/// A feasible trap violates the nonfaulting contract; no resumption is modeled.
/// Requires an explicit normal, nonfaulting,
/// CET-disabled x64 profile and a readable/writable frame disjoint from the
/// immutable image. Frame must be rooted at entry RSP and contain [0, 8).
/// The checker additionally proves entry RSP and the entry return slot are
/// restored on every outer return, before the final native return-address pop.
/// CALL fallthrough is a stored return value, not a collection edge. Internal
/// RET destinations are collected only after complete target enumeration;
/// direct call targets retain eager audits; conditional arms do so by default.
/// Skipped inline bytes have no instruction-evidence claim. Every feasible
/// destination must still decode, lift and meet the full proof contract.
/// Entry constants, byte order, X64FlagsProfile and optional frame-entry
/// alignment must match Options. Alignment restricts only the shared entry
/// root; it does not rewrite its value or supply memory/ABI evidence.
/// Selecting an explicit supported flags profile enables shared, persistent
/// PUSHFQ/POPFQ system state, canonical entry flag bits and mandatory final
/// system-state equality. Every POPFQ must satisfy its profile restrictions in
/// both executions. Kernel profiles require IOPL=0 and model IF as writable;
/// the user profile preserves IF. KernelX64SmapNoFaultV2 additionally retains
/// AC and requires CPUID SMAP support. These profiles supply no kernel API,
/// resource, IRQL, exception or source-ABI certificate.
/// Exact provider-marked CET-disabled RDSSP projections and
/// unreachable INCSSP #UD boundaries retain Missing sidecars and require
/// separate profile receipts; feasible INCSSP always violates the nonfaulting
/// contract. No other CET operation is authorized. Otherwise only Contract's
/// stated observations are certified. Limits bound collection, scalar profile
/// transitions and all executed paths.
BinaryUndefinedIndependenceResult
checkBinaryUndefinedIndependence(const BinaryImage &Image, va_t Entry,
                                 const SpecializationOptions &Options,
                                 const LowIRIndependenceContract &Contract,
                                 const LowIRIndependenceLimits &Limits = {});

struct BinaryLowIRRefinementCertificate {
  std::string InputDigest;
  LowIRRefinementCertificate Relation;
  std::vector<SpecializationInstruction> Instructions;
  /// Includes immutable reads from both original and candidate execution.
  std::vector<SpecializationReadWitness> Reads;
};

struct BinaryLowIRRefinementResult {
  LowIRRefinementResult Proof;
  std::optional<BinaryLowIRRefinementCertificate> Certificate;

  bool proved() const { return Proof.proved() && Certificate.has_value(); }
};

/// Prove a deterministic LowIR candidate matches complete finite original
/// native executions under an explicit constructive undefined-value witness.
/// Requires matching supported x64 flag profiles in Options and Contract.
/// Uses the same original instruction audit, physical CALL/RET, immutable
/// memory, flags profile and mandatory stack/return-slot preservation as the
/// independence checker. The candidate may have different control structure;
/// all its feasible paths must also terminate and satisfy the full contract.
/// Its LowIR addresses identify graph blocks, not original machine bytes.
///
/// This is ISA-allowed refinement into LowIR, not undefined-value independence,
/// CPU-specific equality, a loop induction proof, or C-backend equivalence.
/// Audited lifting and symbolic transition semantics remain trusted premises.
/// A failed witness does not rule out other witnesses. The ordinary recovery
/// and strict independence APIs retain their existing meanings and defaults.
BinaryLowIRRefinementResult checkBinaryLowIRRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &Limits = {});

/// The native form of checkLowIRLoopRefinement. Retains the finite API's
/// complete byte audits, execution profile and mandatory preservation checks.
/// Original cutpoints are physical instruction entries; candidate cutpoints
/// are LowIR block entries. The exact plan and every checked segment are bound
/// into an InductiveNativeToLowIRLoops certificate. Proof hints must establish
/// initiation and closure from the full admitted entry domain, never supply
/// additional entry assumptions. Deferred conditional edges and retained
/// unaudited refusal boundaries use the same native collector in every
/// segment; feasibility and evidence are checked over the complete induction
/// domain. Overlapping instruction entries remain unsupported in this API.
/// This does not certify a source backend.
BinaryLowIRRefinementResult checkBinaryLowIRLoopRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    const LowIRLoopRefinementPlan &Plan,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &Limits = {});

struct BinaryAutomaticLowIRRefinementResult {
  LowIRLoopInferenceResult Inference;
  BinaryLowIRRefinementResult Refinement;

  bool proved() const { return Refinement.proved(); }
};

/// Infer loop proof hints from a complete recovery with uniquely mapped
/// residual origins. Repeated native addresses need literal-bit selectors
/// proved over the complete candidate templates. Unique native origins retain
/// their previous search priority. Independently check the
/// complete native/candidate relation. Recovery metadata and inferred hints are
/// untrusted; only Refinement's certificate establishes the relation. Search
/// and proof budgets are separate and explicit. Neither stage certifies C.
BinaryAutomaticLowIRRefinementResult inferAndCheckBinaryLowIRLoopRefinement(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const SpecializationResult &Recovery,
    const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness = LowIRRefinementWitness::LiftedBits,
    const LowIRRefinementLimits &ProofLimits = {},
    const LowIRLoopInferenceLimits &InferenceLimits = {});

struct SpecializationWithIndependenceResult {
  SpecializationResult Recovery;
  BinaryUndefinedIndependenceResult Independence;
};

/// Require the independent original-graph proof before invoking recovery.
/// Refusal never contains residual code. A successful result retains separate
/// proof and recovery statuses; it does not certify the recovery transforms or
/// either C backend. Ordinary recovery remains available without this gate.
SpecializationWithIndependenceResult
specializeBinaryInterpreterWithIndependence(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits = {});

} // namespace neverd::analysis

#endif
