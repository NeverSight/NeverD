//===- SafetyTypes.h - Shared memory-safety analysis vocabulary -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Format-neutral value types shared by the audit (heap lifetime) and hunt
/// (dangerous-sink overflow) tracks: verdicts, weakness classes, the record of
/// a matched call site, and a single reported finding.  These types never
/// depend on a binary format; the format-specific work lives in the identity,
/// import, and debug adapters the analysis consumes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SAFETYTYPES_H
#define NEVERD_SAFETY_SAFETYTYPES_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::safety {

/// Which of the two analyses produced a record.
enum class Track : uint8_t {
#define SAFETY_TRACK(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// The outcome for one call site.  UNKNOWN is the fail-closed default: an
/// unlifted operation, an unrecovered argument, or an exhausted budget is
/// reported as UNKNOWN, never quietly as SAFE.
enum class Verdict : uint8_t {
#define SAFETY_VERDICT(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// How much weight the evidence carries.  A satisfying symbolic model or a
/// proof over every reachable path is HIGH; a prefilter decision is a definite
/// SAFE-skip; a budget or modelling limit is LOW.
enum class Confidence : uint8_t {
#define SAFETY_CONFIDENCE(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// The category of weakness a catalog entry or finding represents.
enum class VulnClass : uint8_t {
#define SAFETY_VULN_CLASS(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// The behavioural role a catalog entry plays, which selects the property the
/// engine checks at the call site.
enum class SinkKind : uint8_t {
#define SAFETY_SINK_KIND(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// Where the callee name that matched the catalog was established.  This is the
/// per-format identity contract: a stated name from debug info or an import
/// table must not be reported as if it were recovered by a signature guess.
enum class NameSource : uint8_t {
#define SAFETY_NAME_SOURCE(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// How the backward slice classified the argument that decides the property.
enum class ArgFlow : uint8_t {
#define SAFETY_ARG_FLOW(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// Strength of the recovered destination-capacity evidence.  StorageExact
/// proves an allocation or top-level storage boundary, but not the boundary of
/// a writable subobject inside it.  Only TypedBufferExact may establish a
/// generic SAFE verdict or authorize a strict runtime guard.
enum class CapacityPrecision : uint8_t {
  Unknown = 0,
  ContainerUpperBound = 1,
  StorageExact = 2,
  TypedBufferExact = 3,
};
static_assert(static_cast<uint8_t>(CapacityPrecision::Unknown) == 0);
static_assert(static_cast<uint8_t>(CapacityPrecision::ContainerUpperBound) ==
              1);
static_assert(static_cast<uint8_t>(CapacityPrecision::StorageExact) == 2);
static_assert(static_cast<uint8_t>(CapacityPrecision::TypedBufferExact) == 3);

/// The two process inputs for which the report can currently carry an exact,
/// literal replay.  argv, files, network data, and custom sources are excluded
/// until an adapter can give them equally precise occurrence semantics.
enum class ReplayInputKind : uint8_t {
#define SAFETY_REPLAY_INPUT_KIND(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// What a symbolic query variable means at one exact input occurrence.
enum class ReplayBindingRole : uint8_t {
#define SAFETY_REPLAY_BINDING_ROLE(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// Whether a finding's containing function is reachable from the known native
/// entry set.  This is deliberately separate from Verdict: dead code can still
/// contain a real defect, while an incomplete entry/call model must not erase
/// it or promote it to SAFE.
enum class ReachabilityStatus : uint8_t {
#define SAFETY_REACHABILITY_STATUS(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

/// Why a function participates in the known native entry set.
enum class SafetyEntryKind : uint8_t {
#define SAFETY_ENTRY_KIND(ID, SPELLING) ID,
#include "neverd/safety/SafetyEnums.def"
};

const char *toString(Track T);
const char *toString(Verdict V);
const char *toString(Confidence C);
const char *toString(VulnClass C);
const char *toString(SinkKind K);
const char *toString(NameSource S);
const char *toString(ArgFlow F);
const char *toString(CapacityPrecision P);
const char *toString(ReplayInputKind K);
const char *toString(ReplayBindingRole R);
const char *toString(ReachabilityStatus S);
const char *toString(SafetyEntryKind K);

/// A call site that matched a catalog entry, before any property is checked.
struct SinkSite {
  va_t FuncEntry = 0;       ///< entry VA of the function containing the call.
  std::string FuncName;     ///< display name of that function.
  int BlockId = -1;         ///< MedIR block holding the call op.
  int OpIdx = -1;           ///< index of the call op within that block.
  size_t CallInfoIndex = 0; ///< index into the function's recovered call list.
  va_t CallVA = 0;          ///< address of the call instruction.

