//===- MIPSLiftMicro.cpp - microMIPS scalar encoding semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode revision-specific microMIPS fields into the existing scalar semantic
/// forms. Physical instruction and slot lengths remain properties of the
/// original occurrence. Unlisted encodings never acquire classic semantics.
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MicroMIPS64Release6.h"
#include "neverd/lift/MIPS/MicroMIPSRelease6.h"
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
  const bool R6 = ISARevision == 6;
  if (Short) {
    const unsigned RT = compactRegister((First >> 7) & 7);
    const unsigned RS = compactRegister((First >> 4) & 7);
    if (Major == 0x11) {
      const unsigned Dest = R6 ? RT : compactRegister((First >> 3) & 7);
      const unsigned Source = R6 ? RS : compactRegister(First & 7);
      const unsigned Minor = R6 ? First & 15 : (First >> 6) & 15;
      if (!Minor)
        return Encoding{registerForm(39, Dest, Source)}; // NOT16
      if (Minor == (R6 ? 1u : 2u))
        return Encoding{registerForm(36, Dest, Dest, Source)}; // AND16
      if (Minor == (R6 ? 8u : 1u))
        return Encoding{registerForm(38, Dest, Dest, Source)}; // XOR16
      if (Minor == (R6 ? 9u : 3u))
        return Encoding{registerForm(37, Dest, Dest, Source)}; // OR16
    }
    if (R6) {
      if (auto Branch = mips::microR6Compact(First, 2))
        return Encoding{.Word = *Branch, .R6 = true};
      if (Major == 1) // Release 6 interchanges the three compact fields.
        return Encoding{registerForm((First & 1) ? 35 : 33,
                                     compactRegister((First >> 1) & 7),
                                     compactRegister((First >> 7) & 7),
                                     compactRegister((First >> 4) & 7))};
      if (Major == 0x11) {
        const unsigned Fn = First & 31;
        if (Fn == 3 || Fn == 11)
          return Encoding{registerForm(Fn == 11 ? 9 : 8, Fn == 11 ? 31 : 0,
                                       (First >> 5) & 31)};
        return std::nullopt;
      }
    }
    switch (Major) {
    case 0x0b: { // ANDI16 immediate dictionary, microMIPS Volume II-B table
                 // 5-2.
      static constexpr uint16_t Masks[] = {
          128, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 255, 32768, 65535};
      return Encoding{immediateForm(12, RT, RS, Masks[First & 15])};
    }
    case 0x13: { // ADDIUS5 / ADDIUSP
      if (!(First & 1)) {
        const unsigned Reg = (First >> 5) & 31;
        return Encoding{
            immediateForm(9, Reg, Reg, llvm::SignExtend64<4>(First >> 1))};
      }
      int Imm = llvm::SignExtend64<9>(First >> 1);
      if (Imm >= -2 && Imm <= 1)
        Imm += Imm < 0 ? -256 : 256;
      return Encoding{immediateForm(9, 29, 29, Imm * 4)};
    }
    case 0x1b: { // ADDIUR2 / ADDIUR1SP
      if (First & 1)
        return Encoding{immediateForm(9, RT, 29, ((First >> 1) & 63) * 4)};
      const unsigned Imm = (First >> 1) & 7;
      return Encoding{immediateForm(9, RT, RS,
                                    Imm == 7 ? -1
                                    : Imm    ? 4 * Imm
                                             : 1)};
    }
    case 0x01: // ADDU16 / SUBU16
      return Encoding{registerForm((First & 1) ? 35 : 33, RT,
                                   compactRegister((First >> 1) & 7), RS)};
    case 0x03: // MOVE16
      return Encoding{registerForm(isMIPS64(Target) ? 37 : 33,
                                   (First >> 5) & 31, First & 31)};
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
  const unsigned RT = (Word >> 21) & 31, RS = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31;
  if (R6) {
    if (isMIPS64(Target))
      if (const auto Wide = mips::micro64R6Word(Word))
        return release6Encoding(*Wide);
    if (const auto Branch = mips::microR6Compact(Word, 4))
      return Encoding{.Word = *Branch, .R6 = true};
    if (const auto Atomic = mips::microR6Reservation(Word))
      return Encoding{.Word = *Atomic, .R6 = true};
    const auto Native = [](uint32_t W) {
      return Encoding{.Word = W, .R6 = true};
    };
    if (const auto Alu = mips::microR6ScaledALU(Word))
      return Native(*Alu);
    if (const auto Hardware = mips::microR6HardwareRegister(Word))
      return Native(*Hardware);
    if (const auto Cache = mips::microR6Cache(Word))
      return Native(*Cache);
    if (Major == 4) // AUI
      return Native(immediateForm(15, RT, RS, uint16_t(Word)));
    if (Major == 0x1e) // PCREL
      return Native((59u << 26) | (Word & 0x03ffffff));
    if (Major == 0) {
      const unsigned Minor = Word & 0x7ff;
      if ((Word & 0x03e0efff) == 0x477c) // DI / EI: rt is fixed at zero.
        return Native(0x41606000 | (RS << 16) | ((Word & 0x1000) ? 32 : 0));
      if (!(Word & 0xc000)) {
        unsigned Transfer;
        switch (Minor) {
        case 0x00fc:
          Transfer = 0;
          break; // MFC0
        case 0x02fc:
          Transfer = 4;
          break; // MTC0
        case 0x00f4:
          Transfer = 2;
          break; // MFHC0
        case 0x02f4:
          Transfer = 6;
          break; // MTHC0
        default:
          Transfer = 8;
          break;
        }
        if (Transfer != 8)
          return Native((16u << 26) | (Transfer << 21) | (RT << 16) |
                        (RS << 11) | ((Word >> 11) & 7));
      }
      if ((Minor & 0x63f) == 0x18) { // MUL/MUH/MULU/MUHU/DIV/MOD/DIVU/MODU
        const unsigned K = (Minor >> 6) & 7;
        return Native(registerForm(24 + K / 2, RD, RS, RT, 2 + (K & 1)));
      }
      if (Minor == 0x140 || Minor == 0x180)
        return Native(registerForm(Minor == 0x140 ? 53 : 55, RD, RS, RT));
      if ((Word & 0xffff) == 0x4b3c || (Word & 0xffff) == 0x5b3c)
        return Native(registerForm(Word & 0x1000 ? 16 : 17, RT, RS, 0, 1));
      if ((Word & 0xffff) == 0x0b3c) // BITSWAP
        return Native((31u << 26) | (RT << 16) | (RS << 11) | 32);
      if ((Word & 0xffff) == 0x0f3c) // JALRC / JRC, no slot
        return Encoding{registerForm(RT ? 9 : 8, RT, RS)};
      if (Word == 0x2800)
        return Encoding{0x140}; // PAUSE
      if (Word == 0x1800)
        return Encoding{0xc0}; // EHB
    } else {
      switch (Major) {
#define NEVERD_MICRO_IMMEDIATE(Micro, Classic)                                 \
  case Micro:                                                                  \
    break;
#include "neverd/lift/MIPS/MicroMIPSInteger.def"
      default:
        return std::nullopt;
      }
    }
  }
  if (isMIPS64(Target))
    if (auto Classic = mips::micro64Word(Word))
      return Encoding{*Classic};
  if (HasDSP || HasMSA)
    if (auto Classic = microASEWord(Word))
      return Encoding{*Classic, hasDelaySlot(*Classic) ? 1u : 0u};
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
    if (!R6 && ((Word & 0xffff) == 0x0f3c || (Word & 0xffff) == 0x4f3c)) {
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
