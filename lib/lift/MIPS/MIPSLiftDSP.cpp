//===- MIPSLiftDSP.cpp - Explicit DSP control and byte arithmetic --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compute each byte/halfword lane without cross-lane carry. DSPControl remains
/// explicit machine state even when the integer destination is discarded.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <optional>

namespace neverd {

NdVar MIPSLifter::State::select(NdVar Condition, NdVar Yes, NdVar No) {
  const auto Result = makeTemp(Yes.Size);
  emit(NdOp::SELECT, Result, {Condition, Yes, No});
  return Result;
}

NdVar MIPSLifter::State::accumulator(unsigned Bank) {
  return binary(NdOp::CONCAT,
                resize(NdVar::reg(mipsreg::accumulatorHI(Bank), Width), 4),
                resize(NdVar::reg(mipsreg::accumulatorLO(Bank), Width), 4), 8);
}

void MIPSLifter::State::writeAccumulator(unsigned Bank, NdVar Value) {
  const auto High = makeTemp(4);
  emit(NdOp::SUBBYTES, High, {Value, NdVar::cst(4, 4)});
  emit(NdOp::COPY, NdVar::reg(mipsreg::accumulatorHI(Bank), Width),
       {resize(High, Width, true)});
  emit(NdOp::COPY, NdVar::reg(mipsreg::accumulatorLO(Bank), Width),
       {resize(resize(Value, 4), Width, true)});
  if (!Bank)
    emit(NdOp::COPY, NdVar::reg(mipsreg::AC0Pending, 1), {NdVar::cst(0, 1)});
}

void MIPSLifter::State::discardHILO() {
  emitIntrinsic(Intrinsic::MIPSUnpredictable, NdVar::reg(mipsreg::HI, Width));
  emitIntrinsic(Intrinsic::MIPSUnpredictable, NdVar::reg(mipsreg::LO, Width));
  emit(NdOp::COPY, NdVar::reg(mipsreg::AC0Pending, 1), {NdVar::cst(0, 1)});
}

bool MIPSLifter::hasClassicDSP64() const {
  return HasDSP && isMIPS64(Target) && Mode != InstructionMode::MicroMIPS &&
         Mode != InstructionMode::MIPS16 && ISARevision == 2;
}
bool MIPSLifter::isDSPIndexedDoubleword(uint32_t Word) const {
  return HasDSP && isMIPS64(Target) && (Word & 0xfc0007ffu) == 0x7c00020au;
}

bool MIPSLifter::liftDSP(State &S, uint32_t Word) const {
  if (!HasDSP || Mode == InstructionMode::MIPS16)
    return false;
  if (S.Width == 8 && isDSPIndexedDoubleword(Word)) {
    const unsigned Base = (Word >> 21) & 31, Index = (Word >> 16) & 31;
    const unsigned Destination = (Word >> 11) & 31;
    return liftMemory(S, (55u << 26) | (Destination << 16),
                      S.binary(NdOp::INT_ADD, S.read(Base), S.read(Index)));
  }
  if (hasClassicDSP64() && liftDSP64Packed(S, Word))
    return true;
  const auto Kind = mips::dspInstruction(Word);
  if (!Kind)
    return liftDSPAdditional(S, Word);
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
  using DSP = mips::DSPInstruction;
  const unsigned Bank = RD & 3;

  const auto Sticky = [&](NdVar Set, unsigned Bit) {
    WriteControl(S.binary(
        NdOp::INT_OR, ReadControl(),
        S.binary(NdOp::INT_LEFT, S.resize(Set, 4), NdVar::cst(Bit, 4))));
  };
  switch (*Kind) {
  case DSP::ExtractWord:
  case DSP::ExtractVariableWord:
  case DSP::ExtractRoundedWord:
  case DSP::ExtractVariableRoundedWord:
  case DSP::ExtractSaturatedWord:
  case DSP::ExtractVariableSaturatedWord:
  case DSP::ExtractSaturatedHalf:
  case DSP::ExtractVariableSaturatedHalf:
  case DSP::ExtractPosition:
  case DSP::ExtractVariablePosition:
  case DSP::ExtractPositionDecrement:
  case DSP::ExtractVariablePositionDecrement: {
    const auto Size = (Word & 0x40) ? S.binary(NdOp::INT_AND, S.read(RS, 4),
                                               NdVar::cst(31, 4))
                                    : NdVar::cst(RS, 4);
    const bool Decrement = *Kind == DSP::ExtractPositionDecrement ||
                           *Kind == DSP::ExtractVariablePositionDecrement;
    if (Decrement || *Kind == DSP::ExtractPosition ||
        *Kind == DSP::ExtractVariablePosition) {
      const auto Control = ReadControl();
      const auto Position = S.binary(NdOp::INT_AND, Control, NdVar::cst(63, 4));
      const auto Failed = S.binary(NdOp::INT_LESS, Position, Size, 1);
      const auto Offset = S.binary(NdOp::INT_SUB, Position, Size);
      auto SuccessState =
          S.binary(NdOp::INT_AND, Control, NdVar::cst(~0x4000u, 4));
      if (Decrement) {
        const auto Next = S.binary(NdOp::INT_SUB, Offset, NdVar::cst(1, 4));
        SuccessState =
            S.binary(NdOp::INT_OR,
                     S.binary(NdOp::INT_AND, SuccessState, NdVar::cst(~63u, 4)),
                     S.binary(NdOp::INT_AND, Next, NdVar::cst(63, 4)));
      }
      WriteControl(S.select(
          Failed, S.binary(NdOp::INT_OR, Control, NdVar::cst(0x4000, 4)),
          SuccessState));
      if (RT) {
        const auto Mask =
            S.binary(NdOp::INT_RIGHT, NdVar::cst(UINT32_MAX, 4),
                     S.binary(NdOp::INT_SUB, NdVar::cst(31, 4), Size));
        const auto SafeOffset =
            S.binary(NdOp::INT_AND, Offset, NdVar::cst(63, 4));
        const auto Part = [&](unsigned Reg) {
          return S.select(Failed, NdVar::cst(0, 4),
                          S.resize(NdVar::reg(Reg, S.Width), 4));
        };
        const auto Acc =
            S.binary(NdOp::CONCAT, Part(mipsreg::accumulatorHI(Bank)),
                     Part(mipsreg::accumulatorLO(Bank)), 8);
        const auto Bits = S.resize(
            S.binary(NdOp::INT_RIGHT, Acc, S.resize(SafeOffset, 8)), 4);
        const auto Result = S.makeTemp(4);
        S.emitIntrinsic(Intrinsic::MIPSExtractResult, Result,
                        {Failed, S.binary(NdOp::INT_AND, Bits, Mask)});
        S.write(RT, Result);
      }
      return true;
    }

    const bool Half = *Kind == DSP::ExtractSaturatedHalf ||
                      *Kind == DSP::ExtractVariableSaturatedHalf;
    const auto Acc = S.accumulator(Bank);
    const auto Truncated = S.binary(NdOp::INT_ASHR, Acc, S.resize(Size, 8));
    auto Rounded = Truncated;
    if (!Half) {
      // Read the discarded bit without a negative shift when the count is zero.
      const auto Previous = S.binary(
          NdOp::INT_AND, S.binary(NdOp::INT_SUB, Size, NdVar::cst(1, 4)),
          NdVar::cst(31, 4));
      const auto Round = S.binary(
          NdOp::INT_AND, S.binary(NdOp::INT_RIGHT, Acc, S.resize(Previous, 8)),
          NdVar::cst(1, 8));
      Rounded = S.binary(
          NdOp::INT_ADD, Truncated,
          S.select(S.binary(NdOp::INT_NOTEQUAL, Size, NdVar::cst(0, 4), 1),
                   Round, NdVar::cst(0, 8)));
    }
    const auto Min = NdVar::cst(Half ? INT16_MIN : INT32_MIN, 8);
    const auto Max = NdVar::cst(Half ? INT16_MAX : INT32_MAX, 8);
    const auto Below = S.binary(NdOp::INT_SLESS, Truncated, Min, 1);
    const auto Above = S.binary(NdOp::INT_SLESS, Max, Rounded, 1);
    // EXTR.W also reports overflow of the rounded alternative. A negative
    // value that rounds back into range still sets OUflag from the first test.
    Sticky(S.binary(NdOp::BOOL_OR, Below, Above, 1), 23);
    const bool Saturate = Half || *Kind == DSP::ExtractSaturatedWord ||
                          *Kind == DSP::ExtractVariableSaturatedWord;
    auto Result =
        (*Kind == DSP::ExtractWord || *Kind == DSP::ExtractVariableWord)
            ? Truncated
            : Rounded;
    if (Saturate)
      Result = S.select(Above, Max, S.select(Below, Min, Result));
    S.write(RT, S.resize(Result, 4));
    return true;
  }
  case DSP::DotAddSaturatedLong:
  case DSP::DotSubSaturatedLong:
  case DSP::MultiplyAccumulateSaturatedLeft:
  case DSP::MultiplyAccumulateSaturatedRight:
  case DSP::MultiplyAccumulateLeft:
  case DSP::MultiplyAccumulateRight: {
    const bool Long =
        *Kind == DSP::DotAddSaturatedLong || *Kind == DSP::DotSubSaturatedLong;
    const bool Right = *Kind == DSP::MultiplyAccumulateRight ||
                       *Kind == DSP::MultiplyAccumulateSaturatedRight;
    const auto Part = [&](unsigned R) {
      if (Long)
        return S.read(R, 4);
      const auto V = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, V, {S.read(R, 4), NdVar::cst(Right ? 0 : 2, 4)});
      return V;
    };
    const auto A = Part(RS), B = Part(RT);
    const auto Minimum = NdVar::cst(Long ? 0x80000000u : 0x8000u, A.Size);
    auto Overflow =
        S.binary(NdOp::BOOL_AND, S.binary(NdOp::INT_EQUAL, A, Minimum, 1),
                 S.binary(NdOp::INT_EQUAL, B, Minimum, 1), 1);
    const auto Product =
        S.binary(NdOp::INT_MULT, S.resize(A, 8, true), S.resize(B, 8, true));
    const auto Fraction =
        S.select(Overflow, NdVar::cst(Long ? INT64_MAX : INT32_MAX, 8),
                 S.binary(NdOp::INT_LEFT, Product, NdVar::cst(1, 8)));
    const auto Old = S.accumulator(Bank);
    const bool Sub = *Kind == DSP::DotSubSaturatedLong;
    auto Sum = S.binary(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, Old, Fraction);
    if (Long) {
      const auto Range =
          S.binary(Sub ? NdOp::INT_SBOR : NdOp::INT_SOVF, Old, Fraction, 1);
      const auto Limit =
          S.select(S.binary(NdOp::INT_SLESS, Old, NdVar::cst(0, 8), 1),
                   NdVar::cst(INT64_MIN, 8), NdVar::cst(INT64_MAX, 8));
      Sum = S.select(Range, Limit, Sum);
      Overflow = S.binary(NdOp::BOOL_OR, Range, Overflow, 1);
    } else if (*Kind == DSP::MultiplyAccumulateSaturatedLeft ||
               *Kind == DSP::MultiplyAccumulateSaturatedRight) {
      const auto Min = NdVar::cst(INT32_MIN, 8), Max = NdVar::cst(INT32_MAX, 8);
      const auto Low = S.binary(NdOp::INT_SLESS, Sum, Min, 1),
                 High = S.binary(NdOp::INT_SLESS, Max, Sum, 1);
      Sum = S.select(Low, Min, S.select(High, Max, Sum));
      Overflow = S.binary(NdOp::BOOL_OR, Overflow,
                          S.binary(NdOp::BOOL_OR, Low, High, 1), 1);
    }
    Sticky(Overflow, 16 + Bank);
    S.writeAccumulator(Bank, Sum);
    return true;
  }
  default:
    break;
  }
  const bool DotBytes = *Kind == mips::DSPInstruction::DotAddLeftBytes ||
                        *Kind == mips::DSPInstruction::DotAddRightBytes ||
                        *Kind == mips::DSPInstruction::DotSubLeftBytes ||
                        *Kind == mips::DSPInstruction::DotSubRightBytes;
  const bool DotFractional =
      *Kind == mips::DSPInstruction::DotAddFractionalHalfwords ||
      *Kind == mips::DSPInstruction::DotSubFractionalHalfwords ||
      *Kind == mips::DSPInstruction::MultiplySubtractFractionalHalfwords;
  if (DotBytes || DotFractional) {
    const auto A = S.read(RS, 4), B = S.read(RT, 4);
    const auto Old = S.accumulator(Bank);
    const bool Left = *Kind == mips::DSPInstruction::DotAddLeftBytes ||
                      *Kind == mips::DSPInstruction::DotSubLeftBytes;
    const bool Subtract =
        *Kind == mips::DSPInstruction::DotSubLeftBytes ||
        *Kind == mips::DSPInstruction::DotSubRightBytes ||
        *Kind == mips::DSPInstruction::DotSubFractionalHalfwords;
    const bool Difference =
        *Kind == mips::DSPInstruction::MultiplySubtractFractionalHalfwords;
    NdVar Sum = NdVar::cst(0, 8), Overflow = NdVar::cst(0, 1);
    for (unsigned I = 0; I != 2; ++I) {
      const uint16_t Bytes = DotBytes ? 1 : 2;
      const unsigned Offset = DotBytes ? I + (Left ? 2 : 0) : 2 * (1 - I);
      const auto Extract = [&](NdVar V) {
        const auto Part = S.makeTemp(Bytes);
        S.emit(NdOp::SUBBYTES, Part, {V, NdVar::cst(Offset, 4)});
        return Part;
      };
      const auto X = Extract(A), Y = Extract(B);
      auto Product = S.binary(NdOp::INT_MULT, S.resize(X, 8, DotFractional),
                              S.resize(Y, 8, DotFractional));
      if (DotFractional) {
        const auto Saturates =
            S.binary(NdOp::BOOL_AND,
                     S.binary(NdOp::INT_EQUAL, X, NdVar::cst(0x8000, 2), 1),
                     S.binary(NdOp::INT_EQUAL, Y, NdVar::cst(0x8000, 2), 1), 1);
        const auto Clamped = S.makeTemp(8);
        S.emit(NdOp::SELECT, Clamped,
               {Saturates, NdVar::cst(0x7fffffff, 8),
                S.binary(NdOp::INT_LEFT, Product, NdVar::cst(1, 8))});
        Product = Clamped;
        Overflow = S.binary(NdOp::BOOL_OR, Overflow, Saturates, 1);
      }
      Sum = S.binary(Difference && I ? NdOp::INT_SUB : NdOp::INT_ADD, Sum,
                     Product);
    }
    const auto Result =
        S.binary(Subtract ? NdOp::INT_SUB : NdOp::INT_ADD, Old, Sum);
    if (DotFractional)
      Sticky(Overflow, 16 + Bank);
    S.writeAccumulator(Bank, Result);
    return true;
  }
  if (*Kind == mips::DSPInstruction::ReadControl) {
    const uint32_t Bits = mips::dspControlMask(Word >> 16, S.Width == 8);
    if (!RD)
      S.emit(NdOp::NOP, {}, {});
    else
      S.write(RD,
              Bits ? S.binary(NdOp::INT_AND, ReadControl(), NdVar::cst(Bits, 4))
                   : NdVar::cst(0, 4),
              false);
    return true;
  }
  if (*Kind == mips::DSPInstruction::WriteControl) {
    const uint32_t Bits = mips::dspControlMask(Word >> 11, S.Width == 8);
    if (!Bits) {
      // WRDSP with no selected fields has no architectural effect in the
      // admitted DSP-enabled profile. This is a defined encoding, not a
      // fallback.
      S.emit(NdOp::NOP, {}, {});
      return true;
    }
    auto Value = S.binary(NdOp::INT_AND, S.read(RS, 4), NdVar::cst(Bits, 4));
    const auto ControlBits = mips::dspControlMask(63, S.Width == 8);
    if (Bits != ControlBits)
      Value = S.binary(NdOp::INT_OR, Value,
                       S.binary(NdOp::INT_AND, ReadControl(),
                                NdVar::cst(ControlBits & ~Bits, 4)));
    WriteControl(Value);
    return true;
  }

