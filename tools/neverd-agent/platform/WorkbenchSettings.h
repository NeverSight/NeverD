//===- WorkbenchSettings.h - Settings shared with the workbench -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The workbench's interface language and theme, read from its own settings
/// so that the Agent, its CLI and the workbench chat present one product.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_WORKBENCHSETTINGS_H
#define NEVERD_TOOLS_AGENT_PLATFORM_WORKBENCHSETTINGS_H

#include <QString>
#include <QStringList>

namespace neverd::agent {

/// The interface language codes the workbench ships (its Languages.def),
/// English first.
const QStringList &interfaceLanguages();
/// \p code when it names a shipped interface language, else English ("en").
QString knownLanguage(const QString &code);
/// The workbench's saved interface language; English until one is chosen.
QString workbenchLanguage();
/// The workbench's saved theme choice: "dark", "light", "hc-dark",
/// "hc-light" or "system"; "dark" until one is chosen.
QString workbenchTheme();

} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_WORKBENCHSETTINGS_H
