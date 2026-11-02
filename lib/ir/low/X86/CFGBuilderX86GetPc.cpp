//===- CFGBuilderX86GetPc.cpp - i386 get-PC thunk calls -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An i386 get-PC thunk is a function whose whole body is `mov r32, [esp];
/// ret`.  Position-independent 32-bit code has no PC-relative data addressing,
/// so it calls one to read its own return address, which GCC names
/// `__x86.get_pc_thunk.<reg>` (`__i686.get_pc_thunk.<reg>` before GCC 4.7).
/// The call has exactly the effect of `mov r32, <return address>`: the call
/// pushes the return address, the thunk copies it, and its return pops it,
/// leaving every other register, the flags and the stack pointer as they
/// were.  CFG construction lifts the call as that copy, so no calling
/// convention clobbers the caller's scratch registers across it or keeps the
/// register it loads.  The copy is recorded as a get-PC occurrence, so an
/// object file's GOTPC add after it can take the GOT-base model.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/X86/X86GetPcThunk.h"
#include "neverd/support/ISAEncoding.h"

#include <optional>

namespace neverd {

void CFGBuilder::rewriteGetPcThunkCall(const BinaryImage &Img, InsnRecord &Rec,
                                       va_t Next) {
  if (Img.Arch != Arch::X86)
    return;
  const va_t Target = *Rec.Immediate;
  const std::optional<unsigned> Reg = x86GetPcThunkRegister(Img, Target);
  if (!Reg)
    return;
  constexpr uint16_t PtrSize = 4;
  LowOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = NdVar::reg(x86reg::generalReg(*Reg), PtrSize);
  Copy.addInput(NdVar::cst(Next, PtrSize));
  Copy.Addr = Rec.Addr;
  Copy.Seq = 0;
  Rec.Ops = {Copy};
  Rec.IsCall = false;
  Rec.Immediate.reset();
  CallTargets.erase(Target);

  I386GetPcOccurrence Occurrence;
  Occurrence.CallInstructionAddr = Rec.Addr;
  Occurrence.InstructionAddr = Rec.Addr;
  Occurrence.OpSeq = Copy.Seq;
  Occurrence.PCValue = static_cast<uint32_t>(Next);
  Occurrence.OutputOpcode = NdOp::COPY;
  Occurrence.OutputWitness = Copy.Output;
  Occurrence.InputWitness = Copy.Inputs[0];
  I386GetPcOccurrences.push_back(Occurrence);
}

} // namespace neverd
