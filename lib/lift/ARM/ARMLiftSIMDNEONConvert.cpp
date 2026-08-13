//===- ARMLiftSIMDNEONConvert.cpp - ARM32 NEON unary and convert lifter ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane NEON unary operations: VMVN, the bit counts VCLZ/VCNT/
/// VCLS, the widening/narrowing moves VMOVL/VMOVN, VREV and the
/// reciprocal estimate/step family.
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

bool liftSIMDNEONConvert(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                         const cs_arm &ARM) {
  switch (Insn->id) {
  // NEON misc unary: operations with correct NdOp mapping
  case ARM_INS_VMVN: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      const auto &SrcOp = ARM.operands[1];
      // vmvn.iN dN/qN, #imm — broadcast ~imm across ALL lanes.  Previously this
      // form did `INT_NOT Dst(16), imm(4)` which only set lane 0 and left the
      // other lanes undefined/zero (e.g. a broadcast -4096 only landed in
      // lane 0, so `(x & mask) - 4096` was applied to one lane in four).
      if (SrcOp.type == ARM_OP_IMM && Dst.Size > 4) {
        auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
        unsigned LaneSz = LI.LaneSz ? LI.LaneSz : 4;
        uint64_t Imm = static_cast<uint64_t>(SrcOp.imm);
        uint64_t LMask = (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
        uint64_t NotImm = (~Imm) & LMask;
        uint64_t Lo = 0, Hi = 0;
        for (unsigned I = 0; I * LaneSz < 8 && I * LaneSz < Dst.Size; ++I)
          Lo |= NotImm << (I * LaneSz * 8);
        for (unsigned I = 0; I * LaneSz < Dst.Size; ++I)
          if (I * LaneSz >= 8)
            Hi |= NotImm << ((I * LaneSz - 8) * 8);
        if (Dst.Size <= 8) {
          S.emit(NdOp::COPY, Dst, {NdVar::cst(Lo, Dst.Size)});
        } else {
          NdVar LoV = S.makeTemp(8);
          S.emit(NdOp::COPY, LoV, {NdVar::cst(Lo, 8)});
          NdVar HiV = S.makeTemp(Dst.Size - 8);
          S.emit(NdOp::COPY, HiV,
                 {NdVar::cst(Hi, static_cast<uint16_t>(Dst.Size - 8))});
          S.emit(NdOp::CONCAT, Dst, {HiV, LoV});
        }
        break;
      }
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emit(NdOp::INT_NOT, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VCLZ: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      // `vclz.iN` counts leading zeros PER LANE; a single LZCOUNT on the whole
      // register counts zeros of the full width and breaks the reduction.
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::LZCOUNT, Lr, {La});
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
        S.emit(NdOp::LZCOUNT, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VCNT: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      unsigned NBytes = Src.Size;
      if (NBytes > 1) {
        NdVar Acc = NdVar::cst(0, 0);
        for (unsigned I = 0; I < NBytes; ++I) {
          NdVar B = S.makeTemp(1);
          S.emit(NdOp::SUBBYTES, B, {Src, NdVar::cst(I, 4)});
          NdVar Pop = S.makeTemp(1);
          S.emit(NdOp::POPCOUNT, Pop, {B});
          if (I == 0) {
            Acc = Pop;
          } else {
            NdVar P = S.makeTemp(Acc.Size + 1);
            S.emit(NdOp::CONCAT, P, {Pop, Acc});
            Acc = P;
          }
        }
        if (Acc.Size < Dst.Size)
          S.emit(NdOp::INT_ZEXT, Dst, {Acc});
        else
          S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::POPCOUNT, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VCLS: {
    // Count leading sign bits PER LANE: cls(x) = clz(x ^ (x >>s 1)) - 1.
    // Was a COPY placeholder (did nothing).
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned LaneSz = LI.LaneSz;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Sh = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_ASHR, Sh, {La, NdVar::cst(1, LaneSz)});
          NdVar Xr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_XOR, Xr, {La, Sh});
          NdVar Cz = S.makeTemp(LaneSz);
          S.emit(NdOp::LZCOUNT, Cz, {Xr});
          NdVar R = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_SUB, R, {Cz, NdVar::cst(1, LaneSz)});
          if (I == 0) {
            Acc = R;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {R, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VPNOT:
  case ARM_INS_VCTP: {
    // MVE (M-profile Helium) predicate ops — not exercised by A-profile NEON
    // tests; left as a placeholder COPY.
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VMOVL: {
    // Widen each narrow lane of the D source to a wide lane of the Q dest.
    // A single full-width INT_ZEXT (the old behaviour) only zero-extends the
    // whole 64-bit value into the low half of the 128-bit dest, which is wrong
    // for every lane above lane 0.
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned SrcLaneSz = LI.LaneSz;
      if (SrcLaneSz > 0 && SrcLaneSz < 8 && Src.Size >= SrcLaneSz) {
        unsigned DstLaneSz = SrcLaneSz * 2;
        unsigned NLanes = Src.Size / SrcLaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar SLane = S.makeTemp(SrcLaneSz);
          S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * SrcLaneSz, 4)});
          NdVar WLane = S.makeTemp(DstLaneSz);
          S.emit(LI.IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLane, {SLane});
          if (I == 0) {
            Acc = WLane;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
            S.emit(NdOp::CONCAT, Next, {WLane, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::INT_ZEXT, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VMOVLB:
  case ARM_INS_VMOVLT: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VMOVN: {
    // Narrow each element of the Q source to half-width in the D dest.  Taking
    // the low half of the whole register (the old behaviour) only keeps the
    // first half of the lanes instead of truncating every lane.
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      auto LI = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic);
      unsigned SrcLaneSz = LI.LaneSz;
      if (SrcLaneSz >= 2 && Src.Size >= SrcLaneSz) {
        unsigned DstLaneSz = SrcLaneSz / 2;
        unsigned NLanes = Src.Size / SrcLaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar SLane = S.makeTemp(SrcLaneSz);
          S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * SrcLaneSz, 4)});
          NdVar NLane = S.makeTemp(DstLaneSz);
          S.emit(NdOp::SUBBYTES, NLane, {SLane, NdVar::cst(0, 4)});
          if (I == 0) {
            Acc = NLane;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
            S.emit(NdOp::CONCAT, Next, {NLane, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      }
    }
    break;
  }
  case ARM_INS_VMOVNB:
  case ARM_INS_VMOVNT: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
    }
    break;
  }
  case ARM_INS_VREV16:
  case ARM_INS_VREV32:
  case ARM_INS_VREV64: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      // VREV<group>.<elem> reverses the order of <elem>-bit elements within
      // each <group>-bit container.  The container size is encoded by the
      // opcode (16/32/64); the element size is the data-type suffix.  The
      // previous handler simply COPY'd Src→Dst for VREV32/VREV64, so the
      // reversal silently never happened (e.g. `vrev64.32 q,q` left lanes in
      // place).  This went unnoticed because the only tests summed both lanes,
      // which is invariant under reversal.  Do a real per-element shuffle.
      unsigned GroupSz = (Insn->id == ARM_INS_VREV16)   ? 2
                         : (Insn->id == ARM_INS_VREV32) ? 4
                                                        : 8;
      unsigned ElemSz = getNeonLaneInfo(ARM.vector_data, Insn->mnemonic).LaneSz;
      if (ElemSz == 0 || ElemSz >= GroupSz || (GroupSz % ElemSz) != 0 ||
          Dst.Size < GroupSz || (Dst.Size % GroupSz) != 0) {
        // Cannot determine a sensible element/container layout: preserve old
        // behaviour rather than emit something worse.
        S.emit(NdOp::COPY, Dst, {Src});
        break;
      }
      unsigned ElemsPerGroup = GroupSz / ElemSz;
      unsigned NumGroups = Dst.Size / GroupSz;
      // Collect reversed elements in lane order (lane 0 = lowest bytes).
      std::vector<NdVar> Elems;
      for (unsigned G = 0; G < NumGroups; ++G) {
        for (unsigned E = 0; E < ElemsPerGroup; ++E) {
          unsigned SrcElem = ElemsPerGroup - 1 - E;
          unsigned SrcByteOff = G * GroupSz + SrcElem * ElemSz;
          NdVar L = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(SrcByteOff, 4)});
          Elems.push_back(L);
        }
      }
      NdVar Acc = Elems[0];
      for (unsigned I = 1; I < Elems.size(); ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Elems[I], Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // NEON reciprocal estimate/step: use intrinsics
  case ARM_INS_VRECPE: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emitIntrinsic(Intrinsic::ArmVrecpe, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VRECPS: {
    // VRECPS/VRSQRTS are BINARY (`vrecps.f32 Qd,Qn,Qm` = per-lane
    // Newton-Raphson step `2 - Qn*Qm`).  The old code read only operands[1] and
    // dropped the second source, so the refinement step was wrong.  Read both
    // sources; the 2-operand destructive form (Qd = Qd · Qm) uses Qd as the
    // first source.
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                      : L.operandRead(S, ARM.operands[0]);
      NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
      S.emitIntrinsic(Intrinsic::ArmVrecps, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VRSQRTE: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar Src = L.operandRead(S, ARM.operands[1]);
      S.emitIntrinsic(Intrinsic::ArmVrsqrte, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VRSQRTS: {
    if (ARM.op_count >= 2) {
      NdVar Dst = L.operandWrite(ARM.operands[0]);
      NdVar A = (ARM.op_count >= 3) ? L.operandRead(S, ARM.operands[1])
                                      : L.operandRead(S, ARM.operands[0]);
      NdVar B = L.operandRead(S, ARM.operands[ARM.op_count >= 3 ? 2 : 1]);
      S.emitIntrinsic(Intrinsic::ArmVrsqrts, Dst, {A, B});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
