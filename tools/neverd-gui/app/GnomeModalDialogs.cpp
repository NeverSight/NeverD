#include "GnomeModalDialogs.h"

#include <QApplication>
#include <QByteArrayList>
#include <QDialog>
#include <QEvent>
#include <QWindow>

namespace neverd::gui::gnome {
namespace {
constexpr char DesktopVariable[] = "XDG_CURRENT_DESKTOP";
constexpr char DesktopName[] = "gnome";
constexpr char DisabledWaylandInterfaces[] = "QT_WAYLAND_DISABLED_INTERFACES";
constexpr char DialogInterface[] = "xdg_wm_dialog_v1";
constexpr char X11Platform[] = "xcb";

class UtilityModalDialogs final : public QObject {
public:
  using QObject::QObject;

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    // A dialog's show event comes before its window is mapped, while the
    // window manager still reads the type it is given.  A dialog without a
    // parent window, such as the quick start greeting alone, has nothing to
    // be attached to and stays a dialog the window manager places and
    // focuses.
    if (event->type() == QEvent::Show)
      if (auto *dialog = qobject_cast<QDialog *>(object);
          dialog && dialog->isWindow())
        if (QWindow *window = dialog->windowHandle();
            window && window->type() == Qt::Dialog &&
            window->modality() != Qt::NonModal && window->transientParent())
          window->setFlags((window->flags() & ~Qt::WindowType_Mask) | Qt::Tool);
    return QObject::eventFilter(object, event);
  }
};
} // namespace

/// XDG_CURRENT_DESKTOP lists the desktop's names, such as "ubuntu:GNOME".
bool onGnome() {
  for (const QByteArray &name : qgetenv(DesktopVariable).split(':'))
    if (name.trimmed().toLower() == DesktopName)
      return true;
  return false;
}

void prepareModalDialogs() {
  if (!onGnome())
    return;
  QByteArray disabled = qgetenv(DisabledWaylandInterfaces);
  if (disabled.split(',').contains(DialogInterface))
    return;
  if (!disabled.isEmpty())
    disabled += ',';
  qputenv(DisabledWaylandInterfaces, disabled + DialogInterface);
}

void detachModalDialogs(QApplication &app) {
  if (onGnome() &&
      QGuiApplication::platformName() == QLatin1String(X11Platform))
    app.installEventFilter(new UtilityModalDialogs(&app));
}

} // namespace neverd::gui::gnome
