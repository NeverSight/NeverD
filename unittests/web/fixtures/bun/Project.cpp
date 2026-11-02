//===- Project.cpp - Development Bun reconstruction exporter -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit local development driver; never compiles or executes an input.
///
//===----------------------------------------------------------------------===//

#include "BunProject.h"
#include "Internal.h"

#include <iostream>

int main(int Argc, char **Argv) {
  if (Argc != 3) {
    std::cerr
        << "usage: neverd-web-bun-project <input> <new-output-directory>\n";
    return 2;
  }
  try {
    const auto Input = neverd::web::capture(Argv[1], neverd::web::Limits{});
    if (Input.Artifacts.size() != 1 || Input.Artifacts[0].Directory)
      throw neverd::web::Error("bun_project_requires_single_file");
    const auto E = neverd::web::extractBun(Input.Artifacts[0]);
    std::cout << neverd::web::exportBunProject(E, Argv[2]) << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
