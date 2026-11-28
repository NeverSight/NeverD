//===- SafetyScan.cpp - Unified memory-safety report ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Group only observations of the same scoped obligation within this image.
/// Keep each analyzer's original finding, including its evidence and verdict.
//===----------------------------------------------------------------------===//

#include "neverd/safety/SafetyScan.h"

#include "SafetyJSON.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <tuple>

using namespace neverd::safety;
using namespace llvm;

ScanReport::ScanReport(ScanSelection Selection, ScanState Initial,
                       const std::string &Reason) {
  for (Track Origin : {Track::Audit, Track::Hunt}) {
    auto &Result =
        Origin == Track::Audit ? Analyzers.front() : Analyzers.back();
    bool Enabled = Selection == ScanSelection::Both ||
                   (Origin == Track::Audit ? Selection == ScanSelection::Audit
                                           : Selection == ScanSelection::Hunt);
    Result.State = Enabled ? Initial : ScanState::Disabled;
    Result.Report.Origin = Origin;
    if (Enabled)
      Result.Report.Error = Reason;
  }
}

namespace {
const char *stateName(ScanState S) {
  switch (S) {
  case ScanState::Completed:
    return "completed";
  case ScanState::Disabled:
    return "disabled";
  case ScanState::Canceled:
    return "canceled";
  case ScanState::Unsupported:
    return "unsupported";
  case ScanState::BudgetExhausted:
    return "budget_exhausted";
  case ScanState::Failed:
    return "failed";
  }
  llvm_unreachable("invalid scan state");
}

Verdict combine(Verdict A, Verdict B) {
  if (A == Verdict::Unsafe || B == Verdict::Unsafe)
    return Verdict::Unsafe;
  if (A == Verdict::Unknown || B == Verdict::Unknown)
    return Verdict::Unknown;
  return Verdict::Safe;
}

StringRef scopeKind(const Finding &F) {
  if (!F.SharedCheck)
    return toString(F.Origin);
  return F.SharedCheck->Requirement == CatalogRequirement::CallEffects
             ? "catalog_call_effects"
             : "catalog_debug_signature";
}

auto scopeKey(const Finding &F) {
  // Each run shares one immutable image, catalog, identity view and budget.
  // An analyzer-private path has additional allocation and event assumptions.
  return std::tuple_cat(std::tie(F.FuncEntry, F.CallVA, F.Class, F.OriginSeq,
                                 F.BlockId, F.OpIdx, F.Sink, F.Kind),
                        std::tuple{scopeKind(F), F.SharedCheck
                                                     ? F.SharedCheck->Argument
                                                     : F.ArgIndex},
                        std::tie(F.RequireNonNullCallVA, F.RequireReturnedPath,
                                 F.RequiredPathEvents, F.ForbiddenPathEvents,
                                 F.Constraints, F.Capacity, F.CapacityKind));
}

std::string address(neverd::va_t VA) { return "0x" + utohexstr(VA); }

json::Array events(const std::vector<FindingEvent> &Path) {
  json::Array Result;
  for (const auto &E : Path)
    Result.push_back(json::Object{{"va", address(E.VA)},
                                  {"seq", E.Seq},
                                  {"block", E.BlockId},
                                  {"op", E.OpIdx},
                                  {"opcode", neverd::ndOpName(E.Opcode)}});
  return Result;
}

json::Object findingGroup(const Finding &F) {
  json::Object Scope{{"kind", scopeKind(F)}, {"sink", F.Sink}};
  if (F.SharedCheck || F.Origin == Track::Hunt)
    Scope["role"] = toString(F.Kind);
  int Argument = F.SharedCheck ? F.SharedCheck->Argument : F.ArgIndex;
  if (Argument >= 0)
    Scope["arg_index"] = Argument;
  if (F.RequireNonNullCallVA)
    Scope["nonnull_allocation_va"] = address(F.RequireNonNullCallVA);
  if (F.RequireReturnedPath)
    Scope["requires_return"] = true;
  if (!F.RequiredPathEvents.empty())
    Scope["required_events"] = events(F.RequiredPathEvents);
  if (!F.ForbiddenPathEvents.empty())
    Scope["forbidden_events"] = events(F.ForbiddenPathEvents);
  if (!F.Constraints.empty())
    Scope["constraints"] = F.Constraints;
  if (F.Capacity) {
    Scope["capacity"] = *F.Capacity;
    Scope["capacity_precision"] = toString(F.CapacityKind);
  }
  json::Object Location{{"function_va", address(F.FuncEntry)},
                        {"call_va", address(F.CallVA)}};
  if (F.BlockId >= 0)
    Location["block"] = F.BlockId;
  if (F.OpIdx >= 0)
    Location["op"] = F.OpIdx;
  if (F.OriginSeq >= 0)
    Location["seq"] = F.OriginSeq;
  return json::Object{{"property", toString(F.Class)},
                      {"location", std::move(Location)},
                      {"scope", std::move(Scope)}};
}
} // namespace

