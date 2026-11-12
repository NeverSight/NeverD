//===- SourceRouteCases.h - Round trips pinned to a source route -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A round trip checks the lifted object unless the environment selects a
/// source route. A case whose defect was in the decompiled C runs on that
/// route every time: these select such cases from a suite's list.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_SEMANTIC_SOURCEROUTECASES_H
#define NEVERD_UNITTESTS_SEMANTIC_SOURCEROUTECASES_H

#include "SemanticRoundTripFixture.h"

#include <initializer_list>
#include <string>
#include <vector>

/// The cases of \p Cases whose name ends with one of \p Names, every case
/// when there is none, each checked by building and running its decompiled
/// C: LLVM C when \p LlvmRoute, HighC otherwise.
inline std::vector<RoundTripTC>
onSourceRoute(const std::vector<RoundTripTC> &Cases, bool LlvmRoute,
              std::initializer_list<const char *> Names = {}) {
  std::vector<RoundTripTC> Selected;
  for (const RoundTripTC &Case : Cases) {
    bool Named = Names.size() == 0;
    for (const char *Name : Names) {
      const std::string Suffix(Name);
      Named |= Case.Name.size() >= Suffix.size() &&
               Case.Name.compare(Case.Name.size() - Suffix.size(),
                                 Suffix.size(), Suffix) == 0;
    }
    if (!Named)
      continue;
    Selected.push_back(Case);
    Selected.back().SourceRoute = LlvmRoute;
  }
  return Selected;
}

/// \p Cases on the HighC route.
inline std::vector<RoundTripTC>
onHighC(const std::vector<RoundTripTC> &Cases,
        std::initializer_list<const char *> Names = {}) {
  return onSourceRoute(Cases, /*LlvmRoute=*/false, Names);
}

/// \p Cases on the LLVM C route.
inline std::vector<RoundTripTC>
onLLVMC(const std::vector<RoundTripTC> &Cases,
        std::initializer_list<const char *> Names = {}) {
  return onSourceRoute(Cases, /*LlvmRoute=*/true, Names);
}

#endif // NEVERD_UNITTESTS_SEMANTIC_SOURCEROUTECASES_H
