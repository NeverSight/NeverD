//===- X86LiftSIMDLogicShift.cpp - x86/x64 SIMD bitwise logic and shift lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX bulk bitwise AND/OR/XOR, whole-register byte shifts,
/// and per-lane shifts with both a shared and a per-lane
/// (variable) shift count.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDLogicShift(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_VXORPS:
  case X86_INS_VXORPD:
  case X86_INS_VPXOR:
  case X86_INS_VANDPS:
  case X86_INS_VANDPD:
  case X86_INS_VPAND:
  case X86_INS_VORPS:
  case X86_INS_VORPD:
  case X86_INS_VPOR: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    uint8_t SrcCount = X86.op_count;
    NdVar A = (SrcCount >= 3) ? L.operandRead(S, X86.operands[1])
                              : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[SrcCount - 1]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VXORPS:
    case X86_INS_VXORPD:
    case X86_INS_VPXOR:
      Opc = NdOp::INT_XOR;
      break;
    case X86_INS_VANDPS:
    case X86_INS_VANDPD:
    case X86_INS_VPAND:
      Opc = NdOp::INT_AND;
      break;
    default:
      Opc = NdOp::INT_OR;
    }
    S.emit(Opc, Dst, {A, B});
    break;
  }

  // Variable packed shifts (AVX2): each lane is shifted by its OWN count lane.
  // A full-width shift would bleed bits across lanes and ignore per-element
  // counts.  x86 does not mask the count: out-of-range yields 0 (logical) or a
  // sign fill (arithmetic).
  case X86_INS_VPSLLVD:
  case X86_INS_VPSLLVQ:
  case X86_INS_VPSRLVD:
  case X86_INS_VPSRLVQ:
  case X86_INS_VPSRAVD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    unsigned LaneSz =
        (InsnId == X86_INS_VPSLLVQ || InsnId == X86_INS_VPSRLVQ) ? 8 : 4;
    unsigned LaneBits = LaneSz * 8;
    NdOp ShiftOp =
        (InsnId == X86_INS_VPSLLVD || InsnId == X86_INS_VPSLLVQ)
            ? NdOp::INT_LEFT
            : (InsnId == X86_INS_VPSRAVD ? NdOp::INT_ASHR : NdOp::INT_RIGHT);
    bool Arith = (ShiftOp == NdOp::INT_ASHR);
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar LA = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LaneSz, 4)});
      NdVar LCnt = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, LCnt, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar InRange = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, InRange, {LCnt, NdVar::cst(LaneBits, LaneSz)});
      NdVar CntUse = LCnt;
      if (Arith) {
        CntUse = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, CntUse,
               {InRange, LCnt, NdVar::cst(LaneBits - 1, LaneSz)});
      }
      NdVar Raw = S.makeTemp(LaneSz);
      S.emit(ShiftOp, Raw, {LA, CntUse});
      NdVar R = Raw;
      if (!Arith) {
        R = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, R, {InRange, Raw, NdVar::cst(0, LaneSz)});
      }
      if (I == 0) {
        Acc = R;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {R, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // AVX whole-register byte shift: each aligned 128-bit lane shifts
  // independently by the imm8 byte count (>=16 -> 0).
  case X86_INS_VPSLLDQ:
  case X86_INS_VPSRLDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    uint64_t Bytes = (X86.operands[X86.op_count - 1].type == X86_OP_IMM)
                         ? (uint64_t)X86.operands[X86.op_count - 1].imm
                         : 0;
    bool Left = (InsnId == X86_INS_VPSLLDQ);
    unsigned LaneBytes = (Dst.Size >= 16) ? 16 : Dst.Size;
    unsigned NLanes = Dst.Size / LaneBytes;
    NdVar Acc = S.makeTemp(0);
    for (unsigned L = 0; L < NLanes; ++L) {
      NdVar Lane = S.makeTemp(LaneBytes);
      S.emit(NdOp::SUBBYTES, Lane, {A, NdVar::cst(L * LaneBytes, 4)});
      NdVar R;
      if (Bytes >= LaneBytes) {
        R = NdVar::cst(0, LaneBytes);
      } else {
        R = S.makeTemp(LaneBytes);
        S.emit(Left ? NdOp::INT_LEFT : NdOp::INT_RIGHT, R,
               {Lane, NdVar::cst(Bytes * 8, LaneBytes)});
      }
      if (L == 0) {
        Acc = R;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneBytes);
        S.emit(NdOp::CONCAT, Next, {R, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // AVX packed element shifts: uniform scalar count from imm8 or the low 64
  // bits of the count operand, applied per lane.  x86 does not mask the count:
  // out-of-range yields 0 (logical) or a sign fill (arithmetic).
  case X86_INS_VPSLLD:
  case X86_INS_VPSLLW:
  case X86_INS_VPSLLQ:
  case X86_INS_VPSRLD:
  case X86_INS_VPSRLW:
  case X86_INS_VPSRLQ:
  case X86_INS_VPSRAD:
  case X86_INS_VPSRAW:
  case X86_INS_VPSRAQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    unsigned LaneSz = 0;
    NdOp ShiftOp = NdOp::INT_LEFT;
    switch (InsnId) {
    case X86_INS_VPSLLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_VPSLLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_VPSLLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_VPSRLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_VPSRLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_VPSRLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_VPSRAW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_ASHR;
      break;
    case X86_INS_VPSRAD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_ASHR;
      break;
    default:
      LaneSz = 8;
      ShiftOp = NdOp::INT_ASHR;
      break;
    }
    unsigned LaneBits = LaneSz * 8;
    bool Arith = (ShiftOp == NdOp::INT_ASHR);
    bool CntIsImm = (X86.operands[X86.op_count - 1].type == X86_OP_IMM);
    NdVar RawCnt;
    if (CntIsImm) {
      RawCnt = NdVar::cst((uint64_t)X86.operands[X86.op_count - 1].imm, 8);
    } else {
      NdVar SrcFull = L.operandRead(S, X86.operands[X86.op_count - 1]);
      RawCnt = S.makeTemp(8);
      if (SrcFull.Size >= 8)
        S.emit(NdOp::SUBBYTES, RawCnt, {SrcFull, NdVar::cst(0, 4)});
      else
        S.emit(NdOp::INT_ZEXT, RawCnt, {SrcFull});
    }
    NdVar InRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, InRange, {RawCnt, NdVar::cst(LaneBits, 8)});
    NdVar CntL = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, CntL, {RawCnt, NdVar::cst(0, 4)});
    if (Arith) {
      NdVar Clamped = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Clamped,
             {InRange, CntL, NdVar::cst(LaneBits - 1, LaneSz)});
      CntL = Clamped;
    }
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {A, NdVar::cst(I * LaneSz, 4)});
      NdVar Raw = S.makeTemp(LaneSz);
      S.emit(ShiftOp, Raw, {Lane, CntL});
      NdVar Shifted = Raw;
      if (!Arith) {
        Shifted = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Shifted, {InRange, Raw, NdVar::cst(0, LaneSz)});
      }
      if (I == 0) {
        Acc = Shifted;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
