//===- PythonPluginRuntime.h - Embedded Python plugin adapter -*- C++ -*-===//

#ifndef NEVERD_SDK_PLUGIN_PYTHONPLUGINRUNTIME_H
#define NEVERD_SDK_PLUGIN_PYTHONPLUGINRUNTIME_H

#include "PluginRuntime.h"

#include <memory>
#include <string>

std::unique_ptr<PluginRuntime>
loadPythonPluginRuntime(const std::string &CanonicalPath, std::string &Error);

#endif // NEVERD_SDK_PLUGIN_PYTHONPLUGINRUNTIME_H
