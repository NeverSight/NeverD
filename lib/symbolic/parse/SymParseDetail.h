//===- SymParseDetail.h - Shared symbolic text internals --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the precedence ladder shared by the symbolic expression parser and
/// printer, plus the lexer and parser state shared by the translation units
/// that make up the parser itself.
///
/// This header is an implementation detail of the symbolic library and should
/// not be included outside lib/symbolic/parse/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_PARSE_SYMPARSEDETAIL_H
#define NEVERD_SYMBOLIC_PARSE_SYMPARSEDETAIL_H

#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace neverd::symbolic::detail {

/// Binding strength, loosest first, following C.
enum Prec : int {
  PrecTernary = 1,
  PrecLogOr,
  PrecLogAnd,
  PrecBitOr,
  PrecBitXor,
  PrecBitAnd,
  PrecEquality,
  PrecRelational,
  PrecShift,
  PrecAdditive,
  PrecMultiplicative,
  PrecUnary,
  PrecPrimary,
};

//===----------------------------------------------------------------------===//
// Lexing
//===----------------------------------------------------------------------===//

struct Token {
  enum Kind { End, Ident, Number, Punct };
  Kind K = End;
  /// Spelling, for an identifier or a punctuator.
  llvm::StringRef Text;
  /// Digits with any radix prefix removed, for a number.
  llvm::StringRef Digits;
  unsigned Radix = 10;
  size_t Offset = 0;
};

class Lexer {
public:
  explicit Lexer(llvm::StringRef T) : Text(T) { advance(); }

  const Token &peek() const { return Cur; }
  Token take() {
    Token T = Cur;
    advance();
    return T;
  }

private:
  void advance();

  llvm::StringRef Text;
  size_t Pos = 0;
  Token Cur;
};

//===----------------------------------------------------------------------===//
// Parsing
//===----------------------------------------------------------------------===//

class Parser {
public:
  Parser(SymContext &Ctx, llvm::StringRef Text, uint32_t Width,
         const SymParseOptions &Options)
      : Ctx(Ctx), Lex(Text), Width(Width), Options(Options) {
    validateWidth(Width, 0);
  }

  SymParseResult run();

private:
  class DepthGuard {
  public:
    DepthGuard(Parser &P, size_t Offset) : P(P) {
      ++P.Depth;
      if (P.Options.MaxNesting != 0 && P.Depth > P.Options.MaxNesting)
        P.fail(Offset, "expression nests too deeply");
    }
    ~DepthGuard() { --P.Depth; }

  private:
    Parser &P;
  };

  SymRef parseExpr() { return parseTernary(); }
  SymRef parseTernary();
  SymRef parseBinary(int MinPrec);
  SymRef parseUnary();
  SymRef parsePrimary();
  SymRef parseCall(const Token &Name);
  SymRef parseVariadicCall(const Token &Name);
  SymRef parseWidthCall(const Token &Name);
  SymRef makeLiteral(const Token &N);

  bool parsePlainInt(uint64_t &Out);
  uint32_t parseWidthSuffix(uint32_t Default);
  bool validateWidth(uint64_t Candidate, size_t Offset) {
    const uint64_t RepresentationLimit = std::numeric_limits<uint32_t>::max();
    if (Candidate != 0 && Candidate <= RepresentationLimit &&
        (Options.MaxWidth == 0 || Candidate <= Options.MaxWidth))
      return true;

    if (Options.MaxWidth != 0)
      fail(Offset,
           "a width must be between 1 and " + llvm::Twine(Options.MaxWidth));
    else
      fail(Offset, "a width must fit the symbolic IR width field");
    return false;
  }

  SymRef applyBinary(llvm::StringRef Spelling, SymRef A, SymRef B);
  SymRef applyVariadic(const Token &Name, llvm::MutableArrayRef<SymRef> Args);

  /// Widen the narrower operand so a mixed-width expression reads the way C
  /// would read it.  Every builder requires its operands to agree.
  void unify(SymRef &A, SymRef &B) {
    uint32_t WA = Ctx.width(A), WB = Ctx.width(B);
    if (WA < WB)
      A = Ctx.mkZExt(A, WB);
    else if (WB < WA)
      B = Ctx.mkZExt(B, WA);
  }
  void unifyAll(llvm::MutableArrayRef<SymRef> Args);

  /// Reduce a value to the single bit a condition has to be.
  SymRef toBool(SymRef A) {
    uint32_t W = Ctx.width(A);
    return W == 1 ? A : Ctx.mkNe(A, Ctx.mkZero(W));
  }

  SymRef makeIte(SymRef C, SymRef T, SymRef E) {
    unify(T, E);
    return Ctx.mkIte(toBool(C), T, E);
  }

  bool isPunct(llvm::StringRef S) const {
    return Lex.peek().K == Token::Punct && Lex.peek().Text == S;
  }
  bool consumePunct(llvm::StringRef S) {
    if (!isPunct(S))
      return false;
    Lex.take();
    return true;
  }
  bool expectPunct(llvm::StringRef S) {
    if (consumePunct(S))
      return true;
    fail(Lex.peek().Offset, ("expected '" + S + "'").str());
    return false;
  }

  void fail(size_t Offset, const llvm::Twine &Msg) {
    if (Failed)
      return;
    Failed = true;
    Error = Msg.str();
    ErrorOffset = Offset;
  }

  SymContext &Ctx;
  Lexer Lex;
  uint32_t Width;
  SymParseOptions Options;
  size_t Depth = 0;
  bool Failed = false;
  std::string Error;
  size_t ErrorOffset = 0;
};

} // namespace neverd::symbolic::detail

#endif // NEVERD_SYMBOLIC_PARSE_SYMPARSEDETAIL_H
