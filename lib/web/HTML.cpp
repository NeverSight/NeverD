//===- HTML.cpp - Captured HTML script evidence ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured HTML script evidence.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/HTML.h"

#include "neverd/web/Error.h"
#include "neverd/web/Source.h"

#include <algorithm>
#include <map>

namespace neverd::web {
namespace {
bool space(char C) {
  return C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f';
}
bool alpha(char C) { return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z'); }
char lower(char C) { return C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : C; }
std::string lower(std::string_view S) {
  std::string R(S);
  for (auto &C : R)
    C = lower(C);
  return R;
}
std::string_view trim(std::string_view S) {
  while (!S.empty() && space(S.front()))
    S.remove_prefix(1);
  while (!S.empty() && space(S.back()))
    S.remove_suffix(1);
  return S;
}
bool javascriptType(std::string_view Type) {
  for (auto MIME :
       {"application/ecmascript", "application/javascript",
        "application/x-ecmascript", "application/x-javascript",
        "text/ecmascript", "text/javascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5", "text/jscript",
        "text/livescript", "text/x-ecmascript", "text/x-javascript"})
    if (Type == MIME)
      return true;
  return false;
}
struct Attribute {
  std::string Value;
  uint64_t Start = 0, Length = 0;
};
struct Tag {
  std::string Name;
  std::map<std::string, Attribute> Attributes;
  uint64_t Start = 0, End = 0;
  bool Closing = false, SelfClosing = false, Duplicate = false;
  const Attribute *get(std::string_view Name) const {
    const auto I = Attributes.find(std::string(Name));
    return I == Attributes.end() ? nullptr : &I->second;
  }
};
struct Reader {
  std::string_view Bytes;
  HTMLDocument A;
  size_t At = 0;
  uint32_t Base = NoHTMLIndex, TemplateDepth = 0;
  std::vector<std::string> ForeignRoots;
  void step(uint64_t N = 1) {
    if (N > MaxHTMLSteps - A.Steps)
      throw Error("html_work_budget_exceeded");
    A.Steps += N;
  }
  char take() {
    step();
    return Bytes[At++];
  }
  bool starts(std::string_view Text, bool Fold = false) {
    if (Text.size() > Bytes.size() - At)
      return false;
    step(Text.size());
    for (size_t I = 0; I < Text.size(); ++I)
      if ((Fold ? lower(Bytes[At + I]) : Bytes[At + I]) != Text[I])
        return false;
    return true;
  }
  void whitespace() {
    while (At < Bytes.size() && space(Bytes[At]))
      take();
  }
  std::string context() const {
    return !ForeignRoots.empty() ? "foreign_content_unverified"
           : TemplateDepth       ? "template_content"
                                 : "html_source_candidate";
  }
  Tag tag() {
    Tag T;
    T.Start = At;
    if (take() != '<')
      throw Error("invalid_html_token_state");
    if (At < Bytes.size() && Bytes[At] == '/') {
      take();
      T.Closing = true;
    }
    while (At < Bytes.size() && !space(Bytes[At]) && Bytes[At] != '/' &&
           Bytes[At] != '>') {
      if (T.Name.size() >= 128 || Bytes[At] == '<' || Bytes[At] == '\'' ||
          Bytes[At] == '"')
        throw Error("html_unsupported_tag_name");
      T.Name += lower(take());
    }
    uint32_t AttributeCount = 0;
    while (At < Bytes.size()) {
      whitespace();
      if (At - T.Start > 65536)
        throw Error("html_tag_budget_exceeded");
      if (At == Bytes.size())
        break;
      if (Bytes[At] == '>') {
        take();
        T.End = At;
        return T;
      }
      if (Bytes[At] == '/') {
        take();
        if (At == Bytes.size() || take() != '>')
          throw Error("html_malformed_self_closing_tag");
        T.SelfClosing = true;
        T.End = At;
        return T;
      }
      if (++AttributeCount > 256)
        throw Error("html_attribute_count_exceeded");
      std::string Name;
      while (At < Bytes.size() && !space(Bytes[At]) && Bytes[At] != '/' &&
             Bytes[At] != '>' && Bytes[At] != '=') {
        if (Name.size() >= 128 || Bytes[At] == '<' || Bytes[At] == '\'' ||
            Bytes[At] == '"')
          throw Error("html_unsupported_attribute_name");
        Name += lower(take());
      }
      if (Name.empty())
        throw Error("html_malformed_attribute");
      whitespace();
      Attribute V;
      V.Start = At;
      if (At < Bytes.size() && Bytes[At] == '=') {
        take();
        whitespace();
        if (At == Bytes.size())
          throw Error("html_truncated_tag");
        const auto Quote =
            Bytes[At] == '\'' || Bytes[At] == '"' ? take() : '\0';
        V.Start = At;
        while (At < Bytes.size() &&
               (Quote ? Bytes[At] != Quote
                      : !space(Bytes[At]) && Bytes[At] != '>')) {
          if (!Quote &&
              (Bytes[At] == '<' || Bytes[At] == '"' || Bytes[At] == '\'' ||
               Bytes[At] == '=' || Bytes[At] == '`'))
            throw Error("html_unsupported_unquoted_attribute");
          if (At - V.Start >= MaxHTMLAttributeBytes)
            throw Error("html_attribute_budget_exceeded");
          take();
        }
        V.Length = At - V.Start;
        if (Quote) {
          if (At == Bytes.size())
            throw Error("html_truncated_attribute");
          take();
          if (At < Bytes.size() && !space(Bytes[At]) && Bytes[At] != '/' &&
              Bytes[At] != '>')
            throw Error("html_missing_attribute_separator");
        } else if (!V.Length)
          throw Error("html_missing_attribute_value");
      }
      V.Value = decodeHTMLAttribute(Bytes.substr(V.Start, V.Length), A.Steps,
                                    A.DecodedBytes);
      if (!T.Attributes.emplace(std::move(Name), std::move(V)).second)
        T.Duplicate = true;
    }
    throw Error("html_truncated_tag");
  }
  void comment() {
    At += 4;
    if (At < Bytes.size() && Bytes[At] == '>') {
      take();
      return;
    }
    if (starts("->")) {
      At += 2;
      return;
    }
    while (At < Bytes.size()) {
      if (starts("-->")) {
        At += 3;
        return;
      }
      if (starts("--!>")) {
        At += 4;
        return;
      }
      take();
    }
    throw Error("html_truncated_comment");
  }
  void declaration() {
    const bool Doctype = starts("<!doctype", true);
    const bool CDATA = !ForeignRoots.empty() && starts("<![CDATA[");
    if (CDATA) {
      At += 9;
      while (At < Bytes.size()) {
        if (starts("]]>")) {
          At += 3;
          return;
        }
        take();
      }
      throw Error("html_truncated_cdata");
    }
    if (Doctype) {
      At += 9;
      if (At == Bytes.size() || !space(Bytes[At]))
        throw Error("html_unsupported_doctype");
      whitespace();
      if (!starts("html", true))
        throw Error("html_unsupported_doctype");
      At += 4;
      if (At == Bytes.size() || (!space(Bytes[At]) && Bytes[At] != '>'))
        throw Error("html_unsupported_doctype");
      whitespace();
      if (At < Bytes.size() && Bytes[At] != '>') {
        const bool Public = starts("public", true);
        if (!Public && !starts("system", true))
          throw Error("html_unsupported_doctype");
        At += 6;
        if (At == Bytes.size() || !space(Bytes[At]))
          throw Error("html_unsupported_doctype");
        whitespace();
        for (unsigned I = 0; I < (Public ? 2u : 1u); ++I) {
          if (I && At < Bytes.size() && Bytes[At] == '>')
            break;
          if (At == Bytes.size() || (Bytes[At] != '\'' && Bytes[At] != '"'))
            throw Error("html_unsupported_doctype");
          const auto Quote = take();
          while (At < Bytes.size() && Bytes[At] != Quote) {
            if (Bytes[At] == '<' || Bytes[At] == '>')
              throw Error("html_unsupported_doctype");
            take();
          }
          if (At == Bytes.size())
            throw Error("html_truncated_declaration");
          take();
          if (At == Bytes.size() || (!space(Bytes[At]) && Bytes[At] != '>'))
            throw Error("html_unsupported_doctype");
          whitespace();
        }
      }
      if (At == Bytes.size() || take() != '>')
        throw Error("html_unsupported_doctype");
      return;
    }
    while (At < Bytes.size()) {
      const char C = take();
      if (C == '>')
        return;
    }
    throw Error("html_truncated_declaration");
  }
  bool endTag(std::string_view Name) {
    if (!starts("</"))
      return false;
    const auto Save = At;
    At += 2;
    const bool Matched = starts(Name, true);
    const auto End = At + Name.size();
    At = Save;
    return Matched && End < Bytes.size() &&
           (space(Bytes[End]) || Bytes[End] == '/' || Bytes[End] == '>');
  }
  struct Body {
    uint64_t Start, End, After;
    bool Closed;
  };
  Body raw(std::string_view Name, bool Script) {
    const auto Start = At;
    enum { Data, Escaped, DoubleEscaped } State = Data;
    unsigned Dashes = 0;
    while (At < Bytes.size()) {
      if (State != DoubleEscaped && endTag(Name)) {
        const auto End = At;
        tag();
        return {Start, End, At, true};
      }
      if (Script && State == Data && starts("<!--")) {
        At += 4;
        State = Escaped;
        Dashes = 2;
        continue;
      }
      if (Script && State != Data && Bytes[At] == '-') {
        take();
        Dashes = std::min(2u, Dashes + 1);
        continue;
      }
      if (Script && State != Data && Bytes[At] == '>' && Dashes == 2) {
        take();
        State = Data;
        Dashes = 0;
        continue;
      }
      Dashes = 0;
      if (Script && State != Data && Bytes[At] == '<') {
        const auto Save = At;
        ++At;
        if (State == DoubleEscaped) {
          if (At == Bytes.size() || Bytes[At] != '/') {
            At = Save;
            take();
            continue;
          }
          ++At;
        }
        const auto Begin = At;
        while (At < Bytes.size() && alpha(Bytes[At]))
          take();
        const bool Delimiter =
            At < Bytes.size() &&
            (space(Bytes[At]) || Bytes[At] == '/' || Bytes[At] == '>');
        if (Delimiter && At - Begin == 6 &&
            lower(Bytes.substr(Begin, 6)) == "script")
          State = State == Escaped ? DoubleEscaped : Escaped;
        if (At == Begin)
          At = Save + 1;
        continue;
      }
      take();
    }
    return {Start, At, At, false};
  }
  void recordScript(const Tag &T) {
    if (A.Scripts.size() + A.Bases.size() >= MaxHTMLRecords)
      throw Error("html_record_budget_exceeded");
    HTMLScript S;
    S.TagStart = T.Start;
    S.TagEnd = T.End;
    S.Base = Base;
    S.Context = context();
    S.Async = T.get("async");
    S.Defer = T.get("defer");
    S.NoModule = T.get("nomodule");
    S.Integrity = T.get("integrity");
    S.CrossOrigin = T.get("crossorigin");
    S.DuplicateAttributes = T.Duplicate;
    const auto *Type = T.get("type"), *Language = T.get("language");
    std::string TypeName;
    if (Type)
      TypeName = lower(Type->Value);
    else if (Language && !Language->Value.empty())
      TypeName = "text/" + lower(Language->Value);
    if ((!Type && (!Language || Language->Value.empty())) ||
        (Type && Type->Value.empty()) || javascriptType(TypeName) ||
        (Type && javascriptType(trim(TypeName)))) {
      S.Kind = "classic";
      S.SourceType = "script";
    } else if (TypeName == "module") {
      S.Kind = "module";
      S.SourceType = "module";
    } else if (TypeName == "importmap" || TypeName == "speculationrules")
      S.Kind = TypeName;
    else
      S.Kind = "data_block";
    if (const auto *Src = T.get("src")) {
      S.HasSource = true;
      S.Reference = Src->Value;
      S.ReferenceStart = Src->Start;
      S.ReferenceLength = Src->Length;
    }
    S.ID = identity("html-script", {A.ID, std::to_string(T.Start)});
    if (!ForeignRoots.empty()) {
      S.BodyStart = S.BodyEnd = S.End = T.End;
      S.BodyStatus = "foreign_script_body_unavailable";
    } else {
      const auto B = raw("script", true);
      S.BodyStart = B.Start;
      S.BodyEnd = B.End;
      S.End = B.After;
      S.Closed = B.Closed;
      const auto Text = Bytes.substr(B.Start, B.End - B.Start);
      step(Text.size());
      S.BodyHash = sha256(Text);
      if (!S.Closed)
        S.BodyStatus = "unterminated_script";
      else if (S.SourceType.empty())
        S.BodyStatus = "non_javascript_data";
      else if (S.HasSource)
        S.BodyStatus = "external_script_body_ignored";
      else if (TemplateDepth)
        S.BodyStatus = "template_script_not_an_entry";
      else if (Text.size() > MaxJavaScriptBytes)
        S.BodyStatus = "source_byte_budget_exceeded";
      else {
        S.BodyStatus = "raw_inline_source_candidate";
        S.InlineArtifactID =
            identity("html-inline-source", {S.ID, S.BodyHash, S.SourceType});
      }
    }
    A.Scripts.push_back(std::move(S));
  }
  void recordBase(const Tag &T) {
    const auto *Href = T.get("href");
    if (!Href)
      return;
    if (A.Scripts.size() + A.Bases.size() >= MaxHTMLRecords)
      throw Error("html_record_budget_exceeded");
    HTMLBase B;
    B.ID = identity("html-base", {A.ID, std::to_string(T.Start)});
    B.Context = context();
    B.Start = T.Start;
    B.End = T.End;
    B.Reference = Href->Value;
    B.ValueStart = Href->Start;
    B.ValueLength = Href->Length;
    B.DuplicateAttributes = T.Duplicate;
    if (Base == NoHTMLIndex && ForeignRoots.empty() && !TemplateDepth) {
      Base = A.Bases.size();
      B.Selected = true;
    }
    A.Bases.push_back(std::move(B));
  }
  void run() {
    if (!validUtf8(Bytes) || Bytes.find('\0') != Bytes.npos)
      throw Error("html_unsupported_encoding");
    while (At < Bytes.size()) {
      if (Bytes[At] != '<') {
        take();
        continue;
      }
      if (++A.TokenCount > 100000)
        throw Error("html_token_budget_exceeded");
      if (starts("<!--")) {
        comment();
        continue;
      }
      if (starts("<!") || starts("<?")) {
        declaration();
        continue;
      }
      const bool Closing = starts("</");
      const auto NameAt = At + (Closing ? 2 : 1);
      if (Closing && NameAt < Bytes.size() && !alpha(Bytes[NameAt])) {
        // Invalid end-tag openers become bogus comments, not ordinary data
        // containing another possible script tag.
        declaration();
        continue;
      }
      if (NameAt == Bytes.size() || !alpha(Bytes[NameAt])) {
        take();
        continue;
      }
      const auto T = tag();
      if (T.Closing) {
        if (T.Name == "template" && ForeignRoots.empty() && TemplateDepth)
          --TemplateDepth;
        if (T.Name == "svg" || T.Name == "math") {
          const auto I =
              std::find(ForeignRoots.rbegin(), ForeignRoots.rend(), T.Name);
          if (I != ForeignRoots.rend())
            ForeignRoots.resize(std::distance(I, ForeignRoots.rend()) - 1);
        }
        continue;
      }
      if (T.Name == "svg" || T.Name == "math") {
        if (!T.SelfClosing)
          ForeignRoots.push_back(T.Name);
        if (ForeignRoots.size() > 64)
          throw Error("html_context_depth_exceeded");
        continue;
      }
      if (T.Name == "template" && ForeignRoots.empty()) {
        if (++TemplateDepth > 64)
          throw Error("html_context_depth_exceeded");
      }
      if (T.Name == "script")
        recordScript(T);
      else if (T.Name == "base")
        recordBase(T);
      else if (ForeignRoots.empty()) {
        if (T.Name == "plaintext") {
          step(Bytes.size() - At);
          At = Bytes.size();
        } else if (T.Name == "style" || T.Name == "xmp" || T.Name == "iframe" ||
                   T.Name == "noembed" || T.Name == "noframes" ||
                   T.Name == "noscript" || T.Name == "textarea" ||
                   T.Name == "title")
          raw(T.Name, false);
        if (T.Name == "meta") {
          if (const auto *Charset = T.get("charset");
              Charset && lower(trim(Charset->Value)) != "utf-8" &&
              lower(trim(Charset->Value)) != "utf8")
            throw Error("html_conflicting_encoding_declaration");
          if (const auto *Equiv = T.get("http-equiv");
              Equiv && lower(trim(Equiv->Value)) == "content-type")
            throw Error("html_legacy_encoding_declaration_unsupported");
        }
      }
    }
    A.Status = "partial";
    A.Reason = "html_tree_construction_and_execution_not_verified";
  }
};
} // namespace
HTMLDocument inspectHTML(std::string_view ArtifactID, std::string_view Bytes) {
  if (Bytes.size() > MaxHTMLBytes)
    throw Error("html_byte_budget_exceeded");
  Reader R{Bytes};
  R.A.ArtifactID = ArtifactID;
  R.A.BlobHash = sha256(Bytes);
  R.A.ID = identity("html-document", {ArtifactID, R.A.BlobHash, HTMLProfile});
  try {
    R.run();
  } catch (const Error &E) {
    R.A.Status =
        std::string_view(E.what()).find("budget_exceeded") !=
                    std::string_view::npos ||
                std::string_view(E.what()) == "html_attribute_count_exceeded" ||
                std::string_view(E.what()) == "html_context_depth_exceeded"
            ? "budget_exceeded"
            : "unavailable";
    R.A.Reason = E.what();
    R.A.Scripts.clear();
    R.A.Bases.clear();
  }
  return std::move(R.A);
}
} // namespace neverd::web
