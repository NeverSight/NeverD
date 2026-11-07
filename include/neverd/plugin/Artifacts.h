//===- Artifacts.h - Provider artifact verification ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Host-owned paths, content identity and committed task receipts.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_ARTIFACTS_H
#define NEVERD_PLUGIN_ARTIFACTS_H

#include "neverd/plugin/Manifest.h"

namespace neverd::plugin {
bool verifyArtifacts(const std::filesystem::path &Directory, const Json &Result,
                     std::string &Error);
/// Write/flush/close a same-directory temporary before replacing the receipt.
/// A failure does not move the previous receipt aside.
bool writeReceipt(const std::filesystem::path &Directory, const Json &Record,
                  std::string &Error);
/// Atomic host JSON persistence, also used by installation catalog adapters.
bool writeJsonFile(const std::filesystem::path &Destination, const Json &Record,
                   std::string &Error);
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_ARTIFACTS_H
