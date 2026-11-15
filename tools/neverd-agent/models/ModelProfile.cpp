//===- ModelProfile.cpp - Model endpoint and run policy -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize API roots once, validate settings and capture provider identity.
//===----------------------------------------------------------------------===//

#include "ModelProfile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace neverd::agent {
namespace {
struct Text {
  Q_DECLARE_TR_FUNCTIONS(neverd::agent::ModelProfile)
};
} // namespace

QString ModelProfile::protocolName() const {
  switch (Protocol) {
  case ModelProtocol::Responses:
    return "responses";
  case ModelProtocol::Messages:
    return "messages";
  case ModelProtocol::ChatCompletions:
    return "chat-completions";
  }
  Q_UNREACHABLE();
}

QUrl ModelProfile::endpoint() const {
  QUrl result(BaseUrl.trimmed(), QUrl::StrictMode);
  QString path = result.path();
  while (path.endsWith('/'))
    path.chop(1);
  const QString suffix = Protocol == ModelProtocol::Responses ? "/responses"
                         : Protocol == ModelProtocol::Messages
                             ? "/messages"
                             : "/chat/completions";
  if (!path.endsWith(suffix)) {
    if (!path.endsWith("/v1"))
      path += "/v1";
    path += suffix;
  }
  result.setPath(path);
  return result;
}

int ModelProfile::compactionOutputTokens() const {
  return std::min({model_limits::kCompactOutputTokens, OutputTokens,
                   std::max(1, (ContextTokens - OutputTokens) /
                                   model_limits::kCompactOutputDivisor)});
}
qint64 ModelProfile::compactionThreshold() const {
  return qint64(ContextTokens - OutputTokens) *
         model_limits::kAutoCompactPercent / 100;
}
QString ModelProfile::identity() const {
  const QByteArray bytes =
      QJsonDocument(QJsonObject{{"protocol", protocolName()},
                                {"endpoint", endpoint().toString()},
                                {"model", Model},
                                {"credential", CredentialId},
                                {"anthropicVersion", AnthropicVersion},
                                {"reasoningEffort", ReasoningEffort}})
          .toJson(QJsonDocument::Compact);
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool ModelProfile::carriesKeysSafely() const {
  const QUrl url(BaseUrl, QUrl::StrictMode);
  if (url.scheme() == QLatin1String("https"))
    return true;
  const QHostAddress address(url.host());
  return url.scheme() == QLatin1String("http") &&
         (url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) ==
              0 ||
          address.isLoopback());
}

bool ModelProfile::sameServer(const QString &first, const QString &second) {
  const QUrl a(first, QUrl::StrictMode), b(second, QUrl::StrictMode);
  const auto port = [](const QUrl &url) {
    return url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
  };
  return a.isValid() && b.isValid() && a.scheme() == b.scheme() &&
         a.host().compare(b.host(), Qt::CaseInsensitive) == 0 &&
         port(a) == port(b);
}

