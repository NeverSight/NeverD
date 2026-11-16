//===- GuiPluginUiHost.h - Mount native plugin views ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_GUIPLUGINUIHOST_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_GUIPLUGINUIHOST_H

#include <QCoreApplication>
#include <QList>
#include <QObject>
#include <QRect>
#include <functional>

class QAction;
class QWidget;

namespace neverd::gui {
class PluginJobs;
class Session;

class GuiPluginUiHost final : public QObject {
  Q_DECLARE_TR_FUNCTIONS(neverd::gui::GuiPluginUiHost)

public:
  using Navigate = std::function<void(const QString &)>;
  /// Views float in the part of \p workbench that \p available gives, in the
  /// workbench's coordinates.
  GuiPluginUiHost(QWidget &workbench, Session &session,
                  std::function<QRect()> available, Navigate navigate);
  ~GuiPluginUiHost() override;
  bool contains(const QWidget *widget) const;
  QList<QAction *> menuActions(const QString &pluginId) const;
  /// Moves every view onto \p surface, another window the person works in,
  /// to float in the part \p available gives in the surface's coordinates.
  /// Its window takes the plugins' menu shortcuts too. Focus mode lends its
  /// workspace this way. The views come back with returnViews(), or by
  /// themselves when the surface is destroyed.
  void lendViews(QWidget &surface, std::function<QRect()> available);
  void returnViews();

private:
  class Binding;
  bool eventFilter(QObject *object, QEvent *event) override;
  void moveViews(QWidget &surface, std::function<QRect()> available);
  void scheduleLayout();
  void layoutViews();

  QWidget &Workbench;
  QWidget *Surface;
  Session &Project;
  PluginJobs &Jobs;
  std::function<QRect()> WorkbenchArea;
  std::function<QRect()> Available;
  QMetaObject::Connection SurfaceGone;
  Navigate Jump;
  QList<Binding *> Bindings;
  bool LayoutQueued = false;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_GUIPLUGINUIHOST_H
