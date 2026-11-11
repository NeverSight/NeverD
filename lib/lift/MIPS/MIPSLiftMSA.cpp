//===- MIPSLiftMSA.cpp - MSA integer lanes and logical vectors -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Register lane zero always denotes the low bits, independently of memory byte
/// order. Complete temporaries capture aliased inputs before a vector write.
//===----------------------------------------------------------------------===//

#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <array>
#include <optional>

namespace neverd {

bool MIPSLifter::liftMSA(State &S, uint32_t Word) const {
  if (!HasMSA || S.Width != 4 || isMIPSCompressedMode(Mode))
    return false;
  enum class Instruction {
#define NEVERD_MIPS_MSA(Name, Mask, Value) Name,
#include "neverd/lift/MIPS/MIPSMSA.def"
  };
  std::optional<Instruction> Kind;
#define NEVERD_MIPS_MSA(Name, Mask, Value)                                     \
  if ((Word & Mask) == Value)                                                  \
    Kind = Instruction::Name;
#include "neverd/lift/MIPS/MIPSMSA.def"
  if (!Kind)
    return false;
  const unsigned WD = (Word >> 6) & 31;
  const unsigned WS = (Word >> 11) & 31;
  const unsigned WT = (Word >> 16) & 31;
  const unsigned Index = WT & 3;
  const auto Lane = [&](unsigned Reg, unsigned I, uint16_t Bytes = 4) {
    const auto Value = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, Value,
           {NdVar::reg(mipsreg::w(Reg), 16), NdVar::cst(I * Bytes, 4)});
    return Value;
  };
  if (*Kind == Instruction::CopyWord) {
    // No scalar destination also means no lane is observed in the enabled
    // MSA profile. This is the defined architectural $zero behavior.
    if (WD)
      S.write(WD, Lane(WS, Index));
    else
      S.emit(NdOp::NOP, {}, {});
    return true;
  }
  const bool ImmediateLogic =
      *Kind == Instruction::AndImmediate || *Kind == Instruction::OrImmediate ||
      *Kind == Instruction::NorImmediate || *Kind == Instruction::XorImmediate;
  if (ImmediateLogic) {
    const auto Byte = uint64_t((Word >> 16) & 255);
    const auto Repeated = NdVar::cst(Byte * UINT64_C(0x0101010101010101), 8);
    const auto Mask = S.makeTemp(16);
    S.emit(NdOp::CONCAT, Mask, {Repeated, Repeated});
    const NdOp Opcode = *Kind == Instruction::AndImmediate   ? NdOp::INT_AND
                        : *Kind == Instruction::XorImmediate ? NdOp::INT_XOR
                                                             : NdOp::INT_OR;
    auto Result = S.binary(Opcode, NdVar::reg(mipsreg::w(WS), 16), Mask);
    if (*Kind == Instruction::NorImmediate) {
      const auto Inverted = S.makeTemp(16);
      S.emit(NdOp::INT_NOT, Inverted, {Result});
      Result = Inverted;
    }
    S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16), {Result});
    return true;
  }
  if (*Kind == Instruction::SelectVector ||
      *Kind == Instruction::MergeNonzeroVector ||
      *Kind == Instruction::MergeZeroVector) {
    const auto Old = NdVar::reg(mipsreg::w(WD), 16);
    const auto Source = NdVar::reg(mipsreg::w(WS), 16);
    const auto Other = NdVar::reg(mipsreg::w(WT), 16);
    const bool Select = *Kind == Instruction::SelectVector;
    const bool Zero = *Kind == Instruction::MergeZeroVector;
    const auto Mask = Select ? Old : Other;
    const auto Inverted = S.makeTemp(16);
    S.emit(NdOp::INT_NOT, Inverted, {Mask});
    const auto Yes = S.binary(NdOp::INT_AND, Mask,
                              Select ? Other
                              : Zero ? Old
                                     : Source);
    const auto No =
        S.binary(NdOp::INT_AND, Inverted, Select || Zero ? Source : Old);
    const auto Result = S.binary(NdOp::INT_OR, Yes, No);
    S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16), {Result});
    return true;
  }
  if (*Kind == Instruction::AndVector || *Kind == Instruction::OrVector ||
      *Kind == Instruction::NorVector || *Kind == Instruction::XorVector) {
    const NdOp Opcode = *Kind == Instruction::AndVector   ? NdOp::INT_AND
                        : *Kind == Instruction::XorVector ? NdOp::INT_XOR
                                                          : NdOp::INT_OR;
    auto Result = S.binary(Opcode, NdVar::reg(mipsreg::w(WS), 16),
                           NdVar::reg(mipsreg::w(WT), 16));
    if (*Kind == Instruction::NorVector) {
      const auto Inverted = S.makeTemp(16);
      S.emit(NdOp::INT_NOT, Inverted, {Result});
      Result = Inverted;
    }
    S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16), {Result});
    return true;
  }
  // I5 shares the integer lane semantics with 3R; only its right operand
  // differs. The descriptor has already validated the opcode/function bits.
  const bool ImmediateInteger = (Word & 63) == 6 || (Word & 63) == 7;
  const bool SignedImmediate = *Kind == Instruction::EqualImmediate ||
                               *Kind == Instruction::SignedLessImmediate ||
                               *Kind == Instruction::SignedLessEqualImmediate ||
                               *Kind == Instruction::SignedMaximumImmediate ||
                               *Kind == Instruction::SignedMinimumImmediate;
  const int64_t Immediate = SignedImmediate ? int(WT ^ 16) - 16 : int(WT);
  const auto Right = [&](unsigned I, uint16_t Bytes) {
    return ImmediateInteger ? NdVar::cst(uint64_t(Immediate), Bytes)
                            : Lane(WT, I, Bytes);
  };
  std::optional<NdOp> Comparison;
  switch (*Kind) {
  case Instruction::EqualImmediate:
  case Instruction::EqualVector:
    Comparison = NdOp::INT_EQUAL;
    break;
  case Instruction::SignedLessImmediate:
  case Instruction::SignedLessVector:
    Comparison = NdOp::INT_SLESS;
    break;
  case Instruction::UnsignedLessImmediate:
  case Instruction::UnsignedLessVector:
    Comparison = NdOp::INT_LESS;
    break;
  case Instruction::SignedLessEqualImmediate:
  case Instruction::SignedLessEqualVector:
    Comparison = NdOp::INT_SLESSEQUAL;
    break;
  case Instruction::UnsignedLessEqualImmediate:
  case Instruction::UnsignedLessEqualVector:
    Comparison = NdOp::INT_LESSEQUAL;
    break;
  default:
    break;
  }
  std::optional<NdOp> Shift, Bit;
  switch (*Kind) {
  case Instruction::ShiftLeftImmediate:
  case Instruction::ShiftLeftVector:
    Shift = NdOp::INT_LEFT;
    break;
  case Instruction::ShiftRightImmediate:
  case Instruction::ShiftRightVector:
    Shift = NdOp::INT_RIGHT;
    break;
  case Instruction::ShiftRightSignedImmediate:
  case Instruction::ShiftRightSignedVector:
    Shift = NdOp::INT_ASHR;
    break;
  case Instruction::ClearBitImmediate:
  case Instruction::ClearBitVector:
    Bit = NdOp::INT_AND;
    break;
  case Instruction::SetBitImmediate:
  case Instruction::SetBitVector:
    Bit = NdOp::INT_OR;
    break;
  case Instruction::NegateBitImmediate:
  case Instruction::NegateBitVector:
    Bit = NdOp::INT_XOR;
    break;
  default:
    break;
  }
  const bool Minimum = *Kind == Instruction::SignedMinimumVector ||
                       *Kind == Instruction::UnsignedMinimumVector ||
                       *Kind == Instruction::SignedMinimumImmediate ||
                       *Kind == Instruction::UnsignedMinimumImmediate;
  const bool Maximum = *Kind == Instruction::SignedMaximumVector ||
                       *Kind == Instruction::UnsignedMaximumVector ||
                       *Kind == Instruction::SignedMaximumImmediate ||
                       *Kind == Instruction::UnsignedMaximumImmediate;
  const bool SignedExtremum = *Kind == Instruction::SignedMinimumVector ||
                              *Kind == Instruction::SignedMaximumVector ||
                              *Kind == Instruction::SignedMinimumImmediate ||
                              *Kind == Instruction::SignedMaximumImmediate;
  const bool Absolute = *Kind == Instruction::AbsoluteMinimumVector ||
                        *Kind == Instruction::AbsoluteMaximumVector;
  const bool AddAbsolute = *Kind == Instruction::AddAbsoluteVector ||
                           *Kind == Instruction::AddSaturatedAbsoluteVector;
  const bool SaturatedSigned = *Kind == Instruction::AddSaturatedSignedVector ||
                               *Kind == Instruction::SubSaturatedSignedVector;
  const bool SaturatedUnsigned =
      *Kind == Instruction::AddSaturatedUnsignedVector ||
      *Kind == Instruction::SubSaturatedUnsignedVector;
  const bool AverageSigned = *Kind == Instruction::AverageSignedVector ||
                             *Kind == Instruction::RoundAverageSignedVector;
  const bool AverageUnsigned = *Kind == Instruction::AverageUnsignedVector ||
                               *Kind == Instruction::RoundAverageUnsignedVector;
  const bool RoundAverage = *Kind == Instruction::RoundAverageSignedVector ||
                            *Kind == Instruction::RoundAverageUnsignedVector;
  const bool SaturateImmediate =
      *Kind == Instruction::SaturateSignedImmediate ||
      *Kind == Instruction::SaturateUnsignedImmediate;
  const bool AbsoluteDifference =
      *Kind == Instruction::AbsoluteDifferenceSigned ||
      *Kind == Instruction::AbsoluteDifferenceUnsigned;
  const bool RoundShift = *Kind == Instruction::RoundShiftSignedVector ||
                          *Kind == Instruction::RoundShiftUnsignedVector ||
                          *Kind == Instruction::RoundShiftSignedImmediate ||
                          *Kind == Instruction::RoundShiftUnsignedImmediate;
  const bool InsertBits = *Kind == Instruction::InsertLeftVector ||
                          *Kind == Instruction::InsertRightVector ||
                          *Kind == Instruction::InsertLeftImmediate ||
                          *Kind == Instruction::InsertRightImmediate;
  const bool ImmediateBit =
      ((Shift || Bit || InsertBits) && (Word & 63) == 9) ||
      (RoundShift && (Word & 63) == 10) || SaturateImmediate;
  const bool CountBits = *Kind == Instruction::PopulationCount ||
                         *Kind == Instruction::LeadingZeroCount ||
                         *Kind == Instruction::LeadingOneCount;
  const unsigned Field = (Word >> 16) & 127;
  if (ImmediateBit && Field >= 120)
    return false;
  const uint16_t Bytes =
      ImmediateBit ? (Field < 64    ? 8
                      : Field < 96  ? 4
                      : Field < 112 ? 2
                                    : 1)
      : CountBits  ? 1u << ((Word >> 16) & 3)
      : *Kind == Instruction::FillWord || *Kind == Instruction::InsertWord
          ? 4
          : 1u << ((Word >> 21) & 3);
  std::array<NdVar, 16> Values;
  for (unsigned I = 0; I != 16u / Bytes; ++I) {
    if (*Kind == Instruction::FillWord)
      Values[I] = S.read(WS);
    else if (*Kind == Instruction::InsertWord)
      Values[I] = I == Index ? S.read(WS) : Lane(WD, I);
    else if (CountBits) {
      auto Input = Lane(WS, I, Bytes);
      if (*Kind == Instruction::LeadingOneCount) {
        const auto Inverted = S.makeTemp(Bytes);
        S.emit(NdOp::INT_NOT, Inverted, {Input});
        Input = Inverted;
      }
      Values[I] = S.makeTemp(Bytes);
      S.emit(*Kind == Instruction::PopulationCount ? NdOp::POPCOUNT
                                                   : NdOp::LZCOUNT,
             Values[I], {Input});
    } else if (SaturateImmediate) {
      const auto A = Lane(WS, I, Bytes);
      const unsigned Count = Field & (Bytes * 8 - 1);
      const bool Signed = *Kind == Instruction::SaturateSignedImmediate;
      const uint64_t Maximum =
          Signed ? (UINT64_C(1) << Count) - 1 : UINT64_MAX >> (63 - Count);
      const auto Upper = NdVar::cst(Maximum, Bytes);
      auto Result = S.makeTemp(Bytes);
      S.emit(NdOp::SELECT, Result,
             {S.binary(Signed ? NdOp::INT_SLESS : NdOp::INT_LESS, Upper, A, 1),
              Upper, A});
      if (Signed) {
        const auto Lower = NdVar::cst(~Maximum, Bytes);
        const auto Clamped = S.makeTemp(Bytes);
        S.emit(NdOp::SELECT, Clamped,
               {S.binary(NdOp::INT_SLESS, A, Lower, 1), Lower, Result});
        Result = Clamped;
      }
      Values[I] = Result;
    } else if (AbsoluteDifference) {
      const auto A = Lane(WS, I, Bytes), B = Lane(WT, I, Bytes);
      const auto Less = S.binary(*Kind == Instruction::AbsoluteDifferenceSigned
                                     ? NdOp::INT_SLESS
                                     : NdOp::INT_LESS,
                                 A, B, 1);
      // Decide the sign before the modular subtraction; the true magnitude
      // fits an unsigned lane even at signed min/max and in 64-bit lanes.
      Values[I] = S.makeTemp(Bytes);
      S.emit(
          NdOp::SELECT, Values[I],
          {Less, S.binary(NdOp::INT_SUB, B, A), S.binary(NdOp::INT_SUB, A, B)});
    } else if (RoundShift || InsertBits) {
      const auto A = Lane(WS, I, Bytes);
      if (InsertBits && ImmediateBit &&
          (Field & (Bytes * 8 - 1)) == Bytes * 8 - 1) {
        // A full-lane immediate insertion does not observe the old WD.
        Values[I] = A;
        continue;
      }
      const auto Limit = NdVar::cst(Bytes * 8 - 1, Bytes);
      const auto Count =
          ImmediateBit ? NdVar::cst(Field & (Bytes * 8 - 1), Bytes)
                       : S.binary(NdOp::INT_AND, Lane(WT, I, Bytes), Limit);
      if (RoundShift) {
        const bool Signed = *Kind == Instruction::RoundShiftSignedVector ||
                            *Kind == Instruction::RoundShiftSignedImmediate;
        const auto Shifted =
            S.binary(Signed ? NdOp::INT_ASHR : NdOp::INT_RIGHT, A, Count);
        // Mask before shifting: count zero must never evaluate a negative or
        // full-width shift, even if a later select discards the result.
        const auto Previous = S.binary(
            NdOp::INT_AND, S.binary(NdOp::INT_SUB, Count, NdVar::cst(1, Bytes)),
            Limit);
        const auto Round =
            S.binary(NdOp::INT_AND, S.binary(NdOp::INT_RIGHT, A, Previous),
                     NdVar::cst(1, Bytes));
        const auto Nonzero =
            S.binary(NdOp::INT_NOTEQUAL, Count, NdVar::cst(0, Bytes), 1);
        Values[I] =
            S.binary(NdOp::INT_ADD, Shifted,
                     S.binary(NdOp::INT_MULT, Round, S.resize(Nonzero, Bytes)));
      } else {
        const bool Left = *Kind == Instruction::InsertLeftVector ||
                          *Kind == Instruction::InsertLeftImmediate;
        const auto Complement = S.binary(NdOp::INT_SUB, Limit, Count);
        const auto Mask = S.binary(Left ? NdOp::INT_LEFT : NdOp::INT_RIGHT,
                                   NdVar::cst(UINT64_MAX, Bytes), Complement);
        const auto Inverse = S.makeTemp(Bytes);
        S.emit(NdOp::INT_NOT, Inverse, {Mask});
        Values[I] =
            S.binary(NdOp::INT_OR, S.binary(NdOp::INT_AND, A, Mask),
                     S.binary(NdOp::INT_AND, Lane(WD, I, Bytes), Inverse));
      }
    } else if (Shift || Bit) {
      const auto A = Lane(WS, I, Bytes);
      const auto Count = ImmediateBit
                             ? NdVar::cst(Field & (Bytes * 8 - 1), Bytes)
                             : S.binary(NdOp::INT_AND, Lane(WT, I, Bytes),
                                        NdVar::cst(Bytes * 8 - 1, Bytes));
      if (Shift)
        Values[I] = S.binary(*Shift, A, Count);
      else {
        auto Mask = S.binary(NdOp::INT_LEFT, NdVar::cst(1, Bytes), Count);
        if (*Kind == Instruction::ClearBitVector ||
            *Kind == Instruction::ClearBitImmediate) {
          const auto Inverted = S.makeTemp(Bytes);
          S.emit(NdOp::INT_NOT, Inverted, {Mask});
          Mask = Inverted;
        }
        Values[I] = S.binary(*Bit, A, Mask);
      }
    } else if (*Kind == Instruction::MultiplyVector ||
               *Kind == Instruction::MultiplyAddVector ||
               *Kind == Instruction::MultiplySubVector) {
      const auto Product =
          S.binary(NdOp::INT_MULT, Lane(WS, I, Bytes), Lane(WT, I, Bytes));
      Values[I] = *Kind == Instruction::MultiplyVector
                      ? Product
                      : S.binary(*Kind == Instruction::MultiplyAddVector
                                     ? NdOp::INT_ADD
                                     : NdOp::INT_SUB,
                                 Lane(WD, I, Bytes), Product);
    } else if (AverageSigned || AverageUnsigned) {
      const auto A = Lane(WS, I, Bytes), B = Lane(WT, I, Bytes);
      // Carry-free identities preserve the widened sum, including signed
      // negative ties and 64-bit lanes, without requiring a 128-bit carrier.
      const auto HalfDifference =
          S.binary(AverageSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT,
                   S.binary(NdOp::INT_XOR, A, B), NdVar::cst(1, Bytes));
      Values[I] =
          S.binary(RoundAverage ? NdOp::INT_SUB : NdOp::INT_ADD,
                   S.binary(RoundAverage ? NdOp::INT_OR : NdOp::INT_AND, A, B),
                   HalfDifference);
    } else if (SaturatedSigned || SaturatedUnsigned) {
      const auto A = Lane(WS, I, Bytes), B = Lane(WT, I, Bytes);
      const bool Subtract = *Kind == Instruction::SubSaturatedSignedVector ||
                            *Kind == Instruction::SubSaturatedUnsignedVector;
      const auto Arithmetic =
          S.binary(Subtract ? NdOp::INT_SUB : NdOp::INT_ADD, A, B);
      NdVar Overflow, Bound;
      if (SaturatedSigned) {
        auto Signs = S.binary(NdOp::INT_XOR, A, B);
        if (!Subtract) {
          const auto Inverted = S.makeTemp(Bytes);
          S.emit(NdOp::INT_NOT, Inverted, {Signs});
          Signs = Inverted;
        }
        Overflow = S.binary(NdOp::INT_SLESS,
                            S.binary(NdOp::INT_AND, Signs,
                                     S.binary(NdOp::INT_XOR, A, Arithmetic)),
                            NdVar::cst(0, Bytes), 1);
        const uint64_t SignBit = UINT64_C(1) << (Bytes * 8 - 1);
        Bound = S.makeTemp(Bytes);
        S.emit(NdOp::SELECT, Bound,
               {S.binary(NdOp::INT_SLESS, A, NdVar::cst(0, Bytes), 1),
                NdVar::cst(SignBit, Bytes), NdVar::cst(SignBit - 1, Bytes)});
      } else {
        Overflow = S.binary(NdOp::INT_LESS, Subtract ? A : Arithmetic,
                            Subtract ? B : A, 1);
        Bound = NdVar::cst(Subtract ? 0 : UINT64_MAX, Bytes);
      }
      Values[I] = S.makeTemp(Bytes);
      S.emit(NdOp::SELECT, Values[I], {Overflow, Bound, Arithmetic});
    } else if (Absolute || AddAbsolute) {
      const auto A = Lane(WS, I, Bytes), B = Lane(WT, I, Bytes);
      const auto Magnitude = [&](NdVar Value) {
        const auto Result = S.makeTemp(Bytes);
        S.emit(NdOp::SELECT, Result,
               {S.binary(NdOp::INT_SLESS, Value, NdVar::cst(0, Bytes), 1),
                S.binary(NdOp::INT_SUB, NdVar::cst(0, Bytes), Value), Value});
        return Result;
      };
      const auto X = Magnitude(A), Y = Magnitude(B);
      if (AddAbsolute) {
        Values[I] = S.binary(NdOp::INT_ADD, X, Y);
        if (*Kind == Instruction::AddSaturatedAbsoluteVector) {
          const auto Max =
              NdVar::cst((UINT64_C(1) << (Bytes * 8 - 1)) - 1, Bytes);
          const auto Overflow =
              S.binary(NdOp::BOOL_OR, S.binary(NdOp::INT_LESS, Values[I], X, 1),
                       S.binary(NdOp::INT_LESS, Max, Values[I], 1), 1);
          const auto Result = S.makeTemp(Bytes);
          S.emit(NdOp::SELECT, Result, {Overflow, Max, Values[I]});
          Values[I] = Result;
        }
      } else {
        const bool Min = *Kind == Instruction::AbsoluteMinimumVector;
        const auto Condition =
            S.binary(NdOp::INT_LESS, Min ? X : Y, Min ? Y : X, 1);
        Values[I] = S.makeTemp(Bytes);
        S.emit(NdOp::SELECT, Values[I], {Condition, A, B}); // Ties choose WT.
      }
    } else if (Minimum || Maximum) {
      const auto A = Lane(WS, I, Bytes), B = Right(I, Bytes);
      const auto Condition =
          S.binary(SignedExtremum ? NdOp::INT_SLESS : NdOp::INT_LESS, A, B, 1);
      Values[I] = S.makeTemp(Bytes);
      S.emit(NdOp::SELECT, Values[I],
             {Condition, Minimum ? A : B, Minimum ? B : A});
    } else if (Comparison) {
      const auto Condition =
          S.binary(*Comparison, Lane(WS, I, Bytes), Right(I, Bytes), 1);
      // Comparisons replicate their predicate across every bit of the lane.
      Values[I] = S.binary(NdOp::INT_SUB, NdVar::cst(0, Bytes),
                           S.resize(Condition, Bytes));
    } else
      Values[I] = S.binary(*Kind == Instruction::SubVector ||
                                   *Kind == Instruction::SubImmediate
                               ? NdOp::INT_SUB
                               : NdOp::INT_ADD,
                           Lane(WS, I, Bytes), Right(I, Bytes));
  }
  for (unsigned Count = 16u / Bytes; Count > 1; Count /= 2)
    for (unsigned I = 0; I != Count / 2; ++I)
      Values[I] = S.binary(NdOp::CONCAT, Values[I * 2 + 1], Values[I * 2],
                           Values[I * 2].Size * 2);
  S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16), {Values[0]});
  return true;
}

} // namespace neverd