bool ModelProfile::validate(QString &error, bool requireModel) const {
  const QUrl url(BaseUrl, QUrl::StrictMode);
  if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http") ||
      url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment() ||
      url.hasQuery()) {
    error = Text::tr("The API URL must be a valid HTTP(S) address. Enter the "
                     "API key in its own field.");
    return false;
  }
  const QString path =
      url.path().endsWith('/') ? url.path().chopped(1) : url.path();
  const QString expected = Protocol == ModelProtocol::Responses ? "/responses"
                           : Protocol == ModelProtocol::Messages
                               ? "/messages"
                               : "/chat/completions";
  for (const QString suffix : {QString("/responses"), QString("/messages"),
                               QString("/chat/completions")}) {
    if (path.endsWith(suffix) && suffix != expected) {
      error = Text::tr(
          "The API URL's endpoint path does not match the selected protocol.");
      return false;
    }
  }
  if (requireModel && Model.trimmed().isEmpty()) {
    error = Text::tr("Enter a model name that the server supports.");
    return false;
  }
  if (Model.contains('\n') || Model.contains('\r')) {
    error = Text::tr("The model name cannot contain line breaks.");
    return false;
  }
  if (ContextTokens <= 0 || OutputTokens <= 0 ||
      OutputTokens >= ContextTokens) {
    error = Text::tr("The context window must be larger than the output limit, "
                     "and both must be positive integers.");
    return false;
  }
  if (TimeoutSeconds <= 0 ||
      TimeoutSeconds > std::numeric_limits<int>::max() / 1000 ||
      MaxAgents <= 0 || MaxDepth < 0) {
    error = Text::tr("The timeout and parallel task count must be positive "
                     "integers, and the subtask depth cannot be negative.");
    return false;
  }
  if (CompletionTokenField != "max_tokens" &&
      CompletionTokenField != "max_completion_tokens") {
    error = Text::tr("Invalid output field for Chat Completions.");
    return false;
  }
  if (!ReasoningEffort.isEmpty() && ReasoningEffort != "low" &&
      ReasoningEffort != "medium" && ReasoningEffort != "high" &&
      ReasoningEffort != "max") {
    error = Text::tr("Unsupported reasoning effort. Leave it empty to use the "
                     "server default.");
    return false;
  }
  static const QRegularExpression version("^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
  if (!version.match(AnthropicVersion).hasMatch()) {
    error = Text::tr("Enter the Claude API version as YYYY-MM-DD.");
    return false;
  }
  if (!CredentialId.isEmpty() &&
      (QUuid(CredentialId).isNull() ||
       QUuid(CredentialId).toString(QUuid::WithoutBraces) != CredentialId)) {
    error = Text::tr("Invalid model credential reference.");
    return false;
  }
  return true;
}

QJsonObject ModelProfile::toJson() const {
  return {{"protocol", protocolName()},
          {"url", BaseUrl},
          {"model", Model},
          {"credentialId", CredentialId},
          {"contextTokens", ContextTokens},
          {"outputTokens", OutputTokens},
          {"timeoutSeconds", TimeoutSeconds},
          {"maxAgents", MaxAgents},
          {"maxDepth", MaxDepth},
          {"anthropicVersion", AnthropicVersion},
          {"completionTokenField", CompletionTokenField},
          {"reasoningEffort", ReasoningEffort}};
}

std::optional<ModelProfile> ModelProfile::fromJson(const QJsonObject &value,
                                                   QString &error) {
  ModelProfile result;
  const auto protocol = value.value("protocol").toString();
  if (protocol == "responses")
    result.Protocol = ModelProtocol::Responses;
  else if (protocol == "messages")
    result.Protocol = ModelProtocol::Messages;
  else if (protocol == "chat-completions")
    result.Protocol = ModelProtocol::ChatCompletions;
  else {
    error = Text::tr("Unknown model protocol.");
    return std::nullopt;
  }
  for (const char *key : {"url", "model", "credentialId", "anthropicVersion",
                          "completionTokenField", "reasoningEffort"})
    if (value.contains(key) && !value.value(key).isString()) {
      error = Text::tr("Wrong type for model configuration field: %1")
                  .arg(QLatin1String(key));
      return std::nullopt;
    }
  result.BaseUrl = value.value("url").toString(result.BaseUrl).trimmed();
  result.Model = value.value("model").toString().trimmed();
  result.CredentialId = value.value("credentialId").toString();
  result.AnthropicVersion =
      value.value("anthropicVersion").toString(result.AnthropicVersion);
  result.CompletionTokenField =
      value.value("completionTokenField").toString(result.CompletionTokenField);
  result.ReasoningEffort = value.value("reasoningEffort").toString();
  const auto integer = [&](const char *key, int &target) {
    if (!value.contains(key))
      return true;
    const auto raw = value.value(key);
    const double number = raw.toDouble(-1);
    if (!raw.isDouble() || !std::isfinite(number) ||
        std::floor(number) != number || number < 0 ||
        number > std::numeric_limits<int>::max())
      return false;
    target = static_cast<int>(number);
    return true;
  };
  if (!integer("contextTokens", result.ContextTokens) ||
      !integer("outputTokens", result.OutputTokens) ||
      !integer("timeoutSeconds", result.TimeoutSeconds) ||
      !integer("maxAgents", result.MaxAgents) ||
      !integer("maxDepth", result.MaxDepth)) {
    error = Text::tr(
        "Counts and limits in the model configuration must be valid integers.");
    return std::nullopt;
  }
  if (!result.validate(error, false))
    return std::nullopt;
  return result;
}
} // namespace neverd::agent
