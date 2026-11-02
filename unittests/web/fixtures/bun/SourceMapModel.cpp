//===- SourceMapModel.cpp - Private development source-map export -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit local source-map disclosure using the shared decoding owner.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"

#include "neverd/web/SourceMap.h"

#include <iostream>

namespace {
using namespace neverd::web;

struct Budget {
  static constexpr uint64_t Limit = 256ULL * 1024 * 1024;
  uint64_t Reserved = 4096;
  void add(uint64_t Bytes) {
    if (Bytes > Limit - Reserved)
      throw Error("source_map_model_export_budget_exceeded");
    Reserved += Bytes;
  }
  void row() { add(512); }
  llvm::json::Value text(const std::optional<std::string> &Value) {
    if (!Value)
      return nullptr;
    add(Value->size() * 8 + 64);
    return *Value;
  }
};

llvm::json::Value index(std::optional<uint64_t> Value) {
  return Value ? llvm::json::Value(*Value) : llvm::json::Value(nullptr);
}

llvm::json::Object position(TextPosition P) {
  return llvm::json::Object{{"line", P.Line}, {"utf16_column", P.UTF16Column}};
}

llvm::json::Object model(const SourceMap &Map, Budget &B) {
  llvm::json::Array Sources, Names, Segments;
  for (const auto &S : Map.Sources) {
    B.row();
    if (S.Storage)
      throw Error("source_map_model_storage_not_supported");
    // A source root is private metadata, not a path to open. Account for
    // repeated roots before constructing their individual JSON values.
    llvm::json::Value Root = nullptr;
    if (S.Root) {
      B.add(S.Root->size() * 8 + 64);
      Root = *S.Root;
    }
    Sources.emplace_back(llvm::json::Object{{"name", B.text(S.Name)},
                                            {"root", std::move(Root)},
                                            {"contents", B.text(S.Contents)},
                                            {"ignored", S.Ignored}});
  }
  for (const auto &Name : Map.Names) {
    B.add(Name.size() * 8 + 64);
    Names.emplace_back(Name);
  }
  for (const auto &S : Map.Segments) {
    B.row();
    Segments.emplace_back(
        llvm::json::Object{{"generated", position(S.Generated)},
                           {"source", index(S.SourceIndex)},
                           {"original", position(S.Original)},
                           {"name", index(S.NameIndex)}});
  }
  return llvm::json::Object{{"file", B.text(Map.File)},
                            {"indexed", Map.Indexed},
                            {"mapped_anchors_only", Map.MappedAnchorsOnly},
                            {"sources", std::move(Sources)},
                            {"names", std::move(Names)},
                            {"segments", std::move(Segments)}};
}
} // namespace

int main(int Argc, char **Argv) {
  using namespace neverd::web;
  if (Argc != 3) {
    std::cerr << "usage: neverd-web-source-map-model <input> "
                 "<new-output-directory>\n";
    return 2;
  }
  try {
    neverd::web::Limits Bound;
    Bound.MaxInputBytes = Bound.MaxMemberBytes = MaxSourceMapBytes;
    Bound.MaxEntries = 1;
    const auto Input = capture(Argv[1], Bound);
    if (Input.Artifacts.size() != 1 || Input.Artifacts[0].Directory)
      throw Error("source_map_model_requires_single_file");
    const auto &A = Input.Artifacts[0];
    const auto Text = A.Content.read(0, A.Content.size(), MaxSourceMapBytes);
    if (sha256(Text) != A.BlobHash)
      throw Error("source_map_model_hash_mismatch");
    const auto Map = decodeSourceMap(A.ID, Text);
    Budget B;
    const auto Serialized = json(model(Map, B));
    if (Serialized.size() > B.Reserved)
      throw Error("source_map_model_export_accounting_exceeded");
    ExportDirectory Output(Argv[2]);
    Output.write("original.map", Text);
    Output.write("model.json", Serialized);
    const auto Manifest = json(llvm::json::Object{
        {"schema_version", 1},
        {"profile", "development-source-map-model-v1"},
        {"decoder_profile", Map.Profile},
        {"status", "exported"},
        {"source_sha256", A.BlobHash},
        {"source_bytes", std::to_string(Text.size())},
        {"model_sha256", sha256(Serialized)},
        {"source_count", Map.Sources.size()},
        {"name_count", Map.Names.size()},
        {"segment_count", Map.Segments.size()},
        {"export_reserved_bytes", B.Reserved},
        {"reads_source_paths", false},
        {"executes_input", false},
        {"source_provenance", "untrusted_mapping_metadata"}});
    Output.complete(Manifest);
    std::cout << Manifest << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
