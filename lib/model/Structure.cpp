//===- Structure.cpp - Model projection layout evidence ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify projection layouts using matrices in the same numbered layer.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <array>

namespace neverd::model {
namespace {
struct Profile {
  const char *Id;
  std::array<const char *, 4> Suffixes;
};
// These profiles describe stored projections. Names and matrix rank support
// the grouping, but cannot establish operator ordering or attention semantics.
constexpr Profile Profiles[] = {
    {"gguf.separate-qkv",
     {".attn_q.weight", ".attn_k.weight", ".attn_v.weight",
      ".attn_output.weight"}},
    {"layers.separate-qkv",
     {".self_attn.q_proj.weight", ".self_attn.k_proj.weight",
      ".self_attn.v_proj.weight", ".self_attn.o_proj.weight"}},
    {"layers.linear-attention",
     {".linear_attn.in_proj_qkv.weight", ".linear_attn.in_proj_a.weight",
      ".linear_attn.in_proj_b.weight", ".linear_attn.out_proj.weight"}}};
} // namespace

std::optional<unsigned> tensorLayer(llvm::StringRef Name) {
  auto Tail = Name.starts_with("blk.") ? Name.drop_front(4)
                                       : Name.split(".layers.").second;
  if (Tail.empty() && Name.starts_with("layers."))
    Tail = Name.drop_front(7);
  auto [Index, Suffix] = Tail.split('.');
  unsigned Layer;
  if (Suffix.empty() || Index.getAsInteger(10, Layer))
    return std::nullopt;
  return Layer;
}

void inspectStructure(Package &P) {
  std::optional<uint64_t> DeclaredRank;
  for (const auto &A : P.Artifacts)
    if (A.value("id", std::string()) == "adapter_config.json" &&
        A.at("declarations").contains("rank"))
      DeclaredRank = natural(A.at("declarations").at("rank"));
  uint64_t PairIndex = 0;
  for (const auto &[Name, T] : P.Tensors) {
    llvm::StringRef Stem(Name);
    bool Left = Stem.consume_back(".lora_A.weight");
    if (!Left && !Stem.consume_back(".lora_B.weight"))
      continue;
    auto Peer = P.Tensors.find(
        (Stem + (Left ? ".lora_B.weight" : ".lora_A.weight")).str());
    if (!Left && Peer != P.Tensors.end())
      continue;
    const bool HasBoth = Peer != P.Tensors.end();
    const bool Paired = HasBoth && T.Shape.size() == 2 &&
                        Peer->second.Shape.size() == 2 &&
                        T.Shape.front() == Peer->second.Shape.back();
    Json Row = {{"target", Stem.str()},
                {"first_tensor", Name},
                {"first_shape", T.Shape},
                {"artifact", T.FileId},
                {"status", Paired    ? "paired-matrices"
                           : HasBoth ? "incompatible-shapes"
                                     : "missing-partner"}};
    if (HasBoth) {
      Row["partner_tensor"] = Peer->first;
      Row["partner_shape"] = Peer->second.Shape;
      Row["partner_artifact"] = Peer->second.FileId;
    }
    if (Paired) {
      Row["rank"] = T.Shape.front();
      if (DeclaredRank && *DeclaredRank != T.Shape.front())
        Row["status"] = "declared-rank-conflict";
    }
    if (DeclaredRank)
      Row["declared_rank"] = *DeclaredRank;
    if (PairIndex++ >= P.B.Opts.Offset &&
        P.AdapterPairs.size() < P.B.Opts.Limit)
      P.AdapterPairs.push_back(std::move(Row));
  }
  P.AdapterPairCount = PairIndex;
  for (const auto &Fact : P.Declarations) {
    if (Fact.at("kind") != "layer_count")
      continue;
    uint64_t Count = natural(Fact.at("value"));
    for (const auto &[Layer, Tensors] : P.Layers.items()) {
      unsigned Index;
      if (!llvm::StringRef(Layer).getAsInteger(10, Index) && Index >= Count) {
        P.gap(Fact.at("artifact"),
              "named tensor layer exceeds declared layer count",
              {{"field", Fact.at("field")},
               {"declared_layers", Count},
               {"layer", Index}});
        break;
      }
    }
  }
  P.ProjectionLayouts = projectionLayouts(P);
}

Json projectionLayouts(const Package &P) {
  Json Results = Json::array();
  for (const auto &Rule : Profiles) {
    uint64_t Count = 0;
    Json Example = Json::array();
    for (const auto &Entry : P.Tensors) {
      P.B.tick();
      llvm::StringRef Stem(Entry.first);
      if (!Stem.consume_back(Rule.Suffixes.front()))
        continue;
      auto [Group, Number] = Stem.rsplit('.');
      unsigned Layer;
      if ((Group != "blk" && Group != "layers" &&
           !Group.ends_with(".layers")) ||
          Number.getAsInteger(10, Layer))
        continue;
      bool Match = true;
      Json Matrices = Json::array();
      for (auto Suffix : Rule.Suffixes) {
        auto It = P.Tensors.find((Stem + Suffix).str());
        if (It == P.Tensors.end() || It->second.Shape.size() != 2 ||
            !It->second.Bytes) {
          Match = false;
          break;
        }
        if (!Count)
          Matrices.push_back({{"tensor", It->first},
                              {"artifact", It->second.FileId},
                              {"shape", It->second.Shape},
                              {"offset", It->second.Offset}});
      }
      if (Match) {
        if (!Count)
          Example = std::move(Matrices);
        ++Count;
      }
    }
    if (Count)
      Results.push_back(
          {{"id", Rule.Id},
           {"profile_revision", 1},
           {"evidence_level", "inferred from tensor names and rank"},
           {"matched_layers", Count},
           {"example_matrices", std::move(Example)}});
  }
  return Results;
}
} // namespace neverd::model
