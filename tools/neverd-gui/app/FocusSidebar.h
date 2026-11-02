//===- FocusSidebar.h - Collapsible focus navigation ----------*- C++ -*-===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A temporary navigation overlay that leaves the analysis layout intact.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_FOCUSSIDEBAR_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_FOCUSSIDEBAR_H

#include <QPointer>
#include <QString>
#include <QWidget>

class QToolButton;

namespace neverd::gui {

class FocusSidebar final : public QWidget {
  Q_OBJECT
public:
  FocusSidebar(QWidget &host, QWidget &area, QWidget &content,
               const QString &title);
  ~FocusSidebar() override;

  /// Opening the overlay leaves keyboard focus in the analysis view.
  void reveal();
  void dismiss();

signals:
  void widthChanged(int width);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  void place();
  QWidget &host_;
  QPointer<QWidget> area_;
  QPointer<QToolButton> toggle_;
  QPointer<QWidget> previousFocus_;
  QWidget *resizeHandle_;
  int preferredWidth_;
  int resizeStartWidth_ = 0;
  qreal resizeStartX_ = 0;
  bool resizing_ = false;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_FOCUSSIDEBAR_H
