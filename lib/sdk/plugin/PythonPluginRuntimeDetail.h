//===- PythonPluginRuntimeDetail.h - Python host internals ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared by the embedded Python plugin host
/// translation units: interpreter lifecycle and module import
/// (PythonPluginInterpreter.cpp), the script-facing bridge module
/// (PythonPluginBridge.cpp), Python/C marshalling (PythonPluginMarshal.cpp),
/// exception translation (PythonPluginError.cpp), and the runtime adapter
/// itself (PythonPluginRuntime.cpp).
///
/// This header is an implementation detail of lib/sdk/plugin/ and must NOT be
/// included by code outside that directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_PLUGIN_PYTHONPLUGINRUNTIMEDETAIL_H
#define NEVERD_SDK_PLUGIN_PYTHONPLUGINRUNTIMEDETAIL_H

#define PY_SSIZE_T_CLEAN

#include "neverd/sdk/NeverDPlugin.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <Python.h>

#include <atomic>
#include <cstddef>
#include <string>

namespace neverd::sdk::python_plugin {

/// Capsule type name carrying the host session pointer into Python.
inline constexpr const char *SessionCapsuleName = "neverd_plugin.session.v1";

/// Owning reference to a Python object.
class PyRef {
public:
  PyRef() = default;
  explicit PyRef(PyObject *ObjectValue) : Object(ObjectValue) {}
  ~PyRef() { Py_XDECREF(Object); }

  PyRef(const PyRef &) = delete;
  PyRef &operator=(const PyRef &) = delete;

  PyRef(PyRef &&Other) noexcept : Object(Other.release()) {}
  PyRef &operator=(PyRef &&Other) noexcept {
    if (this != &Other) {
      Py_XDECREF(Object);
      Object = Other.release();
    }
    return *this;
  }

  PyObject *get() const { return Object; }
  explicit operator bool() const { return Object != nullptr; }
  PyObject *release() {
    PyObject *Result = Object;
    Object = nullptr;
    return Result;
  }

private:
  PyObject *Object = nullptr;
};

/// Scoped acquisition of the global interpreter lock.
class GILGuard {
public:
  GILGuard() : State(PyGILState_Ensure()) {}
  ~GILGuard() { PyGILState_Release(State); }

  GILGuard(const GILGuard &) = delete;
  GILGuard &operator=(const GILGuard &) = delete;

private:
  PyGILState_STATE State;
};

/// Session pointer plus the liveness flag the capsule destructor clears, so a
/// Python object outliving its host session fails loudly instead of dangling.
struct SessionControl {
  std::atomic<bool> Active{true};
  std::atomic<neverd_session_t> Session{nullptr};
};

// --- Exception translation (PythonPluginError.cpp) ---

/// Render the active Python exception, with its traceback when available.
std::string formatPythonException();

// --- Script-facing bridge module (PythonPluginBridge.cpp) ---

/// Capsule destructor: marks the session context dead and frees the control.
void destroySessionCapsule(PyObject *Capsule);

/// Absolute path of the loaded NeverD library, or empty when unresolvable.
std::string currentLibraryPath();

/// Publish the private `_neverd_plugin` bridge module into sys.modules.
bool installBridgeModule();

// --- Interpreter lifecycle and module import (PythonPluginInterpreter.cpp) ---

/// Initialize CPython once per process; returns an error string on failure.
std::string initializeInterpreter();

/// Prepend the shipped and in-tree Python SDK directories to sys.path.
bool addSDKPaths();

/// Build a process-unique module name for the plugin at \p Path.
std::string makeModuleName(const std::string &Path);

/// Import \p Path as \p ModuleName through importlib, returning a new ref.
PyObject *importPluginModule(const std::string &Path,
                             const std::string &ModuleName);

// --- Python/C marshalling (PythonPluginMarshal.cpp) ---

/// Read string attribute \p Field off \p Spec into \p Result.
bool metadataString(PyObject *Spec, const char *Field, bool AllowEmpty,
                    std::string &Result);

/// Decode \p Value as UTF-8, or return None when it is null.
PyObject *unicodeFromOptionalCString(const char *Value, const char *Field);

} // namespace neverd::sdk::python_plugin

#endif // NEVERD_SDK_PLUGIN_PYTHONPLUGINRUNTIMEDETAIL_H
