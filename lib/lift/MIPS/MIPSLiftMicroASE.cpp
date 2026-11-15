//===- MIPSLiftMicroASE.cpp - microMIPS DSP and MSA operand fields --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize the operands at the instruction encoding boundary. Vector lanes,
/// DSPControl and accumulator effects then use the same semantics in either
/// ISA.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSLifter.h"

#include <array>

namespace neverd {
namespace {

enum class Layout {
  Unary,
  Registers,
  Compare,
  Accumulator,
  FromAccumulator,
  ToAccumulator,
  Reduce,
  ReadControl,
  ReplicateHalf,
  ReplicateByte,
  ShiftAccumulator,
  ShiftHalf,
  ShiftByte,
  ShiftWord,
  WriteControl
};
struct DSPForm {
  uint32_t Opcode, Mask, Classic;
  Layout Operands;
  unsigned Revision;
};
constexpr DSPForm Forms[] = {
#define MICRO_DSP(Code, Mask, Classic, Fields, Revision)                       \
  {Code, Mask, Classic, Layout::Fields, Revision},
#include "neverd/lift/MIPS/MicroMIPSDSP.def"
#undef MICRO_DSP
};
constexpr auto MinorStarts = [] {
  std::array<unsigned, 65> Starts{};
  unsigned I = 0;
  for (unsigned Minor = 0; Minor <= 64; ++Minor) {
    Starts[Minor] = I;
    while (I != std::size(Forms) && (Forms[I].Opcode & 63) == Minor)
      ++I;
  }
  return Starts;
}();
static_assert(MinorStarts.back() == std::size(Forms));

uint32_t dspOperands(uint32_t Word, Layout Fields) {
  const unsigned S = (Word >> 16) & 31, T = (Word >> 21) & 31;
  const unsigned D = (Word >> 11) & 31, AC = (Word >> 14) & 3;
  const unsigned Mask = (Word >> 14) & 63;
  switch (Fields) {
  case Layout::Unary:
    return (S << 16) | (T << 11);
  case Layout::Registers:
    return (S << 21) | (T << 16) | (D << 11);
  case Layout::Compare:
    return (S << 21) | (T << 16);
  case Layout::Accumulator:
    return (S << 21) | (T << 16) | (AC << 11);
  case Layout::FromAccumulator:
    return (AC << 21) | (S << 11);
  case Layout::ToAccumulator:
    return (S << 21) | (AC << 11);
  case Layout::Reduce:
    return (S << 21) | (T << 11);
  case Layout::ReadControl:
    return (Mask << 16) | (T << 11);
  case Layout::WriteControl:
    return (T << 21) | (Mask << 11);
  case Layout::ReplicateHalf:
    return Word & 0x03fff800;
  case Layout::ReplicateByte:
    return (((Word >> 13) & 255) << 16) | (T << 11);
  case Layout::ShiftAccumulator:
    return (((Word >> 16) & 63) << 20) | (AC << 11);
  case Layout::ShiftHalf:
    return ((D >> 1) << 21) | (S << 16) | (T << 11);
  case Layout::ShiftByte:
    return ((D >> 2) << 21) | (S << 16) | (T << 11);
  case Layout::ShiftWord:
    return (D << 21) | (S << 16) | (T << 11);
  }
  llvm_unreachable("unknown microMIPS DSP operand layout");
}
} // namespace

std::optional<uint32_t> MIPSLifter::microASEWord(uint32_t Word) const {
  const unsigned Major = Word >> 26;
  if (HasMSA) {
    if (isMIPS64(Target) && (Word & 0xfc00073f) == 0x58000020) // R5 DLSA
      return dspOperands(Word, Layout::Registers) | (Word & 0xc0) | 21;
    if (Major ==
        0x16) { // POOL32S: vector register / immediate fields are shared.
      unsigned Group;
      switch (Word & 7) {
      case 1:
        Group = 0;
        break;
      case 2:
        Group = 8;
        break;
      case 3:
        Group = 16;
        break;
      case 6:
        Group = 24;
        break;
      case 7:
        Group = 32;
        break;
      default:
        return std::nullopt;
      }
      const unsigned Function = Group + ((Word & 8) >> 1) + ((Word >> 4) & 3);
      return 0x78000000u | (Word & 0x03ffffc0) | Function;
    }
    if (Major ==
        0x20) { // BZ / BNZ retain their register and signed halfword offset.
      const unsigned RS = (Word >> 21) & 31;
      if (RS == 11 || RS == 15 || RS >= 24)
        return 0x44000000u | (Word & 0x03ffffff);
    }
    if ((Word & 0xfc00073f) == 0x20) // LSA
      return (dspOperands(Word, Layout::Registers) | (Word & 0xc0) | 5);
  }
  if (!HasDSP)
    return std::nullopt;
  if ((Word >> 16) == 0x4360) // BPOSGE32
    return 0x041c0000u | uint16_t(Word);
  if (Major != 0)
    return std::nullopt;
  const unsigned Minor = Word & 63;
  for (unsigned I = MinorStarts[Minor]; I != MinorStarts[Minor + 1]; ++I) {
    const auto &Form = Forms[I];
    if ((Word & Form.Mask) == Form.Opcode && (Form.Revision == 1 || HasDSPR2))
      return Form.Classic | dspOperands(Word, Form.Operands);
  }
  return std::nullopt;
}
} // namespace neverd
