//===- MIPSDecoder.cpp - MIPS ASE physical decode adapters ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/lift/MIPS/MicroMIPS64Release6.h"
#include "neverd/lift/MIPS/MicroMIPSRelease6.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace neverd {
namespace {
const char *physicalMnemonic(csh Handle, unsigned ID) {
  switch (ID) {
#define NEVERD_MIPS_PHYSICAL_FORM(Name, Offset, Text)                          \
  case MIPSLifter::Name##ID:                                                   \
    return Text;
#include "neverd/lift/MIPS/MIPSPhysicalForms.def"
  default:
    return cs_insn_name(Handle, ID);
  }
}
constexpr std::array<mips_reg, 32> GPR = {
#define NEVERD_MIPS_GPR(Name, Index, O32, N64, Reg) Reg,
#include "neverd/lift/MIPS/MIPSRegisters.def"
};
} // namespace

bool Decoder::decodeMIPSReservation(const uint8_t *Bytes, uint32_t Word,
                                    va_t Addr) {
  const auto R = mips::r6Reservation(Word);
  const bool Wide = isMIPS64(TargetArch);
  if (!R || (!Wide && R->ElementBytes == 8))
    return false;
  unsigned ID;
  switch (R->Id) {
#define NEVERD_MIPS_RESERVATION(Code, Width, Pair, Load, LID, LM, Store, SID,  \
                                SM, CSLoad, CSStore)                           \
  case Intrinsic::Load:                                                        \
    ID = CSLoad;                                                               \
    break;                                                                     \
  case Intrinsic::Store:                                                       \
    ID = CSStore;                                                              \
    break;
#include "neverd/ir/intrinsics/MIPS/MIPSReservation.def"
  default:
    llvm_unreachable("unknown MIPS reservation transfer");
  }
  auto *DetailBuf = InsnBuf->detail;
  *InsnBuf = {};
  InsnBuf->detail = DetailBuf;
  InsnBuf->id = ID;
  InsnBuf->address = Addr;
  InsnBuf->size = 4;
  std::copy_n(Bytes, 4, InsnBuf->bytes);
  const unsigned RS = (Word >> 21) & 31, RT = (Word >> 16) & 31,
                 RD = (Word >> 11) & 31;
  const auto Offset = R->Paired ? 0 : int(llvm::SignExtend64<9>(Word >> 7));
  if (Text) {
    const auto RegName = [&](unsigned Reg) {
      return Wide ? getMIPS64RegName(mipsreg::r(Reg), 8)
                  : getMIPS32RegName(mipsreg::r(Reg), 4);
    };
    std::snprintf(InsnBuf->mnemonic, sizeof(InsnBuf->mnemonic), "%s",
                  physicalMnemonic(Handle, ID));
    if (R->Paired)
      std::snprintf(InsnBuf->op_str, sizeof(InsnBuf->op_str), "$%s, $%s, ($%s)",
                    RegName(RT), RegName(RD), RegName(RS));
    else
      std::snprintf(InsnBuf->op_str, sizeof(InsnBuf->op_str), "$%s, %d($%s)",
                    RegName(RT), Offset, RegName(RS));
  }
  if (Detail) {
    *DetailBuf = {};
    auto &M = DetailBuf->mips;
    auto &First = M.operands[M.op_count++];
    First.type = MIPS_OP_REG;
    First.reg = GPR[RT];
    First.access = R->Store ? CS_AC_READ_WRITE : CS_AC_WRITE;
    if (R->Paired) {
      auto &Second = M.operands[M.op_count++];
      Second.type = MIPS_OP_REG;
      Second.reg = GPR[RD];
      Second.access = R->Store ? CS_AC_READ : CS_AC_WRITE;
    }
    auto &Mem = M.operands[M.op_count++];
    Mem.type = MIPS_OP_MEM;
    Mem.mem = {GPR[RS], Offset};
    Mem.access = R->Store ? CS_AC_WRITE : CS_AC_READ;
  }
  return true;
}

