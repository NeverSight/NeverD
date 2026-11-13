//===- Config.h - Agent application storage policy ---------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Versioned local storage and presentation limits for the Agent application.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_CONFIG_H
#define NEVERD_TOOLS_AGENT_CONFIG_H

#include <QtGlobal>

namespace neverd::agent {
inline constexpr int kStorageVersion = 1;
inline constexpr qint64 kMaxStateBytes = 8 * 1024 * 1024;
inline constexpr qint64 kMaxPreviewBytes = 256 * 1024;
inline constexpr int kDraftSaveDelayMs = 500;
inline constexpr int kCaptureSettleMs = 200;
inline constexpr int kSideChatTitleCharacters = 24;
inline constexpr qint64 kDefaultGoalTokens = 100000;
inline constexpr int kDefaultGoalRounds = 20;
// Memory maintenance waits for an idle interaction, outside foreground work.
inline constexpr int kMemoryIdleDelayMs = 15000;
// Retrieval is a compact index. Complete records are fetched with read_memory.
inline constexpr int kMemoryRecallCharacters = 6000;
inline constexpr int kMemoryIndexExcerptCharacters = 240;
// Bound the extraction input to one completed exchange, never a full session.
inline constexpr int kMemorySourceCharacters = 24000;
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_CONFIG_H
