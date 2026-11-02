#include "DialogReturnKey.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QRadioButton>

namespace neverd::gui {
namespace {

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
    const auto *key = static_cast<QKeyEvent *>(event);
    auto *dialog =
        qobject_cast<QDialog *>(static_cast<QWidget *>(object)->window());
    if ((key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
        (key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier || !dialog)
      return false;
    // The dialog presses its default button, as for Return in a field.
    QCoreApplication::sendEvent(dialog, event);
    return true;
  }
};

} // namespace

QObject *acceptDialogsOnReturn(QApplication &app) {
  auto *filter = new DialogReturnKey(&app);
  app.installEventFilter(filter);
  return filter;
}

} // namespace neverd::gui
