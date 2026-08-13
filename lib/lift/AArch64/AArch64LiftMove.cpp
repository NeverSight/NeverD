//===- AArch64LiftMove.cpp - AArch64 hint and register move ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The HINT hint aliases (YIELD/WFE/WFI/SEV/SEVL) and the move
/// family: MOV, ORN, and the wide immediate moves MOVZ/MOVN/
/// MOVK.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftMove(AArch64Lifter &L, AArch64Lifter::LiftState &S,
              const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  case AARCH64_INS_HINT:
    if (Insn->is_alias) {
      switch (Insn->alias_id) {
      case AARCH64_INS_ALIAS_YIELD:
        S.emitVoidIntrinsic(Intrinsic::Yield_A64);
        break;
      case AARCH64_INS_ALIAS_WFE:
        S.emitVoidIntrinsic(Intrinsic::Wfe);
        break;
      case AARCH64_INS_ALIAS_WFI:
        S.emitVoidIntrinsic(Intrinsic::Wfi);
        break;
      case AARCH64_INS_ALIAS_SEV:
        S.emitVoidIntrinsic(Intrinsic::Sev);
        break;
      case AARCH64_INS_ALIAS_SEVL:
        S.emitVoidIntrinsic(Intrinsic::Sevl);
        break;
      default:
        S.emit(NdOp::NOP, {}, {});
        break;
      }
    } else {
      S.emit(NdOp::NOP, {}, {});
    }
    break;

  // --- MOV / MVN ---
  case AARCH64_INS_MOV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    S.emit(NdOp::COPY, Dst, {Src});
    // W/X view synchronization is handled uniformly by the W→X zero-extension
    // post-pass in lift() plus the table-driven sub-register fixup.  Emitting
    // an extra `COPY Wd, Ws` here for a 64-bit `mov Xd, Xs` is redundant (the
    // 64-bit COPY already defines the Wd view) and actively harmful: it reads
    // Ws independently, so a later write to Ws gets the wrong SSA version and
    // can clobber Xd (bug #157g, e.g. `mov x8,x0; mov w0,#1`).
    break;
  }
  case AARCH64_INS_ORN: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    if (ARM64.op_count == 2) {
      S.emit(NdOp::INT_NOT, Dst, {Src});
    } else {
      NdVar A = L.operandRead(S, ARM64.operands[1]);
      NdVar Inv = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, Inv, {Src});
      S.emit(NdOp::INT_OR, Dst, {A, Inv});
    }
    break;
  }
  case AARCH64_INS_MOVZ: {
    // MOVZ: Dst = imm16 << shift (zeroing other Bits)
    // Capstone 6 alias: may report MOV with final value directly.
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;
    uint64_t Val;
    if (Insn->is_alias) {
      Val = static_cast<uint64_t>(ARM64.operands[1].imm);
    } else {
      uint64_t Imm16 = static_cast<uint64_t>(ARM64.operands[1].imm) & 0xFFFF;
      uint32_t Shift = 0;
      if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
        Shift = ARM64.operands[1].shift.value;
      Val = Imm16 << Shift;
    }
    S.emit(NdOp::COPY, Dst, {NdVar::cst(Val, Sz)});
    break;
  }
  case AARCH64_INS_MOVN: {
    // MOVN: Dst = ~(Imm16 << Shift)
    // Capstone 6 alias: reports MOV with final value as Imm.
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;
    uint64_t Val;
    if (Insn->is_alias) {
      Val = static_cast<uint64_t>(ARM64.operands[1].imm);
    } else {
      uint64_t Imm16 = static_cast<uint64_t>(ARM64.operands[1].imm) & 0xFFFF;
      uint32_t Shift = 0;
      if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
        Shift = ARM64.operands[1].shift.value;
      Val = ~(Imm16 << Shift);
    }
    if (Sz == 4)
      Val &= 0xFFFFFFFF;
    S.emit(NdOp::COPY, Dst, {NdVar::cst(Val, Sz)});
    break;
  }
  case AARCH64_INS_MOVK: {
    // MOVK: insert 16 bits at Shift position, keep other bits
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;
    uint64_t Imm16 = static_cast<uint64_t>(ARM64.operands[1].imm) & 0xFFFF;
    uint32_t Shift = 0;
    if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
      Shift = ARM64.operands[1].shift.value;
    uint64_t Mask = ~(static_cast<uint64_t>(0xFFFF) << Shift);
    if (Sz == 4)
      Mask &= 0xFFFFFFFF;
    NdVar Cleared = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cleared,
           {NdVar::reg(Dst.Offset, Sz), NdVar::cst(Mask, Sz)});
    uint64_t Inserted = Imm16 << Shift;
    if (Sz == 4)
      Inserted &= 0xFFFFFFFF;
    S.emit(NdOp::INT_OR, Dst, {Cleared, NdVar::cst(Inserted, Sz)});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
