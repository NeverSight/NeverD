//===- NeverDCmdSimplify.cpp - The simplify subcommand --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// `neverd simplify` — semantic optimisation of an expression given as text.
///
/// Every other subcommand starts from a binary.  This one starts from a line
/// someone pasted, which is what makes it the way to point the optimiser at an
/// expression that came out of somewhere else and see what is left of it.
/// Reading a file of them turns that into a measurement: how many of a corpus
/// collapse, by how much, and what the ones that did not have in common.
///
/// It goes through the same C entry point the plugins do, so what a user sees
/// here is what a caller gets.
///
//===----------------------------------------------------------------------===//

#include "NeverDCLI.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace llvm;

namespace neverd::cli {

namespace {

/// What the engine made of one expression, copied out of the C result so the
/// result itself can be released straight away.
struct Outcome {
  bool Ok = false;
  std::string Error;
  size_t ErrorOffset = 0;
  std::string Input;
  std::string Output;
  bool Changed = false;
  int64_t CostBefore = 0;
  int64_t CostAfter = 0;
  int64_t Inputs = 0;
  int64_t Work = 0;
  std::string Reason;
  std::string Evidence;
};

/// Translate the command line into the engine's policy.  Zero means "keep the
/// default" throughout, which is why the flags that take a number all start
/// there rather than at the value the engine happens to use today.
neverd_simplify_options buildOptions() {
  neverd_simplify_options Options{};
  Options.struct_size = sizeof(Options);
  Options.width = SimplifyWidth;
  Options.shallow = SimplifyShallow ? 1 : 0;
  Options.max_atoms = SimplifyMaxAtoms;
  Options.max_work = SimplifyExhaustive
                         ? static_cast<size_t>(-1)
                         : static_cast<size_t>(SimplifyMaxWork.getValue());
  Options.verify_samples = SimplifyVerifySamples;
  Options.allow_growth = SimplifyAllowGrowth ? 1 : 0;
  return Options;
}

Outcome simplifyOne(StringRef Expr, const neverd_simplify_options &Options) {
  neverd_simplify_result Raw{};
  Raw.struct_size = sizeof(Raw);

  Outcome Result;
  if (neverd_simplify_expr(Expr.str().c_str(), &Options, &Raw) != 0) {
    Result.Error = "the engine refused the request";
    return Result;
  }

  auto copy = [](const char *S) { return std::string(S ? S : ""); };
  Result.Ok = Raw.ok != 0;
  if (Result.Ok) {
    Result.Input = copy(Raw.input);
    Result.Output = copy(Raw.output);
    Result.Changed = Raw.changed != 0;
    Result.CostBefore = static_cast<int64_t>(Raw.cost_before);
    Result.CostAfter = static_cast<int64_t>(Raw.cost_after);
    Result.Inputs = Raw.inputs;
    Result.Work = static_cast<int64_t>(Raw.work);
    Result.Reason = copy(Raw.outcome_name);
    Result.Evidence = copy(Raw.evidence_name);
  } else {
    Result.Error = copy(Raw.error);
    Result.ErrorOffset = Raw.error_offset;
  }
  neverd_simplify_result_dispose(&Raw);
  return Result;
}

/// Point at the offending column, the way a compiler does, so a typo in a long
/// expression does not have to be counted out by hand.
void reportSyntaxError(StringRef Expr, const Outcome &Result) {
  WithColor::error() << Result.Error << "\n";
  errs() << "  " << Expr << "\n  " << std::string(Result.ErrorOffset, ' ')
         << "^\n";
}

void printOutcome(const Outcome &Result) {
  outs() << "  in    " << Result.Input << "\n";
  if (Result.Changed) {
    outs() << "  out   " << Result.Output << "\n";
    outs() << "  cost  " << Result.CostBefore << " -> " << Result.CostAfter;
    if (Result.Inputs)
      outs() << "   (" << Result.Inputs << " input"
             << (Result.Inputs == 1 ? "" : "s") << ")";
    outs() << "\n";
  } else {
    // Saying which of the several reasons it was is the difference between a
    // user retrying with a wider budget and giving up on the expression.
    outs() << "  out   unchanged (" << Result.Reason << ")\n";
  }
  if (SimplifyStats)
    outs() << "  work  " << Result.Work << "   evidence " << Result.Evidence
           << "\n";
}

json::Object toJson(StringRef Expr, const Outcome &Result) {
  if (!Result.Ok)
    return json::Object{{"expr", Expr},
                        {"ok", false},
                        {"error", Result.Error},
                        {"offset", static_cast<int64_t>(Result.ErrorOffset)}};
  return json::Object{{"expr", Expr},
                      {"ok", true},
                      {"input", Result.Input},
                      {"output", Result.Output},
                      {"changed", Result.Changed},
                      {"costBefore", Result.CostBefore},
                      {"costAfter", Result.CostAfter},
                      {"inputs", Result.Inputs},
                      {"work", Result.Work},
                      {"outcome", Result.Reason},
                      {"evidence", Result.Evidence}};
}

/// Read the expressions to work on: one per line, blank lines and `#` comments
/// skipped so a corpus file can carry notes.
bool readExpressions(StringRef Path, std::vector<std::string> &Out) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer =
      Path == "-" ? MemoryBuffer::getSTDIN() : MemoryBuffer::getFile(Path);
  if (!Buffer) {
    WithColor::error() << "cannot read " << Path << ": "
                       << Buffer.getError().message() << "\n";
    return false;
  }
  SmallVector<StringRef, 64> Lines;
  (*Buffer)->getBuffer().split(Lines, '\n');
  for (StringRef Line : Lines) {
    Line = Line.trim();
    if (Line.empty() || Line.starts_with("#"))
      continue;
    Out.push_back(Line.str());
  }
  return true;
}

/// Running totals over a corpus.  Keyed by reason so a run over a body of
/// expressions says not only how many collapsed but what stopped the rest.
struct Tally {
  unsigned Simplified = 0;
  unsigned Failed = 0;
  int64_t CostBefore = 0;
  int64_t CostAfter = 0;
  int64_t Work = 0;
  std::map<std::string, unsigned> Reasons;
  /// One entry per expression, in microseconds.
  std::vector<int64_t> Times;
};

/// The value at \p Fraction through \p Sorted, by nearest rank.
///
/// A mean would be the wrong summary here.  Almost every expression is
/// dispatched in microseconds and a rare one is measured over many inputs, so
/// the average describes neither: what a caller needs to know is what the
/// common case costs and how bad the tail gets.
int64_t percentile(const std::vector<int64_t> &Sorted, double Fraction) {
  if (Sorted.empty())
    return 0;
  auto Rank = static_cast<size_t>(Fraction * double(Sorted.size()));
  return Sorted[std::min(Rank, Sorted.size() - 1)];
}

json::Object summaryJson(size_t Count, Tally &Totals) {
  llvm::sort(Totals.Times);
  json::Object Reasons;
  for (const auto &[Reason, Times] : Totals.Reasons)
    Reasons[Reason] = static_cast<int64_t>(Times);
  return json::Object{
      {"expressions", static_cast<int64_t>(Count)},
      {"simplified", static_cast<int64_t>(Totals.Simplified)},
      {"rejected", static_cast<int64_t>(Totals.Failed)},
      {"costBefore", Totals.CostBefore},
      {"costAfter", Totals.CostAfter},
      {"work", Totals.Work},
      {"p50Micros", percentile(Totals.Times, 0.50)},
      {"p95Micros", percentile(Totals.Times, 0.95)},
      {"maxMicros", Totals.Times.empty() ? 0 : Totals.Times.back()},
      {"outcomes", std::move(Reasons)}};
}

void printSummary(size_t Count, Tally &Totals) {
  outs() << "\n" << Count << " expressions, " << Totals.Simplified
         << " simplified";
  if (Totals.Failed)
    outs() << ", " << Totals.Failed << " rejected";
  outs() << "\ntotal cost " << Totals.CostBefore << " -> " << Totals.CostAfter
         << "\n";
  if (!SimplifyStats)
    return;
  llvm::sort(Totals.Times);
  outs() << "total work " << Totals.Work << "\n"
         << "time p50 " << percentile(Totals.Times, 0.50) << "us, p95 "
         << percentile(Totals.Times, 0.95) << "us, max "
         << (Totals.Times.empty() ? 0 : Totals.Times.back()) << "us\n";
  for (const auto &[Reason, Times] : Totals.Reasons)
    outs() << "  " << Reason << ": " << Times << "\n";
}

} // namespace

