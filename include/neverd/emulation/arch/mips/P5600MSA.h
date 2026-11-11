//===- P5600MSA.h - P5600 MSA floating-point evaluator --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Optional Unicorn-backed execution of the unpartitioned P5600 FP profile.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_MIPS_P5600MSA_H
#define NEVERD_EMULATION_ARCH_MIPS_P5600MSA_H

#include "neverd/Common.h"
#include "neverd/ir/low/MIPS/MIPSMSAExecution.h"

#include "llvm/Support/Error.h"

#include <memory>

namespace neverd::emulation {
llvm::Expected<std::unique_ptr<MIPSMSAExecutor>>
createP5600MSAExecutor(Arch Target);
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_ARCH_MIPS_P5600MSA_H
