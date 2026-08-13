//===- ARMLiftSIMDNEONPermute.cpp - ARM32 NEON lane permute lifter -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NEON lane movement: VSWP, the interleave/transpose trio VTRN,
/// VUZP and VZIP, VDUP, VINS, the table lookups VTBL/VTBX and VEXT.
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

bool liftSIMDNEONPermute(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                         const cs_arm &ARM) {
  switch (Insn->id) {
  // VSWP swaps the full contents of two D/Q registers.  The old handler emitted
  // an unhandled intrinsic (silently 0) into operand 0 and never wrote operand
  // 1, so `vswp d0,d1` left d0=0 and d1 untouched.  Snapshot operand 0's old
  // value before the writes so the exchange is correct even when the registers
  // alias.
  case ARM_INS_VSWP: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Other = L.operandWrite(ARM.operands[1]);
      NdVar A = L.operandRead(S, ARM.operands[0]);
      NdVar B = L.operandRead(S, ARM.operands[1]);
      NdVar ATmp = S.makeTemp(A.Size);
      S.emit(NdOp::COPY, ATmp, {A});
      S.emit(NdOp::COPY, Dst, {B});
      S.emit(NdOp::COPY, Other, {ATmp});
    }
    break;
  }
  case ARM_INS_VTRN:
  case ARM_INS_VUZP:
  case ARM_INS_VZIP: {
    if (ARM.op_count < 2)
      break;
    // VZIP/VUZP/VTRN read AND write BOTH register operands.  The previous code
    // only wrote operands[0], leaving the second register (which receives the
    // high-half interleave / odd elements) stale — silently corrupting any
    // later use of that register (e.g. clang's byte-widening sequences).
    NdVar DdIn = L.operandRead(S, ARM.operands[0]);
    NdVar DmIn = L.operandRead(S, ARM.operands[1]);
    NdVar DdOut = L.operandWrite(ARM.operands[0]);
    NdVar DmOut = L.operandWrite(ARM.operands[1]);
    // Element size must come from the data type; capstone often leaves
    // vector_data INVALID for `vzip.8`/`vuzp.8`/`vtrn.8`, in which case the
    // old default of 4 made `vzip.8` interleave 4-byte groups instead of
    // bytes — silently shuffling at the wrong granularity.
    unsigned LaneSz = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
    if (LaneSz == 0)
      LaneSz = 4;

    unsigned N = DdOut.Size / LaneSz; // lanes per register
    if (N < 2) {
      S.emit(NdOp::COPY, DdOut, {DdIn});
      break;
    }

    auto getLane = [&](NdVar Reg, unsigned Idx) {
      NdVar L = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, L,
             {Reg, NdVar::cst(static_cast<uint64_t>(Idx) * LaneSz, 4)});
      return L;
    };
    auto assemble = [&](NdVar Out, const std::vector<NdVar> &Elems) {
      if (Elems.empty())
        return;
      NdVar Acc = Elems[0];
      for (unsigned I = 1; I < Elems.size(); ++I) {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Elems[I], Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Out, {Acc});
    };

    std::vector<NdVar> DdElems, DmElems;
    if (Insn->id == ARM_INS_VZIP) {
      // Interleave: combined[g] = even ? Dd[g/2] : Dm[g/2]; first N -> Dd,
      // next N -> Dm.
      for (unsigned G = 0; G < 2 * N; ++G) {
        NdVar E = (G % 2 == 0) ? getLane(DdIn, G / 2) : getLane(DmIn, G / 2);
        (G < N ? DdElems : DmElems).push_back(E);
      }
    } else if (Insn->id == ARM_INS_VUZP) {
      // De-interleave concat(Dd,Dm): Dd gets even indices, Dm gets odd indices.
      auto concat = [&](unsigned J) {
        return J < N ? getLane(DdIn, J) : getLane(DmIn, J - N);
      };
      for (unsigned K = 0; K < N; ++K)
        DdElems.push_back(concat(2 * K));
      for (unsigned K = 0; K < N; ++K)
        DmElems.push_back(concat(2 * K + 1));
    } else { // VTRN — transpose pairs
      for (unsigned K = 0; 2 * K + 1 < N; ++K) {
        DdElems.push_back(getLane(DdIn, 2 * K));
        DdElems.push_back(getLane(DmIn, 2 * K));
        DmElems.push_back(getLane(DdIn, 2 * K + 1));
        DmElems.push_back(getLane(DmIn, 2 * K + 1));
      }
    }
    assemble(DdOut, DdElems);
    assemble(DmOut, DmElems);
    break;
  }
  case ARM_INS_VDUP: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
      // The broadcast element size comes from the data type (.8/.16/.32), not
      // from the source register width.  `vdup.16 dN, rM` must replicate only
      // the low 16 bits of the core register, not the whole 32-bit register.
      // Capstone often leaves vector_data INVALID for the `vdup.N dN, rM`
      // (GPR→vector) form, so fall back to the mnemonic suffix; otherwise
      // ElemSz defaulted to the 4-byte GPR width and `vdup.8` wrongly
      // broadcast the whole 32-bit register (1 valid byte per 4).
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned ElemSz = LI.LaneSz ? LI.LaneSz : Src.Size;
      if (ElemSz == 0)
        ElemSz = Src.Size;
      // `vdup.32 dN, dM[idx]` broadcasts a specific *lane* of the source
      // vector, not lane 0.  operandRead returns the whole register, so honour
      // the source lane index here — otherwise a reduction's `vdup d,dM[1]`
      // wrongly splatted lane 0 and doubled the wrong partial sum.
      auto &VdupSrcOp = ARM.operands[ARM.op_count - 1];
      int VdupSrcLane = VdupSrcOp.neon_lane >= 0 ? VdupSrcOp.neon_lane
                                                 : VdupSrcOp.vector_index;
      NdVar Elem = Src;
      if (ElemSz < Src.Size) {
        unsigned ByteOff = 0;
        if (VdupSrcLane > 0) {
          ByteOff = static_cast<unsigned>(VdupSrcLane) * ElemSz;
          if (ByteOff + ElemSz > Src.Size)
            ByteOff = 0;
        }
        Elem = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Elem, {Src, NdVar::cst(ByteOff, 4)});
      }
      if (ElemSz > 0 && Dst.Size > ElemSz) {
        unsigned NLanes = Dst.Size / ElemSz;
        NdVar Acc = Elem;
        for (unsigned I = 1; I < NLanes; ++I) {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Elem, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Elem});
      }
    }
    break;
  }
  case ARM_INS_VINS: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[ARM.op_count - 1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  // NEON VTBL/VTBX — per-byte table lookup via a SELECT chain.
  //   VTBL: d[i] = (idx[i] < table_len) ? table[idx[i]] : 0
  //   VTBX: d[i] = (idx[i] < table_len) ? table[idx[i]] : old_d[i]
  // The table spans 1-4 consecutive D registers, which capstone expands into
  // separate operands; the index is the last operand and the table registers
  // are everything between the destination and the index.
  case ARM_INS_VTBL:
  case ARM_INS_VTBX: {
    if (ARM.op_count < 3)
      break;
    bool IsTbx = (Insn->id == ARM_INS_VTBX);
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    unsigned IdxOp = static_cast<unsigned>(ARM.op_count) - 1;
    NdVar Idx = L.operandRead(S, ARM.operands[IdxOp]);
    std::vector<NdVar> TblBytes;
    for (unsigned R = 1; R < IdxOp; ++R) {
      NdVar T = L.operandRead(S, ARM.operands[R]);
      for (unsigned J = 0; J < T.Size; ++J) {
        NdVar B = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, B, {T, NdVar::cst(J, 4)});
        TblBytes.push_back(B);
      }
    }
    unsigned TblLen = static_cast<unsigned>(TblBytes.size());
    unsigned NBytes = Dst.Size;
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NBytes; ++I) {
      NdVar IdxByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, IdxByte, {Idx, NdVar::cst(I, 4)});
      NdVar Res = S.makeTemp(1);
      if (IsTbx)
        S.emit(NdOp::SUBBYTES, Res, {OldDst, NdVar::cst(I, 4)});
      else
        S.emit(NdOp::COPY, Res, {NdVar::cst(0, 1)});
      for (unsigned J = 0; J < TblLen; ++J) {
        NdVar IsJ = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsJ, {IdxByte, NdVar::cst(J, 1)});
        NdVar NewRes = S.makeTemp(1);
        S.emit(NdOp::SELECT, NewRes, {IsJ, TblBytes[J], Res});
        Res = NewRes;
      }
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_VEXT: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    unsigned ImmVal = 0;
    if (ARM.operands[3].type == ARM_OP_IMM)
      ImmVal = ARM.operands[3].imm;
    // The VEXT extract index is in ELEMENTS for the `.16`/`.32` mnemonic forms
    // (capstone reports `vext.32 ...,#3` as 3, not the 12-byte offset).  Scale
    // by the element size to get the byte offset; `.8` has elemSize 1 (the
    // historical byte-accurate behavior is preserved).
    {
      unsigned ElemSz = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
      if (ElemSz > 1)
        ImmVal *= ElemSz;
    }
    unsigned TotalBytes = Dst.Size;
    if (ImmVal == 0) {
      S.emit(NdOp::COPY, Dst, {A});
    } else if (ImmVal >= TotalBytes) {
      S.emit(NdOp::COPY, Dst, {B});
    } else {
      NdVar Hi = S.makeTemp(TotalBytes - ImmVal);
      S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(ImmVal, 4)});
      NdVar Lo = S.makeTemp(ImmVal);
      S.emit(NdOp::SUBBYTES, Lo, {B, NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Lo, Hi});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
