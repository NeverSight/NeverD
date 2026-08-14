//===- ARMLiftMemSingle.cpp - ARM32 single-register load/store lifter ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LDR/LDRB/LDRH/LDRSB/LDRSH and STR/STRB/STRH (including the
/// single-register push/pop aliases) plus the LDRD/STRD pair.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftMemSingle(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  switch (Insn->id) {
  // --- LDR / LDRB / LDRH / LDRSB / LDRSH ---
  case ARM_INS_LDR:
  case ARM_INS_LDRB:
  case ARM_INS_LDRH:
  case ARM_INS_LDRSB:
  case ARM_INS_LDRSH: {
    // capstone aliases a single-register `pop {Rt}` (= `ldr Rt, [sp], #4`) to
    // ARM_INS_LDR with mnemonic "pop" and ONLY the Rt operand (the [sp] base is
    // implicit, mirroring the multi-register `pop` -> LDM alias).  Without this
    // it falls through the op_count<2 guard below and the load is dropped (the
    // value silently stays whatever Rt already held).
    if (ARM.op_count == 1 && isAliasMnemonic(Insn, "pop")) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::LOAD, Dst, {Sp});
      S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(4, 4)});
      break;
    }
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);

    uint16_t LoadSz = 4;
    switch (Insn->id) {
    case ARM_INS_LDRB:
    case ARM_INS_LDRSB:
      LoadSz = 1;
      break;
    case ARM_INS_LDRH:
    case ARM_INS_LDRSH:
      LoadSz = 2;
      break;
    default:
      break;
    }

    if (ARM.operands[1].type == ARM_OP_MEM) {
      auto &MemOp = ARM.operands[1];
      NdVar EA = L.emitSingleMemAddr(S, Insn, ARM, MemOp);

      NdVar Val = S.makeTemp(LoadSz);
      S.emit(NdOp::LOAD, Val, {EA});

      bool SignExt = (Insn->id == ARM_INS_LDRSB || Insn->id == ARM_INS_LDRSH);
      if (SignExt && LoadSz < 4)
        S.emit(NdOp::INT_SEXT, Dst, {Val});
      else if (LoadSz < 4)
        S.emit(NdOp::INT_ZEXT, Dst, {Val});
      else
        S.emit(NdOp::COPY, Dst, {Val});
    } else {
      NdVar Val = L.operandRead(S, ARM.operands[1]);
      S.emit(NdOp::COPY, Dst, {Val});
    }
    break;
  }

  // --- STR / STRB / STRH ---
  case ARM_INS_STR:
  case ARM_INS_STRB:
  case ARM_INS_STRH: {
    // capstone aliases a single-register `push {Rt}` (= `str Rt, [sp, #-4]!`)
    // to ARM_INS_STR with mnemonic "push" and ONLY the Rt operand (the [sp]
    // base is implicit, mirroring the multi-register `push` -> STM alias).
    if (ARM.op_count == 1 && isAliasMnemonic(Insn, "push")) {
      NdVar Src = L.operandRead(S, ARM.operands[0]);
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(4, 4)});
      S.emit(NdOp::STORE, {}, {Sp, Src});
      break;
    }
    if (ARM.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, ARM.operands[0]);

    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STRB)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STRH)
      StoreSz = 2;

    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }

    if (ARM.operands[1].type == ARM_OP_MEM) {
      auto &MemOp = ARM.operands[1];
      NdVar EA = L.emitSingleMemAddr(S, Insn, ARM, MemOp);
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }

  // --- LDRD / STRD ---
  case ARM_INS_LDRD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst1 = L.operandWrite(ARM.operands[0]);
    NdVar Dst2 = L.operandWrite(ARM.operands[1]);
    if (ARM.operands[2].type == ARM_OP_MEM) {
      auto &MemOp = ARM.operands[2];
      // Build the access EA mirroring the single-register LDR path: base +
      // (optionally scaled/shifted/subtracted) index + signed displacement.
      // The old code summed only base + raw disp -> register-offset forms
      // dropped the index and `[Rn,#-imm]` added instead of subtracted.
      bool PostIdx = Insn->detail->writeback && ARM.post_index;
      NdVar EA = L.emitLdrdStrdEA(S, MemOp, PostIdx);

      NdVar EA2 = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
      NdVar V1 = S.makeTemp(4);
      NdVar V2 = S.makeTemp(4);
      S.emit(NdOp::LOAD, V1, {EA});
      S.emit(NdOp::LOAD, V2, {EA2});
      // Both words are read before either architectural destination is
      // updated.  In particular, LDRD may use a destination as its base.
      S.emit(NdOp::COPY, Dst1, {V1});
      S.emit(NdOp::COPY, Dst2, {V2});

      L.emitLdrdStrdWriteback(S, Insn, ARM, MemOp, EA);
    }
    break;
  }
  case ARM_INS_STRD: {
    if (ARM.op_count < 3)
      break;
    NdVar Src1 = L.operandRead(S, ARM.operands[0]);
    NdVar Src2 = L.operandRead(S, ARM.operands[1]);
    if (ARM.operands[2].type == ARM_OP_MEM) {
      auto &MemOp = ARM.operands[2];
      bool PostIdx = Insn->detail->writeback && ARM.post_index;
      NdVar EA = L.emitLdrdStrdEA(S, MemOp, PostIdx);

      S.emit(NdOp::STORE, {}, {EA, Src1});
      NdVar EA2 = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
      S.emit(NdOp::STORE, {}, {EA2, Src2});

      L.emitLdrdStrdWriteback(S, Insn, ARM, MemOp, EA);
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
