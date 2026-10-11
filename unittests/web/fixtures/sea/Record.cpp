//===- Record.cpp - Node SEA compiler fixture recorder -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Development-only fixture author/recorder; never invoked by the product or
/// tests. Writes trusted inert source/config data and records compiler output.
///
//===----------------------------------------------------------------------===//

#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
std::string read(const fs::path &Path) {
  std::ifstream F(Path, std::ios::binary | std::ios::ate);
  if (!F || F.tellg() < 0 || F.tellg() > 256 * 1024 * 1024)
    throw std::runtime_error("fixture input");
  std::string B(size_t(F.tellg()), '\0');
  F.seekg(0);
  F.read(B.data(), B.size());
  if (!F.good())
    throw std::runtime_error("fixture read");
  return B;
}
void write(const fs::path &Path, std::string_view B) {
  std::ofstream F(Path, std::ios::binary);
  F.write(B.data(), B.size());
  if (!F.good())
    throw std::runtime_error("fixture write");
}
std::string hash(std::string_view B) {
  llvm::SHA256 H;
  H.update(llvm::StringRef(B));
  const auto D = H.final();
  std::string Out;
  for (const auto C : D) {
    Out += "0123456789abcdef"[C >> 4];
    Out += "0123456789abcdef"[C & 15];
  }
  return Out;
}
struct Case {
  const char *Name, *Source;
  unsigned Flags;
};
constexpr Case Cases[]{
    {"plain", "const value = 7;\nmodule.exports = value;\n", 1},
    {"minified", "function f(n){return n+1}module.exports=f(6);", 1},
    {"assets", "module.exports = require('node:sea').getAsset('data');\n", 9},
    {"cache", "module.exports = { answer: 42 };\n", 5},
    {"snapshot",
     "require('node:v8').startupSnapshot.setDeserializeMainFunction(() => "
     "{});\n",
     3}};
} // namespace
int main(int Argc, char **Argv) {
  try {
    if (Argc != 3 && Argc != 4)
      throw std::runtime_error("usage: record write ROOT | record ROOT OUTPUT");
    const std::string Mode = Argv[1];
    const fs::path Root = Argv[2];
    if (Mode == "record-images" && Argc == 4) {
      const auto Blob = read(Root.parent_path() / "cases/assets.blob");
      llvm::json::Array Images;
      for (const auto *Name : {"elf-x64", "elf-arm64", "macho-x64",
                               "macho-arm64", "pe-x64", "pe-arm64"}) {
        const auto B = read(Root / Name);
        const auto At = B.find(Blob);
        if (At == std::string::npos ||
            B.find(Blob, At + 1) != std::string::npos)
          throw std::runtime_error("expected unique original compiler blob");
        Images.emplace_back(llvm::json::Object{
            {"name", Name},
            {"profile", std::string("node-sea-22.15.0-") + Name + "-v1"},
            {"size", std::to_string(B.size())},
            {"sha256", hash(B)},
            {"resource_offset", std::to_string(At)},
            {"resource_size", std::to_string(Blob.size())},
            {"resource_sha256", hash(Blob)}});
      }
      std::string JSON;
      llvm::raw_string_ostream(JSON)
          << llvm::json::Value(llvm::json::Object{
                 {"node_version", "22.15.0"},
                 {"postject_version", "1.0.0-alpha.6"},
                 {"postject_archive_sha256", "d1447b53e87d49ddaf7fb3350c870afaf"
                                             "a72760eca47f6d5cce4cefd537e7d92"},
                 {"commander_version", "9.5.0"},
                 {"commander_archive_sha256",
                  "b64948bf68db38c26002a1a4b63b8ecb9a80e497e308fdaccbb179cf0514"
                  "2f52"},
                 {"executes_images", false},
                 {"images", std::move(Images)}})
          << '\n';
      write(Argv[3], JSON);
      return 0;
    }
    if (Mode == "write" && Argc == 3) {
      fs::create_directories(Root);
      write(Root / "asset.bin", std::string_view("\0\1\2\xff", 4));
      write(
          Root / "map.json",
          R"({"version":3,"sources":["input.js"],"names":[],"mappings":"AAAA","sourcesContent":["module.exports = 7;"]})");
      for (const auto &C : Cases) {
        write(Root / (std::string(C.Name) + ".js"), C.Source);
        llvm::json::Object Config{{"main", std::string(C.Name) + ".js"},
                                  {"output", std::string(C.Name) + ".blob"},
                                  {"disableExperimentalSEAWarning", true},
                                  {"useSnapshot", bool(C.Flags & 2)},
                                  {"useCodeCache", bool(C.Flags & 4)}};
        if (C.Flags & 8)
          Config["assets"] = llvm::json::Object{{"data", "asset.bin"},
                                                {"map.json", "map.json"}};
        std::string JSON;
        llvm::raw_string_ostream(JSON) << llvm::json::Value(std::move(Config));
        write(Root / (std::string(C.Name) + ".json"), JSON);
      }
      return 0;
    }
    if (Mode != "record" || Argc != 4)
      throw std::runtime_error("invalid mode");
    const fs::path Output = Argv[3];
    fs::create_directories(Output);
    llvm::json::Array Entries;
    for (const auto &C : Cases) {
      const auto Bytes = read(Root / (std::string(C.Name) + ".blob"));
      llvm::json::Object Entry{{"name", C.Name},
                               {"flags", C.Flags},
                               {"sha256", hash(Bytes)},
                               {"size", std::to_string(Bytes.size())},
                               {"source", C.Flags & 2
                                              ? llvm::json::Value(nullptr)
                                              : llvm::json::Value(C.Source)},
                               {"redistributed", !(C.Flags & 2)}};
      if (C.Flags & 8) {
        Entry["assets"] = llvm::json::Array{hash(read(Root / "asset.bin")),
                                            hash(read(Root / "map.json"))};
      }
      if (!(C.Flags & 2))
        write(Output / (std::string(C.Name) + ".blob"), Bytes);
      Entries.emplace_back(std::move(Entry));
    }
    std::string Manifest;
    llvm::raw_string_ostream(Manifest)
        << llvm::json::Value(llvm::json::Object{
               {"producer", "Node.js"},
               {"version", "22.15.0"},
               {"host", "darwin-arm64"},
               {"random_seed", 42},
               {"compiler_archive_sha256", "92eb58f54d172ed9dee320b8450f1390db6"
                                           "29d4262c936d5c074b25a110fed02"},
               {"compiler_cli",
                "node --random-seed=42 --experimental-sea-config CASE.json"},
               {"cases", std::move(Entries)}})
        << '\n';
    write(Output / "manifest.json", Manifest);
  } catch (const std::exception &E) {
    llvm::errs() << E.what() << '\n';
    return 1;
  }
  return 0;
}
