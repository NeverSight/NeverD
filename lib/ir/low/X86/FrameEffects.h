//===- FrameEffects.h - x86 integer-frame effects --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares checked floating-state effects between PE32 and Win64 frame proofs.

#ifndef NEVERD_IR_LOW_X86_FRAMEEFFECTS_H
#define NEVERD_IR_LOW_X86_FRAMEEFFECTS_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

/// These operations preserve integer-frame memory on their ordinary path.
/// Their floating-point effects, memory reads and possible traps remain in IR.
inline bool isX86FramePreservingIntrinsic(const LowOp &Op, Arch Architecture) {
  if ((Architecture != Arch::X86 && Architecture != Arch::X64) ||
      Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      Op.NumInputs > std::size(Op.Inputs) || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 2 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (uint64_t(Id) != Op.Inputs[0].Offset)
    return false;
  // Use complete vector storage bounds, not a PE32 GPR-count threshold: R8
  // through R15 and APX registers are integer carriers on x64.
  const bool NonIntegerOutput =
      Op.Output.isTemp() ||
      (Op.Output.isReg() && Op.Output.Offset >= x86reg::VectorBase &&
       Op.Output.Offset < x86reg::vectorReg(x86reg::VectorRegCount) &&
       Op.Output.Size <=
           x86reg::vectorReg(x86reg::VectorRegCount) - Op.Output.Offset);
  if (Id == Intrinsic::X86ReadMXCSR)
    return Op.NumInputs == 1 && Op.Output.Size == 4 && NonIntegerOutput;
  if (Id == Intrinsic::X86WriteMXCSR)
    return Op.NumInputs == 2 && Op.Output == NdVar() &&
           Op.Inputs[1].Size == 4 &&
           (Op.Inputs[1].isTemp() || Op.Inputs[1].isReg() ||
            Op.Inputs[1].isConst());
  if (isX86ScalarFPStateIntrinsic(Id) || Id == Intrinsic::X86FPArithState ||
      Id == Intrinsic::X86FPArithMemoryState)
    return NonIntegerOutput &&
           x86FPStateShapeIsValid(Id, x86FPStateLowShape(Op, Architecture));
  // FXAM classifies one stack slot into the status word. It reads no memory
  // and raises no floating-point exception.
  const NdVar StatusWord = NdVar::reg(x86reg::FPU_SW, 2);
  if (Id == Intrinsic::X87Fxam)
    return Op.NumInputs == 3 && Op.Output == StatusWord &&
           Op.Inputs[1].isReg() && Op.Inputs[1].Offset >= x86reg::ST0 &&
           Op.Inputs[1].Offset <= x86reg::ST7 &&
           (Op.Inputs[1].Offset - x86reg::ST0) % x86reg::FPURegStride == 0 &&
           Op.Inputs[1].Size == x86reg::FPURegSize &&
           Op.Inputs[2] == StatusWord;
  return Id == Intrinsic::X87Wait && Op.NumInputs == 1 && Op.Output == NdVar();
}

} // namespace neverd

#endif
