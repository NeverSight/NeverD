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
/// collapse, and by how much.
///
//===----------------------------------------------------------------------===//

#include "NeverDCLI.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace llvm;

namespace neverd::cli {

namespace {

/// What the engine made of one expression.
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
};

Outcome simplifyOne(StringRef Expr) {
  const char *Json = neverd_simplify_expr_json(Expr.str().c_str(),
                                               SimplifyWidth, !SimplifyShallow);
  Outcome Result;
  Expected<json::Value> Parsed = json::parse(Json ? Json : "{}");
  neverd_free_string(Json);
  if (!Parsed) {
    consumeError(Parsed.takeError());
    Result.Error = "the engine returned something unreadable";
    return Result;
  }

  const json::Object *Obj = Parsed->getAsObject();
  if (!Obj) {
    Result.Error = "the engine returned something unreadable";
    return Result;
  }
  Result.Ok = Obj->getBoolean("ok").value_or(false);
  if (!Result.Ok) {
    Result.Error = Obj->getString("error").value_or("").str();
    Result.ErrorOffset =
        static_cast<size_t>(Obj->getInteger("offset").value_or(0));
    return Result;
  }
  Result.Input = Obj->getString("input").value_or("").str();
  Result.Output = Obj->getString("output").value_or("").str();
  Result.Changed = Obj->getBoolean("changed").value_or(false);
  Result.CostBefore = Obj->getInteger("costBefore").value_or(0);
  Result.CostAfter = Obj->getInteger("costAfter").value_or(0);
  Result.Inputs = Obj->getInteger("inputs").value_or(0);
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
  if (!Result.Changed) {
    outs() << "  out   (unchanged)\n";
    return;
  }
  outs() << "  out   " << Result.Output << "\n";
  outs() << "  cost  " << Result.CostBefore << " -> " << Result.CostAfter;
  if (Result.Inputs)
    outs() << "   (" << Result.Inputs << " input"
           << (Result.Inputs == 1 ? "" : "s") << ")";
  outs() << "\n";
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
                      {"inputs", Result.Inputs}};
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

  json::Array Report;
  unsigned Failed = 0;
  unsigned Simplified = 0;
  int64_t TotalBefore = 0;
  int64_t TotalAfter = 0;

  for (const std::string &Expr : Expressions) {
    Outcome Result = simplifyOne(Expr);
    if (SimplifyJson) {
      Report.push_back(toJson(Expr, Result));
    } else if (!Result.Ok) {
      reportSyntaxError(Expr, Result);
    } else {
      printOutcome(Result);
    }

    if (!Result.Ok) {
      ++Failed;
      continue;
    }
    Simplified += Result.Changed;
    TotalBefore += Result.CostBefore;
    TotalAfter += Result.CostAfter;
  }

  if (SimplifyJson) {
    outs() << json::Value(std::move(Report)) << "\n";
    return Failed ? 1 : 0;
  }

  // One expression already printed everything a summary would repeat.
  if (Expressions.size() > 1) {
    outs() << "\n"
           << Expressions.size() << " expressions, " << Simplified
           << " simplified";
    if (Failed)
      outs() << ", " << Failed << " rejected";
    outs() << "\ntotal cost " << TotalBefore << " -> " << TotalAfter << "\n";
  }
  return Failed ? 1 : 0;
}

} // namespace neverd::cli
