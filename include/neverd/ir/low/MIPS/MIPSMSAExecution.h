//===- MIPSMSAExecution.h - MSA floating-point machine state -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit target FP environment. A trapping instruction updates MSACSR but
/// leaves every bit of WD untouched. Input byte zero denotes register bit zero.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_MIPS_MIPSMSAEXECUTION_H
#define NEVERD_IR_LOW_MIPS_MIPSMSAEXECUTION_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/ArrayRef.h"

#include <array>
#include <optional>

namespace neverd {
using MIPSVector = std::array<uint8_t, 16>;
struct MIPSMSAResult {
  uint32_t Control;
  std::optional<MIPSVector> Destination;
};
class MIPSMSAExecutor {
public:
  virtual ~MIPSMSAExecutor() = default;
  /// nullopt means the requested machine state could not be executed; no
  /// architectural output is committed. A missing Destination is an MSAFPE.
  virtual std::optional<MIPSMSAResult>
  execute(Intrinsic Operation, uint32_t Implementation, uint32_t Control,
          llvm::ArrayRef<MIPSVector> Inputs) = 0;
};
} // namespace neverd
#endif // NEVERD_IR_LOW_MIPS_MIPSMSAEXECUTION_H
