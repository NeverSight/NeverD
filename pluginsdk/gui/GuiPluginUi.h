//===- GuiPluginUi.h - Native workbench view extensions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGINSDK_GUI_GUIPLUGINUI_H
#define NEVERD_PLUGINSDK_GUI_GUIPLUGINUI_H

#include <QColor>
#include <QJsonObject>
#include <QString>
#include <QtPlugin>
#include <functional>

class QAction;
class QWidget;

namespace neverd::gui::pluginui {

struct Placement {
  Qt::Corner Corner = Qt::BottomRightCorner;
  QString Anchor;
};

/// Set this dynamic property on a mounted widget to request a QPoint top-left
/// in workbench coordinates, as when a person drags the view. The host
/// constrains the position to the workbench. An anchored view leaves its
/// anchor while the property is set, and views anchored to a moved view
/// follow it. Clear the property to return to the original placement.
inline constexpr char ViewPositionProperty[] = "neverdViewPosition";

/// Workbench-thread API, valid for one component activation. Qt and C++ ABI
/// must match the GUI distribution. This is separate from the engine C ABI.
class Context {
public:
  using Factory = std::function<QWidget *(QWidget *parent)>;
  using Observer = std::function<void(const QJsonObject &)>;

  /// The factory creates a child of the supplied parent. The host owns its
  /// destruction. Show/hide use Qt; close releases the mount.
  virtual QWidget *mountView(const QString &id, const Placement &placement,
                             Factory factory, QString &error) = 0;
  virtual QJsonObject captureContext() const = 0;
  virtual void watchContext(QObject *receiver, Observer observer) = 0;
  virtual QString attachSession(const QJsonObject &binding, QString &error) = 0;
  virtual bool navigate(const QString &address, const QJsonObject &binding) = 0;

protected:
  ~Context() = default;
};

/// Optional workbench menu capability. Query it with dynamic_cast from Context.
class MenuContext {
public:
  using ActionFactory = std::function<QAction *(QObject *parent)>;

  /// Mount a host-owned action under Plugins and the package's display name.
  /// The action belongs to this component activation.
  virtual bool mountMenuAction(const QString &id, ActionFactory factory,
                               QString &error) = 0;

protected:
  ~MenuContext() = default;
};

/// Optional workbench appearance capability. Query it with dynamic_cast from
/// Context. A view that styles or paints itself takes its colors from the
/// workbench's roles, named as in the workbench's chrome color table
/// ("Menu", "Border", "SecondaryText", "ButtonPrimary", ...) instead of
/// choosing its own, so it follows the light, dark and high contrast themes.
/// Standard Qt controls need nothing: the workbench's style already draws
/// them. The application palette changes with the theme, so a view reads the
/// roles again on QEvent::ApplicationPaletteChange.
///
/// A component that ships its own translations loads the catalog for
/// language() and loads it again on QEvent::LanguageChange, which the
/// workbench sends to every widget when its interface language changes.
class AppearanceContext {
public:
  /// The role's color in the current theme; invalid for an unknown role.
  virtual QColor themeColor(const QString &role) const = 0;
  /// The workbench's interface language: "en", "zh-CN", "ar", ...
  virtual QString language() const = 0;

protected:
  ~AppearanceContext() = default;
};

class Module {
public:
  /// Create instance state below lifetime; the loader's root object is a
  /// factory shared by all workbenches, not the owner of a project view.
  virtual bool activate(Context &context, QObject &lifetime,
                        QString &error) = 0;

protected:
  ~Module() = default;
};
} // namespace neverd::gui::pluginui

#define NEVERD_GUI_PLUGIN_UI_IID "org.neversight.NeverD.GuiPluginUi/1"
Q_DECLARE_INTERFACE(neverd::gui::pluginui::Module, NEVERD_GUI_PLUGIN_UI_IID)

#endif // NEVERD_PLUGINSDK_GUI_GUIPLUGINUI_H
