//===- SourceView.cpp - Reviewed source display projections ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reviewed source display projections.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceView.h"

#include "JsonReader.h"
#include "SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <algorithm>
#include <charconv>
#include <map>

namespace neverd::web {
namespace {
uint64_t decimal(const llvm::json::Object &O, const char *Key) {
  const auto V = O.getString(Key);
  uint64_t Result = 0;
  if (!V || V->empty())
    throw Error("invalid_source_view_policy");
  const auto Parsed = std::from_chars(V->data(), V->data() + V->size(), Result);
  if (Parsed.ec != std::errc{} || Parsed.ptr != V->data() + V->size() ||
      std::to_string(Result) != *V)
    throw Error("invalid_source_view_policy");
  return Result;
}

bool layout(std::string_view Bytes) {
  return std::all_of(Bytes.begin(), Bytes.end(), [](char C) {
    return C == ' ' || C == '\t' || C == '\r' || C == '\n';
  });
}

std::string policyID(const std::vector<SourceReviewRange> &Ranges) {
  if (Ranges.size() > 64)
    throw Error("invalid_source_view_policy");
  std::string Canonical;
  uint64_t End = 0;
  for (const auto &R : Ranges) {
    if (R.Start >= R.End || R.End > MaxJavaScriptBytes)
      throw Error("invalid_source_review_range");
    if (R.Start < End)
      throw Error("overlapping_source_review_ranges");
    Canonical += std::to_string(R.Start) + ":" + std::to_string(R.End) + ";";
    End = R.End;
  }
  return identity(
      "source-view-policy",
      {SourceViewProfile, "structural-with-reviewed-ranges-v1", Canonical});
}
} // namespace

SourceViewPolicy sourceViewPolicy(std::string_view Options) {
  SourceViewPolicy Result;
  if (!Options.empty()) {
    const auto Parsed = parseBoundedJSON(Options, {16384, 8, 1024, 256});
    const auto *O = Parsed.getAsObject();
    if (!O || O->getInteger("schema_version") != 1)
      throw Error("invalid_source_view_policy");
    for (const auto &F : *O)
      if (F.first != "schema_version" && F.first != "reviewed_ranges" &&
          F.first != "locally_reviewed")
        throw Error("invalid_source_view_policy");
    if (const auto *Ranges = O->get("reviewed_ranges")) {
      const auto *A = Ranges->getAsArray();
      if (!A || A->size() > 64)
        throw Error("invalid_source_view_policy");
      if (!A->empty() && O->getBoolean("locally_reviewed") != true)
        throw Error("source_view_local_review_required");
      for (const auto &V : *A) {
        const auto *R = V.getAsObject();
        if (!R || R->size() != 2)
          throw Error("invalid_source_view_policy");
        const auto Start = decimal(*R, "byte_offset"),
                   Size = decimal(*R, "byte_length");
        if (!Size || Start > MaxJavaScriptBytes ||
            Size > MaxJavaScriptBytes - Start)
          throw Error("invalid_source_review_range");
        Result.Reviewed.push_back({Start, Start + Size});
      }
    }
    if (O->get("locally_reviewed") && !O->getBoolean("locally_reviewed"))
      throw Error("invalid_source_view_policy");
  }
  std::sort(Result.Reviewed.begin(), Result.Reviewed.end(),
            [](const auto &A, const auto &B) { return A.Start < B.Start; });
  Result.ID = policyID(Result.Reviewed);
  return Result;
}

SourceView makeSourceView(const SourceAnalysis &S,
                          const SourceBindingAnalysis &B,
                          std::string_view Bytes,
                          const SourceViewPolicy &Policy) {
  validateSourceModel(S);
  if (S.LexemeStatus != "ok" || S.Lexemes.size() > MaxJavaScriptLexemes)
    throw Error("source_lexemes_unavailable");
  if (B.SourceID != S.ID)
    throw Error("source_view_bindings_unavailable");
  if (Policy.ID != policyID(Policy.Reviewed))
    throw Error("invalid_source_view_policy");
  if (Bytes.size() > MaxJavaScriptBytes || sha256(Bytes) != S.BlobHash ||
      !validUtf8(Bytes))
    throw Error("source_view_bytes_mismatch");
  for (const auto &R : Policy.Reviewed)
    if (R.Start >= R.End || !sourceByteBoundary(Bytes, R.Start) ||
        !sourceByteBoundary(Bytes, R.End))
      throw Error("invalid_source_review_range");
  struct Alias {
    uint32_t Binding;
    bool Ambiguous;
  };
  std::map<std::pair<uint64_t, uint64_t>, Alias> Aliases;
  auto Add = [&](uint32_t Node, uint32_t Binding) {
    if (Node == NoSourceIndex || Binding == NoSourceIndex)
      return;
    if (Node >= S.Nodes.size() || Binding >= B.Bindings.size())
      throw Error("invalid_source_binding_model");
    const auto &N = S.Nodes[Node];
    if (B.Bindings[Binding].Conflicting)
      return;
    const auto [I, Inserted] =
        Aliases.try_emplace({N.Start, N.End}, Alias{Binding, false});
    if (!Inserted && I->second.Binding != Binding)
      I->second.Ambiguous = true;
  };
  if (B.Status == "ok" || B.Status == "partial") {
    for (const auto &D : B.Declarations)
      Add(D.Node, D.Binding);
    for (const auto &R : B.References)
      if (R.Resolution == "lexical_binding")
        Add(R.Node, R.Binding);
  }
  SourceView V;
  V.ID =
      identity("source-view", {S.ID, SourceViewProfile, JavaScriptLexemeProfile,
                               B.ID, B.Status, Policy.ID});
  V.SourceID = S.ID;
  V.ArtifactID = S.ArtifactID;
  V.SourceHash = S.BlobHash;
  V.SourceSize = Bytes.size();
  V.PolicyID = Policy.ID;
  V.BindingAnalysisID = B.ID;
  V.BindingStatus = B.Status;
  V.Reviewed = Policy.Reviewed;
  size_t Range = 0;
  auto Emit = [&](uint64_t Start, uint64_t End, std::string_view Kind,
                  std::string_view Replacement,
                  uint32_t Binding = NoSourceIndex) {
    if (Start == End)
      return;
    while (Range < Policy.Reviewed.size() &&
           Policy.Reviewed[Range].End <= Start)
      ++Range;
    bool Reviewed = false;
    if (Range < Policy.Reviewed.size() && Policy.Reviewed[Range].Start < End) {
      const auto &R = Policy.Reviewed[Range];
      if (R.Start > Start || R.End < End)
        throw Error("partial_lexeme_review");
      Reviewed = true;
    }
    const auto Original = Bytes.substr(Start, End - Start);
    const bool Identity =
        Reviewed || Kind == "syntax" || (Kind == "layout" && layout(Original));
    const auto Text = Identity ? Original : Replacement;
    if (Text.size() > MaxSourceViewBytes - V.Text.size() ||
        V.Segments.size() >= MaxSourceViewSegments)
      throw Error("source_view_budget_exceeded");
    const auto At = V.Text.size();
    V.Text += Text;
    V.Segments.push_back({Start, End, At, V.Text.size(), Binding,
                          std::string(Kind), Identity, Reviewed});
    V.ReviewedBytes += Reviewed ? End - Start : 0;
    V.HiddenRegions += !Identity;
    ++V.RegionCounts[std::string(Kind)];
  };
  uint64_t Cursor = 0;
  for (size_t I = 0; I < S.Lexemes.size(); ++I) {
    const auto &L = S.Lexemes[I];
    if (L.Start < Cursor || L.Start >= L.End || L.End > Bytes.size())
      throw Error("invalid_source_lexeme_model");
    Emit(Cursor, L.Start, "layout", "[layout]");
    const auto Raw = Bytes.substr(L.Start, L.End - L.Start);
    if ((L.Kind == "keyword" || L.Kind == "punctuation") && !L.Syntax.empty() &&
        L.Syntax == Raw) {
      Emit(L.Start, L.End, "syntax", "");
    } else if (L.Kind == "identifier") {
      const auto A = Aliases.find({L.Start, L.End});
      const auto Binding = A != Aliases.end() && !A->second.Ambiguous
                               ? A->second.Binding
                               : NoSourceIndex;
      const auto Label = Binding == NoSourceIndex
                             ? "identifier_" + std::to_string(I)
                             : "binding_" + std::to_string(Binding);
      Emit(L.Start, L.End, "identifier", Label, Binding);
    } else {
      std::string Kind = "other";
      for (const auto Allowed :
           {"number", "bigint", "string", "regexp", "template", "boolean",
            "null", "comment", "hashbang"})
        if (L.Kind == Allowed)
          Kind = Allowed;
      const auto Label = "[" + Kind + "]";
      Emit(L.Start, L.End, Kind, Label);
    }
    Cursor = L.End;
  }
  Emit(Cursor, Bytes.size(), "layout", "[layout]");
  return V;
}

SourceViewRange locateSourceView(const SourceView &V, uint64_t Start,
                                 uint64_t End) {
  if (Start > End || End > V.SourceSize)
    throw Error("invalid_source_position");
  const auto First = std::lower_bound(
      V.Segments.begin(), V.Segments.end(), Start,
      [](const auto &S, uint64_t At) { return S.SourceEnd <= At; });
  SourceViewRange R;
  R.FirstSegment = First - V.Segments.begin();
  R.SourceStart = Start;
  R.SourceEnd = End;
  if (Start == End && (First == V.Segments.end() ||
                       Start == First->SourceStart || First->Identity)) {
    R.Start = First == V.Segments.end()
                  ? V.Text.size()
                  : First->Start + (Start - First->SourceStart);
    R.End = R.Start;
    R.LastSegment = R.FirstSegment;
    R.Mapping = "exact_boundary";
    return R;
  }
  if (First == V.Segments.end())
    throw Error("invalid_source_view_model");
  auto Last = Start == End ? std::next(First)
                           : std::lower_bound(First, V.Segments.end(), End,
                                              [](const auto &S, uint64_t At) {
                                                return S.SourceStart < At;
                                              });
  const auto &Tail = *std::prev(Last);
  R.LastSegment = Last - V.Segments.begin();
  R.Start = First->Identity ? First->Start + (Start - First->SourceStart)
                            : First->Start;
  R.End = Tail.Identity ? Tail.Start + (End - Tail.SourceStart) : Tail.End;
  if (!First->Identity)
    R.SourceStart = First->SourceStart;
  if (!Tail.Identity)
    R.SourceEnd = Tail.SourceEnd;
  R.Mapping = std::all_of(First, Last, [](const auto &S) { return S.Identity; })
                  ? "byte_identity"
                  : "region_cover";
  return R;
}
} // namespace neverd::web
