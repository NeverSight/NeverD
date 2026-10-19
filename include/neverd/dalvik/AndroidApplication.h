//===- AndroidApplication.h - Explicit framework process state --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#pragma once

#include <optional>
#include <string>

namespace neverd::mobile::dalvik {
// An API 28 snapshot, not execution of ActivityThread.bindApplication. The
// application phase contains the platform Application attached to a ContextImpl
// and a single PathClassLoader for supplied DEX declarations, with boot parent.
// Custom Application subclasses, split loaders and constructors need separate
// evidence/execution; this input does not infer them from native libraries.
struct AndroidApplicationSnapshot {
  enum class Phase {
    Unattached,
    Attached,
    Application
  } phase = Phase::Unattached;
  // Unknown when absent. Only meaningful in the application phase.
  std::optional<std::string> package_name;
  // Initialized, existing app-private files directory. No mkdir/chmod is
  // inferred. Native workloads also require the same directory in LinuxFiles.
  std::optional<std::string> files_directory;
  // Independent of application creation: binding may install instrumentation
  // first, including a custom subclass. Absence is unknown, not null/default.
  enum class Instrumentation { Uninitialized, Platform };
  std::optional<Instrumentation> instrumentation;
};
} // namespace neverd::mobile::dalvik