bool Decoder::decodeMicroMIPSRelease6(const uint8_t *Bytes, size_t Len,
                                      va_t Addr) {
  if (Len < 2)
    return false;
  const bool Little = archLittleEndian(TargetArch);
  const auto First = readTargetEndian<uint16_t>(Bytes, Little);
  const unsigned Major = First >> 10, LengthCode = Major & 7;
  const unsigned Size = LengthCode >= 1 && LengthCode <= 3 ? 2 : 4;
  if (Len < Size)
    return false;
  const uint32_t Word = Size == 2
                            ? First
                            : (uint32_t(First) << 16) |
                                  readTargetEndian<uint16_t>(Bytes + 2, Little);
  if (Size == 4 && Major == 0x18 && ((Word >> 12) & 1)) {
    const auto Classic = mips::microR6Reservation(Word);
    return Classic && decodeMIPSReservation(Bytes, *Classic, Addr);
  }
  if (Size == 4 && isMIPS64(TargetArch)) {
    if (Major == 0x1e) // The shared PCREL group includes LDPC and LWUPC.
      return decodeMIPSEquivalent(Bytes, (59u << 26) | (Word & 0x03ffffff),
                                  Addr);
    if (const auto Wide = mips::micro64R6Word(Word))
      return decodeMIPSEquivalent(Bytes, *Wide, Addr);
  }
  if (Size == 4 && Major == 0) {
    if (const auto Alu = mips::microR6ScaledALU(Word))
      return decodeMIPSEquivalent(Bytes, *Alu, Addr);
    if (const auto Hw = mips::microR6HardwareRegister(Word))
      return decodeR6HardwareRegister(Bytes, *Hw, Addr);
  }
  if (Size == 4 && (Major == 8 || Major == 0x18))
    if (const auto Cache = mips::microR6Cache(Word))
      return decodeMIPSEquivalent(Bytes, *Cache, Addr);
  const auto Branch = mips::microR6Compact(Word, Size);
  const bool Indexed =
      Size == 4 && (Major == 0x20 || Major == 0x28) && !(Word & 0x03e00000);
  const uint8_t *Code = Bytes;
  std::array<uint8_t, 4> Corrected;
  if (Indexed) {
    // The physical decoder interchanges the JIC and JIALC majors. Reuse its
    // correct operand and implicit link metadata for the actual instruction.
    std::copy_n(Bytes, Size, Corrected.begin());
    Corrected[Little ? 1 : 0] ^= 0x20;
    Code = Corrected.data();
  }
  uint64_t PC = Addr;
  size_t Remaining = Size;
  if (!cs_disasm_iter(Handle, &Code, &Remaining, &PC, InsnBuf) ||
      InsnBuf->size != Size)
    return false;
  if (Indexed)
    std::copy_n(Bytes, Size, InsnBuf->bytes);
  if (Branch && !Indexed) {
    const auto Target = MIPS->r6BranchTarget(*Branch, Addr, Size);
    if (Detail) {
      auto &M = InsnBuf->detail->mips;
      if (M.op_count && M.operands[M.op_count - 1].type == MIPS_OP_IMM)
        M.operands[M.op_count - 1].imm = Target;
    }
    if (Text) {
      char *Comma = std::strrchr(InsnBuf->op_str, ',');
      char *Out = Comma ? Comma + 1 : InsnBuf->op_str;
      std::snprintf(Out, sizeof(InsnBuf->op_str) - (Out - InsnBuf->op_str),
                    "%s0x%" PRIx64, Comma ? " " : "", Target);
    }
  }
  return true;
}

bool Decoder::decodeMIPSASE(const uint8_t *Bytes, size_t Len, va_t Addr) {
  if (Len < 4)
    return false;
  const bool Little = archLittleEndian(TargetArch);
  if (MIPSISARevision == 6 &&
      (readTargetEndian<uint32_t>(Bytes, Little) >> 16) == 0x0410) {
    // NAL ignores its encoded offset and writes PC+8 before its delay slot.
    // Capstone does not expose this Release 6 encoding in either GPR width.
    cs_detail *D = InsnBuf->detail;
    *InsnBuf = {};
    InsnBuf->detail = D;
    InsnBuf->id = MIPSLifter::NoOpAndLinkID;
    InsnBuf->address = Addr;
    InsnBuf->size = 4;
    std::copy_n(Bytes, 4, InsnBuf->bytes);
    if (Text)
      std::snprintf(InsnBuf->mnemonic, sizeof(InsnBuf->mnemonic), "%s",
                    physicalMnemonic(Handle, InsnBuf->id));
    if (Detail) {
      *D = {};
      D->regs_read[D->regs_read_count++] = MIPS_REG_PC;
      D->regs_write[D->regs_write_count++] = MIPS_REG_RA;
    }
    return true;
  }
  const auto Physical = readTargetEndian<uint32_t>(Bytes, Little);
  if (MIPSISARevision == 6) {
    const unsigned RS = (Physical >> 21) & 31;
    if ((Physical & 0xfc0007f8) == 0x40000000 && (RS == 2 || RS == 6)) {
      // MFHC0/MTHC0 have the same operand roles as MFC0/MTC0.
      if (!decodeMIPSEquivalent(Bytes, Physical & ~(2u << 21), Addr))
        return false;
      InsnBuf->id = RS == 2 ? MIPS_INS_MFHC0 : MIPS_INS_MTHC0;
      if (Text)
        std::snprintf(InsnBuf->mnemonic, sizeof(InsnBuf->mnemonic), "%s",
                      cs_insn_name(Handle, InsnBuf->id));
      return true;
    }
    if ((Physical & 0xffe0063f) == 0x7c00003b)
      return decodeR6HardwareRegister(Bytes, Physical, Addr);
  }
  if (isMIPS64(TargetArch) && CurrentMode != InstructionMode::MicroMIPS) {
    if (!MIPS->isDSPIndexedDoubleword(Physical) ||
        !decodeMIPSEquivalent(Bytes, Physical & ~0x200u, Addr))
      return false;
    InsnBuf->id = MIPSLifter::IndexedLoadDoublewordID;
    if (Text)
      std::snprintf(InsnBuf->mnemonic, sizeof(InsnBuf->mnemonic), "%s",
                    physicalMnemonic(Handle, InsnBuf->id));
    return true;
  }
  if (CurrentMode != InstructionMode::MicroMIPS)
    return false;
  const uint32_t MicroPhysical =
      uint32_t(readTargetEndian<uint16_t>(Bytes, Little)) << 16 |
      readTargetEndian<uint16_t>(Bytes + 2, Little);
  auto Classic =
      isMIPS64(TargetArch) ? mips::micro64Word(MicroPhysical) : std::nullopt;
  if (!Classic)
    Classic = MIPS->microASEWord(MicroPhysical);
  if (!Classic)
    return false;
  return decodeMIPSEquivalent(Bytes, *Classic, Addr,
                              MIPSLifter::hasDelaySlot(*Classic));
}

