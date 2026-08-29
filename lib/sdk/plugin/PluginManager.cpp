//===- PluginManager.cpp - Plugin loading and dispatch ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PluginManager.h"

#ifdef NEVERD_ENABLE_PYTHON_PLUGINS
#include "PythonPluginRuntime.h"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

fs::path pathFromUTF8(const std::string &Path) {
  const std::u8string UTF8(reinterpret_cast<const char8_t *>(Path.data()),
                           Path.size());
  return fs::path(UTF8);
}

std::string pathToUTF8(const fs::path &Path) {
  const std::u8string UTF8 = Path.u8string();
  return std::string(reinterpret_cast<const char *>(UTF8.data()), UTF8.size());
}

std::string lowercaseExtension(const fs::path &Path) {
  std::string Extension = pathToUTF8(Path.extension());
  std::transform(Extension.begin(), Extension.end(), Extension.begin(),
                 [](unsigned char Character) {
                   return static_cast<char>(std::tolower(Character));
                 });
  return Extension;
}

bool isNativePluginExtension(const fs::path &Path) {
#ifdef _WIN32
  return lowercaseExtension(Path) == ".dll";
#elif defined(__APPLE__)
  return lowercaseExtension(Path) == ".dylib";
#else
  return lowercaseExtension(Path) == ".so";
#endif
}

bool isPythonPluginExtension(const fs::path &Path) {
  return lowercaseExtension(Path) == ".py";
}

class NativePluginRuntime final : public PluginRuntime {
public:
  NativePluginRuntime(void *HandleValue, neverd_plugin_t *DescriptorValue)
      : Handle(HandleValue),
        Name(DescriptorValue->Name ? DescriptorValue->Name : ""),
        Version(DescriptorValue->Version ? DescriptorValue->Version : ""),
        Author(DescriptorValue->Author ? DescriptorValue->Author : ""),
        Description(DescriptorValue->Description ? DescriptorValue->Description
                                                 : "") {
    Descriptor = *DescriptorValue;
    Descriptor.Name = Name.c_str();
    Descriptor.Version = Version.c_str();
    Descriptor.Author = Author.c_str();
    Descriptor.Description = Description.c_str();
  }

  ~NativePluginRuntime() override {
#ifdef _WIN32
    if (Handle)
      FreeLibrary(static_cast<HMODULE>(Handle));
#else
    if (Handle)
      dlclose(Handle);
#endif
  }

  const neverd_plugin_t &descriptor() const override { return Descriptor; }
  const char *kind() const override { return "native"; }

  int init(neverd_session_t Session) override {
    return Descriptor.Init ? Descriptor.Init(Session) : 0;
  }

  void term() override {
    if (Descriptor.Term)
      Descriptor.Term();
  }

  int run(neverd_session_t Session, int Arg) override {
    return Descriptor.Run ? Descriptor.Run(Session, Arg) : -1;
  }

  int event(const neverd_event_t &Event) override {
    return Descriptor.Event ? Descriptor.Event(&Event) : 0;
  }

  std::string lastError() const override { return LastError; }

private:
  void *Handle;
  std::string Name;
  std::string Version;
  std::string Author;
  std::string Description;
  neverd_plugin_t Descriptor{};
  std::string LastError;
};

std::string canonicalPluginPath(const std::string &Path, std::error_code &EC) {
  fs::path Canonical = fs::canonical(pathFromUTF8(Path), EC);
  return EC ? std::string() : pathToUTF8(Canonical);
}

} // namespace

PluginManager::~PluginManager() { termAll(); }

