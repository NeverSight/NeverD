//===- MIPSLiftDSP.cpp - Explicit DSP control and byte arithmetic --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compute each unsigned lane without cross-lane carry. DSPControl remains
/// explicit machine state even when the integer destination is discarded.
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/MIPS/MIPSDSP.h"
#include "neverd/lift/MIPS/MIPSLifter.h"

namespace neverd {

bool MIPSLifter::liftDSP(State &S, uint32_t Word) const {
  if (!HasDSP || S.Width != 4 || Mode == InstructionMode::MicroMIPS)
    return false;
  const auto Kind = mips::dspInstruction(Word);
  if (!Kind)
    return false;
  const unsigned RS = (Word >> 21) & 31;
  const unsigned RT = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31;
  const auto ReadControl = [&] {
    const auto Value = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::MIPSReadDSPControl, Value);
    return Value;
  };
  const auto WriteControl = [&](NdVar Value) {
    S.emitIntrinsic(Intrinsic::MIPSWriteDSPControl, {}, {Value});
  };
  if (*Kind == mips::DSPInstruction::ReadControl) {
    const uint32_t Bits = mips::dspControlMask(Word >> 16);
    if (!RD)
      S.emit(NdOp::NOP, {}, {});
    else
      S.write(RD,
              Bits ? S.binary(NdOp::INT_AND, ReadControl(), NdVar::cst(Bits, 4))
                   : NdVar::cst(0, 4));
    return true;
  }
  if (*Kind == mips::DSPInstruction::WriteControl) {
    const uint32_t Bits = mips::dspControlMask(Word >> 11);
    if (!Bits) {
      // WRDSP with no selected fields has no architectural effect in the
      // admitted DSP-enabled profile. This is a defined encoding, not a
      // fallback.
      S.emit(NdOp::NOP, {}, {});
      return true;
    }
    auto Value = S.binary(NdOp::INT_AND, S.read(RS), NdVar::cst(Bits, 4));
    if (Bits != mips::DSPControlBits)
      Value = S.binary(NdOp::INT_OR, Value,
                       S.binary(NdOp::INT_AND, ReadControl(),
                                NdVar::cst(mips::DSPControlBits & ~Bits, 4)));
    WriteControl(Value);
    return true;
  }

  const auto Left = S.read(RS);
  const auto Right = S.read(RT);
  NdVar Result = NdVar::cst(0, 4);
  NdVar Overflow = NdVar::cst(0, 1);
  for (unsigned Lane = 0; Lane != 4; ++Lane) {
    const auto Byte = [&](NdVar Value) {
      if (Lane)
        Value = S.binary(NdOp::INT_RIGHT, Value, NdVar::cst(Lane * 8, 4));
      return S.binary(NdOp::INT_AND, Value, NdVar::cst(255, 4));
    };
    const auto Sum = S.binary(NdOp::INT_ADD, Byte(Left), Byte(Right));
    const auto Carry = S.binary(NdOp::INT_LESS, NdVar::cst(255, 4), Sum, 1);
    Overflow = S.binary(NdOp::BOOL_OR, Overflow, Carry, 1);
    if (!RD)
      continue;
    auto Value = S.binary(NdOp::INT_AND, Sum, NdVar::cst(255, 4));
    if (*Kind == mips::DSPInstruction::AddSaturatedUnsignedBytes) {
      const auto Saturated = S.makeTemp(4);
      S.emit(NdOp::SELECT, Saturated, {Carry, NdVar::cst(255, 4), Value});
      Value = Saturated;
    }
    if (Lane)
      Value = S.binary(NdOp::INT_LEFT, Value, NdVar::cst(Lane * 8, 4));
    Result = S.binary(NdOp::INT_OR, Result, Value);
  }
  const auto Flag =
      S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4), NdVar::cst(20, 4));
  WriteControl(S.binary(NdOp::INT_OR, ReadControl(), Flag));
  S.write(RD, Result);
  return true;
}

} // namespace neverd
