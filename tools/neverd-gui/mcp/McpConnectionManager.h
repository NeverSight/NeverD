#pragma once

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QSslConfiguration>
#include <QVariantList>
#include <optional>

// A manually operated MCP client. Server text is content, never host
// instructions.
class McpConnectionManager final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool connected READ connected NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QVariantList tools READ tools NOTIFY changed)
  Q_PROPERTY(QVariantList resources READ resources NOTIFY changed)
  Q_PROPERTY(QString lastResult READ lastResult NOTIFY changed)
  Q_PROPERTY(QVariantList callHistory READ callHistory NOTIFY changed)

public:
  explicit McpConnectionManager(QObject *parent = nullptr);
  ~McpConnectionManager() override;
  bool connected() const { return ready_; }
  QString status() const;
  QVariantList tools() const { return tools_; }
  QVariantList resources() const { return resources_; }
  QString lastResult() const { return lastResult_; }
  QVariantList callHistory() const { return history_; }
  Q_INVOKABLE void connectStdio(const QString &program,
                                const QStringList &arguments);
  Q_INVOKABLE void connectHttp(const QString &url, const QString &bearerToken,
                               const QString &caCertificateFile = QString());
  Q_INVOKABLE void disconnectServer();
  Q_INVOKABLE void listTools();
  Q_INVOKABLE void listResources();
  Q_INVOKABLE void callTool(const QString &name, const QString &jsonArguments);
  Q_INVOKABLE void readResource(const QString &uri);
  Q_INVOKABLE void retranslate() { emit changed(); }
  Q_INVOKABLE void cancelCall(const QString &id);
  Q_INVOKABLE void inspectCall(const QString &id);

signals:
  void changed();

private:
  struct Pending {
    QString method;
    qint64 started;
  };
  struct HttpStream {
    QByteArray buffer;
    QByteArray eventData;
    QByteArray eventId;
    QByteArray pendingEventId;
    QString requestId;
    int retryMs = 1000;
    int reconnects = 0;
    bool completed = false;
    bool hasPendingEventId = false;
  };
  void initialize();
  QString send(const QString &method, const QJsonObject &params = {},
               bool notification = false);
  void post(const QJsonObject &message);
  void watchReply(QNetworkReply *reply, HttpStream stream);
  void readHttp(QNetworkReply *reply);
  void consume(const QJsonObject &message);
  void fail(const QString &message);
  void failSource(const char *source, const QStringList &arguments = {});
  void setStatus(const char *source, const QStringList &arguments = {});
  void recordCall(const QString &id, const QString &method, const QString &name,
                  const QString &arguments);
  void finishCall(const QString &id, const QString &state,
                  const QString &result = {});
  void closeReplies(const QString &id);
  QNetworkRequest request() const;
  QProcess process_;
  QNetworkAccessManager network_;
  QByteArray input_;
  QByteArray diagnostics_;
  QHash<QString, Pending> pending_;
  QHash<QNetworkReply *, QSharedPointer<HttpStream>> replies_;
  QUrl endpoint_;
  QByteArray token_;
  QByteArray session_;
  // Even the empty Qt SSL configuration initializes the platform TLS backend.
  // Keep that work on the explicit HTTP connection path, away from GUI startup.
  std::optional<QSslConfiguration> tls_;
  QVariantList tools_;
  QVariantList resources_;
  QVariantList history_;
  QString status_;
  QByteArray statusSource_;
  QStringList statusArguments_;
  QString lastResult_;
  quint64 serial_ = 0;
  quint64 generation_ = 0;
  bool ready_ = false;
  bool stopping_ = false;
};