  std::string StatedName; ///< the callee name as resolved by the pipeline.
  std::string Sink;       ///< the normalized catalog name it matched.
  VulnClass Class = VulnClass::Unknown;
  SinkKind Kind = SinkKind::Copy;
  int ArgIndex = -1;       ///< the argument whose value decides the property.
  bool IsIndirect = false; ///< the call reached the callee indirectly.
  NameSource Source = NameSource::Synthetic;
};

/// A MedIR program point used internally to replay a candidate on a symbolic
/// path.  It is deliberately not serialized into the report.
struct FindingEvent {
  int BlockId = -1;
  int OpIdx = -1;
  va_t VA = 0;
  int Seq = -1;
  NdOp Opcode = NdOp::_COUNT;
};

/// One free-variable assignment from the solver model that establishes a
/// finding.  This is symbolic evidence, not necessarily a process-level input
/// until an argv/stdin/file adapter maps it back to bytes.
struct SolverAssignment {
  uint32_t Id = 0;
  std::string Name;
  uint32_t Width = 0;
  std::string ValueHex;
  bool Fresh = false;
};

/// A typed association between a solver query variable and an exact input
/// occurrence.  The enclosing ReplayInput supplies the occurrence identity and
/// literal bytes; Role prevents an extent or success predicate from being
/// mistaken for a byte value merely because their numeric models coincide.
struct ReplayBinding {
  uint32_t AssignmentId = 0;
  ReplayBindingRole Role = ReplayBindingRole::Byte;
  /// Byte index within ReplayInput::Bytes when Role is Byte; zero otherwise.
  uint64_t ByteOffset = 0;
};

/// Literal data returned by one environment or standard-input occurrence.
/// CallVA/Seq identify the source call, Invocation disambiguates repeated calls
/// at that occurrence, and Offset locates the bytes in the logical source.
struct ReplayInput {
  ReplayInputKind Kind = ReplayInputKind::Environment;
  va_t CallVA = 0;
  int Seq = -1;
  uint64_t Invocation = 0;
  uint64_t Offset = 0;
  std::string Name; ///< environment name; empty for standard input.
  std::vector<uint8_t> Bytes;
  bool EOFAfter = false;
  bool TerminatorImplicit = false;
  std::vector<ReplayBinding> Bindings;
};

/// Versioned, exact process-input recipe for one finding.  QueryVariables is
/// the complete symbolic query set and is validation metadata rather than JSON
/// payload: every listed ID must have exactly one typed input binding.
struct ReplayPlan {
  uint32_t Version = 1;
  std::vector<uint32_t> QueryVariables;
  std::vector<ReplayInput> Inputs;
};

/// One exact internal call edge in a shortest known-entry witness.
struct ReachabilityCall {
  va_t CallerVA = 0;
  va_t CallVA = 0;
  va_t CalleeVA = 0;
  bool Indirect = false;
};

/// One coherent known-entry-to-function witness.  RootFunctionVA names the
/// exact MedIR root even when the native entry address is zero; EntryVA uses
/// presence rather than a numeric sentinel for the same reason.
struct ReachabilityWitness {
  va_t RootFunctionVA = 0;
  std::optional<va_t> EntryVA;
  std::string EntryName;
  SafetyEntryKind Kind = SafetyEntryKind::Image;
  std::vector<ReachabilityCall> CallChain;
};

/// Control reachability and attacker-control evidence for one finding.
struct ReachabilityEvidence {
  ReachabilityStatus Status = ReachabilityStatus::Unknown;
  ArgFlow AttackerControl = ArgFlow::Unknown;
  std::optional<va_t> EntryVA;
  std::string EntryName;
  SafetyEntryKind Kind = SafetyEntryKind::Image;
  std::vector<ReachabilityCall> CallChain;
  std::string Reason;
  bool BudgetHit = false;
};

/// One evidence-carrying record in a report.
struct Finding {
  Track Origin = Track::Hunt;
  VulnClass Class = VulnClass::Unknown;
  Verdict TheVerdict = Verdict::Unknown;
  Confidence TheConfidence = Confidence::Low;

