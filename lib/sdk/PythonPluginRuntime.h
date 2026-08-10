//===- PythonPluginRuntime.h - Embedded Python plugin adapter -*- C++ -*-===//

#ifndef NEVERD_PYTHON_PLUGIN_RUNTIME_H
#define NEVERD_PYTHON_PLUGIN_RUNTIME_H

#include "PluginRuntime.h"

#include <memory>
#include <string>

std::unique_ptr<PluginRuntime>
loadPythonPluginRuntime(const std::string &CanonicalPath, std::string &Error);

#endif // NEVERD_PYTHON_PLUGIN_RUNTIME_H
