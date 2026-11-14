//===- Config.h - Agent application storage policy --------------*- C++ -*-===//
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
// An execution row carries at most this much of a tool's result; the whole
// result stays in the run's record and its evidence. Rows travel to the
// window together, so the protocol's frame limit allows thousands of them.
inline constexpr qint64 kMaxExecutionResultBytes = 8 * 1024;
inline constexpr int kDraftSaveDelayMs = 500;
inline constexpr int kCaptureSettleMs = 200;
inline constexpr int kSideChatTitleCharacters = 24;
inline constexpr qint64 kDefaultGoalTokens = 100000;
// An ordinary question stops once its model calls have used this many tokens,
// so text planted in a sample cannot keep a run calling tools and billing.
inline constexpr qint64 kDefaultRunTokens = 4000000;
inline constexpr int kDefaultGoalRounds = 20;
// Memory maintenance waits for an idle interaction, outside foreground work.
inline constexpr int kMemoryIdleDelayMs = 15000;
// Retrieval is a compact index. Complete records are fetched with read_memory.
inline constexpr int kMemoryRecallCharacters = 6000;
inline constexpr int kMemoryIndexExcerptCharacters = 240;
// Bound the extraction input to one completed exchange, never a full session.
inline constexpr int kMemorySourceCharacters = 24000;
// Memory text replaces the configured API key only when it is at least this
// long; shorter placeholders would match ordinary text.
inline constexpr int kMinRedactedCredentialLength = 8;
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_CONFIG_H
