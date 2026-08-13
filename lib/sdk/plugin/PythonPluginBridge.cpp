//===- PythonPluginBridge.cpp - Script-facing bridge module ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The private `_neverd_plugin` extension module NeverD injects into the
/// embedded interpreter.  It is the whole surface Python plugins may reach the
/// host through: a validated session address, the loaded library path, and the
/// native plugin ABI sizes and offsets.
///
//===----------------------------------------------------------------------===//

#include "PythonPluginRuntimeDetail.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace neverd::sdk::python_plugin {

namespace {

constexpr const char *BridgeModuleName = "_neverd_plugin";

SessionControl *checkedSessionControl(PyObject *Capsule) {
  auto *Control = static_cast<SessionControl *>(
      PyCapsule_GetPointer(Capsule, SessionCapsuleName));
  if (!Control)
    return nullptr;
  if (!Control->Active.load(std::memory_order_acquire) ||
      !Control->Session.load(std::memory_order_acquire)) {
    PyErr_SetString(PyExc_RuntimeError,
                    "NeverD session context is no longer active");
    return nullptr;
  }
  return Control;
}

} // namespace

void destroySessionCapsule(PyObject *Capsule) {
  auto *Control = static_cast<SessionControl *>(
      PyCapsule_GetPointer(Capsule, SessionCapsuleName));
  if (!Control) {
    PyErr_Clear();
    return;
  }
  Control->Active.store(false, std::memory_order_release);
  Control->Session.store(nullptr, std::memory_order_release);
  delete Control;
}

namespace {

PyObject *sessionAddress(PyObject *, PyObject *Capsule) {
  SessionControl *Control = checkedSessionControl(Capsule);
  if (!Control)
    return nullptr;
  return PyLong_FromVoidPtr(Control->Session.load(std::memory_order_acquire));
}

#ifdef _WIN32
std::string utf8FromWide(const wchar_t *Value, int Length) {
  const int Required = WideCharToMultiByte(CP_UTF8, 0, Value, Length, nullptr,
                                           0, nullptr, nullptr);
  if (Required <= 0)
    return {};
  std::string Result(static_cast<size_t>(Required), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, Value, Length, Result.data(), Required,
                          nullptr, nullptr) != Required)
    return {};
  return Result;
}
#endif

extern PyModuleDef BridgeModule;

} // namespace

std::string currentLibraryPath() {
#ifdef _WIN32
  HMODULE Module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&BridgeModule), &Module))
    return {};
  std::wstring Buffer(32768, L'\0');
  DWORD Length = GetModuleFileNameW(Module, Buffer.data(),
                                    static_cast<DWORD>(Buffer.size()));
  if (Length == 0 || Length >= Buffer.size())
    return {};
  return utf8FromWide(Buffer.data(), static_cast<int>(Length));
#else
  Dl_info Info{};
  if (dladdr(static_cast<void *>(&BridgeModule), &Info) == 0 || !Info.dli_fname)
    return {};
  std::error_code EC;
  fs::path Canonical = fs::canonical(fs::path(Info.dli_fname), EC);
  return EC ? std::string(Info.dli_fname) : Canonical.string();
#endif
}

namespace {

PyObject *libraryPath(PyObject *, PyObject *) {
  const std::string Path = currentLibraryPath();
  if (Path.empty()) {
    PyErr_SetString(PyExc_RuntimeError,
                    "cannot resolve the loaded NeverD library path");
    return nullptr;
  }
  return PyUnicode_DecodeUTF8(Path.data(), static_cast<Py_ssize_t>(Path.size()),
                              "strict");
}

bool dictionarySetSize(PyObject *Dictionary, const char *Name, size_t Value) {
  PyRef Number(PyLong_FromSize_t(Value));
  return Number && PyDict_SetItemString(Dictionary, Name, Number.get()) == 0;
}

PyObject *abiLayout(PyObject *, PyObject *) {
  PyRef Result(PyDict_New());
  if (!Result ||
      !dictionarySetSize(Result.get(), "session_handle_size",
                         sizeof(neverd_session_t)) ||
      !dictionarySetSize(Result.get(), "virtual_address_size",
                         sizeof(neverd_va_t)) ||
      !dictionarySetSize(Result.get(), "event_size", sizeof(neverd_event_t)) ||
      !dictionarySetSize(Result.get(), "event_type_offset",
                         offsetof(neverd_event_t, Type)) ||
      !dictionarySetSize(Result.get(), "event_session_offset",
                         offsetof(neverd_event_t, Session)) ||
      !dictionarySetSize(Result.get(), "event_data_offset",
                         offsetof(neverd_event_t, Data)) ||
      !dictionarySetSize(Result.get(), "plugin_size",
                         sizeof(neverd_plugin_t)) ||
      !dictionarySetSize(Result.get(), "plugin_name_offset",
                         offsetof(neverd_plugin_t, Name)) ||
      !dictionarySetSize(Result.get(), "plugin_version_offset",
                         offsetof(neverd_plugin_t, Version)) ||
      !dictionarySetSize(Result.get(), "plugin_type_offset",
                         offsetof(neverd_plugin_t, Type)) ||
      !dictionarySetSize(Result.get(), "plugin_init_offset",
                         offsetof(neverd_plugin_t, Init)) ||
      !dictionarySetSize(Result.get(), "plugin_event_offset",
                         offsetof(neverd_plugin_t, Event)))
    return nullptr;
  return Result.release();
}

PyMethodDef BridgeMethods[] = {
    {"session_address", sessionAddress, METH_O,
     "Return a validated host session address."},
    {"library_path", libraryPath, METH_NOARGS,
     "Return the loaded libneverd path."},
    {"abi_layout", abiLayout, METH_NOARGS,
     "Return native plugin ABI sizes and offsets."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef BridgeModule = {
    PyModuleDef_HEAD_INIT,
    BridgeModuleName,
    "Private in-process bridge for NeverD Python plugins.",
    -1,
    BridgeMethods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

} // namespace

bool installBridgeModule() {
  PyObject *Modules = PyImport_GetModuleDict(); // Borrowed.
  if (!Modules)
    return false;
  PyObject *Existing =
      PyDict_GetItemString(Modules, BridgeModuleName); // Borrowed.
  if (Existing) {
    int IsBridge = PyObject_HasAttrString(Existing, "_neverd_bridge_v1");
    if (IsBridge == 1)
      return true;
    if (IsBridge < 0)
      return false;
    PyErr_SetString(PyExc_ImportError,
                    "sys.modules contains a foreign '_neverd_plugin' module");
    return false;
  }

  PyRef Module(PyModule_Create(&BridgeModule));
  return Module &&
         PyModule_AddIntConstant(Module.get(), "_neverd_bridge_v1", 1) == 0 &&
         PyDict_SetItemString(Modules, BridgeModuleName, Module.get()) == 0;
}

} // namespace neverd::sdk::python_plugin
