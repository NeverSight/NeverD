//===- Source.h - JavaScript source evidence ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// JavaScript source evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace neverd::web {

struct SyntaxChild {
  std::string Field;
  uint32_t Index = 0;
  uint32_t Ordinal = 0;
};

struct SyntaxAttribute {
  std::string Field;
  // Private analysis data. JS strings are UTF-16 code units, including lone
  // surrogates and NUL. Never serialize these as ordinary UTF-8 metadata.
  std::variant<std::u16string, bool, double> Value;
};

struct SyntaxNode {
  std::string ID;
  std::string ParentID;
  std::string Kind;
  uint64_t Start = 0;
  uint64_t End = 0;
  bool Strict = false;
  std::vector<SyntaxChild> Children;
  std::vector<SyntaxAttribute> Attributes;

  const SyntaxAttribute *attribute(std::string_view Field) const {
    for (const auto &A : Attributes)
      if (A.Field == Field)
        return &A;
    return nullptr;
  }
  const std::u16string *text(std::string_view Field) const {
    const auto *A = attribute(Field);
    return A ? std::get_if<std::u16string>(&A->Value) : nullptr;
  }
  bool flag(std::string_view Field) const {
    const auto *A = attribute(Field);
    const auto *Value = A ? std::get_if<bool>(&A->Value) : nullptr;
    return Value && *Value;
  }
};

struct SourceDiagnostic {
  std::string Code;
  int64_t ByteOffset = -1;
};

/// Owned lexer evidence. Syntax is analyzer-selected fixed spelling only;
/// identifiers, literal values and comments never enter that field.
struct SourceLexeme {
  uint64_t Start = 0, End = 0;
  std::string Kind, Syntax;
};

struct SourceAnalysis {
  std::string ID;
  std::string ArtifactID;
  std::string BlobHash;
  std::string SourceType;
  std::string ParseStatus;
  std::vector<SyntaxNode> Nodes;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t CommentCount = 0;
  std::string LexemeStatus = "not_available";
  std::vector<SourceLexeme> Lexemes;
};

inline constexpr std::string_view JavaScriptParserProfile =
    "hermes-602befee-js-v3";
inline constexpr uint64_t MaxJavaScriptBytes = 1024 * 1024;
inline constexpr uint64_t MaxJavaScriptNodes = 100000;
inline constexpr uint64_t MaxJavaScriptStringUnits = 2 * 1024 * 1024;
inline constexpr uint64_t MaxJavaScriptLexemes = 200000;
inline constexpr std::string_view JavaScriptLexemeProfile =
    "hermes-602befee-lexemes-v1";

/// Parse and validate supplied text only. No VM, eval, imports, compiler,
/// target plugins, subprocesses or automatic source-map retrieval.
SourceAnalysis inspectJavaScript(std::string_view ArtifactID,
                                 std::string_view Bytes,
                                 std::string_view SourceType);

} // namespace neverd::web
