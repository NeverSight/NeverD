//===- ScanReportTestSupport.h - Read unified scan observations -*- C++ -*-===//

#ifndef NEVERD_UNITTESTS_SAFETY_SCANREPORTTESTSUPPORT_H
#define NEVERD_UNITTESTS_SAFETY_SCANREPORTTESTSUPPORT_H

#include "gtest/gtest.h"

#include "llvm/Support/JSON.h"

#include <vector>

namespace neverd::test {

inline const llvm::json::Object *scanAnalyzer(const llvm::json::Object &Report,
                                              llvm::StringRef Name) {
  const auto *Analyzers = Report.getObject("analyzers");
  return Analyzers ? Analyzers->getObject(Name) : nullptr;
}

inline std::vector<const llvm::json::Object *>
scanFindings(const llvm::json::Object &Report) {
  std::vector<const llvm::json::Object *> Findings;
  const auto *Groups = Report.getArray("findings");
  if (!Groups) {
    ADD_FAILURE() << "missing scan findings";
    return Findings;
  }
  for (const auto &Value : *Groups) {
    const auto *Group = Value.getAsObject();
    const auto *Observations =
        Group ? Group->getArray("observations") : nullptr;
    if (!Observations || Observations->empty()) {
      ADD_FAILURE() << "missing scan observations";
      continue;
    }
    for (const auto &Entry : *Observations) {
      const auto *Observation = Entry.getAsObject();
      const auto *Finding =
          Observation ? Observation->getObject("finding") : nullptr;
      if (!Finding) {
        ADD_FAILURE() << "missing observation finding";
        continue;
      }
      Findings.push_back(Finding);
    }
  }
  return Findings;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_SAFETY_SCANREPORTTESTSUPPORT_H
