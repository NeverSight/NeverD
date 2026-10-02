#include "NativeSourcePreservation.h"

#include "neverd/pipeline/NativeSourceHints.h"

namespace neverd {
bool hasNativeScalarIntrinsicEvidence(const MedOp &Operation,
                                      Arch Architecture) {
  if (Architecture != Arch::AArch64 || Operation.Opcode != NdOp::INTRINSIC ||
      Operation.NumInputs != 2 || !Operation.Inputs[0].isConst() ||
      Operation.Inputs[0].Size != 2 ||
      Operation.Inputs[0].ConstVal !=
          static_cast<uint64_t>(Intrinsic::A64_Rbit) ||
      (Operation.Output.Kind != MedVar::Reg &&
       Operation.Output.Kind != MedVar::Temp) ||
      Operation.Output.Size != Operation.Inputs[1].Size ||
      (Operation.Output.Size != 1 && Operation.Output.Size != 2 &&
       Operation.Output.Size != 4 && Operation.Output.Size != 8))
    return false;
  return true;
}

bool certifiesPrivateNativeSourceFrame(const LowFunc &Function,
                                       Arch Architecture,
                                       bool ExternalMemoryDisjoint,
                                       int64_t *RequiredFrameSize) {
  return certifiesPrivateSourceFrame(Function, Architecture,
                                     ExternalMemoryDisjoint, RequiredFrameSize);
}
} // namespace neverd
