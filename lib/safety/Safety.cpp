//===- Safety.cpp - Memory-safety audit and hunt entry points ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/Safety.h"

#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::safety;

namespace {

void describeImage(const AnalysisInput &In, SafetyReport &R) {
  if (!In.Img)
    return;
  R.Format = In.Img->getFormatName();
  R.Arch = getArchName(In.Img->Arch);
}

} // namespace

SafetyReport neverd::safety::runHunt(const AnalysisInput &In,
                                     const SinkCatalog &Cat,
                                     const SafetyBudgets &Budgets) {
  SafetyReport R;
  R.Origin = Track::Hunt;
  describeImage(In, R);
  if (!In.MedFuncs)
    return R;

  for (const SinkSite &Site : scanSinks(In, Cat)) {
    const MedFunc *F = In.findMedFunc(Site.FuncEntry);
    if (!F)
      continue;
    std::optional<Finding> Fnd = huntSink(In, Cat, Budgets, *F, Site);
    if (!Fnd)
      continue;
    ++R.Scanned;
    if (!Fnd->SkipReason.empty())
      ++R.Skipped;
    if (Fnd->BudgetHit)
      R.BudgetHit = true;
    R.Findings.push_back(std::move(*Fnd));
  }
  return R;
}

SafetyReport neverd::safety::runAudit(const AnalysisInput &In,
                                      const SinkCatalog &Cat,
                                      const SafetyBudgets &Budgets) {
  SafetyReport R;
  R.Origin = Track::Audit;
  describeImage(In, R);
  for (const SinkSite &Site : scanSinks(In, Cat))
    if (Site.Kind == SinkKind::Alloc || Site.Kind == SinkKind::Realloc)
      ++R.Scanned;
  R.Findings = auditHeap(In, Cat, Budgets);
  for (const Finding &F : R.Findings)
    if (F.BudgetHit)
      R.BudgetHit = true;
  return R;
}

std::string neverd::safety::toJson(const SafetyReport &Report, bool Pretty) {
  using namespace llvm;

  auto vaHex = [](va_t Addr) { return "0x" + utohexstr(Addr); };

  json::Array Findings;
  for (const Finding &F : Report.Findings) {
    json::Object Ev;
    if (!F.SkipReason.empty())
      Ev["skip_reason"] = F.SkipReason;
    if (!F.Constraints.empty())
      Ev["constraints"] = F.Constraints;
    if (!F.Witness.empty()) {
      json::Object Concrete;
      for (const auto &[K, V] : F.Witness)
        Concrete[K] = V;
      Ev["concrete_input"] = std::move(Concrete);
    }

    json::Object O;
    O["class"] = toString(F.Class);
    O["function"] = F.Function;
    O["function_va"] = vaHex(F.FuncEntry);
    O["name"] = F.Name;
    O["name_source"] = toString(F.Source);
    O["call_va"] = vaHex(F.CallVA);
    if (!F.SourceLoc.empty())
      O["source"] = F.SourceLoc;
    O["sink"] = F.Sink;
    if (F.ArgIndex >= 0)
      O["arg_index"] = F.ArgIndex;
    O["flow"] = toString(F.Flow);
    O["verdict"] = toString(F.TheVerdict);
    O["confidence"] = toString(F.TheConfidence);
    if (F.Capacity) {
      O["capacity"] = static_cast<int64_t>(*F.Capacity);
      O["capacity_kind"] = F.CapacityExact ? "exact" : "upper_bound";
    }
    if (!F.Detail.empty())
      O["detail"] = F.Detail;
    if (!F.Corroboration.empty())
      O["corroboration"] = F.Corroboration;
    O["evidence"] = std::move(Ev);
    Findings.push_back(std::move(O));
  }

  // Verdict tally, so a caller can key an exit code off one number.
  unsigned Unsafe = 0, Unknown = 0, Safe = 0;
  for (const Finding &F : Report.Findings) {
    switch (F.TheVerdict) {
    case Verdict::Unsafe:
      ++Unsafe;
      break;
    case Verdict::Unknown:
      ++Unknown;
      break;
    case Verdict::Safe:
      ++Safe;
      break;
    }
  }

  const Verdict Aggregate = Unsafe > 0    ? Verdict::Unsafe
                            : Unknown > 0 ? Verdict::Unknown
                                          : Verdict::Safe;
  Confidence AggregateConfidence = Confidence::High;
  bool HaveAggregateConfidence = false;
  for (const Finding &F : Report.Findings) {
    if (F.TheVerdict != Aggregate)
      continue;
    if (!HaveAggregateConfidence) {
      AggregateConfidence = F.TheConfidence;
      HaveAggregateConfidence = true;
      continue;
    }
    const auto Current = static_cast<unsigned>(AggregateConfidence);
    const auto Candidate = static_cast<unsigned>(F.TheConfidence);
    // One proven unsafe finding establishes the aggregate; SAFE/UNKNOWN must
    // retain the least-confident contributing result.
    AggregateConfidence = static_cast<Confidence>(
        Aggregate == Verdict::Unsafe ? std::min(Current, Candidate)
                                     : std::max(Current, Candidate));
  }

  json::Object Root;
  Root["schema_version"] = 1;
  Root["ok"] = true;
  Root["track"] = toString(Report.Origin);
  Root["verdict"] = toString(Aggregate);
  Root["confidence"] = toString(AggregateConfidence);
  Root["format"] = Report.Format;
  Root["arch"] = Report.Arch;
  Root["scanned"] = Report.Scanned;
  Root["skipped"] = Report.Skipped;
  Root["budget_hit"] = Report.BudgetHit;
  Root["unsafe"] = Unsafe;
  Root["unknown"] = Unknown;
  Root["safe"] = Safe;
  Root["findings"] = std::move(Findings);

  std::string Buf;
  raw_string_ostream OS(Buf);
  if (Pretty)
    OS << formatv("{0:2}", json::Value(std::move(Root)));
  else
    OS << json::Value(std::move(Root));
  return OS.str();
}
