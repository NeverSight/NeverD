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

/// How much weight the evidence carries.  A concrete witness or a proof over
/// every reachable path is HIGH; a prefilter decision is a definite SAFE-skip;
/// a budget or modelling limit is LOW.
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

const char *toString(Track T);
const char *toString(Verdict V);
const char *toString(Confidence C);
const char *toString(VulnClass C);
const char *toString(SinkKind K);
const char *toString(NameSource S);
const char *toString(ArgFlow F);

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
  std::string SourceLoc; ///< "file:line" when debug info supplies it.
  std::string Sink;      ///< normalized catalog name.
  int ArgIndex = -1;
  ArgFlow Flow = ArgFlow::Unknown;

  std::optional<uint64_t> Capacity; ///< destination capacity, when known.
  bool CapacityExact = false;       ///< false means Capacity is an upper bound.

  // Evidence — at most one of the following is populated per finding.
  std::string SkipReason;  ///< why a bounded sink was filtered before solving.
  std::string Constraints; ///< the path predicate, rendered.
  std::vector<std::pair<std::string, std::string>>
      Witness;               ///< concrete inputs that drive the violation.
  std::string Corroboration; ///< how a symbolic pass confirmed a candidate.
  std::string Detail;        ///< a short human-readable note.
  bool BudgetHit = false;    ///< exploration or the solver ran out of budget.

  std::vector<FindingEvent> RequiredPathEvents;
  std::vector<FindingEvent> ForbiddenPathEvents;
  bool RequireReturnedPath = false;
  va_t RequireNonNullCallVA = 0;
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
};

/// Resource limits, mirroring the exploration-budget style used elsewhere in
/// the engine.  A zero field selects the built-in default.
struct SafetyBudgets {
  unsigned MaxPaths = 0;
  unsigned MaxSteps = 0;
  unsigned MaxLoop = 0;
  uint64_t SolverConflicts = 0;
};

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SAFETYTYPES_H
