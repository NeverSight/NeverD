//===- ComparisonSourceView.h - Paired source inspection ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Lazy, revision-bound source pages with original-instruction navigation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONSOURCEVIEW_H
#define NEVERD_GUI_COMPARISONSOURCEVIEW_H

#include <QJsonObject>
#include <QWidget>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace neverd::gui {
class ComparisonSession;
class TypeSourceHighlighter;

class ComparisonSourceView final : public QWidget {
  Q_OBJECT
public:
  explicit ComparisonSourceView(ComparisonSession &Session,
                                QWidget *Parent = nullptr);
  void setFunctions(const QJsonObject &Primary, const QJsonObject &Secondary);

signals:
  void instructionSelected(const QString &Side, int ListingRow);

protected:
  void showEvent(QShowEvent *Event) override;

private:
  struct Pane {
    QJsonObject Function, Document;
    QLabel *Title = nullptr, *Status = nullptr;
    QPlainTextEdit *Text = nullptr;
    TypeSourceHighlighter *Highlighter = nullptr;
    QToolButton *Previous = nullptr, *Next = nullptr;
    QComboBox *Anchors = nullptr;
    QPushButton *Instruction = nullptr;
    QSpinBox *Line = nullptr;
    QString RangeText;
    bool Pending = false, Initial = true, NeedsLoad = true, Refresh = false;
    int Offset = 0;
  } Panes[2];
  ComparisonSession &Session;
  QComboBox *Representation;
  QCheckBox *AlignmentMode;
  QLabel *Hint;
  QAction *Unlimited, *UnlimitedWork, *UnlimitedBytes, *IncludePrelude;
  QString Revision;
  quint64 Generation = 0;
  void reset(bool Refresh = false);
  void load(int Side, int Offset = 0);
  bool aligned() const;
  void loadDifference(int Offset);
  void highlight(int Side);
  void cursor(int Side);
  void updateControls(int Side);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONSOURCEVIEW_H
