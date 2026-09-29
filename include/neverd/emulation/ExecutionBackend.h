//===- ExecutionBackend.h - Execution backend and contract selection -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_EXECUTIONBACKEND_H
#define NEVERD_EMULATION_EXECUTIONBACKEND_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace neverd::emulation {
enum class ExecutionBackendKind {
#define NEVERD_EXECUTION_BACKEND(Name, Text) Name,
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
};
enum class ExecutionContract {
#define NEVERD_EXECUTION_CONTRACT(Name, Text) Name,
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_CONTRACT
};

llvm::Expected<ExecutionBackendKind>
parseExecutionBackend(llvm::StringRef Name);
llvm::Expected<ExecutionContract> parseExecutionContract(llvm::StringRef Name);
const char *executionBackendName(ExecutionBackendKind Kind);
const char *executionContractName(ExecutionContract Contract);

namespace execution {
#define NEVERD_EXECUTION_BACKEND(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_CONTRACT(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#define NEVERD_EXECUTION_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/emulation/ExecutionBackend.def"
#undef NEVERD_EXECUTION_BACKEND
#undef NEVERD_EXECUTION_CONTRACT
#undef NEVERD_EXECUTION_TEXT
} // namespace execution
} // namespace neverd::emulation
#endif
