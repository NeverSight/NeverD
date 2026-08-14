//===- ARMLiftControl.cpp - ARM32 control-flow instruction lifter --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow and system instruction handlers for ARM32: B/BL/BX/BLX,
/// CBZ/CBNZ, SVC/HVC/SMC, BKPT/HLT/UDF, barriers, system register access,
/// coprocessor, CDE, MVE loops, and branch future instructions.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftControl(LiftState &S, const cs_insn *Insn,
                            const cs_arm &ARM) {
  switch (Insn->id) {

  // --- B / BL / BX / BLX ---
  case ARM_INS_B: {
    if (ARM.op_count < 1)
      break;
    S.emit(NdOp::BRANCH, {},
           {NdVar::cst(static_cast<uint64_t>(ARM.operands[0].imm), 4)});
    break;
  }
  case ARM_INS_BL: {
    if (ARM.op_count < 1)
      break;
    NdVar Lr = NdVar::reg(armreg::LR, 4);
    S.emit(NdOp::COPY, Lr, {NdVar::cst(S.Addr + Insn->size, 4)});
    S.emit(NdOp::CALL, NdVar::reg(armreg::R0, 4),
           {NdVar::cst(static_cast<uint64_t>(ARM.operands[0].imm), 4)});
    break;
  }
  case ARM_INS_BX: {
    if (ARM.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM.operands[0]);
    // BX LR = return
    if (ARM.operands[0].type == ARM_OP_REG &&
        ARM.operands[0].reg == ARM_REG_LR) {
      S.emit(NdOp::RETURN, {}, {Target});
    } else {
      S.emit(NdOp::INDIR_BR, {}, {Target});
    }
    break;
  }
  case ARM_INS_BLX: {
    if (ARM.op_count < 1)
      break;
    NdVar Lr = NdVar::reg(armreg::LR, 4);
    S.emit(NdOp::COPY, Lr, {NdVar::cst(S.Addr + Insn->size, 4)});
    if (ARM.operands[0].type == ARM_OP_REG) {
      NdVar Target = operandRead(S, ARM.operands[0]);
      S.emit(NdOp::INDIR_CALL, NdVar::reg(armreg::R0, 4), {Target});
    } else {
      S.emit(NdOp::CALL, NdVar::reg(armreg::R0, 4),
             {NdVar::cst(static_cast<uint64_t>(ARM.operands[0].imm), 4)});
    }
    break;
  }
  case ARM_INS_CBZ:
  case ARM_INS_CBNZ: {
    if (ARM.op_count < 2)
      break;
    NdVar RegV = operandRead(S, ARM.operands[0]);
    NdVar Cond = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Cond, {RegV, NdVar::cst(0, 4)});
    if (Insn->id == ARM_INS_CBNZ) {
      NdVar Inv = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, Inv, {Cond});
      Cond = Inv;
    }
    S.emit(NdOp::COND_BR, {},
           {NdVar::cst(static_cast<uint64_t>(ARM.operands[1].imm), 4), Cond});
    break;
  }

  // ========================================================================
  // System / privileged / debug
  // ========================================================================
  case ARM_INS_SVC: {
    if (ARM.op_count >= 1 && ARM.operands[0].type == ARM_OP_IMM)
      S.emitIntrinsic(
          Intrinsic::ArmSvc, NdVar::reg(armreg::R0, 4),
          {NdVar::cst(static_cast<uint64_t>(ARM.operands[0].imm), 4)});
    else
      S.emitIntrinsic(Intrinsic::ArmSvc);
    break;
  }
  case ARM_INS_HVC:
    S.emitIntrinsic(Intrinsic::ArmHvc);
    break;
  case ARM_INS_SMC:
    S.emitIntrinsic(Intrinsic::ArmSmc);
    break;
  case ARM_INS_BKPT:
    S.emitIntrinsic(Intrinsic::ArmBkpt);
    break;
  case ARM_INS_HLT:
    S.emitIntrinsic(Intrinsic::ArmHlt);
    break;
  case ARM_INS_UDF:
  case ARM_INS_TRAP: {
    LowOp LOp;
    LOp.Opcode = NdOp::INTRINSIC;
    LOp.Addr = S.Addr;
    LOp.Seq = S.Seq++;
    LOp.Output = NdVar::reg(armreg::R0, 4);
    LOp.addInput(NdVar::cst(static_cast<uint64_t>(Intrinsic::ArmUdf), 2));
    S.Ops.push_back(LOp);
    break;
  }
  case ARM_INS_DMB:
    S.emitVoidIntrinsic(Intrinsic::ArmDmb);
    break;
  case ARM_INS_DSB:
    S.emitVoidIntrinsic(Intrinsic::ArmDsb);
    break;
  case ARM_INS_ISB:
    S.emitVoidIntrinsic(Intrinsic::ArmIsb);
    break;
  case ARM_INS_CLREX:
    S.emitVoidIntrinsic(Intrinsic::ArmClrex);
    break;
  case ARM_INS_SB:
  case ARM_INS_TSB:
  case ARM_INS_PLD:
  case ARM_INS_PLDW:
  case ARM_INS_PLI:
    S.emit(NdOp::NOP, {}, {});
    break;
  case ARM_INS_MRS: {
    if (ARM.op_count >= 1) {
      NdVar Dst = operandWrite(ARM.operands[0]);
      emitMrsNzcv(S, Dst);
    }
    break;
  }
  case ARM_INS_MSR: {
    if (ARM.op_count >= 2)
      emitMsrNzcv(S, operandRead(S, ARM.operands[1]));
    break;
  }
  case ARM_INS_CPS:
  case ARM_INS_SETEND:
  case ARM_INS_SETPAN:
  case ARM_INS_DBG:
  case ARM_INS_DCPS1:
  case ARM_INS_DCPS2:
  case ARM_INS_DCPS3:
  case ARM_INS_SG:
  case ARM_INS_BTI:
  case ARM_INS_BXJ:
  case ARM_INS_BXNS:
  case ARM_INS_BLXNS:
  case ARM_INS_ERET:
  case ARM_INS_RFEDA:
  case ARM_INS_RFEDB:
  case ARM_INS_RFEIA:
  case ARM_INS_RFEIB:
  case ARM_INS_SRSDA:
  case ARM_INS_SRSDB:
  case ARM_INS_SRSIA:
  case ARM_INS_SRSIB:
  case ARM_INS_LCTP:
  case ARM_INS_CLRM:
  case ARM_INS_TT:
  case ARM_INS_TTA:
  case ARM_INS_TTAT:
  case ARM_INS_TTT:
  case ARM_INS_PAC:
  case ARM_INS_PACBTI:
  case ARM_INS_PACG:
  case ARM_INS_AUT:
  case ARM_INS_AUTG:
  case ARM_INS_BXAUT:
    // These instructions do not share authentication semantics.  Several
    // restore privileged state, change execution mode, access the security
    // extension, or authenticate a destination with distinct operands.  A
    // placeholder intrinsic would turn unsupported instructions into valid
    // but incorrect IR, so strict lifting must fail closed until each has a
    // typed state-transition contract.
    return false;

  // Coprocessor (traditional CP14/CP15 access — system side effects)
  case ARM_INS_MCR:
  case ARM_INS_MCR2:
  case ARM_INS_MCRR:
  case ARM_INS_MCRR2:
  case ARM_INS_MRC:
  case ARM_INS_MRC2:
  case ARM_INS_MRRC:
  case ARM_INS_MRRC2:
  case ARM_INS_CDP:
  case ARM_INS_CDP2:
  case ARM_INS_LDC:
  case ARM_INS_LDC2:
  case ARM_INS_LDC2L:
  case ARM_INS_LDCL:
  case ARM_INS_STC:
  case ARM_INS_STC2:
  case ARM_INS_STC2L:
  case ARM_INS_STCL:
    // Coprocessor forms differ in register direction, width, addressing, and
    // device side effects.  Treating all of them as MSR loses both data flow
    // and architectural state.
    return false;
  // CDE (Custom Datapath Extension).  These encodings carry
  // implementation-defined coprocessor operands.  Operand-free placeholders
  // invented an R0 definition and are therefore less truthful than rejecting
  // the instruction.
  case ARM_INS_CX1:
  case ARM_INS_CX1A:
  case ARM_INS_CX1D:
  case ARM_INS_CX1DA:
  case ARM_INS_CX2:
  case ARM_INS_CX2A:
  case ARM_INS_CX2D:
  case ARM_INS_CX2DA:
  case ARM_INS_CX3:
  case ARM_INS_CX3A:
  case ARM_INS_CX3D:
  case ARM_INS_CX3DA:
    return false;

  // MVE loops
  case ARM_INS_DLS:
  case ARM_INS_DLSTP:
  case ARM_INS_WLS:
  case ARM_INS_WLSTP:
  case ARM_INS_LE:
  case ARM_INS_LETP:
    // Loop instructions update hidden architectural loop state; substituting
    // LE for every form discards that transition.
    return false;

  // Branch future (M-profile)
  case ARM_INS_BF:
  case ARM_INS_BFL:
  case ARM_INS_BFLX:
  case ARM_INS_BFX:
  case ARM_INS_BFCSEL:
    // Branch-future instructions have distinct control-flow and state
    // contracts.  They cannot be represented by an operand-free BFCSEL.
    return false;

  // AES — AESE/AESD are destructive (Vd = op(Vd, Vm)); AESMC/AESIMC mix only
  // the source.  Map to the specific ARM NEON crypto intrinsic so codegen emits
  // the real aese.8/aesmc.8 and Unicorn (max CPU) runs it bit-exactly.
  case ARM_INS_AESE:
  case ARM_INS_AESD: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar OldD = operandRead(S, ARM.operands[0]);
    NdVar Key = operandRead(S, ARM.operands[1]);
    S.emitIntrinsic(Insn->id == ARM_INS_AESE ? Intrinsic::ArmAese
                                             : Intrinsic::ArmAesd,
                    Dst, {OldD, Key});
    break;
  }
  case ARM_INS_AESMC:
  case ARM_INS_AESIMC: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emitIntrinsic(Insn->id == ARM_INS_AESMC ? Intrinsic::ArmAesmc
                                              : Intrinsic::ArmAesimc,
                    Dst, {Src});
    break;
  }
  // SHA1H reads a single source; SHA1SU1/SHA256SU0 are destructive with one
  // extra source; the rest are destructive with two extra sources (SHA1C/P/M
  // take a scalar hash_e as the first extra source).
  case ARM_INS_SHA1H: {
    if (ARM.op_count < 1)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    cs_arm_op SrcCap = ARM.operands[ARM.op_count >= 2 ? 1 : 0];
    // sha1h.32 is typed as Qn but only uses the low Sn lane (vmov sN,rM).
    if (SrcCap.type == ARM_OP_REG && SrcCap.reg >= ARM_REG_Q0 &&
        SrcCap.reg <= ARM_REG_Q15)
      SrcCap.reg =
          static_cast<arm_reg>(ARM_REG_S0 + (SrcCap.reg - ARM_REG_Q0) * 2);
    NdVar Src32 = operandRead(S, SrcCap);
    NdVar Res32 = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::ArmSha1h, Res32, {Src32});
    if (Dst.Size > 4) {
      NdVar Hi = S.makeTemp(Dst.Size - 4);
      S.emit(NdOp::SUBBYTES, Hi, {Dst, NdVar::cst(4, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Res32});
    } else {
      S.emit(NdOp::COPY, Dst, {Res32});
    }
    break;
  }
  case ARM_INS_SHA1SU1:
  case ARM_INS_SHA256SU0: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar OldD = operandRead(S, ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emitIntrinsic(Insn->id == ARM_INS_SHA1SU1 ? Intrinsic::ArmSha1su1
                                                : Intrinsic::ArmSha256su0,
                    Dst, {OldD, Src});
    break;
  }
  // SHA1SU0 is destructive with two extra sources (Qd, Qn, Qm) — three
  // operands, like SHA256H/H2/SU1 — not the two-operand SHA1SU1/SHA256SU0
  // family.
  case ARM_INS_SHA1SU0:
  case ARM_INS_SHA256H:
  case ARM_INS_SHA256H2:
  case ARM_INS_SHA256SU1: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar OldD = operandRead(S, ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case ARM_INS_SHA1SU0:
      Id = Intrinsic::ArmSha1su0;
      break;
    case ARM_INS_SHA256H:
      Id = Intrinsic::ArmSha256h;
      break;
    case ARM_INS_SHA256H2:
      Id = Intrinsic::ArmSha256h2;
      break;
    default:
      Id = Intrinsic::ArmSha256su1;
      break;
    }
    S.emitIntrinsic(Id, Dst, {OldD, A, B});
    break;
  }
  case ARM_INS_SHA1C:
  case ARM_INS_SHA1P:
  case ARM_INS_SHA1M: {
    if (ARM.op_count < 3)
      break;
    auto scalarLaneOp = [](cs_arm_op Op) -> cs_arm_op {
      if (Op.type == ARM_OP_REG && Op.reg >= ARM_REG_Q0 &&
          Op.reg <= ARM_REG_Q15)
        Op.reg = static_cast<arm_reg>(ARM_REG_S0 + (Op.reg - ARM_REG_Q0) * 2);
      return Op;
    };
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar OldD = operandRead(S, ARM.operands[0]);
    NdVar A = operandRead(S, scalarLaneOp(ARM.operands[1]));
    NdVar B = operandRead(S, ARM.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case ARM_INS_SHA1C:
      Id = Intrinsic::ArmSha1c;
      break;
    case ARM_INS_SHA1P:
      Id = Intrinsic::ArmSha1p;
      break;
    default:
      Id = Intrinsic::ArmSha1m;
      break;
    }
    S.emitIntrinsic(Id, Dst, {OldD, A, B});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
