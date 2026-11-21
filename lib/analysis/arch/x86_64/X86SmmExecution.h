//===- X86SmmExecution.h - Software SMI and RSM ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serialize the KVM64 save map and restore it from isolated system RAM.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMEXECUTION_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SMMEXECUTION_H

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

#include <array>
#include <optional>

namespace neverd::analysis::detail {

struct X86RsmProgress {
  std::optional<std::array<uint8_t, 512>> SaveArea;
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
