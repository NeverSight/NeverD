//===- Comparison.cpp - Compatible local model storage comparisons --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compatible local model storage comparisons.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>

namespace neverd::model {
namespace {
struct Digests {
  std::string Candidate, Base;
  Json Numeric;
  uint64_t Read = 0;
  std::string StopReason;
};
unsigned numericWidth(const Tensor &T) {
  return (T.Type == "F32" || T.Type == "onnx:1")    ? 4
         : (T.Type == "F64" || T.Type == "onnx:11") ? 8
                                                    : 0;
}

struct StorageUnit {
  uint64_t Bytes, Elements;
};
StorageUnit storageUnit(const Tensor &T) {
  if (T.Packing)
    return {T.Packing->Bytes, T.Packing->Elements};
  if (!T.Bytes || !*T.Bytes || !T.Elements)
    return {1, 0};
  auto GCD = std::gcd(*T.Bytes, T.Elements);
  return {*T.Bytes / GCD, T.Elements / GCD};
}
Digests compareRanges(Package &C, const Tensor &A, Package &B, const Tensor &Z,
                      bool Big, uint64_t Length) {
  SHA256Stream AH, BH;
  auto &AF = C.file(A.FileId);
  auto &BF = B.file(Z.FileId);
  AF.verify();
  BF.verify();
  std::array<char, limits::kChunk> Left, Right;
  const unsigned Width = numericWidth(A);
  auto Value = [&](const char *At) -> double {
    auto Order = Big ? llvm::endianness::big : llvm::endianness::little;
    if (Width == 4)
      return std::bit_cast<float>(llvm::support::endian::read32(At, Order));
    return std::bit_cast<double>(llvm::support::endian::read64(At, Order));
  };
  uint64_t Finite = 0, Unequal = 0, Nonfinite = 0, Overflow = 0, Outside = 0,
           Done = 0;
  double MaxAbs = 0;
  std::string StopReason;
  const auto &O = C.B.Opts;
  while (Done < Length) {
    try {
      C.B.tick();
    } catch (const Stop &E) {
      StopReason = E.what();
      break;
    }
    const auto N = std::min<uint64_t>(Left.size(), Length - Done);
    AF.read(A.Offset + Done, std::span(Left).first(N));
    BF.read(Z.Offset + Done, std::span(Right).first(N));
    AH.update(llvm::StringRef(Left.data(), N));
    BH.update(llvm::StringRef(Right.data(), N));
    if (Width)
      for (uint64_t J = 0; J < N; J += Width) {
        auto X = Value(Left.data() + J), Y = Value(Right.data() + J);
        if (!std::isfinite(X) || !std::isfinite(Y)) {
          ++Nonfinite;
          continue;
        }
        ++Finite;
        Unequal += X != Y;
        double Abs = std::abs(X - Y);
        if (std::isfinite(Abs))
          MaxAbs = std::max(MaxAbs, Abs);
        else
          ++Overflow;
        if (Abs <= O.AbsoluteTolerance)
          continue;
        // Scale before subtracting opposite extremes to avoid overflow in
        // either the difference or the relative tolerance bound.
        const double Scale = std::max(std::abs(X), std::abs(Y));
        if (O.RelativeTolerance == 0 ||
            std::abs(X / Scale - Y / Scale) >
                O.AbsoluteTolerance / Scale + O.RelativeTolerance)
          ++Outside;
      }
    C.WeightBytesRead += N;
    B.WeightBytesRead += N;
    C.B.CompareLeft -= 2 * N;
    Done += N;
  }
  Json Numeric = nullptr;
  if (Width)
    Numeric = {{"finite_pairs", Finite},
               {"unequal_finite_pairs", Unequal},
               {"nonfinite_pairs", Nonfinite},
               {"outside_tolerance_pairs", Outside},
               {"absolute_difference_overflows", Overflow},
               {"maximum_finite_absolute_difference",
                Finite > Overflow ? Json(MaxAbs) : Json(nullptr)}};
  return {llvm::toHex(AH.final(), true), llvm::toHex(BH.final(), true),
          std::move(Numeric), Done, std::move(StopReason)};
}
std::set<std::string> representations(const Package &P) {
  std::set<std::string> R;
  for (auto &A : P.Artifacts) {
    auto F = A.at("format").get<std::string>();
    if (F == "gguf")
      R.insert(F + "/" + A.at("byte_order").get<std::string>());
    else if (F == "safetensors" || F == "onnx")
      R.insert(F);
  }
  return R;
}
auto components(const Package &P) {
  auto M = P.Fingerprints;
  for (auto &[Variant, Info] : P.Variants)
    M["template/" + Variant] = Info.first;
  return M;
}
// Digests identify equal serialized tokenizer files without parsing a large
// vocabulary twice. For changed, admitted JSON, compare the decoded ID map.
Json tokenizerDifference(Package &C, Package &B, const std::string &Name) {
  auto load = [&](Package &P) {
    auto &F = P.file(Name);
    F.verify();
    if (F.size() > limits::kJsonBytes || F.size() > P.B.HeaderLeft)
      throw Error("tokenizer JSON detail exceeds remaining metadata budget");
    return parseObject(F.header(0, F.size(), P.B), P.B);
  };
  try {
    auto L = load(C), R = load(B);
    const Json *X = &L, *Y = &R;
    Json Result = {{"status", "compared"}, {"changed_sections", Json::array()}};
    if (Name == "tokenizer.json") {
      for (auto Key : {"normalizer", "pre_tokenizer", "post_processor",
                       "decoder", "added_tokens", "model"}) {
        if (L.contains(Key) != R.contains(Key) ||
            (L.contains(Key) && L.at(Key) != R.at(Key)))
          Result["changed_sections"].push_back(Key);
      }
      if (!L.contains("model") || !R.contains("model") ||
          !L.at("model").is_object() || !R.at("model").is_object() ||
          !L.at("model").contains("vocab") ||
          !R.at("model").contains("vocab")) {
        Result["id_mapping"] = "unavailable";
        return Result;
      }
      X = &L.at("model").at("vocab");
      Y = &R.at("model").at("vocab");
    }
    if (!X->is_object() || !Y->is_object()) {
      Result["id_mapping"] = "unsupported vocabulary encoding";
      return Result;
    }
    uint64_t Added = 0, Removed = 0, Changed = 0;
    for (auto &[Token, Id] : X->items()) {
      C.B.tick();
      auto It = Y->find(Token);
      natural(Id);
      if (It == Y->end())
        ++Added;
      else if (natural(*It) != natural(Id))
        ++Changed;
    }
    for (auto &[Token, Id] : Y->items()) {
      C.B.tick();
      natural(Id);
      if (!X->contains(Token))
        ++Removed;
    }
    Result["id_mapping"] = {{"added_tokens", Added},
                            {"removed_tokens", Removed},
                            {"reassigned_ids", Changed}};
    return Result;
  } catch (const Stop &) {
    throw;
  } catch (const Error &E) {
    return {{"status", "skipped"}, {"reason", E.what()}};
  }
}
} // namespace
Json compare(Package &C, Package &B) {
  const auto &O = C.B.Opts;
  Json Reasons = Json::array(), Rows = Json::array(), Changes = Json::array(),
       Counts = Json::object();
  if (C.GapCount || B.GapCount)
    Reasons.push_back("incomplete input inventory");
  if (C.Architectures != B.Architectures)
    Reasons.push_back("different architecture declarations");
  auto Formats = representations(C);
  if (Formats != representations(B))
    Reasons.push_back("different storage formats or byte orders");
  auto Left = components(C), Right = components(B);
  std::set<std::string> Keys;
  for (auto &[K, V] : Left)
    Keys.insert(K);
  for (auto &[K, V] : Right)
    Keys.insert(K);
  for (auto &K : Keys) {
    auto L = Left.find(K), R = Right.find(K);
    if (L != Left.end() && R != Right.end() && L->second == R->second)
      continue;
    if (K == "quantization" || K.starts_with("adapter/") ||
        K.starts_with("gguf/adapter.") || K == "gguf/general.file_type" ||
        K == "gguf/general.quantization_version")
      Reasons.push_back("different " + K + " declaration");
    Changes.push_back(
        {{"component", K},
         {"evidence", K.starts_with("metadata-file/") ? "serialized metadata"
                      : K.starts_with("graph-topology/")
                          ? "exported operators and ports"
                          : "component declaration or content"},

         {"candidate_digest",
          L == Left.end() ? Json(nullptr) : Json(L->second)},
         {"baseline_digest",
          R == Right.end() ? Json(nullptr) : Json(R->second)}});
    if (L != Left.end() && R != Right.end() &&
        (K == "tokenizer/vocab.json" || K == "tokenizer/tokenizer.json"))
      Changes.back()["tokenizer_detail"] = tokenizerDifference(
          C, B, K.substr(std::string_view("tokenizer/").size()));
  }
  const std::string CandidateSnapshot = C.snapshotID(),
                    BaseSnapshot = B.snapshotID();
  const auto *Map = O.Mapping ? &*O.Mapping : nullptr;
  std::map<std::string, std::string> Claimed;
  if (Map) {
    if (Map->CandidateSnapshot != CandidateSnapshot ||
        Map->BaselineSnapshot != BaseSnapshot)
      throw Error("tensor mapping snapshot changed");
    for (const auto &[Name, Other] : Map->Names) {
      if (!C.Tensors.contains(Name) || !B.Tensors.contains(Other))
        throw Error("tensor mapping names an unknown tensor");
      if (!Claimed.emplace(Other, Name).second)
        throw Error("tensor mapping is not one-to-one");
    }
  }
  Json Differences = Json::array();
  uint64_t Done = 0, Skipped = 0, Index = 0, CandidateBytes = 0, BaseBytes = 0;
  uint64_t CandidateElements = 0, BaseElements = 0, ComparedElements = 0,
           OpaqueCandidate = 0, OpaqueBase = 0;
  std::string StopReason;
  struct LayerSummary {
    uint64_t Pairs = 0, Outside = 0, Nonfinite = 0, Tensors = 0;
  };
  std::map<unsigned, LayerSummary> LayerStats;
  std::set<std::string> Used;
  auto record = [&](Json Row, const std::string &Status) {
    Row["status"] = Status;
    auto &N = Counts[Status];
    N = N.is_null() ? 1 : N.get<uint64_t>() + 1;
    if ((Status == "added" || Status == "removed" ||
         Status == "changed-bytes" || Status == "sample-different" ||
         Status == "storage-layout-changed") &&
        Differences.size() < O.Limit)
      Differences.push_back(Row["tensor"]);
    if (Index++ >= O.Offset && Rows.size() < O.Limit)
      Rows.push_back(std::move(Row));
  };
  for (const auto &[Name, T] : B.Tensors) {
    BaseElements = sum(BaseElements, T.Elements);
    if (T.Bytes)
      BaseBytes = sum(BaseBytes, *T.Bytes);
    else
      ++OpaqueBase;
  }
  for (const auto &[Name, A] : C.Tensors) {
    if (StopReason.empty()) {
      try {
        C.B.tick();
      } catch (const Stop &E) {
        StopReason = E.what();
      }
    }
    CandidateElements = sum(CandidateElements, A.Elements);
    if (A.Bytes)
      CandidateBytes = sum(CandidateBytes, *A.Bytes);
    else
      ++OpaqueCandidate;
    std::string Other = Name;
    bool Mapped = Map && Map->Names.contains(Name);
    if (Mapped)
      Other = Map->Names.at(Name);
    Json Row = {{"tensor", Name}, {"candidate_artifact", A.FileId}};
    auto It = B.Tensors.find(Other);
    if (It == B.Tensors.end() || (!Mapped && Claimed.contains(Other))) {
      Row["bytes_processed_per_side"] = 0;
      Row["candidate_bytes_skipped"] = A.Bytes ? Json(*A.Bytes) : Json(nullptr);
      Row["candidate_elements_skipped"] = A.Elements;
      record(std::move(Row), "added");
      continue;
    }
    Used.insert(Other);
    const auto &Z = It->second;
    Row["baseline_tensor"] = Other;
    Row["baseline_artifact"] = Z.FileId;
    Row["mapping"] = Mapped ? "analyst" : "exact-name";
    std::string Status;
    uint64_t Processed = 0, ElementsRead = 0;
    if (A.Shape != Z.Shape || A.Type != Z.Type || A.Bytes != Z.Bytes) {
      Status = "storage-layout-changed";
      Row["candidate_shape"] = A.Shape;
      Row["baseline_shape"] = Z.Shape;
      Row["candidate_type"] = A.Type;
      Row["baseline_type"] = Z.Type;
    } else if (!Reasons.empty() || !A.Bytes)
      Status = "incomparable";
    else {
      auto Length = *A.Bytes;
      if (O.SampleBytes) {
        auto Unit = storageUnit(A);
        Length = std::min({Length, O.SampleBytes, C.B.CompareLeft / 2});
        Length -= Length % Unit.Bytes;
      }
      if (!StopReason.empty())
        Status = "skipped-interrupted";
      else if (Length > C.B.CompareLeft / 2 || (!Length && *A.Bytes))
        Status = "skipped-budget";
      else {
        auto D =
            compareRanges(C, A, B, Z, Formats.contains("gguf/big"), Length);
        StopReason = std::move(D.StopReason);
        Processed = D.Read;
        const bool Full = Processed == *A.Bytes;
        bool Equal = D.Candidate == D.Base;
        Status = !StopReason.empty() ? "interrupted"
                 : Full ? (Equal ? "byte-identical" : "changed-bytes")
                        : (Equal ? "sample-equal" : "sample-different");
        Row[Full ? "candidate_sha256" : "candidate_prefix_sha256"] =
            D.Candidate;
        Row[Full ? "baseline_sha256" : "baseline_prefix_sha256"] = D.Base;
        if (!D.Numeric.is_null()) {
          if (auto Layer = tensorLayer(Name)) {
            auto &Stats = LayerStats[*Layer];
            ++Stats.Tensors;
            Stats.Pairs += D.Numeric.at("finite_pairs").get<uint64_t>();
            Stats.Outside +=
                D.Numeric.at("outside_tolerance_pairs").get<uint64_t>();
            Stats.Nonfinite += D.Numeric.at("nonfinite_pairs").get<uint64_t>();
          }
          Row["numeric"] = std::move(D.Numeric);
        }
        Done = sum(Done, Processed);
        auto Unit = storageUnit(A);
        ElementsRead =
            Full ? A.Elements : product(Processed / Unit.Bytes, Unit.Elements);
        ComparedElements = sum(ComparedElements, ElementsRead);
      }
    }
    Row["elements_processed_per_side"] = ElementsRead;
    Row["candidate_elements_skipped"] = A.Elements - ElementsRead;
    Row["baseline_elements_skipped"] = Z.Elements - ElementsRead;
    Row["bytes_processed_per_side"] = Processed;
    Row["candidate_offset"] = A.HasOffset ? Json(A.Offset) : Json(nullptr);
    Row["baseline_offset"] = Z.HasOffset ? Json(Z.Offset) : Json(nullptr);
    Row["candidate_bytes_skipped"] =
        A.Bytes ? Json(*A.Bytes - Processed) : Json(nullptr);
    Row["baseline_bytes_skipped"] =
        Z.Bytes ? Json(*Z.Bytes - Processed) : Json(nullptr);
    if (A.Bytes)
      Skipped = sum(Skipped, *A.Bytes - Processed);
    record(std::move(Row), Status);
  }
  for (const auto &[Name, T] : B.Tensors)
    if (!Used.contains(Name))
      record(
          {{"tensor", Name},
           {"baseline_artifact", T.FileId},
           {"bytes_processed_per_side", 0},
           {"baseline_bytes_skipped", T.Bytes ? Json(*T.Bytes) : Json(nullptr)},
           {"baseline_elements_skipped", T.Elements}},
          "removed");
  Json Layers = Json::array();
  uint64_t LayerIndex = 0;
  for (auto &[Layer, S] : LayerStats)
    if (LayerIndex++ >= O.Offset && Layers.size() < O.Limit)
      Layers.push_back({{"layer", Layer},
                        {"tensors", S.Tensors},
                        {"finite_pairs", S.Pairs},
                        {"nonfinite_pairs", S.Nonfinite},
                        {"outside_tolerance_pairs", S.Outside}});
  const Json Names = Map ? Json(Map->Names) : Json::object();
  auto ComparisonID = digest(
      Json::array({CandidateSnapshot, BaseSnapshot, Names, O.CompareBytes,
                   O.SampleBytes, O.AbsoluteTolerance, O.RelativeTolerance})
          .dump());
  return {{"method", "one-to-one tensor mapping, validated shape and encoding, "
                     "SHA-256 of the reported range"},
          {"candidate_snapshot", CandidateSnapshot},
          {"baseline_snapshot", BaseSnapshot},
          {"tensor_mapping", Names},
          {"comparison_id", ComparisonID},
          {"numeric_layers", std::move(Layers)},
          {"numeric_layer_count", LayerStats.size()},
          {"absolute_tolerance", O.AbsoluteTolerance},
          {"relative_tolerance", O.RelativeTolerance},
          {"sample_bytes_per_tensor", O.SampleBytes},
          {"interrupted", !StopReason.empty()},
          {"interruption_reason", StopReason},
          {"candidate_known_bytes_skipped", CandidateBytes - Done},
          {"baseline_known_bytes_skipped", BaseBytes - Done},
          {"candidate_opaque_tensors", OpaqueCandidate},
          {"baseline_opaque_tensors", OpaqueBase},
          {"candidate_logical_elements", CandidateElements},
          {"baseline_logical_elements", BaseElements},
          {"compared_storage_elements", ComparedElements},
          {"candidate_elements_skipped", CandidateElements - ComparedElements},
          {"baseline_elements_skipped", BaseElements - ComparedElements},
          {"baseline_id", B.Id},
          {"candidate_id", C.Id},
          {"compatibility_issues", Reasons},
          {"component_changes", Changes},
          {"counts", Counts},
          {"first_changed_tensor_names", Differences},
          {"tensor_rows", Rows},
          {"tensor_offset", O.Offset},
          {"total_rows", Index},
          {"bytes_compared_per_side", Done},
          {"paired_candidate_bytes_skipped", Skipped},
          {"input_read_budget", O.CompareBytes},
          {"comparison_complete", StopReason.empty() && Reasons.empty() &&
                                      !Skipped &&
                                      !Counts.contains("incomparable") &&
                                      !Counts.contains("skipped-budget")},
          {"numeric_scope",
           "matched float32/float64 raw storage; absolute plus symmetric "
           "relative tolerance; nonfinite pairs counted separately"}};
}
} // namespace neverd::model
