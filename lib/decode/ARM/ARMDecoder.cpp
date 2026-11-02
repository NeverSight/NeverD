//===- ARMDecoder.cpp - ARM and Thumb decoder support --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/Thumb mode selection and scalar relocation handling for lifting.
///
//===----------------------------------------------------------------------===//

#include "../DecoderDetail.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/ARM/ARMLifter.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/Support/raw_ostream.h"

namespace neverd {
namespace decoder_detail {

cs_mode armHandleMode(InstructionMode Mode) {
  return static_cast<cs_mode>(
      (Mode == InstructionMode::Thumb ? CS_MODE_THUMB : CS_MODE_ARM) |
      CS_MODE_V8);
}

bool isARMInstructionAligned(va_t Addr, InstructionMode Mode) {
  return Addr % (Mode == InstructionMode::Thumb ? 2 : 4) == 0;
}

} // namespace decoder_detail

bool Decoder::initARMImageMode(const BinaryImage &Img) {
  if (Img.Arch != Arch::ARM)
    return false;
  if (Img.Entry != 0)
    if (const auto EntryMode = Img.instructionModeAt(Img.Entry))
      return init(Img.Arch, *EntryMode);
  for (const auto &[Addr, Mode] : Img.ARMCodeModeEntries)
    if (Img.instructionModeAt(Addr) == Mode)
      return init(Img.Arch, Mode);
  for (const ARMCodeRegion &Region : Img.ARMCodeRegions)
    if (const auto Mode = Img.instructionModeAt(Region.Start))
      return init(Img.Arch, *Mode);
  syncError() << "mixed ARM/Thumb image has no authenticated code mode\n";
  return false;
}

void Decoder::liftARMToLow(
    const DecodedInsn &Insn, std::vector<LowOp> &Ops,
    llvm::ArrayRef<RelocatedScalarOperand> ScalarRelocs) {
  bool Unrelocated = false;
  if (Insn.Raw)
    for (const RelocatedScalarOperand &Scalar : ScalarRelocs)
      Unrelocated |=
          Scalar.Semantics ==
              RelocatedScalarOperand::Kind::ARMELFUnrelocatedInstruction &&
          Scalar.FieldVA == Insn.Raw->address && Scalar.Width == Insn.Raw->size;
  ARM->lift(Insn.Raw, Ops, Unrelocated);
}

LowInstructionTargetMode
Decoder::controlTargetMode(const DecodedInsn &Insn,
                           InstructionMode SourceMode) const {
  if (!Insn.Raw || !ARM)
    return LowInstructionTargetMode::Preserve;
  return ARMLifter::controlTargetMode(Insn.Raw, SourceMode);
}

} // namespace neverd
