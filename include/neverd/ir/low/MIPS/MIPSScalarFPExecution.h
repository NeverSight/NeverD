//===- MIPSScalarFPExecution.h - Bound scalar FP ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
#ifndef NEVERD_IR_LOW_MIPS_MIPSSCALARFPEXECUTION_H
#define NEVERD_IR_LOW_MIPS_MIPSSCALARFPEXECUTION_H
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
namespace neverd {
struct MIPSScalarFPResult {
  uint32_t Control;
  std::optional<uint64_t> Destination;
};
class MIPSScalarFPExecutor {
public:
  virtual ~MIPSScalarFPExecutor() = default;
  virtual bool supportsRevision(unsigned Revision) const = 0;
  /// Early implementations differ in exception, NaN and denormal behavior.
  /// A modern Release 2 evaluator is not evidence for a historical FPU.
  virtual bool supportsHistoricalISA(unsigned) const { return false; }
  bool supportsProfile(unsigned Revision, unsigned HistoricalISA) const {
    return HistoricalISA && HistoricalISA <= 3
               ? supportsHistoricalISA(HistoricalISA)
               : supportsRevision(HistoricalISA ? 2 : Revision);
  }
  /// A trap updates FCSR before suppressing the destination. An unavailable
  /// observation leaves both unchanged.
  virtual std::optional<MIPSScalarFPResult>
  control(unsigned Selector, bool Write, uint32_t Value, uint32_t FCSR) = 0;
  virtual std::optional<MIPSScalarFPResult>
  execute(Intrinsic Op, uint32_t FCSR, llvm::ArrayRef<uint64_t> Inputs) = 0;
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSSCALARFPEXECUTION_H
