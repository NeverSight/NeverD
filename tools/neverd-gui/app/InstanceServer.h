//===- InstanceServer.h - Other programs' way into a workbench -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Each workbench's local server, as IDA 9.4's IPC server is each IDA's:
/// another NeverD, the command line or a script finds the running
/// workbenches and the program each has open (get_info), and sends one a
/// link to open (open_link), so that a link clicked anywhere lands in the
/// window that has its program.  A request and its reply are one JSON
/// object each, on a line; a client that writes its request without the
/// newline, as IDA's hcli does, is answered too.  Requests, by `cmd`:
/// `ping`, `get_info`, `open_link` with `uri`, and `is_analysis_complete`;
/// a reply carries `status` ("ok" or "error") and, for an error, `message`.
///
/// Where IDA's sockets lie in /tmp, the servers live in a directory only
/// this user can enter (the runtime directory, owner-only), and a
/// connection from another user is refused.  On Windows each server is a
/// named pipe with the user's own access, which a file in that directory
/// names.  The `instances/disabled` setting keeps a workbench from serving,
/// as IDA's DisableIpcServer does.
//===----------------------------------------------------------------------===//

#pragma once

#include "../ResourceLimits.h"

#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

class QLocalSocket;

namespace neverd::gui {

struct DeepLink;

class InstanceServer final : public QObject {
  Q_OBJECT
public:
  /// A running workbench: its process and the server to connect to.
  struct Instance {
    qint64 pid = 0;
    QString server;
  };
  /// What get_info answers, beyond the process and server.
  using Info = std::function<QJsonObject()>;
  /// Opens \p link and calls \p done with nothing, or the reason it could
  /// not.
  using LinkHandler = std::function<void(
      const DeepLink &link, std::function<void(const QString &error)> done)>;

  explicit InstanceServer(QObject *parent = nullptr);
  ~InstanceServer() override;

  /// Serve, unless the setting turns serving off; false, with the reason in
  /// errorString(), when the server cannot listen.
  bool start();
  void stop();
  bool listening() const { return server_.isListening(); }
  /// What a client connects to.
  QString serverName() const { return server_.fullServerName(); }
  QString errorString() const { return error_; }
  void setInfo(Info info) { info_ = std::move(info); }
  void setLinkHandler(LinkHandler handler) { handler_ = std::move(handler); }

  /// The directory the servers live in, owner-only; empty when none can be
  /// made.
  static QString directory();
  /// The running workbenches; what a dead one left behind goes.
  static QList<Instance> discover();
  /// \p request to \p server and its reply, none when the server does not
  /// answer within \p timeoutMs.  It blocks, for a process of its own, such
  /// as the command line's dispatch.
  static std::optional<QJsonObject> request(
      const QString &server, const QJsonObject &request,
      int timeoutMs = ::neverd::limits::gui::kInstanceServerRequestTimeoutMs);
  /// Probe servers concurrently under one deadline. A true stop result
  /// ends probing after that reply; unresponsive entries remain empty. Each
  /// wave gets a share of the budget so stalled peers cannot starve the queue.
  static QList<std::optional<QJsonObject>> requestMany(
      const QStringList &servers, const QJsonObject &request,
      int timeoutMs = ::neverd::limits::gui::kInstanceServerRequestTimeoutMs,
      std::function<bool(int, const QJsonObject &)> stop = {});

private:
  struct RequestBuffer;
  void accept();
  void read(QLocalSocket *socket, RequestBuffer &request);
  void answer(QLocalSocket *socket, const QByteArray &text);
  static void reply(QLocalSocket *socket, const QJsonObject &reply);

  QLocalServer server_;
  /// Windows: the file that names this server's pipe.
  QString marker_;
  QString error_;
  Info info_;
  LinkHandler handler_;
};

} // namespace neverd::gui
