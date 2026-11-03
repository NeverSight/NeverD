//===- ProcessTools.h - Process plugin tool catalog ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind an optional tools catalog to ordinary process plugin actions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_PROCESSTOOLS_H
#define NEVERD_TOOLS_PROCESSTOOLS_H

#include "neverd/plugin/PluginCatalog.h"
#include "neverd/tools/ToolRegistry.h"

namespace neverd::tools {
/// Component activation is atomic: any failed action/tool registration retires
/// only this attempted component. Independent components are untouched.
std::optional<plugin::Owner>
installProcessTools(plugin::PluginCatalog &Installations, ToolRegistry &Tools,
                    const plugin::Manifest &Plugin, const Json &Catalog,
                    plugin::Diagnostic &Error);
} // namespace neverd::tools

#endif // NEVERD_TOOLS_PROCESSTOOLS_H
