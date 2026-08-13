//===- SymMBATestsDetail.h - Shared MBA simplification assertions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parse a spelling, simplify it, and say what came back — the two shapes
/// every MBA test is written in.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SYMBOLIC_SYMMBATESTSDETAIL_H
#define NEVERD_UNITTESTS_SYMBOLIC_SYMMBATESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymParse.h"

#include <cstdint>
#include <string>

namespace neverd::symbolic::test {

inline constexpr uint32_t W32 = 32;

/// Simplify \p Text and return what the solver made of it.
inline std::string simplified(llvm::StringRef Text, uint32_t Width = W32,
                              const MBAOptions &Opts = {}) {
  SymContext Ctx;
  SymParseResult Parsed = parseSymExpr(Ctx, Text, Width);
  EXPECT_TRUE(Parsed.ok()) << Text.str() << ": " << Parsed.Error;
  if (!Parsed.ok())
    return "<parse error>";
  return Ctx.toString(simplifyMBA(Ctx, Parsed.Root, Opts).Expr);
}

/// Assert that \p Text simplifies to something denoting the same node as
/// \p Want, comparing interned nodes rather than spelling.
inline void simplifiesTo(llvm::StringRef Text, llvm::StringRef Want,
                         uint32_t Width = W32) {
  SymContext Ctx;
  SymParseResult Parsed = parseSymExpr(Ctx, Text, Width);
  ASSERT_TRUE(Parsed.ok()) << Text.str() << ": " << Parsed.Error;
  SymParseResult Expected = parseSymExpr(Ctx, Want, Width);
  ASSERT_TRUE(Expected.ok()) << Want.str() << ": " << Expected.Error;

  MBAResult R = simplifyMBA(Ctx, Parsed.Root);
  EXPECT_EQ(R.Expr, Expected.Root)
      << Text.str() << "\n  became: " << Ctx.toString(R.Expr)
      << "\n  wanted: " << Ctx.toString(Expected.Root);
}

} // namespace neverd::symbolic::test

#endif // NEVERD_UNITTESTS_SYMBOLIC_SYMMBATESTSDETAIL_H
