//===- Inventory.cpp - Development Bun graph readback ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit local inventory using the shared extractor. Never executes input.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"

#include "neverd/web/Bun.h"

#include <iostream>

int main(int Argc, char **Argv) {
  using namespace neverd::web;
  if (Argc != 3) {
    std::cerr
        << "usage: neverd-web-bun-inventory <input> <new-output-directory>\n";
    return 2;
  }
  try {
    const auto Input = capture(Argv[1], Limits{});
    if (Input.Artifacts.size() != 1 || Input.Artifacts[0].Directory)
      throw Error("bun_inventory_requires_single_file");
    const auto &A = Input.Artifacts[0];
    const auto E = extractBun(A);
    auto Region = [&](uint32_t Index) -> llvm::json::Value {
      if (Index == NoBunIndex)
        return nullptr;
      const auto &R = E.Regions.at(Index);
      return llvm::json::Object{{"bytes", std::to_string(R.Content.size())},
                                {"sha256", R.BlobHash},
                                {"container_offset", std::to_string(R.Offset)}};
    };
    uint64_t NameBytes = 0;
    llvm::json::Array Modules, RuntimeOptions;
    for (size_t I = 0; I < E.Modules.size(); ++I) {
      const auto &M = E.Modules[I];
      const auto &N = E.Regions.at(M.Name).Content;
      constexpr uint64_t NameBudget = 16ULL * 1024 * 1024;
      if (N.size() > NameBudget - NameBytes)
        throw Error("bun_inventory_name_budget_exceeded");
      NameBytes += N.size();
      const auto Name = N.read(0, N.size(), MaxBunNameBytes);
      if (!validUtf8(Name))
        throw Error("bun_inventory_name_not_utf8");
      llvm::json::Object Row{{"module_index", I},
                             {"virtual_name", Name},
                             {"encoding_id", M.Encoding},
                             {"loader_id", M.Loader},
                             {"format_id", M.Format},
                             {"side_id", M.Side},
                             {"storage", Region(M.Contents)},
                             {"source_map", Region(M.SourceMap)},
                             {"bytecode", Region(M.Bytecode)},
                             {"module_info", Region(M.ModuleInfo)},
                             {"bytecode_origin", Region(M.BytecodeOrigin)}};
      if (!M.SourceArtifactID.empty())
        Row["source_sha256"] =
            sha256(bunSourceBytes(E, M, MaxBunDecodedSourceBytes));
      Modules.emplace_back(std::move(Row));
    }
    if (E.RuntimeOptions)
      for (auto Byte : *E.RuntimeOptions)
        RuntimeOptions.emplace_back(uint64_t(Byte));
    const auto Manifest = json(llvm::json::Object{
        {"schema_version", 1},
        {"status", "admitted"},
        {"profile", "development-bun-inventory-v1"},
        {"layout_profile", E.Profile},
        {"input_sha256", A.BlobHash},
        {"platform", E.Platform},
        {"architecture", E.Architecture},
        {"module_count", E.Modules.size()},
        {"entry_point_index", E.EntryPoint},
        {"startup_count", E.StartupCount},
        {"flags", E.Flags},
        {"prelinked_module_count", E.PrelinkedModuleFiles.size()},
        {"runtime_options_bytes", std::move(RuntimeOptions)},
        {"compile_arguments", Region(E.CompileArguments)},
        {"modules", std::move(Modules)},
        {"executes_input", false}});
    ExportDirectory Output(Argv[2]);
    Output.complete(Manifest);
    std::cout << json(llvm::json::Object{{"status", "admitted"},
                                         {"input_sha256", A.BlobHash},
                                         {"manifest_sha256", sha256(Manifest)},
                                         {"module_count", E.Modules.size()}})
              << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
