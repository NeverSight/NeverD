//===- NeverDCAPISimplify.cpp - Expression simplification C API -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The C entry point to semantic optimisation.
///
/// Everything else in this library works on a loaded binary; this works on a
/// string, because an expression is the one thing worth simplifying that a
/// user can also type.  That makes it the way to try the optimiser against a
/// sample lifted somewhere else, and it is the only surface the Python plugin
/// SDK needs in order to reach the same machinery.
///
/// Two entry points are offered and one implementation serves both: a typed
/// struct pair for callers that want the numbers, and the older JSON spelling
/// for callers that would rather parse one string than declare a struct.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include "SessionImpl.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/Support/JSON.h"

#include <cstddef>
#include <string>

using namespace neverd;
using namespace neverd::sdk;

namespace {

// The C enumerations are the engine's, spelled for callers who cannot include a
// C++ header.  Pinning them against each other here is what stops a value added
// on one side from quietly meaning something else on the other.
static_assert(static_cast<int>(symbolic::MBAOutcome::NotApplicable) ==
                  NEVERD_SIMPLIFY_NOT_APPLICABLE,
              "outcome enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAOutcome::AlreadyShortest) ==
                  NEVERD_SIMPLIFY_ALREADY_SHORTEST,
              "outcome enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAOutcome::TooManyInputs) ==
                  NEVERD_SIMPLIFY_TOO_MANY_INPUTS,
              "outcome enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAOutcome::BudgetExhausted) ==
                  NEVERD_SIMPLIFY_BUDGET_EXHAUSTED,
              "outcome enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAOutcome::Rewritten) ==
                  NEVERD_SIMPLIFY_REWRITTEN,
              "outcome enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAEvidence::None) ==
                  NEVERD_SIMPLIFY_EVIDENCE_NONE,
              "evidence enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAEvidence::Derivation) ==
                  NEVERD_SIMPLIFY_EVIDENCE_DERIVATION,
              "evidence enumerations have drifted apart");
static_assert(static_cast<int>(symbolic::MBAEvidence::Samples) ==
                  NEVERD_SIMPLIFY_EVIDENCE_SAMPLES,
              "evidence enumerations have drifted apart");

/// The width a leaf takes when the caller names none.
constexpr unsigned kDefaultWidth = 32;

/// Where a field ends, measured from the start of its struct.
#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

/// Whether a caller's struct, being \p Size bytes, reaches a field that ends at
/// \p End.
///
/// This is the whole of what `struct_size` buys.  A caller compiled against an
/// older header does not have the fields added since, so reading one would be
/// reading past what it allocated and writing one would be scribbling on
/// something else.  Checking instead of assuming is what lets the struct grow
/// without every plugin having to be rebuilt.
bool reaches(size_t Size, size_t End) { return Size >= End; }

/// One expression, read and simplified.  Both entry points build this and then
/// only differ in how they render it, so neither can drift from the other.
struct Simplified {
  bool Ok = false;
  std::string Error;
  size_t ErrorOffset = 0;
  std::string Input;
  std::string Output;
  symbolic::MBAResult Result;
};

Simplified simplify(const char *Expr, unsigned Width, bool Deep,
                    const symbolic::MBAOptions &Opts) {
  Simplified Out;
  if (!Expr) {
    Out.Error = "no expression given";
    return Out;
  }

  symbolic::SymContext Ctx;
  symbolic::SymParseResult Parsed = symbolic::parseSymExpr(Ctx, Expr, Width);
  if (!Parsed.ok()) {
    Out.Error = Parsed.Error;
    Out.ErrorOffset = Parsed.ErrorOffset;
    return Out;
  }

  Out.Ok = true;
  Out.Result = Deep ? symbolic::simplifyMBADeep(Ctx, Parsed.Root, Opts)
                    : symbolic::simplifyMBA(Ctx, Parsed.Root, Opts);
  // Rendered here because the context that gives these references meaning does
  // not outlive this function; nothing downstream holds a SymRef.
  Out.Input = Ctx.toString(Parsed.Root);
  Out.Output = Ctx.toString(Out.Result.Expr);
  return Out;
}

symbolic::MBAOptions readOptions(const neverd_simplify_options *In,
                                 unsigned &Width, bool &Deep) {
  symbolic::MBAOptions Opts;
  Width = kDefaultWidth;
  Deep = true;
  if (!In)
    return Opts;

  const size_t Size = In->struct_size;
  if (reaches(Size, FIELD_END(neverd_simplify_options, width)) && In->width)
    Width = In->width;
  if (reaches(Size, FIELD_END(neverd_simplify_options, shallow)))
    Deep = In->shallow == 0;
  if (reaches(Size, FIELD_END(neverd_simplify_options, max_atoms)) &&
      In->max_atoms)
    Opts.MaxAtoms = In->max_atoms;
  if (reaches(Size, FIELD_END(neverd_simplify_options, max_work)) &&
      In->max_work)
    Opts.MaxWork = In->max_work;
  if (reaches(Size, FIELD_END(neverd_simplify_options, verify_samples)) &&
      In->verify_samples)
    Opts.VerifySamples = In->verify_samples;
  if (reaches(Size, FIELD_END(neverd_simplify_options, allow_growth)))
    Opts.AllowGrowth = In->allow_growth != 0;
  return Opts;
}

void writeResult(const Simplified &From, neverd_simplify_result *To) {
  const size_t Size = To->struct_size;
#define SET(Field, Value)                                                      \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_simplify_result, Field)))               \
      To->Field = (Value);                                                     \
  } while (0)

