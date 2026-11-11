//===- MIPSDecoder.cpp - MIPS ASE physical decode adapters ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>

namespace neverd {
namespace {
#define NEVERD_MIPS_PHYSICAL_FORM(Name, Offset, Text)                          \
  constexpr char Name##Mnemonic[] = Text;
#include "neverd/lift/MIPS/MIPSPhysicalForms.def"
} // namespace

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
      std::copy_n(NoOpAndLinkMnemonic, sizeof(NoOpAndLinkMnemonic),
                  InsnBuf->mnemonic);
    if (Detail) {
      *D = {};
      D->regs_read[D->regs_read_count++] = MIPS_REG_PC;
      D->regs_write[D->regs_write_count++] = MIPS_REG_RA;
    }
    return true;
  }
  if (isMIPS64(TargetArch)) {
    const auto Physical = readTargetEndian<uint32_t>(Bytes, Little);
    if (!MIPS->isDSPIndexedDoubleword(Physical))
      return false;
    // LDX has the identical operand layout and access roles as LWX. Capstone
    // provides the operand detail. Keep LDX's own physical identity and bytes.
    uint8_t Encoded[4];
    llvm::support::endian::write32(Encoded, Physical & ~0x200u,
                                   Little ? llvm::endianness::little
                                          : llvm::endianness::big);
    const uint8_t *Code = Encoded;
    size_t Size = sizeof(Encoded);
    uint64_t PC = Addr;
    InsnBuf->illegal = false;
    if (!cs_disasm_iter(Handle, &Code, &Size, &PC, InsnBuf))
      return false;
    InsnBuf->id = MIPSLifter::IndexedLoadDoublewordID;
    if (Text)
      std::copy_n(IndexedLoadDoublewordMnemonic,
                  sizeof(IndexedLoadDoublewordMnemonic), InsnBuf->mnemonic);
    std::copy_n(Bytes, 4, InsnBuf->bytes);
    return true;
  }
  if (CurrentMode != InstructionMode::MicroMIPS)
    return false;
  const uint32_t Physical = uint32_t(readTargetEndian<uint16_t>(Bytes, Little))
                                << 16 |
                            readTargetEndian<uint16_t>(Bytes + 2, Little);
  const auto Classic = MIPS->microASEWord(Physical);
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
