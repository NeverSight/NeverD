//===- FindInViewDialog.h - Search the originating pane --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One modeless find dialog for pseudocode, IR and disassembly text.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_FINDINVIEWDIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_FINDINVIEWDIALOG_H

#include "ViewSearch.h"

#include <QDialog>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <utility>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace neverd::gui {

class CodeText;
class ListingView;

class FindInViewDialog final : public QDialog {
  Q_OBJECT
public:
  explicit FindInViewDialog(QWidget *parent);
  ~FindInViewDialog() override;
  void search(CodeText *code, ListingView *listing);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void done(int result) override;

private:
  QWidget *target() const;
  void releaseTarget();
  void refreshDocument();
  void rebuildMatches(bool select);
  void step(bool forward, bool skipSelected = true);
  void updateStatus();
  std::pair<int, int> selection() const;
  QPointer<CodeText> Code;
  QPointer<ListingView> Listing;
  QVector<QMetaObject::Connection> Connections;
  QLineEdit *Input;
  QCheckBox *MatchCase, *WholeWords, *Wrap;
  QLabel *Scope, *Status;
  QPushButton *Previous, *Next;
  QTimer Delay;
  QString Document;
  QVector<int> Matches;
  ViewSearchQuery Query;
  bool Ready = false, Pending = false, Selecting = false;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_FINDINVIEWDIALOG_H
