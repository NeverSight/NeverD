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
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include "SessionImpl.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/Support/JSON.h"

using namespace neverd;
using namespace neverd::sdk;

extern "C" {

const char *neverd_simplify_expr_json(const char *Expr, unsigned Width,
                                      int Deep) {
  auto fail = [](llvm::StringRef Message, size_t Offset) {
    return dupStr(jsonToString(llvm::json::Object{
        {"ok", false},
        {"error", Message},
        {"offset", static_cast<int64_t>(Offset)}}));
  };

  if (!Expr)
    return fail("no expression given", 0);

  symbolic::SymContext Ctx;
  symbolic::SymParseResult Parsed = symbolic::parseSymExpr(Ctx, Expr, Width);
  if (!Parsed.ok())
    return fail(Parsed.Error, Parsed.ErrorOffset);

  symbolic::MBAResult Result = Deep ? symbolic::simplifyMBADeep(Ctx, Parsed.Root)
                                  : symbolic::simplifyMBA(Ctx, Parsed.Root);

  return dupStr(jsonToString(llvm::json::Object{
      {"ok", true},
      {"input", Ctx.toString(Parsed.Root)},
      {"output", Ctx.toString(Result.Expr)},
      {"changed", Result.Changed},
      {"costBefore", static_cast<int64_t>(Result.SizeBefore)},
      {"costAfter", static_cast<int64_t>(Result.SizeAfter)},
      {"inputs", static_cast<int64_t>(Result.NumAtoms)}}));
}

} // extern "C"
