//===- X86SgxExecution.h - Software enclave transfers ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// SGX1 x64 thread entry, asynchronous exit and resume over an imported EPC.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SGXEXECUTION_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SGXEXECUTION_H

#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

namespace neverd::analysis::detail {
struct X86SgxInstructionProgress {
  std::optional<uint64_t> TCSPhysical, XSavePhysical;
};

class X86SgxExecution {
public:
  static llvm::Expected<bool>
  illegalInstruction(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC,
                     const translate::X86SgxMemory &Memory);
  static llvm::Expected<X86ModeExecutionResult>
  instruction(X86ModeState &State, translate::X86PagedMemory &Memory,
              bool Fetched, llvm::ArrayRef<uint8_t> Original, bool Illegal,
              X86SgxInstructionProgress &Progress, uint32_t MXCSRMask);
  /// Called after CPU event admission and before handler frame accesses.
  static llvm::Error asynchronousExit(X64ContinuationStateV6 &State,
                                      translate::X86SgxMemory &Memory,
                                      X64ContinuationEventV1 &Event,
                                      uint64_t SavedFlags);
};
} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SGXEXECUTION_H
