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
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
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
MIPSLifter::mips16JumpEncoding(const cs_insn &Instruction) const {
  if (Instruction.size != 2 && Instruction.size != 4)
    return std::nullopt;
  const bool Little = archLittleEndian(Target);
  const uint16_t First = readTargetEndian<uint16_t>(Instruction.bytes, Little);
  // Only direct calls and short register jumps can have a delay slot. Keep
  // their normalization here so slot queries do not normalize scalar ops.
  if (First >> 11 == 3 && Instruction.size == 4) {
    const uint16_t Second =
        readTargetEndian<uint16_t>(Instruction.bytes + 2, Little);
    const uint32_t Index =
        ((First & 31) << 21) | (((First >> 5) & 31) << 16) | Second;
    return Encoding{(uint32_t(First & 0x400 ? 29 : 3) << 26) | Index, 2};
  }
  if (First >> 11 != 0x1d || Instruction.size != 2)
    return std::nullopt;
  if (First == 0xe820 || First == 0xe8a0)
    return Encoding{registerForm(8, 0, 31), First == 0xe820 ? 2u : 0u};
  if (First & 0x3f)
    return std::nullopt;
  const unsigned Kind = (First >> 6) & 3;
  return Encoding{registerForm(Kind & 1 ? 9 : 8, Kind & 1 ? 31 : 0,
                               compactRegister((First >> 8) & 7)),
                  Kind < 2 ? 2u : 0u};
}

