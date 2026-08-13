//===- X86LiftSIMDAVXInt.cpp - x86/x64 AVX-512 packed integer lifter ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AVX-512 packed integer (EVEX VP*) instructions: bitwise
/// logic and ternary logic, compress/expand, scatter,
/// population count, leading-zero and conflict detection,
/// mask-producing tests, rotates and variable shifts,
/// multiplies, blends, truncating moves, mask/vector
/// conversions, funnel shifts and dot products.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDAVXInt(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // P1: AVX-512 packed VP* integer instructions.
  // ========================================================================

  // VPAND{D,Q} / VPOR{D,Q} / VPXOR{D,Q} — AVX-512 bitwise (EVEX).
  case X86_INS_VPANDD:
  case X86_INS_VPANDQ:
  case X86_INS_VPORD:
  case X86_INS_VPORQ:
  case X86_INS_VPXORD:
  case X86_INS_VPXORQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VPANDD:
    case X86_INS_VPANDQ:
      Opc = NdOp::INT_AND;
      break;
    case X86_INS_VPORD:
    case X86_INS_VPORQ:
      Opc = NdOp::INT_OR;
      break;
    default:
      Opc = NdOp::INT_XOR;
    }
    S.emit(Opc, Dst, {A, B});
    break;
  }

  // VPANDN{D,Q} — AVX-512 AND-NOT.
  case X86_INS_VPANDND:
  case X86_INS_VPANDNQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // VPTERNLOG{D,Q} — ternary logic (imm8 truth table).
  case X86_INS_VPTERNLOGD:
  case X86_INS_VPTERNLOGQ: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Imm = L.operandRead(S, X86.operands[3]);
    S.emitIntrinsic(Intrinsic::Vpternlog, Dst, {Dst, A, B, Imm});
    break;
  }

  // VPCOMPRESS{B,W,D,Q} — compress active elements under Mask.
  case X86_INS_VPCOMPRESSB:
  case X86_INS_VPCOMPRESSW:
  case X86_INS_VPCOMPRESSD:
  case X86_INS_VPCOMPRESSQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vpcompress, Dst, {Src});
    break;
  }

  // VPEXPAND{B,W,D,Q} — expand active elements under Mask.
  case X86_INS_VPEXPANDB:
  case X86_INS_VPEXPANDW:
  case X86_INS_VPEXPANDD:
  case X86_INS_VPEXPANDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vpexpand, Dst, {Src});
    break;
  }

  // VPSCATTER{DD,DQ,QD,QQ} — scatter store to memory via index vector.
  case X86_INS_VPSCATTERDD:
  case X86_INS_VPSCATTERDQ:
  case X86_INS_VPSCATTERQD:
  case X86_INS_VPSCATTERQQ: {
    if (X86.op_count >= 2) {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
      S.emitIntrinsic(Intrinsic::Vpscatter, Dst, {Src});
    }
    break;
  }

  // VPOPCNT{B,W,D,Q} — packed population count.
  case X86_INS_VPOPCNTB:
  case X86_INS_VPOPCNTW:
  case X86_INS_VPOPCNTD:
  case X86_INS_VPOPCNTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::POPCOUNT, Dst, {Src});
    break;
  }

  // VPLZCNT{D,Q} — packed leading zero count.
  case X86_INS_VPLZCNTD:
  case X86_INS_VPLZCNTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::LZCOUNT, Dst, {Src});
    break;
  }

  // VPCONFLICT{D,Q} — conflict detection (per-Lane broadcast equality test).
  case X86_INS_VPCONFLICTD:
  case X86_INS_VPCONFLICTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Vpconflict, Dst, {Src});
    break;
  }

  // VPTESTM{B,W,D,Q} / VPTESTNM{B,W,D,Q} — test Mask Bits.
  case X86_INS_VPTESTMB:
  case X86_INS_VPTESTMW:
  case X86_INS_VPTESTMD:
  case X86_INS_VPTESTMQ:
  case X86_INS_VPTESTNMB:
  case X86_INS_VPTESTNMW:
  case X86_INS_VPTESTNMD:
  case X86_INS_VPTESTNMQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    break;
  }

  // VPROL{D,Q} / VPROR{D,Q} — packed rotate by immediate.
  case X86_INS_VPROLD:
  case X86_INS_VPROLQ:
  case X86_INS_VPRORD:
  case X86_INS_VPRORQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Cnt = L.operandRead(S, X86.operands[2]);
    uint16_t Bits = Dst.Size * 8;
    bool IsLeft = (InsnId == X86_INS_VPROLD || InsnId == X86_INS_VPROLQ);
    NdVar APart = S.makeTemp(Dst.Size);
    NdVar BPart = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, APart, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), Cnt});
    S.emit(IsLeft ? NdOp::INT_RIGHT : NdOp::INT_LEFT, BPart, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {APart, BPart});
    break;
  }

  // VPROLV{D,Q} / VPRORV{D,Q} — packed variable rotate.
  case X86_INS_VPROLVD:
  case X86_INS_VPROLVQ:
  case X86_INS_VPRORVD:
  case X86_INS_VPRORVQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Cnt = L.operandRead(S, X86.operands[2]);
    uint16_t Bits = Dst.Size * 8;
    bool IsLeft = (InsnId == X86_INS_VPROLVD || InsnId == X86_INS_VPROLVQ);
    NdVar APart = S.makeTemp(Dst.Size);
    NdVar BPart = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, APart, {Src, Cnt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), Cnt});
    S.emit(IsLeft ? NdOp::INT_RIGHT : NdOp::INT_LEFT, BPart, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {APart, BPart});
    break;
  }

  // VPSRAV{W,Q} / VPSLLV{W} / VPSRLV{W} — variable shifts (additional widths).
  case X86_INS_VPSRAVW:
  case X86_INS_VPSRAVQ:
  case X86_INS_VPSLLVW:
  case X86_INS_VPSRLVW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VPSRAVW:
    case X86_INS_VPSRAVQ:
      Opc = NdOp::INT_ASHR;
      break;
    case X86_INS_VPSLLVW:
      Opc = NdOp::INT_LEFT;
      break;
    default:
      Opc = NdOp::INT_RIGHT;
    }
    S.emit(Opc, Dst, {A, B});
    break;
  }

  // VPMULL{D,W} / VPMULH{W,UW} / VPMUL{DQ,UDQ} — packed multiply per-lane.
  case X86_INS_VPMULLD:
  case X86_INS_VPMULLW:
  case X86_INS_VPMULHW:
  case X86_INS_VPMULHUW:
  case X86_INS_VPMULDQ:
  case X86_INS_VPMULUDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 4;
    if (InsnId == X86_INS_VPMULLW || InsnId == X86_INS_VPMULHW ||
        InsnId == X86_INS_VPMULHUW)
      LaneSz = 2;
    else if (InsnId == X86_INS_VPMULDQ || InsnId == X86_INS_VPMULUDQ)
      LaneSz = 8;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool IsHigh = (InsnId == X86_INS_VPMULHW || InsnId == X86_INS_VPMULHUW);
      bool IsWidening =
          (InsnId == X86_INS_VPMULDQ || InsnId == X86_INS_VPMULUDQ);
      bool IsSigned = (InsnId == X86_INS_VPMULHW || InsnId == X86_INS_VPMULDQ);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        // The widening forms consume every other narrow element, so both forms
        // step one destination lane per iteration.
        unsigned SrcLaneSz = IsWidening ? LaneSz / 2 : LaneSz;
        unsigned SrcOff = I * LaneSz;
        NdVar La = S.makeTemp(SrcLaneSz);
        NdVar Lb = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(SrcOff, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(SrcOff, 4)});
        NdVar Lr;
        if (IsHigh) {
          unsigned WideSz = LaneSz * 2;
          NdVar WA = S.makeTemp(WideSz);
          NdVar WB = S.makeTemp(WideSz);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {La});
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {Lb});
          NdVar WR = S.makeTemp(WideSz);
          S.emit(NdOp::INT_MULT, WR, {WA, WB});
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lr, {WR, NdVar::cst(LaneSz, 4)});
        } else if (IsWidening) {
          NdVar WA = S.makeTemp(LaneSz);
          NdVar WB = S.makeTemp(LaneSz);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {La});
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {Lb});
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {WA, WB});
        } else {
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {La, Lb});
        }
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
    break;
  }

  // VPABSQ — packed absolute value (qword).
  case X86_INS_VPABSQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPBLENDM{B,W,D,Q} — Masked blend.
  case X86_INS_VPBLENDMB:
  case X86_INS_VPBLENDMW:
  case X86_INS_VPBLENDMD:
  case X86_INS_VPBLENDMQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPMOV{DB,DW,QB,QW,QD,WB} — packed truncate (down-convert).
  case X86_INS_VPMOVDB:
  case X86_INS_VPMOVDW:
  case X86_INS_VPMOVQB:
  case X86_INS_VPMOVQW:
  case X86_INS_VPMOVQD:
  case X86_INS_VPMOVWB:
  case X86_INS_VPMOVSDB:
  case X86_INS_VPMOVSDW:
  case X86_INS_VPMOVSQB:
  case X86_INS_VPMOVSQW:
  case X86_INS_VPMOVSQD:
  case X86_INS_VPMOVSWB:
  case X86_INS_VPMOVUSDB:
  case X86_INS_VPMOVUSDW:
  case X86_INS_VPMOVUSQB:
  case X86_INS_VPMOVUSQW:
  case X86_INS_VPMOVUSQD:
  case X86_INS_VPMOVUSWB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPMOVM2{B,W,D,Q} — Mask to vector.
  case X86_INS_VPMOVM2B:
  case X86_INS_VPMOVM2W:
  case X86_INS_VPMOVM2D:
  case X86_INS_VPMOVM2Q: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPMOV{B,W,D,Q}2M — vector to Mask.
  case X86_INS_VPMOVB2M:
  case X86_INS_VPMOVW2M:
  case X86_INS_VPMOVD2M:
  case X86_INS_VPMOVQ2M: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPBROADCASTM{B2Q,W2D} — broadcast Mask to vector.
  case X86_INS_VPBROADCASTMB2Q:
  case X86_INS_VPBROADCASTMW2D: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPSHLD{D,Q,W} / VPSHRD{D,Q,W} — double shift by immediate.
  case X86_INS_VPSHLDD:
  case X86_INS_VPSHLDQ:
  case X86_INS_VPSHLDW:
  case X86_INS_VPSHRDD:
  case X86_INS_VPSHRDQ:
  case X86_INS_VPSHRDW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    bool IsLeft = (InsnId == X86_INS_VPSHLDD || InsnId == X86_INS_VPSHLDQ ||
                   InsnId == X86_INS_VPSHLDW);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Dst, {A, B});
    break;
  }

  // VPSHLDV{D,Q,W} / VPSHRDV{D,Q,W} — variable double shift.
  case X86_INS_VPSHLDVD:
  case X86_INS_VPSHLDVQ:
  case X86_INS_VPSHLDVW:
  case X86_INS_VPSHRDVD:
  case X86_INS_VPSHRDVQ:
  case X86_INS_VPSHRDVW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    bool IsLeft = (InsnId == X86_INS_VPSHLDVD || InsnId == X86_INS_VPSHLDVQ ||
                   InsnId == X86_INS_VPSHLDVW);
    S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Dst, {A, B});
    break;
  }

  // VPDPBUSD{,S} / VPDPWSSD{,S} — VNNI dot product.
  case X86_INS_VPDPBUSD:
  case X86_INS_VPDPBUSDS:
  case X86_INS_VPDPWSSD:
  case X86_INS_VPDPWSSDS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }

  // VPMADD52{H,L}UQ — packed multiply-add 52-bit unsigned.
  case X86_INS_VPMADD52HUQ:
  case X86_INS_VPMADD52LUQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_MULT, Dst, {A, B});
    break;
  }

  // VPSHUFBITQMB — shuffle Bits into Mask register.
  case X86_INS_VPSHUFBITQMB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vpshufbitqmb, Dst, {A, B});
    break;
  }

  // VPERMB / VPERMW — packed byte/word permute.
  case X86_INS_VPERMB:
  case X86_INS_VPERMW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VPERM{I2,T2}{B,D,PS,PD,Q,W} already handled above in permute section.

  // VPMULTISHIFTQB — multi-shift bytes from qwords.
  case X86_INS_VPMULTISHIFTQB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Vpmultishiftqb, Dst, {A, B});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
