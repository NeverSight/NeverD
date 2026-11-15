#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QThread>

// Transport lives on a dedicated thread. The UI receives complete bounded
// pages.
class WorkerTransport final : public QObject {
  Q_OBJECT
public slots:
  void start(const QString &program, quint64 epoch);
  void send(const QJsonObject &request);
  void stop();
  void shutdown();
signals:
  /// \p bytes is the frame's size as it came off the pipe, none for a message
  /// made here.
  void message(const QJsonObject &message, quint64 epoch, qint64 bytes = 0);
  void diagnostic(const QString &message, quint64 epoch);
  void failure(const QString &message, quint64 epoch);
  void stopped(quint64 epoch);

private:
  void receive();
  /// Report the complete lines of engine stderr; with \p all, the rest too.
  void flushDiagnostics(bool all);
  QProcess *process_ = nullptr;
  QByteArray input_;
  /// Engine stderr after its last complete line: one diagnostic often
  /// arrives in several writes.
  QByteArray diagnostics_;
  quint64 epoch_ = 0;
};

class EngineClient final : public QObject {
  Q_OBJECT
public:
  explicit EngineClient(QObject *parent = nullptr);
  ~EngineClient() override;
  void start(const QString &program);
  QString request(const QString &operation, const QJsonObject &payload = {},
                  const QString &revision = {});
  void stop();
signals:
  void startRequested(const QString &program, quint64 epoch);
  void sendRequested(const QJsonObject &request);
  void stopRequested();
  /// \p bytes is the message's size on the wire, where it came off one: what
  /// a reply weighs without being written out again.
  void message(const QJsonObject &message, qint64 bytes = 0);
  void diagnostic(const QString &message);
  void failure(const QString &message);
  void stopped();

private:
  QThread thread_;
  WorkerTransport *transport_;
  quint64 nextRequest_ = 0, epoch_ = 0;
};
