#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>

class QLocalSocket;

// Optional, authenticated bridge to the GUI's existing EngineClient.
class GuiSessionBroker final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
  Q_PROPERTY(QString credentialFile READ credentialFile NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)

public:
  explicit GuiSessionBroker(QObject *parent = nullptr);
  ~GuiSessionBroker() override;
  bool enabled() const { return server_.isListening(); }
  QString credentialFile() const { return credentialFile_; }
  QString status() const;
  Q_INVOKABLE void retranslate() { emit changed(); }
  Q_INVOKABLE bool start();
  Q_INVOKABLE void stop();
  void setSelection(const QJsonObject &selection);
  void reply(const QString &brokerRequestId, const QJsonObject &workerResponse);

signals:
  void changed();
  void queryRequested(QString brokerRequestId, QString operation,
                      QJsonObject payload, QString expectedRevision);
  void navigationRequested(QString address, bool highlightOnly);

private:
  struct Client {
    QByteArray buffer;
    bool authenticated = false;
    int pending = 0;
  };
  struct Route {
    QPointer<QLocalSocket> socket;
    QString clientId;
    qint64 started;
  };
  void acceptClients();
  void readClient(QLocalSocket *socket);
  void handle(QLocalSocket *socket, const QJsonObject &request);
  void write(QLocalSocket *socket, const QJsonObject &message);
  void error(QLocalSocket *socket, const QString &id, const QString &code,
             const QString &message);
  void setStatus(const char *source, const QString &detail = {});
  QLocalServer server_;
  QHash<QLocalSocket *, Client> clients_;
  QHash<QString, Route> routes_;
  QJsonObject selection_;
  std::unique_ptr<QTemporaryDir> directory_;
  QString credentialFile_;
  QString token_;
  QString sharedProject_;
  QByteArray statusSource_;
  QString statusDetail_;
  quint64 serial_ = 0;
};
