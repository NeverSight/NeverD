//===- ARMLiftSIMDArith.cpp - ARM32 VFP/NEON arithmetic lifter -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-lane VFP/NEON arithmetic: VADD, VSUB, VMUL, VDIV, VNEG, VABS
/// and VSQRT.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool liftSIMDArith(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                   const cs_arm &ARM) {
  switch (Insn->id) {
  // ========================================================================
  // VFP (Floating Point)
  // ========================================================================
  case ARM_INS_VADD:
  case ARM_INS_VSUB:
  case ARM_INS_VMUL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A, B;
    if (ARM.op_count >= 3) {
      A = L.operandRead(S, ARM.operands[1]);
      B = L.operandRead(S, ARM.operands[2]);
    } else {
      A = L.operandRead(S, ARM.operands[0]);
      B = L.operandRead(S, ARM.operands[1]);
    }
    auto VD = ARM.vector_data;
    // vmul.p8 — polynomial (carry-less) per-byte GF(2) multiply, NOT integer
    // multiply.  Map to the ARM NEON intrinsic (same width, i8 lanes).
    if (Insn->id == ARM_INS_VMUL && VD == ARM_VECTORDATA_P8) {
      S.emitIntrinsic(Intrinsic::ArmVmulp, Dst, {A, B});
      break;
    }
    bool IsFloat = (VD == ARM_VECTORDATA_F32 || VD == ARM_VECTORDATA_F64 ||
                    VD == ARM_VECTORDATA_F16);
    NdOp IntOp, FpOp;
    switch (Insn->id) {
    case ARM_INS_VADD:
      IntOp = NdOp::INT_ADD;
      FpOp = NdOp::FLOAT_ADD;
      break;
    case ARM_INS_VSUB:
      IntOp = NdOp::INT_SUB;
      FpOp = NdOp::FLOAT_SUB;
      break;
    default:
      IntOp = NdOp::INT_MULT;
      FpOp = NdOp::FLOAT_MULT;
      break;
    }
    unsigned LaneSz = 0;
    if (!IsFloat) {
      if (VD == ARM_VECTORDATA_I8 || VD == ARM_VECTORDATA_S8 ||
          VD == ARM_VECTORDATA_U8)
        LaneSz = 1;
      else if (VD == ARM_VECTORDATA_I16 || VD == ARM_VECTORDATA_S16 ||
               VD == ARM_VECTORDATA_U16)
        LaneSz = 2;
      else if (VD == ARM_VECTORDATA_I32 || VD == ARM_VECTORDATA_S32 ||
               VD == ARM_VECTORDATA_U32)
        LaneSz = 4;
    } else {
      if (VD == ARM_VECTORDATA_F32)
        LaneSz = 4;
      else if (VD == ARM_VECTORDATA_F64)
        LaneSz = 8;
    }
    if (LaneSz == 0) {
      // capstone frequently leaves vector_data == INVALID for
      // `vadd/vsub/vmul.iN` (q-register forms especially).  Without recovering
      // the element width the op fell back to a single full-width INT_ADD/SUB
      // on the whole 8/16-byte register, propagating carries/borrows across
      // lanes (e.g. adding a broadcast -4096 to a small positive lane corrupts
      // the next lane). Recover the lane width (and float-ness) from the
      // mnemonic suffix.
      llvm::StringRef M(Insn->mnemonic);
      size_t Dot = M.rfind('.');
      if (Dot != llvm::StringRef::npos) {
        llvm::StringRef Suf = M.substr(Dot + 1);
        if (!Suf.empty() && Suf[0] == 'f')
          IsFloat = true;
        while (!Suf.empty() && (Suf[0] == 'i' || Suf[0] == 's' ||
                                Suf[0] == 'u' || Suf[0] == 'f'))
          Suf = Suf.drop_front();
        unsigned Bits = 0;
        if (!Suf.getAsInteger(10, Bits) && Bits >= 8)
          LaneSz = Bits / 8;
      }
    }
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp Op = IsFloat ? FpOp : IntOp;
      // `vmul.iN/.fN qD, qN, dM[idx]` — multiply every lane by ONE broadcast
      // scalar lane.  operandRead returns the whole Dm register, so detect the
      // indexed form via the operand's lane index and splat that single lane;
      // a plain per-lane SUBBYTES would instead walk dM[0],dM[1],… and read
      // past the register.
      int BLane = -1;
      if (ARM.op_count >= 3) {
        const auto &BOp = ARM.operands[2];
        BLane = BOp.neon_lane >= 0 ? BOp.neon_lane : BOp.vector_index;
      }
      NdVar ScalarB;
      if (BLane >= 0) {
        ScalarB = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, ScalarB,
               {B, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
      }
      NdVar Acc = NdVar::cst(0, 0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LA,
               {A, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
        NdVar LB = ScalarB;
        if (BLane < 0) {
          LB = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, LB,
                 {B, NdVar::cst(static_cast<uint64_t>(I) * LaneSz, 4)});
        }
        NdVar LR = S.makeTemp(LaneSz);
        S.emit(Op, LR, {LA, LB});
        if (I == 0)
          Acc = LR;
        else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {LR, Acc});
          Acc = Next;
        }
      }
      if (Acc.Size < Dst.Size)
        S.emit(NdOp::INT_ZEXT, Dst, {Acc});
      else
        S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(IsFloat ? FpOp : IntOp, Dst, {A, B});
    }
    break;
  }
  case ARM_INS_VDIV: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar A = L.operandRead(S, ARM.operands[1]);
    NdVar B = L.operandRead(S, ARM.operands[2]);
    S.emit(NdOp::FLOAT_DIV, Dst, {A, B});
    break;
  }
  case ARM_INS_VNEG: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    auto VD = ARM.vector_data;
    bool IsFloat = (VD == ARM_VECTORDATA_F32 || VD == ARM_VECTORDATA_F64 ||
                    VD == ARM_VECTORDATA_F16);
    if (IsFloat) {
      // Vector `vneg.f32 q`/`vneg.f64 q` negates EACH lane; a single FLOAT_NEG
      // on the whole register treats 16 bytes as one FP value and corrupts the
      // lanes (only the scalar Sn/Dn form is a single value).
      unsigned LaneSz = (VD == ARM_VECTORDATA_F64)   ? 8
                        : (VD == ARM_VECTORDATA_F32) ? 4
                                                     : 0;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Neg = S.makeTemp(LaneSz);
          S.emit(NdOp::FLOAT_NEG, Neg, {La});
          if (I == 0) {
            Acc = Neg;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Neg, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::FLOAT_NEG, Dst, {Src});
      }
    } else {
      unsigned LaneSz = 0;
      if (VD == ARM_VECTORDATA_S8 || VD == ARM_VECTORDATA_I8)
        LaneSz = 1;
      else if (VD == ARM_VECTORDATA_S16 || VD == ARM_VECTORDATA_I16)
        LaneSz = 2;
      else if (VD == ARM_VECTORDATA_S32 || VD == ARM_VECTORDATA_I32)
        LaneSz = 4;
      if (LaneSz > 0 && Src.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = NdVar::cst(0, 0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Neg = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_NEG2, Neg, {La});
          if (I == 0)
            Acc = Neg;
          else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Neg, Acc});
            Acc = Next;
          }
        }
        if (Acc.Size < Dst.Size)
          S.emit(NdOp::INT_ZEXT, Dst, {Acc});
        else
          S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::INT_NEG2, Dst, {Src});
      }
    }
    break;
  }
  case ARM_INS_VABS: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    auto VD = ARM.vector_data;
    bool IsFloat = (VD == ARM_VECTORDATA_F32 || VD == ARM_VECTORDATA_F64 ||
                    VD == ARM_VECTORDATA_F16);
    if (IsFloat) {
      // Per-lane for vector `vabs.f32 q`/`vabs.f64 q`; whole-register FLOAT_ABS
      // would treat 16 bytes as one FP value (only Sn/Dn scalar is one value).
      unsigned LaneSz = (VD == ARM_VECTORDATA_F64)   ? 8
                        : (VD == ARM_VECTORDATA_F32) ? 4
                                                     : 0;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar Abs = S.makeTemp(LaneSz);
          S.emit(NdOp::FLOAT_ABS, Abs, {La});
          if (I == 0) {
            Acc = Abs;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Abs, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::FLOAT_ABS, Dst, {Src});
      }
    } else {
      unsigned LaneSz = 0;
      if (VD == ARM_VECTORDATA_S8 || VD == ARM_VECTORDATA_I8)
        LaneSz = 1;
      else if (VD == ARM_VECTORDATA_S16 || VD == ARM_VECTORDATA_I16)
        LaneSz = 2;
      else if (VD == ARM_VECTORDATA_S32 || VD == ARM_VECTORDATA_I32)
        LaneSz = 4;
      if (LaneSz > 0 && Src.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
          NdVar IsNeg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsNeg, {La, NdVar::cst(0, LaneSz)});
          NdVar Neg = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_NEG2, Neg, {La});
          NdVar Abs = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, Abs, {IsNeg, Neg, La});
          if (I == 0) {
            Acc = Abs;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Abs, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Src, NdVar::cst(0, Src.Size)});
        NdVar Neg = S.makeTemp(Src.Size);
        S.emit(NdOp::INT_NEG2, Neg, {Src});
        S.emit(NdOp::SELECT, Dst, {IsNeg, Neg, Src});
      }
    }
    break;
  }
  case ARM_INS_VSQRT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM.operands[0]);
    NdVar Src = L.operandRead(S, ARM.operands[1]);
    S.emit(NdOp::FLOAT_SQRT, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
