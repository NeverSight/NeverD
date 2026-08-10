//===- PythonPluginRuntime.cpp - Embedded Python plugin adapter -*- C++ -*-===//

#define PY_SSIZE_T_CLEAN
#include "PythonPluginRuntime.h"

#include "neverd/sdk/NeverDPlugin.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <Python.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr const char *BridgeModuleName = "_neverd_plugin";
constexpr const char *SessionCapsuleName = "neverd_plugin.session.v1";

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

class GILGuard {
public:
  GILGuard() : State(PyGILState_Ensure()) {}
  ~GILGuard() { PyGILState_Release(State); }

  GILGuard(const GILGuard &) = delete;
  GILGuard &operator=(const GILGuard &) = delete;

private:
  PyGILState_STATE State;
};

std::string fallbackPythonException(PyObject *Value) {
  if (!Value)
    return "unknown Python exception";
  PyRef Text(PyObject_Str(Value));
  if (!Text) {
    PyErr_Clear();
    return "unprintable Python exception";
  }
  Py_ssize_t Length = 0;
  const char *Data = PyUnicode_AsUTF8AndSize(Text.get(), &Length);
  if (!Data) {
    PyErr_Clear();
    return "unprintable Python exception";
  }
  return std::string(Data, static_cast<size_t>(Length));
}

std::string formatPythonException() {
  if (!PyErr_Occurred())
    return "unknown Python exception";

  PyObject *RawType = nullptr;
  PyObject *RawValue = nullptr;
  PyObject *RawTraceback = nullptr;
  PyErr_Fetch(&RawType, &RawValue, &RawTraceback);
  PyErr_NormalizeException(&RawType, &RawValue, &RawTraceback);
  PyRef Type(RawType);
  PyRef Value(RawValue);
  PyRef Traceback(RawTraceback);

  PyRef TracebackModule(PyImport_ImportModule("traceback"));
  PyRef Formatter(
      TracebackModule
          ? PyObject_GetAttrString(TracebackModule.get(), "format_exception")
          : nullptr);
  PyObject *TracebackArgument = Traceback ? Traceback.get() : Py_None;
  PyRef Lines(Formatter ? PyObject_CallFunctionObjArgs(
                              Formatter.get(), Type ? Type.get() : Py_None,
                              Value ? Value.get() : Py_None, TracebackArgument,
                              nullptr)
                        : nullptr);
  PyRef Separator(Lines ? PyUnicode_FromString("") : nullptr);
  PyRef Joined(Separator ? PyUnicode_Join(Separator.get(), Lines.get())
                         : nullptr);
  if (!Joined) {
    PyErr_Clear();
    return fallbackPythonException(Value.get());
  }
  Py_ssize_t Length = 0;
  const char *Data = PyUnicode_AsUTF8AndSize(Joined.get(), &Length);
  if (!Data) {
    PyErr_Clear();
    return fallbackPythonException(Value.get());
  }
  return std::string(Data, static_cast<size_t>(Length));
}

struct SessionControl {
  std::atomic<bool> Active{true};
  std::atomic<neverd_session_t> Session{nullptr};
};

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

std::once_flag InterpreterOnce;
std::string InterpreterError;

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

std::atomic<uint64_t> NextModuleID{1};

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

bool metadataString(PyObject *Spec, const char *Field, bool AllowEmpty,
                    std::string &Result) {
  PyRef Value(PyObject_GetAttrString(Spec, Field));
  if (!Value || !PyUnicode_Check(Value.get())) {
    if (!PyErr_Occurred())
      PyErr_Format(PyExc_TypeError, "plugin metadata '%s' must be a string",
                   Field);
    return false;
  }
  Py_ssize_t Length = 0;
  const char *Data = PyUnicode_AsUTF8AndSize(Value.get(), &Length);
  if (!Data)
    return false;
  Result.assign(Data, static_cast<size_t>(Length));
  if ((!AllowEmpty && Result.empty()) ||
      Result.find('\0') != std::string::npos) {
    PyErr_Format(PyExc_ValueError,
                 "plugin metadata '%s' must be non-empty and NUL-free", Field);
    return false;
  }
  return true;
}

PyObject *unicodeFromOptionalCString(const char *Value, const char *Field) {
  if (!Value)
    return Py_NewRef(Py_None);
  PyObject *Result = PyUnicode_DecodeUTF8(
      Value, static_cast<Py_ssize_t>(strlen(Value)), "strict");
  if (!Result && PyErr_ExceptionMatches(PyExc_UnicodeDecodeError)) {
    PyErr_Clear();
    PyErr_Format(PyExc_ValueError, "event %s is not valid UTF-8", Field);
  }
  return Result;
}

