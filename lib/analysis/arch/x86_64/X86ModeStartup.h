//===- X86ModeStartup.h - INIT and startup IPI -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local CPU startup after an externally routed INIT or startup vector.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESTARTUP_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESTARTUP_H

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

namespace neverd::analysis::detail {
/// Return true when INIT, SIPI or wait-for-SIPI ends this execution slice.
llvm::Expected<bool> pollX86Startup(X86ModeState &State,
                                    translate::X86PagedMemory &Normal,
                                    X86ModeInterrupts *Inputs,
                                    X86ModeExecutionResult &Result);
} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86MODESTARTUP_H
