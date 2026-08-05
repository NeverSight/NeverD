//===- ARMLiftMem.cpp - ARM32 memory access instruction lifter ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory access instruction handlers for ARM32: LDR/STR (including
/// sign-extending and byte/halfword variants), PUSH/POP, LDM/STM,
/// LDRD/STRD, LDREX/STREX (exclusive), LDA/STL (acquire/release),
/// and unprivileged LDRT/STRT.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

static bool isAliasMnemonic(const cs_insn *Insn, const char *Alias) {
  size_t AliasLen = std::strlen(Alias);
  return std::strcmp(Insn->mnemonic, Alias) == 0 ||
         (std::strncmp(Insn->mnemonic, Alias, AliasLen) == 0 &&
          std::strcmp(Insn->mnemonic + AliasLen, ".w") == 0);
}

NdVar ARMLifter::emitLdrdStrdEA(LiftState &S, const cs_arm_op &MemOp,
                                bool PostIndex) {
  NdVar EA = S.makeTemp(4);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, EA, {V});
      First = false;
    } else
      S.emit(NdOp::INT_ADD, EA, {EA, V});
  };
  if (MemOp.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
    Acc(NdVar::reg(RI.Offset, 4));
  }
  if (MemOp.mem.index != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.index));
    NdVar IdxVal = NdVar::reg(RI.Offset, 4);
    // Shift the index first, then apply its sign exactly once: capstone signals
    // a subtracted index via mem.scale == -1 and/or `subtracted`, which denote
    // the SAME sign — applying both double-negates `[Rn,-Rm]` (#387).
    if (MemOp.shift.type != ARM_SFT_INVALID &&
        (MemOp.shift.value != 0 || MemOp.shift.type == ARM_SFT_RRX))
      IdxVal = emitImmShift(S, IdxVal, MemOp.shift.type, MemOp.shift.value);
    if (MemOp.mem.scale == -1 || MemOp.subtracted) {
      NdVar Neg = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, Neg, {IdxVal});
      Acc(Neg);
    } else {
      Acc(IdxVal);
    }
  }
  // Post-indexed: capstone carries the post-increment in mem.disp, but the
  // access is at the unmodified base — exclude the displacement here (the
  // writeback helper applies it to the base afterward).
  if (!PostIndex && MemOp.mem.disp != 0) {
    int64_t SignedDisp = MemOp.mem.disp;
    if (MemOp.subtracted && MemOp.mem.index == ARM_REG_INVALID)
      SignedDisp = -(SignedDisp < 0 ? -SignedDisp : SignedDisp);
    Acc(NdVar::cst(static_cast<uint64_t>(SignedDisp), 4));
  }
  if (First)
    Acc(NdVar::cst(0, 4));
  return EA;
}

void ARMLifter::emitLdrdStrdWriteback(LiftState &S, const cs_insn *Insn,
                                      const cs_arm &ARM, const cs_arm_op &MemOp,
                                      NdVar EA) {
  if (!Insn->detail->writeback || MemOp.mem.base == ARM_REG_INVALID)
    return;
  auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
  NdVar BaseReg = NdVar::reg(RI.Offset, 4);
  if (ARM.post_index) {
    // Post-indexed: the access used the unmodified base (capstone leaves
    // mem.disp==0 and carries the increment in the trailing operand, which for
    // LDRD/STRD follows the memory operand at operands[3]).
    int64_t WBOffset = MemOp.mem.disp;
    if (ARM.op_count >= 4 && ARM.operands[3].type == ARM_OP_IMM)
      WBOffset = ARM.operands[3].imm;
    if (MemOp.subtracted)
      WBOffset = -(WBOffset < 0 ? -WBOffset : WBOffset);
    if (WBOffset != 0)
      S.emit(NdOp::INT_ADD, BaseReg,
             {BaseReg,
              NdVar::cst(
                  static_cast<uint64_t>(static_cast<uint32_t>(WBOffset)), 4)});
  } else {
    // Pre-indexed: base takes the computed access address.
    S.emit(NdOp::COPY, BaseReg, {EA});
  }
}