class PythonPluginRuntime final : public PluginRuntime {
public:
  PythonPluginRuntime(std::string ModuleNameValue, PyObject *ModuleValue,
                      PyObject *APIModuleValue, PyObject *InstanceValue,
                      std::string NameValue, std::string VersionValue,
                      std::string AuthorValue, std::string DescriptionValue,
                      neverd_plugin_type_t TypeValue)
      : ModuleName(std::move(ModuleNameValue)), Module(ModuleValue),
        APIModule(APIModuleValue), Instance(InstanceValue),
        Name(std::move(NameValue)), Version(std::move(VersionValue)),
        Author(std::move(AuthorValue)),
        Description(std::move(DescriptionValue)) {
    Descriptor.Name = Name.c_str();
    Descriptor.Version = Version.c_str();
    Descriptor.Author = Author.c_str();
    Descriptor.Description = Description.c_str();
    Descriptor.Type = TypeValue;
    Descriptor.Init = nullptr;
    Descriptor.Term = nullptr;
    Descriptor.Run = nullptr;
    Descriptor.Event = nullptr;
  }

  ~PythonPluginRuntime() override {
    if (!Py_IsInitialized())
      return;
    GILGuard Guard;
    invalidateSession();
    Py_CLEAR(SessionObject);
    Py_CLEAR(SessionCapsule);
    Py_CLEAR(Instance);
    Py_CLEAR(APIModule);
    PyObject *Modules = PyImport_GetModuleDict(); // Borrowed.
    if (Modules && !ModuleName.empty() &&
        PyDict_DelItemString(Modules, ModuleName.c_str()) < 0)
      PyErr_Clear();
    Py_CLEAR(Module);
  }

  const neverd_plugin_t &descriptor() const override { return Descriptor; }
  const char *kind() const override { return "python"; }

  int init(neverd_session_t Session) override {
    GILGuard Guard;
    clearLastError();
    if (!ensureSession(Session))
      return pythonFailure("on_init");
    return invokeHook("on_init", SessionObject, nullptr);
  }

  void term() override {
    if (!Py_IsInitialized() || Terminated)
      return;
    GILGuard Guard;
    clearLastError();
    invalidateSession();
    (void)invokeHook("on_term", nullptr, nullptr, true);
    Terminated = true;
  }

  int run(neverd_session_t Session, int Arg) override {
    GILGuard Guard;
    clearLastError();
    if (!ensureSession(Session))
      return pythonFailure("on_run");
    PyRef Argument(PyLong_FromLong(Arg));
    return Argument ? invokeHook("on_run", SessionObject, Argument.get())
                    : pythonFailure("on_run");
  }

  int event(const neverd_event_t &Event) override {
    GILGuard Guard;
    clearLastError();
    if (!ensureSession(Event.Session))
      return pythonFailure("on_event");
    PyRef EventObject(createEvent(Event));
    if (!EventObject)
      return pythonFailure("on_event");
    return invokeHook("on_event", EventObject.get(), nullptr);
  }

  std::string lastError() const override {
    std::lock_guard<std::mutex> Lock(ErrorMutex);
    return LastError;
  }

private:
  bool ensureSession(neverd_session_t Session) {
    if (!Session) {
      PyErr_SetString(PyExc_RuntimeError,
                      "Python plugin callback has no NeverD session");
      return false;
    }
    if (Control) {
      if (!Control->Active.load(std::memory_order_acquire) ||
          Control->Session.load(std::memory_order_acquire) != Session) {
        PyErr_SetString(PyExc_RuntimeError,
                        "Python plugin session context is no longer active");
        return false;
      }
      return true;
    }

    auto *NewControl = new SessionControl();
    NewControl->Session.store(Session, std::memory_order_release);
    PyRef Capsule(
        PyCapsule_New(NewControl, SessionCapsuleName, destroySessionCapsule));
    if (!Capsule) {
      delete NewControl;
      return false;
    }
    PyRef SessionClass(PyObject_GetAttrString(APIModule, "Session"));
    PyRef NewSession(SessionClass && Capsule
                         ? PyObject_CallFunctionObjArgs(SessionClass.get(),
                                                        Capsule.get(), nullptr)
                         : nullptr);
    if (!NewSession)
      return false;
    Control = NewControl;
    SessionCapsule = Capsule.release();
    SessionObject = NewSession.release();
    return true;
  }

  void invalidateSession() {
    if (!Control)
      return;
    Control->Active.store(false, std::memory_order_release);
    Control->Session.store(nullptr, std::memory_order_release);
    Control = nullptr;
  }

