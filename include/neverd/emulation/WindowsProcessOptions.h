//===- WindowsProcessOptions.h - Explicit Windows inputs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWSPROCESSOPTIONS_H
#define NEVERD_EMULATION_WINDOWSPROCESSOPTIONS_H
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace windows_process_limits {
#define NEVERD_WINDOWS_OPTION_LIMIT(Name, Value)                               \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/WindowsProcessOptions.def"
#undef NEVERD_WINDOWS_OPTION_LIMIT
} // namespace windows_process_limits
struct WindowsModuleInput {
  /// Exact guest basename, compared without ASCII case. No guest search path.
  std::string Name;
  /// Explicit host input file, never loaded as host executable code.
  std::filesystem::path Path;
};
struct WindowsProcessOptions {
  /// Only reachable startup dependencies are read. API providers cannot be
  /// overridden. DLL initialization and dynamic loading are separate contracts.
  std::vector<WindowsModuleInput> Modules;
};
} // namespace neverd::emulation
#endif
