//===- SymParseCall.cpp - Literals and call forms of the syntax -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the leaf and call halves of the grammar: numeric literals, the
/// `#<bits>` width suffix, and the named forms that stand in for operators
/// with no C spelling — the signed and rotating ones, and everything that
/// changes width.
///
//===----------------------------------------------------------------------===//

#include "SymParseDetail.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>

namespace neverd::symbolic::detail {

namespace {

/// True for the forms whose trailing arguments are bit positions rather than
/// expressions.  They are parsed as plain integers because a bit count is not
/// a value in the expression's word size — `zext(x, 256)` has to mean 256 even
/// when the surrounding width is 8.
bool takesBitCounts(llvm::StringRef F) {
  return F == "zext" || F == "sext" || F == "trunc" || F == "extract";
}

} // namespace

SymRef Parser::makeLiteral(const Token &N) {
  if (N.Digits.empty()) {
    fail(N.Offset, "malformed numeric literal");
    return {};
  }
  uint32_t W = parseWidthSuffix(Width);
  if (Failed)
    return {};

  unsigned Needed = llvm::APInt::getBitsNeeded(N.Digits, N.Radix);
  llvm::APInt V(std::max(Needed, W), N.Digits, N.Radix);
  return Ctx.mkConst(V.zextOrTrunc(W));
}

uint32_t Parser::parseWidthSuffix(uint32_t Default) {
  if (!isPunct("#"))
    return Default;
  size_t Offset = Lex.peek().Offset;
  Lex.take();
  uint64_t W = 0;
  if (!parsePlainInt(W))
    return Default;
  if (!validateWidth(W, Offset)) {
    return Default;
  }
  return static_cast<uint32_t>(W);
}

bool Parser::parsePlainInt(uint64_t &Out) {
  const Token &T = Lex.peek();
  if (T.K != Token::Number || T.Digits.empty()) {
    fail(T.Offset, "expected a bit count");
    return false;
  }
  Token N = Lex.take();
  if (N.Digits.getAsInteger(N.Radix, Out)) {
    fail(N.Offset, "bit count is out of range");
    return false;
  }
  return true;
}

SymRef Parser::parseCall(const Token &Name) {
  DepthGuard Guard(*this, Name.Offset);
  if (Failed)
    return {};
  Lex.take(); // '('
  if (Name.Text == "varid") {
    const size_t IdOffset = Lex.peek().Offset;
    uint64_t Id = 0;
    if (!parsePlainInt(Id) || !expectPunct(")"))
      return {};
    if (Id >= Ctx.numVars()) {
      fail(IdOffset, "symbolic variable id is out of range");
      return {};
    }
    const uint32_t Width = Ctx.varInfo(static_cast<uint32_t>(Id)).Width;
    if (!validateWidth(Width, IdOffset))
      return {};
    return Ctx.varRef(static_cast<uint32_t>(Id));
  }
  return takesBitCounts(Name.Text) ? parseWidthCall(Name)
                                   : parseVariadicCall(Name);
}

SymRef Parser::parseWidthCall(const Token &Name) {
  llvm::StringRef F = Name.Text;
  SymRef A = parseExpr();
  if (Failed)
    return {};

  unsigned NumCounts = F == "extract" ? 2 : 1;
  uint64_t Counts[2] = {0, 0};
  size_t Offsets[2] = {0, 0};
  for (unsigned I = 0; I < NumCounts; ++I) {
    if (!expectPunct(","))
      return {};
    Offsets[I] = Lex.peek().Offset;
    if (!parsePlainInt(Counts[I]))
      return {};
  }
  if (!expectPunct(")"))
    return {};

  uint32_t AW = Ctx.width(A);
  if (F == "extract") {
    uint64_t Low = Counts[0], W = Counts[1];
    if (!validateWidth(W, Offsets[1]))
      return {};
    if (Low > AW || W > uint64_t(AW) - Low) {
      fail(Offsets[0], "extract window does not fit in a " + llvm::Twine(AW) +
                           "-bit operand");
      return {};
    }
    return Ctx.mkExtract(A, static_cast<uint32_t>(Low),
                         static_cast<uint32_t>(W));
  }

  uint64_t W = Counts[0];
  if (!validateWidth(W, Offsets[0])) {
    return {};
  }
  if (F == "trunc") {
    if (W > AW) {
      fail(Offsets[0],
           "trunc must not widen a " + llvm::Twine(AW) + "-bit operand");
      return {};
    }
    return Ctx.mkExtract(A, 0, static_cast<uint32_t>(W));
  }
  if (W < AW) {
    fail(Offsets[0],
         F + " must not narrow a " + llvm::Twine(AW) + "-bit operand");
    return {};
  }
  return F == "zext" ? Ctx.mkZExt(A, static_cast<uint32_t>(W))
                     : Ctx.mkSExt(A, static_cast<uint32_t>(W));
}

SymRef Parser::parseVariadicCall(const Token &Name) {
  llvm::SmallVector<SymRef, 4> Args;
  if (!isPunct(")")) {
    do {
      SymRef A = parseExpr();
      if (Failed)
        return {};
      Args.push_back(A);
    } while (consumePunct(","));
  }
  if (!expectPunct(")"))
    return {};
  return applyVariadic(Name, Args);
}

void Parser::unifyAll(llvm::MutableArrayRef<SymRef> Args) {
  uint32_t W = 0;
  for (SymRef A : Args)
    W = std::max(W, Ctx.width(A));
  for (SymRef &A : Args)
    if (Ctx.width(A) != W)
      A = Ctx.mkZExt(A, W);
}

SymRef Parser::applyVariadic(const Token &Name,
                             llvm::MutableArrayRef<SymRef> Args) {
  llvm::StringRef F = Name.Text;
  size_t N = Args.size();

  auto arity = [&](size_t Want) {
    if (N == Want)
      return true;
    fail(Name.Offset, F + " takes " + llvm::Twine(Want) + " argument" +
                          (Want == 1 ? "" : "s") + ", got " + llvm::Twine(N));
    return false;
  };

  if (F == "add" || F == "mul" || F == "and" || F == "or" || F == "xor" ||
      F == "concat") {
    if (N == 0) {
      fail(Name.Offset, F + " needs at least one argument");
      return {};
    }
    // Concatenation is the one variadic that means to keep its operands at
    // different widths.
    if (F == "concat")
      return Ctx.mkConcat(Args);
    unifyAll(Args);
    if (F == "add")
      return Ctx.mkAdd(Args);
    if (F == "mul")
      return Ctx.mkMul(Args);
    if (F == "and")
      return Ctx.mkAnd(Args);
    if (F == "or")
      return Ctx.mkOr(Args);
    return Ctx.mkXor(Args);
  }

  if (F == "not" || F == "neg") {
    if (!arity(1))
      return {};
    return F == "not" ? Ctx.mkNot(Args[0]) : Ctx.mkNeg(Args[0]);
  }

  if (F == "ite") {
    if (!arity(3))
      return {};
    return makeIte(Args[0], Args[1], Args[2]);
  }

  if (F == "sub" || F == "udiv" || F == "sdiv" || F == "urem" || F == "srem" ||
      F == "shl" || F == "lshr" || F == "ashr" || F == "rol" || F == "ror" ||
      F == "eq" || F == "ne" || F == "ult" || F == "ule" || F == "ugt" ||
      F == "uge" || F == "slt" || F == "sle" || F == "sgt" || F == "sge") {
    if (!arity(2))
      return {};
    SymRef A = Args[0], B = Args[1];
    unify(A, B);
    if (F == "sub")
      return Ctx.mkSub(A, B);
    if (F == "udiv")
      return Ctx.mkUDiv(A, B);
    if (F == "sdiv")
      return Ctx.mkSDiv(A, B);
    if (F == "urem")
      return Ctx.mkURem(A, B);
    if (F == "srem")
      return Ctx.mkSRem(A, B);
    if (F == "shl")
      return Ctx.mkShl(A, B);
    if (F == "lshr")
      return Ctx.mkLShr(A, B);
    if (F == "ashr")
      return Ctx.mkAShr(A, B);
    if (F == "rol")
      return Ctx.mkRol(A, B);
    if (F == "ror")
      return Ctx.mkRor(A, B);
    if (F == "eq")
      return Ctx.mkEq(A, B);
    if (F == "ne")
      return Ctx.mkNe(A, B);
    if (F == "ult")
      return Ctx.mkUlt(A, B);
    if (F == "ule")
      return Ctx.mkUle(A, B);
    if (F == "ugt")
      return Ctx.mkUgt(A, B);
    if (F == "uge")
      return Ctx.mkUge(A, B);
    if (F == "slt")
      return Ctx.mkSlt(A, B);
    if (F == "sle")
      return Ctx.mkSle(A, B);
    if (F == "sgt")
      return Ctx.mkSgt(A, B);
    return Ctx.mkSge(A, B);
  }

  fail(Name.Offset, "unknown function '" + F + "'");
  return {};
}

} // namespace neverd::symbolic::detail
