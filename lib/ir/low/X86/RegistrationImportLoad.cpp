//===- RegistrationImportLoad.cpp - Cached PE32 imports ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates complete MOV loads without treating IAT bytes as addresses.

#include "RegistrationImportLoad.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_abi {

std::optional<RegistrationImportLoad>
registrationImportLoad(const LowBlock &Block,
                       const LowInstructionBoundary &Boundary) {
  if (Boundary.Control != LowInstructionControl::None ||
      Boundary.Mode != InstructionMode::Default ||
      Boundary.ControlFlags != LowInstructionControlFlag::None ||
      Boundary.OpCount != 4 || Boundary.FirstOp > Block.Ops.size() ||
      Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp)
    return std::nullopt;
  const auto *Ops = Block.Ops.data() + Boundary.FirstOp;
  for (unsigned I = 0; I != 4; ++I)
    if (Ops[I].Addr != Boundary.Address || Ops[I].Seq != int(I) ||
        Ops[I].NumInputs != 1 ||
        Ops[I].MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Ops[I].MemoryOrdering != NdMemoryOrdering::None)
      return std::nullopt;
  if (Ops[0].Opcode != NdOp::COPY || !Ops[0].Inputs[0].isConst() ||
      Ops[0].Inputs[0].Size != 4 || Ops[0].Inputs[0].Offset > UINT32_MAX ||
      !Ops[0].Output.isTemp() || Ops[0].Output.Size != 4 ||
      Ops[1].Opcode != NdOp::INT_ZEXT || Ops[1].Inputs[0] != Ops[0].Output ||
      !Ops[1].Output.isTemp() || Ops[1].Output.Size != 8 ||
      Ops[2].Opcode != NdOp::LOAD || Ops[2].Inputs[0] != Ops[1].Output ||
      !Ops[2].Output.isTemp() || Ops[2].Output.Size != 4 ||
      Ops[3].Opcode != NdOp::COPY || Ops[3].Inputs[0] != Ops[2].Output ||
      !Ops[3].Output.isReg() || Ops[3].Output.Size != 4 ||
      Ops[3].Output.Offset >= 8 * x86reg::GeneralRegStride ||
      Ops[3].Output.Offset % x86reg::GeneralRegStride)
    return std::nullopt;
  return RegistrationImportLoad{Ops[0].Inputs[0].Offset, Ops[2].Seq,
                                Ops[3].Output.Offset};
}

} // namespace neverd::registration_abi
