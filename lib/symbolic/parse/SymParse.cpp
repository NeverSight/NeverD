//===- SymParse.cpp - Parsing symbolic expressions ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the operator grammar and \c parseSymExpr.  Lexing lives in
/// SymParseLex.cpp and the literal and call forms in SymParseCall.cpp;
/// SymPrint.cpp implements the inverse rendering operation.  All four share
/// the precedence ladder in the private SymParseDetail.h header.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymParse.h"

#include "SymParseDetail.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>

namespace neverd::symbolic::detail {

namespace {

int binaryPrec(llvm::StringRef S) {
  if (S == "||")
    return PrecLogOr;
  if (S == "&&")
    return PrecLogAnd;
  if (S == "|")
    return PrecBitOr;
  if (S == "^")
    return PrecBitXor;
  if (S == "&")
    return PrecBitAnd;
  if (S == "==" || S == "!=")
    return PrecEquality;
  if (S == "<" || S == "<=" || S == ">" || S == ">=")
    return PrecRelational;
  if (S == "<<" || S == ">>")
    return PrecShift;
  if (S == "+" || S == "-")
    return PrecAdditive;
  if (S == "*" || S == "/" || S == "%")
    return PrecMultiplicative;
  return 0;
}

} // namespace

SymParseResult Parser::run() {
  SymParseResult Result;
  SymRef Root = parseExpr();
  if (!Failed && Lex.peek().K != Token::End)
    fail(Lex.peek().Offset,
         "unexpected '" + Lex.peek().Text + "' after the expression");
  if (Failed) {
    Result.Error = std::move(Error);
    Result.ErrorOffset = ErrorOffset;
    return Result;
  }
  Result.Root = Root;
  return Result;
}

SymRef Parser::parseTernary() {
  SymRef C = parseBinary(PrecLogOr);
  if (Failed || !isPunct("?"))
    return C;

  size_t Offset = Lex.peek().Offset;
  Lex.take();
  DepthGuard Guard(*this, Offset);
  if (Failed)
    return {};
  SymRef T = parseTernary();
  if (Failed || !expectPunct(":"))
    return {};
  SymRef E = parseTernary();
  if (Failed)
    return {};
  return makeIte(C, T, E);
}

SymRef Parser::parseBinary(int MinPrec) {
  SymRef L = parseUnary();
  if (Failed)
    return {};

  while (Lex.peek().K == Token::Punct) {
    llvm::StringRef S = Lex.peek().Text;
    int P = binaryPrec(S);
    if (P == 0 || P < MinPrec)
      break;
    Lex.take();
    // Every infix operator here is left-associative, so the right side stops
    // at the next operator of equal strength.
    SymRef R = parseBinary(P + 1);
    if (Failed)
      return {};
    L = applyBinary(S, L, R);
    if (Failed)
      return {};
  }
  return L;
}

SymRef Parser::applyBinary(llvm::StringRef S, SymRef A, SymRef B) {
  // The short-circuit forms are the only ones that read their operands as
  // conditions rather than as words.
  if (S == "&&")
    return Ctx.mkAnd(toBool(A), toBool(B));
  if (S == "||")
    return Ctx.mkOr(toBool(A), toBool(B));

  unify(A, B);
  if (S == "|")
    return Ctx.mkOr(A, B);
  if (S == "^")
    return Ctx.mkXor(A, B);
  if (S == "&")
    return Ctx.mkAnd(A, B);
  if (S == "==")
    return Ctx.mkEq(A, B);
  if (S == "!=")
    return Ctx.mkNe(A, B);
  if (S == "<")
    return Ctx.mkUlt(A, B);
  if (S == "<=")
    return Ctx.mkUle(A, B);
  if (S == ">")
    return Ctx.mkUgt(A, B);
  if (S == ">=")
    return Ctx.mkUge(A, B);
  if (S == "<<")
    return Ctx.mkShl(A, B);
  if (S == ">>")
    return Ctx.mkLShr(A, B);
  if (S == "+")
    return Ctx.mkAdd(A, B);
  if (S == "-")
    return Ctx.mkSub(A, B);
  if (S == "*")
    return Ctx.mkMul(A, B);
  if (S == "/")
    return Ctx.mkUDiv(A, B);
  if (S == "%")
    return Ctx.mkURem(A, B);
  llvm_unreachable("binaryPrec accepted an operator applyBinary does not know");
}

SymRef Parser::parseUnary() {
  const Token &T = Lex.peek();
  if (T.K == Token::Punct &&
      (T.Text == "~" || T.Text == "-" || T.Text == "!" || T.Text == "+")) {
    llvm::StringRef S = T.Text;
    size_t Offset = T.Offset;
    Lex.take();
    DepthGuard Guard(*this, Offset);
    if (Failed)
      return {};
    SymRef A = parseUnary();
    if (Failed)
      return {};
    if (S == "+")
      return A;
    if (S == "-")
      return Ctx.mkNeg(A);
    if (S == "~")
      return Ctx.mkNot(A);
    // `!` is logical, so at any width above one it asks whether the value is
    // zero rather than complementing its bits.
    uint32_t W = Ctx.width(A);
    return W == 1 ? Ctx.mkNot(A) : Ctx.mkEq(A, Ctx.mkZero(W));
  }
  return parsePrimary();
}

SymRef Parser::parsePrimary() {
  const Token &T = Lex.peek();
  switch (T.K) {
  case Token::End:
    fail(T.Offset, "expected an expression");
    return {};

  case Token::Number:
    return makeLiteral(Lex.take());

  case Token::Ident: {
    Token Id = Lex.take();
    if (isPunct("("))
      return parseCall(Id);

    uint32_t W = parseWidthSuffix(Width);
    if (Failed)
      return {};
    // mkVar asserts on a width change, and text from a user is exactly where
    // that would fire, so the clash becomes a diagnostic here instead.
    if (std::optional<uint32_t> Existing = Ctx.findVar(Id.Text)) {
      uint32_t Have = Ctx.varInfo(*Existing).Width;
      if (Have != W) {
        fail(Id.Offset, "'" + Id.Text + "' was already declared at width " +
                            llvm::Twine(Have));
        return {};
      }
    }
    return Ctx.mkVar(Id.Text, W);
  }

  case Token::Punct: {
    if (T.Text == "(") {
      size_t Offset = T.Offset;
      Lex.take();
      DepthGuard Guard(*this, Offset);
      if (Failed)
        return {};
      SymRef E = parseExpr();
      if (Failed || !expectPunct(")"))
        return {};
      return E;
    }
    fail(T.Offset, "unexpected '" + T.Text + "'");
    return {};
  }
  }
  llvm_unreachable("unhandled token kind");
}

} // namespace neverd::symbolic::detail

namespace neverd::symbolic {

SymParseResult parseSymExpr(SymContext &Ctx, llvm::StringRef Text,
                            uint32_t Width) {
  return parseSymExpr(Ctx, Text, Width, SymParseOptions{});
}

SymParseResult parseSymExpr(SymContext &Ctx, llvm::StringRef Text,
                            uint32_t Width, const SymParseOptions &Options) {
  return detail::Parser(Ctx, Text, Width, Options).run();
}

} // namespace neverd::symbolic
