//===- LuaRecovery.h - Lua-family artifact recovery -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reads an inert artifact and returns the shared source and navigation report.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LUARECOVERY_H
#define NEVERD_ANALYSIS_LUARECOVERY_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <string>

namespace neverd::lua {
using Cancel = llvm::function_ref<bool()>;
std::string serializeReport(llvm::json::Object Report);
llvm::json::Object recoverFile(llvm::StringRef Path, llvm::StringRef Profile,
                               Cancel Stop);
llvm::json::Object recover(llvm::StringRef Bytes, llvm::StringRef Extension,
                           llvm::StringRef Profile, Cancel Stop);
} // namespace neverd::lua
#endif // NEVERD_ANALYSIS_LUARECOVERY_H
