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

#include <span>
#include <vector>

namespace neverd::analysis::detail {
struct X86SgxFrameProgress {
  std::optional<uint64_t> XSavePage, GprPage;
};

struct X86SgxInstructionProgress {
  std::optional<uint64_t> TCSPhysical;
  X86SgxFrameProgress Frame, NotificationFrame;
  // Retain a completed KEYREQUEST/TARGETINFO read across a later backing stop.
  std::vector<uint8_t> Request;
  std::optional<std::array<uint8_t, 64>> ReportData;
  // Completed page walks survive a later missing page-table backing.
  std::optional<uint64_t> SecInfo, Destination, Source;
};

class X86SgxExecution {
public:
  static llvm::Expected<std::optional<uint8_t>>
  instructionException(llvm::ArrayRef<uint8_t> Bytes, uint64_t PC,
                       const translate::X86PagedMemory &Memory);
  static llvm::Expected<X86ModeExecutionResult>
  instruction(X86ModeState &State, translate::X86PagedMemory &Memory,
              bool Fetched, llvm::ArrayRef<uint8_t> Original,
              std::optional<uint8_t> InstructionException,
              X86SgxInstructionProgress &Progress, uint32_t MXCSRMask,
              X86EventKind &FaultKind);
  /// Called after CPU event admission and before handler frame accesses.
  static llvm::Error asynchronousExit(X64ContinuationStateV6 &State,
                                      translate::X86SgxMemory &Memory,
                                      X64ContinuationEventV1 &Event,
                                      uint64_t SavedFlags);

private:
  static constexpr unsigned Report2Size = 1024, Report2PayloadSize = 768;
  static llvm::Expected<std::array<uint8_t, Report2Size>>
  buildReport2(const translate::X86SgxKeyPlatform &Platform,
               const translate::X86SgxPage &SECS,
               std::span<const uint8_t, 64> Data);
  enum class KeyReportLeaf : uint32_t {
    Report = 0,
    Key = 1,
    Report2 = 10,
    Key256 = 11
  };
  static llvm::Expected<X86ModeExecutionResult>
  keyOrReport(KeyReportLeaf Leaf, X86ModeState &State,
              translate::X86PagedMemory &Memory,
              X86SgxInstructionProgress &Progress, unsigned InstructionSize,
              X86ModeExecutionResult Result);
  static llvm::Expected<X86ModeExecutionResult>
  dynamicMemory(X86ModeState &State, translate::X86PagedMemory &Memory,
                X86SgxInstructionProgress &Progress, unsigned InstructionSize,
                X86ModeExecutionResult Result);
  static llvm::Expected<bool>
  walkOperand(translate::X86PagedMemory &Memory,
              const translate::X86LinearAccess &Access,
              X86ModeExecutionResult &Result, uint64_t &Physical);
  static void retireEnclaveLeaf(X86ModeState &State, unsigned InstructionSize,
                                X86ModeExecutionResult &Result);
  static llvm::Expected<bool> bindFrame(translate::X86PagedMemory &Memory,
                                        const translate::X86SgxEnclave &Enclave,
                                        const translate::X86SgxPage &TCS,
                                        uint32_t FramePages, uint32_t Index,
                                        X86SgxFrameProgress &Progress,
                                        X86ModeExecutionResult &Result);
};
} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X86SGXEXECUTION_H
