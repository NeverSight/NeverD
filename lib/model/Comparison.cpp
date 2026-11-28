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
#include <cstring>

namespace neverd::model {
namespace {
struct Digests {
  std::string Candidate, Base;
  Json Numeric;
};
Digests compareRanges(Package &C, const Tensor &A, Package &B, const Tensor &Z,
                      bool Big) {
  SHA256Stream AH, BH;
  auto &AF = C.file(A.FileId);
  auto &BF = B.file(Z.FileId);
  AF.verify();
  BF.verify();
  std::array<char, limits::kChunk> Left, Right;
  const unsigned Width = (A.Type == "F32" || A.Type == "onnx:1")    ? 4
                         : (A.Type == "F64" || A.Type == "onnx:11") ? 8
                                                                    : 0;
  auto Value = [&](const char *At) -> double {
    if (Width == 4)
      return std::bit_cast<float>(Big ? llvm::support::endian::read32be(At)
                                      : llvm::support::endian::read32le(At));
    return std::bit_cast<double>(Big ? llvm::support::endian::read64be(At)
                                     : llvm::support::endian::read64le(At));
  };
  uint64_t Finite = 0, Unequal = 0, Nonfinite = 0, Overflow = 0;
  double MaxAbs = 0;
  for (uint64_t I = 0; I < *A.Bytes;) {
    C.B.tick();
    auto N = std::min<uint64_t>(Left.size(), *A.Bytes - I);
    AF.read(A.Offset + I, std::span(Left).first(N));
    BF.read(Z.Offset + I, std::span(Right).first(N));
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
        Unequal += (X != Y);
        auto Abs = std::abs(X - Y);
        if (std::isfinite(Abs))
          MaxAbs = std::max(MaxAbs, Abs);
        else
          ++Overflow;
      }
    C.WeightBytesRead += N;
    B.WeightBytesRead += N;
    C.B.CompareLeft -= 2 * N;
    I += N;
  }
  Json Numeric = nullptr;
  if (Width)
    Numeric = {{"finite_pairs", Finite},
               {"unequal_finite_pairs", Unequal},
               {"nonfinite_pairs", Nonfinite},
               {"absolute_difference_overflows", Overflow},
               {"maximum_finite_absolute_difference",
                Finite > Overflow ? Json(MaxAbs) : Json(nullptr)}};
  return {llvm::toHex(AH.final(), true), llvm::toHex(BH.final(), true),
          std::move(Numeric)};
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
    Changes.push_back({{"component", K},
                       {"candidate_digest",
                        L == Left.end() ? Json(nullptr) : Json(L->second)},
                       {"baseline_digest",
                        R == Right.end() ? Json(nullptr) : Json(R->second)}});
    if (L != Left.end() && R != Right.end() &&
        (K == "tokenizer/vocab.json" || K == "tokenizer/tokenizer.json"))
      Changes.back()["tokenizer_detail"] = tokenizerDifference(
          C, B, K.substr(std::string_view("tokenizer/").size()));
  }
  auto L = C.Tensors.begin(), R = B.Tensors.begin();
  Json Differences = Json::array();
  uint64_t Done = 0, Skipped = 0, Index = 0;
  while (L != C.Tensors.end() || R != B.Tensors.end()) {
    C.B.tick();
    Json Row;
    std::string Status;
    if (R == B.Tensors.end() || (L != C.Tensors.end() && L->first < R->first)) {
      Row["tensor"] = L->first;
      Status = "added";
      ++L;
    } else if (L == C.Tensors.end() || R->first < L->first) {
      Row["tensor"] = R->first;
      Status = "removed";
      ++R;
    } else {
      Row["tensor"] = L->first;
      auto &A = L++->second;
      auto &Z = R++->second;
      Row["candidate_artifact"] = A.FileId;
      Row["baseline_artifact"] = Z.FileId;
      if (A.Shape != Z.Shape || A.Type != Z.Type || A.Bytes != Z.Bytes) {
        Status = "storage-layout-changed";
        Row["candidate_shape"] = A.Shape;
        Row["baseline_shape"] = Z.Shape;
        Row["candidate_type"] = A.Type;
        Row["baseline_type"] = Z.Type;
      } else if (!Reasons.empty() || !A.Bytes)
        Status = "incomparable";
      else if (*A.Bytes > C.B.CompareLeft / 2)
        Status = "skipped-budget";
      else {
        auto Dig = compareRanges(C, A, B, Z, Formats.contains("gguf/big"));
        Status = Dig.Candidate == Dig.Base ? "byte-identical" : "changed-bytes";
        Row["candidate_sha256"] = Dig.Candidate;
        Row["baseline_sha256"] = Dig.Base;
        if (!Dig.Numeric.is_null())
          Row["numeric"] = std::move(Dig.Numeric);
        Done += *A.Bytes;
      }
      if (Status != "byte-identical" && Status != "changed-bytes" && A.Bytes)
        Skipped = sum(Skipped, *A.Bytes);
    }
    Row["status"] = Status;
    auto &N = Counts[Status];
    N = N.is_null() ? 1 : N.get<uint64_t>() + 1;
    if ((Status == "added" || Status == "removed" ||
         Status == "changed-bytes" || Status == "storage-layout-changed") &&
        Differences.size() < O.Limit)
      Differences.push_back(Row["tensor"]);
    if (Index++ >= O.Offset && Rows.size() < O.Limit)
      Rows.push_back(std::move(Row));
  }
  return {
      {"method", "exact tensor name, shape, storage encoding and SHA-256 of "
                 "complete ranges"},
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
      {"comparison_complete", Reasons.empty() && !Skipped &&
                                  !Counts.contains("incomparable") &&
                                  !Counts.contains("skipped-budget")},
      {"numeric_scope",
       "matched float32/float64 raw storage; finite pairs, zero tolerance"}};
}
} // namespace neverd::model
