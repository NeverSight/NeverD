//===- X86LiftSIMDMove.cpp - x86/x64 SIMD data movement lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX vector data movement: aligned and unaligned register
/// and memory moves, lane extract/insert, byte-align, high/low
/// quadword moves, scalar and 128-bit broadcast, 128-bit lane
/// insert/extract, byte-mask extraction, and the vector-state
/// ops (VZEROUPPER/VZEROALL, EMMS/FEMMS).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Additional SSE / AVX (VEX) packed instructions — treat as their SSE
  // counterparts. Loses per-Lane semantics but preserves dataflow.
  // ========================================================================
  case X86_INS_VMOVDQA:
  case X86_INS_VMOVDQU:
  case X86_INS_VMOVDQA32:
  case X86_INS_VMOVDQA64:
  case X86_INS_VMOVDQU8:
  case X86_INS_VMOVDQU16:
  case X86_INS_VMOVDQU32:
  case X86_INS_VMOVDQU64:
  case X86_INS_VMOVAPS:
  case X86_INS_VMOVAPD:
  case X86_INS_VMOVUPS:
  case X86_INS_VMOVUPD:
  case X86_INS_VMOVSS:
  case X86_INS_VMOVSD:
  case X86_INS_VMOVD:
  case X86_INS_VMOVQ: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (X86.operands[0].type == X86_OP_MEM) {
      // Memory destination (store form, e.g. `vmovdqa [mem], xmm`).  As with
      // the SSE MOV* path, L.operandWrite() on a memory operand yields a
      // discarded ram(0) placeholder, so the store must be emitted explicitly
      // or it is silently dropped (the value never reaches memory).  Scalar
      // forms write only the low element width.
      unsigned StoreSz = 0;
      if (InsnId == X86_INS_VMOVSS || InsnId == X86_INS_VMOVD)
        StoreSz = 4;
      else if (InsnId == X86_INS_VMOVSD || InsnId == X86_INS_VMOVQ)
        StoreSz = 8;
      if (StoreSz && Src.Size > StoreSz) {
        NdVar Lo = S.makeTemp(StoreSz);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.storeToMem(X86.operands[0], Src);
    } else {
      NdVar Dst = L.operandWrite(X86.operands[0]);
      if (Src.Size > Dst.Size) {
        NdVar Lo = S.makeTemp(Dst.Size);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  case X86_INS_VZEROUPPER:
  case X86_INS_VZEROALL:
    // Clear Upper 128b of all YMM regs. We don't model YMM separately
    // (using XMM for both), so this is conservatively a NOP.
    S.emit(NdOp::NOP, {}, {});
    break;

  // PMOVMSKB / VPMOVMSKB — extract MSBs of each byte → GPR bitmask.
  case X86_INS_PMOVMSKB:
  case X86_INS_VPMOVMSKB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pmovmskb, Dst, {Src});
    break;
  }

  // PEXTRB/W/D/Q: extract element from XMM to GPR/mem.
  // Extract ElemSz bytes at the given lane index, then zero-extend to Dst.
  case X86_INS_PEXTRB:
  case X86_INS_PEXTRW:
  case X86_INS_PEXTRD:
  case X86_INS_PEXTRQ: {
    if (X86.op_count < 2)
      break;
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_PEXTRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_PEXTRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_PEXTRQ)
      ElemSz = 8;
    // A MEMORY destination must be written with an explicit STORE:
    // L.operandWrite() returns a discarded ram(0) placeholder, so writing the
    // element into it silently dropped the store (memory left unchanged).
    // Extract into an element-sized temp and store it back; the register form
    // is unchanged (it zero-extends the element into the GPR via the ElemSz <
    // Dst.Size path).
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    if (X86.op_count >= 3 && X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm;
      uint64_t ShiftBits = Idx * ElemSz * 8;
      NdVar ExtSrc = Src;
      if (ShiftBits > 0) {
        ExtSrc = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_RIGHT, ExtSrc, {Src, NdVar::cst(ShiftBits, Src.Size)});
      }
      if (ElemSz < Dst.Size) {
        NdVar Elem = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Elem, {ExtSrc, NdVar::cst(0, 4)});
        S.emit(NdOp::INT_ZEXT, Dst, {Elem});
      } else {
        S.emit(NdOp::SUBBYTES, Dst, {ExtSrc, NdVar::cst(0, 4)});
      }
    } else {
      if (ElemSz < Dst.Size) {
        NdVar Elem = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(0, 4)});
        S.emit(NdOp::INT_ZEXT, Dst, {Elem});
      } else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      }
    }
    if (IsMem)
      S.storeToMem(X86.operands[0], Dst);
    break;
  }
  // PINSRB/W/D/Q: insert element into XMM at Lane index.
  // Use per-lane SUBBYTES+CONCAT to avoid uint64_t mask overflow for i128.
  case X86_INS_PINSRB:
  case X86_INS_PINSRW:
  case X86_INS_PINSRD:
  case X86_INS_PINSRQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_PINSRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_PINSRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_PINSRQ)
      ElemSz = 8;
    if (X86.op_count >= 3 && X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm;
      unsigned NLanes = Dst.Size / ElemSz;
      // A degenerate (size-0) destination would make NLanes 0 and the modulo a
      // division by zero; guard defensively.
      Idx = NLanes ? (Idx % NLanes) : 0;
      NdVar ElemVal = Src;
      if (Src.Size > ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, ElemVal, {Src, NdVar::cst(0, 4)});
      } else if (Src.Size < ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, ElemVal, {Src});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane;
        if (I == Idx) {
          Lane = ElemVal;
        } else {
          Lane = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Lane, {DstR, NdVar::cst(I * ElemSz, 4)});
        }
        if (I == 0) {
          Acc = Lane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Lane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_OR, Dst, {DstR, Src});
    }
    break;
  }
  case X86_INS_PALIGNR: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Palignr, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_MOVLHPS: {
    // dst[63:0] preserved, dst[127:64] = src[63:0].  Built with
    // SUBBYTES/CONCAT: a 64-bit mask constant widened to 16 bytes becomes
    // all-ones (not a low mask), which left the old high half OR'd into the new
    // one.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Lo, {DstR, NdVar::cst(0, 4)});
    NdVar SrcLo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, SrcLo, {Src, NdVar::cst(0, 4)});
    S.emit(NdOp::CONCAT, Dst, {SrcLo, Lo});
    break;
  }
  case X86_INS_MOVHLPS: {
    // dst[63:0] = src[127:64], dst[127:64] preserved.  The old SUBBYTES-to-reg
    // wrote only the low 8 bytes and dropped (zeroed) the preserved high half.
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar SrcHi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, SrcHi, {Src, NdVar::cst(8, 4)});
    NdVar Hi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Hi, {DstR, NdVar::cst(8, 4)});
    S.emit(NdOp::CONCAT, Dst, {Hi, SrcHi});
    break;
  }

  case X86_INS_VBROADCASTSS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastSS, Dst, {Src});
    break;
  }
  case X86_INS_VBROADCASTSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastSD, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastB, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastW, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastD, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastQ, Dst, {Src});
    break;
  }

  case X86_INS_VINSERTF128:
  case X86_INS_VINSERTI128: {
    // VINSERTF128 ymm1, ymm2, xmm3/m128, imm8
    // inserts 128-bit Src into the Lane selected by imm8[0]
    if (X86.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar YmmSrc = L.operandRead(S, X86.operands[1]);
    NdVar XmmSrc = L.operandRead(S, X86.operands[2]);
    uint8_t Lane = static_cast<uint8_t>(X86.operands[3].imm) & 1;
    if (Lane == 0) {
      // Replace low 128 Bits: build from XmmSrc (low) + high half of YmmSrc
      NdVar Hi = S.makeTemp(16);
      S.emit(NdOp::SUBBYTES, Hi, {YmmSrc, NdVar::cst(16, 1)});
      S.emit(NdOp::CONCAT, Dst, {Hi, XmmSrc});
    } else {
      // Replace high 128 Bits: build from low half of YmmSrc + XmmSrc (high)
      NdVar Lo = S.makeTemp(16);
      S.emit(NdOp::SUBBYTES, Lo, {YmmSrc, NdVar::cst(0, 1)});
      S.emit(NdOp::CONCAT, Dst, {XmmSrc, Lo});
    }
    break;
  }
  case X86_INS_VEXTRACTF128:
  case X86_INS_VEXTRACTI128: {
    // VEXTRACTF128 xmm1/m128, ymm2, imm8
    // extracts the 128-bit Lane selected by imm8[0]
    if (X86.op_count < 3)
      break;
    // A MEMORY destination must be written with an explicit STORE:
    // L.operandWrite() of a mem operand yields a discarded ram(0) placeholder,
    // so emitting the extracted lane straight into it silently dropped the
    // write-back for `vextractf128/vextracti128 [mem],ymm,imm` (lane computed,
    // memory left unchanged) — the same class of bug already fixed for
    // PEXTR*/EXTRACTPS and the MOVHPS/MOVLPS partial stores.  Extract into a
    // temp and store it.
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = MemDst ? S.makeTemp(16) : L.operandWrite(X86.operands[0]);
    NdVar YmmSrc = L.operandRead(S, X86.operands[1]);
    uint8_t Lane = static_cast<uint8_t>(X86.operands[2].imm) & 1;
    S.emit(NdOp::SUBBYTES, Dst, {YmmSrc, NdVar::cst(Lane * 16, 1)});
    if (MemDst)
      S.storeToMem(X86.operands[0], Dst);
    break;
  }

  // VPEXTRB/W/D/Q: VEX extract element (same operand layout as SSE).
  case X86_INS_VPEXTRB:
  case X86_INS_VPEXTRW:
  case X86_INS_VPEXTRD:
  case X86_INS_VPEXTRQ: {
    if (X86.op_count < 2)
      break;
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_VPEXTRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_VPEXTRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_VPEXTRQ)
      ElemSz = 8;
    // A MEMORY destination needs an explicit STORE (operandWrite yields a
    // discarded ram(0) placeholder, so the store was dropped).  The byte/word
    // forms must also zero-extend the element into the GPR rather than copying
    // ElemSz-plus-neighbouring-lane bytes (cf. the PEXTR path above).
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar ExtSrc = Src;
    if (X86.op_count >= 3 && X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm;
      uint64_t ShiftBits = Idx * ElemSz * 8;
      if (ShiftBits > 0) {
        ExtSrc = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_RIGHT, ExtSrc, {Src, NdVar::cst(ShiftBits, Src.Size)});
      }
    }
    if (ElemSz < Dst.Size) {
      NdVar Elem = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, Elem, {ExtSrc, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_ZEXT, Dst, {Elem});
    } else {
      S.emit(NdOp::SUBBYTES, Dst, {ExtSrc, NdVar::cst(0, 4)});
    }
    if (IsMem)
      S.storeToMem(X86.operands[0], Dst);
    break;
  }
  // VPMULLQ additional AVX integer.
  case X86_INS_VPMULLQ:
  // VPINSRB/W/D/Q: VEX insert element — per-lane SUBBYTES+CONCAT.
  case X86_INS_VPINSRB:
  case X86_INS_VPINSRW:
  case X86_INS_VPINSRD:
  case X86_INS_VPINSRQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar Base = IsVEX ? L.operandRead(S, X86.operands[1])
                       : L.operandRead(S, X86.operands[0]);
    NdVar Src = IsVEX ? L.operandRead(S, X86.operands[2])
                      : L.operandRead(S, X86.operands[1]);
    int ImmIdx = IsVEX ? 3 : 2;
    uint16_t ElemSz = 1;
    if (InsnId == X86_INS_VPINSRW)
      ElemSz = 2;
    else if (InsnId == X86_INS_VPINSRD)
      ElemSz = 4;
    else if (InsnId == X86_INS_VPINSRQ)
      ElemSz = 8;
    if (ImmIdx < X86.op_count && X86.operands[ImmIdx].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[ImmIdx].imm;
      unsigned NLanes = Dst.Size / ElemSz;
      // A degenerate (size-0) destination would make NLanes 0 and the modulo a
      // division by zero; guard defensively.
      Idx = NLanes ? (Idx % NLanes) : 0;
      NdVar ElemVal = Src;
      if (Src.Size > ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, ElemVal, {Src, NdVar::cst(0, 4)});
      } else if (Src.Size < ElemSz) {
        ElemVal = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, ElemVal, {Src});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane;
        if (I == Idx) {
          Lane = ElemVal;
        } else {
          Lane = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Lane, {Base, NdVar::cst(I * ElemSz, 4)});
        }
        if (I == 0) {
          Acc = Lane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Lane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Base});
    }
    break;
  }

  case X86_INS_VBROADCASTF128:
  case X86_INS_VBROADCASTI128:
  case X86_INS_VBLENDMPS:
  case X86_INS_VBLENDMPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);
    if (InsnId == X86_INS_VPMULLQ) {
      NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Src;
      S.emit(NdOp::INT_MULT, Dst, {A, Src});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // MOVMSKPS/MOVMSKPD/VMOVMSKPS/VMOVMSKPD — extract sign bits per lane.
  case X86_INS_MOVMSKPS:
  case X86_INS_MOVMSKPD:
  case X86_INS_VMOVMSKPS:
  case X86_INS_VMOVMSKPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsPS = (InsnId == X86_INS_MOVMSKPS || InsnId == X86_INS_VMOVMSKPS);
    unsigned LaneSz = IsPS ? 4 : 8;
    unsigned NLanes = Src.Size / LaneSz;
    unsigned SignBit = LaneSz * 8 - 1;
    NdVar Accum = NdVar::cst(0, Dst.Size);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar SignWide = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_RIGHT, SignWide, {Lane, NdVar::cst(SignBit, LaneSz)});
      NdVar Sign = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Sign, {SignWide, NdVar::cst(0, 4)});
      NdVar SignExt = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, SignExt, {Sign});
      NdVar Shifted = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_LEFT, Shifted, {SignExt, NdVar::cst(I, Dst.Size)});
      NdVar NewAccum = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_OR, NewAccum, {Accum, Shifted});
      Accum = NewAccum;
    }
    S.emit(NdOp::COPY, Dst, {Accum});
    break;
  }

  // FEMMS / EMMS — exit MMX State (no-Opc for our purposes).
  case X86_INS_EMMS:
  case X86_INS_FEMMS:
    S.emit(NdOp::NOP, {}, {});
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
