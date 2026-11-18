//===- LLVMTargetInitialization.h - Linked target registry ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared initialization for the LLVM backends linked by NeverD.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_LLVMTARGETINITIALIZATION_H
#define NEVERD_SUPPORT_LLVMTARGETINITIALIZATION_H

namespace neverd {

/// Register the linked target information, machines, MCs, printers and parsers
/// once for both object rewriting and translation. An LLVM SDK may contain
/// additional backends that NeverD does not link or admit for code generation.
void initializeLLVMCodegenTargets();

} // namespace neverd

#endif // NEVERD_SUPPORT_LLVMTARGETINITIALIZATION_H
