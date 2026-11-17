//===- CSyntaxLexer.cpp - The tokens of the C NeverD emits ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Splits emitted C into tokens (C17 6.4).  Comments and preprocessor lines
/// are tokens too: another spelling of the code keeps them.
///
//===----------------------------------------------------------------------===//

#include "CSyntax.h"

#include "llvm/ADT/StringExtras.h"

using namespace neverd;
using namespace neverd::csyntax;

namespace {

bool isIdentifierStart(char C) { return llvm::isAlpha(C) || C == '_'; }

bool isIdentifierChar(char C) { return llvm::isAlnum(C) || C == '_'; }

llvm::Error lexError(size_t Offset, const llvm::Twine &What) {
  return llvm::createStringError("C text at byte " + llvm::Twine(Offset) +
                                 ": " + What);
}

/// The length of the quoted literal that starts at \p Text[Start], which is
/// \p Quote; none when it does not end on its line.
std::optional<size_t> quotedLength(llvm::StringRef Text, size_t Start,
                                   char Quote) {
  for (size_t I = Start + 1; I < Text.size(); ++I) {
    if (Text[I] == '\\') {
      ++I;
      continue;
    }
    if (Text[I] == '\n')
      return std::nullopt;
    if (Text[I] == Quote)
      return I + 1 - Start;
  }
  return std::nullopt;
}

/// The length of the prefix of a string or character literal at \p Text[I]
/// (`u8`, `u`, `U`, `L`), when a quote follows it.
size_t literalPrefixLength(llvm::StringRef Text, size_t I) {
  for (llvm::StringRef Prefix : {"u8", "u", "U", "L"})
    if (Text.substr(I).starts_with(Prefix) && I + Prefix.size() < Text.size() &&
        (Text[I + Prefix.size()] == '"' || Text[I + Prefix.size()] == '\''))
      return Prefix.size();
  return 0;
}

} // namespace

llvm::Error
csyntax::forEachToken(llvm::StringRef Source,
                      llvm::function_ref<void(const Token &)> Visit) {
  bool LineStart = true;
  size_t I = 0;
  auto Push = [&](TokenKind Kind, size_t Start, size_t Length) {
    Visit({Kind, Source.substr(Start, Length), Start, LineStart});
    LineStart = false;
  };
  while (I < Source.size()) {
    const char C = Source[I];
    if (C == '\n') {
      LineStart = true;
      ++I;
      continue;
    }
    if (C == ' ' || C == '\t' || C == '\r' || C == '\f' || C == '\v') {
      ++I;
      continue;
    }
    if (C == '#' && LineStart) {
      // A directive runs to the end of its line, continued by `\`.
      size_t End = I;
      while (End < Source.size() && Source[End] != '\n') {
        if (Source[End] == '\\' && End + 1 < Source.size() &&
            Source[End + 1] == '\n')
          ++End;
        ++End;
      }
      Push(TokenKind::Directive, I, End - I);
      I = End;
      continue;
    }
    if (Source.substr(I).starts_with("/*")) {
      const size_t Close = Source.find("*/", I + 2);
      if (Close == llvm::StringRef::npos)
        return lexError(I, "unterminated comment");
      Push(TokenKind::Comment, I, Close + 2 - I);
      I = Close + 2;
      continue;
    }
    if (Source.substr(I).starts_with("//")) {
      size_t End = Source.find('\n', I);
      if (End == llvm::StringRef::npos)
        End = Source.size();
      Push(TokenKind::Comment, I, End - I);
      I = End;
      continue;
    }
    if (const size_t Prefix = literalPrefixLength(Source, I);
        Prefix || C == '"' || C == '\'') {
      const char Quote = Source[I + Prefix];
      const auto Length = quotedLength(Source, I + Prefix, Quote);
      if (!Length)
        return lexError(I, "unterminated literal");
      Push(Quote == '"' ? TokenKind::String : TokenKind::Char, I,
           Prefix + *Length);
      I += Prefix + *Length;
      continue;
    }
    if (isIdentifierStart(C)) {
      size_t End = I + 1;
      while (End < Source.size() && isIdentifierChar(Source[End]))
        ++End;
      Push(TokenKind::Identifier, I, End - I);
      I = End;
      continue;
    }
    if (llvm::isDigit(C) ||
        (C == '.' && I + 1 < Source.size() && llvm::isDigit(Source[I + 1]))) {
      // A preprocessing number (C17 6.4.8): digits, letters, `.`, and a sign
      // after an exponent letter.
      size_t End = I + 1;
      while (End < Source.size()) {
        const char D = Source[End];
        if ((D == '+' || D == '-') &&
            llvm::StringRef("eEpP").contains(Source[End - 1])) {
          ++End;
          continue;
        }
        if (!isIdentifierChar(D) && D != '.')
          break;
        ++End;
      }
      Push(TokenKind::Number, I, End - I);
      I = End;
      continue;
    }
    size_t Length = 0;
#define NEVERD_C_PUNCTUATOR(Spelling)                                          \
  if (!Length && Source.substr(I).starts_with(Spelling))                       \
    Length = llvm::StringRef(Spelling).size();
#include "neverd/backend/c/dialect/CPunctuators.def"
    if (!Length)
      return lexError(I, "no C token starts with byte 0x" +
                             llvm::utohexstr(static_cast<unsigned char>(C)));
    Push(TokenKind::Punctuator, I, Length);
    I += Length;
  }
  Visit(
      {TokenKind::End, Source.substr(Source.size()), Source.size(), LineStart});
  return llvm::Error::success();
}

llvm::Expected<std::vector<Token>> csyntax::lex(llvm::StringRef Source) {
  std::vector<Token> Tokens;
  if (auto Error = forEachToken(
          Source, [&](const Token &Value) { Tokens.push_back(Value); }))
    return std::move(Error);
  return Tokens;
}
