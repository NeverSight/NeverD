//===- LineageDialog.h - Version comparison review workspace ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Offline source composition, graph navigation, explicit path conflicts and
/// retained analyst assessments through one disposable worker session.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_LINEAGEDIALOG_H
#define NEVERD_GUI_LINEAGEDIALOG_H

#include "LineageSession.h"

#include <QDialog>
#include <QMap>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;

namespace neverd::gui {
class ElidedLabel;
class LineageGraph;
class LineagePageView;
class LineageReleaseView;
class LineageDialog final : public QDialog {
  Q_OBJECT
public:
  explicit LineageDialog(const QString &Worker, QWidget *Parent = nullptr);
  LineageSession &session() { return Session; }

protected:
  bool event(QEvent *Event) override;
  void closeEvent(QCloseEvent *Event) override;

private:
  LineageSession Session;
  LineagePageView *Versions, *Functions, *Sources, *Conflicts, *Reviews;
  LineagePageView *TraceNodes, *TraceEdges, *TraceConflicts;
  LineageGraph *Graph;
  LineageReleaseView *Releases;
  QTabWidget *Tabs;
  QLabel *Summary, *Status;
  ElidedLabel *FunctionTitle, *TraceTitle;
  QLineEdit *Search;
  QPlainTextEdit *Details;
  QPushButton *Add, *Open, *Save, *Cancel, *Rename, *Trace, *Inspect, *Export,
      *Members, *OptionsButton;
  QProgressBar *Progress;
  QTimer Filter;
  QJsonObject Options;
  QMap<QString, QString> Labels;
  QString CurrentVersion, TraceVersion, TraceAddress, SavedPath, OpeningPath,
      PendingOpenPath;
  bool Dirty = false, CloseWhenIdle = false, CloseAfterSave = false,
       SavingSource = false;
  void update();
  void loaded(bool Appended);
  QString versionLabel(const QString &ID) const;
  void updateTitles();
  void selectVersion(const QString &ID);
  void navigateToVersion(const QString &ID);
  void trace(const QJsonObject &Row);
  void source(const QString &Report);
  void members(const QJsonObject &Conflict);
  void metadata();
  void options();
  void save();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_LINEAGEDIALOG_H
