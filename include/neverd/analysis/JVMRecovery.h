//===- JVMRecovery.h - Offline JVM class and JAR recovery -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Inspect immutable class files and release-specific JAR views.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_JVMRECOVERY_H
#define NEVERD_ANALYSIS_JVMRECOVERY_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

namespace neverd::jvm {
using Cancel = llvm::function_ref<bool()>;
/// Options are shared by the CLI, SDK and worker. An empty object selects the
/// base JAR view. Bytecode offsets in method rows address the physical class.
llvm::json::Object recover(llvm::StringRef Bytes, llvm::StringRef Name,
                           const llvm::json::Object &Options, Cancel Stop);
std::string recoverFileJSON(llvm::StringRef Path, llvm::StringRef OptionsJSON,
                            Cancel Stop);
} // namespace neverd::jvm
#endif // NEVERD_ANALYSIS_JVMRECOVERY_H
