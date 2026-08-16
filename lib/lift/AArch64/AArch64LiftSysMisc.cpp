//===- AArch64LiftSysMisc.cpp - Memory tagging and system ops -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory-tagging load/store (STG/STZG/LDG/IRG/GMI/SUBP), the BC
/// branch, the misc integer ops UXTW/CTZ/AXFLAG/UDF, and the
/// system/debug/privileged group including TME, GCS and the
/// guarded-page GENTER/GEXIT.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSysMisc(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // Memory tagging (MTE, ARMv8.5)
  // ========================================================================
  case AARCH64_INS_STG: {
    if (ARM64.op_count < 2)
      break;
    NdVar TagSource = L.operandRead(S, ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::Stg, {}, {TagSource, EA});
    break;
  }
  case AARCH64_INS_STZG:
    S.emitIntrinsic(Intrinsic::Stzg);
    break;
  case AARCH64_INS_ST2G:
    S.emitIntrinsic(Intrinsic::St2g);
    break;
  case AARCH64_INS_STZ2G:
    S.emitIntrinsic(Intrinsic::Stz2g);
    break;
  case AARCH64_INS_STGM:
  case AARCH64_INS_STZGM: {
    S.emitIntrinsic(Intrinsic::Stg);
    break;
  }
  case AARCH64_INS_STGP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Src1 = L.operandRead(S, ARM64.operands[0]);
    NdVar Src2 = L.operandRead(S, ARM64.operands[1]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[2]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Src1.Size, 8)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    break;
  }
  case AARCH64_INS_LDG: {
    if (ARM64.op_count < 2)
      break;
    NdVar ResultAddress = L.operandRead(S, ARM64.operands[0]);
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::Ldg, Dst, {ResultAddress, EA});
    break;
  }
  case AARCH64_INS_LDGM: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar EA = L.operandEffAddr(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {EA});
    break;
  }
  case AARCH64_INS_IRG:
  case AARCH64_INS_GMI: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case AARCH64_INS_SUBP:
  case AARCH64_INS_SUBPS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SUB, Dst, {A, B});
    if (Insn->id == AARCH64_INS_SUBPS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
    }
    break;
  }

  // ========================================================================
  // Branch: BC (conditional branch with hint)
  // ========================================================================
  case AARCH64_INS_BC: {
    if (ARM64.op_count < 1)
      break;
    NdVar Cond = L.buildCondCode(ARM64.cc, S);
    NdVar Target = NdVar::cst(static_cast<uint64_t>(ARM64.operands[0].imm), 8);
    S.emit(NdOp::COND_BR, {}, {Target, Cond});
    break;
  }

  // ========================================================================
  // Misc integer: UXTW, CTZ, AXFLAG, UDF
  // ========================================================================
  case AARCH64_INS_UXTW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_ZEXT, Dst, {Src});
    break;
  }
  case AARCH64_INS_CTZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // CTZ counts TRAILING zeros; the old code emitted LZCOUNT (leading).
    // ctz(x) = popcount(~x & (x-1))  (mirrors the x86 TZCNT lowering).
    NdVar NotX = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_NOT, NotX, {Src});
    NdVar XM1 = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_SUB, XM1, {Src, NdVar::cst(1, Src.Size)});
    NdVar Iso = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_AND, Iso, {NotX, XM1});
    S.emit(NdOp::POPCOUNT, Dst, {Iso});
    break;
  }
  case AARCH64_INS_AXFLAG: {
    // FEAT_FlagM2: convert NZCV from the Arm encoding to the "alternative"
    // (JavaScript) FP-compare encoding.  Depends on Z, V, C (per the ARM ARM /
    // QEMU `gen_axflag`):
    //   N = 0;  Z = Z OR V;  C = C AND NOT V;  V = 0.
    // Was a bare opaque `A64_Axflag` intrinsic that left NeverD's modelled
    // flags untouched, so the conversion was lost.
    NdVar OldZ = S.makeTemp(1);
    S.emit(NdOp::COPY, OldZ, {NdVar::reg(a64reg::ZFLAG, 1)});
    NdVar OldV = S.makeTemp(1);
    S.emit(NdOp::COPY, OldV, {NdVar::reg(a64reg::VFLAG, 1)});
    NdVar OldC = S.makeTemp(1);
    S.emit(NdOp::COPY, OldC, {NdVar::reg(a64reg::CFLAG, 1)});
    NdVar NotV = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotV, {OldV});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::NFLAG, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::ZFLAG, 1), {OldZ, OldV});
    S.emit(NdOp::BOOL_AND, NdVar::reg(a64reg::CFLAG, 1), {OldC, NotV});
    S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
    break;
  }
  case AARCH64_INS_UDF:
    S.emitIntrinsic(Intrinsic::Brk);
    break;

  // ========================================================================
  // System / debug / privileged
  // ========================================================================
  case AARCH64_INS_DCPS1:
  case AARCH64_INS_DCPS2:
  case AARCH64_INS_DCPS3:
  case AARCH64_INS_DRPS:
  case AARCH64_INS_SB:
  case AARCH64_INS_SDSB:
  case AARCH64_INS_TSB:
  case AARCH64_INS_TRCIT:
  case AARCH64_INS_BRB:
    S.emitIntrinsic(Intrinsic::A64_Brb);
    break;
  case AARCH64_INS_WFET:
    S.emitIntrinsic(Intrinsic::A64_Wfet);
    break;
  case AARCH64_INS_WFIT:
    S.emitIntrinsic(Intrinsic::A64_Wfit);
    break;
  case AARCH64_INS_SYSL: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Intrinsic::Dc, Dst);
    } else {
      S.emitIntrinsic(Intrinsic::Dc);
    }
    break;
  }
  case AARCH64_INS_SYSP:
    S.emitIntrinsic(Intrinsic::Dc);
    break;
  case AARCH64_INS_AT_AS1ELX:
    S.emitIntrinsic(Intrinsic::At);
    break;
  case AARCH64_INS_MRRS: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emitIntrinsic(Intrinsic::Mrs, Dst);
    }
    break;
  }
  case AARCH64_INS_MSRR: {
    if (ARM64.op_count >= 1) {
      NdVar Src = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emitIntrinsic(Intrinsic::Msr, NdVar::reg(a64reg::X0, 8), {Src});
    }
    break;
  }

  // TME (Transactional Memory Extension)
  case AARCH64_INS_TSTART: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_TTEST: {
    if (ARM64.op_count >= 1) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      S.emit(NdOp::COPY, Dst, {NdVar::cst(0, Dst.Size)});
    }
    break;
  }
  case AARCH64_INS_TCOMMIT:
    S.emitIntrinsic(Intrinsic::A64_Tcommit);
    break;
  case AARCH64_INS_TCANCEL:
    S.emitIntrinsic(Intrinsic::A64_Tcancel);
    break;

  // GCS (Guarded Control Stack)
  case AARCH64_INS_GCSPOPCX:
  case AARCH64_INS_GCSPOPM:
  case AARCH64_INS_GCSPOPX:
  case AARCH64_INS_GCSPUSHM:
  case AARCH64_INS_GCSPUSHX:
  case AARCH64_INS_GCSSS1:
  case AARCH64_INS_GCSSS2:
    S.emitIntrinsic(Intrinsic::A64_Gcsstr);
    break;
  case AARCH64_INS_GCSSTR:
    S.emitIntrinsic(Intrinsic::A64_Gcsstr);
    break;
  case AARCH64_INS_GCSSTTR:
    S.emitIntrinsic(Intrinsic::A64_Gcssttr);
    break;

  // GENTER / GEXIT (Guarded pages)
  case AARCH64_INS_GENTER:
    S.emitIntrinsic(Intrinsic::A64_Genter);
    break;
  case AARCH64_INS_GEXIT:
    S.emitIntrinsic(Intrinsic::A64_Gexit);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
