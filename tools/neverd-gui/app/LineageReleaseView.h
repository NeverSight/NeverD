//===- LineageReleaseView.h - Declared release timelines --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded release selection and metadata edits. The worker owns chronology,
/// parent validity and transactional publication; Qt owns presentation only.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_LINEAGERELEASEVIEW_H
#define NEVERD_GUI_LINEAGERELEASEVIEW_H

#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace neverd::gui {
class LineagePageView;
class LineageSession;
class ElidedLabel;
class LineageReleaseView final : public QWidget {
  Q_OBJECT
public:
  explicit LineageReleaseView(LineageSession &Session,
                              QWidget *Parent = nullptr);
  void reload(bool Preserve);
  void setSnapshot(const QString &ID, const QString &Label);
  void setOptions(const QJsonObject &Options) { Admission = Options; }

signals:
  void snapshotRequested(const QString &ID);

protected:
  bool event(QEvent *Event) override;

private:
  LineageSession &Session;
  LineagePageView *Releases, *Parents, *Children;
  QLineEdit *Search;
  QLabel *Status;
  ElidedLabel *SelectedRelease;
  QPushButton *Add, *Edit, *Remove, *OpenSnapshot, *AddParent, *RemoveParent;
  QTimer Filter;
  QJsonObject Admission;
  QString Snapshot, SnapshotLabel, Focus;
  QString RestoreRelations, RestoreParent, RestoreChild;
  qint64 ParentOffset = 0, ChildOffset = 0;
  bool Edited = false;
  void update();
  void selected(const QJsonObject &Row);
  void edit(bool Existing);
  void parent(bool Add);
  QJsonObject choose(const QString &Kind, const QString &Title,
                     const QString &Excluded = {});
};
} // namespace neverd::gui

#endif // NEVERD_GUI_LINEAGERELEASEVIEW_H
