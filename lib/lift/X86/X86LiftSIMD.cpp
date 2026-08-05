//===- X86LiftSIMD.cpp - x86/x64 SIMD instruction lifter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SIMD/SSE/AVX/VEX/EVEX/XOP instruction handlers for x86/x64.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool X86Lifter::liftSIMD(LiftState &S, const cs_insn *Insn, const cs_x86 &X86) {
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
    NdVar Src = operandRead(S, X86.operands[1]);
    if (X86.operands[0].type == X86_OP_MEM) {
      // Memory destination (store form, e.g. `vmovdqa [mem], xmm`).  As with
      // the SSE MOV* path, operandWrite() on a memory operand yields a
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
      NdVar Dst = operandWrite(X86.operands[0]);
      if (Src.Size > Dst.Size) {
        NdVar Lo = S.makeTemp(Dst.Size);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        Src = Lo;
      }
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

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
    NdVar Dst = operandWrite(X86.operands[0]);
    uint8_t SrcCount = X86.op_count;
    NdVar A = (SrcCount >= 3) ? operandRead(S, X86.operands[1])
                                : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[SrcCount - 1]);
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
  case X86_INS_VZEROUPPER:
  case X86_INS_VZEROALL:
    // Clear Upper 128b of all YMM regs. We don't model YMM separately
    // (using XMM for both), so this is conservatively a NOP.
    S.emit(NdOp::NOP, {}, {});
    break;

  // VPCMPEQ{B,W,D,Q} — per-lane equality comparison, result is all-1s or 0.
  case X86_INS_VPCMPEQB:
  case X86_INS_VPCMPEQW:
  case X86_INS_VPCMPEQD:
  case X86_INS_VPCMPEQQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_VPCMPEQW:
      LaneSz = 2;
      break;
    case X86_INS_VPCMPEQD:
      LaneSz = 4;
      break;
    case X86_INS_VPCMPEQQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Eq = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, Eq, {La, Lb});
        NdVar Mask = S.makeTemp(LaneSz);
        uint64_t AllOnes = (LaneSz == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                         : ((1ULL << (LaneSz * 8)) - 1);
        S.emit(NdOp::SELECT, Mask,
               {Eq, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Mask;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Mask, Acc});
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

  // PMOVMSKB / VPMOVMSKB — extract MSBs of each byte → GPR bitmask.
  case X86_INS_PMOVMSKB:
  case X86_INS_VPMOVMSKB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Pmovmskb, Dst, {Src});
    break;
  }

  // SIMD saturating add/sub — per-lane with saturation.
  // Uses LLVM saturating intrinsics (@llvm.{s,u}{add,sub}.sat) to avoid
  // a constant-folding bug in the fork's optimizer that mis-computes manual
  // sext→add→icmp→select→trunc clamp chains for signed saturation.
  case X86_INS_PADDSB:
  case X86_INS_PADDSW:
  case X86_INS_PADDUSB:
  case X86_INS_PADDUSW:
  case X86_INS_PSUBSB:
  case X86_INS_PSUBSW:
  case X86_INS_PSUBUSB:
  case X86_INS_PSUBUSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned LaneSz = 1;
    bool IsSigned = false;
    bool IsSub = false;
    switch (InsnId) {
    case X86_INS_PADDSW:
    case X86_INS_PSUBSW:
    case X86_INS_PADDUSW:
    case X86_INS_PSUBUSW:
      LaneSz = 2;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_PADDSB:
    case X86_INS_PADDSW:
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
      IsSigned = true;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
    case X86_INS_PSUBUSB:
    case X86_INS_PSUBUSW:
      IsSub = true;
      break;
    default:
      break;
    }
    Intrinsic IC;
    if (IsSigned)
      IC = IsSub ? Intrinsic::X86_SsubSat : Intrinsic::X86_SaddSat;
    else
      IC = IsSub ? Intrinsic::X86_UsubSat : Intrinsic::X86_UaddSat;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {DstR, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emitIntrinsic(IC, Lr, {La, Lb});
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
    // operandWrite() returns a discarded ram(0) placeholder, so writing the
    // element into it silently dropped the store (memory left unchanged).
    // Extract into an element-sized temp and store it back; the register form
    // is unchanged (it zero-extends the element into the GPR via the ElemSz <
    // Dst.Size path).
    bool IsMem = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    if (X86.op_count >= 3 && X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm;
      uint64_t ShiftBits = Idx * ElemSz * 8;
      NdVar ExtSrc = Src;
      if (ShiftBits > 0) {
        ExtSrc = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_RIGHT, ExtSrc,
               {Src, NdVar::cst(ShiftBits, Src.Size)});
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Palignr, Dst, {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_MOVLHPS: {
    // dst[63:0] preserved, dst[127:64] = src[63:0].  Built with SUBBYTES/CONCAT:
    // a 64-bit mask constant widened to 16 bytes becomes all-ones (not a low
    // mask), which left the old high half OR'd into the new one.
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar SrcHi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, SrcHi, {Src, NdVar::cst(8, 4)});
    NdVar Hi = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Hi, {DstR, NdVar::cst(8, 4)});
    S.emit(NdOp::CONCAT, Dst, {Hi, SrcHi});
    break;
  }
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[Base]);
    NdVar Src = operandRead(S, X86.operands[Base + 1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar FalseV = operandRead(S, X86.operands[0]); // dst keeps its lane
    NdVar TrueV = operandRead(S, X86.operands[1]);  // src provides its lane
    NdVar Mask = (X86.op_count >= 3) ? operandRead(S, X86.operands[2])
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

  // SSE float compare with predicate (CMPPS/CMPPD).
  // CMPSS/CMPSD are handled by the dual-purpose dispatcher in LiftString.
  // VEX versions (VCMPPS/VCMPPD) are handled separately.
  case X86_INS_CMPPS:
  case X86_INS_CMPPD: {
    if (X86.op_count < 2)
      break;
    bool IsVEX = false;
    bool IsPD = (InsnId == X86_INS_CMPPD);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = IsVEX ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[IsVEX ? 2 : 1]);
    uint8_t Pred = 0;
    bool FoundImm = false;
    for (uint8_t N = 0; N < X86.op_count; ++N) {
      if (X86.operands[N].type == X86_OP_IMM) {
        Pred = static_cast<uint8_t>(X86.operands[N].imm) & 7;
        FoundImm = true;
      }
    }
    if (!FoundImm && Insn->size >= 1)
      Pred = Insn->bytes[Insn->size - 1] & 7;
    unsigned LaneSz = IsPD ? 8 : 4;
    unsigned NLanes = Dst.Size / LaneSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      bool Neg = false;
      switch (Pred) {
      default:
      case 0:
        S.emit(NdOp::FLOAT_EQUAL, Cmp, {La, Lb});
        break;
      case 1:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        break;
      case 2:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        break;
      case 3: { // UNORD: isNaN(a) || isNaN(b) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        break;
      }
      case 4:
        S.emit(NdOp::FLOAT_NOTEQUAL, Cmp, {La, Lb});
        break;
      case 5:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        Neg = true;
        break;
      case 6:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        Neg = true;
        break;
      case 7: { // ORD: !(isNaN(a) || isNaN(b)) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        Neg = true;
        break;
      }
      }
      if (Neg) {
        NdVar NotCmp = S.makeTemp(1);
        S.emit(NdOp::BOOL_NOT, NotCmp, {Cmp});
        Cmp = NotCmp;
      }
      NdVar Mask = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_ZEXT, Mask, {Cmp});
      NdVar AllOnes = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_NEG2, AllOnes, {Mask});
      if (I == 0) {
        Acc = AllOnes;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {AllOnes, Acc});
        Acc = Next;
      }
    }
    if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // PCMPISTRI/PCMPESTRI (index→ECX) and PCMPISTRM/PCMPESTRM (mask→XMM0) —
  // SSE4.2 string compare.  The index/mask result maps to the real LLVM
  // pcmp*str* intrinsic; CF/ZF/SF/OF come from per-flag intrinsics (the
  // comparison clears AF/PF).  Explicit-length forms read the string lengths
  // from EAX (first string) and EDX (second string).
  case X86_INS_PCMPISTRI:
  case X86_INS_PCMPESTRI:
  case X86_INS_PCMPISTRM:
  case X86_INS_PCMPESTRM: {
    if (X86.op_count < 3)
      break;
    bool IsExplicit =
        (InsnId == X86_INS_PCMPESTRI || InsnId == X86_INS_PCMPESTRM);
    bool IsIndex = (InsnId == X86_INS_PCMPISTRI || InsnId == X86_INS_PCMPESTRI);
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar Imm = NdVar::cst(X86.operands[2].imm & 0xFF, 1);
    NdVar La = NdVar::reg(x86reg::RAX, 4);
    NdVar Lb = NdVar::reg(x86reg::RDX, 4);
    NdVar Out =
        IsIndex ? NdVar::reg(x86reg::RCX, 4) : NdVar::reg(x86reg::XMM0, 16);
    Intrinsic FlagId;
    if (IsExplicit) {
      S.emitIntrinsic(IsIndex ? Intrinsic::Pcmpestri : Intrinsic::Pcmpestrm,
                      Out, {A, La, B, Lb, Imm});
      FlagId = Intrinsic::PcmpestrFlag;
    } else {
      S.emitIntrinsic(IsIndex ? Intrinsic::Pcmpistri : Intrinsic::Pcmpistrm,
                      Out, {A, B, Imm});
      FlagId = Intrinsic::PcmpistrFlag;
    }
    static constexpr struct {
      uint64_t Sel;
      uint64_t Reg;
    } StatusFlags[] = {
        {0, x86reg::CF}, {1, x86reg::ZF}, {2, x86reg::SF}, {3, x86reg::OF}};
    uint64_t ImmVal = X86.operands[2].imm & 0xFF;
    for (const auto &F : StatusFlags) {
      NdVar Bit = S.makeTemp(1);
      // Pack the control imm (bits 0-7) and the flag selector (bits 8-9) into a
      // single operand so the explicit form stays within the 6-input INTRINSIC
      // limit (A/LA/B/LB/immsel + the intrinsic code).
      NdVar ImmSel = NdVar::cst(ImmVal | (F.Sel << 8), 2);
      if (IsExplicit)
        S.emitIntrinsic(FlagId, Bit, {A, La, B, Lb, ImmSel});
      else
        S.emitIntrinsic(FlagId, Bit, {A, B, ImmSel});
      S.emit(NdOp::COPY, NdVar::reg(F.Reg, 1), {Bit});
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::AF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {NdVar::cst(0, 1)});
    break;
  }

  // SSE int<->float conversions, additional variants.
  case X86_INS_CVTDQ2PS:
  case X86_INS_CVTDQ2PD:
  case X86_INS_CVTPI2PS:
  case X86_INS_CVTPI2PD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool IsPD = (InsnId == X86_INS_CVTDQ2PD || InsnId == X86_INS_CVTPI2PD);
    unsigned IntSz = 4;
    unsigned FPSz = IsPD ? 8 : 4;
    unsigned NLanes = Src.Size / IntSz;
    if (NLanes < 2 || NLanes > 4) {
      S.emit(NdOp::FLOAT_INT2FLOAT, Dst, {Src});
      break;
    }
    if (IsPD && NLanes > 2)
      NLanes = 2;
    std::vector<NdVar> Lanes(NLanes);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(IntSz);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * IntSz, 4)});
      Lanes[I] = S.makeTemp(FPSz);
      S.emit(NdOp::FLOAT_INT2FLOAT, Lanes[I], {Elem});
    }
    if (NLanes == 2) {
      unsigned PairSz = FPSz * 2;
      NdVar Pair = S.makeTemp(PairSz);
      S.emit(NdOp::CONCAT, Pair, {Lanes[1], Lanes[0]});
      if (Dst.Size == PairSz) {
        S.emit(NdOp::COPY, Dst, {Pair});
      } else {
        NdVar ZHi = S.makeTemp(Dst.Size - PairSz);
        S.emit(NdOp::COPY, ZHi,
               {NdVar::cst(0, (uint16_t)(Dst.Size - PairSz))});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Pair});
      }
    } else {
      NdVar Lo = S.makeTemp(FPSz * 2);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      NdVar Hi = S.makeTemp(FPSz * 2);
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    }
    break;
  }
  case X86_INS_CVTPS2DQ:
  case X86_INS_CVTPD2DQ:
  case X86_INS_CVTPS2PI:
  case X86_INS_CVTPD2PI:
  case X86_INS_CVTTPS2DQ:
  case X86_INS_CVTTPD2DQ:
  case X86_INS_CVTTPS2PI:
  case X86_INS_CVTTPD2PI: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool IsPD = (InsnId == X86_INS_CVTPD2DQ || InsnId == X86_INS_CVTTPD2DQ ||
                 InsnId == X86_INS_CVTPD2PI || InsnId == X86_INS_CVTTPD2PI);
    // The non-T variants round using MXCSR (default: nearest, ties to even);
    // only the CVTT* variants truncate toward zero.
    bool IsTrunc =
        (InsnId == X86_INS_CVTTPS2DQ || InsnId == X86_INS_CVTTPD2DQ ||
         InsnId == X86_INS_CVTTPS2PI || InsnId == X86_INS_CVTTPD2PI);
    unsigned FPSz = IsPD ? 8 : 4;
    unsigned DstElemSz = 4;
    unsigned NLanes = Src.Size / FPSz;
    unsigned DstLanes = Dst.Size / DstElemSz;
    if (NLanes < 2 || NLanes > 4) {
      S.emit(NdOp::FLOAT_TRUNC, Dst, {Src});
      break;
    }
    if (NLanes > DstLanes)
      NLanes = DstLanes;
    std::vector<NdVar> Lanes(NLanes);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Elem = S.makeTemp(FPSz);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * FPSz, 4)});
      Lanes[I] = S.makeTemp(DstElemSz);
      if (IsTrunc) {
        S.emit(NdOp::FLOAT_TRUNC, Lanes[I], {Elem});
      } else {
        NdVar Rnd = S.makeTemp(FPSz);
        S.emit(NdOp::FLOAT_ROUNDEVEN, Rnd, {Elem});
        S.emit(NdOp::FLOAT_FLOAT2INT, Lanes[I], {Rnd});
      }
    }
    if (NLanes == 2) {
      if (Dst.Size == 8) {
        S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
      } else {
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
        NdVar ZHi = S.makeTemp(8);
        S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, 8)});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Lo});
      }
    } else {
      NdVar Lo = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
      NdVar Hi = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
      S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
    }
    break;
  }
  // Packed FP width conversions (scalar CVTSS2SD/CVTSD2SS are handled in
  // liftCore, which runs first).  Each lane must carry its true FP width so
  // the emitter picks fpext/fptrunc correctly.
  case X86_INS_CVTPS2PD: {
    // Low two single-precision lanes -> two double-precision lanes.
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    std::vector<NdVar> Lanes(2);
    for (unsigned I = 0; I < 2; ++I) {
      NdVar Elem = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 4, 4)});
      Lanes[I] = S.makeTemp(8);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Lanes[I], {Elem});
    }
    S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
    break;
  }
  case X86_INS_CVTPD2PS: {
    // Two double-precision lanes -> two single-precision lanes (low 64 bits);
    // the upper 64 bits of the destination are zeroed.
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    std::vector<NdVar> Lanes(2);
    for (unsigned I = 0; I < 2; ++I) {
      NdVar Elem = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(I * 8, 4)});
      Lanes[I] = S.makeTemp(4);
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Lanes[I], {Elem});
    }
    NdVar Lo = S.makeTemp(8);
    S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
    if (Dst.Size > 8) {
      NdVar ZHi = S.makeTemp(Dst.Size - 8);
      S.emit(NdOp::COPY, ZHi, {NdVar::cst(0, (uint16_t)(Dst.Size - 8))});
      S.emit(NdOp::CONCAT, Dst, {ZHi, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Lo});
    }
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint16_t ES = (InsnId == X86_INS_VPERMILPD) ? 8 : 4; // element size
    uint16_t EPL = 16 / ES;                              // elems per 128 lane
    uint16_t N = Dst.Size / ES;                          // total elements
    if (N < 2 || N > 8) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    bool IsImm = (X86.operands[2].type == X86_OP_IMM);
    uint64_t Imm = IsImm ? static_cast<uint64_t>(X86.operands[2].imm) : 0;
    NdVar Ctrl = IsImm ? NdVar() : operandRead(S, X86.operands[2]);
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
        NdVar Dst = operandWrite(X86.operands[0]);
        NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Idx = operandRead(S, X86.operands[1]);
    NdVar Src = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // HADDPS/HADDPD/HSUBPS/HSUBPD — horizontal pair-wise add/sub.
  // HADDPS: Dst[0]=A[0]+A[1], Dst[1]=A[2]+A[3], Dst[2]=B[0]+B[1],
  // Dst[3]=B[2]+B[3] HADDPD: Dst[0]=A[0]+A[1], Dst[1]=B[0]+B[1]
  case X86_INS_VHADDPS:
  case X86_INS_VHSUBPS:
  case X86_INS_HADDPS:
  case X86_INS_HSUBPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_VHSUBPS || InsnId == X86_INS_HSUBPS);
    NdOp Opc = IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    // Horizontal pair-add/sub is per-128-bit lane: each result lane's low 64
    // bits are src1's two pair reductions, its high 64 bits src2's (for the
    // SAME lane).  The old code only read the low 128 bits, so the 256-bit ymm
    // (VEX) form computed garbage for the high lane.  Build lane-by-lane.
    unsigned NumLanes = Dst.Size >= 16 ? Dst.Size / 16 : 1;
    auto laneF = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar E = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(Off, 4)});
      return E;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      unsigned Base = L * 16;
      NdVar R0 = S.makeTemp(4), R1 = S.makeTemp(4), R2 = S.makeTemp(4),
              R3 = S.makeTemp(4);
      S.emit(Opc, R0, {laneF(A, Base + 0), laneF(A, Base + 4)});
      S.emit(Opc, R1, {laneF(A, Base + 8), laneF(A, Base + 12)});
      S.emit(Opc, R2, {laneF(B, Base + 0), laneF(B, Base + 4)});
      S.emit(Opc, R3, {laneF(B, Base + 8), laneF(B, Base + 12)});
      NdVar Lo = S.makeTemp(8), Hi = S.makeTemp(8), Lane = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Lo, {R1, R0});
      S.emit(NdOp::CONCAT, Hi, {R3, R2});
      S.emit(NdOp::CONCAT, Lane, {Hi, Lo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * 16);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_VHADDPD:
  case X86_INS_VHSUBPD:
  case X86_INS_HADDPD:
  case X86_INS_HSUBPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_VHSUBPD || InsnId == X86_INS_HSUBPD);
    NdOp Opc = IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    // Per-128-bit lane (each lane holds two doubles): result lane low = src1's
    // pair sum, high = src2's pair sum.  The old code handled only the low 128
    // bits; rebuild lane-by-lane so the 256-bit ymm form is correct.
    unsigned NumLanes = Dst.Size >= 16 ? Dst.Size / 16 : 1;
    auto laneD = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar E = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(Off, 4)});
      return E;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      unsigned Base = L * 16;
      NdVar R0 = S.makeTemp(8), R1 = S.makeTemp(8), Lane = S.makeTemp(16);
      S.emit(Opc, R0, {laneD(A, Base + 0), laneD(A, Base + 8)});
      S.emit(Opc, R1, {laneD(B, Base + 0), laneD(B, Base + 8)});
      S.emit(NdOp::CONCAT, Lane, {R1, R0});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * 16);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // ADDSUBPS/ADDSUBPD — alternating sub/add per lane.
  // PS: Dst[0]=A[0]-B[0], Dst[1]=A[1]+B[1], Dst[2]=A[2]-B[2], Dst[3]=A[3]+B[3]
  // PD: Dst[0]=A[0]-B[0], Dst[1]=A[1]+B[1]
  case X86_INS_VADDSUBPS:
  case X86_INS_ADDSUBPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    // Even single lanes subtract, odd lanes add — a uniform per-element pattern
    // across the whole register.  The old code only did the low 4 floats, so
    // the 256-bit ymm (VEX) form left the high 128 bits uncomputed.
    unsigned NElems = Dst.Size / 4;
    NdVar Full = S.makeTemp(0);
    for (unsigned I = 0; I < NElems; ++I) {
      NdVar Ae = S.makeTemp(4), Be = S.makeTemp(4), R = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Ae, {A, NdVar::cst(I * 4, 4)});
      S.emit(NdOp::SUBBYTES, Be, {B, NdVar::cst(I * 4, 4)});
      S.emit((I & 1) ? NdOp::FLOAT_ADD : NdOp::FLOAT_SUB, R, {Ae, Be});
      if (I == 0) {
        Full = R;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 4);
        S.emit(NdOp::CONCAT, Next, {R, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_VADDSUBPD:
  case X86_INS_ADDSUBPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    // Even double lanes subtract, odd lanes add; uniform per-element across the
    // whole register (the old code only did the low 2 doubles → ymm truncated).
    unsigned NElems = Dst.Size / 8;
    NdVar Full = S.makeTemp(0);
    for (unsigned I = 0; I < NElems; ++I) {
      NdVar Ae = S.makeTemp(8), Be = S.makeTemp(8), R = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Ae, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::SUBBYTES, Be, {B, NdVar::cst(I * 8, 4)});
      S.emit((I & 1) ? NdOp::FLOAT_ADD : NdOp::FLOAT_SUB, R, {Ae, Be});
      if (I == 0) {
        Full = R;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 8);
        S.emit(NdOp::CONCAT, Next, {R, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PAVG{B,W} — packed average bytes/words.
  case X86_INS_PAVGB:
  case X86_INS_PAVGW:
  case X86_INS_VPAVGB:
  case X86_INS_VPAVGW: {
    // Packed unsigned rounding average per lane: dst[i] = (a[i]+b[i]+1) >> 1.
    // The old code emitted a single full-width INT_ADD — no divide, no rounding
    // and not lane-isolated (carries crossed byte/word boundaries).
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz =
        (InsnId == X86_INS_PAVGB || InsnId == X86_INS_VPAVGB) ? 1 : 2;
    unsigned WideSz = LaneSz * 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(Off, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ZEXT, Aw, {Al});
        S.emit(NdOp::INT_ZEXT, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        NdVar Sum1 = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(NdOp::INT_RIGHT, Sh, {Sum1, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
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

  // Intel TSX (transactional memory) instructions.
  case X86_INS_XBEGIN:
  case X86_INS_XEND:
  case X86_INS_XABORT:
  case X86_INS_XTEST:
  case X86_INS_XACQUIRE:
  case X86_INS_XRELEASE: {
    if (InsnId == X86_INS_XABORT) {
      // xabort imm8 — capture the immediate so codegen can re-emit
      // `xabort $imm` (a bare `xabort` is rejected: too few operands).
      uint64_t Imm = (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM)
                         ? static_cast<uint64_t>(X86.operands[0].imm)
                         : 0;
      S.emitIntrinsic(Intrinsic::Xabort, NdVar::reg(x86reg::RAX, 8),
                      {NdVar::cst(Imm & 0xFF, 1)});
      break;
    }
    Intrinsic Id;
    switch (InsnId) {
    case X86_INS_XBEGIN:
      Id = Intrinsic::Xbegin;
      break;
    case X86_INS_XEND:
      Id = Intrinsic::Xend;
      break;
    case X86_INS_XTEST:
      Id = Intrinsic::Xtest;
      break;
    case X86_INS_XACQUIRE:
      Id = Intrinsic::Xacquire;
      break;
    case X86_INS_XRELEASE:
      Id = Intrinsic::Xrelease;
      break;
    default:
      Id = Intrinsic::Xbegin;
      break;
    }
    S.emitIntrinsic(Id);
    break;
  }

  // BNDxxx — MPX bounds (deprecated). Treat as side-effect or COPY.
  case X86_INS_BNDMK:
  case X86_INS_BNDMOV:
  case X86_INS_BNDCL:
  case X86_INS_BNDCU:
  case X86_INS_BNDCN:
  case X86_INS_BNDLDX:
  case X86_INS_BNDSTX: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // PMULLD/PMULLW — packed integer multiply (low result), per-lane.
  case X86_INS_PMULLD:
  case X86_INS_PMULLW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned LaneSz = (InsnId == X86_INS_PMULLD) ? 4 : 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {DstR, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
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
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  // PMULDQ/PMULUDQ — per-lane widening multiply.  Each 64-bit output lane is
  // the product of the *even* (low) dword of that lane from dst and src,
  // sign-extended (PMULDQ) or zero-extended (PMULUDQ) to 64 bits.  A full
  // i128 INT_MULT (the old behaviour) is semantically wrong: it propagates
  // carries across the dword/qword lanes.
  case X86_INS_PMULDQ:
  case X86_INS_PMULUDQ: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool IsSigned = (InsnId == X86_INS_PMULDQ);
    unsigned NLanes = Dst.Size / 8; // 64-bit output lanes (2=XMM, 4=YMM)
    if (NLanes == 0) {
      S.emit(NdOp::INT_MULT, Dst, {DstR, Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned Off = I * 8; // even dword sits at the base of each qword lane
      NdVar Ad = S.makeTemp(4), Bd = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Ad, {DstR, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Bd, {Src, NdVar::cst(Off, 4)});
      NdVar Aq = S.makeTemp(8), Bq = S.makeTemp(8);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Aq, {Ad});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bq, {Bd});
      NdVar Prod = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Prod, {Aq, Bq});
      if (I == 0) {
        Acc = Prod;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 8);
        S.emit(NdOp::CONCAT, Next, {Prod, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // PMULHW/PMULHUW: per-lane word multiply, return high 16-bit result.
  case X86_INS_PMULHW:
  case X86_INS_PMULHUW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool IsSigned = (InsnId == X86_INS_PMULHW);
    unsigned HalfSz = Dst.Size / 2;
    unsigned WordsPerHalf = HalfSz / 2;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < WordsPerHalf; ++I) {
        unsigned Off = BaseOff + I * 2;
        NdVar Aw = S.makeTemp(2), Bw = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Aw, {DstR, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bw, {Src, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(4), Bx = S.makeTemp(4);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ax, {Aw});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bx, {Bw});
        NdVar Prod = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Prod, {Ax, Bx});
        NdVar Hi = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Hi, {Prod, NdVar::cst(2, 4)});
        if (I == 0) {
          Acc = Hi;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Hi, Acc});
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

  // PCMPEQQ / VPCMPEQQ — per-lane qword equal compare (SSE4.1 / AVX).
  case X86_INS_PCMPEQQ:
  case X86_INS_PCMPGTQ: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    unsigned LaneSz = 8;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {DstR, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Cmp = S.makeTemp(1);
        if (InsnId == X86_INS_PCMPEQQ)
          S.emit(NdOp::INT_EQUAL, Cmp, {La, Lb});
        else
          S.emit(NdOp::INT_SLESS, Cmp, {Lb, La});
        NdVar AllOnes = NdVar::cst(~uint64_t(0), LaneSz);
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Lane, {Cmp, AllOnes, NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Lane;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lane, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Cmp = S.makeTemp(1);
      if (InsnId == X86_INS_PCMPEQQ)
        S.emit(NdOp::INT_EQUAL, Cmp, {DstR, Src});
      else
        S.emit(NdOp::INT_SLESS, Cmp, {Src, DstR});
      S.emit(NdOp::SELECT, Dst,
             {Cmp, NdVar::cst(~uint64_t(0), Dst.Size),
              NdVar::cst(0, Dst.Size)});
    }
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
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

  // VEX 3-operand shuffles: VPSHUFB xmm1, xmm2, xmm3 → pshufb(xmm2, xmm3)
  case X86_INS_VPSHUFB: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Pshufb, Dst, {A, B});
    break;
  }
  case X86_INS_VPSHUFD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufd, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPSHUFLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshuflw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPSHUFHW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Pshufhw, Dst, {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VSHUFPS: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Shufps, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VSHUFPD: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Shufpd, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPALIGNR: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Palignr, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  // VEX 3-operand unpacks
  case X86_INS_VPUNPCKLBW: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpcklbw, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHBW: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhbw, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKLWD: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpcklwd, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHWD: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhwd, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKLDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckldq, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhdq, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKLQDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpcklqdq, D, {A, B});
    break;
  }
  case X86_INS_VPUNPCKHQDQ: {
    if (X86.op_count < 3)
      break;
    NdVar D = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Punpckhqdq, D, {A, B});
    break;
  }
  case X86_INS_VBROADCASTSS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastSS, Dst, {Src});
    break;
  }
  case X86_INS_VBROADCASTSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastSD, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastB, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastW, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastD, Dst, {Src});
    break;
  }
  case X86_INS_VPBROADCASTQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::BroadcastQ, Dst, {Src});
    break;
  }
  // VPBLENDD ymm1, ymm2, ymm3/m, imm8 — per-dword select: imm8[i] picks src2
  // (operands[2]) when set, else src1 (operands[1]).  Lift natively per dword
  // (SUBBYTES + CONCAT) so the 256-bit form roundtrips without an opaque
  // INTRINSIC; 128-bit uses the low 4 imm8 bits.
  case X86_INS_VPBLENDD: {
    if (X86.op_count < 4)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[3].imm);
    S.emitIntrinsic(Intrinsic::Perm2f128, Dst, {A, B, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_VPERMD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Idx = operandRead(S, X86.operands[1]);
    NdVar Src = operandRead(S, X86.operands[2]);
    S.emitIntrinsic(Intrinsic::Permd, Dst, {Idx, Src});
    break;
  }
  case X86_INS_VINSERTF128:
  case X86_INS_VINSERTI128: {
    // VINSERTF128 ymm1, ymm2, xmm3/m128, imm8
    // inserts 128-bit Src into the Lane selected by imm8[0]
    if (X86.op_count < 4)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar YmmSrc = operandRead(S, X86.operands[1]);
    NdVar XmmSrc = operandRead(S, X86.operands[2]);
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
    // operandWrite() of a mem operand yields a discarded ram(0) placeholder, so
    // emitting the extracted lane straight into it silently dropped the
    // write-back for `vextractf128/vextracti128 [mem],ymm,imm` (lane computed,
    // memory left unchanged) — the same class of bug already fixed for
    // PEXTR*/EXTRACTPS and the MOVHPS/MOVLPS partial stores.  Extract into a
    // temp and store it.
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Dst = MemDst ? S.makeTemp(16) : operandWrite(X86.operands[0]);
    NdVar YmmSrc = operandRead(S, X86.operands[1]);
    uint8_t Lane = static_cast<uint8_t>(X86.operands[2].imm) & 1;
    S.emit(NdOp::SUBBYTES, Dst, {YmmSrc, NdVar::cst(Lane * 16, 1)});
    if (MemDst)
      S.storeToMem(X86.operands[0], Dst);
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
      // operandRead() on a MEM operand would emit a LOAD and return the loaded
      // value, so the masked store wrote to the value-at-the-destination
      // instead of the destination itself.  Use computeEA for the address.
      NdVar AddrVn = S.computeEA(X86.operands[0]);
      NdVar MaskVn = operandRead(S, X86.operands[1]);
      NdVar DataVn = operandRead(S, X86.operands[2]);
      Intrinsic IId =
          IsQword ? Intrinsic::MaskedStoreQ : Intrinsic::MaskedStoreD;
      S.emitIntrinsic(IId, NdVar::reg(x86reg::RAX, 0),
                      {AddrVn, MaskVn, DataVn});
    } else {
      NdVar Dst = operandWrite(X86.operands[0]);
      NdVar MaskVn = operandRead(S, X86.operands[1]);
      // Likewise the emitter loads through input[1] as an ADDRESS; passing
      // operandRead() (the loaded value) caused a double dereference.
      NdVar AddrVn = S.computeEA(X86.operands[2]);
      Intrinsic IId = IsQword ? Intrinsic::MaskedLoadQ : Intrinsic::MaskedLoadD;
      S.emitIntrinsic(IId, Dst, {AddrVn, MaskVn});
    }
    break;
  }

  // PMOVZX/PMOVSX — packed move with per-element zero/sign extension.
  // Each of the N low source elements (byte/word/dword) is independently
  // extended to the wider destination element.  A single whole-register
  // INT_ZEXT/INT_SEXT is wrong: it would treat the packed source as one
  // scalar and only fill lane 0 (and corrupt the rest).
  case X86_INS_PMOVZXBW:
  case X86_INS_PMOVZXBD:
  case X86_INS_PMOVZXBQ:
  case X86_INS_PMOVZXWD:
  case X86_INS_PMOVZXWQ:
  case X86_INS_PMOVZXDQ:
  case X86_INS_VPMOVZXBW:
  case X86_INS_VPMOVZXBD:
  case X86_INS_VPMOVZXBQ:
  case X86_INS_VPMOVZXWD:
  case X86_INS_VPMOVZXWQ:
  case X86_INS_VPMOVZXDQ:
  case X86_INS_PMOVSXBW:
  case X86_INS_PMOVSXBD:
  case X86_INS_PMOVSXBQ:
  case X86_INS_PMOVSXWD:
  case X86_INS_PMOVSXWQ:
  case X86_INS_PMOVSXDQ:
  case X86_INS_VPMOVSXBW:
  case X86_INS_VPMOVSXBD:
  case X86_INS_VPMOVSXBQ:
  case X86_INS_VPMOVSXWD:
  case X86_INS_VPMOVSXWQ:
  case X86_INS_VPMOVSXDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint16_t SrcElemSz = 1, DstElemSz = 2;
    bool IsSigned = false;
    switch (InsnId) {
    case X86_INS_PMOVZXBW:
    case X86_INS_VPMOVZXBW:
      SrcElemSz = 1;
      DstElemSz = 2;
      break;
    case X86_INS_PMOVZXBD:
    case X86_INS_VPMOVZXBD:
      SrcElemSz = 1;
      DstElemSz = 4;
      break;
    case X86_INS_PMOVZXBQ:
    case X86_INS_VPMOVZXBQ:
      SrcElemSz = 1;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVZXWD:
    case X86_INS_VPMOVZXWD:
      SrcElemSz = 2;
      DstElemSz = 4;
      break;
    case X86_INS_PMOVZXWQ:
    case X86_INS_VPMOVZXWQ:
      SrcElemSz = 2;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVZXDQ:
    case X86_INS_VPMOVZXDQ:
      SrcElemSz = 4;
      DstElemSz = 8;
      break;
    case X86_INS_PMOVSXBW:
    case X86_INS_VPMOVSXBW:
      SrcElemSz = 1;
      DstElemSz = 2;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXBD:
    case X86_INS_VPMOVSXBD:
      SrcElemSz = 1;
      DstElemSz = 4;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXBQ:
    case X86_INS_VPMOVSXBQ:
      SrcElemSz = 1;
      DstElemSz = 8;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXWD:
    case X86_INS_VPMOVSXWD:
      SrcElemSz = 2;
      DstElemSz = 4;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXWQ:
    case X86_INS_VPMOVSXWQ:
      SrcElemSz = 2;
      DstElemSz = 8;
      IsSigned = true;
      break;
    case X86_INS_PMOVSXDQ:
    case X86_INS_VPMOVSXDQ:
      SrcElemSz = 4;
      DstElemSz = 8;
      IsSigned = true;
      break;
    default:
      break;
    }
    unsigned NLanes = DstElemSz ? (Dst.Size / DstElemSz) : 0;
    if (NLanes == 0) {
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar El = S.makeTemp(SrcElemSz);
      S.emit(NdOp::SUBBYTES, El, {Src, NdVar::cst(I * SrcElemSz, 4)});
      NdVar Ext = S.makeTemp(DstElemSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ext, {El});
      if (I == 0) {
        Acc = Ext;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstElemSz);
        S.emit(NdOp::CONCAT, Next, {Ext, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
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
    NdVar Dst = IsMem ? S.makeTemp(ElemSz) : operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar ExtSrc = Src;
    if (X86.op_count >= 3 && X86.operands[2].type == X86_OP_IMM) {
      uint64_t Idx = X86.operands[2].imm;
      uint64_t ShiftBits = Idx * ElemSz * 8;
      if (ShiftBits > 0) {
        ExtSrc = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_RIGHT, ExtSrc,
               {Src, NdVar::cst(ShiftBits, Src.Size)});
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
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVEX = (X86.op_count >= 4);
    NdVar Base = IsVEX ? operandRead(S, X86.operands[1])
                         : operandRead(S, X86.operands[0]);
    NdVar Src = IsVEX ? operandRead(S, X86.operands[2])
                        : operandRead(S, X86.operands[1]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar FalseV = operandRead(S, X86.operands[1]); // src1
    NdVar TrueV = operandRead(S, X86.operands[2]);  // src2
    NdVar Mask = operandRead(S, X86.operands[3]);
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

  case X86_INS_VBROADCASTF128:
  case X86_INS_VBROADCASTI128:
  case X86_INS_VBLENDMPS:
  case X86_INS_VBLENDMPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    if (InsnId == X86_INS_VPMULLQ) {
      NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Src;
      S.emit(NdOp::INT_MULT, Dst, {A, Src});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // VCMPxx / VCOMISS / VCOMISD / VUCOMISS / VUCOMISD — vector/scalar fp
  // compare. Capstone has both the generic VCMP* (with predicate as
  // immediate operand) AND pseudo-mnemonic VCMP ids (capstone renders
  // these with a suffix like "vcmpgt_oqps" but the underlying ID is the
  // catch-all X86_INS_VCMP). We include all variants.
  case X86_INS_VCOMISS:
  case X86_INS_VCOMISD:
  case X86_INS_VUCOMISS:
  case X86_INS_VUCOMISD: {
    if (X86.op_count < 2)
      break;
    NdVar Lhs = operandRead(S, X86.operands[0]);
    NdVar Rhs = operandRead(S, X86.operands[X86.op_count - 1]);
    // VCOMISS/VUCOMISS compare the low single (4B), the *SD forms the low
    // double (8B).  operandRead returns the whole XMM for a register operand;
    // extract the scalar so the float emitter infers the right precision (a *SS
    // compare left 16B wide reads 8 bytes as a double and inverts the sign
    // test).
    unsigned ScalarSz =
        (InsnId == X86_INS_VCOMISS || InsnId == X86_INS_VUCOMISS) ? 4 : 8;
    if (Lhs.Size > ScalarSz) {
      NdVar T = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, T, {Lhs, NdVar::cst(0, 4)});
      Lhs = T;
    }
    if (Rhs.Size > ScalarSz) {
      NdVar T = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, T, {Rhs, NdVar::cst(0, 4)});
      Rhs = T;
    }
    // An unordered (NaN) compare sets ZF=PF=CF=1.  Bare ordered relations leave
    // ZF/CF 0 on NaN, which makes the SETA/SETAE ordered idioms (!CF&&!ZF /
    // !CF) wrongly read true, so OR the unordered predicate into ZF and CF.
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {Lhs});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {Rhs});
    NdVar Unord = S.makeTemp(1);
    S.emit(NdOp::BOOL_OR, Unord, {NanA, NanB});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::FLOAT_EQUAL, Eq, {Lhs, Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::ZF, 1), {Eq, Unord});
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {Lhs, Rhs});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {Lt, Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {Unord});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::cst(0, 1)});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::cst(0, 1)});
    break;
  }
  // VEX FP compare (3-operand: dst, src1, src2 [, imm]).  The predicate lives
  // in the trailing immediate byte; for non-NaN inputs predicate & 7 selects
  // the relation (the 32 AVX predicates are four NaN/signaling variants of the
  // same eight relations).  Packed forms fill the result per lane; scalar forms
  // compare lane 0 and keep src1's upper lanes (VEX scalar semantics).
  case X86_INS_VCMP:
  case X86_INS_VCMPSS:
  case X86_INS_VCMPSD:
  case X86_INS_VCMPPS:
  case X86_INS_VCMPPD: {
    if (X86.op_count < 3)
      break;
    bool IsWide = (InsnId == X86_INS_VCMPPD || InsnId == X86_INS_VCMPSD);
    bool IsScalar = (InsnId == X86_INS_VCMPSS || InsnId == X86_INS_VCMPSD);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    uint8_t Pred = 0;
    bool FoundImm = false;
    int SrcIdx = X86.op_count - 1;
    for (uint8_t N = 0; N < X86.op_count; ++N)
      if (X86.operands[N].type == X86_OP_IMM) {
        Pred = static_cast<uint8_t>(X86.operands[N].imm) & 7;
        FoundImm = true;
      }
    if (FoundImm)
      SrcIdx = X86.op_count - 2;
    else if (Insn->size >= 1)
      Pred = Insn->bytes[Insn->size - 1] & 7;
    NdVar B = operandRead(S, X86.operands[SrcIdx]);
    unsigned LaneSz = IsWide ? 8 : 4;
    unsigned NLanes = IsScalar ? 1 : (Dst.Size / LaneSz);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      bool Neg = false;
      switch (Pred) {
      default:
      case 0:
        S.emit(NdOp::FLOAT_EQUAL, Cmp, {La, Lb});
        break;
      case 1:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        break;
      case 2:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        break;
      case 3: { // UNORD: isNaN(a) || isNaN(b) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        break;
      }
      case 4:
        S.emit(NdOp::FLOAT_NOTEQUAL, Cmp, {La, Lb});
        break;
      case 5:
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
        Neg = true;
        break;
      case 6:
        S.emit(NdOp::FLOAT_LESSEQUAL, Cmp, {La, Lb});
        Neg = true;
        break;
      case 7: { // ORD: !(isNaN(a) || isNaN(b)) — must inspect BOTH operands
        NdVar NanA = S.makeTemp(1), NanB = S.makeTemp(1);
        S.emit(NdOp::FLOAT_ISNAN, NanA, {La});
        S.emit(NdOp::FLOAT_ISNAN, NanB, {Lb});
        S.emit(NdOp::BOOL_OR, Cmp, {NanA, NanB});
        Neg = true;
        break;
      }
      }
      if (Neg) {
        NdVar NotCmp = S.makeTemp(1);
        S.emit(NdOp::BOOL_NOT, NotCmp, {Cmp});
        Cmp = NotCmp;
      }
      NdVar Mask = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_ZEXT, Mask, {Cmp});
      NdVar AllOnes = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_NEG2, AllOnes, {Mask});
      if (I == 0) {
        Acc = AllOnes;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {AllOnes, Acc});
        Acc = Next;
      }
    }
    if (IsScalar && Dst.Size > LaneSz) {
      NdVar Hi = S.makeTemp(Dst.Size - LaneSz);
      S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(LaneSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Acc});
    } else if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // Generic VPCMP with predicate (AVX-512). Capstone 6 maps predicate-
  // suffixed vpcmp* mnemonics to X86_INS_VPCMP /
  // X86_INS_VPCMPB/D/Q/W/UB/UD/UQ/UW.
  case X86_INS_VPCMP:
  case X86_INS_VPCMPB:
  case X86_INS_VPCMPD:
  case X86_INS_VPCMPQ:
  case X86_INS_VPCMPW:
  case X86_INS_VPCMPUB:
  case X86_INS_VPCMPUD:
  case X86_INS_VPCMPUQ:
  case X86_INS_VPCMPUW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B =
        operandRead(S, X86.operands[X86.op_count >= 4 ? 2 : X86.op_count - 1]);
    S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    break;
  }

  // PTEST / VPTEST — set ZF/CF based on bit test.
  case X86_INS_PTEST:
  case X86_INS_VPTEST: {
    if (X86.op_count < 2)
      break;
    NdVar A = operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[1]);
    NdVar Masked = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, Masked, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Masked, NdVar::cst(0, A.Size)});
    NdVar AndNot = S.makeTemp(A.Size);
    NdVar NegA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, NegA, {A});
    S.emit(NdOp::INT_AND, AndNot, {NegA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndNot, NdVar::cst(0, A.Size)});
    break;
  }

  // MOVMSKPS/MOVMSKPD/VMOVMSKPS/VMOVMSKPD — extract sign bits per lane.
  case X86_INS_MOVMSKPS:
  case X86_INS_MOVMSKPD:
  case X86_INS_VMOVMSKPS:
  case X86_INS_VMOVMSKPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    bool IsPS = (InsnId == X86_INS_MOVMSKPS || InsnId == X86_INS_VMOVMSKPS);
    unsigned LaneSz = IsPS ? 4 : 8;
    unsigned NLanes = Src.Size / LaneSz;
    unsigned SignBit = LaneSz * 8 - 1;
    NdVar Accum = NdVar::cst(0, Dst.Size);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar Sign = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_RIGHT, Sign, {Lane, NdVar::cst(SignBit, LaneSz)});
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

  // SQRTSS/SQRTSD/SQRTPS/SQRTPD — float square root.
  case X86_INS_SQRTSS:
  case X86_INS_SQRTSD:
  case X86_INS_SQRTPS:
  case X86_INS_SQRTPD:
  case X86_INS_RSQRTSS:
  case X86_INS_RSQRTPS:
  case X86_INS_RCPSS:
  case X86_INS_RCPPS:
  case X86_INS_VSQRTSS:
  case X86_INS_VSQRTSD:
  case X86_INS_VSQRTPS:
  case X86_INS_VSQRTPD:
  case X86_INS_VRSQRTSS:
  case X86_INS_VRSQRTPS:
  case X86_INS_VRCPSS:
  case X86_INS_VRCPPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsPacked = false;
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_SQRTPS:
    case X86_INS_VSQRTPS:
    case X86_INS_RSQRTPS:
    case X86_INS_VRSQRTPS:
    case X86_INS_RCPPS:
    case X86_INS_VRCPPS:
      IsPacked = true;
      LaneSz = 4;
      break;
    case X86_INS_SQRTPD:
    case X86_INS_VSQRTPD:
      IsPacked = true;
      LaneSz = 8;
      break;
    default:
      break;
    }

    bool IsRcp = (InsnId == X86_INS_RCPPS || InsnId == X86_INS_RCPSS ||
                  InsnId == X86_INS_VRCPPS || InsnId == X86_INS_VRCPSS);
    bool IsRsqrt = (InsnId == X86_INS_RSQRTPS || InsnId == X86_INS_RSQRTSS ||
                    InsnId == X86_INS_VRSQRTPS || InsnId == X86_INS_VRSQRTSS);

    auto emitLaneOp = [&](NdVar In, NdVar Out) {
      if (IsRcp) {
        NdVar One = NdVar::cst(0x3F800000, In.Size); // 1.0f
        S.emit(NdOp::FLOAT_DIV, Out, {One, In});
      } else if (IsRsqrt) {
        NdVar Sq = S.makeTemp(In.Size);
        S.emit(NdOp::FLOAT_SQRT, Sq, {In});
        NdVar One = NdVar::cst(0x3F800000, In.Size); // 1.0f
        S.emit(NdOp::FLOAT_DIV, Out, {One, Sq});
      } else {
        S.emit(NdOp::FLOAT_SQRT, Out, {In});
      }
    };

    if (IsPacked && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Res = S.makeTemp(LaneSz);
        emitLaneOp(Lane, Res);
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (Dst.Size > 8) {
      // Scalar form (SQRTSS/SD, RSQRTSS, RCPSS): operate on the low element
      // only and keep the upper lanes.  Extracting the scalar — instead of
      // handing the whole 16B XMM to the float emitter, which infers double
      // from the width — is what makes a *SS op single-precision (else SQRTSS
      // became sqrt.f64).
      unsigned ScalarSz =
          (InsnId == X86_INS_SQRTSD || InsnId == X86_INS_VSQRTSD) ? 8 : 4;
      NdVar In = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, In, {Src, NdVar::cst(0, 4)});
      NdVar Res = S.makeTemp(ScalarSz);
      emitLaneOp(In, Res);
      // VEX scalar takes the upper lanes from src1 (operand 1); SSE keeps
      // Dst's.
      NdVar Upper =
          (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
      NdVar Hi = S.makeTemp(Dst.Size - ScalarSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(ScalarSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Res});
    } else {
      emitLaneOp(Src, Dst);
    }
    break;
  }

  // Packed min/max — per-lane comparison + select.
  case X86_INS_PMINSB:
  case X86_INS_PMINSW:
  case X86_INS_PMINSD:
  case X86_INS_PMINUB:
  case X86_INS_PMINUW:
  case X86_INS_PMINUD:
  case X86_INS_PMAXSB:
  case X86_INS_PMAXSW:
  case X86_INS_PMAXSD:
  case X86_INS_PMAXUB:
  case X86_INS_PMAXUW:
  case X86_INS_PMAXUD:
  case X86_INS_VPMINSB:
  case X86_INS_VPMINSW:
  case X86_INS_VPMINSD:
  case X86_INS_VPMINSQ:
  case X86_INS_VPMINUB:
  case X86_INS_VPMINUW:
  case X86_INS_VPMINUD:
  case X86_INS_VPMINUQ:
  case X86_INS_VPMAXSB:
  case X86_INS_VPMAXSW:
  case X86_INS_VPMAXSD:
  case X86_INS_VPMAXSQ:
  case X86_INS_VPMAXUB:
  case X86_INS_VPMAXUW:
  case X86_INS_VPMAXUD:
  case X86_INS_VPMAXUQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_PMINSW:
    case X86_INS_PMAXSW:
    case X86_INS_PMINUW:
    case X86_INS_PMAXUW:
    case X86_INS_VPMINSW:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMINUW:
    case X86_INS_VPMAXUW:
      LaneSz = 2;
      break;
    case X86_INS_PMINSD:
    case X86_INS_PMAXSD:
    case X86_INS_PMINUD:
    case X86_INS_PMAXUD:
    case X86_INS_VPMINSD:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMINUD:
    case X86_INS_VPMAXUD:
      LaneSz = 4;
      break;
    case X86_INS_VPMINSQ:
    case X86_INS_VPMAXSQ:
    case X86_INS_VPMINUQ:
    case X86_INS_VPMAXUQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    bool IsSigned = false;
    switch (InsnId) {
    case X86_INS_PMINSB:
    case X86_INS_PMINSW:
    case X86_INS_PMINSD:
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_VPMINSB:
    case X86_INS_VPMINSW:
    case X86_INS_VPMINSD:
    case X86_INS_VPMINSQ:
    case X86_INS_VPMAXSB:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMAXSQ:
      IsSigned = true;
      break;
    default:
      break;
    }
    bool IsMax = false;
    switch (InsnId) {
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_PMAXUB:
    case X86_INS_PMAXUW:
    case X86_INS_PMAXUD:
    case X86_INS_VPMAXSB:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMAXSQ:
    case X86_INS_VPMAXUB:
    case X86_INS_VPMAXUW:
    case X86_INS_VPMAXUD:
    case X86_INS_VPMAXUQ:
      IsMax = true;
      break;
    default:
      break;
    }
    NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Cond = S.makeTemp(1);
        S.emit(CmpOp, Cond, {La, Lb});
        NdVar Lr = S.makeTemp(LaneSz);
        if (IsMax)
          S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
        else
          S.emit(NdOp::SELECT, Lr, {Cond, La, Lb});
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

  // Packed min/max — float: per-lane FLOAT_LESS + SELECT
  case X86_INS_MINSS:
  case X86_INS_MINSD:
  case X86_INS_MINPS:
  case X86_INS_MINPD:
  case X86_INS_MAXSS:
  case X86_INS_MAXSD:
  case X86_INS_MAXPS:
  case X86_INS_MAXPD:
  case X86_INS_VMINSS:
  case X86_INS_VMINSD:
  case X86_INS_VMINPS:
  case X86_INS_VMINPD:
  case X86_INS_VMAXSS:
  case X86_INS_VMAXSD:
  case X86_INS_VMAXPS:
  case X86_INS_VMAXPD: {
    if (X86.op_count < 2)
      break;
    bool IsVEX = (X86.op_count == 3);
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = IsVEX ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsMin = (InsnId == X86_INS_MINSS || InsnId == X86_INS_MINSD ||
                  InsnId == X86_INS_MINPS || InsnId == X86_INS_MINPD ||
                  InsnId == X86_INS_VMINSS || InsnId == X86_INS_VMINSD ||
                  InsnId == X86_INS_VMINPS || InsnId == X86_INS_VMINPD);

    bool IsScalar = (InsnId == X86_INS_MINSS || InsnId == X86_INS_MAXSS ||
                     InsnId == X86_INS_VMINSS || InsnId == X86_INS_VMAXSS ||
                     InsnId == X86_INS_MINSD || InsnId == X86_INS_MAXSD ||
                     InsnId == X86_INS_VMINSD || InsnId == X86_INS_VMAXSD);

    bool IsDouble = (InsnId == X86_INS_MINSD || InsnId == X86_INS_MAXSD ||
                     InsnId == X86_INS_MINPD || InsnId == X86_INS_MAXPD ||
                     InsnId == X86_INS_VMINSD || InsnId == X86_INS_VMAXSD ||
                     InsnId == X86_INS_VMINPD || InsnId == X86_INS_VMAXPD);

    unsigned LaneSz = IsDouble ? 8 : 4;
    unsigned NLanes = IsScalar ? 1 : (Dst.Size / LaneSz);

    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      if (IsMin)
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
      else
        S.emit(NdOp::FLOAT_LESS, Cmp, {Lb, La});
      NdVar Sel = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Sel, {Cmp, La, Lb});
      if (I == 0) {
        Acc = Sel;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Sel, Acc});
        Acc = Next;
      }
    }

    if (IsScalar && Dst.Size > LaneSz) {
      NdVar Upper = S.makeTemp(Dst.Size - LaneSz);
      S.emit(NdOp::SUBBYTES, Upper, {A, NdVar::cst(LaneSz, 4)});
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::CONCAT, Full, {Upper, Acc});
      S.emit(NdOp::COPY, Dst, {Full});
    } else if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // Packed convert/pack — per-lane saturating narrow.
  case X86_INS_PACKUSWB:
  case X86_INS_PACKUSDW:
  case X86_INS_PACKSSWB:
  case X86_INS_PACKSSDW:
  case X86_INS_VPACKUSWB:
  case X86_INS_VPACKUSDW:
  case X86_INS_VPACKSSWB:
  case X86_INS_VPACKSSDW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar DstR = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                       : operandRead(S, X86.operands[0]);
    NdVar Src2 = operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsSigned =
        (InsnId == X86_INS_PACKSSWB || InsnId == X86_INS_PACKSSDW ||
         InsnId == X86_INS_VPACKSSWB || InsnId == X86_INS_VPACKSSDW);
    bool IsWord = (InsnId == X86_INS_PACKSSWB || InsnId == X86_INS_PACKUSWB ||
                   InsnId == X86_INS_VPACKSSWB || InsnId == X86_INS_VPACKUSWB);
    unsigned SrcLaneSz = IsWord ? 2 : 4;
    unsigned DstLaneSz = IsWord ? 1 : 2;
    // PACK operates within each 128-bit lane: the lane's low half comes from
    // src1's lane, the high half from src2's lane.  The 256-bit (VEX.256) form
    // therefore interleaves the two operands PER 128-bit LANE
    // (dst = [pack(s1.lane0), pack(s2.lane0), pack(s1.lane1), pack(s2.lane1)]),
    // NOT as one 256-bit-wide pack — emitting all of src1 then all of src2 (the
    // naive extension) mislays the high lane.  MMX (64-bit dst) is a single
    // 8-byte lane; XMM one 16-byte lane; YMM two 16-byte lanes.
    unsigned LaneBytes = (Dst.Size >= 16) ? 16u : Dst.Size;
    unsigned NLanes128 = Dst.Size / LaneBytes; // MMX:1, XMM:1, YMM:2
    unsigned SrcElemsPerLane = LaneBytes / SrcLaneSz;

    auto ClampLane = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar Lane = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(Off, 4)});

      // Narrowing saturate: trunc to DstLaneSz, sext back, compare with
      // original.  If equal the value fits; otherwise pick 127 / -128 (signed)
      // or 255 / 0 (unsigned) based on the sign of the original.  This avoids
      // both the fork's InstCombine crash on @llvm.smax/@llvm.smin chains AND
      // the constant-folding mis-compute on INT_SLESS+SELECT clamp patterns.
      NdVar Narrow = S.makeTemp(DstLaneSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(SrcLaneSz);
      if (IsSigned)
        S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
      else
        S.emit(NdOp::INT_ZEXT, BackWide, {Narrow});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Lane, BackWide});
      NdVar IsPos = S.makeTemp(1);
      int64_t Hi = IsSigned ? (1LL << (DstLaneSz * 8 - 1)) - 1
                            : (1LL << (DstLaneSz * 8)) - 1;
      int64_t Lo = IsSigned ? -(1LL << (DstLaneSz * 8 - 1)) : 0;
      if (IsSigned) {
        S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, SrcLaneSz), Lane});
      } else {
        S.emit(NdOp::INT_SLESS, IsPos,
               {NdVar::cst(static_cast<uint64_t>(Hi), SrcLaneSz), Lane});
      }
      NdVar OverflowVal = S.makeTemp(DstLaneSz);
      NdVar HiNarrow = NdVar::cst(static_cast<uint64_t>(Hi), DstLaneSz);
      NdVar LoNarrow = NdVar::cst(static_cast<uint64_t>(Lo), DstLaneSz);
      S.emit(NdOp::SELECT, OverflowVal, {IsPos, HiNarrow, LoNarrow});
      NdVar Result = S.makeTemp(DstLaneSz);
      S.emit(NdOp::SELECT, Result, {Fits, Narrow, OverflowVal});
      return Result;
    };

    // Build the result low-to-high.  For each 128-bit lane L, first the
    // SrcElemsPerLane clamped elements of src1's lane L, then src2's lane L.
    // CONCAT(hi, lo) prepends `hi` above the accumulator, so iterating elements
    // in increasing significance yields the correct little-endian layout.
    bool First = true;
    NdVar Acc = S.makeTemp(0);
    auto AppendLane = [&](NdVar Src, unsigned Lane) {
      unsigned Base = Lane * LaneBytes; // byte offset of this lane in Src
      for (unsigned E = 0; E < SrcElemsPerLane; ++E) {
        NdVar B = ClampLane(Src, Base + E * SrcLaneSz);
        if (First) {
          Acc = B;
          First = false;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {B, Acc});
          Acc = Next;
        }
      }
    };
    for (unsigned L = 0; L < NLanes128; ++L) {
      AppendLane(DstR, L); // src1 lane L -> low half of dst lane L
      AppendLane(Src2, L); // src2 lane L -> high half of dst lane L
    }
    if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // AVX whole-register byte shift: each aligned 128-bit lane shifts
  // independently by the imm8 byte count (>=16 -> 0).
  case X86_INS_VPSLLDQ:
  case X86_INS_VPSRLDQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
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
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
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
      NdVar SrcFull = operandRead(S, X86.operands[X86.op_count - 1]);
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

  // AVX packed integer add/sub — per-lane decomposition.
  case X86_INS_VPADDB:
  case X86_INS_VPADDW:
  case X86_INS_VPADDD:
  case X86_INS_VPADDQ:
  case X86_INS_VPSUBB:
  case X86_INS_VPSUBW:
  case X86_INS_VPSUBD:
  case X86_INS_VPSUBQ: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_VPADDB:
    case X86_INS_VPSUBB:
      LaneSz = 1;
      break;
    case X86_INS_VPADDW:
    case X86_INS_VPSUBW:
      LaneSz = 2;
      break;
    case X86_INS_VPADDD:
    case X86_INS_VPSUBD:
      LaneSz = 4;
      break;
    case X86_INS_VPADDQ:
    case X86_INS_VPSUBQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    // NOTE: even qword (LaneSz==8) lanes must be added/subtracted
    // independently — a full-width INT_ADD/INT_SUB would propagate the carry
    // from lane 0 into lane 1 (wrong for VPADDQ/VPSUBQ).
    bool IsSub = (InsnId == X86_INS_VPSUBB || InsnId == X86_INS_VPSUBW ||
                  InsnId == X86_INS_VPSUBD || InsnId == X86_INS_VPSUBQ);
    NdOp LaneOpc = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
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
  // AVX saturating add/sub — per-lane with saturation.
  case X86_INS_VPADDSB:
  case X86_INS_VPADDSW:
  case X86_INS_VPADDUSB:
  case X86_INS_VPADDUSW:
  case X86_INS_VPSUBSB:
  case X86_INS_VPSUBSW:
  case X86_INS_VPSUBUSB:
  case X86_INS_VPSUBUSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    bool IsSigned = false, IsSub = false;
    switch (InsnId) {
    case X86_INS_VPADDSW:
    case X86_INS_VPSUBSW:
    case X86_INS_VPADDUSW:
    case X86_INS_VPSUBUSW:
      LaneSz = 2;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_VPADDSB:
    case X86_INS_VPADDSW:
    case X86_INS_VPSUBSB:
    case X86_INS_VPSUBSW:
      IsSigned = true;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_VPSUBSB:
    case X86_INS_VPSUBSW:
    case X86_INS_VPSUBUSB:
    case X86_INS_VPSUBUSW:
      IsSub = true;
      break;
    default:
      break;
    }
    NdOp ArithOp = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    unsigned WiderSz = LaneSz * 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(WiderSz), Bx = S.makeTemp(WiderSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ax, {La});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bx, {Lb});
        NdVar Wide = S.makeTemp(WiderSz);
        S.emit(ArithOp, Wide, {Ax, Bx});
        int64_t MaxV, MinV;
        if (IsSigned) {
          MaxV = (1LL << (LaneSz * 8 - 1)) - 1;
          MinV = -(1LL << (LaneSz * 8 - 1));
        } else {
          MaxV = (1LL << (LaneSz * 8)) - 1;
          MinV = 0;
        }
        NdVar HiClamp = S.makeTemp(1);
        if (IsSigned)
          S.emit(NdOp::INT_SLESS, HiClamp, {NdVar::cst(MaxV, WiderSz), Wide});
        else
          S.emit(NdOp::INT_LESS, HiClamp, {NdVar::cst(MaxV, WiderSz), Wide});
        NdVar Clamped1 = S.makeTemp(WiderSz);
        S.emit(NdOp::SELECT, Clamped1,
               {HiClamp, NdVar::cst(MaxV, WiderSz), Wide});
        NdVar LoClamp = S.makeTemp(1);
        if (IsSigned)
          S.emit(NdOp::INT_SLESS, LoClamp,
                 {Clamped1, NdVar::cst(MinV, WiderSz)});
        else
          S.emit(NdOp::INT_SLESS, LoClamp, {Wide, NdVar::cst(0, WiderSz)});
        NdVar Clamped2 = S.makeTemp(WiderSz);
        S.emit(NdOp::SELECT, Clamped2,
               {LoClamp, NdVar::cst(MinV, WiderSz), Clamped1});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lr, {Clamped2, NdVar::cst(0, 4)});
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

  // AVX packed integer compare — per-lane greater-than.
  case X86_INS_VPCMPGTB:
  case X86_INS_VPCMPGTW:
  case X86_INS_VPCMPGTD:
  case X86_INS_VPCMPGTQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = operandRead(S, X86.operands[1]);
    NdVar B = operandRead(S, X86.operands[2]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_VPCMPGTW:
      LaneSz = 2;
      break;
    case X86_INS_VPCMPGTD:
      LaneSz = 4;
      break;
    case X86_INS_VPCMPGTQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Gt = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, Gt, {Lb, La});
        NdVar Mask = S.makeTemp(LaneSz);
        uint64_t AllOnes = (LaneSz == 8) ? 0xFFFFFFFFFFFFFFFFULL
                                         : ((1ULL << (LaneSz * 8)) - 1);
        S.emit(NdOp::SELECT, Mask,
               {Gt, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
        if (I == 0) {
          Acc = Mask;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Mask, Acc});
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

  // AVX float arithmetic
  case X86_INS_VADDSS:
  case X86_INS_VADDSD:
  case X86_INS_VADDPS:
  case X86_INS_VADDPD:
  case X86_INS_VSUBSS:
  case X86_INS_VSUBSD:
  case X86_INS_VSUBPS:
  case X86_INS_VSUBPD:
  case X86_INS_VMULSS:
  case X86_INS_VMULSD:
  case X86_INS_VMULPS:
  case X86_INS_VMULPD:
  case X86_INS_VDIVSS:
  case X86_INS_VDIVSD:
  case X86_INS_VDIVPS:
  case X86_INS_VDIVPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? operandRead(S, X86.operands[1])
                                    : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VADDSS:
    case X86_INS_VADDSD:
    case X86_INS_VADDPS:
    case X86_INS_VADDPD:
      Opc = NdOp::FLOAT_ADD;
      break;
    case X86_INS_VSUBSS:
    case X86_INS_VSUBSD:
    case X86_INS_VSUBPS:
    case X86_INS_VSUBPD:
      Opc = NdOp::FLOAT_SUB;
      break;
    case X86_INS_VMULSS:
    case X86_INS_VMULSD:
    case X86_INS_VMULPS:
    case X86_INS_VMULPD:
      Opc = NdOp::FLOAT_MULT;
      break;
    default:
      Opc = NdOp::FLOAT_DIV;
    }
    bool IsPacked = (InsnId == X86_INS_VADDPS || InsnId == X86_INS_VADDPD ||
                     InsnId == X86_INS_VSUBPS || InsnId == X86_INS_VSUBPD ||
                     InsnId == X86_INS_VMULPS || InsnId == X86_INS_VMULPD ||
                     InsnId == X86_INS_VDIVPS || InsnId == X86_INS_VDIVPD);
    if (IsPacked && Dst.Size >= 16) {
      bool IsPD = (InsnId == X86_INS_VADDPD || InsnId == X86_INS_VSUBPD ||
                   InsnId == X86_INS_VMULPD || InsnId == X86_INS_VDIVPD);
      unsigned ElemSz = IsPD ? 8 : 4;
      unsigned NLanes = Dst.Size / ElemSz;
      std::vector<NdVar> Lanes(NLanes);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * ElemSz, 4)});
        NdVar LB = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * ElemSz, 4)});
        Lanes[I] = S.makeTemp(ElemSz);
        S.emit(Opc, Lanes[I], {LA, LB});
      }
      if (NLanes == 2) {
        S.emit(NdOp::CONCAT, Dst, {Lanes[1], Lanes[0]});
      } else {
        NdVar Lo = S.makeTemp(ElemSz * 2);
        S.emit(NdOp::CONCAT, Lo, {Lanes[1], Lanes[0]});
        NdVar Hi = S.makeTemp(ElemSz * 2);
        S.emit(NdOp::CONCAT, Hi, {Lanes[3], Lanes[2]});
        S.emit(NdOp::CONCAT, Dst, {Hi, Lo});
      }
    } else {
      bool IsSS = (InsnId == X86_INS_VADDSS || InsnId == X86_INS_VSUBSS ||
                   InsnId == X86_INS_VMULSS || InsnId == X86_INS_VDIVSS);
      bool IsSD = (InsnId == X86_INS_VADDSD || InsnId == X86_INS_VSUBSD ||
                   InsnId == X86_INS_VMULSD || InsnId == X86_INS_VDIVSD);
      if ((IsSS || IsSD) && Dst.Size > 8) {
        unsigned ScalarSz = IsSS ? 4 : 8;
        NdVar SA = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, SA, {A, NdVar::cst(0, 4)});
        NdVar SB = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, SB, {B, NdVar::cst(0, 4)});
        NdVar Res = S.makeTemp(ScalarSz);
        S.emit(Opc, Res, {SA, SB});
        unsigned HiSz = Dst.Size - ScalarSz;
        NdVar Hi = S.makeTemp(HiSz);
        S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(ScalarSz, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, Res});
      } else {
        S.emit(Opc, Dst, {A, B});
      }
    }
    break;
  }

  // PMADDWD: paired word multiply-add → dword results.
  case X86_INS_PMADDWD:
  case X86_INS_VPMADDWD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned HalfSz = Dst.Size / 2;
    unsigned DwordsPerHalf = HalfSz / 4;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < DwordsPerHalf; ++I) {
        unsigned WOff = BaseOff + I * 4;
        NdVar Aw0 = S.makeTemp(2), Aw1 = S.makeTemp(2);
        NdVar Bw0 = S.makeTemp(2), Bw1 = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Aw0, {A, NdVar::cst(WOff, 4)});
        S.emit(NdOp::SUBBYTES, Aw1, {A, NdVar::cst(WOff + 2, 4)});
        S.emit(NdOp::SUBBYTES, Bw0, {B, NdVar::cst(WOff, 4)});
        S.emit(NdOp::SUBBYTES, Bw1, {B, NdVar::cst(WOff + 2, 4)});
        NdVar Aw0x = S.makeTemp(4), Aw1x = S.makeTemp(4);
        NdVar Bw0x = S.makeTemp(4), Bw1x = S.makeTemp(4);
        S.emit(NdOp::INT_SEXT, Aw0x, {Aw0});
        S.emit(NdOp::INT_SEXT, Aw1x, {Aw1});
        S.emit(NdOp::INT_SEXT, Bw0x, {Bw0});
        S.emit(NdOp::INT_SEXT, Bw1x, {Bw1});
        NdVar P0 = S.makeTemp(4), P1 = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, P0, {Aw0x, Bw0x});
        S.emit(NdOp::INT_MULT, P1, {Aw1x, Bw1x});
        NdVar Sum = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Sum, {P0, P1});
        if (I == 0) {
          Acc = Sum;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 4);
          S.emit(NdOp::CONCAT, Next, {Sum, Acc});
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

  // PHADDD/PHADDW/PHSUBD/PHSUBW: horizontal add/sub of adjacent pairs.
  case X86_INS_PHADDD:
  case X86_INS_PHADDW:
  case X86_INS_PHSUBD:
  case X86_INS_PHSUBW:
  case X86_INS_VPHADDD:
  case X86_INS_VPHADDW:
  case X86_INS_VPHSUBD:
  case X86_INS_VPHSUBW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 4;
    if (InsnId == X86_INS_PHADDW || InsnId == X86_INS_PHSUBW ||
        InsnId == X86_INS_VPHADDW || InsnId == X86_INS_VPHSUBW)
      LaneSz = 2;
    bool IsSub = (InsnId == X86_INS_PHSUBD || InsnId == X86_INS_PHSUBW ||
                  InsnId == X86_INS_VPHSUBD || InsnId == X86_INS_VPHSUBW);
    NdOp Opc = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    // Horizontal add/sub operate INDEPENDENTLY within each 128-bit lane: the
    // low half of each result lane holds src1's adjacent-pair reductions, the
    // high half holds src2's — for the SAME 128-bit lane.  Treating a 256-bit
    // ymm as one wide register ("all src1 pairs then all src2 pairs")
    // mis-routes the high-lane results.  Build each 128-bit lane separately
    // (the 64-bit MMX and 128-bit xmm forms are the NumLanes==1 special case).
    unsigned LaneWidth = Dst.Size < 16 ? Dst.Size : 16;
    unsigned HalfWidth = LaneWidth / 2;
    unsigned NumLanes = Dst.Size / LaneWidth;
    unsigned PairsPerLane = HalfWidth / LaneSz;
    auto BuildLaneHalf = [&](NdVar Src, unsigned LaneBase) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < PairsPerLane; ++I) {
        unsigned Off0 = LaneBase + I * 2 * LaneSz;
        unsigned Off1 = Off0 + LaneSz;
        NdVar E0 = S.makeTemp(LaneSz), E1 = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, E0, {Src, NdVar::cst(Off0, 4)});
        S.emit(NdOp::SUBBYTES, E1, {Src, NdVar::cst(Off1, 4)});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(Opc, R, {E0, E1});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      NdVar Alo = BuildLaneHalf(A, L * LaneWidth);
      NdVar Bhi = BuildLaneHalf(B, L * LaneWidth);
      NdVar Lane = S.makeTemp(LaneWidth);
      S.emit(NdOp::CONCAT, Lane, {Bhi, Alo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * LaneWidth);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PSADBW: sum of absolute differences of bytes.
  case X86_INS_PSADBW:
  case X86_INS_VPSADBW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    // Each destination qword holds the sum of |A[b]-B[b]| over the 8 bytes b in
    // that qword (byte offset Q*8 .. Q*8+7); the upper 48 bits are zero.  The
    // grouping is per-8-byte-group with no cross-lane interaction, so xmm (2
    // qwords) and ymm (4 qwords) differ only in the qword count — every qword
    // must be built (an earlier version returned after Q==0, silently dropping
    // the high 128-bit lane and leaving a 128-bit result in a 256-bit dst).
    unsigned NumQwords = Dst.Size / 8;
    auto BuildQword = [&](unsigned QIdx) -> NdVar {
      unsigned BaseOff = QIdx * 8;
      NdVar Sum = S.makeTemp(2);
      S.emit(NdOp::COPY, Sum, {NdVar::cst(0, 2)});
      for (unsigned I = 0; I < 8; ++I) {
        unsigned Off = BaseOff + I;
        NdVar Ab = S.makeTemp(1), Bb = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ab, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bb, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(2), Bx = S.makeTemp(2);
        S.emit(NdOp::INT_ZEXT, Ax, {Ab});
        S.emit(NdOp::INT_ZEXT, Bx, {Bb});
        NdVar Diff = S.makeTemp(2);
        S.emit(NdOp::INT_SUB, Diff, {Ax, Bx});
        NdVar Neg = S.makeTemp(2);
        S.emit(NdOp::INT_NEG2, Neg, {Diff});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Diff, NdVar::cst(0, 2)});
        NdVar Abs = S.makeTemp(2);
        S.emit(NdOp::SELECT, Abs, {IsNeg, Neg, Diff});
        NdVar NewSum = S.makeTemp(2);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Abs});
        Sum = NewSum;
      }
      NdVar SumExt = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, SumExt, {Sum});
      return SumExt;
    };
    NdVar Full = BuildQword(0);
    for (unsigned Q = 1; Q < NumQwords; ++Q) {
      NdVar Next = S.makeTemp((Q + 1) * 8);
      // CONCAT takes {mostSignificant, leastSignificant}; qword Q is higher than
      // the accumulated low qwords, so it is the MSB operand.
      S.emit(NdOp::CONCAT, Next, {BuildQword(Q), Full});
      Full = Next;
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PABS*: per-lane absolute value.
  case X86_INS_PABSB:
  case X86_INS_PABSW:
  case X86_INS_PABSD:
  case X86_INS_VPABSB:
  case X86_INS_VPABSW:
  case X86_INS_VPABSD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    if (InsnId == X86_INS_PABSW || InsnId == X86_INS_VPABSW)
      LaneSz = 2;
    if (InsnId == X86_INS_PABSD || InsnId == X86_INS_VPABSD)
      LaneSz = 4;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(Off, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {Lane});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Lane, NdVar::cst(0, LaneSz)});
        NdVar Abs = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Abs, {IsNeg, Neg, Lane});
        if (I == 0) {
          Acc = Abs;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Abs, Acc});
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

  // PHADDSW/PHSUBSW: horizontal add/sub with signed saturation (word).
  case X86_INS_PHADDSW:
  case X86_INS_PHSUBSW:
  case X86_INS_VPHADDSW:
  case X86_INS_VPHSUBSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_PHSUBSW || InsnId == X86_INS_VPHSUBSW);
    // Like PHADD/PHSUB, the saturating word forms reduce adjacent pairs PER
    // 128-bit lane: each result lane's low 64 bits come from src1's pairs and
    // its high 64 bits from src2's pairs of the SAME lane.  Build lane-by-lane
    // (NumLanes==1 covers the 64-bit MMX and 128-bit xmm forms).
    unsigned LaneWidth = Dst.Size < 16 ? Dst.Size : 16;
    unsigned HalfWidth = LaneWidth / 2;
    unsigned NumLanes = Dst.Size / LaneWidth;
    unsigned PairsPerLane = HalfWidth / 2;
    Intrinsic SatIC = IsSub ? Intrinsic::X86_SsubSat : Intrinsic::X86_SaddSat;
    auto BuildLaneHalf = [&](NdVar Src, unsigned LaneBase) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < PairsPerLane; ++I) {
        unsigned Off0 = LaneBase + I * 4, Off1 = Off0 + 2;
        NdVar E0 = S.makeTemp(2), E1 = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, E0, {Src, NdVar::cst(Off0, 4)});
        S.emit(NdOp::SUBBYTES, E1, {Src, NdVar::cst(Off1, 4)});
        NdVar Clamped = S.makeTemp(2);
        S.emitIntrinsic(SatIC, Clamped, {E0, E1});
        if (I == 0) {
          Acc = Clamped;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Clamped, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      NdVar Alo = BuildLaneHalf(A, L * LaneWidth);
      NdVar Bhi = BuildLaneHalf(B, L * LaneWidth);
      NdVar Lane = S.makeTemp(LaneWidth);
      S.emit(NdOp::CONCAT, Lane, {Bhi, Alo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * LaneWidth);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PSIGNB/PSIGNW/PSIGND: conditional negate based on sign of second operand.
  case X86_INS_PSIGND:
  case X86_INS_PSIGNW:
  case X86_INS_PSIGNB:
  case X86_INS_VPSIGND:
  case X86_INS_VPSIGNW:
  case X86_INS_VPSIGNB: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    if (InsnId == X86_INS_PSIGNW || InsnId == X86_INS_VPSIGNW)
      LaneSz = 2;
    if (InsnId == X86_INS_PSIGND || InsnId == X86_INS_VPSIGND)
      LaneSz = 4;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {La});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Lb, NdVar::cst(0, LaneSz)});
        NdVar IsZero = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsZero, {Lb, NdVar::cst(0, LaneSz)});
        NdVar R1 = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, R1, {IsNeg, Neg, La});
        NdVar R2 = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, R2, {IsZero, NdVar::cst(0, LaneSz), R1});
        if (I == 0) {
          Acc = R2;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R2, Acc});
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

  // PMADDUBSW: unsigned bytes * signed bytes → signed words with saturation.
  case X86_INS_PMADDUBSW:
  case X86_INS_VPMADDUBSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned HalfSz = Dst.Size / 2;
    unsigned WordsPerHalf = HalfSz / 2;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < WordsPerHalf; ++I) {
        unsigned ByteOff = BaseOff + I * 2;
        NdVar Ab0 = S.makeTemp(1), Ab1 = S.makeTemp(1);
        NdVar Bb0 = S.makeTemp(1), Bb1 = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ab0, {A, NdVar::cst(ByteOff, 4)});
        S.emit(NdOp::SUBBYTES, Ab1, {A, NdVar::cst(ByteOff + 1, 4)});
        S.emit(NdOp::SUBBYTES, Bb0, {B, NdVar::cst(ByteOff, 4)});
        S.emit(NdOp::SUBBYTES, Bb1, {B, NdVar::cst(ByteOff + 1, 4)});
        NdVar Ax0 = S.makeTemp(4), Ax1 = S.makeTemp(4);
        NdVar Bx0 = S.makeTemp(4), Bx1 = S.makeTemp(4);
        S.emit(NdOp::INT_ZEXT, Ax0, {Ab0});
        S.emit(NdOp::INT_ZEXT, Ax1, {Ab1});
        S.emit(NdOp::INT_SEXT, Bx0, {Bb0});
        S.emit(NdOp::INT_SEXT, Bx1, {Bb1});
        NdVar P0 = S.makeTemp(4), P1 = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, P0, {Ax0, Bx0});
        S.emit(NdOp::INT_MULT, P1, {Ax1, Bx1});
        NdVar Sum32 = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Sum32, {P0, P1});
        NdVar Clamped = S.makeTemp(2);
        NdVar IsOvfPos = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsOvfPos, {NdVar::cst(0x7FFF, 4), Sum32});
        NdVar IsOvfNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsOvfNeg,
               {Sum32, NdVar::cst(0xFFFF8000U, 4)});
        NdVar Lo = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Lo, {Sum32, NdVar::cst(0, 4)});
        NdVar R1 = S.makeTemp(2);
        S.emit(NdOp::SELECT, R1, {IsOvfPos, NdVar::cst(0x7FFF, 2), Lo});
        S.emit(NdOp::SELECT, Clamped, {IsOvfNeg, NdVar::cst(0x8000, 2), R1});
        if (I == 0) {
          Acc = Clamped;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Clamped, Acc});
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

  // PMULHRSW: multiply high with rounding and scale.
  // result[i] = (a[i]*b[i] + 0x4000) >> 15, per word lane.
  case X86_INS_PMULHRSW:
  case X86_INS_VPMULHRSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? operandRead(S, X86.operands[1])
                      : operandRead(S, X86.operands[0]);
    NdVar B = operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned HalfSz = Dst.Size / 2;
    unsigned WordsPerHalf = HalfSz / 2;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < WordsPerHalf; ++I) {
        unsigned Off = BaseOff + I * 2;
        NdVar Aw = S.makeTemp(2), Bw = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Aw, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bw, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(4), Bx = S.makeTemp(4);
        S.emit(NdOp::INT_SEXT, Ax, {Aw});
        S.emit(NdOp::INT_SEXT, Bx, {Bw});
        NdVar Prod = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Prod, {Ax, Bx});
        NdVar Rounded = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Rounded, {Prod, NdVar::cst(0x4000, 4)});
        NdVar Shifted = S.makeTemp(4);
        S.emit(NdOp::INT_ASHR, Shifted, {Rounded, NdVar::cst(15, 4)});
        NdVar Result = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Result, {Shifted, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Result;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Result, Acc});
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

  case X86_INS_AESENC:
  case X86_INS_VAESENC: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesEnc, Dst, {State, Key});
    break;
  }
  case X86_INS_AESENCLAST:
  case X86_INS_VAESENCLAST: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesEncLast, Dst, {State, Key});
    break;
  }
  case X86_INS_AESDEC:
  case X86_INS_VAESDEC: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesDec, Dst, {State, Key});
    break;
  }
  case X86_INS_AESDECLAST:
  case X86_INS_VAESDECLAST: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar State = (X86.op_count >= 3) ? operandRead(S, X86.operands[1]) : Dst;
    NdVar Key = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesDecLast, Dst, {State, Key});
    break;
  }
  case X86_INS_AESIMC:
  case X86_INS_VAESIMC: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[X86.op_count - 1]);
    S.emitIntrinsic(Intrinsic::AesImc, Dst, {Src});
    break;
  }
  case X86_INS_AESKEYGENASSIST:
  case X86_INS_VAESKEYGENASSIST: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Intrinsic::AesKeyGenAssist, Dst,
                    {Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_PCLMULQDQ:
  case X86_INS_VPCLMULQDQ: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    int SrcIdx = (X86.op_count >= 4) ? 1 : 0;
    NdVar Src1 = operandRead(S, X86.operands[SrcIdx]);
    NdVar Src2 = operandRead(S, X86.operands[SrcIdx + 1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[X86.op_count - 1].imm);
    S.emitIntrinsic(Intrinsic::Pclmulqdq, Dst,
                    {Src1, Src2, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHA1RNDS4: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    uint8_t Imm = static_cast<uint8_t>(X86.operands[2].imm);
    S.emitIntrinsic(Intrinsic::Sha1Rnds4, Dst,
                    {Dst, Src, NdVar::cst(Imm, 1)});
    break;
  }
  case X86_INS_SHA1NEXTE: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha1Nexte, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA1MSG1: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha1Msg1, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA1MSG2: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha1Msg2, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA256RNDS2: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar Xmm0 = NdVar::reg(x86reg::XMM0, 16);
    S.emitIntrinsic(Intrinsic::Sha256Rnds2, Dst, {Dst, Src, Xmm0});
    break;
  }
  case X86_INS_SHA256MSG1: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha256Msg1, Dst, {Dst, Src});
    break;
  }
  case X86_INS_SHA256MSG2: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Intrinsic::Sha256Msg2, Dst, {Dst, Src});
    break;
  }
  case X86_INS_CRC32: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = operandWrite(X86.operands[0]);
    NdVar Src = operandRead(S, X86.operands[1]);
    Intrinsic IC;
    switch (Src.Size) {
    case 1:
      IC = Intrinsic::X86Crc32b;
      break;
    case 2:
      IC = Intrinsic::X86Crc32w;
      break;
    case 4:
      IC = Intrinsic::X86Crc32d;
      break;
    default:
      IC = Intrinsic::X86Crc32q;
      break;
    }
    S.emitIntrinsic(IC, Dst, {Dst, Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
