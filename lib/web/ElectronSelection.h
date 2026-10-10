#pragma once

#include "neverd/web/ElectronIPC.h"

namespace neverd::web {
// Shared admission for explicitly selected Electron consumers. Borrowed
// pointers live only during the synchronous analysis. The diagnostic names
// retain the first IPC consumer's public contract.
struct ElectronSelection {
  std::string Directory;
  std::vector<ElectronIPCInput> Sources;
  uint64_t Steps = 0;
};
/// Shared bounded selection options; legacy IPC diagnostic names are retained.
std::vector<std::string> electronSourceSelection(std::string_view Options);
ElectronSelection
selectElectronSources(const Snapshot &Namespace,
                      const ElectronManifest &Manifest,
                      std::span<const ElectronIPCInput> Input);
} // namespace neverd::web
