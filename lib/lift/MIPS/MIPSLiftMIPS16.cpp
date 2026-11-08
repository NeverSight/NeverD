//===- MIPSLiftMIPS16.cpp - MIPS16e32 scalar encoding semantics ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Admit exact MIPS16e32 fields into existing scalar semantics. An EXTEND pair
/// is one physical instruction; compact branches never acquire a delay slot.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd {
namespace {

unsigned compactRegister(unsigned Field) {
  return Field < 2 ? Field + 16 : Field;
}

uint32_t registerForm(unsigned Function, unsigned RD, unsigned RS,
                      unsigned RT = 0) {
  return (RS << 21) | (RT << 16) | (RD << 11) | Function;
}

uint32_t immediateForm(unsigned Opcode, unsigned RT, unsigned RS,
                       uint16_t Immediate) {
  return (Opcode << 26) | (RS << 21) | (RT << 16) | Immediate;
}

} // namespace

std::optional<MIPSLifter::Encoding>
MIPSLifter::mips16Encoding(const cs_insn &Instruction) const {
  if (Instruction.size != 2 && Instruction.size != 4)
    return std::nullopt;
  const bool Little = archLittleEndian(Target);
  const uint16_t First = readTargetEndian<uint16_t>(Instruction.bytes, Little);
  const bool Extended = First >> 11 == 0x1e;
  if (Instruction.size != (Extended ? 4 : 2))
    return std::nullopt;
  const uint16_t Half =
      Extended ? readTargetEndian<uint16_t>(Instruction.bytes + 2, Little)
               : First;
  const unsigned Major = Half >> 11;
  const unsigned RX = compactRegister((Half >> 8) & 7);
  const unsigned RY = compactRegister((Half >> 5) & 7);
  const uint16_t Immediate =
      Extended ? ((First & 31) << 11) | (First & 0x7e0) | (Half & 31)
               : Half & 255;
  const uint16_t SignedImmediate =
      Extended ? Immediate : uint16_t(int16_t(int8_t(Immediate)));
  switch (Major) {
#define NEVERD_MIPS16_IMMEDIATE(Major, Classic, Destination, Source, Signed)   \
  case Major:                                                                  \
    if (Extended && (Half & 0xe0))                                             \
      return std::nullopt;                                                     \
    return Encoding{immediateForm(Classic, Destination, Source,                \
                                  Signed ? SignedImmediate : Immediate)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
  case 0x02: { // B
    if (Extended && (Half & 0x7e0))
      return std::nullopt;
    const uint16_t Displacement =
        Extended ? Immediate : uint16_t(((Half & 0x7ff) ^ 0x400) - 0x400);
    return Encoding{immediateForm(4, 0, 0, Displacement), 0, true};
  }
  case 0x04: // BEQZ
  case 0x05: // BNEZ
    if (Extended && (Half & 0xe0))
      return std::nullopt;
    return Encoding{immediateForm(Major, 0, RX, SignedImmediate)};
  case 0x0c: {
    const unsigned Minor = (Half >> 8) & 7;
    if (Extended && (Half & 0xe0))
      return std::nullopt;
    if (Minor <= 1) // BTEQZ / BTNEZ use GPR24 as T.
      return Encoding{immediateForm(Minor ? 5 : 4, 0, 24, SignedImmediate)};
    if (Minor == 3) // ADJSP
      return Encoding{immediateForm(
          9, 29, 29, Extended ? Immediate : uint16_t(int8_t(Immediate) * 8))};
    if (!Extended && Minor == 5) { // MOVE full GPR, compact GPR
      const unsigned Destination = ((Half >> 3) & 3) * 8 + ((Half >> 5) & 7);
      return Encoding{
          Destination ? registerForm(33, Destination, compactRegister(Half & 7))
                      : 0};
    }
    return std::nullopt;
  }
  case 0x1c: // ADDU / SUBU
    if (Extended || ((Half & 3) != 1 && (Half & 3) != 3))
      return std::nullopt;
    return Encoding{registerForm((Half & 3) == 1 ? 33 : 35,
                                 compactRegister((Half >> 2) & 7), RX, RY)};
  case 0x1d:
    if (Extended)
      return std::nullopt;
    if (Half == 0xe820 || Half == 0xe8a0) // JR / JRC ra
      return Encoding{registerForm(8, 0, 31), Half == 0xe820 ? 2u : 0u};
    if ((Half & 255) == 0) // JR compact GPR
      return Encoding{registerForm(8, 0, RX), 2};
    switch (Half & 31) {
#define NEVERD_MIPS16_LOGICAL(Minor, Classic)                                  \
  case Minor:                                                                  \
    return Encoding{registerForm(Classic, RX, RX, RY)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
    case 0x0f: // NOT
      return Encoding{registerForm(39, RX, RY)};
    default:
      return std::nullopt;
    }
  default:
    return std::nullopt;
  }
}

} // namespace neverd
