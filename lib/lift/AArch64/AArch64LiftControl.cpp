//===- AArch64LiftControl.cpp - AArch64 control-flow instruction lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow and system instruction handlers for AArch64: B/Bcc, CBZ/CBNZ,
/// TBZ/TBNZ, BL/BLR/BR/RET, ERET, SVC/HVC/SMC, BRK/HLT, barriers,
/// system register access, and pointer authentication.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

NdVar AArch64Lifter::buildCondCode(AArch64CC_CondCode CC, LiftState &S) {
  NdVar Cond = S.makeTemp(1);
  switch (CC) {
  case AArch64CC_EQ:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
    break;
  case AArch64CC_NE:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
    break;
  case AArch64CC_LT:
    S.emit(NdOp::INT_NOTEQUAL, Cond,
           {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
    break;
  case AArch64CC_GE:
    S.emit(NdOp::INT_EQUAL, Cond,
           {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
    break;
  case AArch64CC_GT: {
    NdVar NZ = S.makeTemp(1);
    NdVar EqFlags = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
    S.emit(NdOp::INT_EQUAL, EqFlags,
           {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
    S.emit(NdOp::BOOL_AND, Cond, {NZ, EqFlags});
    break;
  }
  case AArch64CC_LE: {
    NdVar NeFlags = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, NeFlags,
           {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
    S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NeFlags});
    break;
  }
  case AArch64CC_HS:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
    break;
  case AArch64CC_LO:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
    break;
  case AArch64CC_HI: {
    NdVar NZ = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
    S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(a64reg::CFLAG, 1), NZ});
    break;
  }
  case AArch64CC_LS: {
    NdVar NC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(a64reg::CFLAG, 1)});
    S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NC});
    break;
  }
  case AArch64CC_MI:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
    break;
  case AArch64CC_PL:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
    break;
  case AArch64CC_VS:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
    break;
  case AArch64CC_VC:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
    break;
  default:
    S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
    break;
  }
  return Cond;
}

