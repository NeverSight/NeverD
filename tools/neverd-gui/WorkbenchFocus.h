#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>

// Docked and floating panels share the same keyboard routing state.
class WorkbenchFocus final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QQuickItem *focusedItem READ focusedItem NOTIFY focusedItemChanged)
  Q_PROPERTY(
      QQuickWindow *focusWindow READ focusWindow NOTIFY focusWindowChanged)

public:
  explicit WorkbenchFocus(QObject *parent = nullptr);
  QQuickItem *focusedItem() const { return focusedItem_; }
  QQuickWindow *focusWindow() const { return focusWindow_; }

signals:
  void focusedItemChanged();
  void focusWindowChanged();

private:
  bool eventFilter(QObject *object, QEvent *event) override;
  void setFocusWindow(QWindow *window);
  void setFocusedItem(QQuickItem *item);

  QPointer<QQuickWindow> focusWindow_;
  QPointer<QQuickItem> focusedItem_;
  QMetaObject::Connection windowFocusConnection_;
  QMetaObject::Connection windowDestroyedConnection_;
  QMetaObject::Connection itemDestroyedConnection_;
};