int PluginManager::loadPluginsFromDir(const std::string &Dir) {
  LastError.clear();
  try {
    std::error_code EC;
    const fs::path Directory = pathFromUTF8(Dir);
    if (!fs::is_directory(Directory, EC)) {
      setError("plugin directory not found: " + Dir);
      return 0;
    }

    std::vector<std::string> Candidates;
    for (fs::directory_iterator It(Directory, EC), End; !EC && It != End;
         It.increment(EC)) {
      if (!It->is_regular_file(EC))
        continue;
      if (isNativePluginExtension(It->path())
#ifdef NEVERD_ENABLE_PYTHON_PLUGINS
          || isPythonPluginExtension(It->path())
#endif
      ) {
        const std::string CandidatePath = pathToUTF8(It->path());
        std::error_code CanonicalEC;
        std::string CanonicalPath =
            canonicalPluginPath(CandidatePath, CanonicalEC);
        if (CanonicalEC) {
          setError("cannot canonicalize plugin candidate '" + CandidatePath +
                   "': " + CanonicalEC.message());
          return 0;
        }
        Candidates.push_back(std::move(CanonicalPath));
      }
    }
    if (EC) {
      setError("cannot scan plugin directory '" + Dir + "': " + EC.message());
      return 0;
    }

    std::sort(Candidates.begin(), Candidates.end());
    int Loaded = 0;
    std::vector<std::string> Failures;
    for (const std::string &Path : Candidates) {
      if (loadPluginFile(Path)) {
        ++Loaded;
      } else if (!LastError.empty()) {
        Failures.push_back(LastError);
      }
    }
    if (!Failures.empty()) {
      std::string Message =
          std::to_string(Failures.size()) + " plugin file(s) failed to load";
      for (const std::string &Failure : Failures)
        Message += "\n- " + Failure;
      setError(std::move(Message));
    } else {
      LastError.clear();
    }
    return Loaded;
  } catch (const std::exception &Error) {
    setError("cannot scan plugin directory '" + Dir + "': " + Error.what());
    return 0;
  }
}

bool PluginManager::loadPluginFile(const std::string &Path) {
  LastError.clear();
  try {
    if (Path.empty()) {
      setError("plugin path is empty");
      return false;
    }
    std::error_code EC;
    std::string CanonicalPath = canonicalPluginPath(Path, EC);
    if (EC) {
      setError("plugin file not found: " + Path);
      return false;
    }
    for (const LoadedPlugin &Plugin : Plugins) {
      if (Plugin.Path == CanonicalPath) {
        setError("plugin path is already loaded: " + CanonicalPath);
        return false;
      }
    }
    const fs::path PluginPath = pathFromUTF8(CanonicalPath);
    if (!fs::is_regular_file(PluginPath, EC) || EC) {
      setError("plugin path is not a regular file: " + CanonicalPath);
      return false;
    }
    if (!isNativePluginExtension(PluginPath)) {
#ifdef NEVERD_ENABLE_PYTHON_PLUGINS
      if (isPythonPluginExtension(PluginPath)) {
        std::string Error;
        std::unique_ptr<PluginRuntime> Runtime =
            loadPythonPluginRuntime(CanonicalPath, Error);
        if (!Runtime) {
          setError(Error.empty() ? "cannot load Python plugin: " + CanonicalPath
                                 : std::move(Error));
          return false;
        }
        return validateAndAdd(CanonicalPath, std::move(Runtime));
      }
#else
      if (isPythonPluginExtension(PluginPath)) {
        setError("Python plugins are disabled; configure NeverD with "
                 "NEVERD_ENABLE_PYTHON_PLUGINS=ON");
        return false;
      }
#endif
      setError("unsupported plugin file extension: " +
               pathToUTF8(PluginPath.extension()));
      return false;
    }
    return loadNativePlugin(CanonicalPath);
  } catch (const std::exception &Error) {
    setError("cannot load plugin '" + Path + "': " + Error.what());
    return false;
  }
}