int runSimplify() {
  if (SimplifyWidth == 0) {
    WithColor::error() << "--width must be at least 1\n";
    return 1;
  }

  std::vector<std::string> Expressions;
  if (!SimplifyFile.empty()) {
    if (!readExpressions(SimplifyFile, Expressions))
      return 1;
  } else if (!SimplifyExpr.empty()) {
    Expressions.push_back(SimplifyExpr);
  } else {
    WithColor::error() << "give an expression, or -f to read a file of them\n";
    errs() << "  neverd simplify \"(x ^ y) + 2 * (x & y)\"\n";
    return 1;
  }

  const neverd_simplify_options Options = buildOptions();
  json::Array Report;
  Tally Totals;

  for (const std::string &Expr : Expressions) {
    const auto Started = std::chrono::steady_clock::now();
    Outcome Result = simplifyOne(Expr, Options);
    Totals.Times.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - Started)
            .count());

    if (SimplifyJson) {
      Report.push_back(toJson(Expr, Result));
    } else if (!Result.Ok) {
      reportSyntaxError(Expr, Result);
    } else {
      printOutcome(Result);
    }

    if (!Result.Ok) {
      ++Totals.Failed;
      continue;
    }
    Totals.Simplified += Result.Changed;
    Totals.CostBefore += Result.CostBefore;
    Totals.CostAfter += Result.CostAfter;
    Totals.Work += Result.Work;
    ++Totals.Reasons[Result.Reason];
  }

  if (SimplifyJson) {
    // Asking for statistics changes the shape from a bare array to an object,
    // because a corpus run wants one place to read the totals from rather than
    // a consumer that has to re-derive them.
    if (SimplifyStats)
      outs() << json::Value(json::Object{
                    {"results", std::move(Report)},
                    {"summary", summaryJson(Expressions.size(), Totals)}})
             << "\n";
    else
      outs() << json::Value(std::move(Report)) << "\n";
    return Totals.Failed ? 1 : 0;
  }

  // One expression already printed everything a summary would repeat.
  if (Expressions.size() > 1)
    printSummary(Expressions.size(), Totals);
  return Totals.Failed ? 1 : 0;
}

} // namespace neverd::cli
