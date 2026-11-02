#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <array>
#include <functional>

class QAction;
class QMainWindow;
class QMenu;
class QMenuBar;
class QToolBar;
class QWidget;

namespace neverd::gui {

enum class ActionId : int {
#define NEVERD_ACTION(Id, Text, Shortcut, Icon, Tip) Id,
#include "Actions.def"
  Count
};

/// The workbench commands of Actions.def, their menus (Menus.def) and
/// toolbars (Toolbars.def).  Texts are retranslated in place.
class ActionRegistry final : public QObject {
  Q_OBJECT
public:
  explicit ActionRegistry(QObject *parent = nullptr);

  QAction *action(ActionId id) const { return actions_[static_cast<int>(id)]; }
  /// Commands whose shortcuts apply only inside analysis views.
  const QList<QAction *> &viewActions() const { return viewActions_; }
  const QList<QAction *> &allActions() const { return all_; }
  /// Attach the view-scoped shortcuts to \p view.
  void attachViewShortcuts(QWidget *view) const;
  /// Keep workbench shortcuts available when a dock becomes a top-level window.
  void attachWindowShortcuts(QWidget *view) const;

  /// Placeholder callback: fill \p menu with dynamic entries for \p name.
  using PlaceholderFiller = std::function<void(QMenu *menu, const QString &)>;
  void buildMenus(QMenuBar *bar, const PlaceholderFiller &fill);
  QList<QToolBar *> buildToolbars(QMainWindow *window);
  /// The same commands in one compact row for the focus window's caption.
  QToolBar *buildFocusToolbar(QWidget *parent) const;
  QMenu *menu(const QString &id) const { return menus_.value(id); }

  void retranslate();

private:
  struct Entry {
    const char *text;
    const char *tip;
  };
  std::array<QAction *, static_cast<int>(ActionId::Count)> actions_{};
  QList<QAction *> viewActions_, all_;
  QHash<QString, QMenu *> menus_;
  QHash<QMenu *, const char *> menuTexts_;
  QHash<QToolBar *, const char *> toolbarTitles_;
};

} // namespace neverd::gui
