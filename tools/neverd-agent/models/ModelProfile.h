//===- ModelProfile.h - Model endpoint and run policy -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validated provider configuration. Credentials are referenced, never
/// embedded.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_MODELS_MODELPROFILE_H
#define NEVERD_TOOLS_AGENT_MODELS_MODELPROFILE_H

#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <optional>

namespace neverd::agent {
enum class ModelProtocol { Responses, Messages, ChatCompletions };

struct ModelProfile {
  ModelProtocol Protocol = ModelProtocol::Responses;
  QString BaseUrl = "https://api.openai.com/v1";
  QString Model;
  QString CredentialId;
  QString AnthropicVersion = "2026-10-21";
  QString CompletionTokenField = "max_completion_tokens";
  QString ReasoningEffort;
  int ContextTokens = 128000;
  int OutputTokens = 8192;
  int TimeoutSeconds = 120;
  int MaxAgents = 4;
  int MaxDepth = 3;

  QString protocolName() const;
  QUrl endpoint() const;
  QString identity() const;
  qint64 compactionThreshold() const;
  int compactionOutputTokens() const;
  QJsonObject toJson() const;
  bool validate(QString &error, bool requireModel = true) const;
  /// Whether an API key may travel to this profile's server: over HTTPS, or
  /// over plain HTTP only to this computer.
  bool carriesKeysSafely() const;
  /// Whether \p first and \p second address the same server (scheme, host
  /// and port), to which a saved API key belongs.
  static bool sameServer(const QString &first, const QString &second);
  static std::optional<ModelProfile> fromJson(const QJsonObject &value,
                                              QString &error);
};

namespace model_limits {
inline constexpr qint64 kMaxResponseBytes = 16 * 1024 * 1024;
inline constexpr qint64 kMaxEventBytes = 8 * 1024 * 1024;
inline constexpr qint64 kMaxCredentialBytes = 16 * 1024;
inline constexpr int kConnectionTestOutputTokens = 1024;
inline constexpr int kContextEnvelopeTokens = 256;
// Compact before the input allowance fills, leaving room for the summary
// request itself. Keep recent complete exchanges only within the target.
inline constexpr int kAutoCompactPercent = 80;
inline constexpr int kCompactTargetPercent = 50;
// Reserve part of retained history for exact earlier user requests.
inline constexpr int kCompactUserPercent = 25;
inline constexpr int kCompactOutputTokens = 4096;
inline constexpr int kCompactOutputDivisor = 8;
// A local heuristic, not a provider tokenizer or an exact context guarantee.
inline constexpr int kEstimatedBytesPerToken = 3;
inline constexpr int kRunAutosaveMs = 1000;
inline constexpr int kStreamUiUpdateMs = 40;
inline constexpr int kProcessExitWaitMs = 1000;
inline constexpr int kMaxToolTextCharacters = 65536;
} // namespace model_limits
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_MODELS_MODELPROFILE_H
