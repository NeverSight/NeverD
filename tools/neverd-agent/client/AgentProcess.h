//===- AgentProcess.h - Agent CLI subprocess client -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own the CLI process and exchange frames on its private standard streams.

#ifndef NEVERD_TOOLS_AGENT_CLIENT_AGENTPROCESS_H
#define NEVERD_TOOLS_AGENT_CLIENT_AGENTPROCESS_H

#include <QJsonObject>
#include <QProcess>

namespace neverd::agent {
class AgentProcess final : public QObject {
  Q_OBJECT

public:
  explicit AgentProcess(QObject *parent = nullptr);
  ~AgentProcess() override;

  bool start(const QString &directory, QString &error);
  bool active() const;
  bool send(const QJsonObject &message);
  bool waitForOutput(int milliseconds);
  void close();

signals:
  void received(const QJsonObject &message);
  void failed(const QString &error);

private:
  void readOutput();
  void fail(const QString &error);

  QProcess Process;
  QByteArray Input;
  bool Closing = false;
  bool Failed = false;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_CLIENT_AGENTPROCESS_H
