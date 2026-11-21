//===- AArch64AddressResult.cpp - Darwin address results ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/AArch64AddressResult.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/DenseSet.h"

namespace neverd {
bool isAArch64LinkRegisterReturn(const DecodedInsn &I) {
  return (I.Id == AARCH64_INS_RET && !I.Raw->detail->aarch64.op_count) ||
         I.Id == AARCH64_INS_RETAA || I.Id == AARCH64_INS_RETAB;
}

// Follow an address through copies, offsets and shared local epilogues to a
// return in X0 or a store in a caller-supplied output slot.
bool darwinAddressValueEscapes(Decoder &Decode, llvm::ArrayRef<uint8_t> Bytes,
                               va_t Start, va_t PC, aarch64_reg AddressReg) {
  constexpr auto Word = aarch64::kInsnSize;
  if (AddressReg == AARCH64_REG_XZR)
    return false;
  bool Stored = false;
  llvm::SmallDenseSet<va_t> Jumps;
  while (PC >= Start && rangeInBounds(PC - Start, Word, Bytes.size())) {
    const auto *At = Bytes.data() + (PC - Start);
    DecodedInsn I{};
    if (Decode.decodeOne(At, Word, PC, I) != int(Word) || !I.Raw ||
        !I.Raw->detail)
      return false;
    if (I.Id == AARCH64_INS_B)
      if (const auto Jump =
              decodeDirectBranch(Arch::AArch64, InstructionMode::Default, At,
                                 Word, PC, {.Jumps = true})) {
        const auto Target = Jump->target();
        if (!Target || !Jumps.insert(PC).second)
          return false;
        PC = *Target;
        continue;
      }
    PC += Word;
    const auto &D = I.Raw->detail->aarch64;
    if (isAArch64LinkRegisterReturn(I))
      return Stored || AddressReg == AARCH64_REG_X0;
    if (!D.op_count || D.operands[0].type != AARCH64_OP_REG)
      return false;
    // Darwin epilogues may restore saved registers after defining the result.
    // W and X views of a register overwrite the same address value.
    if (I.Id == AARCH64_INS_LDP && D.op_count >= 3 &&
        D.operands[1].type == AARCH64_OP_REG &&
        D.operands[2].type == AARCH64_OP_MEM &&
        D.operands[2].mem.base == AARCH64_REG_SP &&
        mapCapstoneReg(D.operands[0].reg).Offset !=
            mapCapstoneReg(AddressReg).Offset &&
        mapCapstoneReg(D.operands[1].reg).Offset !=
            mapCapstoneReg(AddressReg).Offset)
      continue;
    if (I.Id == AARCH64_INS_ADD && D.op_count == 3 &&
        D.operands[0].reg == AARCH64_REG_SP &&
        D.operands[1].type == AARCH64_OP_REG &&
        D.operands[1].reg == AARCH64_REG_SP &&
        D.operands[2].type == AARCH64_OP_IMM)
      continue;
    if (D.op_count == 2 &&
        (I.Id == AARCH64_INS_STR || I.Id == AARCH64_INS_STUR) &&
        D.operands[0].reg == AddressReg &&
        D.operands[1].type == AARCH64_OP_MEM &&
        D.operands[1].mem.base != AddressReg &&
        D.operands[1].mem.index != AddressReg) {
      const auto Base = D.operands[1].mem.base;
      Stored |= Base != AARCH64_REG_SP && Base != AARCH64_REG_FP;
    } else {
      const bool Copy =
          D.op_count == 2 && (I.Id == AARCH64_INS_MOV ||
                              (I.Id == AARCH64_INS_ORR && I.Raw->is_alias));
      const bool Offset =
          D.op_count == 3 &&
          (I.Id == AARCH64_INS_ADD || I.Id == AARCH64_INS_SUB) &&
          D.operands[2].type == AARCH64_OP_IMM;
      if ((!Copy && !Offset) || D.operands[0].reg < AARCH64_REG_X0 ||
          D.operands[0].reg > AARCH64_REG_X28 ||
          D.operands[1].type != AARCH64_OP_REG ||
          D.operands[1].reg != AddressReg)
        return false;
      AddressReg = D.operands[0].reg;
    }
  }
  return false;
}
} // namespace neverd
