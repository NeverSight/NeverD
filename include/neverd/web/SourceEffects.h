#pragma once

#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceValues.h"

namespace neverd::web {

inline constexpr std::string_view JavaScriptEffectProfile =
    "javascript-conservative-effects-v1";
inline constexpr uint64_t MaxJavaScriptEffectSteps = 2000000;

enum SourceEffect : uint32_t {
  ReadBinding = 1 << 0,
  WriteBinding = 1 << 1,
  ReadProperty = 1 << 2,
  WriteProperty = 1 << 3,
  Call = 1 << 4,
  MayThrow = 1 << 5,
  Allocate = 1 << 6,
  Suspend = 1 << 7,
  Control = 1 << 8,
  UnknownEffect = 1 << 9,
  Debugger = 1 << 10,
  ModuleLink = 1 << 11,
  MayDiverge = 1 << 12,
};

struct SourceNodeEffects {
  // Conservative may-effects, not execution counts or a reachability proof.
  uint32_t Immediate = 0;
  uint32_t Deferred = 0;
  // Syntactic declarations survive branch pruning (e.g. if(false){var x}).
  // Neither zero Immediate nor a constant value authorizes source deletion.
  bool ContainsDeclaration = false;
};

struct SourceEffectAnalysis {
  std::string ID;
  std::string SourceID;
  std::string Status;
  std::vector<SourceNodeEffects> Nodes;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t Steps = 0;
};

SourceEffectAnalysis analyzeSourceEffects(const SourceAnalysis &Source,
                                          const SourceBindingAnalysis &Bindings,
                                          const SourceValueAnalysis &Values);
std::vector<std::string> sourceEffectNames(uint32_t Effects);

} // namespace neverd::web
