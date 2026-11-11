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
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <cstdio>

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
  const auto DecodeEquivalent = [&](uint32_t Word) {
    uint8_t Encoded[4];
    llvm::support::endian::write32(Encoded, Word,
                                   Little ? llvm::endianness::little
                                          : llvm::endianness::big);
    const uint8_t *Code = Encoded;
    size_t Size = sizeof(Encoded);
    uint64_t PC = Addr;
    InsnBuf->illegal = false;
    if (!cs_disasm_iter(Handle, &Code, &Size, &PC, InsnBuf))
      return false;
    std::copy_n(Bytes, 4, InsnBuf->bytes);
    return true;
  };
  if (MIPSISARevision == 6) {
    const unsigned RS = (Physical >> 21) & 31;
    if ((Physical & 0xfc0007f8) == 0x40000000 && (RS == 2 || RS == 6)) {
      // MFHC0/MTHC0 have the same operand roles as MFC0/MTC0.
      if (!DecodeEquivalent(Physical & ~(2u << 21)))
        return false;
      InsnBuf->id = RS == 2 ? MIPS_INS_MFHC0 : MIPS_INS_MTHC0;
      if (Text)
        std::snprintf(InsnBuf->mnemonic, sizeof(InsnBuf->mnemonic), "%s",
                      cs_insn_name(Handle, InsnBuf->id));
      return true;
    }
    if ((Physical & 0xffe0063f) == 0x7c00003b) {
      const unsigned RD = (Physical >> 11) & 31, RT = (Physical >> 16) & 31;
      const unsigned Selector = (Physical >> 6) & 7;
      if (!mips::supportedR6HardwareRegister((Selector << 5) | RD) ||
          !DecodeEquivalent(0x7c00e83b | (RT << 16)))
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
          std::snprintf(InsnBuf->op_str, sizeof(InsnBuf->op_str),
                        "$%s, $%u, %u", Name, RD, Selector);
        else
          std::snprintf(InsnBuf->op_str, sizeof(InsnBuf->op_str), "$%s, $%u",
                        Name, RD);
      }
      return true;
    }
  }
  if (isMIPS64(TargetArch)) {
    if (!MIPS->isDSPIndexedDoubleword(Physical) ||
        !DecodeEquivalent(Physical & ~0x200u))
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
  const auto Classic = MIPS->microASEWord(MicroPhysical);
  if (!Classic)
    return false;
  // Capstone has classic MSA and DSP tables for forms missing in its
  // microMIPS tables. Preserve physical bytes and address in the public record.
  if (!MicroASEHandle) {
    const auto Mode = static_cast<cs_mode>(
        (MIPSISARevision == 5 ? CS_MODE_MIPS32R5 : CS_MODE_MIPS32R2) |
        (Little ? CS_MODE_LITTLE_ENDIAN : CS_MODE_BIG_ENDIAN));
    if (cs_open(CS_ARCH_MIPS, Mode, &MicroASEHandle) != CS_ERR_OK)
      return false;
    cs_option(MicroASEHandle, CS_OPT_DETAIL, Detail ? CS_OPT_ON : CS_OPT_OFF);
    cs_option(MicroASEHandle, CS_OPT_TEXT, Text ? CS_OPT_ON : CS_OPT_OFF);
  }
  uint8_t Encoded[4];
  llvm::support::endian::write32(Encoded, *Classic,
                                 Little ? llvm::endianness::little
                                        : llvm::endianness::big);
  const uint8_t *Code = Encoded;
  size_t Size = sizeof(Encoded);
  // Classic branch operands are rendered with displacement * 4, micro with * 2.
  uint64_t PC = Addr;
  if (MIPSLifter::hasDelaySlot(*Classic)) {
    const int64_t Offset = int16_t(*Classic);
    const auto Target = uint32_t(Addr + 4 + Offset * 2);
    PC = uint64_t(Target) - (4 + Offset * 4);
  }
  InsnBuf->illegal = false;
  if (!cs_disasm_iter(MicroASEHandle, &Code, &Size, &PC, InsnBuf))
    return false;
  InsnBuf->address = Addr;
  std::copy_n(Bytes, 4, InsnBuf->bytes);
  return true;
}

} // namespace neverd
