//===- SymParseLex.cpp - Lexing symbolic expression text ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the lexer declared in SymParseDetail.h: the scan from input text
/// to the identifier, number and punctuator tokens the grammar in SymParse.cpp
/// consumes.
///
//===----------------------------------------------------------------------===//

#include "SymParseDetail.h"

#include <cctype>

namespace neverd::symbolic::detail {

namespace {

bool isIdentStart(char C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
}

/// Decompiler-generated names carry `.` and `$`, so an identifier admits them
/// rather than forcing every caller to quote.
bool isIdentBody(char C) {
  return isIdentStart(C) || (C >= '0' && C <= '9') || C == '.' || C == '$';
}

bool isDigit(char C) { return C >= '0' && C <= '9'; }

bool isRadixDigit(char C, unsigned Radix) {
  switch (Radix) {
  case 16:
    return isDigit(C) || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F');
  case 2:
    return C == '0' || C == '1';
  default:
    return isDigit(C);
  }
}

/// Punctuators, longest first, so that scanning them in order is maximal
/// munch: `<<` is found before `<`, and `!=` before `!`.
constexpr const char *kPuncts[] = {
    "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "?", ":", "|", "^", "&",
    "<",  ">",  "+",  "-",  "*",  "/",  "%",  "~",  "!", "(", ")", ",", "#",
};

} // namespace

void Lexer::advance() {
  while (Pos < Text.size() &&
         std::isspace(static_cast<unsigned char>(Text[Pos])))
    ++Pos;

  Cur = Token{};
  Cur.Offset = Pos;
  if (Pos >= Text.size())
    return;

  char C = Text[Pos];

  if (isIdentStart(C)) {
    size_t Start = Pos;
    while (Pos < Text.size() && isIdentBody(Text[Pos]))
      ++Pos;
    Cur.K = Token::Ident;
    Cur.Text = Text.slice(Start, Pos);
    return;
  }

  if (isDigit(C)) {
    unsigned Radix = 10;
    if (C == '0' && Pos + 1 < Text.size()) {
      char P = Text[Pos + 1];
      if (P == 'x' || P == 'X')
        Radix = 16;
      else if (P == 'b' || P == 'B')
        Radix = 2;
      if (Radix != 10)
        Pos += 2;
    }
    size_t Start = Pos;
    while (Pos < Text.size() && isRadixDigit(Text[Pos], Radix))
      ++Pos;
    Cur.K = Token::Number;
    Cur.Digits = Text.slice(Start, Pos);
    Cur.Radix = Radix;
    return;
  }

  for (const char *P : kPuncts) {
    llvm::StringRef S(P);
    if (Text.substr(Pos).starts_with(S)) {
      Cur.K = Token::Punct;
      Cur.Text = Text.slice(Pos, Pos + S.size());
      Pos += S.size();
      return;
    }
  }

  // Anything else comes back as a one-character punctuator, so the parser
  // reports it as unexpected at the right offset instead of looping.
  Cur.K = Token::Punct;
  Cur.Text = Text.slice(Pos, Pos + 1);
  ++Pos;
}

} // namespace neverd::symbolic::detail
