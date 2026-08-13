//===- X86LiftLegacySSEInt.cpp - x86 baseline SSE integer lifter ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Baseline (non-VEX) SSE/SSE2 integer instructions modelled
/// as bulk 128-bit operations: bitwise logic, packed
/// add/subtract, whole-register and per-lane shifts, and the
/// shuffle/unpack family.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftLegacySSEInt(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Legacy SSE element-wise integer ops (bulk 128-bit nd-var operations).
  // Loses per-lane semantics but preserves dataflow shape.
  // ========================================================================
  case X86_INS_XORPS:
  case X86_INS_XORPD:
  case X86_INS_PXOR:
  case X86_INS_ORPS:
  case X86_INS_ORPD:
  case X86_INS_POR:
  case X86_INS_ANDPS:
  case X86_INS_ANDPD:
  case X86_INS_PAND:
  case X86_INS_ANDNPS:
  case X86_INS_ANDNPD:
  case X86_INS_PANDN:
  case X86_INS_PADDB:
  case X86_INS_PADDW:
  case X86_INS_PADDD:
  case X86_INS_PADDQ:
  case X86_INS_PSUBB:
  case X86_INS_PSUBW:
  case X86_INS_PSUBD:
  case X86_INS_PSUBQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdOp Opc = NdOp::COPY;
    switch (InsnId) {
    case X86_INS_XORPS:
    case X86_INS_XORPD:
    case X86_INS_PXOR:
      Opc = NdOp::INT_XOR;
      break;
    case X86_INS_ORPS:
    case X86_INS_ORPD:
    case X86_INS_POR:
      Opc = NdOp::INT_OR;
      break;
    case X86_INS_ANDPS:
    case X86_INS_ANDPD:
    case X86_INS_PAND:
      Opc = NdOp::INT_AND;
      break;
    case X86_INS_ANDNPS:
    case X86_INS_ANDNPD:
    case X86_INS_PANDN: {
      NdVar Neg = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, Neg, {Dst});
      S.emit(NdOp::INT_AND, Dst, {Neg, Src});
      break;
    }
    case X86_INS_PADDB:
    case X86_INS_PADDW:
    case X86_INS_PADDD:
    case X86_INS_PADDQ:
    case X86_INS_PSUBB:
    case X86_INS_PSUBW:
    case X86_INS_PSUBD:
    case X86_INS_PSUBQ: {
      unsigned LaneSz = 0;
      switch (InsnId) {
      case X86_INS_PADDB:
      case X86_INS_PSUBB:
        LaneSz = 1;
        break;
      case X86_INS_PADDW:
      case X86_INS_PSUBW:
        LaneSz = 2;
        break;
      case X86_INS_PADDD:
      case X86_INS_PSUBD:
        LaneSz = 4;
        break;
      case X86_INS_PADDQ:
      case X86_INS_PSUBQ:
        LaneSz = 8;
        break;
      default:
        break;
      }
      NdOp LaneOpc = (InsnId == X86_INS_PADDB || InsnId == X86_INS_PADDW ||
                      InsnId == X86_INS_PADDD || InsnId == X86_INS_PADDQ)
                         ? NdOp::INT_ADD
                         : NdOp::INT_SUB;
      // Split into low/high halves to avoid non-power-of-2 intermediate types.
      unsigned HalfSz = Dst.Size / 2;
      unsigned LanesPerHalf = HalfSz / LaneSz;
      auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < LanesPerHalf; ++I) {
          unsigned Off = BaseOff + I * LaneSz;
          NdVar La = S.makeTemp(LaneSz);
          NdVar Lb = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Dst, NdVar::cst(Off, 4)});
          S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(LaneOpc, Lr, {La, Lb});
          if (I == 0) {
            Acc = Lr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Lr, Acc});
            Acc = Next;
          }
        }
        return Acc;
      };
      NdVar LoHalf = BuildHalf(0);
      NdVar HiHalf = BuildHalf(HalfSz);
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
      S.emit(NdOp::COPY, Dst, {Full});
      break;
    }
    default:
      Opc = NdOp::COPY;
    }
    if (InsnId != X86_INS_ANDNPS && InsnId != X86_INS_ANDNPD &&
        InsnId != X86_INS_PANDN && InsnId != X86_INS_PADDB &&
        InsnId != X86_INS_PADDW && InsnId != X86_INS_PADDD &&
        InsnId != X86_INS_PADDQ && InsnId != X86_INS_PSUBB &&
        InsnId != X86_INS_PSUBW && InsnId != X86_INS_PSUBD &&
        InsnId != X86_INS_PSUBQ) {
      S.emit(Opc, Dst, {Dst, Src});
    }
    break;
  }

  // ========================================================================
  // PSLLDQ / PSRLDQ (byte-shift) and packed element shifts
  // ========================================================================
  case X86_INS_PSLLDQ:
  case X86_INS_PSRLDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Bits = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_LEFT, Bits, {Src, NdVar::cst(3, Dst.Size)});
    NdOp Opc = (InsnId == X86_INS_PSLLDQ) ? NdOp::INT_LEFT : NdOp::INT_RIGHT;
    S.emit(Opc, Dst, {Dst, Bits});
    break;
  }
  case X86_INS_PSLLD:
  case X86_INS_PSLLW:
  case X86_INS_PSLLQ:
  case X86_INS_PSRLD:
  case X86_INS_PSRLW:
  case X86_INS_PSRLQ:
  case X86_INS_PSRAW:
  case X86_INS_PSRAD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    unsigned LaneSz = 0;
    NdOp ShiftOp = NdOp::INT_LEFT;
    switch (InsnId) {
    case X86_INS_PSLLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSLLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSLLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_LEFT;
      break;
    case X86_INS_PSRLW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRLD:
      LaneSz = 4;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRLQ:
      LaneSz = 8;
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case X86_INS_PSRAW:
      LaneSz = 2;
      ShiftOp = NdOp::INT_ASHR;
      break;
    default:
      LaneSz = 4;
      ShiftOp = NdOp::INT_ASHR;
      break;
    }
    unsigned LaneBits = LaneSz * 8;
    bool Arith = (ShiftOp == NdOp::INT_ASHR);

    // The shift count is the scalar in the low 64 bits of the source (or imm8),
    // shared by all lanes.  x86 does NOT mask it: a logical shift yields 0 and
    // an arithmetic shift yields a sign fill when count >= lane bit width.
    NdVar RawCnt;
    if (X86.operands[1].type == X86_OP_IMM) {
      RawCnt = NdVar::cst((uint64_t)X86.operands[1].imm, 8);
    } else {
      NdVar SrcFull = L.operandRead(S, X86.operands[1]);
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
      S.emit(NdOp::SUBBYTES, Lane, {Dst, NdVar::cst(I * LaneSz, 4)});
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

  // ========================================================================
  // SIMD shuffles / unpacks
  // ========================================================================
  case X86_INS_PSHUFD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufd, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PSHUFB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pshufb, Dst, {Dst, Src});
    break;
  }
  case X86_INS_PSHUFLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshuflw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PSHUFHW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufhw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHUFPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Shufps, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHUFPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Shufpd, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PUNPCKLBW:
  case X86_INS_PUNPCKHBW:
  case X86_INS_PUNPCKLWD:
  case X86_INS_PUNPCKHWD:
  case X86_INS_PUNPCKLDQ:
  case X86_INS_PUNPCKHDQ:
  case X86_INS_PUNPCKLQDQ:
  case X86_INS_PUNPCKHQDQ:
  case X86_INS_UNPCKLPS:
  case X86_INS_UNPCKHPS:
  case X86_INS_UNPCKLPD:
  case X86_INS_UNPCKHPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_PUNPCKLBW:
      Id = Intrinsic::Punpcklbw;
      break;
    case X86_INS_PUNPCKHBW:
      Id = Intrinsic::Punpckhbw;
      break;
    case X86_INS_PUNPCKLWD:
      Id = Intrinsic::Punpcklwd;
      break;
    case X86_INS_PUNPCKHWD:
      Id = Intrinsic::Punpckhwd;
      break;
    case X86_INS_PUNPCKLDQ:
      Id = Intrinsic::Punpckldq;
      break;
    case X86_INS_PUNPCKHDQ:
      Id = Intrinsic::Punpckhdq;
      break;
    case X86_INS_PUNPCKLQDQ:
      Id = Intrinsic::Punpcklqdq;
      break;
    case X86_INS_PUNPCKHQDQ:
      Id = Intrinsic::Punpckhqdq;
      break;
    case X86_INS_UNPCKLPS:
      Id = Intrinsic::Unpcklps;
      break;
    case X86_INS_UNPCKHPS:
      Id = Intrinsic::Unpckhps;
      break;
    case X86_INS_UNPCKLPD:
      Id = Intrinsic::Unpcklpd;
      break;
    default:
      Id = Intrinsic::Unpckhpd;
      break;
    }
    S.emitIntrinsic(Id, Dst, {Dst, Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
