//===- AArch64LiftSVELdSt.cpp - SVE load and store ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Predicated contiguous, replicating, first-faulting and
/// non-temporal loads (LD1B../LD1R../LDFF1../LDNT1..), the
/// matching stores (ST1B../STNT1..) and the SVE prefetches.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftSVELdSt(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                 const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ========================================================================
  // SVE load/store
  // ========================================================================
  case AARCH64_INS_LD1B:
  case AARCH64_INS_LD1D:
  case AARCH64_INS_LD1H:
  case AARCH64_INS_LD1Q:
  case AARCH64_INS_LD1W:
  case AARCH64_INS_LD1SB:
  case AARCH64_INS_LD1SH:
  case AARCH64_INS_LD1SW:
  case AARCH64_INS_LD1RB:
  case AARCH64_INS_LD1RD:
  case AARCH64_INS_LD1RH:
  case AARCH64_INS_LD1RW:
  case AARCH64_INS_LD1RSB:
  case AARCH64_INS_LD1RSH:
  case AARCH64_INS_LD1RSW:
  case AARCH64_INS_LD1ROB:
  case AARCH64_INS_LD1ROD:
  case AARCH64_INS_LD1ROH:
  case AARCH64_INS_LD1ROW:
  case AARCH64_INS_LD1RQB:
  case AARCH64_INS_LD1RQD:
  case AARCH64_INS_LD1RQH:
  case AARCH64_INS_LD1RQW:
  case AARCH64_INS_LD2B:
  case AARCH64_INS_LD2D:
  case AARCH64_INS_LD2H:
  case AARCH64_INS_LD2Q:
  case AARCH64_INS_LD2W:
  case AARCH64_INS_LD3B:
  case AARCH64_INS_LD3D:
  case AARCH64_INS_LD3H:
  case AARCH64_INS_LD3Q:
  case AARCH64_INS_LD3W:
  case AARCH64_INS_LD4B:
  case AARCH64_INS_LD4D:
  case AARCH64_INS_LD4H:
  case AARCH64_INS_LD4Q:
  case AARCH64_INS_LD4W:
  case AARCH64_INS_LDFF1B:
  case AARCH64_INS_LDFF1D:
  case AARCH64_INS_LDFF1H:
  case AARCH64_INS_LDFF1SB:
  case AARCH64_INS_LDFF1SH:
  case AARCH64_INS_LDFF1SW:
  case AARCH64_INS_LDFF1W:
  case AARCH64_INS_LDNF1B:
  case AARCH64_INS_LDNF1D:
  case AARCH64_INS_LDNF1H:
  case AARCH64_INS_LDNF1SB:
  case AARCH64_INS_LDNF1SH:
  case AARCH64_INS_LDNF1SW:
  case AARCH64_INS_LDNF1W:
  case AARCH64_INS_LDNT1B:
  case AARCH64_INS_LDNT1D:
  case AARCH64_INS_LDNT1H:
  case AARCH64_INS_LDNT1SB:
  case AARCH64_INS_LDNT1SH:
  case AARCH64_INS_LDNT1SW:
  case AARCH64_INS_LDNT1W: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar EA = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case AARCH64_INS_LDX:
  case AARCH64_INS_LDY:
  case AARCH64_INS_LDZ:
  case AARCH64_INS_LDZI: {
    if (ARM64.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar EA = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::LOAD, Dst, {EA});
    }
    break;
  }
  case AARCH64_INS_ST1B:
  case AARCH64_INS_ST1D:
  case AARCH64_INS_ST1H:
  case AARCH64_INS_ST1Q:
  case AARCH64_INS_ST1W:
  case AARCH64_INS_ST2B:
  case AARCH64_INS_ST2D:
  case AARCH64_INS_ST2H:
  case AARCH64_INS_ST2Q:
  case AARCH64_INS_ST2W:
  case AARCH64_INS_ST3B:
  case AARCH64_INS_ST3D:
  case AARCH64_INS_ST3H:
  case AARCH64_INS_ST3Q:
  case AARCH64_INS_ST3W:
  case AARCH64_INS_ST4B:
  case AARCH64_INS_ST4D:
  case AARCH64_INS_ST4H:
  case AARCH64_INS_ST4Q:
  case AARCH64_INS_ST4W:
  case AARCH64_INS_STNT1B:
  case AARCH64_INS_STNT1D:
  case AARCH64_INS_STNT1H:
  case AARCH64_INS_STNT1W: {
    if (ARM64.op_count >= 2) {
      NdVar Src = L.operandRead(S, ARM64.operands[0]);
      NdVar EA = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }
  case AARCH64_INS_STX:
  case AARCH64_INS_STY:
  case AARCH64_INS_STZ:
  case AARCH64_INS_STZI: {
    if (ARM64.op_count >= 2) {
      NdVar Src = L.operandRead(S, ARM64.operands[0]);
      NdVar EA = L.operandRead(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::STORE, {}, {EA, Src});
    }
    break;
  }
  case AARCH64_INS_PRFB:
  case AARCH64_INS_PRFD:
  case AARCH64_INS_PRFH:
  case AARCH64_INS_PRFW:
  case AARCH64_INS_RPRFM:
    S.emit(NdOp::NOP, {}, {});
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
