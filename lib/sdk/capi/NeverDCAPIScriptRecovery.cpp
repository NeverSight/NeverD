//===- NeverDCAPIScriptRecovery.cpp - Script artifact recovery C ABI ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Return the same owned recovery report to both frontends.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/JVMRecovery.h"
#include "neverd/analysis/LuaRecovery.h"
#include "neverd/analysis/PythonRecovery.h"
#include "neverd/sdk/NeverDCAPISession.h"

#include <cstdlib>
#include <cstring>
#include <exception>

namespace {
template <class Build> const char *ownedReport(Build Make) {
  try {
    auto Text = Make();
    auto *Owned = static_cast<char *>(std::malloc(Text.size() + 1));
    if (Owned)
      std::memcpy(Owned, Text.c_str(), Text.size() + 1);
    return Owned;
  } catch (const std::exception &) {
    return nullptr;
  }
}
} // namespace
const char *neverd_lua_recover_json_v1(const char *Path, const char *Profile,
                                       neverd_cancel_probe_fn Stop,
                                       void *Context) {
  return ownedReport([&] {
    return neverd::lua::serializeReport(
        neverd::lua::recoverFile(Path ? Path : "", Profile ? Profile : "auto",
                                 [&] { return Stop && Stop(Context); }));
  });
}
const char *neverd_python_recover_json_v1(const char *Path, const char *Profile,
                                          int Component,
                                          neverd_cancel_probe_fn Stop,
                                          void *Context) {
  return ownedReport([&] {
    return neverd::python::recoverFileJSON(
        Path ? Path : "", Profile ? Profile : "auto",
        [&] { return Stop && Stop(Context); }, Component);
  });
}
const char *neverd_python_export_native_json_v1(
    const char *Path, const char *Profile, unsigned Component,
    const char *ExpectedSHA256, const char *Destination,
    neverd_cancel_probe_fn Stop, void *Context) {
  return ownedReport([&] {
    return neverd::python::serializeReport(neverd::python::exportNative(
        Path ? Path : "", Profile ? Profile : "auto", Component,
        ExpectedSHA256 ? ExpectedSHA256 : "", Destination ? Destination : "",
        [&] { return Stop && Stop(Context); }));
  });
}

const char *neverd_jvm_recover_json_v1(const char *Path, const char *Options,
                                       neverd_cancel_probe_fn Stop,
                                       void *Context) {
  return ownedReport([&] {
    return neverd::jvm::recoverFileJSON(Path ? Path : "",
                                        Options ? Options : "{}",
                                        [&] { return Stop && Stop(Context); });
  });
}
