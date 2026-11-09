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
  std::optional<NdOp> Comparison;
  switch (*Kind) {
  case Instruction::EqualVector:
    Comparison = NdOp::INT_EQUAL;
    break;
  case Instruction::SignedLessVector:
    Comparison = NdOp::INT_SLESS;
    break;
  case Instruction::UnsignedLessVector:
    Comparison = NdOp::INT_LESS;
    break;
  case Instruction::SignedLessEqualVector:
    Comparison = NdOp::INT_SLESSEQUAL;
    break;
  case Instruction::UnsignedLessEqualVector:
    Comparison = NdOp::INT_LESSEQUAL;
    break;
  default:
    break;
  }
  std::optional<NdOp> Shift, Bit;
  switch (*Kind) {
  case Instruction::ShiftLeftVector:
    Shift = NdOp::INT_LEFT;
    break;
  case Instruction::ShiftRightVector:
    Shift = NdOp::INT_RIGHT;
    break;
  case Instruction::ShiftRightSignedVector:
    Shift = NdOp::INT_ASHR;
    break;
  case Instruction::ClearBitVector:
    Bit = NdOp::INT_AND;
    break;
  case Instruction::SetBitVector:
    Bit = NdOp::INT_OR;
    break;
  case Instruction::NegateBitVector:
    Bit = NdOp::INT_XOR;
    break;
  default:
    break;
  }
  const bool Minimum = *Kind == Instruction::SignedMinimumVector ||
                       *Kind == Instruction::UnsignedMinimumVector;
  const bool Maximum = *Kind == Instruction::SignedMaximumVector ||
                       *Kind == Instruction::UnsignedMaximumVector;
  const bool SignedExtremum = *Kind == Instruction::SignedMinimumVector ||
                              *Kind == Instruction::SignedMaximumVector;
  const uint16_t Bytes =
      *Kind == Instruction::FillWord || *Kind == Instruction::InsertWord
          ? 4
          : 1u << ((Word >> 21) & 3);
  std::array<NdVar, 16> Values;
  for (unsigned I = 0; I != 16u / Bytes; ++I) {
    if (*Kind == Instruction::FillWord)
      Values[I] = S.read(WS);
    else if (*Kind == Instruction::InsertWord)
      Values[I] = I == Index ? S.read(WS) : Lane(WD, I);
    else if (Shift || Bit) {
      const auto A = Lane(WS, I, Bytes);
      const auto Count = S.binary(NdOp::INT_AND, Lane(WT, I, Bytes),
                                  NdVar::cst(Bytes * 8 - 1, Bytes));
      if (Shift)
        Values[I] = S.binary(*Shift, A, Count);
      else {
        auto Mask = S.binary(NdOp::INT_LEFT, NdVar::cst(1, Bytes), Count);
        if (*Kind == Instruction::ClearBitVector) {
          const auto Inverted = S.makeTemp(Bytes);
          S.emit(NdOp::INT_NOT, Inverted, {Mask});
          Mask = Inverted;
        }
        Values[I] = S.binary(*Bit, A, Mask);
      }
    } else if (*Kind == Instruction::MultiplyVector)
      Values[I] =
          S.binary(NdOp::INT_MULT, Lane(WS, I, Bytes), Lane(WT, I, Bytes));
    else if (Minimum || Maximum) {
      const auto A = Lane(WS, I, Bytes), B = Lane(WT, I, Bytes);
      const auto Condition =
          S.binary(SignedExtremum ? NdOp::INT_SLESS : NdOp::INT_LESS, A, B, 1);
      Values[I] = S.makeTemp(Bytes);
      S.emit(NdOp::SELECT, Values[I],
             {Condition, Minimum ? A : B, Minimum ? B : A});
    } else if (Comparison) {
      const auto Condition =
          S.binary(*Comparison, Lane(WS, I, Bytes), Lane(WT, I, Bytes), 1);
      // Comparisons replicate their predicate across every bit of the lane.
      Values[I] = S.binary(NdOp::INT_SUB, NdVar::cst(0, Bytes),
                           S.resize(Condition, Bytes));
    } else
      Values[I] = S.binary(*Kind == Instruction::SubVector ? NdOp::INT_SUB
                                                           : NdOp::INT_ADD,
                           Lane(WS, I, Bytes), Lane(WT, I, Bytes));
  }
  for (unsigned Count = 16u / Bytes; Count > 1; Count /= 2)
    for (unsigned I = 0; I != Count / 2; ++I)
      Values[I] = S.binary(NdOp::CONCAT, Values[I * 2 + 1], Values[I * 2],
                           Values[I * 2].Size * 2);
  S.emit(NdOp::COPY, NdVar::reg(mipsreg::w(WD), 16), {Values[0]});
  return true;
}

} // namespace neverd
