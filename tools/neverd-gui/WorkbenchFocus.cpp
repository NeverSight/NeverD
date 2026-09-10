#include "WorkbenchFocus.h"

#include <QEvent>
#include <QGuiApplication>
#include <QVariant>

WorkbenchFocus::WorkbenchFocus(QObject *parent) : QObject(parent) {
  connect(qGuiApp, &QGuiApplication::focusWindowChanged, this,
          &WorkbenchFocus::setFocusWindow);
  setFocusWindow(QGuiApplication::focusWindow());
}

void WorkbenchFocus::setFocusWindow(QWindow *window) {
  auto *quickWindow = qobject_cast<QQuickWindow *>(window);
  if (focusWindow_ == quickWindow) {
    setFocusedItem(quickWindow ? quickWindow->activeFocusItem() : nullptr);
    return;
  }
  disconnect(windowFocusConnection_);
  disconnect(windowDestroyedConnection_);
  focusWindow_ = quickWindow;
  if (quickWindow) {
    windowFocusConnection_ = connect(
        quickWindow, &QQuickWindow::activeFocusItemChanged, this, [this] {
          setFocusedItem(focusWindow_ ? focusWindow_->activeFocusItem()
                                      : nullptr);
        });
    windowDestroyedConnection_ =
        connect(quickWindow, &QObject::destroyed, this, [this] {
          focusWindow_ = nullptr;
          setFocusedItem(nullptr);
          emit focusWindowChanged();
        });
  }
  setFocusedItem(quickWindow ? quickWindow->activeFocusItem() : nullptr);
  emit focusWindowChanged();
}

void WorkbenchFocus::setFocusedItem(QQuickItem *item) {
  if (focusedItem_ == item)
    return;
  disconnect(itemDestroyedConnection_);
  if (focusedItem_)
    focusedItem_->removeEventFilter(this);
  focusedItem_ = item;
  if (item) {
    item->installEventFilter(this);
    itemDestroyedConnection_ = connect(item, &QObject::destroyed, this, [this] {
      focusedItem_ = nullptr;
      emit focusedItemChanged();
    });
  }
  emit focusedItemChanged();
}

bool WorkbenchFocus::eventFilter(QObject *object, QEvent *event) {
  if (object == focusedItem_ && event->type() == QEvent::ShortcutOverride &&
      object->inherits("QQuickTextEdit") &&
      object->property("readOnly").toBool()) {
    // Qt 6.8's delivery agent pre-accepts this event. A read-only TextEdit
    // preserves that flag and returns before its base class can offer the
    // shortcut to the application. Continue normal dispatch with no claim;
    // attached Keys handlers may still explicitly accept the override.
    event->ignore();
  }
  return QObject::eventFilter(object, event);
}
