//===- PythonRecovery.h - Offline Python artifact recovery ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recovers source and navigation evidence without loading a Python runtime.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_PYTHONRECOVERY_H
#define NEVERD_ANALYSIS_PYTHONRECOVERY_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <string>

namespace neverd::python {
using Cancel = llvm::function_ref<bool()>;
llvm::json::Object recover(llvm::StringRef Bytes, llvm::StringRef Name,
                           llvm::StringRef Profile, Cancel Stop,
                           int Component = -1);
llvm::json::Object recoverFile(llvm::StringRef Path, llvm::StringRef Profile,
                               Cancel Stop, int Component = -1);
llvm::json::Object exportNative(llvm::StringRef Path, llvm::StringRef Profile,
                                unsigned Component,
                                llvm::StringRef ExpectedHash,
                                llvm::StringRef Destination, Cancel Stop);
std::string serializeReport(llvm::json::Object Report);
/// Serialize file recovery directly, with the same evidence and report limit.
std::string recoverFileJSON(llvm::StringRef Path, llvm::StringRef Profile,
                            Cancel Stop, int Component = -1);
} // namespace neverd::python

#endif // NEVERD_ANALYSIS_PYTHONRECOVERY_H
