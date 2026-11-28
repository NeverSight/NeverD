//===- Template.cpp - Static evidence in embedded and companion templates -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Static evidence in embedded and companion templates.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cctype>

namespace neverd::model {
namespace {
bool ident(char C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
         (C >= '0' && C <= '9') || C == '_';
}
bool space(char C) { return C == ' ' || C == '\t' || C == '\r' || C == '\n'; }
std::string lower(std::string_view S) {
  std::string R(S);
  for (char &C : R)
    if (C >= 'A' && C <= 'Z')
      C += 'a' - 'A';
  return R;
}
struct Token {
  std::string_view Text;
  size_t Pos;
  bool Quoted = false;
};
using Tokens = std::vector<Token>;
bool privateName(std::string_view S) {
  return S.size() > 4 && S.starts_with("__") && S.ends_with("__");
}
} // namespace
void inspectTemplate(Package &P, const std::string &FileId,
                     const std::string &Field, std::string_view S,
                     std::optional<uint64_t> Offset, std::string Variant) {
  std::string Source = FileId + (Field.empty() ? "" : "#" + Field);
  if (S.size() > limits::kTemplateBytes || !llvm::json::isUTF8(S)) {
    P.gap(Source, "oversized template or invalid UTF-8");
    return;
  }
  auto H = digest(S);
  Json Record = {{"source", Source}, {"artifact", FileId},
                 {"field", Field},   {"variant", Variant},
                 {"sha256", H},      {"byte_length", S.size()}};
  if (Offset)
    Record["file_offset"] = *Offset;
  P.Templates.emplace(Source, std::move(Record));
  auto [V, Inserted] = P.Variants.emplace(Variant, std::pair(H, Source));
  if (!Inserted && V->second.first != H)
    P.finding("template.conflict", Source, "review", "template-source",
              "observed",
              "Sources declare different text for this template name. Check "
              "which source the loader selects.",
              {{"variant", Variant},
               {"other_source", V->second.second},
               {"other_sha256", V->second.first},
               {"sha256", H}});
  auto report = [&](const char *Rule, const char *Category, const char *Message,
                    size_t At, size_t Len) {
    Json E = {{"template_offset", At}, {"length", Len}};
    if (Offset)
      E["file_offset"] = *Offset + At;
    P.finding(Rule, Source, "review", Category, "indicator", Message,
              std::move(E));
  };
  auto prose = [&](std::string_view Text, size_t Base) {
    // Match an imperative plus its object. A lone "system" or "instruction"
    // is common model vocabulary. Report positions without retaining the text.
    for (auto Sequence :
         {std::string_view("\xe2\x80\xad"), std::string_view("\xe2\x80\xae")}) {
      auto Hit = Text.find(Sequence);
      if (Hit != Text.npos)
        report(
            "prompt.bidi-override", "text-display",
            "Bidirectional override can change how emitted text is displayed.",
            Base + Hit, Sequence.size());
    }
    unsigned State = 0;
    size_t Start = 0;
    for (size_t I = 0; I < Text.size();) {
      if (!ident(Text[I])) {
        if (!space(Text[I]))
          State = 0;
        ++I;
        continue;
      }
      size_t At = I;
      while (I < Text.size() && ident(Text[I]))
        ++I;
      auto Word = lower(Text.substr(At, I - At));
      if (Word == "ignore" || Word == "disregard" || Word == "override") {
        State = 1;
        Start = At;
      } else if (State == 1 && (Word == "all" || Word == "the" ||
                                Word == "your" || Word == "any")) {
      } else if (State == 1 && (Word == "previous" || Word == "prior" ||
                                Word == "system" || Word == "earlier"))
        State = 2;
      else if (State == 2 && (Word == "instructions" || Word == "prompts" ||
                              Word == "rules")) {
        report("prompt.instruction-override", "prompt-behavior",
               "Instruction-like text may affect the emitted prompt. Review "
               "its context.",
               Base + Start, I - Start);
        State = 0;
      } else
        State = 0;
    }
  };
  std::vector<std::string_view> Blocks;
  bool Raw = false;
  size_t Pos = 0;
  while (Pos < S.size()) {
    P.B.tick();
    size_t At = S.find('{', Pos);
    while (At < S.size() &&
           (At + 1 == S.size() ||
            (S[At + 1] != '{' && S[At + 1] != '%' && S[At + 1] != '#')))
      At = S.find('{', At + 1);
    prose(S.substr(Pos, At == S.npos ? At : At - Pos), Pos);
    if (At == S.npos)
      break;
    char Kind = S[At + 1];
    if (Raw) {
      if (Kind == '%') {
        size_t Q = At + 2;
        if (Q < S.size() && (S[Q] == '-' || S[Q] == '+'))
          ++Q;
        while (Q < S.size() && space(S[Q]))
          ++Q;
        if (S.substr(Q, 6) == "endraw") {
          Q += 6;
          while (Q < S.size() && space(S[Q]))
            ++Q;
          if (Q < S.size() && (S[Q] == '-' || S[Q] == '+'))
            ++Q;
          if (S.substr(Q, 2) == "%}") {
            Raw = false;
            Pos = Q + 2;
            continue;
          }
        }
      }
      Pos = At + 2;
      continue;
    }
    if (Kind == '#') {
      auto End = S.find("#}", At + 2);
      if (End == S.npos) {
        P.gap(Source, "unterminated template comment",
              {{"template_offset", At}});
        return;
      }
      Pos = End + 2;
      continue;
    }
    Tokens T;
    std::vector<char> Brackets;
    size_t I = At + 2;
    bool Closed = false;
    while (I < S.size()) {
      P.B.tick();
      char C = S[I];
      if (C == (Kind == '%' ? '%' : '}') && I + 1 < S.size() &&
          S[I + 1] == '}' && Brackets.empty()) {
        I += 2;
        Closed = true;
        break;
      }
      if (space(C) || ((C == '-' || C == '+') && I == At + 2)) {
        ++I;
        continue;
      }
      size_t Begin = I++;
      if (C == '\'' || C == '"') {
        while (I < S.size() && S[I] != C) {
          if (S[I] == '\\' && I + 1 < S.size())
            ++I;
          ++I;
        }
        if (I == S.size())
          break;
        T.push_back({S.substr(Begin + 1, I - Begin - 1), Begin + 1, true});
        ++I;
      } else {
        if (ident(C))
          while (I < S.size() && ident(S[I]))
            ++I;
        if (C == '(' || C == '[' || C == '{') {
          if (Brackets.size() >= limits::kDepth) {
            P.gap(Source, "template expression nesting limit");
            return;
          }
          Brackets.push_back(C);
        } else if (C == ')' || C == ']' || C == '}') {
          if (Brackets.empty() || Brackets.back() != (C == ')'   ? '('
                                                      : C == ']' ? '['
                                                                 : '{')) {
            P.gap(Source, "unbalanced template expression",
                  {{"template_offset", Begin}});
            return;
          }
          Brackets.pop_back();
        }
        T.push_back({S.substr(Begin, I - Begin), Begin});
      }
    }
    if (!Closed || T.empty()) {
      P.gap(Source, "unfinished or empty template tag",
            {{"template_offset", At}});
      return;
    }
    auto Head = T.front().Text;
    if (Kind == '%') {
      if (Head == "raw") {
        Raw = true;
        Pos = I;
        continue;
      }
      if (Head == "include" || Head == "import" || Head == "from" ||
          Head == "extends")
        report("renderer.external-template", "renderer",
               "Template requests another source. Review loader access.",
               T[0].Pos, Head.size());
      if (Head.starts_with("end")) {
        if (Blocks.empty() || Head.substr(3) != Blocks.back()) {
          P.gap(Source, "unmatched template end tag",
                {{"template_offset", At}});
          return;
        }
        Blocks.pop_back();
      } else if (Head == "if" || Head == "for" || Head == "macro" ||
                 Head == "block" || Head == "filter" || Head == "with" ||
                 Head == "call" || Head == "generation" ||
                 Head == "autoescape" ||
                 (Head == "set" && std::none_of(T.begin(), T.end(), [](auto E) {
                    return !E.Quoted && E.Text == "=";
                  }))) {
        if (Blocks.size() >= limits::kDepth) {
          P.gap(Source, "template block nesting limit");
          return;
        }
        Blocks.push_back(Head);
      }
    }
    for (size_t J = 0; J < T.size(); ++J) {
      auto X = T[J];
      bool Member = J && !T[J - 1].Quoted &&
                    (T[J - 1].Text == "." || T[J - 1].Text == "[");
      bool Attr = J >= 2 && T[J - 1].Text == "(" && T[J - 2].Text == "attr" &&
                  !T[J - 2].Quoted;
      if (!X.Quoted && J + 1 < T.size() && T[J + 1].Text == "(" &&
          !T[J + 1].Quoted &&
          (X.Text == "eval" || X.Text == "exec" || X.Text == "compile"))
        report("renderer.dynamic-evaluation", "renderer",
               "Expression calls a dynamic-evaluation name. Inspect the "
               "renderer bindings.",
               X.Pos, X.Text.size());
      if ((Member || Attr) && privateName(X.Text))
        report("renderer.private-attribute", "renderer",
               "Private attribute access needs renderer policy review.", X.Pos,
               X.Text.size());
      if (Kind == '{' && X.Quoted &&
          (J == 0 || T[J - 1].Text == "+" || T[J - 1].Text == "~"))
        prose(X.Text, X.Pos);
    }
    Pos = I;
  }
  if (Raw || !Blocks.empty())
    P.gap(Source, "unterminated template block");
}
} // namespace neverd::model
