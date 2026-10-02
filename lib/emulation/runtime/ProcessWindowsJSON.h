//===- ProcessWindowsJSON.h - Windows process wire inputs ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSWINDOWSJSON_H
#define NEVERD_EMULATION_PROCESSWINDOWSJSON_H
#include "neverd/emulation/WindowsProcessOptions.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<WindowsProcessOptions>
windowsOptionsFromJSON(const llvm::json::Value &Value);
} // namespace neverd::emulation
#endif
