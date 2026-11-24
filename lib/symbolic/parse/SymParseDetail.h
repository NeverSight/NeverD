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

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymParse.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

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

inline bool isIdentStart(char C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
}

/// Decompiler-generated names carry `.` and `$`, so an identifier admits them
/// rather than forcing every caller to quote.
inline bool isIdentBody(char C) {
  return isIdentStart(C) || (C >= '0' && C <= '9') || C == '.' || C == '$';
}

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

  llvm::StringRef parenthesizedSpelling(size_t Begin) const;
  llvm::StringRef from(size_t Begin) const { return Text.drop_front(Begin); }
  void skipSpelling(llvm::StringRef Spelling) {
    Pos = Spelling.end() - Text.begin();
    advance();
  }
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
         const SymParseOptions &Options, bool ReuseProductTerms = true)
      : Reuse(ReuseProductTerms ? CallReuse::OutsideProduct
                                : CallReuse::Disabled),
        Ctx(Ctx), Lex(Text), Width(Width), Options(Options) {
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
  SymRef parseGroup(size_t Offset);

  // Keys borrow the input and values belong to this parser's context. Only
  // success at the same or a greater depth discharges the nesting policy.
  template <typename Parse>
  SymRef reuseProductTerm(size_t Begin, Parse ParseNormally) {
    const auto Spelling = Lex.parenthesizedSpelling(Begin);
    if (Spelling.empty())
      return reuseLongProductTerm(Begin, ParseNormally);
    const auto It = ProductTerms.find(Spelling);
    if (It != ProductTerms.end() && Depth <= It->second.Depth) {
      Lex.skipSpelling(Spelling);
      return It->second.Value;
    }
    const SymRef Result = ParseNormally();
    if (Result)
      ProductTerms[Spelling] = {Result, Depth};
    return Result;
  }

  // Longer fields often share a prefix. Only an exact match through the
  // closing delimiter of a previously validated field can skip the grammar.
  template <typename Parse>
  SymRef reuseLongProductTerm(size_t Begin, Parse ParseNormally) {
    const auto Input = Lex.from(Begin);
    const auto Prefix =
        Input.take_front(limits::symbolic::kSymParseProductSpellingBytes);
    const auto It = LongProductTerms.find(Prefix);
    if (It != LongProductTerms.end() && Depth <= It->second.Term.Depth) {
      const auto Saved = It->second.Spelling;
      if (Input.drop_front(Prefix.size())
              .starts_with(Saved.drop_front(Prefix.size()))) {
        Lex.skipSpelling(Input.take_front(Saved.size()));
        return It->second.Term.Value;
      }
    }
    const auto Result = ParseNormally();
    if (Result) {
      const auto Full = Input.take_front(Lex.peek().Offset - Begin).rtrim();
      LongProductTerms[Prefix] = {Full, {Result, Depth}};
    }
    return Result;
  }
  SymRef parseCall(const Token &Name);
  SymRef parseCallArguments(const Token &Name);
  SymRef parseVariadicCall(const Token &Name);
  SymRef parseWidthCall(const Token &Name);
  SymRef makeLiteral(const Token &N, bool Negative = false);

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

  // The spelling borrows the input for this parse. Explicitly different
  // widths use the ordinary literal interner.
  llvm::DenseMap<std::pair<llvm::StringRef, unsigned>,
                 std::pair<SymRef, SymRef>>
      WideLiterals;
  enum class CallReuse {
    Disabled,
    OutsideProduct,
    Factor,
    InsideCall,
    InsideGroup
  };
  CallReuse Reuse;
  struct ParsedTerm {
    SymRef Value;
    size_t Depth;
  };
  llvm::DenseMap<llvm::StringRef, ParsedTerm> ProductTerms;
  struct LongTerm {
    llvm::StringRef Spelling;
    ParsedTerm Term;
  };
  llvm::DenseMap<llvm::StringRef, LongTerm> LongProductTerms;
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
