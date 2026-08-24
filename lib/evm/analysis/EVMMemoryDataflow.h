//===- EVMMemoryDataflow.h - EVM memory reaching definitions ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the private, flow-sensitive memory analysis used by HighIR fact
/// recovery.  The analysis exposes only proven byte ranges at operation entry;
/// unknown writes and disagreeing CFG predecessors remove knowledge.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMMEMORYDATAFLOW_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMMEMORYDATAFLOW_H

#include "neverd/evm/EVMIR.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace neverd::evm {
struct AnalyzeOptions;
}

namespace neverd::evm::detail {

class DefiniteExecutionIndex;

/// Proven memory bytes immediately before selected MedIR operations.
///
/// Only byte ranges consumed by current HighIR recovery are tracked.  This
/// keeps the state proportional to observable facts while retaining exact
/// MSTORE/MSTORE8 overlap semantics and CFG joins.
class EVMMemoryDataflow {
public:
  static llvm::Expected<EVMMemoryDataflow>
  analyze(const EVMLowIR &Low, const EVMMedIR &Med,
          const DefiniteExecutionIndex &Execution,
          const AnalyzeOptions &Options,
          llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
          llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
          llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit);

  /// Returns the big-endian byte range beginning at \p Address immediately
  /// before the operation at \p PC, when every byte is proven.
  [[nodiscard]] std::optional<llvm::APInt> read(uint64_t PC, uint64_t Address,
                                                size_t Size) const;

private:
  struct ReadKey {
    uint64_t PC = 0;
    uint64_t Address = 0;
    size_t Size = 0;

    friend bool operator<(const ReadKey &Left, const ReadKey &Right) {
      if (Left.PC != Right.PC)
        return Left.PC < Right.PC;
      if (Left.Address != Right.Address)
        return Left.Address < Right.Address;
      return Left.Size < Right.Size;
    }
  };

  std::map<ReadKey, llvm::APInt> Reads;
};

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMMEMORYDATAFLOW_H
