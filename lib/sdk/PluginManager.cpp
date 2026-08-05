//===- PluginManager.cpp - Plugin loading and dispatch ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PluginManager.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

#include <cstring>

PluginManager::~PluginManager() { termAll(); }

void PluginManager::loadPluginsFromDir(const std::string &Dir) {
#ifdef _WIN32
  auto Pattern = Dir + "\\*.dll";
  WIN32_FIND_DATAA Fd;
  HANDLE H = FindFirstFileA(Pattern.c_str(), &Fd);
  if (H == INVALID_HANDLE_VALUE)
    return;
  do {
    auto Path = Dir + "\\" + Fd.cFileName;
    loadPlugin(Path);
  } while (FindNextFileA(H, &Fd));
  FindClose(H);
#else
  DIR *D = opendir(Dir.c_str());
  if (!D)
    return;

#ifdef __APPLE__
  const char *Ext = ".dylib";
#else
  const char *Ext = ".so";
#endif

  size_t ExtLen = strlen(Ext);
  while (auto *Entry = readdir(D)) {
    auto *Name = Entry->d_name;
    size_t Len = strlen(Name);
    if (Len > ExtLen && strcmp(Name + Len - ExtLen, Ext) == 0) {
      auto Path = Dir + "/" + Name;
      loadPlugin(Path);
    }
  }
  closedir(D);
#endif
}

bool PluginManager::loadPlugin(const std::string &Path) {
#ifdef _WIN32
  void *Handle = LoadLibraryA(Path.c_str());
  if (!Handle)
    return false;
  auto *Desc = reinterpret_cast<neverd_plugin_t *>(
      GetProcAddress(static_cast<HMODULE>(Handle), "neverd_plugin"));
#else
  void *Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!Handle)
    return false;
  auto *Desc =
      reinterpret_cast<neverd_plugin_t *>(dlsym(Handle, "neverd_plugin"));
#endif

  if (!Desc || !Desc->Name) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(Handle));
#else
    dlclose(Handle);
#endif
    return false;
  }

  Plugins.push_back({Path, Desc, Handle});
  return true;
}

void PluginManager::initAll(neverd_session_t Session) {
  for (auto &P : Plugins) {
    if (P.Descriptor->Init)
      P.Descriptor->Init(Session);
  }
}

void PluginManager::termAll() {
  for (auto &P : Plugins) {
    if (P.Descriptor->Term)
      P.Descriptor->Term();
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(P.Handle));
#else
    dlclose(P.Handle);
#endif
  }
  Plugins.clear();
}

void PluginManager::dispatchEvent(const neverd_event_t &Event) {
  for (auto &P : Plugins) {
    if (P.Descriptor->Event)
      P.Descriptor->Event(&Event);
  }
}

int PluginManager::runPlugin(const std::string &Name, neverd_session_t Session,
                             int Arg) {
  for (auto &P : Plugins) {
    if (P.Descriptor->Name && Name == P.Descriptor->Name) {
      if (P.Descriptor->Run)
        return P.Descriptor->Run(Session, Arg);
      return -1;
    }
  }
  return -1;
}
