//===- NeverDCAPILua.cpp - Lua source recovery C ABI ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// CLI and worker receive the same owned artifact report.
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LuaRecovery.h"
#include "neverd/sdk/NeverDCAPISession.h"

#include <cstdlib>
#include <cstring>
#include <exception>

const char *neverd_lua_recover_json_v1(const char *Path, const char *Profile,
                                       neverd_cancel_probe_fn Stop,
                                       void *Context) {
  try {
    auto Result =
        neverd::lua::recoverFile(Path ? Path : "", Profile ? Profile : "auto",
                                 [&] { return Stop && Stop(Context); });
    auto Text = neverd::lua::serializeReport(std::move(Result));
    auto *Owned = static_cast<char *>(std::malloc(Text.size() + 1));
    if (Owned)
      std::memcpy(Owned, Text.c_str(), Text.size() + 1);
    return Owned;
  } catch (const std::exception &) {
    return nullptr;
  }
}
