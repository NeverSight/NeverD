//===- MedLLVMInstructionText.h - An intrinsic's instruction ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The operand a lifted trap, breakpoint or barrier keeps as its intrinsic's
/// first input (intrinsicCarriesInstructionOperand), for the LLVM emitters:
/// the option an emitted barrier runs with, and the instruction text the
/// call or fence standing for it carries (machine_instruction_text).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_MEDLLVMINSTRUCTIONTEXT_H
#define NEVERD_BACKEND_LLVM_MEDLLVMINSTRUCTIONTEXT_H

#include "neverd/backend/MachineInstructionText.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/ir/med/MedIR.h"

#include <optional>

namespace neverd {

/// The operand \p Op's intrinsic keeps for its instruction, if the lift
/// kept one.
inline std::optional<uint64_t> instructionOperand(const MedOp &Op) {
  if (Op.NumInputs >= 2 && Op.Inputs[1].isConst())
    return Op.Inputs[1].ConstVal;
  return std::nullopt;
}

/// Record on \p I the instruction \p Op, an intrinsic \p Id, was lifted
/// from, where its C spelling leaves out the operand (`brk #0x3E8`).
inline void markInstructionText(llvm::Instruction &I, const MedOp &Op,
                                Intrinsic Id) {
  machine_instruction_text::set(
      I, intrinsicInstructionText(Id, instructionOperand(Op)));
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_MEDLLVMINSTRUCTIONTEXT_H
