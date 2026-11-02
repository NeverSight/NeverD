//===- ModelessDialog.cpp - Browsers owned by their source pane -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Follow native window ownership without blocking input to analysis panes.
//===----------------------------------------------------------------------===//

#include "ModelessDialog.h"

#include "Session.h"

#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QEvent>
#include <QObject>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace neverd::gui {
namespace {
constexpr char OwnerName[] = "modelessDialogOwner";

void setTransientOwner(QWindow *window, QWindow *parent) {
  window->setTransientParent(parent);
#ifdef Q_OS_WIN
  // Qt 6.8 updates this property without changing an existing HWND's owner.
  // Synchronize it before a floating owner is destroyed, and again when the
  // pane reaches its destination. Offscreen windows do not have Win32 handles.
  if (QGuiApplication::platformName() == QStringLiteral("windows")) {
    const auto native = reinterpret_cast<HWND>(window->winId());
    const auto owner =
        parent ? reinterpret_cast<HWND>(parent->winId()) : nullptr;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(native, GWLP_HWNDPARENT,
                           reinterpret_cast<LONG_PTR>(owner))) {
      const auto error = GetLastError();
      if (error != ERROR_SUCCESS)
        qWarning() << "Unable to update modeless window owner:" << error;
    }
  }
#endif
}

class DialogOwner final : public QObject {
public:
  DialogOwner(QDialog *dialog, QWidget *source, Session *session)
      : QObject(dialog), Dialog(dialog), Source(source) {
    setObjectName(QLatin1String(OwnerName));
    if (session)
      connect(session, &Session::unloaded, this, [this] { Dialog->reject(); });
    for (auto *parent = source; parent; parent = parent->parentWidget())
      if (auto *dock =
              qobject_cast<KDDockWidgets::QtWidgets::DockWidget *>(parent)) {
        connect(dock, &KDDockWidgets::QtWidgets::DockWidget::isOpenChanged,
                this, [this](bool open) {
                  if (!open)
                    Dialog->reject();
                });
        break;
      }
    if (auto *parent = qobject_cast<QDialog *>(source))
      connect(parent, &QDialog::finished, this, [this] { Dialog->reject(); });
    qApp->installEventFilter(this);
  }

  ~DialogOwner() override { qApp->removeEventFilter(this); }

private:
  bool eventFilter(QObject *object, QEvent *event) override {
    // A floating native owner may be destroyed before the queued update.
    // Detach while the old ancestry still exists: Windows destroys owned
    // windows with their owner even when the QObject parent survives.
    if (event->type() == QEvent::ParentAboutToChange)
      if (auto *widget = qobject_cast<QWidget *>(object);
          widget && (widget == Source || widget->isAncestorOf(Source)))
        if (auto *window = Dialog->windowHandle())
          setTransientOwner(window, nullptr);
    // Wait until the destination window exists, including grouped docking.
    if (event->type() == QEvent::ParentChange || event->type() == QEvent::Show)
      if (auto *widget = qobject_cast<QWidget *>(object);
          widget && (widget == Source || widget->isAncestorOf(Source)))
        QTimer::singleShot(0, this, [this] {
          if (Dialog->isVisible())
            if (auto *window = Dialog->windowHandle())
              setTransientOwner(window, Source->window()->windowHandle());
        });
    return false;
  }

  QDialog *Dialog;
  QWidget *Source;
};
} // namespace

void showModelessDialog(QDialog *dialog, QWidget *source, Session *session) {
  if (auto *owner = dialog->findChild<QObject *>(QLatin1String(OwnerName),
                                                 Qt::FindDirectChildrenOnly))
    delete owner;
  if (!dialog->objectName().isEmpty())
    for (auto *previous : source->findChildren<QDialog *>(
             dialog->objectName(), Qt::FindDirectChildrenOnly))
      if (previous != dialog && previous->isVisible())
        previous->reject();
  if (dialog->parentWidget() != source)
    dialog->setParent(source, dialog->windowFlags());
  dialog->setWindowModality(Qt::NonModal);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  new DialogOwner(dialog, source, session);
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}
} // namespace neverd::gui