bool Decoder::decodeR6HardwareRegister(const uint8_t *Bytes, uint32_t Word,
                                       va_t Addr) {
  const unsigned RD = (Word >> 11) & 31, RT = (Word >> 16) & 31;
  const unsigned Selector = (Word >> 6) & 7;
  if (!mips::supportedR6HardwareRegister((Selector << 5) | RD) ||
      !decodeMIPSEquivalent(Bytes, 0x7c00e83b | (RT << 16), Addr))
    return false;
  if (Detail) {
    auto &M = InsnBuf->detail->mips;
    M.op_count = 2;
    M.operands[0].type = MIPS_OP_REG;
    M.operands[0].reg = GPR[RT];
    M.operands[0].access = CS_AC_WRITE;
    M.operands[1].type = MIPS_OP_REG;
    M.operands[1].reg = static_cast<mips_reg>(MIPS_REG_HWR0 + RD);
    M.operands[1].access = CS_AC_READ;
    if (Selector) {
      auto &Sel = M.operands[M.op_count++];
      Sel = {};
      Sel.type = MIPS_OP_IMM;
      Sel.imm = Selector;
      Sel.access = CS_AC_READ;
    }
  }
  if (Text) {
    const auto Name = isMIPS64(TargetArch)
                          ? getMIPS64RegName(mipsreg::r(RT), 8)
                          : getMIPS32RegName(mipsreg::r(RT), 4);
    if (Selector)
      std::snprintf(InsnBuf->op_str, sizeof(InsnBuf->op_str), "$%s, $%u, %u",
                    Name, RD, Selector);
    else
      std::snprintf(InsnBuf->op_str, sizeof(InsnBuf->op_str), "$%s, $%u", Name,
                    RD);
  }
  return true;
}

bool Decoder::decodeMIPSEquivalent(const uint8_t *Bytes, uint32_t Classic,
                                   va_t Addr, bool HalfwordBranch) {
  const bool Little = archLittleEndian(TargetArch);
  const bool Micro = CurrentMode == InstructionMode::MicroMIPS;
  // Capstone has classic MSA and DSP tables for forms missing in its
  // microMIPS tables. Preserve physical bytes and address in the public record.
  if (Micro && !MicroASEHandle) {
    const auto Mode = static_cast<cs_mode>(
        (MIPSISARevision == 6
             ? (isMIPS64(TargetArch) ? CS_MODE_MIPS64R6 | CS_MODE_MIPS_PTR64
                                     : CS_MODE_MIPS32R6)
         : isMIPS64(TargetArch) ? CS_MODE_MIPS64R2
         : MIPSISARevision == 5 ? CS_MODE_MIPS32R5
                                : CS_MODE_MIPS32R2) |
        (Little ? CS_MODE_LITTLE_ENDIAN : CS_MODE_BIG_ENDIAN));
    if (cs_open(CS_ARCH_MIPS, Mode, &MicroASEHandle) != CS_ERR_OK)
      return false;
    cs_option(MicroASEHandle, CS_OPT_DETAIL, Detail ? CS_OPT_ON : CS_OPT_OFF);
    cs_option(MicroASEHandle, CS_OPT_TEXT, Text ? CS_OPT_ON : CS_OPT_OFF);
  }
  uint8_t Encoded[4];
  llvm::support::endian::write32(Encoded, Classic,
                                 Little ? llvm::endianness::little
                                        : llvm::endianness::big);
  const uint8_t *Code = Encoded;
  size_t Size = sizeof(Encoded);
  // Classic branch operands are rendered with displacement * 4, micro with * 2.
  uint64_t PC = Addr;
  if (HalfwordBranch) {
    const int64_t Offset = int16_t(Classic);
    const auto Target = Addr + 4 + Offset * 2;
    PC = (isMIPS64(TargetArch) ? Target : uint32_t(Target)) - (4 + Offset * 4);
  }
  InsnBuf->illegal = false;
  if (!cs_disasm_iter(Micro ? MicroASEHandle : Handle, &Code, &Size, &PC,
                      InsnBuf))
    return false;
  InsnBuf->address = Addr;
  std::copy_n(Bytes, 4, InsnBuf->bytes);
  return true;
}

} // namespace neverd
