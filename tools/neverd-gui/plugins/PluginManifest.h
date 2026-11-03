//===- PluginManifest.h - External plugin declarations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External plugin declarations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_PLUGINS_PLUGINMANIFEST_H
#define NEVERD_TOOLS_NEVERD_GUI_PLUGINS_PLUGINMANIFEST_H

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <optional>

namespace neverd::plugin {
struct Manifest;
}

namespace neverd::gui {

struct GuiPlugin {
  QString id, name, version, path, program, icon;
  QStringList arguments;
  QJsonArray actions;
  QStringList queries;
};

/// Process plugins are separate from the worker's read-only contributions.
/// Validation is transactional: a rejected manifest changes no registration.
std::optional<GuiPlugin> readGuiPlugin(const QString &path, QString &error);
GuiPlugin guiPlugin(const neverd::plugin::Manifest &manifest);
bool validatePluginParameters(const QJsonArray &fields,
                              const QJsonObject &values, QString &error);
std::optional<QJsonObject>
resolvePluginParameters(const QJsonArray &fields, const QJsonObject &user,
                        const QJsonObject &project,
                        const QJsonObject &explicitValues, QString &error);
bool validatePluginResult(const QJsonObject &result, QString &error);

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_PLUGINS_PLUGINMANIFEST_H
