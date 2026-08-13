//===- SymParse.h - Textual syntax for symbolic expressions -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A C-style infix syntax for bitvector expressions, and its parser.
///
/// The syntax exists so that rewrite rules, regression vectors and
/// command-line input can be written the way they are actually read —
/// `(x^y) + 2*(x&y)` rather than a builder call chain — and so that
/// \c SymContext::toString round-trips.  Precisely:
///
///     parseSymExpr(Ctx, Ctx.toString(R), Ctx.width(R)).Root == R
///
/// for every \p R in \p Ctx.  The parser and printer share one precedence
/// definition so the two halves of that identity stay aligned.
///
/// Operators and precedence follow C, so an expression copied out of an
/// obfuscated decompilation reads the same here.  From loosest to tightest:
///
///     ?:   ||   &&   |   ^   &   == !=   < <= > >=   << >>   + -   * / %
///     unary ~ - !
///
/// The infix comparisons, division, remainder and right shift are the
/// *unsigned* and *logical* forms, matching the default reading of a machine
/// word.  Operators with no C spelling — the signed and rotating ones, and
/// everything that changes width — are written as calls:
///
///     sdiv srem ashr rol ror slt sle sgt sge ult ule ugt uge eq ne
///     add sub mul and or xor not neg concat ite
///     zext(x, w)  sext(x, w)  trunc(x, w)  extract(x, lowbit, w)
///
/// Identifiers denote free variables and literals may be decimal,
/// `0x`-prefixed hex, or `0b` binary.  Both are created at the width passed to
/// the parser unless they carry an explicit `#<bits>` suffix — `x#8`, `255#8`
/// — which is how a mixed-width expression survives the round trip.  A leaf at
/// the width the parser was given prints without a suffix, so the common case
/// of one uniform word size stays free of clutter.
///
/// Operands of differing widths are zero-extended to the wider of the two, and
/// a condition that is not already a single bit is compared against zero, so
/// the reading of `(a == b) + c` and `x ? y : z` is the one C gives them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMPARSE_H
#define NEVERD_SYMBOLIC_SYMPARSE_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd::symbolic {

/// Outcome of a parse: either a root expression or a diagnostic.
struct SymParseResult {
  SymRef Root;
  /// Empty when the parse succeeded.
  std::string Error;
  /// Byte offset in the input where the error was detected.
  size_t ErrorOffset = 0;

  bool ok() const { return Error.empty(); }
  explicit operator bool() const { return ok(); }
};

/// Parse \p Text into \p Ctx, creating every identifier as a \p Width-bit
/// variable.  Identifiers already declared in \p Ctx at that width are reused,
/// so several expressions can be parsed into a shared variable space and then
/// compared.
SymParseResult parseSymExpr(SymContext &Ctx, llvm::StringRef Text,
                            uint32_t Width);

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMPARSE_H