NdVar ARMLifter::emitSingleMemAddr(LiftState &S, const cs_insn *Insn,
                                     const cs_arm &ARM,
                                     const cs_arm_op &MemOp) {
  // Post-indexed access uses the UNMODIFIED base; the entire offset (register
  // or immediate) folds into the base writeback rather than the access address
  // — otherwise it is double-applied (once to the address, once by the
  // writeback).
  bool PostIdx = Insn->detail->writeback && ARM.post_index;
  NdVar EA = S.makeTemp(4);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, EA, {V});
      First = false;
    } else
      S.emit(NdOp::INT_ADD, EA, {EA, V});
  };
  if (MemOp.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
    Acc(NdVar::reg(RI.Offset, 4));
  }
  // Index register offset: shift first, then sign exactly once.  capstone marks
  // a subtracted index via mem.scale==-1 and/or `subtracted` (the SAME sign,
  // not cumulative; #387 `ldrsb r1,[r9,-r6]`).  ROR/RRX must rotate, not
  // left-shift.
  bool HasIndex = (MemOp.mem.index != ARM_REG_INVALID);
  NdVar IndexOff;
  if (HasIndex) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.index));
    NdVar IdxVal = NdVar::reg(RI.Offset, 4);
    if (MemOp.shift.type != ARM_SFT_INVALID &&
        (MemOp.shift.value != 0 || MemOp.shift.type == ARM_SFT_RRX))
      IdxVal = emitImmShift(S, IdxVal, MemOp.shift.type, MemOp.shift.value);
    if (MemOp.mem.scale == -1 || MemOp.subtracted) {
      IndexOff = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, IndexOff, {IdxVal});
    } else {
      IndexOff = IdxVal;
    }
    if (!PostIdx)
      Acc(IndexOff);
  }
  int64_t SignedDisp = MemOp.mem.disp;
  if (MemOp.subtracted && !HasIndex)
    SignedDisp = -(SignedDisp < 0 ? -SignedDisp : SignedDisp);
  if (!PostIdx && SignedDisp != 0)
    Acc(NdVar::cst(static_cast<uint64_t>(SignedDisp), 4));
  if (First)
    Acc(NdVar::cst(0, 4));

  if (Insn->detail->writeback && MemOp.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(MemOp.mem.base));
    NdVar BaseReg = NdVar::reg(RI.Offset, 4);
    if (!PostIdx) {
      // Pre-indexed: base takes the computed access address.
      S.emit(NdOp::COPY, BaseReg, {EA});
    } else if (HasIndex) {
      // Register post-index: base advances by the (shifted/signed) index.
      S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, IndexOff});
    } else {
      int64_t WBOffset = MemOp.mem.disp;
      if (ARM.op_count >= 3 && ARM.operands[2].type == ARM_OP_IMM)
        WBOffset = ARM.operands[2].imm;
      if (MemOp.subtracted)
        WBOffset = -(WBOffset < 0 ? -WBOffset : WBOffset);
      if (WBOffset != 0)
        S.emit(NdOp::INT_ADD, BaseReg,
               {BaseReg, NdVar::cst(static_cast<uint64_t>(
                                          static_cast<uint32_t>(WBOffset)),
                                      4)});
    }
  }
  return EA;
}

