//===- SemanticIndex.cpp - Development source structure navigator ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "BunSemanticIndex.h"
#include "Internal.h"

#include <iostream>

int main(int Argc, char **Argv) {
  if (Argc != 3) {
    std::cerr << "usage: neverd-web-bun-semantic-index <input> "
                 "<new-output-directory>\n";
    return 2;
  }
  try {
    const auto Snapshot = neverd::web::capture(Argv[1], neverd::web::Limits{});
    if (Snapshot.Artifacts.size() != 1 || Snapshot.Artifacts.front().Directory)
      throw neverd::web::Error("bun_semantic_requires_single_file");
    const auto Extraction = neverd::web::extractBun(Snapshot.Artifacts.front());
    std::cout << neverd::web::exportBunSemanticIndex(Extraction, Argv[2])
              << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
