//===- SourceModel.cpp - Validated JavaScript source models ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated JavaScript source models.
///
//===----------------------------------------------------------------------===//

#include "SourceModel.h"

#include "neverd/web/Error.h"

namespace neverd::web {

uint64_t validateSourceModel(const SourceAnalysis &Source) {
  if (Source.ParseStatus != "parsed" || Source.Nodes.empty() ||
      Source.Nodes.size() > MaxJavaScriptNodes ||
      Source.Nodes[0].Kind != "Program" || !Source.Nodes[0].ParentID.empty() ||
      (Source.SourceType != "script" && Source.SourceType != "module" &&
       Source.SourceType != "commonjs"))
    throw Error("invalid_source_model");
  std::vector<unsigned> Parents(Source.Nodes.size()),
      Depths(Source.Nodes.size());
  uint64_t Edges = 0;
  for (uint32_t I = 0; I < Source.Nodes.size(); ++I) {
    const auto &N = Source.Nodes[I];
    if (N.Start > N.End || N.ID.empty())
      throw Error("invalid_source_model");
    for (const auto &C : N.Children) {
      ++Edges;
      if (C.Index <= I || C.Index >= Source.Nodes.size() ||
          ++Parents[C.Index] != 1)
        throw Error("invalid_source_model");
      const auto &Child = Source.Nodes[C.Index];
      if (Child.ParentID != N.ID || Child.Start < N.Start || Child.End > N.End)
        throw Error("invalid_source_model");
      Depths[C.Index] = Depths[I] + 1;
      if (Depths[C.Index] >= 256)
        throw Error("source_model_depth_exceeded");
    }
    if (I && Parents[I] != 1)
      throw Error("invalid_source_model");
  }
  return Edges;
}

} // namespace neverd::web
