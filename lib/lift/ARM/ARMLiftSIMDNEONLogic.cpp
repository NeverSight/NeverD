//===- ARMLiftSIMDNEONLogic.cpp - ARM32 NEON bitwise logic lifter --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NEON bitwise data processing: VAND, VORR, VEOR, VORN, VBIC and
/// the bit-select family VBSL, VBIT and VBIF.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDNEONLogic(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                       const cs_arm &ARM) {
  switch (Insn->id) {
  // NEON data processing — binary ops
  case ARM_INS_VAND:
  case ARM_INS_VORR:
  case ARM_INS_VEOR: {
    // Immediate form: `vorr.iN dD, #imm` (dD |= imm) and `vand.iN dD, #imm`
    // (dD &= imm) carry only two operands (dst + immediate).  The old code
    // required 3 operands and silently dropped them, so e.g. clang's
    // `(x & 0x7e) | 1` lost its `| 1` — the divisor became even/zero and the
    // recompiled float divide produced inf/NaN (VectorAlgo8 arm32 fdiv).  Like
    // VBIC's immediate form (and AArch64 #220), broadcast the per-lane
    // immediate and OR/AND it into every lane.  VEOR has no immediate form.
    if ((Insn->id == ARM_INS_VORR || Insn->id == ARM_INS_VAND) &&
        ARM.op_count == 2 && ARM.operands[1].type == ARM_OP_IMM) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar DstIn = L.operandRead(S, ARM.operands[0]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz ? LI.LaneSz : 4;
      uint64_t LaneMask = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
      uint64_t Imm = static_cast<uint64_t>(ARM.operands[1].imm) & LaneMask;
      NdOp ImmOpc = (Insn->id == ARM_INS_VORR) ? NdOp::INT_OR : NdOp::INT_AND;
      if (DstIn.Size > LaneSz) {
        unsigned NLanes = DstIn.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La,
                 {DstIn, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
          NdVar R = S.makeTemp(LaneSz);
          S.emit(ImmOpc, R, {La, NdVar::cst(Imm, LaneSz)});
          if (I == 0)
            Acc = R;
          else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {R, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(ImmOpc, Dst, {DstIn, NdVar::cst(Imm, DstIn.Size)});
      }
      break;
    }
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdOp Opc = NdOp::INT_AND;
    if (Insn->id == ARM_INS_VORR)
      Opc = NdOp::INT_OR;
    else if (Insn->id == ARM_INS_VEOR)
      Opc = NdOp::INT_XOR;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case ARM_INS_VORN: {
    // VORN: Dst = a | ~b
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_OR, Dst, {A, NB});
    break;
  }
  case ARM_INS_VBIC: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    // Immediate form: `vbic.iN dD, #imm`  =>  dD &= ~imm (imm broadcast/lane).
    // The previous code required 3 operands and silently dropped this form, so
    // the masking it performs (e.g. clearing the high byte of each 16-bit lane
    // when widening bytes) was a no-op and the value was left unchanged.
    if (ARM.op_count == 2 && ARM.operands[1].type == ARM_OP_IMM) {
      NdVar DstIn = L.operandRead(S, ARM.operands[0]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz ? LI.LaneSz : 2;
      uint64_t LaneMask = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
      uint64_t NotImm =
          (~static_cast<uint64_t>(ARM.operands[1].imm)) & LaneMask;
      if (DstIn.Size > LaneSz) {
        unsigned NLanes = DstIn.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {DstIn, NdVar::cst(I * LaneSz, 4)});
          NdVar R = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_AND, R, {La, NdVar::cst(NotImm, LaneSz)});
          if (I == 0)
            Acc = R;
          else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {R, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::INT_AND, Dst, {DstIn, NdVar::cst(NotImm, DstIn.Size)});
      }
      break;
    }
    // Register form: Dst = a & ~b
    if (ARM.op_count < 3)
      break;
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    S.emit(NdOp::INT_AND, Dst, {A, NB});
    break;
  }
  case ARM_INS_VBSL: {
    // VBSL: Dst = (op1 & dst_old) | (op2 & ~dst_old)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar Mask = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, Mask});
    NdVar NMask = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NMask, {Mask});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {B, NMask});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case ARM_INS_VBIT: {
    // VBIT: Dst = (op1 & op2) | (dst_old & ~op2)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {NdVar::reg(Dst.Offset, Dst.Size), NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case ARM_INS_VBIF: {
    // VBIF: Dst = (dst_old & op2) | (op1 & ~op2)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {NdVar::reg(Dst.Offset, Dst.Size), B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {A, NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
