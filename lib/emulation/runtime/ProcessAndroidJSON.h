//===- ProcessAndroidJSON.h - Android native wire helpers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSANDROIDJSON_H
#define NEVERD_EMULATION_PROCESSANDROIDJSON_H
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/JSON.h"
namespace neverd::emulation {
llvm::Expected<AndroidNativeOptions>
androidOptionsFromJSON(const llvm::json::Value &Value);
void writeAndroidResultFields(llvm::json::OStream &J,
                              const ProcessResult &Result);
} // namespace neverd::emulation
#endif
