//===- SourceRecovery.cpp - Verified readable source recovery ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verified readable source recovery.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceRecovery.h"

#include "RecoveryParser.h"

#include "neverd/web/Error.h"

#include <algorithm>

namespace neverd::web {
namespace {
bool sameTree(const SourceAnalysis &A, const SourceAnalysis &B) {
  if (A.Nodes.size() != B.Nodes.size())
    return false;
  for (size_t I = 0; I < A.Nodes.size(); ++I) {
    const auto &X = A.Nodes[I], &Y = B.Nodes[I];
    if (X.Kind != Y.Kind || X.Strict != Y.Strict ||
        X.Children.size() != Y.Children.size() ||
        X.Attributes.size() != Y.Attributes.size())
      return false;
    for (size_t J = 0; J < X.Children.size(); ++J) {
      const auto &XC = X.Children[J], &YC = Y.Children[J];
      if (XC.Field != YC.Field || XC.Index != YC.Index ||
          XC.Ordinal != YC.Ordinal)
        return false;
    }
    for (size_t J = 0; J < X.Attributes.size(); ++J)
      if (X.Attributes[J].Field != Y.Attributes[J].Field ||
          X.Attributes[J].Value != Y.Attributes[J].Value)
        return false;
  }
  return true;
}
} // namespace

ReadableSource recoverReadableJavaScript(std::string_view ArtifactID,
                                         std::string_view Text,
                                         std::string_view SourceType) {
  ReadableSource Result;
  try {
    auto Parsed = inspectRecoveryJavaScript(ArtifactID, Text, SourceType);
    Result.ParseStatus = Parsed.ParseStatus;
    Result.Nodes = Parsed.Nodes.size();
    Result.Diagnostics = Parsed.Diagnostics;
    if (Parsed.ParseStatus != "parsed" || Parsed.LexemeStatus != "ok") {
      Result.Status = Parsed.ParseStatus == "parsed" ? Parsed.LexemeStatus
                                                     : Parsed.ParseStatus;
      return Result;
    }
    std::string Candidate;
    uint64_t At = 0, Depth = 0;
    bool NewLine = false;
    auto Append = [&](std::string_view Value) {
      if (Value.size() > MaxRecoverySourceBytes - Candidate.size())
        throw Error("readable_source_budget_exceeded");
      Candidate += Value;
    };
    for (const auto &L : Parsed.Lexemes) {
      Append(Text.substr(At, L.Start - At));
      if (L.Syntax == "}" && Depth)
        --Depth;
      if (!Candidate.empty() && (NewLine || L.Syntax == "}")) {
        if (Candidate.back() != '\n' && Candidate.back() != '\r')
          Append("\n");
        Append(std::string(size_t(std::min<uint64_t>(Depth, 32)) * 2, ' '));
      }
      Append(Text.substr(L.Start, L.End - L.Start));
      At = L.End;
      if (L.Syntax == "{")
        ++Depth;
      NewLine = L.Syntax == "{" || L.Syntax == "}" || L.Syntax == ";";
    }
    Append(Text.substr(At));
    Append("\n");
    auto Reparsed =
        inspectRecoveryJavaScript(ArtifactID, Candidate, SourceType);
    if (Reparsed.ParseStatus != "parsed" || !sameTree(Parsed, Reparsed)) {
      Result.Status = "readable_reparse_mismatch";
      // Offsets in these diagnostics refer to the unpublished candidate.
      // Do not attach them to the original source evidence.
      return Result;
    }
    Result.Status = "verified_same_parser_tree";
    Result.Text = std::move(Candidate);
  } catch (const Error &E) {
    Result.Status = E.what();
  }
  return Result;
}
} // namespace neverd::web
