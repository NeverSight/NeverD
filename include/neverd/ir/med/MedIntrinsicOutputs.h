//===- MedIntrinsicOutputs.h - Multi-result intrinsic bindings -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDINTRINSICOUTPUTS_H
#define NEVERD_IR_MED_MEDINTRINSICOUTPUTS_H

#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace neverd {

struct MedIntrinsicOutputBinding {
  size_t OpIndex = 0;
  MedVar Source;
  bool IsCopy = false;
};

/// Bind the temporary values consumed by consecutive intrinsic output writes.
/// LowToMed may replace an output COPY with a zero/sign extension or subpiece
/// extraction when an architectural sub-register is written.  Both LLVM and
/// HighIR emission must use this same ordering rather than rediscovering it.
inline std::vector<MedIntrinsicOutputBinding>
collectMedIntrinsicOutputBindings(const MedBlock &Block, size_t IntrinsicIndex,
                                  uint8_t OutputCount) {
  std::vector<MedIntrinsicOutputBinding> Adjacent;
  Adjacent.reserve(OutputCount);
  for (size_t Index = IntrinsicIndex + 1;
       Index < Block.Ops.size() && Adjacent.size() < OutputCount; ++Index) {
    const MedOp &Op = Block.Ops[Index];
    const bool IsCopy = Op.Opcode == NdOp::COPY;
    const bool IsSubregisterWrite =
        Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT ||
        Op.Opcode == NdOp::SUBBYTES;
    if (!IsCopy && !IsSubregisterWrite)
      break;
    if (Op.NumInputs == 0 || Op.Inputs[0].Kind != MedVar::Temp)
      continue;
    const MedVar &Source = Op.Inputs[0];
    if (std::any_of(Adjacent.begin(), Adjacent.end(),
                    [&](const MedIntrinsicOutputBinding &Binding) {
                      return Binding.Source == Source;
                    }))
      continue;
    Adjacent.push_back({Index, Source, IsCopy});
  }
  const auto &Outputs = Block.Ops[IntrinsicIndex].IntrinsicOutputs;
  if (Outputs.empty())
    return Adjacent;
  std::vector<MedIntrinsicOutputBinding> Bindings;
  Bindings.reserve(std::min<size_t>(Outputs.size(), OutputCount));
  for (const MedVar &Output : Outputs) {
    if (Bindings.size() >= OutputCount)
      break;
    const auto It = std::find_if(
        Adjacent.begin(), Adjacent.end(),
        [&](const MedIntrinsicOutputBinding &Binding) {
          return Binding.Source == Output;
        });
    // Propagation can remove the transport COPY entirely.  The intrinsic's
    // SSA definition remains authoritative even without that adjacent op.
    Bindings.push_back(It != Adjacent.end()
                           ? *It
                           : MedIntrinsicOutputBinding{0, Output, false});
  }
  return Bindings;
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDINTRINSICOUTPUTS_H
