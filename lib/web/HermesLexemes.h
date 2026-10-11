//===- HermesLexemes.h - Embedded parser token conversion --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Embedded parser token conversion.
///
//===----------------------------------------------------------------------===//

#pragma once

// Parser-only boundary. Do not include LLVM headers in this translation unit.
#include "hermes/Parser/JSParser.h"

#include "neverd/web/Error.h"
#include "neverd/web/Source.h"

#include <algorithm>

namespace neverd::web::hermes_model {
inline void collectLexemes(SourceAnalysis &Source,
                           const hermes::parser::JSParser &Parser,
                           const char *Base, std::string_view Bytes,
                           uint64_t Limit = MaxJavaScriptLexemes) {
  using TK = hermes::parser::TokenKind;
  const auto Tokens = Parser.getStoredTokens();
  const auto Comments = Parser.getStoredComments();
  if (Tokens.size() > Limit || Comments.size() > Limit - Tokens.size())
    throw Error("source_lexeme_budget_exceeded");
  const auto Begin = reinterpret_cast<uintptr_t>(Base);
  auto Span = [&](auto Range) {
    const auto Start = reinterpret_cast<uintptr_t>(Range.Start.getPointer());
    const auto End = reinterpret_cast<uintptr_t>(Range.End.getPointer());
    if (Start < Begin || End < Start || End - Begin > Bytes.size())
      throw Error("invalid_parser_lexeme_span");
    return SourceLexeme{Start - Begin, End - Begin, "other", {}};
  };
  // Keywords used as property/import/export names are target-derived names.
  // Merge identifier ranges before checking tokens (no node x token scan).
  std::vector<std::pair<uint64_t, uint64_t>> Names, Merged;
  for (const auto &N : Source.Nodes)
    if (N.Kind == "Identifier" || N.Kind == "PrivateName")
      Names.emplace_back(N.Start, N.End);
  std::sort(Names.begin(), Names.end());
  for (const auto &N : Names) {
    if (Merged.empty() || Merged.back().second < N.first)
      Merged.push_back(N);
    else
      Merged.back().second = std::max(Merged.back().second, N.second);
  }
  for (const auto &T : Tokens) {
    if (T.getKind() == TK::eof)
      continue;
    auto L = Span(T.getSourceRange());
    switch (T.getKind()) {
#define TOK(NAME, STR)
#define RESWORD(NAME)                                                          \
  case TK::rw_##NAME:                                                          \
    L.Kind = "keyword";                                                        \
    L.Syntax = #NAME;                                                          \
    break;
#define PUNCTUATOR(NAME, STR)                                                  \
  case TK::NAME:                                                               \
    L.Kind = "punctuation";                                                    \
    L.Syntax = STR;                                                            \
    break;
#include "hermes/Parser/TokenKinds.def"
    case TK::identifier:
    case TK::private_identifier:
      L.Kind = "identifier";
      break;
    case TK::numeric_literal:
      L.Kind = "number";
      break;
    case TK::bigint_literal:
      L.Kind = "bigint";
      break;
    case TK::string_literal:
      L.Kind = "string";
      break;
    case TK::regexp_literal:
      L.Kind = "regexp";
      break;
    case TK::no_substitution_template:
    case TK::template_head:
    case TK::template_middle:
    case TK::template_tail:
      L.Kind = "template";
      break;
    default:
      break;
    }
    if (T.getKind() == TK::rw_true || T.getKind() == TK::rw_false) {
      L.Kind = "boolean";
      L.Syntax.clear();
    } else if (T.getKind() == TK::rw_null) {
      L.Kind = "null";
      L.Syntax.clear();
    }
    const auto Name = std::lower_bound(
        Merged.begin(), Merged.end(), L.Start,
        [](const auto &N, uint64_t Start) { return N.second <= Start; });
    if (Name != Merged.end() && Name->first < L.End) {
      L.Kind = "identifier";
      L.Syntax.clear();
    }
    // Escapes or unexpected spellings never become trusted syntax text.
    if (!L.Syntax.empty() &&
        Bytes.substr(L.Start, L.End - L.Start) != L.Syntax) {
      L.Kind = "other";
      L.Syntax.clear();
    }
    Source.Lexemes.push_back(std::move(L));
  }
  for (const auto &C : Comments) {
    auto L = Span(C.getSourceRange());
    L.Kind = C.getKind() == hermes::parser::StoredComment::Kind::Hashbang
                 ? "hashbang"
                 : "comment";
    Source.Lexemes.push_back(std::move(L));
  }
  std::sort(Source.Lexemes.begin(), Source.Lexemes.end(),
            [](const auto &A, const auto &B) { return A.Start < B.Start; });
  uint64_t End = 0;
  for (const auto &L : Source.Lexemes) {
    if (L.Start < End || L.Start >= L.End)
      throw Error("invalid_parser_lexeme_order");
    End = L.End;
  }
  Source.LexemeStatus = "ok";
}
} // namespace neverd::web::hermes_model
