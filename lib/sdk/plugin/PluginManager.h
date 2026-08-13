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

#ifndef NEVERD_SDK_PLUGIN_PLUGINMANAGER_H
#define NEVERD_SDK_PLUGIN_PLUGINMANAGER_H

#include "PluginRuntime.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

/// Discovers, loads, and manages NeverD plugins.
/// Each plugin is a shared library exporting a `neverd_plugin` symbol.
class PluginManager {
public:
  struct LoadedPlugin {
    std::string Path;
    std::unique_ptr<PluginRuntime> Runtime;
    bool Initialized = false;
    bool Terminated = false;

    const neverd_plugin_t &descriptor() const { return Runtime->descriptor(); }
  };

  ~PluginManager();

  int loadPluginsFromDir(const std::string &Dir);
  bool loadPluginFile(const std::string &Path);
  void initAll(neverd_session_t Session);
  void termAll();
  void dispatchEvent(const neverd_event_t &Event);
  int runPlugin(const std::string &Name, neverd_session_t Session, int Arg);

  const std::vector<LoadedPlugin> &plugins() const { return Plugins; }
  const std::string &lastError() const { return LastError; }

private:
  std::vector<LoadedPlugin> Plugins;
  std::string LastError;

  bool loadNativePlugin(const std::string &CanonicalPath);
  bool validateAndAdd(std::string CanonicalPath,
                      std::unique_ptr<PluginRuntime> Runtime);
  void setError(std::string Message) { LastError = std::move(Message); }
};

#endif // NEVERD_SDK_PLUGIN_PLUGINMANAGER_H
