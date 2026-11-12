//===- MIPSR6FPExecution.h - Bound Release 6 scalar FP ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSR6FPEXECUTION_H
#define NEVERD_IR_LOW_MIPS_MIPSR6FPEXECUTION_H
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
namespace neverd {
struct MIPSR6FPResult {
  uint32_t Control;
  std::optional<uint64_t> Destination;
};
class MIPSR6FPExecutor {
public:
  virtual ~MIPSR6FPExecutor() = default;
  /// A trap updates FCSR before suppressing the destination. An unavailable
  /// observation leaves both unchanged.
  virtual std::optional<MIPSR6FPResult>
  control(unsigned Selector, bool Write, uint32_t Value, uint32_t FCSR) = 0;
  virtual std::optional<MIPSR6FPResult>
  execute(Intrinsic Op, uint32_t FCSR, llvm::ArrayRef<uint64_t> Inputs) = 0;
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSR6FPEXECUTION_H
