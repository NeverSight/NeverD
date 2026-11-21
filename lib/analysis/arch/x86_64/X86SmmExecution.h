//===- X86SmmExecution.h - Software SMI and RSM ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run software SMI and original RSM against isolated system RAM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMEXECUTION_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMEXECUTION_H

#include "X86SmmSaveMap.h"

#include <optional>

namespace neverd::analysis::detail {

struct X86RsmProgress {
  std::optional<X86SmmSaveImage> SaveArea;
  translate::X86PAEPDPTEContinuation PDPTE;
};

llvm::Expected<X86ModeExecutionResult>
enterX86Smm(X86ModeState &State, translate::X86PagedMemory &Normal,
            translate::X86PagedMemory &SMRAM,
            const X86ModeExecutionOptions &Options);
llvm::Expected<X86ModeExecutionResult>
executeX86Rsm(X86ModeState &State, translate::X86PagedMemory &Normal,
              translate::X86PagedMemory &SMRAM, llvm::ArrayRef<uint8_t> Bytes,
              bool Locked, bool Fetched, const X86ModeExecutionOptions &Options,
              X86RsmProgress &Progress, X86EventKind &FaultKind);

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMEXECUTION_H
