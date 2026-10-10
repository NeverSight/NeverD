#pragma once

#include "neverd/web/Source.h"

namespace neverd::web {
// An explicit sequential export profile; these models never enter the
// interactive session caches or its semantic consumers.
inline constexpr std::string_view JavaScriptRecoveryProfile =
    "hermes-602befee-recovery-js-v2";
inline constexpr uint64_t MaxRecoverySourceBytes = 32ULL * 1024 * 1024;
inline constexpr uint64_t MaxRecoveryNodes = 2000000;
inline constexpr uint64_t MaxRecoveryLexemes = 2000000;
inline constexpr uint64_t MaxRecoveryStringUnits = 16ULL * 1024 * 1024;
SourceAnalysis inspectRecoveryJavaScript(std::string_view ArtifactID,
                                         std::string_view Bytes,
                                         std::string_view SourceType);
} // namespace neverd::web
