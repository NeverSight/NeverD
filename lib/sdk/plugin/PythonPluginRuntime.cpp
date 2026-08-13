//===- PythonPluginRuntime.cpp - Embedded Python plugin adapter -*- C++ -*-===//

#include "PythonPluginRuntime.h"
#include "PythonPluginRuntimeDetail.h"

#include "neverd/sdk/NeverDPlugin.h"

#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

using namespace neverd::sdk::python_plugin;

namespace {

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
