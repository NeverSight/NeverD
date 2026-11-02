//===- ComparisonSession.h - Asynchronous comparison workspace -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A disposable worker for two comparison inputs, independent of the current
/// project. Replies and edits are bound to the published comparison revision.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONSESSION_H
#define NEVERD_GUI_COMPARISONSESSION_H

#include "EngineClient.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <functional>

namespace neverd::gui {
class ComparisonSession final : public QObject {
  Q_OBJECT
public:
  using Reply = std::function<void(const QJsonObject &)>;
  ComparisonSession(const QString &Worker, QObject *Parent = nullptr);
  bool ready() const { return Ready; }
  bool supports(const QString &Operation) const {
    return Operations.contains(Operation);
  }
  bool busy() const { return Busy; }
  bool cancellable() const { return !Cancellable.isEmpty(); }
  bool cancelRequested() const { return CancelRequested; }
  const QJsonObject &summary() const { return Summary; }
  QString operationMessage() const { return OperationMessage; }
  QString revision() const { return Summary["comparison_revision"].toString(); }
  void start(const QJsonObject &Primary, const QJsonObject &Secondary,
             const QJsonObject &Options = {}, const QString &Saved = {});
  void cancel();
  void command(const QString &Operation, const QJsonObject &Payload = {});
  void request(const QString &Operation, QJsonObject Payload, QObject *Owner,
               Reply Complete);

signals:
  void changed();
  void mappingChanged();
  void progress(const QJsonObject &Progress);
  void failed(const QString &Message);
  void saved(const QString &Path);

private:
  struct Pending {
    QPointer<QObject> Owner;
    Reply Complete;
  };
  EngineClient Client;
  QHash<QString, Pending> Requests;
  QJsonObject Summary;
  QString OperationMessage;
  QStringList Cancellable, CancellableOperations, Operations;
  bool Ready = false, Busy = false, CancelRequested = false;
  void receive(const QJsonObject &Message);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONSESSION_H
