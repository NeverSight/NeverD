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
signals:
  void message(const QJsonObject &message, quint64 epoch);
  void diagnostic(const QString &message, quint64 epoch);
  void failure(const QString &message, quint64 epoch);
  void stopped(quint64 epoch);

private:
  void receive();
  QProcess *process_ = nullptr;
  QByteArray input_;
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
  void message(const QJsonObject &message);
  void diagnostic(const QString &message);
  void failure(const QString &message);
  void stopped();

private:
  QThread thread_;
  WorkerTransport *transport_;
  quint64 nextRequest_ = 0, epoch_ = 0;
};