  PyObject *createEvent(const neverd_event_t &Event) {
    PyRef EventClass(PyObject_GetAttrString(APIModule, "Event"));
    PyRef Factory(EventClass
                      ? PyObject_GetAttrString(EventClass.get(), "from_host")
                      : nullptr);
    PyRef Type(PyLong_FromLong(static_cast<long>(Event.Type)));
    PyRef Arguments(Type ? PyTuple_Pack(2, SessionObject, Type.get())
                         : nullptr);
    PyRef Keywords(PyDict_New());
    if (!Factory || !Arguments || !Keywords)
      return nullptr;

    auto SetKeyword = [&](const char *Name, PyObject *Value) {
      if (!Value)
        return false;
      const int Result = PyDict_SetItemString(Keywords.get(), Name, Value);
      Py_DECREF(Value);
      return Result == 0;
    };

    switch (Event.Type) {
    case NEVERD_EVT_BINARY_LOADED:
      if (!SetKeyword("path", unicodeFromOptionalCString(
                                  Event.Data.BinaryLoaded.Path, "path")))
        return nullptr;
      break;
    case NEVERD_EVT_FUNC_SELECTED:
      if (!SetKeyword("address", PyLong_FromUnsignedLongLong(
                                     Event.Data.FuncSelected.Addr)) ||
          !SetKeyword("name",
                      unicodeFromOptionalCString(Event.Data.FuncSelected.Name,
                                                 "function name")))
        return nullptr;
      break;
    case NEVERD_EVT_ADDR_CHANGED:
      if (!SetKeyword("address",
                      PyLong_FromUnsignedLongLong(Event.Data.AddrChanged.Addr)))
        return nullptr;
      break;
    case NEVERD_EVT_PATCH_APPLIED:
      if (!SetKeyword("output_path",
                      unicodeFromOptionalCString(
                          Event.Data.PatchApplied.OutputPath, "output path")) ||
          !SetKeyword("code_size",
                      PyLong_FromLong(Event.Data.PatchApplied.CodeSize)))
        return nullptr;
      break;
    case NEVERD_EVT_BINARY_CLOSING:
    case NEVERD_EVT_ANALYSIS_DONE:
      break;
    default:
      PyErr_Format(PyExc_ValueError, "unknown NeverD event type: %d",
                   static_cast<int>(Event.Type));
      return nullptr;
    }
    return PyObject_Call(Factory.get(), Arguments.get(), Keywords.get());
  }

  int invokeHook(const char *NameValue, PyObject *First, PyObject *Second,
                 bool RequireNone = false) {
    PyRef Method(PyObject_GetAttrString(Instance, NameValue));
    if (!Method) {
      if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return 0;
      }
      return pythonFailure(NameValue);
    }
    if (!PyCallable_Check(Method.get())) {
      PyErr_Format(PyExc_TypeError, "plugin attribute '%s' is not callable",
                   NameValue);
      return pythonFailure(NameValue);
    }

    PyRef Result;
    if (First && Second)
      Result = PyRef(
          PyObject_CallFunctionObjArgs(Method.get(), First, Second, nullptr));
    else if (First)
      Result =
          PyRef(PyObject_CallFunctionObjArgs(Method.get(), First, nullptr));
    else
      Result = PyRef(PyObject_CallNoArgs(Method.get()));
    if (!Result)
      return pythonFailure(NameValue);
    if (Result.get() == Py_None)
      return 0;
    if (RequireNone) {
      PyErr_Format(PyExc_TypeError, "plugin hook '%s' must return None",
                   NameValue);
      return pythonFailure(NameValue);
    }
    if (!PyLong_Check(Result.get()) || PyBool_Check(Result.get())) {
      PyErr_Format(PyExc_TypeError, "plugin hook '%s' must return int or None",
                   NameValue);
      return pythonFailure(NameValue);
    }
    const long long Value = PyLong_AsLongLong(Result.get());
    if (Value == -1 && PyErr_Occurred())
      return pythonFailure(NameValue);
    if (Value < std::numeric_limits<int>::min() ||
        Value > std::numeric_limits<int>::max()) {
      PyErr_Format(PyExc_OverflowError,
                   "plugin hook '%s' result does not fit a C int", NameValue);
      return pythonFailure(NameValue);
    }
    return static_cast<int>(Value);
  }

  int pythonFailure(const char *Callback) {
    const std::string Traceback = formatPythonException();
    setLastError("Python callback '" + std::string(Callback) + "' raised:\n" +
                 Traceback);
    return -1;
  }

  void setLastError(std::string Message) {
    std::lock_guard<std::mutex> Lock(ErrorMutex);
    LastError = std::move(Message);
  }

  void clearLastError() {
    std::lock_guard<std::mutex> Lock(ErrorMutex);
    LastError.clear();
  }

  std::string ModuleName;
  PyObject *Module = nullptr;
  PyObject *APIModule = nullptr;
  PyObject *Instance = nullptr;
  PyObject *SessionCapsule = nullptr;
  PyObject *SessionObject = nullptr;
  SessionControl *Control = nullptr;
  std::string Name;
  std::string Version;
  std::string Author;
  std::string Description;
  neverd_plugin_t Descriptor{};
  bool Terminated = false;
  mutable std::mutex ErrorMutex;
  std::string LastError;
};

} // namespace

