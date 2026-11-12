//===- R6ScalarFP.h - Release 6 scalar FPU evaluator --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Instantiate an FR=1 Release 6 scalar FPU for the selected target.
#ifndef NEVERD_EMULATION_ARCH_MIPS_R6SCALARFP_H
#define NEVERD_EMULATION_ARCH_MIPS_R6SCALARFP_H

#include "neverd/Common.h"
#include "neverd/ir/low/MIPS/MIPSR6FPExecution.h"

#include "llvm/Support/Error.h"

#include <memory>

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<MIPSR6FPExecutor>>
createR6ScalarFPExecutor(Arch Target);
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_ARCH_MIPS_R6SCALARFP_H
