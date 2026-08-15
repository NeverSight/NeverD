//===- AArch64LiftSME.cpp - SME tile ops and FEAT_MOPS copy/set -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Scalable Matrix Extension fused multiply-add, outer products
/// (SMOPA/FMOPA/BMOPA), tile move/zero and lookup tables, plus
/// the FEAT_MOPS CPY*/SET* memory copy and set families.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSME(AArch64Lifter &L, AArch64Lifter::LiftState &S, const cs_insn *Insn,
             const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // SME: FMA/FMS/MAC
  case AARCH64_INS_FMA16:
  case AARCH64_INS_FMA32:
  case AARCH64_INS_FMA64:
  case AARCH64_INS_FMS16:
  case AARCH64_INS_FMS32:
  case AARCH64_INS_FMS64:
  case AARCH64_INS_MAC16:
  case AARCH64_INS_MATFP:
  case AARCH64_INS_MATINT: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[1]);
      NdVar B = L.operandRead(S, ARM64.operands[2]);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    }
    break;
  }
  case AARCH64_INS_MUL53HI:
  case AARCH64_INS_MUL53LO: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }

  // SME outer products
  case AARCH64_INS_SMOPA:
  case AARCH64_INS_SMOPS:
  case AARCH64_INS_UMOPA:
  case AARCH64_INS_UMOPS:
  case AARCH64_INS_SUMOPA:
  case AARCH64_INS_SUMOPS:
  case AARCH64_INS_USMOPA:
  case AARCH64_INS_USMOPS:
  case AARCH64_INS_FMOPA:
  case AARCH64_INS_FMOPS:
  case AARCH64_INS_BMOPA:
  case AARCH64_INS_BMOPS: {
    if (ARM64.op_count >= 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar A = L.operandRead(S, ARM64.operands[1]);
      NdVar B = L.operandRead(S, ARM64.operands[2]);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    }
    break;
  }

  // SME tile ops
  case AARCH64_INS_MOVA:
  case AARCH64_INS_MOVAZ:
  case AARCH64_INS_PMOV: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_ZERO:
  case AARCH64_INS_CLR:
  case AARCH64_INS_VECFP:
  case AARCH64_INS_VECINT:
    S.emit(NdOp::NOP, {}, {});
    break;
  case AARCH64_INS_LUTI2:
  case AARCH64_INS_LUTI4:
  case AARCH64_INS_GENLUT: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // ========================================================================
  // Memory Copy / Set (ARMv8.8 FEAT_MOPS) — ~96+32 variants
  // All CPY* are memory copy, all SET* are memory set.
  // ========================================================================
  case AARCH64_INS_SETP:
  case AARCH64_INS_SETPN:
  case AARCH64_INS_SETPT:
  case AARCH64_INS_SETPTN: {
    // SETP is the prologue of a FEAT_MOPS memory-set sequence.  Its progress
    // amount is implementation-defined, so a scalar STORE cannot represent
    // it.  Capstone exposes the printed operands here: [Rd]!, Rn!, Rm.  Keep
    // the address value (rather than loading through [Rd]), the full count and
    // the fill register, then collect both writebacks plus NZCV from the real
    // architectural instruction.
    if (ARM64.op_count < 3 || ARM64.operands[0].type != AARCH64_OP_MEM ||
        ARM64.operands[0].mem.base == AARCH64_REG_INVALID ||
        ARM64.operands[1].type != AARCH64_OP_REG)
      break;

    Intrinsic Id = Intrinsic::A64_MopsSetP;
    switch (Insn->id) {
    case AARCH64_INS_SETPN:
      Id = Intrinsic::A64_MopsSetPN;
      break;
    case AARCH64_INS_SETPT:
      Id = Intrinsic::A64_MopsSetPT;
      break;
    case AARCH64_INS_SETPTN:
      Id = Intrinsic::A64_MopsSetPTN;
      break;
    default:
      break;
    }

    auto DstRI =
        mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[0].mem.base));
    auto CountRI =
        mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[1].reg));
    if (DstRI.Size == 0 || CountRI.Size == 0)
      break;

    NdVar DstReg = NdVar::reg(DstRI.Offset, 8);
    NdVar CountReg = NdVar::reg(CountRI.Offset, 8);
    NdVar DstIn = L.operandEffAddr(S, ARM64.operands[0]);
    NdVar CountIn = L.operandRead(S, ARM64.operands[1]);
    NdVar FillIn = L.operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Id, DstReg, {DstIn, CountIn, FillIn});

    // Multi-output intrinsics bind their returned values through the COPY
    // inputs immediately following the intrinsic (the same convention used
    // by CPUID/RDTSC).  Keep all three together before unpacking NZCV.
    NdVar DstResult = S.makeTemp(8);
    S.emit(NdOp::COPY, DstReg, {DstResult});
    NdVar CountResult = S.makeTemp(8);
    S.emit(NdOp::COPY, CountReg, {CountResult});
    NdVar NzcvResult = S.makeTemp(8);
    NdVar PackedNzcv = S.makeTemp(8);
    S.emit(NdOp::COPY, PackedNzcv, {NzcvResult});
    AArch64Lifter::emitMsrNzcv(S, PackedNzcv);
    break;
  }

  case AARCH64_INS_CPYE:
  case AARCH64_INS_CPYEN:
  case AARCH64_INS_CPYERN:
  case AARCH64_INS_CPYERT:
  case AARCH64_INS_CPYERTN:
  case AARCH64_INS_CPYERTRN:
  case AARCH64_INS_CPYERTWN:
  case AARCH64_INS_CPYET:
  case AARCH64_INS_CPYETN:
  case AARCH64_INS_CPYETRN:
  case AARCH64_INS_CPYETWN:
  case AARCH64_INS_CPYEWN:
  case AARCH64_INS_CPYEWT:
  case AARCH64_INS_CPYEWTN:
  case AARCH64_INS_CPYEWTRN:
  case AARCH64_INS_CPYEWTWN:
  case AARCH64_INS_CPYFE:
  case AARCH64_INS_CPYFEN:
  case AARCH64_INS_CPYFERN:
  case AARCH64_INS_CPYFERT:
  case AARCH64_INS_CPYFERTN:
  case AARCH64_INS_CPYFERTRN:
  case AARCH64_INS_CPYFERTWN:
  case AARCH64_INS_CPYFET:
  case AARCH64_INS_CPYFETN:
  case AARCH64_INS_CPYFETRN:
  case AARCH64_INS_CPYFETWN:
  case AARCH64_INS_CPYFEWN:
  case AARCH64_INS_CPYFEWT:
  case AARCH64_INS_CPYFEWTN:
  case AARCH64_INS_CPYFEWTRN:
  case AARCH64_INS_CPYFEWTWN:
  case AARCH64_INS_CPYFM:
  case AARCH64_INS_CPYFMN:
  case AARCH64_INS_CPYFMRN:
  case AARCH64_INS_CPYFMRT:
  case AARCH64_INS_CPYFMRTN:
  case AARCH64_INS_CPYFMRTRN:
  case AARCH64_INS_CPYFMRTWN:
  case AARCH64_INS_CPYFMT:
  case AARCH64_INS_CPYFMTN:
  case AARCH64_INS_CPYFMTRN:
  case AARCH64_INS_CPYFMTWN:
  case AARCH64_INS_CPYFMWN:
  case AARCH64_INS_CPYFMWT:
  case AARCH64_INS_CPYFMWTN:
  case AARCH64_INS_CPYFMWTRN:
  case AARCH64_INS_CPYFMWTWN:
  case AARCH64_INS_CPYFP:
  case AARCH64_INS_CPYFPN:
  case AARCH64_INS_CPYFPRN:
  case AARCH64_INS_CPYFPRT:
  case AARCH64_INS_CPYFPRTN:
  case AARCH64_INS_CPYFPRTRN:
  case AARCH64_INS_CPYFPRTWN:
  case AARCH64_INS_CPYFPT:
  case AARCH64_INS_CPYFPTN:
  case AARCH64_INS_CPYFPTRN:
  case AARCH64_INS_CPYFPTWN:
  case AARCH64_INS_CPYFPWN:
  case AARCH64_INS_CPYFPWT:
  case AARCH64_INS_CPYFPWTN:
  case AARCH64_INS_CPYFPWTRN:
  case AARCH64_INS_CPYFPWTWN:
  case AARCH64_INS_CPYM:
  case AARCH64_INS_CPYMN:
  case AARCH64_INS_CPYMRN:
  case AARCH64_INS_CPYMRT:
  case AARCH64_INS_CPYMRTN:
  case AARCH64_INS_CPYMRTRN:
  case AARCH64_INS_CPYMRTWN:
  case AARCH64_INS_CPYMT:
  case AARCH64_INS_CPYMTN:
  case AARCH64_INS_CPYMTRN:
  case AARCH64_INS_CPYMTWN:
  case AARCH64_INS_CPYMWN:
  case AARCH64_INS_CPYMWT:
  case AARCH64_INS_CPYMWTN:
  case AARCH64_INS_CPYMWTRN:
  case AARCH64_INS_CPYMWTWN:
  case AARCH64_INS_CPYP:
  case AARCH64_INS_CPYPN:
  case AARCH64_INS_CPYPRN:
  case AARCH64_INS_CPYPRT:
  case AARCH64_INS_CPYPRTN:
  case AARCH64_INS_CPYPRTRN:
  case AARCH64_INS_CPYPRTWN:
  case AARCH64_INS_CPYPT:
  case AARCH64_INS_CPYPTN:
  case AARCH64_INS_CPYPTRN:
  case AARCH64_INS_CPYPTWN:
  case AARCH64_INS_CPYPWN:
  case AARCH64_INS_CPYPWT:
  case AARCH64_INS_CPYPWTN:
  case AARCH64_INS_CPYPWTRN:
  case AARCH64_INS_CPYPWTWN:
  case AARCH64_INS_SET:
  case AARCH64_INS_SETE:
  case AARCH64_INS_SETEN:
  case AARCH64_INS_SETET:
  case AARCH64_INS_SETETN:
  case AARCH64_INS_SETGE:
  case AARCH64_INS_SETGEN:
  case AARCH64_INS_SETGET:
  case AARCH64_INS_SETGETN:
  case AARCH64_INS_SETGM:
  case AARCH64_INS_SETGMN:
  case AARCH64_INS_SETGMT:
  case AARCH64_INS_SETGMTN:
  case AARCH64_INS_SETGP:
  case AARCH64_INS_SETGPN:
  case AARCH64_INS_SETGPT:
  case AARCH64_INS_SETGPTN:
  case AARCH64_INS_SETM:
  case AARCH64_INS_SETMN:
  case AARCH64_INS_SETMT:
  case AARCH64_INS_SETMTN: {
    if (ARM64.op_count >= 3) {
      NdVar DstAddr = L.operandRead(S, ARM64.operands[0]);
      NdVar SrcVal = L.operandRead(S, ARM64.operands[1]);
      NdVar Val = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Val, {SrcVal, NdVar::cst(0, 4)});
      S.emit(NdOp::STORE, {}, {DstAddr, Val});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