std::optional<MIPSLifter::Encoding>
MIPSLifter::mips16E2Encoding(uint16_t First, uint16_t Half) const {
  const unsigned Major = Half >> 11;
  const unsigned RX = compactRegister((Half >> 8) & 7);
  const unsigned RY = compactRegister((Half >> 5) & 7);
  const uint16_t Immediate =
      ((First & 31) << 11) | (First & 0x7e0) | (Half & 31);
  if (First == 0xf140 && Half == 0x3018)
    return Encoding{0x00000140}; // PAUSE
  const unsigned CP0Key = (((First >> 5) & 7) << 5) | (Half & 31);
  if ((First & 0xff1e) == 0xf000 && (Half & 0xff00) == 0x6700 &&
      (HasMT || !mips::cp0RequiresMT(CP0Key)))
    return Encoding{uint32_t(0x40000000 | ((First & 1) << 23) | (RY << 16) |
                             ((Half & 31) << 11) | ((First >> 5) & 7))};
  if (HasMT &&
      (First == 0xf022 || First == 0xf023 || First == 0xf026 ||
       First == 0xf027) &&
      (Half & 0xff1e) == 0x6700) {
    if ((First & 4) && (Half & 0xe0))
      return std::nullopt;
    return Encoding{uint32_t((Half & 1 ? 0x41600bc1 : 0x41600001) |
                             ((First & 1) << 5) |
                             ((First & 4 ? 0 : RY) << 16))};
  }
  if (First == 0xf0c0 && Half == 0x3010)
    return Encoding{0x000000c0}; // EHB
  if (!(First & 63) && Half == 0x3014)
    return Encoding{uint32_t(First & 0x7c0) | 15}; // SYNC stype
  if (!(First & 0x7e0) && (Half & 0xff1f) == 0x300c &&
      mips::supportedHardwareRegister(First & 31))
    return Encoding{0x7c00003b | (RY << 16) | ((First & 31) << 11)};
  if ((First == 0xf002 || First == 0xf003 || First == 0xf006 ||
       First == 0xf007) &&
      (Half & 0xff1f) == 0x670c) {
    const bool Discard = First & 4;
    if (Discard && (Half & 0xe0))
      return std::nullopt;
    return Encoding{
        uint32_t(0x41606000 | ((First & 1) << 5) | ((Discard ? 0 : RY) << 16))};
  }
  switch ((Major << 3) | ((Half >> 5) & 7)) {
#define NEVERD_MIPS16E2_IMMEDIATE(Major, Selector, Classic, Source)            \
  case ((Major) << 3) | (Selector):                                            \
    return Encoding{immediateForm(Classic, RX, Source, Immediate)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
  case (0x1a << 3) | 4:
  case (0x1a << 3) | 5: {
    if (First & 0x600)
      return std::nullopt;
    const int Offset = int(((First & 0x1e0) | (Half & 31)) ^ 0x100) - 0x100;
    return Encoding{
        immediateForm(Half & 32 ? 47 : 51, First & 31, RX, uint16_t(Offset))};
  }
  case (0x12 << 3) | 6:
  case (0x1a << 3) | 6: {
    if (First & 0x618)
      return std::nullopt;
    const int Offset = int(((First & 0x1e0) | (Half & 31)) ^ 0x100) - 0x100;
    return Encoding{immediateForm(Major == 0x12 ? 48 : 56, RX,
                                  compactRegister(First & 7),
                                  uint16_t(Offset))};
  }
  case (0x12 << 3) | 7:
  case (0x1a << 3) | 7: {
    if (First & 0x608)
      return std::nullopt;
    const unsigned Opcode =
        Major == 0x12 ? (First & 16 ? 38 : 34) : (First & 16 ? 46 : 42);
    const int Offset = int(((First & 0x1e0) | (Half & 31)) ^ 0x100) - 0x100;
    return Encoding{immediateForm(Opcode, RX, compactRegister(First & 7),
                                  uint16_t(Offset))};
  }
  }
  if (Major == 6 && (Half & 0x1c)) {
    const unsigned Selector = (Half >> 2) & 7;
    if (!(Half & 3) && (Selector == 1 || Selector == 2)) {
      const bool Insert = Selector == 1;
      const unsigned Position = (First >> 6) & 31;
      const unsigned Limit = First & 31;
      const bool Source = First & 32;
      if ((!Source && (!Insert || (Half & 0x700))) ||
          (Insert ? Limit < Position : Limit + Position > 31))
        return std::nullopt;
      return Encoding{0x7c000000u | ((Source ? RX : 0) << 21) | (RY << 16) |
                      (Limit << 11) | (Position << 6) | (Insert ? 4u : 0u)};
    }
    if ((Half & 3) == 2 &&
        (Selector == 1 || Selector == 2 || Selector == 5 || Selector == 6)) {
      if ((First & 0x7d8) || (!(First & 32) && (First & 7)) ||
          (Selector >= 5 && (Half & 0xe0)))
        return std::nullopt;
      return Encoding{registerForm(Selector & 1 ? 10 : 11, RX,
                                   First & 32 ? compactRegister(First & 7) : 0,
                                   Selector >= 5 ? 24 : RY)};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<MIPSLifter::Encoding>
MIPSLifter::mips16Encoding(const cs_insn &Instruction) const {
  if (Instruction.size != 2 && Instruction.size != 4)
    return std::nullopt;
  const bool Little = archLittleEndian(Target);
  const uint16_t First = readTargetEndian<uint16_t>(Instruction.bytes, Little);
  const bool Extended = First >> 11 == 0x1e;
  const bool DirectCall = First >> 11 == 3;
  if (Instruction.size != (Extended || DirectCall ? 4 : 2))
    return std::nullopt;
  if (DirectCall)
    return mips16JumpEncoding(Instruction);
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
  // Keep the optional e2 decoder out of the compact integer hot path.
  if (Extended && HasMIPS16E2)
    if (auto E2 = mips16E2Encoding(First, Half))
      return E2;
  switch (Major) {
#define NEVERD_MIPS16_MEMORY(Major, Classic, Scale)                            \
  case Major:                                                                  \
    return Encoding{immediateForm(                                             \
        Classic, RY, RX, Extended ? Immediate : (Half & 31) << Scale)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
  case 0x00:   // ADDIU rx,sp,offset
  case 0x01:   // ADDIU rx,pc,offset
  case 0x12:   // LW rx,offset(sp)
  case 0x16:   // LW rx,offset(pc)
  case 0x1a: { // SW rx,offset(sp)
    if (Extended && (Half & 0xe0))
      return std::nullopt;
    const bool PCRelative = Major == 1 || Major == 0x16;
    const unsigned Opcode = Major < 2 ? 9 : Major == 0x1a ? 43 : 35;
    return Encoding{immediateForm(Opcode, RX, PCRelative ? 0 : 29,
                                  Extended ? Immediate : (Half & 255) << 2),
                    0, false, PCRelative};
  }
#define NEVERD_MIPS16_IMMEDIATE(Major, Classic, Destination, Source, Signed)   \
  case Major:                                                                  \
    if (Extended && (Half & 0xe0))                                             \
      return std::nullopt;                                                     \
    return Encoding{immediateForm(Classic, Destination, Source,                \
                                  Signed ? SignedImmediate : Immediate)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
  case 0x06: { // SHFT
    if (Extended && ((First & 0x3f) || (Half & 0x1c)))
      return std::nullopt;
    const unsigned Count = Extended ? (First >> 6) & 31 : (Half >> 2) & 7;
    const unsigned Amount = Extended || Count ? Count : 8;
    switch (Half & 3) {
#define NEVERD_MIPS16_SHIFT(Minor, Classic)                                    \
  case Minor:                                                                  \
    return Encoding{registerForm(Classic, RX, 0, RY) | (Amount << 6)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
    default:
      return std::nullopt;
    }
  }
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
    if (Minor == 4) { // SVRS has its own EXTEND layout, not an immediate form.
      if (Extended && (First & 15) == 15)
        return std::nullopt;
      return Encoding{(Extended ? uint32_t(First) << 16 : 0) | Half, 0, false,
                      false, true};
    }
    if (Extended && (Half & 0xe0))
      return std::nullopt;
    if (Minor <= 1) // BTEQZ / BTNEZ use GPR24 as T.
      return Encoding{immediateForm(Minor ? 5 : 4, 0, 24, SignedImmediate)};
    if (Minor == 2) // SW ra,offset(sp)
      return Encoding{
          immediateForm(43, 31, 29, Extended ? Immediate : (Half & 255) << 2)};
    if (Minor == 3) // ADJSP
      return Encoding{immediateForm(
          9, 29, 29, Extended ? Immediate : uint16_t(int8_t(Immediate) * 8))};
    if (!Extended && Minor == 5) { // MOVE full GPR, compact GPR
      const unsigned Destination = ((Half >> 3) & 3) * 8 + ((Half >> 5) & 7);
      return Encoding{
          Destination ? registerForm(33, Destination, compactRegister(Half & 7))
                      : 0};
    }
    if (!Extended && Minor == 7) // MOVE compact GPR, full GPR
      return Encoding{registerForm(33, RY, Half & 31)};
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
    if (!(Half & 31)) // JR/JALR/JRC/JALRC, including the dedicated ra forms.
      return mips16JumpEncoding(Instruction);
    switch (Half & 31) {
#define NEVERD_MIPS16_REGISTER(Minor, Classic, Destination, Left, Right)       \
  case Minor:                                                                  \
    return Encoding{registerForm(Classic, Destination, Left, Right)};
#include "neverd/lift/MIPS/MIPS16Integer.def"
    case 0x10: // MFHI
    case 0x12: // MFLO
      if (Half & 0xe0)
        return std::nullopt;
      return Encoding{registerForm(Half & 31, RX, 0)};
    case 0x0f: // NOT
      return Encoding{registerForm(39, RX, RY)};
    default:
      return std::nullopt;
    }
  default:
    return std::nullopt;
  }
}

void MIPSLifter::liftMIPS16Frame(State &S, uint32_t Word) const {
  const unsigned Prefix = Word >> 16;
  const bool Save = Word & 0x80;
  const unsigned Frame = ((Word & 15) | (Prefix & 0xf0)) * 8;
  const unsigned Bytes = !Prefix && !Frame ? 128 : Frame;
  const unsigned Aregs = Prefix & 15;
  const unsigned Arguments = Aregs == 14 ? 4 : Aregs == 11 ? 0 : Aregs >> 2;
  const unsigned Statics = Aregs == 11 ? 4 : Aregs == 14 ? 0 : Aregs & 3;
  const unsigned Extended = (Prefix >> 8) & 7;
  const auto OldSP = S.makeTemp(4);
  S.emit(NdOp::COPY, OldSP, {S.read(29)});
  const auto Top =
      Save ? OldSP : S.binary(NdOp::INT_ADD, OldSP, NdVar::cst(Bytes, 4));
  const auto Access = [&](unsigned Register, int Offset) {
    // Reuse ordinary word memory semantics, including alignment and faults.
    liftMemory(S, immediateForm(Save ? 43 : 35, Register, 29, uint16_t(Offset)),
               Top);
  };
  if (Save)
    for (unsigned I = 0; I != Arguments; ++I)
      Access(4 + I, I * 4);
  int Offset = 0;
  const auto Static = [&](unsigned Register) {
    Offset -= 4;
    Access(Register, Offset);
  };
  if (Word & 0x40)
    Static(31);
  for (unsigned I = Extended; I != 0; --I)
    Static(I == 7 ? 30 : 17 + I);
  if (Word & 0x10)
    Static(17);
  if (Word & 0x20)
    Static(16);
  for (unsigned I = 0; I != Statics; ++I)
    Static(7 - I);
  // Earlier accesses remain visible if a later one faults; SP changes only
  // after the complete sequence. A frame with no accesses need not be aligned.
  S.write(29,
          Save ? S.binary(NdOp::INT_SUB, OldSP, NdVar::cst(Bytes, 4)) : Top);
}

bool MIPSLifter::liftPCRelative(State &S, uint32_t Word, va_t BasePC) const {
  // MD00076: align the executing instruction's first halfword (or its
  // transfer head in a slot), then add the offset. Never align the sum.
  const uint32_t Base = uint32_t(BasePC) & ~uint32_t(3);
  if (Word >> 26 == 9) {
    S.write((Word >> 16) & 31, NdVar::cst(uint32_t(Base + int16_t(Word)), 4));
    return true;
  }
  return liftMemory(S, Word, NdVar::cst(Base, 4));
}

} // namespace neverd
