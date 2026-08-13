//===- AArch64LiftNEONMove.cpp - NEON move, duplicate and lane transfer ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MOVI/MVNI vector immediates, DUP/INS lane duplication and
/// insertion, and UMOV/SMOV lane-to-GPR extraction.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

// Effective immediate for a MOVI/MVNI/ORR/BIC vector-immediate operand,
// applying the LSL (zero-fill) or MSL (one-fill, "modified shift left") shift.
// capstone reports the raw immediate and the shift separately; ignoring the
// shift produced wrong constants (e.g. `movi v.4s,#0x1f,msl #8` should be
// 0x1FFF = (0x1f<<8)|0xFF, not 0x1f).
static uint64_t moviImmWithShift(const cs_aarch64_op &Op) {
  uint64_t Imm = static_cast<uint64_t>(Op.imm);
  if (Op.shift.value > 0) {
    unsigned N = Op.shift.value;
    if (Op.shift.type == AARCH64_SFT_LSL)
      Imm <<= N;
    else if (Op.shift.type == AARCH64_SFT_MSL)
      Imm = (Imm << N) | ((1ULL << N) - 1);
  }
  return Imm;
}

bool liftNEONMove(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // ====================================================================
  // NEON / SIMD — bulk dataflow, same approach as x86 SSE/AVX
  // (preserves shape for decompilation; per-lane semantics would need
  // dedicated VEC_* ops — see intrinsics for per-lane NEON ops)
  // ====================================================================
  case AARCH64_INS_MOVI: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    if (ARM64.operands[1].type == AARCH64_OP_IMM) {
      uint64_t Imm = moviImmWithShift(ARM64.operands[1]);
      Src = NdVar::cst(Imm, (Imm <= 0xFFFFFFFFULL) ? 4 : 8);
    }
    uint16_t ElemSz = 0;
    auto VAS = ARM64.operands[0].vas;
    if (VAS == AARCH64LAYOUT_VL_16B || VAS == AARCH64LAYOUT_VL_8B)
      ElemSz = 1;
    else if (VAS == AARCH64LAYOUT_VL_8H || VAS == AARCH64LAYOUT_VL_4H)
      ElemSz = 2;
    else if (VAS == AARCH64LAYOUT_VL_4S || VAS == AARCH64LAYOUT_VL_2S)
      ElemSz = 4;
    else if (VAS == AARCH64LAYOUT_VL_2D)
      ElemSz = 8;
    if (ElemSz > 0 && Dst.Size > ElemSz) {
      unsigned NLanes = Dst.Size / ElemSz;
      NdVar Elem = Src;
      if (Elem.Size > ElemSz) {
        NdVar Trunc = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Trunc, {Elem, NdVar::cst(0, 4)});
        Elem = Trunc;
      } else if (Elem.Size < ElemSz) {
        NdVar Ext = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, Ext, {Elem});
        Elem = Ext;
      }
      NdVar Acc = Elem;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Elem, Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_MVNI: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    if (ARM64.operands[1].type == AARCH64_OP_IMM) {
      uint64_t Imm = moviImmWithShift(ARM64.operands[1]);
      Src = NdVar::cst(Imm, (Imm <= 0xFFFFFFFFULL) ? 4 : 8);
    }
    uint16_t ElemSz = 0;
    auto VAS = ARM64.operands[0].vas;
    if (VAS == AARCH64LAYOUT_VL_16B || VAS == AARCH64LAYOUT_VL_8B)
      ElemSz = 1;
    else if (VAS == AARCH64LAYOUT_VL_8H || VAS == AARCH64LAYOUT_VL_4H)
      ElemSz = 2;
    else if (VAS == AARCH64LAYOUT_VL_4S || VAS == AARCH64LAYOUT_VL_2S)
      ElemSz = 4;
    else if (VAS == AARCH64LAYOUT_VL_2D)
      ElemSz = 8;
    if (ElemSz > 0 && Dst.Size > ElemSz) {
      unsigned NLanes = Dst.Size / ElemSz;
      NdVar Elem = Src;
      if (Elem.Size > ElemSz) {
        NdVar Trunc = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Trunc, {Elem, NdVar::cst(0, 4)});
        Elem = Trunc;
      } else if (Elem.Size < ElemSz) {
        NdVar Ext = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, Ext, {Elem});
        Elem = Ext;
      }
      NdVar Acc = Elem;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Elem, Acc});
        Acc = Next;
      }
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::COPY, Full, {Acc});
      S.emit(NdOp::INT_NOT, Dst, {Full});
    } else {
      S.emit(NdOp::INT_NOT, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_DUP:
  case AARCH64_INS_INS: {
    if (ARM64.op_count < 2)
      break;
    const auto &DstOp = ARM64.operands[0];
    NdVar Src = L.operandRead(S, ARM64.operands[1]);

    if (Insn->id == AARCH64_INS_INS && DstOp.vector_index >= 0 &&
        DstOp.type == AARCH64_OP_REG) {
      unsigned ElemSz = 0;
      switch (DstOp.vas) {
      case AARCH64LAYOUT_VL_B:
        ElemSz = 1;
        break;
      case AARCH64LAYOUT_VL_H:
        ElemSz = 2;
        break;
      case AARCH64LAYOUT_VL_S:
        ElemSz = 4;
        break;
      case AARCH64LAYOUT_VL_D:
        ElemSz = 8;
        break;
      default:
        break;
      }
      auto RI = mapCapstoneReg(static_cast<aarch64_reg>(DstOp.reg));
      if (ElemSz > 0 && RI.Size > ElemSz) {
        unsigned ByteOff = DstOp.vector_index * ElemSz;
        // vector_index is only checked as >= 0; a malformed encoding can report
        // an index whose lane spills past the register, which would underflow
        // the RI.Size - (ByteOff + ElemSz) high-slice size below into a huge
        // makeTemp.  Skip lifting the (invalid) lane insert instead.
        if (ByteOff + ElemSz > RI.Size)
          break;
        NdVar QReg = NdVar::reg(RI.Offset, RI.Size);
        NdVar Trunc = Src;
        if (Src.Size > ElemSz) {
          Trunc = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
        }
        // Build new Q value: [hi | new_elem | lo]
        if (ByteOff == 0) {
          NdVar Hi = S.makeTemp(RI.Size - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {QReg, NdVar::cst(ElemSz, 4)});
          NdVar New = S.makeTemp(RI.Size);
          S.emit(NdOp::CONCAT, New, {Hi, Trunc});
          S.emit(NdOp::COPY, QReg, {New});
        } else if (ByteOff + ElemSz == RI.Size) {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {QReg, NdVar::cst(0, 4)});
          NdVar New = S.makeTemp(RI.Size);
          S.emit(NdOp::CONCAT, New, {Trunc, Lo});
          S.emit(NdOp::COPY, QReg, {New});
        } else {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {QReg, NdVar::cst(0, 4)});
          unsigned HiOff = ByteOff + ElemSz;
          unsigned HiSz = RI.Size - HiOff;
          NdVar Hi = S.makeTemp(HiSz);
          S.emit(NdOp::SUBBYTES, Hi, {QReg, NdVar::cst(HiOff, 4)});
          NdVar MidLo = S.makeTemp(ByteOff + ElemSz);
          S.emit(NdOp::CONCAT, MidLo, {Trunc, Lo});
          NdVar New = S.makeTemp(RI.Size);
          S.emit(NdOp::CONCAT, New, {Hi, MidLo});
          S.emit(NdOp::COPY, QReg, {New});
        }
        break;
      }
    }
    NdVar Dst = L.operandWrite(DstOp);
    if (Insn->id == AARCH64_INS_DUP && DstOp.vas != AARCH64LAYOUT_INVALID) {
      uint16_t ElemSz = 0;
      auto VAS = DstOp.vas;
      if (VAS == AARCH64LAYOUT_VL_16B || VAS == AARCH64LAYOUT_VL_8B)
        ElemSz = 1;
      else if (VAS == AARCH64LAYOUT_VL_8H || VAS == AARCH64LAYOUT_VL_4H)
        ElemSz = 2;
      else if (VAS == AARCH64LAYOUT_VL_4S || VAS == AARCH64LAYOUT_VL_2S)
        ElemSz = 4;
      else if (VAS == AARCH64LAYOUT_VL_2D)
        ElemSz = 8;
      if (ElemSz > 0 && Dst.Size > ElemSz) {
        unsigned NLanes = Dst.Size / ElemSz;
        NdVar Elem = Src;
        if (Elem.Size > ElemSz) {
          NdVar Trunc = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Trunc, {Elem, NdVar::cst(0, 4)});
          Elem = Trunc;
        } else if (Elem.Size < ElemSz) {
          NdVar Ext = S.makeTemp(ElemSz);
          S.emit(NdOp::INT_ZEXT, Ext, {Elem});
          Elem = Ext;
        }
        NdVar Acc = Elem;
        for (unsigned I = 1; I < NLanes; ++I) {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Elem, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_UMOV:
  case AARCH64_INS_SMOV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    if (Src.Size == Dst.Size)
      S.emit(NdOp::COPY, Dst, {Src});
    else if (Insn->id == AARCH64_INS_SMOV)
      S.emit(NdOp::INT_SEXT, Dst, {Src});
    else
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
