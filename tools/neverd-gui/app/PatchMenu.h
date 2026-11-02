//===- PatchMenu.h - Shared patch menu binding ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed second keys without application-wide shortcut collisions.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_PATCHMENU_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_PATCHMENU_H
#include <QObject>
#include <QPoint>
#include <QSet>
#include <QString>
#include <functional>
#include <optional>
class QAction;
class QMenu;
namespace neverd::gui {
class ActionRegistry;
class PatchMenu final : public QObject {
  Q_OBJECT
public:
  explicit PatchMenu(QMenu *menu, std::function<void(QAction *)> dispatch = {});
  void fill(ActionRegistry &registry, const QSet<QString> &kinds);
  void popupAt(const QPoint &globalPosition);

protected:
  bool eventFilter(QObject *, QEvent *) override;

private:
  QMenu *menu_;
  std::function<void(QAction *)> dispatch_;
  std::optional<QPoint> popupPosition_;
  int pendingKey_ = 0;
  bool ready_ = false;
  bool trigger(int key);
};
} // namespace neverd::gui
#endif // NEVERD_TOOLS_NEVERD_GUI_APP_PATCHMENU_H
