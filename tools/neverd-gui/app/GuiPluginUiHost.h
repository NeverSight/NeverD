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
  GuiPluginUiHost(QWidget &workbench, Session &session,
                  std::function<QRect()> available, Navigate navigate);
  ~GuiPluginUiHost() override;
  bool contains(const QWidget *widget) const;
  QList<QAction *> menuActions(const QString &pluginId) const;

private:
  class Binding;
  bool eventFilter(QObject *object, QEvent *event) override;
  void scheduleLayout();
  void layoutViews();

  QWidget &Workbench;
  Session &Project;
  PluginJobs &Jobs;
  std::function<QRect()> Available;
  Navigate Jump;
  QList<Binding *> Bindings;
  bool LayoutQueued = false;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_GUIPLUGINUIHOST_H
