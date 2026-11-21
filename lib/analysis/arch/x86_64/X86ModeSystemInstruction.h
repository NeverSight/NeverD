//===- X86ModeSystemInstruction.h - Privileged x86 steps -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticated halt and machine-check MSR operations over the full CPU bank.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESYSTEMINSTRUCTION_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESYSTEMINSTRUCTION_H

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

namespace neverd::analysis::detail {
enum class X86ModeSystemOp { Halt, ReadMCA, WriteMCA };
llvm::Expected<X86ModeExecutionResult> executeModeSystemInstruction(
    X86ModeState &State, translate::X86PagedMemory &Memory,
    const X86ModeInstruction &Instruction,
    const X86ModeExecutionOptions &Options, X86ModeSystemOp Operation,
    bool Locked, X86EventKind &FaultKind, translate::X86SgxMemory *Enclaves);
} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESYSTEMINSTRUCTION_H
