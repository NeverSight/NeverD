//===- OfflineQml.h - A QML engine that fetches nothing ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The window shows what the CLI tells it: names from project files, paths,
/// and text a model wrote about a binary. None of it may reach another
/// machine by being shown, so the window's engine answers every request it
/// is asked to make with an error instead of a connection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_OFFLINEQML_H
#define NEVERD_TOOLS_AGENT_PLATFORM_OFFLINEQML_H

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>

namespace neverd::agent {
class OfflineAccessManager final : public QNetworkAccessManager {
public:
  using QNetworkAccessManager::QNetworkAccessManager;

protected:
  // A request without a URL fails in the manager, before any socket.
  QNetworkReply *createRequest(Operation operation, const QNetworkRequest &,
                               QIODevice *data) override {
    return QNetworkAccessManager::createRequest(operation, QNetworkRequest(),
                                                data);
  }
};

class OfflineAccessFactory final : public QQmlNetworkAccessManagerFactory {
public:
  QNetworkAccessManager *create(QObject *parent) override {
    return new OfflineAccessManager(parent);
  }
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_OFFLINEQML_H