std::unique_ptr<PluginRuntime>
loadPythonPluginRuntime(const std::string &CanonicalPath, std::string &Error) {
  Error.clear();
  const std::string InitializationError = initializeInterpreter();
  if (!InitializationError.empty()) {
    Error = "cannot initialize CPython: " + InitializationError;
    return nullptr;
  }

  GILGuard Guard;
  if (!installBridgeModule() || !addSDKPaths()) {
    Error = "cannot prepare NeverD's Python plugin bridge:\n" +
            formatPythonException();
    return nullptr;
  }

  PyRef APIModule(PyImport_ImportModule("neverd_plugin"));
  if (!APIModule) {
    Error =
        "cannot import the 'neverd_plugin' SDK:\n" + formatPythonException();
    return nullptr;
  }

#ifdef NEVERD_PYTHON_SDK_VERSION
  std::string SDKVersion;
  if (!metadataString(APIModule.get(), "__version__", false, SDKVersion)) {
    Error = "cannot read the 'neverd_plugin' SDK version:\n" +
            formatPythonException();
    return nullptr;
  }
  if (SDKVersion != NEVERD_PYTHON_SDK_VERSION) {
    Error = "incompatible 'neverd_plugin' SDK version " + SDKVersion +
            "; this NeverD host requires " + NEVERD_PYTHON_SDK_VERSION;
    return nullptr;
  }
#endif

  const std::string ModuleName = makeModuleName(CanonicalPath);
  PyRef Module(importPluginModule(CanonicalPath, ModuleName));
  if (!Module) {
    Error = "cannot import Python plugin '" + CanonicalPath + "':\n" +
            formatPythonException();
    return nullptr;
  }

  auto RemoveModuleOnFailure = [&] {
    PyObject *Modules = PyImport_GetModuleDict();
    if (Modules && PyDict_DelItemString(Modules, ModuleName.c_str()) < 0)
      PyErr_Clear();
  };

  PyRef Spec(PyObject_GetAttrString(Module.get(), "__neverd_plugin__"));
  std::string Name;
  std::string Version;
  std::string Author;
  std::string Description;
  if (!Spec || !metadataString(Spec.get(), "name", false, Name) ||
      !metadataString(Spec.get(), "version", false, Version) ||
      !metadataString(Spec.get(), "author", true, Author) ||
      !metadataString(Spec.get(), "description", true, Description)) {
    Error = "invalid metadata in Python plugin '" + CanonicalPath + "':\n" +
            formatPythonException();
    RemoveModuleOnFailure();
    return nullptr;
  }

  PyRef TypeValue(PyObject_GetAttrString(Spec.get(), "type"));
  const long Type = TypeValue ? PyLong_AsLong(TypeValue.get()) : -1;
  if (!TypeValue || (Type == -1 && PyErr_Occurred()) ||
      Type < NEVERD_PLUGIN_GENERIC || Type > NEVERD_PLUGIN_UI) {
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_ValueError, "plugin metadata has invalid type");
    Error = "invalid metadata in Python plugin '" + CanonicalPath + "':\n" +
            formatPythonException();
    RemoveModuleOnFailure();
    return nullptr;
  }

  PyRef PluginClass(PyObject_GetAttrString(Spec.get(), "plugin_class"));
  if (!PluginClass || !PyCallable_Check(PluginClass.get())) {
    if (!PyErr_Occurred())
      PyErr_SetString(PyExc_TypeError,
                      "plugin metadata 'plugin_class' must be callable");
    Error = "invalid metadata in Python plugin '" + CanonicalPath + "':\n" +
            formatPythonException();
    RemoveModuleOnFailure();
    return nullptr;
  }
  PyRef Instance(PyObject_CallNoArgs(PluginClass.get()));
  if (!Instance) {
    Error = "Python plugin constructor raised for '" + CanonicalPath + "':\n" +
            formatPythonException();
    RemoveModuleOnFailure();
    return nullptr;
  }

  return std::make_unique<PythonPluginRuntime>(
      ModuleName, Module.release(), APIModule.release(), Instance.release(),
      std::move(Name), std::move(Version), std::move(Author),
      std::move(Description), static_cast<neverd_plugin_type_t>(Type));
}
