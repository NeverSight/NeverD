//===- Config.h - Shared plugin task policy -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared admission defaults for serial provider tasks.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_CONFIG_H
#define NEVERD_PLUGIN_CONFIG_H

#include <cstddef>

namespace neverd::plugin {
inline constexpr size_t kMaxQueuedTasks = 8;
inline constexpr size_t kMaxTaskHistory = 64;

namespace limits {
inline constexpr int kMaxFrameBytes = 1 << 20;
inline constexpr int kMaxArtifactBytes = 64 << 20;
inline constexpr int kMaxImageImportBytes = 3 << 20;
inline constexpr int kMaxTransportBytes = 16 << 20;
inline constexpr int kMaxManifestBytes = 64 << 10;
inline constexpr int kMaxLogBytes = 64 << 10;
inline constexpr int kReadChunkBytes = 64 << 10;
inline constexpr int kMaxMessages = 10000;
inline constexpr int kMaxPlugins = 16;
inline constexpr int kMaxOutstandingQueries = 16;
inline constexpr int kMaxEventSubscriptions = 32;
inline constexpr int kMaxTurnFrames = 16;
inline constexpr int kShutdownWaitMs = 1000;
inline constexpr int kHandshakeTimeoutMs = 5000;
inline constexpr int kDefaultActionTimeoutMs = 60000;
inline constexpr int kMinActionTimeoutMs = 100;
inline constexpr int kMaxActionTimeoutMs = 3600000;
inline constexpr int kMaxArguments = 32;
inline constexpr int kMaxActions = 32;
inline constexpr int kMaxParameters = 16;
inline constexpr int kMaxChoices = 32;
inline constexpr int kDefaultParameterMaximum = 1000000;
inline constexpr int kMaxParameterCharacters = 4096;
inline constexpr int kMaxPathCharacters = 32768;
inline constexpr int kMaxLabelCharacters = 256;
inline constexpr int kMaxRequestIdCharacters = 128;
inline constexpr int kMaxProgressCharacters = 1024;
inline constexpr int kMaxDiagnosticCharacters = 8192;
inline constexpr int kMaxResultViews = 16;
inline constexpr int kMaxArtifacts = 16;
inline constexpr int kMaxTextCharacters = 262144;
inline constexpr int kMaxTableColumns = 16;
inline constexpr int kMaxTableRows = 512;
inline constexpr int kMaxCellCharacters = 8192;
inline constexpr int kMaxGraphNodes = 128;
inline constexpr int kMaxGraphEdges = 512;
inline constexpr int kMaxGraphNodeCharacters = 4096;
inline constexpr int kMaxAddressCharacters = 32;
inline constexpr int kMaxSectionNameCharacters = 128;
} // namespace limits

struct TaskLimits {
  size_t MaxQueued = kMaxQueuedTasks;
  size_t MaxHistory = kMaxTaskHistory;
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_CONFIG_H