bool AArch64Lifter::liftControl(LiftState &S, const cs_insn *Insn,
                                const cs_aarch64 &ARM64) {
  switch (Insn->id) {

  // --- B / BL / BR / BLR / RET ---
  case AARCH64_INS_B: {
    if (ARM64.op_count < 1)
      break;
    if (ARM64.cc != AArch64CC_AL && ARM64.cc != AArch64CC_Invalid) {
      // Conditional branch
      NdVar Cond = buildCondCode(ARM64.cc, S);
      S.emit(
          NdOp::COND_BR, {},
          {NdVar::cst(static_cast<uint64_t>(ARM64.operands[0].imm), 8), Cond});
    } else {
      S.emit(NdOp::BRANCH, {},
             {NdVar::cst(static_cast<uint64_t>(ARM64.operands[0].imm), 8)});
    }
    break;
  }
  case AARCH64_INS_CBZ:
  case AARCH64_INS_CBNZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar RegV = operandRead(S, ARM64.operands[0]);
    NdVar Cond = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Cond, {RegV, NdVar::cst(0, RegV.Size)});
    if (Insn->id == AARCH64_INS_CBNZ) {
      NdVar Inv = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, Inv, {Cond});
      Cond = Inv;
    }
    S.emit(NdOp::COND_BR, {},
           {NdVar::cst(static_cast<uint64_t>(ARM64.operands[1].imm), 8), Cond});
    break;
  }
  case AARCH64_INS_BL: {
    if (ARM64.op_count < 1)
      break;
    NdVar Lr = NdVar::reg(a64reg::X30, 8);
    S.emit(NdOp::COPY, Lr, {NdVar::cst(S.Addr + 4, 8)});
    // Define the full X0 return register: a 64-bit callee result lands here in
    // full, and a 32-bit result is zero-extended into X0 by the W-write rule,
    // so X0 always holds the complete return state.  A narrower W0 read is
    // recovered as a SUBBYTES of X0 by fixupSubRegisters.
    S.emit(NdOp::CALL, NdVar::reg(a64reg::X0, 8),
           {NdVar::cst(static_cast<uint64_t>(ARM64.operands[0].imm), 8)});
    break;
  }
  case AARCH64_INS_BLR: {
    if (ARM64.op_count < 1)
      break;
    NdVar Lr = NdVar::reg(a64reg::X30, 8);
    S.emit(NdOp::COPY, Lr, {NdVar::cst(S.Addr + 4, 8)});
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::INDIR_CALL, NdVar::reg(a64reg::X0, 8), {Target});
    break;
  }
  case AARCH64_INS_BR: {
    if (ARM64.op_count < 1)
      break;
    NdVar Target = operandRead(S, ARM64.operands[0]);
    S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }
  case AARCH64_INS_RET: {
    NdVar Target = ARM64.op_count >= 1 ? operandRead(S, ARM64.operands[0])
                                       : NdVar::reg(a64reg::X30, 8);
    if (Target.isReg() && Target.Offset == a64reg::X30)
      S.emit(NdOp::RETURN, {}, {Target});
    else
      S.emit(NdOp::INDIR_BR, {}, {Target});
    break;
  }

  // --- TBNZ / TBZ (test bit and branch) ---
  case AARCH64_INS_TBNZ:
  case AARCH64_INS_TBZ: {
    if (ARM64.op_count < 3)
      break;
    NdVar RegV = operandRead(S, ARM64.operands[0]);
    uint64_t BitPos = static_cast<uint64_t>(ARM64.operands[1].imm);
    va_t TargetAddr = static_cast<uint64_t>(ARM64.operands[2].imm);

    // Extract bit: (reg >> BitPos) & 1
    NdVar Shifted = S.makeTemp(RegV.Size);
    S.emit(NdOp::INT_RIGHT, Shifted, {RegV, NdVar::cst(BitPos, RegV.Size)});
    NdVar BitVal = S.makeTemp(1);
    S.emit(NdOp::INT_AND, BitVal, {Shifted, NdVar::cst(1, RegV.Size)});

    NdVar Cond = S.makeTemp(1);
    if (Insn->id == AARCH64_INS_TBNZ) {
      // Branch if bit is set (non-Zero)
      S.emit(NdOp::INT_NOTEQUAL, Cond, {BitVal, NdVar::cst(0, 1)});
    } else {
      // Branch if bit is clear (Zero)
      S.emit(NdOp::INT_EQUAL, Cond, {BitVal, NdVar::cst(0, 1)});
    }
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(TargetAddr, 8), Cond});
    break;
  }

  // PRFM (prefetch)
  case AARCH64_INS_PRFM:
  case AARCH64_INS_PRFUM: {
    S.emit(NdOp::NOP, {}, {});
    break;
  }

  // --- SVC / HVC / SMC ---
  case AARCH64_INS_SVC: {
    Intrinsic Id = Intrinsic::Svc;
    if (ARM64.op_count >= 1 && ARM64.operands[0].type == AARCH64_OP_IMM)
      S.emitIntrinsic(
          Id, NdVar::reg(a64reg::X0, 8),
          {NdVar::cst(static_cast<uint64_t>(ARM64.operands[0].imm), 2)});
    else
      S.emitIntrinsic(Id);
    break;
  }
  case AARCH64_INS_HVC:
    S.emitIntrinsic(Intrinsic::Hvc);
    break;
  case AARCH64_INS_SMC:
    S.emitIntrinsic(Intrinsic::Smc);
    break;

  // --- BRK (software breakpoint) ---
  case AARCH64_INS_BRK:
    S.emitIntrinsic(Intrinsic::Brk);
    break;

  // --- HLT (debug halt) ---
  case AARCH64_INS_HLT:
    S.emitIntrinsic(Intrinsic::Hlt_A64);
    break;

  // --- Memory barriers ---
  case AARCH64_INS_DMB:
    S.emitVoidIntrinsic(Intrinsic::Dmb);
    break;
  case AARCH64_INS_DSB:
    S.emitVoidIntrinsic(Intrinsic::Dsb);
    break;
  case AARCH64_INS_ISB:
    S.emitVoidIntrinsic(Intrinsic::Isb);
    break;

  // --- System register access ---
  case AARCH64_INS_MRS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    // `mrs Xn, NZCV` packs the condition flags; the compiler uses it to spill a
    // live flag value.  FPSR must remain architectural state so exception and
    // cumulative-saturation flags survive recompilation.
    if (ARM64.operands[1].type == AARCH64_OP_SYSREG &&
        ARM64.operands[1].sysop.reg.sysreg == AARCH64_SYSREG_NZCV) {
      emitMrsNzcv(S, Dst);
      break;
    }
    if (ARM64.operands[1].type == AARCH64_OP_SYSREG &&
        ARM64.operands[1].sysop.reg.sysreg == AARCH64_SYSREG_FPSR) {
      S.emitIntrinsic(Intrinsic::A64_GetFPSR, Dst);
      break;
    }
    if (ARM64.operands[1].type == AARCH64_OP_SYSREG &&
        ARM64.operands[1].sysop.reg.sysreg == AARCH64_SYSREG_FPCR) {
      S.emitIntrinsic(Intrinsic::A64_GetFPCR, Dst);
      break;
    }
    S.emitIntrinsic(Intrinsic::Mrs, Dst);
    break;
  }
  case AARCH64_INS_MSR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (ARM64.operands[0].type == AARCH64_OP_SYSREG &&
        ARM64.operands[0].sysop.reg.sysreg == AARCH64_SYSREG_NZCV) {
      emitMsrNzcv(S, Src);
      break;
    }
    if (ARM64.operands[0].type == AARCH64_OP_SYSREG &&
        ARM64.operands[0].sysop.reg.sysreg == AARCH64_SYSREG_FPSR) {
      S.emitVoidIntrinsic(Intrinsic::A64_SetFPSR, {Src});
      break;
    }
    if (ARM64.operands[0].type == AARCH64_OP_SYSREG &&
        ARM64.operands[0].sysop.reg.sysreg == AARCH64_SYSREG_FPCR) {
      S.emitVoidIntrinsic(Intrinsic::A64_SetFPCR, {Src});
      break;
    }
    S.emitIntrinsic(Intrinsic::Msr, NdVar::reg(a64reg::X0, 8), {Src});
    break;
  }

  // --- Cache / TLB maintenance ---
  case AARCH64_INS_SYS:
    S.emitIntrinsic(Intrinsic::Dc);
    break;

  // YIELD/WFE/WFI/SEV/SEVL → HINT in Capstone 6 (handled by HINT case)

  // --- Exception return ---
  case AARCH64_INS_ERET:
    S.emitIntrinsic(Intrinsic::Eret);
    break;

  // --- CLREX (clear exclusive monitor) ---
  case AARCH64_INS_CLREX:
    S.emitVoidIntrinsic(Intrinsic::A64_Clrex);
    break;

  // --- Pointer authentication (ARMv8.3) ---
  // Capstone 6 consolidates PACIASP/PACIAZ → PACIA, PACIBSP/PACIBZ → PACIB,
  // etc.
  case AARCH64_INS_PACIA:
  case AARCH64_INS_PACIZA:
    S.emitIntrinsic(Intrinsic::Pacia);
    break;
  case AARCH64_INS_PACIB:
  case AARCH64_INS_PACIZB:
    S.emitIntrinsic(Intrinsic::Pacib);
    break;
  case AARCH64_INS_AUTIA:
  case AARCH64_INS_AUTIZA:
    S.emitIntrinsic(Intrinsic::Autia);
    break;
  case AARCH64_INS_AUTIB:
  case AARCH64_INS_AUTIZB:
    S.emitIntrinsic(Intrinsic::Autib);
    break;
  case AARCH64_INS_XPACI:
    S.emitIntrinsic(Intrinsic::Xpaci);
    break;

  default:
    return false;
  }
  return true;
}

