//===- X86LiftSIMDAVXSSE.cpp - x86/x64 late-SSE instruction lifter --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE3/SSSE3/SSE4 instructions reached through the AVX
/// dispatcher: MOVBE, non-temporal loads and stores, ROUND,
/// dot product, EXTRACTPS/INSERTPS, mask moves, LDDQU, the
/// MOVDDUP/MOVSHDUP/MOVSLDUP duplications, MPSADBW,
/// PHMINPOSUW and the cache-control hints.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftSIMDAVXSSE(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // P0: SSE — MOVBE, non-temporal stores/loads, ROUND, dot product,
  //     extract/insert, Mask moves, LDDQU, MOVDDUP, MPSADBW, PHMINPOSUW,
  //     MOVSHDUP, MOVSLDUP, cache control extensions.
  // ========================================================================

  // MOVBE — byte-swap load/store (endian convert on the moved value).
  case X86_INS_MOVBE: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Swapped = S.emitByteSwap(Src);
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Swapped);
    } else {
      S.emit(NdOp::COPY, Dst, {Swapped});
    }
    break;
  }

  // Non-temporal stores/loads — semantically identical to normal moves for
  // our purposes (the non-temporal hint only affects cache behavior).
  case X86_INS_MOVNTDQ:
  case X86_INS_MOVNTDQA:
  case X86_INS_MOVNTI:
  case X86_INS_MOVNTPD:
  case X86_INS_MOVNTPS:
  case X86_INS_MOVNTQ:
  case X86_INS_MOVNTSD:
  case X86_INS_MOVNTSS:
  case X86_INS_VMOVNTDQ:
  case X86_INS_VMOVNTDQA:
  case X86_INS_VMOVNTPD:
  case X86_INS_VMOVNTPS: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Src);
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // LDDQU — load unaligned double quadword (semantically identical to
  // MOVDQU for our purposes).
  case X86_INS_LDDQU:
  case X86_INS_VLDDQU: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // MOVDDUP / MOVSHDUP / MOVSLDUP — duplicate lanes.
  case X86_INS_MOVDDUP:
  case X86_INS_VMOVDDUP: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Movddup, Dst, {Src});
    break;
  }
  case X86_INS_MOVSHDUP:
  case X86_INS_VMOVSHDUP: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Movshdup, Dst, {Src});
    break;
  }
  case X86_INS_MOVSLDUP:
  case X86_INS_VMOVSLDUP: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Movsldup, Dst, {Src});
    break;
  }

  // ROUNDSS/VROUNDSS — scalar single float rounding (lowest 32 bits).
  // Legacy: roundss xmm, xmm/m32, imm8 (3 operands: dst, src, imm)
  // VEX:    vroundss xmm, xmm, xmm/m32, imm8 (4 operands)
  case X86_INS_ROUNDSS:
  case X86_INS_VROUNDSS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar PassThru = L.operandRead(S, X86.operands[IsVEX ? 1 : 0]);
    NdVar Src = L.operandRead(S, X86.operands[IsVEX ? 2 : 1]);
    uint8_t Imm = X86.operands[X86.op_count - 1].imm & 0x3;
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
    NdVar Rounded = S.makeTemp(4);
    if (Imm == 3) {
      // Round toward zero = floor for non-negative, ceil for negative.  Using
      // floor/ceil (not float->int->float) stays correct past 2^31/2^63.
      NdVar Fl = S.makeTemp(4), Ce = S.makeTemp(4), IsNeg = S.makeTemp(1);
      S.emit(NdOp::FLOAT_FLOOR, Fl, {Lo});
      S.emit(NdOp::FLOAT_CEIL, Ce, {Lo});
      S.emit(NdOp::FLOAT_LESS, IsNeg, {Lo, NdVar::cst(0, 4)});
      S.emit(NdOp::SELECT, Rounded, {IsNeg, Ce, Fl});
    } else {
      NdOp RndOp = Imm == 1 ? NdOp::FLOAT_FLOOR
                   : Imm == 2
                       ? NdOp::FLOAT_CEIL
                       : NdOp::FLOAT_ROUNDEVEN; // imm0: nearest, ties even
      S.emit(RndOp, Rounded, {Lo});
    }
    if (Dst.Size > 4) {
      NdVar Hi = S.makeTemp(Dst.Size - 4);
      S.emit(NdOp::SUBBYTES, Hi, {PassThru, NdVar::cst(4, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Rounded});
    } else {
      S.emit(NdOp::COPY, Dst, {Rounded});
    }
    break;
  }
  // ROUNDSD/VROUNDSD — scalar double float rounding (lowest 64 bits).
  case X86_INS_ROUNDSD:
  case X86_INS_VROUNDSD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar PassThru = L.operandRead(S, X86.operands[IsVEX ? 1 : 0]);
    NdVar Src = L.operandRead(S, X86.operands[IsVEX ? 2 : 1]);
    uint8_t Imm = X86.operands[X86.op_count - 1].imm & 0x3;
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
    NdVar Rounded = S.makeTemp(8);
    if (Imm == 3) {
      // Round toward zero = floor for non-negative, ceil for negative.  Using
      // floor/ceil (not float->int->float) stays correct past 2^31/2^63.
      NdVar Fl = S.makeTemp(8), Ce = S.makeTemp(8), IsNeg = S.makeTemp(1);
      S.emit(NdOp::FLOAT_FLOOR, Fl, {Lo});
      S.emit(NdOp::FLOAT_CEIL, Ce, {Lo});
      S.emit(NdOp::FLOAT_LESS, IsNeg, {Lo, NdVar::cst(0, 8)});
      S.emit(NdOp::SELECT, Rounded, {IsNeg, Ce, Fl});
    } else {
      NdOp RndOp = Imm == 1 ? NdOp::FLOAT_FLOOR
                   : Imm == 2
                       ? NdOp::FLOAT_CEIL
                       : NdOp::FLOAT_ROUNDEVEN; // imm0: nearest, ties even
      S.emit(RndOp, Rounded, {Lo});
    }
    if (Dst.Size > 8) {
      NdVar Hi = S.makeTemp(Dst.Size - 8);
      S.emit(NdOp::SUBBYTES, Hi, {PassThru, NdVar::cst(8, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Rounded});
    } else {
      S.emit(NdOp::COPY, Dst, {Rounded});
    }
    break;
  }
  // ROUNDPS/ROUNDPD/VROUNDPS/VROUNDPD — packed float rounding (per-lane).
  case X86_INS_ROUNDPD:
  case X86_INS_ROUNDPS:
  case X86_INS_VROUNDPD:
  case X86_INS_VROUNDPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count >= 3 ? 1 : 0]);
    uint8_t Imm = X86.operands[X86.op_count - 1].imm & 0x3;
    bool IsPD = (InsnId == X86_INS_ROUNDPD || InsnId == X86_INS_VROUNDPD);
    unsigned LaneSz = IsPD ? 8 : 4;
    unsigned NLanes = Dst.Size / LaneSz;
    std::vector<NdVar> Lanes;
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar R = S.makeTemp(LaneSz);
      if (Imm == 3) {
        // Round toward zero = floor for non-negative, ceil for negative.  Using
        // floor/ceil (not float->int->float) stays correct past 2^31/2^63.
        NdVar Fl = S.makeTemp(LaneSz), Ce = S.makeTemp(LaneSz),
              IsNeg = S.makeTemp(1);
        S.emit(NdOp::FLOAT_FLOOR, Fl, {Lane});
        S.emit(NdOp::FLOAT_CEIL, Ce, {Lane});
        S.emit(NdOp::FLOAT_LESS, IsNeg, {Lane, NdVar::cst(0, LaneSz)});
        S.emit(NdOp::SELECT, R, {IsNeg, Ce, Fl});
      } else {
        NdOp RndOp = Imm == 1 ? NdOp::FLOAT_FLOOR
                     : Imm == 2
                         ? NdOp::FLOAT_CEIL
                         : NdOp::FLOAT_ROUNDEVEN; // imm0: nearest, ties even
        S.emit(RndOp, R, {Lane});
      }
      Lanes.push_back(R);
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

  // DPPS / DPPD — dot product with imm8 Lane Mask.
  // DPPS/DPPD — dot product with immediate lane mask.
  // imm8[7:4] = which src lanes to multiply, imm8[3:0] = which dst lanes get
  // result.
  case X86_INS_DPPS:
  case X86_INS_VDPPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    unsigned SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar A = L.operandRead(S, X86.operands[SrcIdx]);
    NdVar B = L.operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = (uint8_t)X86.operands[X86.op_count - 1].imm;
    std::vector<NdVar> Blocks;
    for (unsigned Block = 0; Block < Dst.Size / 16; ++Block) {
      std::vector<NdVar> Products;
      for (unsigned I = 0; I < 4; ++I) {
        if (!(Imm & (1u << (I + 4)))) {
          Products.push_back(NdVar::cst(0, 4));
          continue;
        }
        unsigned Offset = Block * 16 + I * 4;
        NdVar AI = S.makeTemp(4), BI = S.makeTemp(4);
        S.emit(NdOp::SUBBYTES, AI, {A, NdVar::cst(Offset, 4)});
        S.emit(NdOp::SUBBYTES, BI, {B, NdVar::cst(Offset, 4)});
        NdVar Prod = S.makeTemp(4);
        S.emit(NdOp::FLOAT_MULT, Prod, {AI, BI});
        Products.push_back(Prod);
      }
      NdVar Pair01 = S.makeTemp(4), Pair23 = S.makeTemp(4);
      NdVar Sum = S.makeTemp(4);
      S.emit(NdOp::FLOAT_ADD, Pair01, {Products[0], Products[1]});
      S.emit(NdOp::FLOAT_ADD, Pair23, {Products[2], Products[3]});
      S.emit(NdOp::FLOAT_ADD, Sum, {Pair01, Pair23});
      std::vector<NdVar> Lanes;
      for (unsigned I = 0; I < 4; ++I) {
        if (Imm & (1u << I))
          Lanes.push_back(Sum);
        else
          Lanes.push_back(NdVar::cst(0, 4));
      }
      NdVar Lo = S.makeTemp(8), Hi = S.makeTemp(8), Result = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Result, {Hi, Lo});
      Blocks.push_back(Result);
    }
    NdVar Result = Blocks[0];
    for (unsigned Block = 1; Block < Blocks.size(); ++Block) {
      NdVar Combined = S.makeTemp((Block + 1) * 16);
      S.emit(NdOp::CONCAT, Combined, {Blocks[Block], Result});
      Result = Combined;
    }
    S.emit(NdOp::COPY, Dst, {Result});
    break;
  }
  case X86_INS_DPPD:
  case X86_INS_VDPPD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    unsigned SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar A = L.operandRead(S, X86.operands[SrcIdx]);
    NdVar B = L.operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = (uint8_t)X86.operands[X86.op_count - 1].imm;
    std::vector<NdVar> Products;
    for (unsigned I = 0; I < 2; ++I) {
      if (!(Imm & (1u << (I + 4)))) {
        Products.push_back(NdVar::cst(0, 8));
        continue;
      }
      NdVar AI = S.makeTemp(8), BI = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, AI, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::SUBBYTES, BI, {B, NdVar::cst(I * 8, 4)});
      NdVar Prod = S.makeTemp(8);
      S.emit(NdOp::FLOAT_MULT, Prod, {AI, BI});
      Products.push_back(Prod);
    }
    NdVar Sum = S.makeTemp(8);
    S.emit(NdOp::FLOAT_ADD, Sum, {Products[0], Products[1]});
    NdVar L0 = (Imm & 1) ? Sum : NdVar::cst(0, 8);
    NdVar L1 = (Imm & 2) ? Sum : NdVar::cst(0, 8);
    S.emit(NdOp::CONCAT, Dst, {L1, L0});
    break;
  }

  // EXTRACTPS — extract float element to GPR/memory.
  case X86_INS_EXTRACTPS:
  case X86_INS_VEXTRACTPS: {
    if (X86.op_count < 3)
      break;
    // EXTRACTPS extracts a 32-bit element to r/m32.  A MEMORY destination needs
    // an explicit STORE (operandWrite yields a discarded ram(0) placeholder, so
    // the store was dropped); a 64-bit register destination zero-extends.
    const uint16_t ElemSz = 4;
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    NdVar ExtSrc = Src;
    if (X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm & 0x3;
      uint64_t ByteOff = Idx * 4;
      if (ByteOff > 0) {
        ExtSrc = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_RIGHT, ExtSrc,
               {Src, NdVar::cst(ByteOff * 8, Src.Size)});
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

  // INSERTPS — insert float element with zero mask.
  // imm8[7:6]=src_elem, imm8[5:4]=dst_elem, imm8[3:0]=zero_mask
  case X86_INS_INSERTPS:
  case X86_INS_VINSERTPS: {
    bool IsVEX = (InsnId == X86_INS_VINSERTPS);
    int MinOps = IsVEX ? 4 : 3;
    if (X86.op_count < MinOps)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Base = IsVEX ? L.operandRead(S, X86.operands[1])
                       : L.operandRead(S, X86.operands[0]);
    int SrcIdx = IsVEX ? 2 : 1;
    int ImmIdx = IsVEX ? 3 : 2;
    NdVar SrcRaw = L.operandRead(S, X86.operands[SrcIdx]);
    uint8_t Imm = 0;
    if (ImmIdx < X86.op_count && X86.operands[ImmIdx].type == X86_OP_IMM)
      Imm = static_cast<uint8_t>(X86.operands[ImmIdx].imm);
    unsigned SrcElem = (Imm >> 6) & 0x3;
    unsigned DstElem = (Imm >> 4) & 0x3;
    unsigned ZMask = Imm & 0xF;

    NdVar Elem = S.makeTemp(4);
    if (X86.operands[SrcIdx].type == X86_OP_MEM) {
      S.emit(NdOp::SUBBYTES, Elem, {SrcRaw, NdVar::cst(0, 4)});
    } else if (SrcElem > 0) {
      NdVar Shifted = S.makeTemp(SrcRaw.Size);
      S.emit(NdOp::INT_RIGHT, Shifted,
             {SrcRaw, NdVar::cst(SrcElem * 32, SrcRaw.Size)});
      S.emit(NdOp::SUBBYTES, Elem, {Shifted, NdVar::cst(0, 4)});
    } else {
      S.emit(NdOp::SUBBYTES, Elem, {SrcRaw, NdVar::cst(0, 4)});
    }

    NdVar Lanes[4];
    for (unsigned I = 0; I < 4; ++I) {
      Lanes[I] = S.makeTemp(4);
      if (ZMask & (1U << I)) {
        S.emit(NdOp::COPY, Lanes[I], {NdVar::cst(0, 4)});
      } else if (I == DstElem) {
        S.emit(NdOp::COPY, Lanes[I], {Elem});
      } else {
        S.emit(NdOp::SUBBYTES, Lanes[I], {Base, NdVar::cst(I * 4, 4)});
      }
    }
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
    NdVar Hi = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
    S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    break;
  }

  // MASKMOVDQU / MASKMOVQ — conditional Masked store to memory via RDI.
  case X86_INS_MASKMOVDQU:
  case X86_INS_MASKMOVQ:
  case X86_INS_VMASKMOVDQU: {
    if (X86.op_count < 2)
      break;
    NdVar Src = L.operandRead(S, X86.operands[0]);
    NdVar Rdi = NdVar::reg(x86reg::RDI, 8);
    S.emit(NdOp::STORE, {}, {Rdi, Src});
    break;
  }

  // MPSADBW — multiple packed sums of absolute differences.  The immediate
  // control byte selects the source block/offset and MUST be forwarded to the
  // emitter (which maps to @llvm.x86.sse41.mpsadbw); the old handler dropped it
  // and mis-read the imm as the second vector source, so the emitter (which
  // requires the imm) bailed and the result was silently 0.  SSE form is
  // `mpsadbw xmm1,xmm2/m,imm8` (xmm1 is also src1); VEX form adds an explicit
  // src1 operand.
  case X86_INS_MPSADBW:
  case X86_INS_VMPSADBW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    unsigned SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar Src = L.operandRead(S, X86.operands[SrcIdx]);
    NdVar Src2 = L.operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Intrinsic::Mpsadbw, Dst, {Src, Src2, NdVar::cst(Imm, 1)});
    break;
  }

  // PHMINPOSUW — packed horizontal unsigned word minimum + index.
  case X86_INS_PHMINPOSUW:
  case X86_INS_VPHMINPOSUW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Phminposuw, Dst, {Src});
    break;
  }

  // CLFLUSHOPT / CLWB — cache-line flush/write-back extensions.
  case X86_INS_CLFLUSHOPT:
    S.emitIntrinsic(Intrinsic::Clflushopt);
    break;
  case X86_INS_CLWB:
    S.emitIntrinsic(Intrinsic::Clwb);
    break;

  // PREFETCHW / PREFETCHWT1 — prefetch with write intent.
  case X86_INS_PREFETCHW:
  case X86_INS_PREFETCHWT1:
    S.emitIntrinsic(Intrinsic::Prefetch);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
