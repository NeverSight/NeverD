//===- PythonPluginInterpreter.cpp - CPython lifecycle setup ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Process-wide CPython bring-up for the plugin host: one-shot interpreter
/// initialization, the sys.path entries that make the shipped Python SDK
/// importable, and loading a plugin file as a uniquely named module.
///
//===----------------------------------------------------------------------===//

#include "PythonPluginRuntimeDetail.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace neverd::sdk::python_plugin {

namespace {

std::once_flag InterpreterOnce;
std::string InterpreterError;

} // namespace

std::string initializeInterpreter() {
  std::call_once(InterpreterOnce, [] {
    if (Py_IsInitialized())
      return;

    PyConfig Config;
    PyConfig_InitPythonConfig(&Config);
    Config.parse_argv = 0;
    Config.install_signal_handlers = 0;
    Config.write_bytecode = 0;
    PyStatus Status = Py_InitializeFromConfig(&Config);
    if (PyStatus_Exception(Status)) {
      InterpreterError =
          Status.err_msg ? Status.err_msg : "CPython initialization failed";
      PyConfig_Clear(&Config);
      return;
    }
    PyConfig_Clear(&Config);
    (void)PyEval_SaveThread();
  });
  return InterpreterError;
}

namespace {

bool prependPythonPath(const fs::path &Path) {
  std::error_code EC;
  if (!fs::is_directory(Path, EC) || EC)
    return true;
  const std::string NativePath = Path.string();
  PyRef Item(PyUnicode_DecodeFSDefaultAndSize(
      NativePath.data(), static_cast<Py_ssize_t>(NativePath.size())));
  PyObject *SearchPath = PySys_GetObject("path"); // Borrowed.
  if (!Item || !SearchPath || !PyList_Check(SearchPath)) {
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_RuntimeError, "Python sys.path is unavailable");
    return false;
  }
  const int Contains = PySequence_Contains(SearchPath, Item.get());
  return Contains == 1 ||
         (Contains == 0 && PyList_Insert(SearchPath, 0, Item.get()) == 0);
}

} // namespace

bool addSDKPaths() {
  const std::string Library = currentLibraryPath();
  if (!Library.empty()) {
    const std::u8string UTF8Path(
        reinterpret_cast<const char8_t *>(Library.data()), Library.size());
    if (!prependPythonPath(fs::path(UTF8Path).parent_path() / "sdk" / "python"))
      return false;
  }
#ifdef NEVERD_PYTHON_SDK_SOURCE_DIR
  if (!prependPythonPath(fs::path(NEVERD_PYTHON_SDK_SOURCE_DIR)))
    return false;
#endif
  return true;
}

namespace {

std::atomic<uint64_t> NextModuleID{1};

} // namespace

std::string makeModuleName(const std::string &Path) {
  std::ostringstream Name;
  Name << "_neverd_user_plugin_" << std::hex << std::hash<std::string>{}(Path)
       << '_' << NextModuleID.fetch_add(1, std::memory_order_relaxed);
  return Name.str();
}

PyObject *importPluginModule(const std::string &Path,
                             const std::string &ModuleName) {
  PyRef Util(PyImport_ImportModule("importlib.util"));
  PyRef SpecFactory(
      Util ? PyObject_GetAttrString(Util.get(), "spec_from_file_location")
           : nullptr);
  PyRef ModuleFactory(
      Util ? PyObject_GetAttrString(Util.get(), "module_from_spec") : nullptr);
  PyRef Name(PyUnicode_FromString(ModuleName.c_str()));
  PyRef File(PyUnicode_DecodeFSDefaultAndSize(
      Path.data(), static_cast<Py_ssize_t>(Path.size())));
  PyRef Spec(SpecFactory
                 ? PyObject_CallFunctionObjArgs(SpecFactory.get(), Name.get(),
                                                File.get(), nullptr)
                 : nullptr);
  if (!Spec || Spec.get() == Py_None) {
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_ImportError,
                      "importlib could not create a Python plugin spec");
    return nullptr;
  }
  PyRef Module(ModuleFactory ? PyObject_CallFunctionObjArgs(ModuleFactory.get(),
                                                            Spec.get(), nullptr)
                             : nullptr);
  PyRef Loader(PyObject_GetAttrString(Spec.get(), "loader"));
  PyRef Execute(Loader ? PyObject_GetAttrString(Loader.get(), "exec_module")
                       : nullptr);
  if (!Module || !Execute || !PyCallable_Check(Execute.get())) {
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_ImportError,
                      "Python plugin spec has no executable loader");
    return nullptr;
  }

  PyObject *Modules = PyImport_GetModuleDict(); // Borrowed.
  if (!Modules ||
      PyDict_SetItemString(Modules, ModuleName.c_str(), Module.get()) < 0)
    return nullptr;
  PyRef Executed(
      PyObject_CallFunctionObjArgs(Execute.get(), Module.get(), nullptr));
  if (!Executed) {
    if (PyDict_DelItemString(Modules, ModuleName.c_str()) < 0)
      PyErr_Clear();
    return nullptr;
  }
  return Module.release();
}

} // namespace neverd::sdk::python_plugin