bool PluginManager::loadNativePlugin(const std::string &CanonicalPath) {
#ifdef _WIN32
  void *Handle = LoadLibraryW(pathFromUTF8(CanonicalPath).c_str());
  if (!Handle) {
    setError("cannot load native plugin '" + CanonicalPath +
             "' (Windows error " + std::to_string(GetLastError()) + ")");
    return false;
  }
  auto *Desc = reinterpret_cast<neverd_plugin_t *>(
      GetProcAddress(static_cast<HMODULE>(Handle), "neverd_plugin"));
#else
  dlerror();
  void *Handle = dlopen(CanonicalPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!Handle) {
    const char *Error = dlerror();
    setError("cannot load native plugin '" + CanonicalPath +
             "': " + (Error ? Error : "unknown loader error"));
    return false;
  }
  auto *Desc =
      reinterpret_cast<neverd_plugin_t *>(dlsym(Handle, "neverd_plugin"));
#endif

  if (!Desc) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(Handle));
#else
    dlclose(Handle);
#endif
    setError("native plugin does not export 'neverd_plugin': " + CanonicalPath);
    return false;
  }

  return validateAndAdd(CanonicalPath,
                        std::make_unique<NativePluginRuntime>(Handle, Desc));
}

bool PluginManager::validateAndAdd(std::string CanonicalPath,
                                   std::unique_ptr<PluginRuntime> Runtime) {
  const neverd_plugin_t &Descriptor = Runtime->descriptor();
  if (!Descriptor.Name || Descriptor.Name[0] == '\0') {
    setError("plugin has no non-empty name: " + CanonicalPath);
    return false;
  }
  if (Descriptor.Type < NEVERD_PLUGIN_GENERIC ||
      Descriptor.Type > NEVERD_PLUGIN_UI) {
    setError("plugin has invalid type: " + std::string(Descriptor.Name));
    return false;
  }
  for (const LoadedPlugin &Plugin : Plugins) {
    if (Plugin.descriptor().Name &&
        std::string(Plugin.descriptor().Name) == Descriptor.Name) {
      setError("plugin name is already loaded: " +
               std::string(Descriptor.Name));
      return false;
    }
  }

  Plugins.push_back({std::move(CanonicalPath), std::move(Runtime)});
  return true;
}

void PluginManager::initAll(neverd_session_t Session) {
  LastError.clear();
  for (auto &P : Plugins) {
    if (P.Initialized || P.Terminated)
      continue;
    const int Result = P.Runtime->init(Session);
    if (Result != 0) {
      std::string Detail = P.Runtime->lastError();
      setError("plugin '" + std::string(P.descriptor().Name) +
               "' initialization failed with code " + std::to_string(Result) +
               (Detail.empty() ? std::string() : ": " + Detail));
      continue;
    }
    P.Initialized = true;
  }
}

void PluginManager::termAll() {
  LastError.clear();
  for (auto &P : Plugins) {
    if (P.Initialized && !P.Terminated) {
      P.Runtime->term();
      const std::string Detail = P.Runtime->lastError();
      if (!Detail.empty())
        setError("plugin '" + std::string(P.descriptor().Name) +
                 "' termination failed: " + Detail);
    }
    P.Terminated = true;
  }
  Plugins.clear();
}

void PluginManager::dispatchEvent(const neverd_event_t &Event) {
  LastError.clear();
  for (auto &P : Plugins) {
    const int Result = P.Runtime->event(Event);
    if (Result != 0) {
      std::string Detail = P.Runtime->lastError();
      setError("plugin '" + std::string(P.descriptor().Name) +
               "' event callback failed with code " + std::to_string(Result) +
               (Detail.empty() ? std::string() : ": " + Detail));
    }
  }
}

int PluginManager::runPlugin(const std::string &Name, neverd_session_t Session,
                             int Arg) {
  LastError.clear();
  for (auto &P : Plugins) {
    if (P.descriptor().Name && Name == P.descriptor().Name) {
      const int Result = P.Runtime->run(Session, Arg);
      if (Result == -1 && !P.Runtime->lastError().empty())
        setError("plugin '" + Name + "' failed: " + P.Runtime->lastError());
      return Result;
    }
  }
  setError("plugin not found: " + Name);
  return -1;
}
