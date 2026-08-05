//===- PluginManager.h - Plugin loading and dispatch ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal C++ plugin manager.  Not part of the public SDK — third-party
/// plugin authors only need NeverDCAPI.h and NeverDPlugin.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGINMANAGER_H
#define NEVERD_PLUGINMANAGER_H

#include "neverd/sdk/NeverDPlugin.h"

#include <string>
#include <vector>

/// Discovers, loads, and manages NeverD plugins.
/// Each plugin is a shared library exporting a `neverd_plugin` symbol.
class PluginManager {
public:
  struct LoadedPlugin {
    std::string Path;
    neverd_plugin_t *Descriptor;
    void *Handle;
  };

  ~PluginManager();

  void loadPluginsFromDir(const std::string &Dir);
  void initAll(neverd_session_t Session);
  void termAll();
  void dispatchEvent(const neverd_event_t &Event);
  int runPlugin(const std::string &Name, neverd_session_t Session, int Arg);

  const std::vector<LoadedPlugin> &plugins() const { return Plugins; }

private:
  std::vector<LoadedPlugin> Plugins;

  bool loadPlugin(const std::string &Path);
};

#endif // NEVERD_PLUGINMANAGER_H