  const bool ExpandedMultiply =
      *Kind == mips::DSPInstruction::MultiplyExpandedLeftHalfword ||
      *Kind == mips::DSPInstruction::MultiplyExpandedRightHalfword;
  const bool FractionalMultiply =
      ExpandedMultiply ||
      *Kind == mips::DSPInstruction::MultiplyRoundedHalfwords;
  if (FractionalMultiply || *Kind == mips::DSPInstruction::MultiplyLeftBytes ||
      *Kind == mips::DSPInstruction::MultiplyRightBytes) {
    const auto A = S.read(RS, 4), B = S.read(RT, 4);
    NdVar Result = NdVar::cst(0, 4), Overflow = NdVar::cst(0, 1);
    for (unsigned I = 0; I != (ExpandedMultiply ? 1u : 2u); ++I) {
      const auto X = S.makeTemp(FractionalMultiply ? 2 : 1);
      const auto Y = S.makeTemp(2);
      const unsigned Offset =
          ExpandedMultiply
              ? (*Kind == mips::DSPInstruction::MultiplyExpandedLeftHalfword
                     ? 2
                     : 0)
          : FractionalMultiply
              ? 2 * I
              : I + (*Kind == mips::DSPInstruction::MultiplyLeftBytes ? 2 : 0);
      S.emit(NdOp::SUBBYTES, X, {A, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, Y,
             {B, NdVar::cst(ExpandedMultiply ? Offset : 2 * I, 4)});
      auto Product =
          S.binary(NdOp::INT_MULT, S.resize(X, 8, FractionalMultiply),
                   S.resize(Y, 8, FractionalMultiply));
      NdVar Saturated;
      if (FractionalMultiply) {
        Saturated =
            S.binary(NdOp::BOOL_AND,
                     S.binary(NdOp::INT_EQUAL, X, NdVar::cst(0x8000, 2), 1),
                     S.binary(NdOp::INT_EQUAL, Y, NdVar::cst(0x8000, 2), 1), 1);
        // Widen before rounding, including negative halfway products.
        Product = ExpandedMultiply
                      ? S.binary(NdOp::INT_LEFT, Product, NdVar::cst(1, 8))
                      : S.binary(NdOp::INT_ASHR,
                                 S.binary(NdOp::INT_ADD, Product,
                                          NdVar::cst(0x4000, 8)),
                                 NdVar::cst(15, 8));
      } else {
        Saturated = S.binary(NdOp::INT_LESS, NdVar::cst(0xffff, 8), Product, 1);
      }
      Overflow = S.binary(NdOp::BOOL_OR, Overflow, Saturated, 1);
      const uint16_t Bytes = ExpandedMultiply ? 4 : 2;
      const auto Lane = S.makeTemp(Bytes);
      S.emit(NdOp::SELECT, Lane,
             {Saturated,
              NdVar::cst(ExpandedMultiply     ? 0x7fffffff
                         : FractionalMultiply ? 0x7fff
                                              : 0xffff,
                         Bytes),
              S.resize(Product, Bytes)});
      Result = S.binary(
          NdOp::INT_OR, Result,
          S.binary(NdOp::INT_LEFT, S.resize(Lane, 4), NdVar::cst(16 * I, 4)));
    }
    WriteControl(S.binary(
        NdOp::INT_OR, ReadControl(),
        S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4), NdVar::cst(21, 4))));
    S.write(RD, Result);
    // DSP r1 preserves ac1-ac3, but every GPR-targeting multiply invalidates
    // ac0, even if the destination is zero. Do not expose stale HI/LO values.
    S.discardHILO();
    return true;
  }

  const bool PackRounded =
      *Kind == mips::DSPInstruction::PackRoundedSaturatedHalfwords;
  if (PackRounded ||
      *Kind == mips::DSPInstruction::PackSaturatedUnsignedBytes) {
    const auto A = S.read(RS, 4), B = S.read(RT, 4);
    NdVar Result = NdVar::cst(0, 4), Overflow = NdVar::cst(0, 1);
    const unsigned Count = PackRounded ? 2 : 4;
    for (unsigned I = 0; I != Count; ++I) {
      const auto Source = I < Count / 2 ? B : A;
      NdVar Value, Saturated;
      if (PackRounded) {
        const auto Rounded = S.binary(NdOp::INT_ADD, S.resize(Source, 8, true),
                                      NdVar::cst(0x8000, 8));
        Saturated =
            S.binary(NdOp::INT_SLESS, NdVar::cst(0x7fffffff, 8), Rounded, 1);
        Value = S.makeTemp(4);
        S.emit(NdOp::SELECT, Value,
               {Saturated, NdVar::cst(0x7fff, 4),
                S.binary(NdOp::INT_AND,
                         S.resize(S.binary(NdOp::INT_ASHR, Rounded,
                                           NdVar::cst(16, 8)),
                                  4),
                         NdVar::cst(0xffff, 4))});
      } else {
        const auto Lane = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Lane, {Source, NdVar::cst((I % 2) * 2, 4)});
        const auto Negative =
            S.binary(NdOp::INT_SLESS, Lane, NdVar::cst(0, 2), 1);
        const auto High =
            S.binary(NdOp::INT_SLESS, NdVar::cst(0x7f80, 2), Lane, 1);
        Saturated = S.binary(NdOp::BOOL_OR, Negative, High, 1);
        const auto Positive = S.makeTemp(2), Clamped = S.makeTemp(2);
        S.emit(NdOp::SELECT, Positive,
               {High, NdVar::cst(255, 2),
                S.binary(NdOp::INT_RIGHT, Lane, NdVar::cst(7, 2))});
        S.emit(NdOp::SELECT, Clamped, {Negative, NdVar::cst(0, 2), Positive});
        Value = S.resize(Clamped, 4);
      }
      Overflow = S.binary(NdOp::BOOL_OR, Overflow, Saturated, 1);
      Result = S.binary(NdOp::INT_OR, Result,
                        S.binary(NdOp::INT_LEFT, Value,
                                 NdVar::cst(I * (PackRounded ? 16 : 8), 4)));
    }
    WriteControl(S.binary(
        NdOp::INT_OR, ReadControl(),
        S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4), NdVar::cst(22, 4))));
    S.write(RD, Result);
    return true;
  }

  const bool CompareGPR =
      *Kind == mips::DSPInstruction::CompareGPRBytesEqual ||
      *Kind == mips::DSPInstruction::CompareGPRBytesLess ||
      *Kind == mips::DSPInstruction::CompareGPRBytesLessEqual;
  const bool Compare = *Kind == mips::DSPInstruction::CompareEqualBytes ||
                       *Kind == mips::DSPInstruction::CompareLessBytes ||
                       *Kind == mips::DSPInstruction::CompareLessEqualBytes;
  const bool Pick = *Kind == mips::DSPInstruction::PickBytes ||
                    *Kind == mips::DSPInstruction::PickHalfwords;
  if (Compare || CompareGPR || Pick ||
      *Kind == mips::DSPInstruction::ModularSubtract) {
    if (!Compare && !RD) {
      S.emit(NdOp::NOP, {}, {});
      return true;
    }
    const auto A = S.read(RS, 4), B = S.read(RT, 4);
    if (*Kind == mips::DSPInstruction::ModularSubtract) {
      const auto Last = S.binary(NdOp::INT_AND,
                                 S.binary(NdOp::INT_RIGHT, B, NdVar::cst(8, 4)),
                                 NdVar::cst(0xffff, 4));
      const auto Difference = S.binary(
          NdOp::INT_SUB, A, S.binary(NdOp::INT_AND, B, NdVar::cst(255, 4)));
      const auto Result = S.makeTemp(4);
      S.emit(NdOp::SELECT, Result,
             {S.binary(NdOp::INT_EQUAL, A, NdVar::cst(0, 4), 1), Last,
              Difference});
      S.write(RD, Result);
      return true;
    }
    const auto Control = CompareGPR ? NdVar{} : ReadControl();
    const uint16_t Bytes = *Kind == mips::DSPInstruction::PickHalfwords ? 2 : 1;
    NdVar Result = NdVar::cst(0, 4);
    for (unsigned I = 0; I != 4u / Bytes; ++I) {
      const auto Lane = [&](NdVar Value) {
        const auto Part = S.makeTemp(Bytes);
        S.emit(NdOp::SUBBYTES, Part, {Value, NdVar::cst(I * Bytes, 4)});
        return Part;
      };
      NdVar Value;
      if (Compare || CompareGPR) {
        const NdOp Opcode =
            *Kind == mips::DSPInstruction::CompareEqualBytes ||
                    *Kind == mips::DSPInstruction::CompareGPRBytesEqual
                ? NdOp::INT_EQUAL
            : *Kind == mips::DSPInstruction::CompareLessBytes ||
                    *Kind == mips::DSPInstruction::CompareGPRBytesLess
                ? NdOp::INT_LESS
                : NdOp::INT_LESSEQUAL;
        Value = S.resize(S.binary(Opcode, Lane(A), Lane(B), 1), 4);
      } else {
        const auto Condition =
            S.binary(NdOp::INT_AND,
                     S.binary(NdOp::INT_RIGHT, Control, NdVar::cst(24 + I, 4)),
                     NdVar::cst(1, 4));
        const auto Selected = S.makeTemp(Bytes);
        S.emit(NdOp::SELECT, Selected, {Condition, Lane(A), Lane(B)});
        Value = S.resize(Selected, 4);
      }
      Result = S.binary(NdOp::INT_OR, Result,
                        S.binary(NdOp::INT_LEFT, Value,
                                 NdVar::cst(Compare      ? 24 + I
                                            : CompareGPR ? I
                                                         : I * Bytes * 8,
                                            4)));
    }
    if (Compare)
      WriteControl(S.binary(
          NdOp::INT_OR,
          S.binary(NdOp::INT_AND, Control, NdVar::cst(~0x0f000000u, 4)),
          Result));
    else
      S.write(RD, Result);
    return true;
  }

  const bool SetCarry = *Kind == mips::DSPInstruction::AddSetCarry;
  const bool WithCarry = *Kind == mips::DSPInstruction::AddWithCarry;
  const bool WordSaturate =
      *Kind == mips::DSPInstruction::AddSaturatedSignedWord ||
      *Kind == mips::DSPInstruction::SubSaturatedSignedWord;
  if (SetCarry || WithCarry || WordSaturate) {
    const auto A = S.resize(S.read(RS, 4), 8, !SetCarry);
    const auto B = S.resize(S.read(RT, 4), 8, !SetCarry);
    const auto Control = ReadControl();
    auto Sum = S.binary(*Kind == mips::DSPInstruction::SubSaturatedSignedWord
                            ? NdOp::INT_SUB
                            : NdOp::INT_ADD,
                        A, B);
    if (WithCarry) {
      const auto Carry = S.binary(
          NdOp::INT_AND, S.binary(NdOp::INT_RIGHT, Control, NdVar::cst(13, 4)),
          NdVar::cst(1, 4));
      Sum = S.binary(NdOp::INT_ADD, Sum, S.resize(Carry, 8));
    }
    if (SetCarry) {
      const auto Carry = S.binary(
          NdOp::INT_LEFT,
          S.resize(S.binary(NdOp::INT_RIGHT, Sum, NdVar::cst(32, 8)), 4),
          NdVar::cst(13, 4));
      WriteControl(S.binary(
          NdOp::INT_OR,
          S.binary(NdOp::INT_AND, Control, NdVar::cst(~0x2000u, 4)), Carry));
    } else {
      const auto Low = S.binary(NdOp::INT_SLESS, Sum,
                                NdVar::cst(UINT64_C(0xffffffff80000000), 8), 1);
      const auto High =
          S.binary(NdOp::INT_SLESS, NdVar::cst(0x7fffffff, 8), Sum, 1);
      const auto Overflow = S.binary(NdOp::BOOL_OR, Low, High, 1);
      WriteControl(S.binary(
          NdOp::INT_OR, Control,
          S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4), NdVar::cst(20, 4))));
      if (WordSaturate) {
        const auto Bound = S.makeTemp(4), Result = S.makeTemp(4);
        S.emit(NdOp::SELECT, Bound,
               {Low, NdVar::cst(0x80000000, 4), NdVar::cst(0x7fffffff, 4)});
        S.emit(NdOp::SELECT, Result, {Overflow, Bound, S.resize(Sum, 4)});
        Sum = Result;
      }
    }
    S.write(RD, S.resize(Sum, 4));
    return true;
  }

  struct LeftForm {
    uint16_t Bytes;
    bool Variable, Saturated;
  };
  std::optional<LeftForm> LeftShift;
  switch (*Kind) {
#define NEVERD_MIPS_DSP_LEFT(Name, Mask, Value, Bytes, Variable, Saturated)    \
  case mips::DSPInstruction::Name:                                             \
    LeftShift = LeftForm{Bytes, Variable, Saturated};                          \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSDSP.def"
  default:
    break;
  }
  const bool Absolute =
      *Kind == mips::DSPInstruction::AbsoluteSaturatedHalfwords ||
      *Kind == mips::DSPInstruction::AbsoluteSaturatedWord;
  if (Absolute || LeftShift) {
    const uint16_t Bytes =
        LeftShift ? LeftShift->Bytes
        : *Kind == mips::DSPInstruction::AbsoluteSaturatedWord ? 4
                                                               : 2;
    const unsigned Bits = Bytes * 8;
    const auto Source = S.read(RT, 4);
    const auto Count = !LeftShift ? NdVar::cst(0, 8)
                       : LeftShift->Variable
                           ? S.resize(S.binary(NdOp::INT_AND, S.read(RS, 4),
                                               NdVar::cst(Bits - 1, 4)),
                                      8)
                           : NdVar::cst(RS, 8);
    NdVar Result = NdVar::cst(0, 4), Overflow = NdVar::cst(0, 1);
    for (unsigned I = 0; I != 4u / Bytes; ++I) {
      const auto Lane = S.makeTemp(Bytes);
      S.emit(NdOp::SUBBYTES, Lane, {Source, NdVar::cst(I * Bytes, 4)});
      const auto Wide = S.resize(Lane, 8, Bytes != 1);
      auto Value = S.binary(NdOp::INT_LEFT, Wide, Count);
      const auto Negative =
          S.binary(NdOp::INT_SLESS, Wide, NdVar::cst(0, 8), 1);
      if (Absolute) {
        Value = S.makeTemp(8);
        S.emit(
            NdOp::SELECT, Value,
            {Negative, S.binary(NdOp::INT_SUB, NdVar::cst(0, 8), Wide), Wide});
      }
      // The widened shift has at most 63 significant bits. Comparing it to
      // the reconstructed lane detects every lost sign/unsigned bit, also at
      // count zero, without an undefined shift by the lane width.
      const auto Truncated = S.resize(Value, Bytes);
      const auto Lost = S.binary(NdOp::INT_NOTEQUAL, Value,
                                 S.resize(Truncated, 8, Bytes != 1), 1);
      Overflow = S.binary(NdOp::BOOL_OR, Overflow, Lost, 1);
      auto Packed = Truncated;
      if (Absolute || LeftShift->Saturated) {
        const auto Bound = S.makeTemp(Bytes);
        const uint64_t Sign = UINT64_C(1) << (Bits - 1);
        S.emit(NdOp::SELECT, Bound,
               {Absolute ? NdVar::cst(0, 1) : Negative, NdVar::cst(Sign, Bytes),
                NdVar::cst(Sign - 1, Bytes)});
        Packed = S.makeTemp(Bytes);
        S.emit(NdOp::SELECT, Packed, {Lost, Bound, Truncated});
      }
      Result = S.binary(NdOp::INT_OR, Result,
                        S.binary(NdOp::INT_LEFT, S.resize(Packed, 4),
                                 NdVar::cst(I * Bits, 4)));
    }
    WriteControl(S.binary(NdOp::INT_OR, ReadControl(),
                          S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4),
                                   NdVar::cst(Absolute ? 20 : 22, 4))));
    S.write(RD, Result);
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
      const auto Input = S.read(RT, 4);
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
      Result = S.binary(NdOp::INT_AND, S.read(RT, 4), NdVar::cst(0xffff, 4));
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
      const auto Input = S.read(RS, 4);
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
    const auto Right = S.read(RT, 4);
    NdVar Result = NdVar::cst(0, 4);
    if (Pack) {
      const auto Left = S.read(RS, 4);
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
      const auto Count =
          Shift->Variable
              ? S.binary(NdOp::INT_AND, S.read(RS, 4), NdVar::cst(Bits - 1, 4))
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

  const auto Left = S.read(RS, 4);
  const auto Right = S.read(RT, 4);
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
