//===- NphTools.h - Agent consumer of shared ND tools -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind model-visible actions to an exact NPH registration set per model step.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_NPHTOOLS_H
#define NEVERD_TOOLS_AGENT_RUNTIME_NPHTOOLS_H
#include "models/ProviderCodec.h"

#include "neverd/plugin/Config.h"
#include "neverd/tools/ToolRegistry.h"

#include <QHash>
#include <QObject>
#include <QSet>

namespace neverd::agent {
class NphTools final : public QObject {
public:
  using Done = std::function<void(QJsonObject)>;
  using Handler = plugin::Actions::Handler;
  NphTools(QString project, QString session, QObject *parent = nullptr);
  bool add(const QString &name, const QString &description,
           const QJsonArray &parameters, Handler handler, QString &error,
           plugin::ActionEffect effect = plugin::ActionEffect::Read,
           bool concurrent = false);
  bool concurrent(const QString &name) const {
    return Concurrent.contains(name);
  }
  bool activate(QString &error);
  QJsonArray resolve(int step, QString &error);
  void call(const ModelToolCall &call, Done done);
  void cancel();
  void close(std::function<void()> done);
  std::function<void(QJsonObject)> Receipt;
  static QJsonObject
  stringParameter(const QString &id, const QString &title,
                  int maxLength = plugin::limits::kMaxParameterCharacters);
  static plugin::Json json(const QJsonObject &object);
  static QJsonObject object(const plugin::Json &value);
  static void succeed(const std::shared_ptr<plugin::ActionCall> &call,
                      const QJsonObject &result);

private:
  plugin::PluginHost Host;
  plugin::Actions Actions;
  tools::Environment Environment;
  tools::ToolRegistry Registry;
  plugin::Owner Component;
  tools::ResolvedTools Set;
  QString Session, SetupError;
  QHash<QString, plugin::Registration> Tokens;
  QHash<QString, Done> Pending;
  QStringList Operations;
  QSet<QString> Concurrent;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_RUNTIME_NPHTOOLS_H
