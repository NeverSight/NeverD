//===- X86LiftSIMDShuffle.cpp - x86/x64 SIMD shuffle, permute and blend lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX lane-selection instructions: immediate and variable
/// blends, byte/word/dword shuffles, in-lane and cross-lane
/// permutes, 128-bit lane permutes, and the masked vector
/// load/store forms.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDShuffle(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_PBLENDW:
  case X86_INS_BLENDPS:
  case X86_INS_BLENDPD:
  case X86_INS_VPBLENDW:
  case X86_INS_VBLENDPS:
  case X86_INS_VBLENDPD: {
    // Immediate blend: each imm bit selects the matching lane from src2 (set)
    // or src1 (clear).  SSE forms reuse dst as src1 (3 operands); VEX forms are
    // non-destructive with an explicit src1 (4 operands: dst, src1, src2, imm).
    if (X86.op_count < 3)
      break;
    unsigned Base = (X86.op_count >= 4) ? 1 : 0;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[Base]);
    NdVar Src = L.operandRead(S, X86.operands[Base + 1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[Base + 2].imm);
    unsigned LaneSz =
        (InsnId == X86_INS_BLENDPD || InsnId == X86_INS_VBLENDPD)   ? 8
        : (InsnId == X86_INS_BLENDPS || InsnId == X86_INS_VBLENDPS) ? 4
                                                                    : 2;
    unsigned NLanes = Dst.Size / LaneSz;
    std::vector<NdVar> Lanes;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      if (Imm & (1u << I))
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      else
        S.emit(NdOp::SUBBYTES, Lane, {DstR, NdVar::cst(I * LaneSz, 4)});
      Lanes.push_back(Lane);
    }
    NdVar Acc = Lanes[0];
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar W = S.makeTemp((I + 1) * LaneSz);
      S.emit(NdOp::CONCAT, W, {Lanes[I], Acc});
      Acc = W;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case X86_INS_PBLENDVB:
  case X86_INS_BLENDVPS:
  case X86_INS_BLENDVPD: {
    // SSE4.1 variable blend: per-lane, if the high (sign) bit of the
    // corresponding element of the implicit XMM0 mask is set, take the lane
    // from the source operand, otherwise keep the destination's lane.
    //   PBLENDVB: 8-bit lanes (mask bit 7), BLENDVPS: 32-bit (bit 31),
    //   BLENDVPD: 64-bit (bit 63).
    // Capstone exposes XMM0 as operands[2]; fall back to the register if not.
    if (X86.op_count < 2)
      break;
    unsigned LaneSz = (InsnId == X86_INS_BLENDVPS)   ? 4
                      : (InsnId == X86_INS_BLENDVPD) ? 8
                                                     : 1;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar FalseV = L.operandRead(S, X86.operands[0]); // dst keeps its lane
    NdVar TrueV = L.operandRead(S, X86.operands[1]);  // src provides its lane
    NdVar Mask = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[2])
                                     : NdVar::reg(x86reg::XMM0, Dst.Size);
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned Off = I * LaneSz;
      NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz),
            Lm = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {FalseV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {TrueV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lm, {Mask, NdVar::cst(Off, 4)});
      NdVar Cond = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, Cond, {Lm, NdVar::cst(0, LaneSz)});
      NdVar Lr = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
      if (I == 0) {
        Acc = Lr;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Lr, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // VPERMILPS / VPERMILPD — IN-LANE permute of 32/64-bit elements.  The control
  // (imm8, or a per-element register) selects an element WITHIN each 128-bit
  // lane; the same imm8 applies to every lane.  The old code COPYed the source
  // and dropped the permutation entirely (control ignored) — wrong for every
  // non-identity control.  imm forms use constant lane indices; the variable
  // form selects per element from the low control bits (PS: bits[1:0], PD:
  // bit[1]).  Assemble the result low->high with a power-of-two CONCAT tree.
  case X86_INS_VPERMILPS:
  case X86_INS_VPERMILPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t ES = (InsnId == X86_INS_VPERMILPD) ? 8 : 4; // element size
    uint16_t EPL = 16 / ES;                              // elems per 128 lane
    uint16_t N = Dst.Size / ES;                          // total elements
    if (N < 2 || N > 8) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    bool IsImm = (X86.operands[2].type == X86_OP_IMM);
    uint64_t Imm = IsImm ? static_cast<uint64_t>(X86.operands[2].imm) : 0;
    NdVar Ctrl = IsImm ? NdVar() : L.operandRead(S, X86.operands[2]);
    NdVar Elems[8];
    for (uint16_t I = 0; I < N; ++I) {
      uint16_t Base = (I / EPL) * EPL; // first src element of this 128 lane
      if (IsImm) {
        uint16_t Sel =
            (ES == 4) ? ((Imm >> (2 * (I % EPL))) & 3) : ((Imm >> I) & 1);
        Elems[I] = S.makeTemp(ES);
        S.emit(NdOp::SUBBYTES, Elems[I],
               {Src, NdVar::cst(static_cast<uint64_t>(Base + Sel) * ES, 4)});
      } else {
        NdVar CtrlI = S.makeTemp(ES);
        S.emit(NdOp::SUBBYTES, CtrlI,
               {Ctrl, NdVar::cst(static_cast<uint64_t>(I) * ES, 4)});
        NdVar Idx = S.makeTemp(ES);
        if (ES == 4) {
          S.emit(NdOp::INT_AND, Idx, {CtrlI, NdVar::cst(3, ES)});
        } else {
          NdVar Sh = S.makeTemp(ES);
          S.emit(NdOp::INT_RIGHT, Sh, {CtrlI, NdVar::cst(1, ES)});
          S.emit(NdOp::INT_AND, Idx, {Sh, NdVar::cst(1, ES)});
        }
        // Dynamic in-lane gather: select among the EPL candidates by Idx.
        NdVar Acc = S.makeTemp(ES);
        S.emit(
            NdOp::SUBBYTES, Acc,
            {Src, NdVar::cst(static_cast<uint64_t>(Base + EPL - 1) * ES, 4)});
        for (int K = static_cast<int>(EPL) - 2; K >= 0; --K) {
          NdVar Cand = S.makeTemp(ES);
          S.emit(NdOp::SUBBYTES, Cand,
                 {Src, NdVar::cst(static_cast<uint64_t>(Base + K) * ES, 4)});
          NdVar Eq = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, Eq,
                 {Idx, NdVar::cst(static_cast<uint64_t>(K), ES)});
          NdVar NewAcc = S.makeTemp(ES);
          S.emit(NdOp::SELECT, NewAcc, {Eq, Cand, Acc});
          Acc = NewAcc;
        }
        Elems[I] = Acc;
      }
    }
    // Power-of-two CONCAT tree (only 8/16/32-byte temps); final merge -> Dst.
    uint16_t Count = N, Sz = ES;
    while (Count > 1) {
      uint16_t Half = Count / 2;
      uint16_t NewSz = static_cast<uint16_t>(Sz * 2);
      for (uint16_t K = 0; K < Half; ++K) {
        NdVar Out = (Half == 1) ? Dst : S.makeTemp(NewSz);
        S.emit(NdOp::CONCAT, Out, {Elems[2 * K + 1], Elems[2 * K]});
        Elems[K] = Out;
      }
      Count = Half;
      Sz = NewSz;
    }
    break;
  }

  // VPERMPD / VPERMQ — CROSS-LANE permute of 4 qwords by imm8 (each 2-bit field
  // selects any of the 4 source qwords).  Bit-identical ops (fp vs int label).
  case X86_INS_VPERMPD:
  case X86_INS_VPERMQ: {
    if (X86.op_count < 3 || X86.operands[2].type != X86_OP_IMM) {
      if (X86.op_count >= 2) {
        NdVar Dst = L.operandWrite(X86.operands[0]);
        NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint64_t Imm = static_cast<uint64_t>(X86.operands[2].imm);
    NdVar E[4];
    for (int I = 0; I < 4; ++I) {
      uint16_t Sel = (Imm >> (2 * I)) & 3;
      E[I] = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E[I],
             {Src, NdVar::cst(static_cast<uint64_t>(Sel) * 8, 4)});
    }
    NdVar Lo = S.makeTemp(16), Hi = S.makeTemp(16);
    S.emit(NdOp::CONCAT, Lo, {E[1], E[0]});
    S.emit(NdOp::CONCAT, Hi, {E[3], E[2]});
    S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    break;
  }

  // VPERMPS — CROSS-LANE permute of 8 single-precision elements by a
  // per-element dword index (operands[1]).  Bit-identical to VPERMD, so reuse
  // that intrinsic (operands order: dst, indices, source).  Old code dropped
  // the index.
  case X86_INS_VPERMPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Idx = L.operandRead(S, X86.operands[1]);
    NdVar Src = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Permd, Dst, {Idx, Src});
    break;
  }

  // VPERMI2*/VPERMT2* — AVX-512 two-source table permutes (the destination is
  // also a source).  Unicorn does not implement AVX-512, so these cannot be
  // roundtrip-verified; keep a non-crashing COPY placeholder (control ignored).
  case X86_INS_VPERMI2D:
  case X86_INS_VPERMI2Q:
  case X86_INS_VPERMI2PS:
  case X86_INS_VPERMI2PD:
  case X86_INS_VPERMI2W:
  case X86_INS_VPERMI2B:
  case X86_INS_VPERMT2D:
  case X86_INS_VPERMT2Q:
  case X86_INS_VPERMT2PS:
  case X86_INS_VPERMT2PD:
  case X86_INS_VPERMT2W:
  case X86_INS_VPERMT2B: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // VEX 3-operand shuffles: VPSHUFB xmm1, xmm2, xmm3 → pshufb(xmm2, xmm3)
  case X86_INS_VPSHUFB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pshufb, Dst, {A, B});
    break;
  }
  case X86_INS_VPSHUFD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufd, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPSHUFLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshuflw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPSHUFHW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufhw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VSHUFPS: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Shufps, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VSHUFPD: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Shufpd, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPALIGNR: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Palignr, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }

  // VPBLENDD ymm1, ymm2, ymm3/m, imm8 — per-dword select: imm8[i] picks src2
  // (operands[2]) when set, else src1 (operands[1]).  Lift natively per dword
  // (SUBBYTES + CONCAT) so the 256-bit form roundtrips without an opaque
  // INTRINSIC; 128-bit uses the low 4 imm8 bits.
  case X86_INS_VPBLENDD: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    int N = static_cast<int>(Dst.Size) / 4;
    NdVar E[8];
    for (int I = 0; I < N; ++I) {
      NdVar Src = ((Imm >> I) & 1) ? B : A;
      E[I] = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E[I],
             {Src, NdVar::cst(static_cast<uint64_t>(I) * 4, 4)});
    }
    NdVar Acc = E[0];
    for (int I = 1; I < N; ++I) {
      NdVar T = (I == N - 1) ? Dst : S.makeTemp(4 * (I + 1));
      S.emit(NdOp::CONCAT, T, {E[I], Acc});
      Acc = T;
    }
    break;
  }
  case X86_INS_VPERM2F128:
  case X86_INS_VPERM2I128: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Perm2f128, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPERMD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Idx = L.operandRead(S, X86.operands[1]);
    NdVar Src = L.operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Permd, Dst, {Idx, Src});
    break;
  }

  case X86_INS_VPMASKMOVD:
  case X86_INS_VPMASKMOVQ:
  case X86_INS_VMASKMOVPS:
  case X86_INS_VMASKMOVPD: {
    if (X86.op_count < 3)
      break;
    bool IsQword =
        (InsnId == X86_INS_VPMASKMOVQ || InsnId == X86_INS_VMASKMOVPD);
    bool IsStore = (X86.operands[0].type == X86_OP_MEM);
    if (IsStore) {
      // The emitter consumes input[1] as the destination ADDRESS (IntToPtr).
      // L.operandRead() on a MEM operand would emit a LOAD and return the
      // loaded value, so the masked store wrote to the value-at-the-destination
      // instead of the destination itself.  Use computeEA for the address.
      NdVar AddrVn = S.computeEA(X86.operands[0]);
      NdVar MaskVn = L.operandRead(S, X86.operands[1]);
      NdVar DataVn = L.operandRead(S, X86.operands[2]);
      Intrinsic IId =
          IsQword ? Intrinsic::MaskedStoreQ : Intrinsic::MaskedStoreD;
      S.emitIntrinsic(IId, NdVar::reg(x86reg::RAX, 0),
                      {AddrVn, MaskVn, DataVn});
    } else {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar MaskVn = L.operandRead(S, X86.operands[1]);
      // Likewise the emitter loads through input[1] as an ADDRESS; passing
      // L.operandRead() (the loaded value) caused a double dereference.
      NdVar AddrVn = S.computeEA(X86.operands[2]);
      Intrinsic IId = IsQword ? Intrinsic::MaskedLoadQ : Intrinsic::MaskedLoadD;
      S.emitIntrinsic(IId, Dst, {AddrVn, MaskVn});
    }
    break;
  }

  // VEX variable blend: VBLENDVPS/VBLENDVPD/VPBLENDVB take an explicit mask
  // register as the last operand (operands[3]); per lane, the high (sign) bit
  // of the mask element selects src2 (operands[2]) over src1 (operands[1]).
  case X86_INS_VPBLENDVB:
  case X86_INS_VBLENDVPS:
  case X86_INS_VBLENDVPD: {
    if (X86.op_count < 4)
      break;
    unsigned LaneSz = (InsnId == X86_INS_VBLENDVPS)   ? 4
                      : (InsnId == X86_INS_VBLENDVPD) ? 8
                                                      : 1;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar FalseV = L.operandRead(S, X86.operands[1]); // src1
    NdVar TrueV = L.operandRead(S, X86.operands[2]);  // src2
    NdVar Mask = L.operandRead(S, X86.operands[3]);
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned Off = I * LaneSz;
      NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz),
            Lm = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {FalseV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {TrueV, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Lm, {Mask, NdVar::cst(Off, 4)});
      NdVar Cond = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, Cond, {Lm, NdVar::cst(0, LaneSz)});
      NdVar Lr = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
      if (I == 0) {
        Acc = Lr;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Lr, Acc});
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
