//===- ElidedLabel.h - Bounded titles with complete text --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared single-line headings retain full accessible text and tooltips while
/// fitting the available width across window and font changes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_ELIDEDLABEL_H
#define NEVERD_GUI_APP_ELIDEDLABEL_H

#include "SingleLineText.h"

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QString>

namespace neverd::gui {
class ElidedLabel final : public QLabel {
public:
  explicit ElidedLabel(QWidget *Parent) : QLabel(Parent) {
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMinimumWidth(0);
  }
  void setFullText(const QString &Text) {
    FullText = Text;
    setToolTip(Text);
    setAccessibleName(Text);
    updateText();
  }

protected:
  void resizeEvent(QResizeEvent *Event) override {
    QLabel::resizeEvent(Event);
    updateText();
  }
  void changeEvent(QEvent *Event) override {
    QLabel::changeEvent(Event);
    if (Event->type() == QEvent::FontChange)
      updateText();
  }

private:
  QString FullText;
  void updateText() {
    setText(fontMetrics().elidedText(singleLineDisplayText(FullText),
                                     Qt::ElideMiddle, contentsRect().width()));
  }
};
} // namespace neverd::gui

#endif // NEVERD_GUI_APP_ELIDEDLABEL_H
