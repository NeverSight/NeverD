//===- WriteInputs.cpp - Controlled nexe producer inputs ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Development-only fixture preparation. Does not build or execute samples.
///
//===----------------------------------------------------------------------===//

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

int main(int Argc, char **Argv) {
  try {
    if (Argc != 3)
      throw std::runtime_error("usage: write-inputs MANIFEST NEW-DIRECTORY");
    std::ifstream F(Argv[1], std::ios::binary | std::ios::ate);
    if (!F || F.tellg() < 0 || F.tellg() > 65536)
      throw std::runtime_error("invalid fixture manifest");
    const auto Size = size_t(F.tellg());
    F.seekg(0);
    std::string Bytes(Size, '\0');
    if (!F.read(Bytes.data(), Bytes.size()))
      throw std::runtime_error("manifest read failed");
    auto V = llvm::json::parse(Bytes);
    if (!V) {
      llvm::consumeError(V.takeError());
      throw std::runtime_error("invalid manifest JSON");
    }
    const auto *Object = V->getAsObject();
    const auto *Inputs = Object ? Object->getArray("inputs") : nullptr;
    if (!Inputs || Inputs->size() != 4)
      throw std::runtime_error("invalid fixture inputs");
    const std::filesystem::path Root(Argv[2]);
    if (!std::filesystem::create_directory(Root))
      throw std::runtime_error("output directory must be new");
    for (const auto *Name :
         {"main.js", "minified.js", "asset.txt", "package.json"}) {
      const llvm::json::Object *Entry = nullptr;
      for (const auto &I : *Inputs)
        if (I.getAsObject() && I.getAsObject()->getString("name") == Name)
          Entry = I.getAsObject();
      if (!Entry || !Entry->getString("bytes"))
        throw std::runtime_error("missing fixed fixture input");
      const auto Text = *Entry->getString("bytes");
      std::ofstream Out(Root / Name, std::ios::binary);
      if (!Out.write(Text.data(), Text.size()))
        throw std::runtime_error("fixture write failed");
    }
  } catch (const std::exception &E) {
    llvm::errs() << E.what() << '\n';
    return 1;
  }
  return 0;
}
