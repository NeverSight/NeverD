//===- MIPSLiftMicro.cpp - microMIPS scalar encoding semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode pre-R6 microMIPS fields into the existing scalar semantic forms.
/// Physical instruction and slot lengths remain properties of the original
/// occurrence. Unlisted encodings never acquire classic semantics.
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
                      unsigned RT = 0, unsigned Shift = 0) {
  return (RS << 21) | (RT << 16) | (RD << 11) | (Shift << 6) | Function;
}

uint32_t immediateForm(unsigned Opcode, unsigned RT, unsigned RS,
                       uint16_t Immediate) {
  return (Opcode << 26) | (RS << 21) | (RT << 16) | Immediate;
}

} // namespace

std::optional<MIPSLifter::Encoding>
MIPSLifter::microEncoding(const cs_insn &Instruction) const {
  if (Instruction.size != 2 && Instruction.size != 4)
    return std::nullopt;
  const bool Little = archLittleEndian(Target);
  const uint16_t First = readTargetEndian<uint16_t>(Instruction.bytes, Little);
  const unsigned Major = First >> 10;
  const bool Short = (Major & 7) >= 1 && (Major & 7) <= 3;
  if (Instruction.size != (Short ? 2 : 4))
    return std::nullopt;
  if (Short) {
    const unsigned RT = compactRegister((First >> 7) & 7);
    const unsigned RS = compactRegister((First >> 4) & 7);
    switch (Major) {
    case 0x01: // ADDU16 / SUBU16
      return Encoding{registerForm((First & 1) ? 35 : 33, RT,
                                   compactRegister((First >> 1) & 7), RS)};
    case 0x03: // MOVE16
      return Encoding{registerForm(33, (First >> 5) & 31, First & 31)};
    case 0x09: { // SLL16 / SRL16: encoded zero means eight.
      const unsigned Shift = (First >> 1) & 7;
      return Encoding{
          registerForm((First & 1) ? 2 : 0, RT, 0, RS, Shift ? Shift : 8)};
    }
    case 0x11: { // JR16 / JRC / JALR16 / JALRS16
      const unsigned Minor = (First >> 5) & 31;
      if (Minor < 12 || Minor > 15)
        return std::nullopt;
      const bool Link = Minor >= 14;
      return Encoding{registerForm(Link ? 9 : 8, Link ? 31 : 0, First & 31),
                      Minor == 13   ? 0u
                      : Minor == 14 ? 4u
                      : Minor == 15 ? 2u
                                    : 1u};
    }
    case 0x02: // LBU16: encoded 15 means -1.
      return Encoding{immediateForm(
          36, RT, RS, (First & 15) == 15 ? uint16_t(-1) : First & 15)};
    case 0x0a: // LHU16
      return Encoding{immediateForm(37, RT, RS, (First & 15) * 2)};
    case 0x1a: // LW16
      return Encoding{immediateForm(35, RT, RS, (First & 15) * 4)};
    case 0x22:   // SB16
    case 0x2a:   // SH16
    case 0x3a: { // SW16: the first compact source is zero, not s0.
      const unsigned Scale = Major == 0x22 ? 1 : Major == 0x2a ? 2 : 4;
      return Encoding{immediateForm(Major == 0x22   ? 40
                                    : Major == 0x2a ? 41
                                                    : 43,
                                    RT == 16 ? 0 : RT, RS,
                                    (First & 15) * Scale)};
    }
    case 0x12: // LWSP16
    case 0x32: // SWSP16
      return Encoding{immediateForm(Major == 0x12 ? 35 : 43, (First >> 5) & 31,
                                    29, (First & 31) * 4)};
    case 0x19: // LWGP16
      return Encoding{
          immediateForm(35, RT, 28, ((First & 127) ^ 64) * 4 - 256)};
    case 0x23: // BEQZ16
    case 0x2b: // BNEZ16
      return Encoding{immediateForm(Major == 0x23 ? 4 : 5, 0, RT,
                                    ((First & 127) ^ 64) - 64),
                      1};
    case 0x33: // B16
      return Encoding{immediateForm(4, 0, 0, ((First & 1023) ^ 512) - 512), 1};
    case 0x3b: // LI16: encoded 127 means -1; 64..126 are positive.
      return Encoding{immediateForm(
          9, RT, 0, (First & 127) == 127 ? uint16_t(-1) : First & 127)};
    default:
      return std::nullopt;
    }
  }

  // microMIPS fetch order is two target-endian halfwords, including on LE.
  const uint32_t Word =
      (uint32_t(First) << 16) |
      readTargetEndian<uint16_t>(Instruction.bytes + 2, Little);
  const unsigned RT = (Word >> 21) & 31;
  const unsigned RS = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31;
  switch (Major) {
#define NEVERD_MICRO_IMMEDIATE(Micro, Classic)                                 \
  case Micro:                                                                  \
    return Encoding{immediateForm(Classic, RT, RS, uint16_t(Word))};
#include "neverd/lift/MIPS/MicroMIPSInteger.def"
  case 0x25: // BEQ32
  case 0x2d: // BNE32
    return Encoding{
        immediateForm(Major == 0x25 ? 4 : 5, RT, RS, uint16_t(Word)), 1};
  case 0x35: // J
  case 0x3d: // JAL
  case 0x1d: // JALS
  case 0x3c: // JALX
    return Encoding{((Major == 0x35   ? 2u
                      : Major == 0x3c ? 29u
                                      : 3u)
                     << 26) |
                        (Word & 0x03ffffff),
                    Major == 0x1d   ? 2u
                    : Major == 0x35 ? 1u
                                    : 4u};
  case 0x00: {
    const unsigned Minor = (Word >> 6) & 31;
    if ((Word & 63) == 0 && Minor <= 3) { // SLL/SRL/SRA/ROTR
      return Encoding{registerForm(Minor == 0   ? 0
                                   : Minor == 2 ? 3
                                                : 2,
                                   RT, Minor == 3 ? 1 : 0, RS, RD)};
    }
    if ((Word & 63) == 0x10) {
      switch (Minor) {
#define NEVERD_MICRO_REGISTER(Micro, Classic)                                  \
  case Micro:                                                                  \
    return Encoding{registerForm(Classic, RD, RS, RT)};
#include "neverd/lift/MIPS/MicroMIPSInteger.def"
      default:
        break;
      }
    }
    if ((Word & 0xffff) == 0x0f3c || (Word & 0xffff) == 0x4f3c) {
      // JR is the rt=zero alias of JALR, with the same required slot size.
      return Encoding{registerForm(RT ? 9 : 8, RT, RS),
                      (Word & 0x4000) ? 2u : 4u};
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

} // namespace neverd
