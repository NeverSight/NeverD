//===- LowIntrinsicOutputs.h - Original multi-result bindings -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Consecutive result transports identify an original intrinsic's temporary
/// carriers before SSA. Conversion, validation and tracing share this query.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWINTRINSICOUTPUTS_H
#define NEVERD_IR_LOW_LOWINTRINSICOUTPUTS_H

#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace neverd {

inline std::vector<NdVar>
collectLowIntrinsicOutputBindings(const LowOp &Instruction,
                                  llvm::ArrayRef<LowOp> Following,
                                  uint8_t OutputCount) {
  std::vector<NdVar> Outputs;
  Outputs.reserve(OutputCount);
  for (const auto &Write : Following) {
    if (Outputs.size() == OutputCount || Write.Addr != Instruction.Addr)
      break;
    if (Write.Opcode != NdOp::COPY && Write.Opcode != NdOp::INT_ZEXT &&
        Write.Opcode != NdOp::INT_SEXT && Write.Opcode != NdOp::SUBBYTES)
      break;
    if (!Write.NumInputs || !Write.Inputs[0].isTemp())
      continue;
    const auto &Source = Write.Inputs[0];
    // Match LowToMed's typed temporary identity without merging width views.
    if (std::none_of(Outputs.begin(), Outputs.end(), [&](const NdVar &V) {
          return V.Offset == Source.Offset && V.Size == Source.Size;
        }))
      Outputs.push_back(Source);
  }
  return Outputs;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWINTRINSICOUTPUTS_H
