//===- LineageSession.h - Offline version workspace ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A disposable worker retains the collection independently of the project.
/// The GUI owns navigation and bounded pages, never correspondence semantics.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_LINEAGESESSION_H
#define NEVERD_GUI_LINEAGESESSION_H

#include "EngineClient.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <functional>

namespace neverd::gui {
class LineageSession final : public QObject {
  Q_OBJECT
public:
  using Reply = std::function<void(const QJsonObject &)>;
  explicit LineageSession(const QString &Worker, QObject *Parent = nullptr);
  bool ready() const { return Ready; }
  bool releaseEditingSupported() const { return ReleaseEditing; }
  bool busy() const { return !Requests.isEmpty(); }
  bool cancelRequested() const { return CancelRequested; }
  bool cancellable() const { return !Cancellable.isEmpty(); }
  const QJsonObject &summary() const { return Summary; }
  QString revision() const { return Summary["revision"].toString(); }
  QString message() const { return Message; }
  void load(const QJsonObject &Payload);
  void editReleases(QJsonObject Edit, const QJsonObject &Options,
                    const QString &Focus);
  void save(const QString &Path, const QString &Report = {});
  void cancel();
  void setQueryOptions(QJsonObject Options) {
    QueryOptions = std::move(Options);
  }
  void request(const QString &Operation, QJsonObject Payload, QObject *Owner,
               Reply Complete);

signals:
  void changed();
  void collectionChanged(bool Appended);
  void releasesEdited(const QString &Focus);
  void progress(const QJsonObject &Value);
  void failed(const QString &Message);
  void saved(const QString &Path);

private:
  struct Pending {
    QPointer<QObject> Owner;
    Reply Complete;
  };
  EngineClient Client;
  QHash<QString, Pending> Requests;
  QStringList Cancellable, CancellableOperations;
  QJsonObject Summary, QueryOptions;
  QString Message;
  bool Ready = false, CancelRequested = false, ReleaseEditing = false;
  void receive(const QJsonObject &Value);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_LINEAGESESSION_H
