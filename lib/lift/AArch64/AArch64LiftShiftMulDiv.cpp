//===- AArch64LiftShiftMulDiv.cpp - Shift, multiply/divide and address ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The LSL/LSR/ASR shifts (including the Capstone LSLV/LSRV/ASRV
/// aliases decoded straight from the instruction word),
/// MUL/SDIV/UDIV, the PC-relative ADR/ADRP and SXTW.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/decode/AArch64NativeDecode.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftShiftMulDiv(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- LSL / LSR / ASR ---
  // Capstone 6 alias: LSLV/LSRV/ASRV Xd,Xn,Xm may be reported as
  // LSL/LSR/ASR with only 2 operands (Rd,Rn) — Rm is dropped.
  // Extract Rn (bits [9:5]) and Rm (bits [20:16]) from the encoding.
  case AARCH64_INS_LSL: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A, B;
    bool IsRegShift = false;
    if (ARM64.op_count >= 3) {
      A = L.operandRead(S, ARM64.operands[1]);
      B = L.operandRead(S, ARM64.operands[2]);
      IsRegShift = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = a64native::gpr(RnIdx, Is64);
      auto RmReg = a64native::gpr(RmIdx, Is64);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      A = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                          : L.operandRead(S, ARM64.operands[0]);
      B = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                          : NdVar::cst(0, Dst.Size);
      IsRegShift = true;
    } else {
      A = L.operandRead(S, ARM64.operands[0]);
      B = L.operandRead(S, ARM64.operands[1]);
    }
    if (IsRegShift) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      NdVar Masked = S.makeTemp(B.Size);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(Mask, B.Size)});
      S.emit(NdOp::INT_LEFT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_LSR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A, B;
    bool IsRegShift = false;
    if (ARM64.op_count >= 3) {
      A = L.operandRead(S, ARM64.operands[1]);
      B = L.operandRead(S, ARM64.operands[2]);
      IsRegShift = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = a64native::gpr(RnIdx, Is64);
      auto RmReg = a64native::gpr(RmIdx, Is64);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      A = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                          : L.operandRead(S, ARM64.operands[0]);
      B = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                          : NdVar::cst(0, Dst.Size);
      IsRegShift = true;
    } else {
      A = L.operandRead(S, ARM64.operands[0]);
      B = L.operandRead(S, ARM64.operands[1]);
    }
    if (IsRegShift) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      NdVar Masked = S.makeTemp(B.Size);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(Mask, B.Size)});
      S.emit(NdOp::INT_RIGHT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_RIGHT, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_ASR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A, B;
    bool IsRegShift = false;
    if (ARM64.op_count >= 3) {
      A = L.operandRead(S, ARM64.operands[1]);
      B = L.operandRead(S, ARM64.operands[2]);
      IsRegShift = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = a64native::gpr(RnIdx, Is64);
      auto RmReg = a64native::gpr(RmIdx, Is64);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      A = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                          : L.operandRead(S, ARM64.operands[0]);
      B = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                          : NdVar::cst(0, Dst.Size);
      IsRegShift = true;
    } else {
      A = L.operandRead(S, ARM64.operands[0]);
      B = L.operandRead(S, ARM64.operands[1]);
    }
    if (IsRegShift) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      NdVar Masked = S.makeTemp(B.Size);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(Mask, B.Size)});
      S.emit(NdOp::INT_ASHR, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_ASHR, Dst, {A, B});
    }
    break;
  }

  // --- MUL / SDIV / UDIV ---
  case AARCH64_INS_MUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    // By-element form `mul v.T, v.T, vN.<ty>[idx]` broadcasts a single source
    // lane to every destination lane.  capstone leaves vas non-element so
    // operandRead returns the whole register (high lanes unset) — reading B
    // per-lane would multiply most lanes by 0.
    int BLane = ARM64.operands[2].vector_index;
    {
      auto Vas = ARM64.operands[0].vas;
      unsigned LaneSz = 0;
      if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
        LaneSz = 4;
      else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
        LaneSz = 2;
      else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
        LaneSz = 1;
      else if (Vas == AARCH64LAYOUT_VL_2D)
        LaneSz = 8;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        NdVar BElem;
        if (BLane >= 0) {
          NdVar BFull = L.operandWrite(ARM64.operands[2]);
          BElem = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, BElem,
                 {BFull, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
        }
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
          NdVar Lb;
          if (BLane >= 0) {
            Lb = BElem;
          } else {
            Lb = S.makeTemp(LaneSz);
            S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
          }
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {La, Lb});
          if (I == 0) {
            Acc = Lr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Lr, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::INT_MULT, Dst, {A, B});
      }
    }
    break;
  }
  case AARCH64_INS_SDIV: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SDIV, Dst, {A, B});
    break;
  }
  case AARCH64_INS_UDIV: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_DIV, Dst, {A, B});
    break;
  }

  // --- ADRP / ADR ---
  case AARCH64_INS_ADRP:
  case AARCH64_INS_ADR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // --- SXTW / UXTW / SXTH / UXTH ---
  case AARCH64_INS_SXTW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_SEXT, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
