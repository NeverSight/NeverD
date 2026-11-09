//===- MIPSLiftDSP.cpp - Explicit DSP control and byte arithmetic --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compute each byte/halfword lane without cross-lane carry. DSPControl remains
/// explicit machine state even when the integer destination is discarded.
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/MIPS/MIPSDSP.h"
#include "neverd/lift/MIPS/MIPSLifter.h"

#include <optional>

namespace neverd {

bool MIPSLifter::liftDSP(State &S, uint32_t Word) const {
  if (!HasDSP || S.Width != 4 || isMIPSCompressedMode(Mode))
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

  struct Expansion {
    uint16_t Bytes, First, Step, Shift, OutBytes;
  };
  std::optional<Expansion> Expand;
  switch (*Kind) {
#define NEVERD_MIPS_DSP_EXPAND(Name, Value, Bytes, First, Step, Shift,         \
                               OutBytes)                                       \
  case mips::DSPInstruction::Name:                                             \
    Expand = Expansion{Bytes, First, Step, Shift, OutBytes};                   \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.def"
  default:
    break;
  }
  const bool Reverse = *Kind == mips::DSPInstruction::ReverseHalfwordBits;
  const bool Reduce = *Kind == mips::DSPInstruction::ReduceAddBytes;
  const bool Replicate =
      *Kind == mips::DSPInstruction::ReplicateBytes ||
      *Kind == mips::DSPInstruction::ReplicateHalfwords ||
      *Kind == mips::DSPInstruction::ReplicateVariableBytes ||
      *Kind == mips::DSPInstruction::ReplicateVariableHalfwords;
  if (Expand || Reverse || Reduce || Replicate) {
    if (!RD) {
      S.emit(NdOp::NOP, {}, {});
      return true;
    }
    NdVar Result = NdVar::cst(0, 4);
    if (Expand) {
      const auto Input = S.read(RT);
      for (unsigned I = 0; I != 4u / Expand->OutBytes; ++I) {
        const auto Part = S.makeTemp(Expand->Bytes);
        S.emit(NdOp::SUBBYTES, Part,
               {Input, NdVar::cst(Expand->First + I * Expand->Step, 4)});
        auto Value = S.resize(Part, 4);
        const unsigned Shift = Expand->Shift + I * Expand->OutBytes * 8;
        if (Shift)
          Value = S.binary(NdOp::INT_LEFT, Value, NdVar::cst(Shift, 4));
        Result = S.binary(NdOp::INT_OR, Result, Value);
      }
    } else if (Reverse) {
      Result = S.binary(NdOp::INT_AND, S.read(RT), NdVar::cst(0xffff, 4));
      for (unsigned Shift = 1; Shift <= 8; Shift *= 2) {
        const auto Mask = NdVar::cst(0xffffu / ((1u << Shift) + 1), 4);
        Result = S.binary(
            NdOp::INT_OR,
            S.binary(NdOp::INT_AND,
                     S.binary(NdOp::INT_RIGHT, Result, NdVar::cst(Shift, 4)),
                     Mask),
            S.binary(NdOp::INT_LEFT, S.binary(NdOp::INT_AND, Result, Mask),
                     NdVar::cst(Shift, 4)));
      }
    } else if (Reduce) {
      const auto Input = S.read(RS);
      for (unsigned I = 0; I != 4; ++I) {
        const auto Byte = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Byte, {Input, NdVar::cst(I, 4)});
        Result = S.binary(NdOp::INT_ADD, Result, S.resize(Byte, 4));
      }
    } else {
      const bool Half =
          *Kind == mips::DSPInstruction::ReplicateHalfwords ||
          *Kind == mips::DSPInstruction::ReplicateVariableHalfwords;
      const bool Variable =
          *Kind == mips::DSPInstruction::ReplicateVariableBytes ||
          *Kind == mips::DSPInstruction::ReplicateVariableHalfwords;
      const uint16_t Bytes = Half ? 2 : 1;
      const unsigned Immediate = (Word >> 16) & (Half ? 1023 : 255);
      const auto Value =
          Variable
              ? S.resize(S.read(RT, Bytes), 4)
              : NdVar::cst(Half ? uint16_t((Immediate ^ 512) - 512) : Immediate,
                           4);
      for (unsigned I = 0; I != 4u / Bytes; ++I)
        Result = S.binary(
            NdOp::INT_OR, Result,
            I ? S.binary(NdOp::INT_LEFT, Value, NdVar::cst(I * Bytes * 8, 4))
              : Value);
    }
    S.write(RD, Result);
    return true;
  }

  struct ShiftForm {
    uint16_t Bytes;
    bool Variable, Rounded;
  };
  std::optional<ShiftForm> Shift;
  switch (*Kind) {
#define NEVERD_MIPS_DSP_SHIFT(Name, Mask, Value, Bytes, Variable, Rounded)     \
  case mips::DSPInstruction::Name:                                             \
    Shift = ShiftForm{Bytes, Variable, Rounded};                               \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.def"
  default:
    break;
  }
  const bool Pack = *Kind == mips::DSPInstruction::PackHalfwords ||
                    *Kind == mips::DSPInstruction::PackBytes;
  if (Shift || Pack) {
    // These enabled-DSP forms have no state effect or input dependency when
    // their sole result is discarded.
    if (!RD) {
      S.emit(NdOp::NOP, {}, {});
      return true;
    }
    const auto Right = S.read(RT);
    NdVar Result = NdVar::cst(0, 4);
    if (Pack) {
      const auto Left = S.read(RS);
      const unsigned Bytes =
          *Kind == mips::DSPInstruction::PackHalfwords ? 2 : 1;
      for (unsigned I = 0; I != 4 / Bytes; ++I) {
        const bool FromLeft = I >= 2 / Bytes;
        const unsigned Offset = ((I % (2 / Bytes)) * 2 + 1) * Bytes;
        const auto Lane = S.makeTemp(Bytes);
        S.emit(NdOp::SUBBYTES, Lane,
               {FromLeft ? Left : Right, NdVar::cst(Offset, 4)});
        auto Value = S.resize(Lane, 4);
        if (I)
          Value = S.binary(NdOp::INT_LEFT, Value, NdVar::cst(I * Bytes * 8, 4));
        Result = S.binary(NdOp::INT_OR, Result, Value);
      }
    } else {
      const unsigned Bits = Shift->Bytes * 8;
      const uint16_t Wide = Shift->Bytes == 4 ? 8 : 4;
      const auto Count = Shift->Variable ? S.binary(NdOp::INT_AND, S.read(RS),
                                                    NdVar::cst(Bits - 1, 4))
                                         : NdVar::cst(RS, 4);
      for (unsigned I = 0; I != 4 / Shift->Bytes; ++I) {
        const auto Lane = S.makeTemp(Shift->Bytes);
        S.emit(NdOp::SUBBYTES, Lane, {Right, NdVar::cst(I * Shift->Bytes, 4)});
        auto Value = S.resize(Lane, Wide, Shift->Bytes != 1);
        // One extra precision bit implements rounding without count-1 or a
        // shift by the word width, and also gives the identity for count zero.
        if (Shift->Rounded)
          Value = S.binary(NdOp::INT_LEFT, Value, NdVar::cst(1, Wide));
        Value = S.binary(Shift->Bytes == 1 ? NdOp::INT_RIGHT : NdOp::INT_ASHR,
                         Value, S.resize(Count, Wide));
        if (Shift->Rounded)
          Value = S.binary(NdOp::INT_ASHR,
                           S.binary(NdOp::INT_ADD, Value, NdVar::cst(1, Wide)),
                           NdVar::cst(1, Wide));
        Value = S.resize(S.resize(Value, Shift->Bytes), 4);
        if (I)
          Value = S.binary(NdOp::INT_LEFT, Value, NdVar::cst(I * Bits, 4));
        Result = S.binary(NdOp::INT_OR, Result, Value);
      }
    }
    S.write(RD, Result);
    return true;
  }

  const auto Left = S.read(RS);
  const auto Right = S.read(RT);
  const bool Subtract =
      *Kind == mips::DSPInstruction::SubUnsignedBytes ||
      *Kind == mips::DSPInstruction::SubSaturatedUnsignedBytes ||
      *Kind == mips::DSPInstruction::SubSignedHalfwords ||
      *Kind == mips::DSPInstruction::SubSaturatedSignedHalfwords;
  const bool SignedHalfwords =
      *Kind == mips::DSPInstruction::AddSignedHalfwords ||
      *Kind == mips::DSPInstruction::AddSaturatedSignedHalfwords ||
      *Kind == mips::DSPInstruction::SubSignedHalfwords ||
      *Kind == mips::DSPInstruction::SubSaturatedSignedHalfwords;
  const bool Saturate =
      *Kind == mips::DSPInstruction::AddSaturatedUnsignedBytes ||
      *Kind == mips::DSPInstruction::SubSaturatedUnsignedBytes ||
      *Kind == mips::DSPInstruction::AddSaturatedSignedHalfwords ||
      *Kind == mips::DSPInstruction::SubSaturatedSignedHalfwords;
  const unsigned Bits = SignedHalfwords ? 16 : 8;
  const unsigned Mask = SignedHalfwords ? 65535 : 255;
  NdVar Result = NdVar::cst(0, 4);
  NdVar Overflow = NdVar::cst(0, 1);
  for (unsigned Lane = 0; Lane != 32 / Bits; ++Lane) {
    const auto Element = [&](NdVar Value) {
      if (Lane)
        Value = S.binary(NdOp::INT_RIGHT, Value, NdVar::cst(Lane * Bits, 4));
      if (SignedHalfwords)
        return S.resize(S.resize(Value, 2), 4, true);
      return S.binary(NdOp::INT_AND, Value, NdVar::cst(Mask, 4));
    };
    const auto A = Element(Left), B = Element(Right);
    const auto Arithmetic =
        S.binary(Subtract ? NdOp::INT_SUB : NdOp::INT_ADD, A, B);
    const auto Overflowed =
        SignedHalfwords
            ? S.binary(NdOp::BOOL_OR,
                       S.binary(NdOp::INT_SLESS, Arithmetic,
                                NdVar::cst(uint32_t(-32768), 4), 1),
                       S.binary(NdOp::INT_SLESS, NdVar::cst(32767, 4),
                                Arithmetic, 1),
                       1)
        : Subtract
            ? S.binary(NdOp::INT_LESS, A, B, 1)
            : S.binary(NdOp::INT_LESS, NdVar::cst(255, 4), Arithmetic, 1);
    Overflow = S.binary(NdOp::BOOL_OR, Overflow, Overflowed, 1);
    if (!RD)
      continue;
    auto Value = S.binary(NdOp::INT_AND, Arithmetic, NdVar::cst(Mask, 4));
    if (Saturate) {
      NdVar Bound = NdVar::cst(Subtract ? 0 : 255, 4);
      if (SignedHalfwords) {
        Bound = S.makeTemp(4);
        S.emit(NdOp::SELECT, Bound,
               {S.binary(NdOp::INT_SLESS, Arithmetic, NdVar::cst(0, 4), 1),
                NdVar::cst(0x8000, 4), NdVar::cst(0x7fff, 4)});
      }
      const auto Saturated = S.makeTemp(4);
      S.emit(NdOp::SELECT, Saturated, {Overflowed, Bound, Value});
      Value = Saturated;
    }
    if (Lane)
      Value = S.binary(NdOp::INT_LEFT, Value, NdVar::cst(Lane * Bits, 4));
    Result = S.binary(NdOp::INT_OR, Result, Value);
  }
  const auto Flag =
      S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4), NdVar::cst(20, 4));
  WriteControl(S.binary(NdOp::INT_OR, ReadControl(), Flag));
  S.write(RD, Result);
  return true;
}

} // namespace neverd