std::string neverd::safety::toJson(const ScanReport &Scan, bool Pretty) {
  std::vector<const Finding *> Ordered;
  json::Object Analyzers;
  bool Complete = true, AnyAnalyzed = false;
  Verdict Overall = Verdict::Safe;
  for (const AnalyzerResult &Result : Scan.Analyzers) {
    const SafetyReport &R = Result.Report;
    const bool Finished = Result.State == ScanState::Completed;
    const bool Enabled = Result.State != ScanState::Disabled;
    Verdict V = Finished ? Verdict::Safe : Verdict::Unknown;
    unsigned Unknown = 0;
    for (const Finding &F : R.Findings) {
      Ordered.push_back(&F);
      V = combine(V, F.TheVerdict);
      Unknown += F.TheVerdict == Verdict::Unknown;
    }
    json::Object Summary{
        {"state", stateName(Result.State)},
        {"scanned", R.Scanned},
        {"skipped", R.Skipped},
        {"observations", R.Findings.size()},
        {"unknown", Unknown},
        {"budget_hit",
         R.BudgetHit || Result.State == ScanState::BudgetExhausted}};
    if (Result.DirectReads) {
      const DirectReadInitialization &Reads = *Result.DirectReads;
      Summary["direct_read_initialization"] = json::Object{
          {"preceding_constant_store", Reads.PrecedingConstantStore},
          {"local_stack", Reads.LocalStack},
          {"outside_or_unresolved", Reads.OutsideOrUnresolved}};
    }
    if (Enabled) {
      Complete &= Finished;
      AnyAnalyzed |= Finished || R.AnalysisComplete || !R.Findings.empty();
      Overall = combine(Overall, V);
      Summary["verdict"] = toString(V);
    }
    if (!R.Error.empty())
      Summary["error"] = R.Error;
    Analyzers[toString(R.Origin)] = std::move(Summary);
  }

  std::stable_sort(Ordered.begin(), Ordered.end(),
                   [](const Finding *A, const Finding *B) {
                     return scopeKey(*A) < scopeKey(*B);
                   });
  json::Array Findings;
  for (size_t I = 0; I < Ordered.size();) {
    const Finding &F = *Ordered[I];
    json::Object Group = findingGroup(F);
    json::Array Observations;
    Verdict V = F.TheVerdict;
    do {
      const Finding &Source = *Ordered[I++];
      Observations.push_back(json::Object{{"analyzer", toString(Source.Origin)},
                                          {"finding", findingJson(Source)}});
      V = combine(V, Source.TheVerdict);
      // Without an exact IR occurrence two reports at the same address may
      // describe different accesses. Preserve them as separate findings.
    } while (I < Ordered.size() && F.OriginSeq >= 0 &&
             scopeKey(F) == scopeKey(*Ordered[I]));
    Group["observations"] = std::move(Observations);
    Group["verdict"] = toString(V);
    Findings.push_back(std::move(Group));
  }
  const SafetyReport &Image = Scan.Analyzers.front().Report;
  json::Object Root{{"schema", "neverd.memory-safety-scan"},
                    {"schema_version", 1},
                    {"format", Image.Format},
                    {"arch", Image.Arch},
                    {"ok", AnyAnalyzed},
                    {"coverage_complete", Complete},
                    {"verdict", toString(Overall)},
                    {"analyzers", std::move(Analyzers)},
                    {"findings", std::move(Findings)}};
  return formatv(Pretty ? "{0:2}" : "{0}", json::Value(std::move(Root))).str();
}