void AArch64Lifter::emitMrsNzcv(LiftState &S, NdVar Dst) {
  auto zextFlag = [&](uint64_t Off) {
    NdVar T = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, T, {NdVar::reg(Off, 1)});
    return T;
  };
  NdVar Packed = S.makeTemp(4);
  S.emit(NdOp::INT_LEFT, Packed,
         {zextFlag(a64reg::NFLAG), NdVar::cst(a64reg::NzcvNBit, 4)});
  auto orShifted = [&](NdVar Acc, NdVar F, unsigned Bit) {
    NdVar Sh = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Sh, {F, NdVar::cst(Bit, 4)});
    NdVar Out = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Out, {Acc, Sh});
    return Out;
  };
  Packed = orShifted(Packed, zextFlag(a64reg::ZFLAG), a64reg::NzcvZBit);
  Packed = orShifted(Packed, zextFlag(a64reg::CFLAG), a64reg::NzcvCBit);
  Packed = orShifted(Packed, zextFlag(a64reg::VFLAG), a64reg::NzcvVBit);
  if (Dst.Size > 4)
    S.emit(NdOp::INT_ZEXT, Dst, {Packed});
  else if (Dst.Size == 4)
    S.emit(NdOp::COPY, Dst, {Packed});
  else
    S.emit(NdOp::SUBBYTES, Dst, {Packed, NdVar::cst(0, 4)});
}

void AArch64Lifter::emitMsrNzcv(LiftState &S, NdVar Src) {
  // The flags live in bits 31:28 of the source GPR; narrow/widen to a 32-bit
  // word so the bit slices below are width-correct regardless of the X/W form.
  NdVar Word = Src;
  if (Src.Size > 4) {
    Word = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, Word, {Src, NdVar::cst(0, 4)});
  } else if (Src.Size < 4) {
    Word = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, Word, {Src});
  }
  auto setFlag = [&](unsigned Bit, uint64_t Off) {
    NdVar Sh = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Sh, {Word, NdVar::cst(Bit, 4)});
    NdVar Bit0 = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Bit0, {Sh, NdVar::cst(1, 4)});
    NdVar Lo = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, Lo, {Bit0, NdVar::cst(0, 4)});
    S.emit(NdOp::COPY, NdVar::reg(Off, 1), {Lo});
  };
  setFlag(a64reg::NzcvNBit, a64reg::NFLAG);
  setFlag(a64reg::NzcvZBit, a64reg::ZFLAG);
  setFlag(a64reg::NzcvCBit, a64reg::CFLAG);
  setFlag(a64reg::NzcvVBit, a64reg::VFLAG);
}

} // namespace neverd
