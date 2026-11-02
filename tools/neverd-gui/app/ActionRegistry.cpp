#include "ActionRegistry.h"

#include "../Config.h"
#include "Icons.h"
#include "ShortcutHint.h"

#include <QAction>
#include <QCoreApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>

namespace neverd::gui {
namespace {
struct ActionSpec {
  const char *id;
  const char *text;
  const char *shortcut;
  const char *icon;
  const char *tip;
  bool viewScoped;
};
constexpr ActionSpec Specs[] = {
#define NEVERD_ACTION(Id, Text, Shortcut, Icon, Tip)                           \
  {#Id, Text, Shortcut, Icon, Tip, false},
#define NEVERD_VIEW_ACTION(Id, Text, Shortcut, Icon, Tip)                      \
  {#Id, Text, Shortcut, Icon, Tip, true},
#include "Actions.def"
};
static_assert(std::size(Specs) == static_cast<std::size_t>(ActionId::Count));

QString translated(const char *context, const char *text) {
  return QCoreApplication::translate(context, text);
}
} // namespace

ActionRegistry::ActionRegistry(QObject *parent) : QObject(parent) {
  for (std::size_t i = 0; i < std::size(Specs); ++i) {
    const auto &spec = Specs[i];
    auto *action = new QAction(this);
    action->setObjectName(QString::fromLatin1(spec.id));
    action->setIcon(icon(QString::fromLatin1(spec.icon)));
    action->setShortcutVisibleInContextMenu(true);
    if (*spec.shortcut)
      action->setShortcuts(QKeySequence::listFromString(
          QString::fromLatin1(spec.shortcut), QKeySequence::PortableText));
    if (spec.viewScoped) {
      action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
      viewActions_.append(action);
    }
    actions_[i] = action;
    all_.append(action);
  }
  retranslate();
}

void ActionRegistry::attachViewShortcuts(QWidget *view) const {
  view->addActions(viewActions_);
}

void ActionRegistry::attachWindowShortcuts(QWidget *view) const {
  for (auto *action : all_)
    if (action->shortcutContext() == Qt::WindowShortcut)
      view->addAction(action);
}

void ActionRegistry::buildMenus(QMenuBar *bar, const PlaceholderFiller &fill) {
  const auto actionByName = [this](const char *name) -> QAction * {
    for (std::size_t i = 0; i < std::size(Specs); ++i)
      if (qstrcmp(Specs[i].id, name) == 0)
        return actions_[i];
    return nullptr;
  };
  const auto makeMenu = [&](const char *id, const char *parent,
                            const char *text) {
    QMenu *menu = nullptr;
    if (qstrcmp(parent, "None") == 0)
      menu = bar->addMenu(QString());
    else if (auto *owner = menus_.value(QString::fromLatin1(parent)))
      menu = owner->addMenu(QString());
    if (!menu)
      return;
    menu->setObjectName(QStringLiteral("menu") + QString::fromLatin1(id));
    menus_.insert(QString::fromLatin1(id), menu);
    menuTexts_.insert(menu, text);
  };
  const auto addItem = [&](const char *menuId, const char *actionId) {
    if (auto *menu = menus_.value(QString::fromLatin1(menuId)))
      if (auto *action = actionByName(actionId))
        menu->addAction(action);
  };
  const auto addSeparator = [&](const char *menuId) {
    if (auto *menu = menus_.value(QString::fromLatin1(menuId)))
      menu->addSeparator();
  };
  const auto addPlaceholder = [&](const char *menuId, const char *name) {
    if (auto *menu = menus_.value(QString::fromLatin1(menuId)))
      fill(menu, QString::fromLatin1(name));
  };
  const auto setMenuIcon = [&](const char *menuId, const char *name) {
    if (auto *menu = menus_.value(QString::fromLatin1(menuId)))
      menu->setIcon(icon(QString::fromLatin1(name)));
  };
#define NEVERD_MENU(Id, Parent, Text) makeMenu(#Id, #Parent, Text);
#define NEVERD_MENU_ITEM(Menu, Action) addItem(#Menu, #Action);
#define NEVERD_MENU_SEPARATOR(Menu) addSeparator(#Menu);
#define NEVERD_MENU_PLACEHOLDER(Menu, Name) addPlaceholder(#Menu, #Name);
#define NEVERD_MENU_ICON(Menu, Icon) setMenuIcon(#Menu, Icon);
#include "Menus.def"
  retranslate();
}

QList<QToolBar *> ActionRegistry::buildToolbars(QMainWindow *window) {
  QList<QToolBar *> result;
  QHash<QString, QToolBar *> byId;
  const auto actionByName = [this](const char *name) -> QAction * {
    for (std::size_t i = 0; i < std::size(Specs); ++i)
      if (qstrcmp(Specs[i].id, name) == 0)
        return actions_[i];
    return nullptr;
  };
  const auto makeToolbar = [&](const char *id, const char *title) {
    auto *toolbar = new QToolBar(window);
    toolbar->setObjectName(QStringLiteral("toolbar") + QString::fromLatin1(id));
    toolbar->setIconSize(QSize(16, 16));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setMovable(true);
    window->addToolBar(Qt::TopToolBarArea, toolbar);
    byId.insert(QString::fromLatin1(id), toolbar);
    toolbarTitles_.insert(toolbar, title);
    result.append(toolbar);
  };
  const auto addItem = [&](const char *toolbarId, const char *actionId) {
    if (auto *toolbar = byId.value(QString::fromLatin1(toolbarId)))
      if (auto *action = actionByName(actionId))
        toolbar->addAction(action);
  };
  const auto addSeparator = [&](const char *toolbarId) {
    if (auto *toolbar = byId.value(QString::fromLatin1(toolbarId)))
      toolbar->addSeparator();
  };
#define NEVERD_TOOLBAR(Id, Title) makeToolbar(#Id, Title);
#define NEVERD_TOOLBAR_ITEM(Toolbar, Action) addItem(#Toolbar, #Action);
#define NEVERD_TOOLBAR_SEPARATOR(Toolbar) addSeparator(#Toolbar);
#include "Toolbars.def"
  retranslate();
  return result;
}

QToolBar *ActionRegistry::buildFocusToolbar(QWidget *parent) const {
  auto *toolbar = new QToolBar(parent);
  toolbar->setObjectName(QStringLiteral("focusToolbar"));
  toolbar->setIconSize(
      QSize(::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize,
            ::neverd::limits::gui_config::kWorkbenchStyleSmallIconSize));
  toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  toolbar->setFixedHeight(::neverd::limits::gui_config::kTitleBarHeight);
  const auto *focus = action(ActionId::ViewFocusMode);
  const auto describe = [toolbar, focus] {
    QString title = focus->text();
    title.remove(QLatin1Char('&'));
    toolbar->setAccessibleName(title);
  };
  connect(focus, &QAction::changed, toolbar, describe);
  describe();
#define NEVERD_TOOLBAR(Id, Title)                                              \
  if (!toolbar->actions().isEmpty())                                           \
    toolbar->addSeparator();
#define NEVERD_TOOLBAR_ITEM(Toolbar, Action)                                   \
  toolbar->addAction(action(ActionId::Action));
#define NEVERD_TOOLBAR_SEPARATOR(Toolbar) toolbar->addSeparator();
#include "Toolbars.def"
  return toolbar;
}

void ActionRegistry::retranslate() {
  for (std::size_t i = 0; i < std::size(Specs); ++i) {
    auto *action = actions_[i];
    const QString text = translated("Actions", Specs[i].text);
    action->setText(text);
    QString tip = translated("Actions", Specs[i].tip);
    action->setStatusTip(tip);
    QString plain = text;
    plain.remove(QLatin1Char('&'));
    if (plain.endsWith(QStringLiteral("...")))
      plain.chop(3);
    const QString keys = shortcutHint(action->shortcuts());
    action->setToolTip(
        keys.isEmpty() ? plain : QStringLiteral("%1 (%2)").arg(plain, keys));
  }
  for (auto it = menuTexts_.cbegin(); it != menuTexts_.cend(); ++it)
    it.key()->setTitle(translated("Menus", it.value()));
  for (auto it = toolbarTitles_.cbegin(); it != toolbarTitles_.cend(); ++it)
    it.key()->setWindowTitle(translated("Toolbars", it.value()));
}

} // namespace neverd::gui
