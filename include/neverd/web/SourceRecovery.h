//===- SourceRecovery.h - Verified readable source recovery ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verified readable source recovery.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Source.h"

namespace neverd::web {
struct ReadableSource {
  std::string Status;
  std::string ParseStatus;
  std::string Text;
  uint64_t Nodes = 0;
  std::vector<SourceDiagnostic> Diagnostics;
};
/// Explicit local source disclosure consumer. Retains every input byte and
/// only inserts whitespace at parser-owned token boundaries. Reparse and
/// compare the full retained syntax model before publishing a readable file.
/// This does not restore deleted names, types, comments or original modules.
ReadableSource recoverReadableJavaScript(std::string_view ArtifactID,
                                         std::string_view Text,
                                         std::string_view SourceType);
} // namespace neverd::web
