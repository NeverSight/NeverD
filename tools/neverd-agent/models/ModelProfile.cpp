//===- ModelProfile.cpp - Model endpoint and run policy
//---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize API roots once, validate settings and capture provider identity.
//===----------------------------------------------------------------------===//

#include "ModelProfile.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace neverd::agent {
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

bool ModelProfile::validate(QString &error, bool requireModel) const {
  const QUrl url(BaseUrl, QUrl::StrictMode);
  if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http") ||
      url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment() ||
      url.hasQuery()) {
    error = QStringLiteral(
        "API URL 必须是有效的 HTTP(S) 地址；API Key 请填在单独的字段中。");
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
      error = QStringLiteral("API URL 的接口路径与所选协议不一致。");
      return false;
    }
  }
  if (requireModel && Model.trimmed().isEmpty()) {
    error = QStringLiteral("请填写服务端支持的模型名称。");
    return false;
  }
  if (Model.contains('\n') || Model.contains('\r')) {
    error = QStringLiteral("模型名称不能包含换行。");
    return false;
  }
  if (ContextTokens <= 0 || OutputTokens <= 0 ||
      OutputTokens >= ContextTokens) {
    error = QStringLiteral("上下文窗口必须大于输出上限，两者均需为正整数。");
    return false;
  }
  if (TimeoutSeconds <= 0 ||
      TimeoutSeconds > std::numeric_limits<int>::max() / 1000 ||
      MaxAgents <= 0 || MaxDepth < 0) {
    error = QStringLiteral(
        "超时和并行任务数必须为有效正整数，子任务深度不能为负。");
    return false;
  }
  if (CompletionTokenField != "max_tokens" &&
      CompletionTokenField != "max_completion_tokens") {
    error = QStringLiteral("Chat Completions 的输出字段无效。");
    return false;
  }
  if (!ReasoningEffort.isEmpty() && ReasoningEffort != "low" &&
      ReasoningEffort != "medium" && ReasoningEffort != "high" &&
      ReasoningEffort != "max") {
    error = QStringLiteral("不支持的思考强度。留空使用服务端默认值。");
    return false;
  }
  static const QRegularExpression version("^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
  if (!version.match(AnthropicVersion).hasMatch()) {
    error = QStringLiteral("Claude API 版本请填写 YYYY-MM-DD。");
    return false;
  }
  if (!CredentialId.isEmpty() &&
      (QUuid(CredentialId).isNull() ||
       QUuid(CredentialId).toString(QUuid::WithoutBraces) != CredentialId)) {
    error = QStringLiteral("模型凭据引用无效。");
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
    error = QStringLiteral("未知的模型协议。");
    return std::nullopt;
  }
  for (const char *key : {"url", "model", "credentialId", "anthropicVersion",
                          "completionTokenField", "reasoningEffort"})
    if (value.contains(key) && !value.value(key).isString()) {
      error =
          QStringLiteral("模型配置字段类型错误：%1").arg(QLatin1String(key));
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
    error = QStringLiteral("模型配置中的数量和上限必须是有效整数。");
    return std::nullopt;
  }
  if (!result.validate(error, false))
    return std::nullopt;
  return result;
}
} // namespace neverd::agent
