#include "DialogReturnKey.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QToolButton>

namespace neverd::gui {
namespace {

bool plainReturn(const QKeyEvent &key) {
  return (key.key() == Qt::Key_Return || key.key() == Qt::Key_Enter) &&
         (key.modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
}

class DialogReturnKey final : public QObject {
public:
  using QObject::QObject;

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    const auto *choices = qobject_cast<QComboBox *>(object);
    if (event->type() != QEvent::KeyPress ||
        !(qobject_cast<QCheckBox *>(object) ||
          qobject_cast<QRadioButton *>(object) ||
          (choices && !choices->isEditable())))
      return false;
    auto *dialog =
        qobject_cast<QDialog *>(static_cast<QWidget *>(object)->window());
    if (!plainReturn(*static_cast<QKeyEvent *>(event)) || !dialog)
      return false;
    // The dialog presses its default button, as for Return in a field.
    QCoreApplication::sendEvent(dialog, event);
    return true;
  }
};

class NoDefaultButton final : public QObject {
public:
  explicit NoDefaultButton(QDialog &dialog)
      : QObject(&dialog), dialog_(dialog) {
    dialog.installEventFilter(this);
    clear();
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (object != &dialog_)
      return false;
    // Before QDialog looks for a button to make the default.
    if (event->type() == QEvent::Show)
      clear();
    if (event->type() != QEvent::KeyPress ||
        !plainReturn(*static_cast<QKeyEvent *>(event)))
      return false;
    // Where the platform lets Return press a focused button, the key does not
    // come this far; elsewhere a button without autoDefault ignores it.
    QWidget *focus = dialog_.focusWidget();
    if (qobject_cast<QPushButton *>(focus) ||
        qobject_cast<QToolButton *>(focus))
      static_cast<QAbstractButton *>(focus)->click();
    event->accept();
    return true;
  }

private:
  void clear() {
    for (QPushButton *button : dialog_.findChildren<QPushButton *>()) {
      // An open child window keeps its own.
      if (button->window() != &dialog_)
        continue;
      button->setAutoDefault(false);
      button->setDefault(false);
    }
  }

  QDialog &dialog_;
};

} // namespace

void leaveWithoutDefaultButton(QDialog &dialog) { new NoDefaultButton(dialog); }

void showAsMainAction(QPushButton &button) {
  button.setProperty("mainAction", true);
  // The style sheet reads the property as it polishes the button.
  if (button.testAttribute(Qt::WA_WState_Polished)) {
    button.style()->unpolish(&button);
    button.style()->polish(&button);
  }
  button.update();
}

QObject *acceptDialogsOnReturn(QApplication &app) {
  auto *filter = new DialogReturnKey(&app);
  app.installEventFilter(filter);
  return filter;
}

} // namespace neverd::gui
