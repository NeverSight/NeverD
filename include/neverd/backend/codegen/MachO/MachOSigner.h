//===- MachOSigner.h - Controlled Darwin signing subprocess ---*- C++ -*-===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOSIGNER_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOSIGNER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <functional>
#include <string>
#include <vector>

namespace neverd::macho_signing {

struct ProcessRequest {
  std::string Program;
  std::vector<std::string> Arguments;
  std::vector<std::string> Environment;
  unsigned TimeoutSeconds = 0;
  bool DiscardStandardStreams = false;
  bool InheritEnvironment = false;
};

struct ProcessResult {
  int ExitCode = 0;
  bool ExecutionFailed = false;
  std::string Detail;
};

struct Operations {
  std::function<ProcessResult(const ProcessRequest &)> Execute;
};

const Operations &productionOperations();

llvm::Error verifyStrict(llvm::StringRef Path,
                         const Operations &Ops = productionOperations());
llvm::Error adHocResign(llvm::StringRef Path, llvm::StringRef Identifier,
                        const Operations &Ops = productionOperations());

} // namespace neverd::macho_signing

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOSIGNER_H
