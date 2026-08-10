//===- PluginRuntime.h - Unified plugin runtime adapter ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_PLUGIN_RUNTIME_H
#define NEVERD_SDK_PLUGIN_RUNTIME_H

#include "neverd/sdk/NeverDPlugin.h"

#include <string>

/// Private lifecycle adapter shared by native and Python plugin hosts.
class PluginRuntime {
public:
  virtual ~PluginRuntime() = default;

  virtual const neverd_plugin_t &descriptor() const = 0;
  virtual const char *kind() const = 0;

  virtual int init(neverd_session_t Session) = 0;
  virtual void term() = 0;
  virtual int run(neverd_session_t Session, int Arg) = 0;
  virtual int event(const neverd_event_t &Event) = 0;

  /// Return the last language/runtime diagnostic, if one exists.
  virtual std::string lastError() const = 0;
};

#endif // NEVERD_SDK_PLUGIN_RUNTIME_H
