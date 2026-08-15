//===- AArch64LiftMem.cpp - AArch64 memory access instruction lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory access instruction handlers for AArch64: LDR/LDUR, STR/STUR,
/// LDP/STP (including write-back and sign-extending variants), and PRFM.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

bool AArch64Lifter::liftMem(LiftState &S, const cs_insn *Insn,
                            const cs_aarch64 &ARM64) {
  switch (Insn->id) {

  // --- LDR / LDUR / STR / STUR ---
  case AARCH64_INS_LDR:
  case AARCH64_INS_LDRB:
  case AARCH64_INS_LDRH:
  case AARCH64_INS_LDRSW:
  case AARCH64_INS_LDRSH:
  case AARCH64_INS_LDRSB:
  case AARCH64_INS_LDUR:
  case AARCH64_INS_LDURB:
  case AARCH64_INS_LDURH:
  case AARCH64_INS_LDURSW:
  case AARCH64_INS_LDURSH:
  case AARCH64_INS_LDURSB: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);

    // Determine correct load size from instruction type
    uint16_t LoadSz = Dst.Size;
    switch (Insn->id) {
    case AARCH64_INS_LDRB:
    case AARCH64_INS_LDRSB:
    case AARCH64_INS_LDURB:
    case AARCH64_INS_LDURSB:
      LoadSz = 1;
      break;
    case AARCH64_INS_LDRH:
    case AARCH64_INS_LDRSH:
    case AARCH64_INS_LDURH:
    case AARCH64_INS_LDURSH:
      LoadSz = 2;
      break;
    case AARCH64_INS_LDRSW:
    case AARCH64_INS_LDURSW:
      LoadSz = 4;
      break;
    default:
      break;
    }

    auto EmitLoad = [&](NdVar EA) {
      NdVar Val = S.makeTemp(LoadSz);
      S.emit(NdOp::LOAD, Val, {EA});

      bool SignExt =
          (Insn->id == AARCH64_INS_LDRSW || Insn->id == AARCH64_INS_LDRSH ||
           Insn->id == AARCH64_INS_LDRSB || Insn->id == AARCH64_INS_LDURSW ||
           Insn->id == AARCH64_INS_LDURSH || Insn->id == AARCH64_INS_LDURSB);
      if (SignExt && LoadSz < Dst.Size)
        S.emit(NdOp::INT_SEXT, Dst, {Val});
      else if (LoadSz < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Val});
      else
        S.emit(NdOp::COPY, Dst, {Val});
    };

    // Load-register-literal encodes a signed imm19 relative to this
    // instruction's PC.  Capstone models its target as a memory displacement,
    // whose public AArch64 detail field is only int32_t; Mach-O virtual
    // addresses such as 0x100000468 are therefore truncated to 0x468 even
    // though the rendered disassembly retains the full address.  Recover the
    // architectural address from the instruction word instead of consuming
    // that lossy detail field.  This mask covers integer and FP/SIMD literal
    // loads; PRFM shares the encoding class but never reaches this LDR switch.
    const uint32_t Word = static_cast<uint32_t>(Insn->bytes[0]) |
                          (static_cast<uint32_t>(Insn->bytes[1]) << 8) |
                          (static_cast<uint32_t>(Insn->bytes[2]) << 16) |
                          (static_cast<uint32_t>(Insn->bytes[3]) << 24);
    const bool IsLiteralLoad =
        Insn->size == 4 && (Word & 0x3B000000u) == 0x18000000u;
    if (IsLiteralLoad) {
      int64_t Imm19 = static_cast<int64_t>((Word >> 5) & 0x7FFFFu);
      if ((Imm19 & 0x40000) != 0)
        Imm19 -= 0x80000;
      const va_t LiteralVA = Insn->address + static_cast<uint64_t>(Imm19 * 4);
      NdVar EA = S.makeTemp(8);
      S.emit(NdOp::COPY, EA, {NdVar::cst(LiteralVA, 8)});
      EmitLoad(EA);
      break;
    }

    // Build EA and emit LOAD with correct size
    if (ARM64.operands[1].type == AARCH64_OP_MEM) {
      auto &M = ARM64.operands[1];
      NdVar EA = S.makeTemp(8);
      bool First = true;
      auto Acc = [&](NdVar V) {
        if (First) {
          S.emit(NdOp::COPY, EA, {V});
          First = false;
        } else
          S.emit(NdOp::INT_ADD, EA, {EA, V});
      };
      if (M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        Acc(NdVar::reg(RI.Offset, 8));
      }
      // Extended/scaled register offset (e.g. `ldr w0,[x1,w2,sxtw #2]`): the
      // 32-bit signed/unsigned index must be sign/zero-extended to 64 bits
      // BEFORE scaling (emitMemIndex), not read as a whole 64-bit register.
      if (M.mem.index != AARCH64_REG_INVALID)
        Acc(emitMemIndex(S, M));
      // For POST-index addressing the displacement is the write-back amount,
      // not part of the effective address (EA = [base]).  Only pre-index /
      // offset forms fold the displacement into the EA.
      if (M.mem.disp != 0 && !ARM64.post_index)
        Acc(NdVar::cst(static_cast<uint64_t>(M.mem.disp), 8));
      if (First)
        Acc(NdVar::cst(0, 8));

      EmitLoad(EA);

      if (Insn->detail->writeback && M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        NdVar BaseReg = NdVar::reg(RI.Offset, 8);
        if (ARM64.post_index) {
          int64_t WBOffset = M.mem.disp;
          if (ARM64.op_count >= 3 && ARM64.operands[2].type == AARCH64_OP_IMM)
            WBOffset = ARM64.operands[2].imm;
          if (WBOffset != 0)
            S.emit(NdOp::INT_ADD, BaseReg,
                   {BaseReg, NdVar::cst(static_cast<uint64_t>(WBOffset), 8)});
        } else {
          S.emit(NdOp::COPY, BaseReg, {EA});
        }
      }
    } else {
      NdVar Val = operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Val});
    }
    break;
  }
  case AARCH64_INS_LDP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst1 = operandWrite(ARM64.operands[0]);
    NdVar Dst2 = operandWrite(ARM64.operands[1]);
    uint16_t RegSize = Dst1.Size > 0 ? Dst1.Size : 8;

    auto &M = ARM64.operands[2];
    NdVar EA = S.makeTemp(8);
    bool First = true;
    auto Acc = [&](NdVar V) {
      if (First) {
        S.emit(NdOp::COPY, EA, {V});
        First = false;
      } else
        S.emit(NdOp::INT_ADD, EA, {EA, V});
    };
    if (M.type == AARCH64_OP_MEM) {
      if (M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        Acc(NdVar::reg(RI.Offset, 8));
      }
      // Post-index: displacement is the write-back amount, not part of the EA.
      if (M.mem.disp != 0 && !ARM64.post_index)
        Acc(NdVar::cst(static_cast<uint64_t>(M.mem.disp), 8));
    }
    if (First)
      Acc(NdVar::cst(0, 8));

    NdVar Val1 = S.makeTemp(RegSize);
    S.emit(NdOp::LOAD, Val1, {EA});
    S.emit(NdOp::COPY, Dst1, {Val1});

    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(RegSize, 8)});
    NdVar Val2 = S.makeTemp(RegSize);
    S.emit(NdOp::LOAD, Val2, {EA2});
    S.emit(NdOp::COPY, Dst2, {Val2});

    if (M.type == AARCH64_OP_MEM && Insn->detail->writeback &&
        M.mem.base != AARCH64_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
      NdVar BaseReg = NdVar::reg(RI.Offset, 8);
      if (ARM64.post_index) {
        int64_t WBOffset = M.mem.disp;
        if (ARM64.op_count >= 4 && ARM64.operands[3].type == AARCH64_OP_IMM)
          WBOffset = ARM64.operands[3].imm;
        if (WBOffset != 0)
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(WBOffset), 8)});
      } else {
        S.emit(NdOp::COPY, BaseReg, {EA});
      }
    }
    break;
  }
  case AARCH64_INS_STR:
  case AARCH64_INS_STRB:
  case AARCH64_INS_STRH:
  case AARCH64_INS_STUR:
  case AARCH64_INS_STURB:
  case AARCH64_INS_STURH: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[0]);

    uint16_t StoreSz = Src.Size;
    switch (Insn->id) {
    case AARCH64_INS_STRB:
    case AARCH64_INS_STURB:
      StoreSz = 1;
      break;
    case AARCH64_INS_STRH:
    case AARCH64_INS_STURH:
      StoreSz = 2;
      break;
    default:
      break;
    }
    if (StoreSz < Src.Size) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }

    if (ARM64.operands[1].type == AARCH64_OP_MEM) {
      NdVar EA = S.makeTemp(8);
      bool First = true;
      auto Acc = [&](NdVar V) {
        if (First) {
          S.emit(NdOp::COPY, EA, {V});
          First = false;
        } else
          S.emit(NdOp::INT_ADD, EA, {EA, V});
      };
      auto &M = ARM64.operands[1];
      if (M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        Acc(NdVar::reg(RI.Offset, 8));
      }
      // Register-offset addressing (e.g. `str w, [x9, w11, uxtw #2]`) scales
      // the index by the LSL/extend shift and sign/zero-extends a 32-bit index
      // (sxtw/uxtw) to 64 bits before scaling.  The STORE path previously read
      // the whole 64-bit index register (dropping the extend) and an earlier
      // version dropped the scale entirely, so indexed scatter stores with a
      // negative sxtw index wrote to a wildly wrong address.
      if (M.mem.index != AARCH64_REG_INVALID)
        Acc(emitMemIndex(S, M));
      // Post-index: displacement is the write-back amount, not part of the EA.
      if (M.mem.disp != 0 && !ARM64.post_index)
        Acc(NdVar::cst(static_cast<uint64_t>(M.mem.disp), 8));
      if (First)
        Acc(NdVar::cst(0, 8));
      S.emit(NdOp::STORE, {}, {EA, Src});

      if (Insn->detail->writeback && M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        NdVar BaseReg = NdVar::reg(RI.Offset, 8);
        if (ARM64.post_index) {
          int64_t WBOffset = M.mem.disp;
          if (ARM64.op_count >= 3 && ARM64.operands[2].type == AARCH64_OP_IMM)
            WBOffset = ARM64.operands[2].imm;
          if (WBOffset != 0)
            S.emit(NdOp::INT_ADD, BaseReg,
                   {BaseReg, NdVar::cst(static_cast<uint64_t>(WBOffset), 8)});
        } else {
          S.emit(NdOp::COPY, BaseReg, {EA});
        }
      }
    }
    break;
  }
  case AARCH64_INS_STP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Src1 = operandRead(S, ARM64.operands[0]);
    NdVar Src2 = operandRead(S, ARM64.operands[1]);
    uint16_t RegSize = Src1.Size > 0 ? Src1.Size : 8;
    if (ARM64.operands[2].type == AARCH64_OP_MEM) {
      NdVar EA = S.makeTemp(8);
      auto &M = ARM64.operands[2];
      bool First = true;
      auto Acc = [&](NdVar V) {
        if (First) {
          S.emit(NdOp::COPY, EA, {V});
          First = false;
        } else
          S.emit(NdOp::INT_ADD, EA, {EA, V});
      };
      if (M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        Acc(NdVar::reg(RI.Offset, 8));
      }
      // Post-index: displacement is the write-back amount, not part of the EA.
      if (M.mem.disp != 0 && !ARM64.post_index)
        Acc(NdVar::cst(static_cast<uint64_t>(M.mem.disp), 8));
      if (First)
        Acc(NdVar::cst(0, 8));
      S.emit(NdOp::STORE, {}, {EA, Src1});
      NdVar EA2 = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(RegSize, 8)});
      S.emit(NdOp::STORE, {}, {EA2, Src2});

      if (Insn->detail->writeback && M.mem.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
        NdVar BaseReg = NdVar::reg(RI.Offset, 8);
        if (ARM64.post_index) {
          int64_t WBOffset = M.mem.disp;
          if (ARM64.op_count >= 4 && ARM64.operands[3].type == AARCH64_OP_IMM)
            WBOffset = ARM64.operands[3].imm;
          if (WBOffset != 0)
            S.emit(NdOp::INT_ADD, BaseReg,
                   {BaseReg, NdVar::cst(static_cast<uint64_t>(WBOffset), 8)});
        } else {
          S.emit(NdOp::COPY, BaseReg, {EA});
        }
      }
    }
    break;
  }

  // PRFM (prefetch)
  case AARCH64_INS_PRFM:
  case AARCH64_INS_PRFUM: {
    S.emit(NdOp::NOP, {}, {});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
