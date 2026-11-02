//===- PatchEditor.h - Inline patch preview and editing --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-fenced byte and assembly editor shared by listing and hex.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_PATCHEDITOR_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_PATCHEDITOR_H
#include "Address.h"

#include <QFrame>
#include <QJsonObject>
#include <QTimer>
class QLineEdit;
class QCheckBox;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
namespace neverd::gui {
class Session;
class PatchEditor final : public QFrame {
  Q_OBJECT
public:
  PatchEditor(Session &session, Address address, QString kind, QString initial,
              QWidget *parent = nullptr, bool nibble = false);
  /// Open next to the caret in the owning viewport. The title can be dragged.
  void openAt(const QRect &anchor);
signals:
  void applied();
  void cancelled();
  void navigateRequested(int direction);

protected:
  bool eventFilter(QObject *, QEvent *) override;
  void resizeEvent(QResizeEvent *) override;

private:
  void preview();
  void showPreview(const QString &text);
  void apply();
  void place();
  QJsonObject request() const;
  Session &session_;
  Address address_;
  QString kind_, revision_;
  quint64 epoch_, serial_ = 0;
  QLineEdit *input_;
  QCheckBox *fill_, *overlap_;
  QComboBox *dialect_;
  QPlainTextEdit *preview_;
  QPushButton *apply_;
  QTimer debounce_;
  QJsonObject planned_;
  QWidget *header_;
  QRect anchor_;
  QPoint dragStart_, dragPosition_;
  bool placed_ = false, pressed_ = false, dragging_ = false, moved_ = false;
};
} // namespace neverd::gui
#endif // NEVERD_TOOLS_NEVERD_GUI_APP_PATCHEDITOR_H
