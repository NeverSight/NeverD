//===- McpSession.h - Object-scoped ND analysis process ----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Asynchronous stdio MCP client, bound to one fingerprinted analysis object.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_MCPSESSION_H
#define NEVERD_TOOLS_AGENT_RUNTIME_MCPSESSION_H
#include "project/ProjectStore.h"

#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>
namespace neverd::agent {
class McpSession final : public QObject {
public:
  using Done = std::function<void(QJsonObject)>;
  explicit McpSession(QObject *parent = nullptr);
  ~McpSession() override;
  void start(const QString &executable, const AnalysisObject &object,
             int timeoutSeconds, Done ready, const QString &attachment = {});
  qint64 call(const QString &tool, const QJsonObject &arguments, Done done);
  qint64 readResource(const QString &uri, Done done);
  void cancel(qint64 id);
  bool ready() const { return Ready; }
  QJsonArray tools() const { return Tools; }
  QString identity() const { return Identity; }

private:
  qint64 request(const QString &method, const QJsonObject &params, Done done);
  void send(const QJsonObject &message);
  void failAll(const QString &error);
  QProcess Process;
  QByteArray Buffer;
  QHash<qint64, Done> Pending;
  QHash<qint64, QTimer *> Timers;
  QJsonArray Tools;
  qint64 NextId = 0;
  int TimeoutMs = 0;
  bool Ready = false;
  QString Identity;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_RUNTIME_MCPSESSION_H