bool ARMLifter::liftMem(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
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
      NdVar Dst = operandWrite(ARM.operands[0]);
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::LOAD, Dst, {Sp});
      S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(4, 4)});
      if (Dst.Offset == armreg::PC)
        S.emit(NdOp::RETURN, {}, {NdVar::reg(armreg::PC, 4)});
      break;
    }
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);

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
      NdVar EA = emitSingleMemAddr(S, Insn, ARM, MemOp);

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
      NdVar Val = operandRead(S, ARM.operands[1]);
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
      NdVar Src = operandRead(S, ARM.operands[0]);
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(4, 4)});
      S.emit(NdOp::STORE, {}, {Sp, Src});
      break;
    }
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[0]);

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
      NdVar EA = emitSingleMemAddr(S, Insn, ARM, MemOp);
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }

  // --- PUSH / POP ---
  case ARM_INS_PUSH: {
    int NumRegs = ARM.op_count;
    NdVar Sp = NdVar::reg(armreg::SP, 4);
    S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
    for (int I = 0; I < NumRegs; I++) {
      NdVar R = operandRead(S, ARM.operands[I]);
      NdVar Slot = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
      S.emit(NdOp::STORE, {}, {Slot, R});
    }
    break;
  }
  case ARM_INS_POP: {
    int NumRegs = ARM.op_count;
    NdVar Sp = NdVar::reg(armreg::SP, 4);
    bool PopsPC = false;
    for (int I = 0; I < NumRegs; I++) {
      NdVar Slot = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
      NdVar Val = S.makeTemp(4);
      S.emit(NdOp::LOAD, Val, {Slot});
      NdVar Dst = operandWrite(ARM.operands[I]);
      S.emit(NdOp::COPY, Dst, {Val});
      if (Dst.Offset == armreg::PC)
        PopsPC = true;
    }
    S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
    if (PopsPC) {
      S.emit(NdOp::RETURN, {}, {NdVar::reg(armreg::PC, 4)});
    }
    break;
  }

  // --- LDM / STM ---
  case ARM_INS_LDM:
  case ARM_INS_LDMIB:
  case ARM_INS_LDMDA:
  case ARM_INS_LDMDB: {
    // capstone decodes `pop {regs}` (= ldmia sp!, {regs}) as ARM_INS_LDM with
    // mnemonic "pop" and NO explicit base operand: every operand is part of the
    // register list and SP is the implicit base.  The generic path below treats
    // operands[0] as the base, which would use the first popped register as the
    // load address and drop one register from the list.
    if (isAliasMnemonic(Insn, "pop")) {
      int NumRegs = ARM.op_count;
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      bool PopsPC = false;
      for (int I = 0; I < NumRegs; I++) {
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
        NdVar Val = S.makeTemp(4);
        S.emit(NdOp::LOAD, Val, {Slot});
        NdVar Dst = operandWrite(ARM.operands[I]);
        S.emit(NdOp::COPY, Dst, {Val});
        if (Dst.Offset == armreg::PC)
          PopsPC = true;
      }
      S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
      if (PopsPC)
        S.emit(NdOp::RETURN, {}, {NdVar::reg(armreg::PC, 4)});
      break;
    }
    if (ARM.op_count < 2)
      break;
    auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
    NdVar Base = NdVar::reg(RI.Offset, 4);

    int OffsetStart = 0;
    int Delta = 4;
    if (Insn->id == ARM_INS_LDMIB) {
      OffsetStart = 4;
    } else if (Insn->id == ARM_INS_LDMDA) {
      OffsetStart = -4 * (ARM.op_count - 2);
    } else if (Insn->id == ARM_INS_LDMDB) {
      OffsetStart = -4 * (ARM.op_count - 1);
    }

    bool LoadsPC = false;
    for (int I = 1; I < ARM.op_count; I++) {
      int Off = OffsetStart + (I - 1) * Delta;
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Off)), 4)});
      NdVar Val = S.makeTemp(4);
      S.emit(NdOp::LOAD, Val, {EA});
      NdVar Dst = operandWrite(ARM.operands[I]);
      S.emit(NdOp::COPY, Dst, {Val});
      if (Dst.Offset == armreg::PC)
        LoadsPC = true;
    }
    if (Insn->detail->writeback) {
      int Total = (ARM.op_count - 1) * 4;
      if (Insn->id == ARM_INS_LDM || Insn->id == ARM_INS_LDMIB)
        S.emit(NdOp::INT_ADD, Base, {Base, NdVar::cst(Total, 4)});
      else
        S.emit(NdOp::INT_SUB, Base, {Base, NdVar::cst(Total, 4)});
    }
    if (LoadsPC) {
      S.emit(NdOp::RETURN, {}, {NdVar::reg(armreg::PC, 4)});
    }
    break;
  }
  case ARM_INS_STM:
  case ARM_INS_STMIB:
  case ARM_INS_STMDA:
  case ARM_INS_STMDB: {
    // capstone decodes `push {regs}` (= stmdb sp!, {regs}) as ARM_INS_STMDB
    // with mnemonic "push" and NO explicit base operand: every operand is part
    // of the register list and SP is the implicit base.  Treating operands[0]
    // as the base (the generic path below) would use the first pushed register
    // as the store address, drop one register, and — critically — leave SP
    // undecremented so the callee-saved save area is missing from the computed
    // frame size.
    if (isAliasMnemonic(Insn, "push")) {
      int NumRegs = ARM.op_count;
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(4 * NumRegs, 4)});
      for (int I = 0; I < NumRegs; I++) {
        NdVar R = operandRead(S, ARM.operands[I]);
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(4 * I, 4)});
        S.emit(NdOp::STORE, {}, {Slot, R});
      }
      break;
    }
    if (ARM.op_count < 2)
      break;
    auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
    NdVar Base = NdVar::reg(RI.Offset, 4);

    int OffsetStart = 0;
    int Delta = 4;
    if (Insn->id == ARM_INS_STMIB) {
      OffsetStart = 4;
    } else if (Insn->id == ARM_INS_STMDA) {
      OffsetStart = -4 * (ARM.op_count - 2);
    } else if (Insn->id == ARM_INS_STMDB) {
      OffsetStart = -4 * (ARM.op_count - 1);
    }

    for (int I = 1; I < ARM.op_count; I++) {
      NdVar Src = operandRead(S, ARM.operands[I]);
      int Off = OffsetStart + (I - 1) * Delta;
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Off)), 4)});
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    if (Insn->detail->writeback) {
      int Total = (ARM.op_count - 1) * 4;
      if (Insn->id == ARM_INS_STM || Insn->id == ARM_INS_STMIB)
        S.emit(NdOp::INT_ADD, Base, {Base, NdVar::cst(Total, 4)});
      else
        S.emit(NdOp::INT_SUB, Base, {Base, NdVar::cst(Total, 4)});
    }
    break;
  }

  // --- LDRD / STRD ---
  case ARM_INS_LDRD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst1 = operandWrite(ARM.operands[0]);
    NdVar Dst2 = operandWrite(ARM.operands[1]);
    if (ARM.operands[2].type == ARM_OP_MEM) {
      auto &MemOp = ARM.operands[2];
      // Build the access EA mirroring the single-register LDR path: base +
      // (optionally scaled/shifted/subtracted) index + signed displacement.
      // The old code summed only base + raw disp -> register-offset forms
      // dropped the index and `[Rn,#-imm]` added instead of subtracted.
      bool PostIdx = Insn->detail->writeback && ARM.post_index;
      NdVar EA = emitLdrdStrdEA(S, MemOp, PostIdx);

      NdVar V1 = S.makeTemp(4);
      S.emit(NdOp::LOAD, V1, {EA});
      S.emit(NdOp::COPY, Dst1, {V1});

      NdVar EA2 = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
      NdVar V2 = S.makeTemp(4);
      S.emit(NdOp::LOAD, V2, {EA2});
      S.emit(NdOp::COPY, Dst2, {V2});

      emitLdrdStrdWriteback(S, Insn, ARM, MemOp, EA);
    }
    break;
  }
  case ARM_INS_STRD: {
    if (ARM.op_count < 3)
      break;
    NdVar Src1 = operandRead(S, ARM.operands[0]);
    NdVar Src2 = operandRead(S, ARM.operands[1]);
    if (ARM.operands[2].type == ARM_OP_MEM) {
      auto &MemOp = ARM.operands[2];
      bool PostIdx = Insn->detail->writeback && ARM.post_index;
      NdVar EA = emitLdrdStrdEA(S, MemOp, PostIdx);

      S.emit(NdOp::STORE, {}, {EA, Src1});
      NdVar EA2 = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
      S.emit(NdOp::STORE, {}, {EA2, Src2});

      emitLdrdStrdWriteback(S, Insn, ARM, MemOp, EA);
    }
    break;
  }

  // ========================================================================
  // Load exclusive / store exclusive / acquire / release
  // ========================================================================
  case ARM_INS_LDREX:
  case ARM_INS_LDREXB:
  case ARM_INS_LDREXH:
  case ARM_INS_LDA:
  case ARM_INS_LDAB:
  case ARM_INS_LDAH:
  case ARM_INS_LDAEX:
  case ARM_INS_LDAEXB:
  case ARM_INS_LDAEXH: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[1])
                     : operandRead(S, ARM.operands[1]);
    // Byte/halfword forms access 1/2 bytes and zero-extend into the register;
    // the old code loaded the full register width (4 bytes).
    uint16_t LoadSz = 4;
    if (Insn->id == ARM_INS_LDREXB || Insn->id == ARM_INS_LDAB ||
        Insn->id == ARM_INS_LDAEXB)
      LoadSz = 1;
    else if (Insn->id == ARM_INS_LDREXH || Insn->id == ARM_INS_LDAH ||
             Insn->id == ARM_INS_LDAEXH)
      LoadSz = 2;
    if (LoadSz < 4) {
      NdVar Val = S.makeTemp(LoadSz);
      S.emit(NdOp::LOAD, Val, {EA});
      S.emit(NdOp::INT_ZEXT, Dst, {Val});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case ARM_INS_LDREXD:
  case ARM_INS_LDAEXD: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst1 = operandWrite(ARM.operands[0]);
    NdVar Dst2 = operandWrite(ARM.operands[1]);
    NdVar EA = (ARM.operands[2].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[2])
                     : operandRead(S, ARM.operands[2]);
    S.emit(NdOp::LOAD, Dst1, {EA});
    NdVar EA2 = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
    S.emit(NdOp::LOAD, Dst2, {EA2});
    break;
  }
  case ARM_INS_STREX:
  case ARM_INS_STREXB:
  case ARM_INS_STREXH:
  case ARM_INS_STLEX:
  case ARM_INS_STLEXB:
  case ARM_INS_STLEXH: {
    if (ARM.op_count < 3)
      break;
    NdVar Status = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STREXB || Insn->id == ARM_INS_STLEXB)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STREXH || Insn->id == ARM_INS_STLEXH)
      StoreSz = 2;
    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }
    NdVar EA = (ARM.operands[2].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[2])
                     : operandRead(S, ARM.operands[2]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    S.emit(NdOp::COPY, Status, {NdVar::cst(0, 4)});
    break;
  }
  case ARM_INS_STREXD:
  case ARM_INS_STLEXD: {
    if (ARM.op_count < 4)
      break;
    NdVar Status = operandWrite(ARM.operands[0]);
    NdVar Src1 = operandRead(S, ARM.operands[1]);
    NdVar Src2 = operandRead(S, ARM.operands[2]);
    NdVar EA = (ARM.operands[3].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[3])
                     : operandRead(S, ARM.operands[3]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(4, 4)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    S.emit(NdOp::COPY, Status, {NdVar::cst(0, 4)});
    break;
  }
  case ARM_INS_STL:
  case ARM_INS_STLB:
  case ARM_INS_STLH: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[0]);
    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STLB)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STLH)
      StoreSz = 2;
    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[1])
                     : operandRead(S, ARM.operands[1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case ARM_INS_SWP:
  case ARM_INS_SWPB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar EA = (ARM.operands[2].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[2])
                     : operandRead(S, ARM.operands[2]);
    S.emit(NdOp::LOAD, Dst, {EA});
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }

  // Unprivileged load/store
  case ARM_INS_LDRT:
  case ARM_INS_LDRBT:
  case ARM_INS_LDRHT:
  case ARM_INS_LDRSBT:
  case ARM_INS_LDRSHT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[1])
                     : operandRead(S, ARM.operands[1]);
    // Byte/halfword forms access 1/2 bytes; sign- or zero-extend into the
    // register.  The old code loaded the full register width (4 bytes).
    uint16_t LoadSz = 4;
    if (Insn->id == ARM_INS_LDRBT || Insn->id == ARM_INS_LDRSBT)
      LoadSz = 1;
    else if (Insn->id == ARM_INS_LDRHT || Insn->id == ARM_INS_LDRSHT)
      LoadSz = 2;
    bool SignExt = (Insn->id == ARM_INS_LDRSBT || Insn->id == ARM_INS_LDRSHT);
    if (LoadSz < 4) {
      NdVar Val = S.makeTemp(LoadSz);
      S.emit(NdOp::LOAD, Val, {EA});
      S.emit(SignExt ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Val});
    } else {
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case ARM_INS_STRT:
  case ARM_INS_STRBT:
  case ARM_INS_STRHT: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[0]);
    uint16_t StoreSz = 4;
    if (Insn->id == ARM_INS_STRBT)
      StoreSz = 1;
    else if (Insn->id == ARM_INS_STRHT)
      StoreSz = 2;
    if (StoreSz < 4) {
      NdVar Trunc = S.makeTemp(StoreSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
      Src = Trunc;
    }
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[1])
                     : operandRead(S, ARM.operands[1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
