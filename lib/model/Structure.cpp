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
