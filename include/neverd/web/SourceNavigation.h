//===- SourceNavigation.h - Source containment and lexical links -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source containment and lexical links.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceBindings.h"

namespace neverd::web {
inline constexpr std::string_view JavaScriptNavigationProfile =
    "javascript-source-navigation-v1";
inline constexpr std::string_view SourceAnchorProfile =
    "source-storage-anchor-v1";
inline constexpr uint64_t MaxJavaScriptNavigationSteps = 2000000;

struct SourceFunction {
  std::string ID;
  uint32_t Node = NoSourceIndex, ParentFunction = NoSourceIndex;
  uint32_t Name = NoSourceIndex, Body = NoSourceIndex;
  uint32_t NameBinding = NoSourceIndex, InitializerBinding = NoSourceIndex;
  uint32_t Parameters = 0;
};
struct SourceCall {
  std::string ID, Kind;
  uint32_t Node = NoSourceIndex, EnclosingFunction = NoSourceIndex;
  uint32_t Callee = NoSourceIndex, Reference = NoSourceIndex;
  uint32_t Argument = NoSourceIndex;
  uint32_t SyntacticFunction = NoSourceIndex;
};
struct SourceNavigationReference {
  uint32_t Reference = NoSourceIndex, EnclosingFunction = NoSourceIndex;
};
struct SourceNavigation {
  std::string ID, SourceID, BindingID, BindingStatus, Status;
  std::vector<SourceFunction> Functions;
  std::vector<SourceCall> Calls;
  std::vector<SourceNavigationReference> References;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t Steps = 0;
};

/// A source-containment index, not a runtime call graph. Function initializer
/// links and lexical callee bindings do not prove which value is called.
SourceNavigation analyzeSourceNavigation(const SourceAnalysis &Source,
                                         const SourceBindingAnalysis &Bindings);
} // namespace neverd::web