  std::string Function; ///< containing function display name.
  va_t FuncEntry = 0;
  std::string Name; ///< sink / callee name.
  NameSource Source = NameSource::Synthetic;
  va_t CallVA = 0;
  /// Exact non-serialized MedIR identity consumed by strict instrumentation.
  int BlockId = -1;
  int OpIdx = -1;
  int OriginSeq = -1;
  uint32_t CallSiteId = 0;
  std::string SourceLoc; ///< "file:line" when debug info supplies it.
  std::string Sink;      ///< normalized catalog name.
  /// Non-serialized sink role used by strict instrumentation classification.
  SinkKind Kind = SinkKind::Copy;
  int ArgIndex = -1;
  ArgFlow Flow = ArgFlow::Unknown;

  std::optional<uint64_t> Capacity; ///< destination capacity, when known.
  CapacityPrecision CapacityKind = CapacityPrecision::Unknown;
  bool CapacityExact = false; ///< false means Capacity is an upper bound.

  // Evidence — at most one of the following is populated per finding.
  std::string SkipReason;  ///< why a bounded sink was filtered before solving.
  std::string Constraints; ///< the path predicate, rendered.
  std::vector<std::pair<std::string, std::string>>
      Witness; ///< candidate input/derived values for the violation.
  std::vector<SolverAssignment> SymbolicModel;
  std::optional<ReplayPlan> Replay;
  /// Fail-closed explanation when no validated replay plan is available.
  std::string ReplayReason;
  std::string Corroboration; ///< how a symbolic pass confirmed a candidate.
  std::string Detail;        ///< a short human-readable note.
  bool BudgetHit = false;    ///< exploration or the solver ran out of budget.

  std::vector<FindingEvent> RequiredPathEvents;
  std::vector<FindingEvent> ForbiddenPathEvents;
  bool RequireReturnedPath = false;
  va_t RequireNonNullCallVA = 0;

  ReachabilityEvidence Reachability;
  /// Internal provenance used to keep an attacker-control fact paired with
  /// the root/call chain that actually produced it.  Reachability serialization
  /// consumes this into Reachability rather than emitting a duplicate field.
  std::optional<ReachabilityWitness> AttackerWitness;
};

/// The result of running one track over a binary.
struct SafetyReport {
  Track Origin = Track::Hunt;
  std::string Format; ///< "PE" / "ELF" / "Mach-O".
  std::string Arch;
  std::vector<Finding> Findings;
  unsigned Scanned = 0;   ///< matched call sites considered.
  unsigned Skipped = 0;   ///< prefilter-bounded call sites.
  bool BudgetHit = false; ///< a budget cut at least one exploration short.
  /// False when the public C++ entry point did not receive a complete,
  /// matching image/MedIR/LowIR inventory.  Such a report aggregates to
  /// UNKNOWN even when no call site could be scanned.
  bool AnalysisComplete = false;
  std::string Error;
};

/// Resource limits, mirroring the exploration-budget style used elsewhere in
/// the engine.  A zero field selects the built-in default.
struct SafetyBudgets {
  unsigned MaxPaths = 0;
  unsigned MaxSteps = 0;
  unsigned MaxLoop = 0;
  uint64_t SolverConflicts = 0;
  /// Maximum number of internal call edges in a known-entry witness.  Zero
  /// selects the finite production default.
  unsigned MaxCallDepth = 0;
  /// Maximum fixed-point rounds used for caller-argument to callee-parameter
  /// attacker-control propagation.  Zero selects an inventory-sized bound.
  unsigned MaxSummaryIterations = 0;
};

/// Default SAT-search ceiling used when a caller leaves SolverConflicts at
/// zero.  Safety analysis must remain bounded unless a future API exposes an
/// explicit unbounded mode.
inline constexpr uint64_t kDefaultSafetySolverConflicts = uint64_t(1) << 18;

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SAFETYTYPES_H