  SET(ok, From.Ok ? 1 : 0);
  if (!From.Ok) {
    SET(error, dupStr(From.Error));
    SET(error_offset, From.ErrorOffset);
    return;
  }

  SET(input, dupStr(From.Input));
  SET(output, dupStr(From.Output));
  SET(changed, From.Result.Changed ? 1 : 0);
  SET(cost_before, From.Result.SizeBefore);
  SET(cost_after, From.Result.SizeAfter);
  SET(inputs, From.Result.NumAtoms);
  SET(work, From.Result.Work);
  SET(outcome, static_cast<neverd_simplify_outcome_t>(From.Result.Outcome));
  SET(evidence, static_cast<neverd_simplify_evidence_t>(From.Result.Evidence));
  SET(outcome_name, dupStr(symbolic::mbaOutcomeName(From.Result.Outcome)));
  SET(evidence_name, dupStr(symbolic::mbaEvidenceName(From.Result.Evidence)));
#undef SET
}

} // namespace

extern "C" {

int neverd_simplify_expr(const char *Expr,
                         const neverd_simplify_options *Options,
                         neverd_simplify_result *Result) {
  // Without room for `ok` there is nowhere to put an answer, which is the one
  // thing this cannot report through the result.
  if (!Result || !reaches(Result->struct_size,
                          FIELD_END(neverd_simplify_result, ok)))
    return 1;

  unsigned Width = kDefaultWidth;
  bool Deep = true;
  symbolic::MBAOptions Opts = readOptions(Options, Width, Deep);
  writeResult(simplify(Expr, Width, Deep, Opts), Result);
  return 0;
}

void neverd_simplify_result_dispose(neverd_simplify_result *Result) {
  if (!Result)
    return;
  const size_t Size = Result->struct_size;
#define RELEASE(Field)                                                         \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_simplify_result, Field))) {             \
      neverd_free_string(Result->Field);                                       \
      Result->Field = nullptr;                                                 \
    }                                                                          \
  } while (0)

  RELEASE(error);
  RELEASE(input);
  RELEASE(output);
  RELEASE(outcome_name);
  RELEASE(evidence_name);
#undef RELEASE
}

const char *neverd_simplify_expr_json(const char *Expr, unsigned Width,
                                      int Deep) {
  symbolic::MBAOptions Opts;
  Simplified S =
      simplify(Expr, Width ? Width : kDefaultWidth, Deep != 0, Opts);
  if (!S.Ok)
    return dupStr(jsonToString(
        llvm::json::Object{{"ok", false},
                           {"error", S.Error},
                           {"offset", static_cast<int64_t>(S.ErrorOffset)}}));

  return dupStr(jsonToString(llvm::json::Object{
      {"ok", true},
      {"input", S.Input},
      {"output", S.Output},
      {"changed", S.Result.Changed},
      {"costBefore", static_cast<int64_t>(S.Result.SizeBefore)},
      {"costAfter", static_cast<int64_t>(S.Result.SizeAfter)},
      {"inputs", static_cast<int64_t>(S.Result.NumAtoms)},
      {"work", static_cast<int64_t>(S.Result.Work)},
      {"outcome", symbolic::mbaOutcomeName(S.Result.Outcome)},
      {"evidence", symbolic::mbaEvidenceName(S.Result.Evidence)}}));
}

} // extern "C"
