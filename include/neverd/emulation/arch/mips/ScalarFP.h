//===- ScalarFP.h - Scalar FPU evaluator --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Instantiate an FR=1 scalar FPU for the selected target and ISA revision.
/// \file
#ifndef NEVERD_EMULATION_ARCH_MIPS_SCALARFP_H
#define NEVERD_EMULATION_ARCH_MIPS_SCALARFP_H

#include "neverd/Common.h"
#include "neverd/ir/low/MIPS/MIPSScalarFPExecution.h"

#include "llvm/Support/Error.h"

#include <memory>

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<MIPSScalarFPExecutor>>
createScalarFPExecutor(Arch Target, unsigned Revision = 6);
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_ARCH_MIPS_SCALARFP_H
