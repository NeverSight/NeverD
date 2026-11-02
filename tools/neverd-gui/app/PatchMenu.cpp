//===- PatchMenu.cpp - Fixed patch menu accelerators ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Populate menus only with actions admitted by the current engine.
//===----------------------------------------------------------------------===//
#include "PatchMenu.h"

#include "ActionRegistry.h"

#include <QAction>
#include <QKeyEvent>
#include <QMenu>
#include <QTimer>
#include <utility>
namespace neverd::gui {
namespace {
bool patchKey(int key) {
  switch (key) {
#define NEVERD_PATCH_ITEM(Id, Key, Kind) case Qt::Key_##Key:
#include "PatchMenu.def"
    return true;
  default:
    return false;
  }
}
} // namespace
PatchMenu::PatchMenu(QMenu *menu, std::function<void(QAction *)> dispatch)
    : QObject(menu), menu_(menu), dispatch_(std::move(dispatch)) {
  menu->setObjectName(QStringLiteral("patchMenu"));
  menu->setCursor(Qt::ArrowCursor);
  menu->installEventFilter(this);
  connect(menu, &QMenu::aboutToHide, this, [this] { pendingKey_ = 0; });
}
void PatchMenu::fill(ActionRegistry &registry, const QSet<QString> &kinds) {
  menu_->clear();
#define NEVERD_PATCH_ITEM(Id, Key, Kind)                                       \
  if (kinds.contains(QLatin1String(Kind))) {                                   \
    auto *source = registry.action(ActionId::Id);                              \
    auto *item = menu_->addAction(source->icon(), source->text() + "\t" #Key); \
    item->setObjectName(source->objectName());                                 \
    item->setEnabled(source->isEnabled());                                     \
    item->setProperty("patchKey", int(Qt::Key_##Key));                         \
    connect(item, &QAction::triggered, this, [this, source] {                  \
      if (dispatch_)                                                           \
        dispatch_(source);                                                     \
      else                                                                     \
        source->trigger();                                                     \
    });                                                                        \
  }
#include "PatchMenu.def"
  ready_ = true;
  if (pendingKey_) {
    const int key = pendingKey_;
    pendingKey_ = 0;
    trigger(key);
  }
}
void PatchMenu::popupAt(const QPoint &globalPosition) {
  popupPosition_ = globalPosition;
  menu_->popup(globalPosition);
}
bool PatchMenu::trigger(int key) {
  for (auto *item : menu_->actions())
    if (item->property("patchKey").toInt() == key && item->isEnabled()) {
      menu_->close();
      item->trigger();
      return true;
    }
  return false;
}
bool PatchMenu::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::Resize && popupPosition_ && menu_->isVisible())
    QTimer::singleShot(0, this, [this] {
      // The async result can be wider or taller than the loading row. Let Qt
      // place the final menu against the same caret and screen boundaries.
      if (menu_->isVisible() && popupPosition_)
        menu_->popup(*popupPosition_);
    });
  if (event->type() == QEvent::ShortcutOverride ||
      event->type() == QEvent::KeyPress) {
    const auto *key = static_cast<QKeyEvent *>(event);
    if ((key->modifiers() == Qt::NoModifier ||
         key->modifiers() == Qt::ShiftModifier) &&
        key->key() >= Qt::Key_A && key->key() <= Qt::Key_Z) {
      // QWindow dispatch checks shortcuts before delivering KeyPress. Without
      // this override J reopens the menu, and other letters reach view actions.
      event->accept();
      if (event->type() == QEvent::ShortcutOverride || key->isAutoRepeat())
        return true;
      if (!ready_) {
        if (!pendingKey_ && patchKey(key->key()))
          pendingKey_ = key->key();
      } else
        trigger(key->key());
      return true;
    }
  }
  return QObject::eventFilter(object, event);
}
} // namespace neverd::gui
