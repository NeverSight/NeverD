//===- ARMLiftSIMD.cpp - ARM32 VFP/NEON instruction lifter --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// VFP and NEON (Advanced SIMD) instruction handlers for ARM32.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftSIMD(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  switch (Insn->id) {

  // ========================================================================
  // VFP (Floating Point)
  // ========================================================================
  case ARM_INS_VADD:
  case ARM_INS_VSUB:
  case ARM_INS_VMUL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A, B;
    if (ARM.op_count >= 3) {
      A = operandRead(S, ARM.operands[1]);
      B = operandRead(S, ARM.operands[2]);
    } else {
      A = operandRead(S, ARM.operands[0]);
      B = operandRead(S, ARM.operands[1]);
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
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar LB = ScalarB;
        if (BLane < 0) {
          LB = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LaneSz, 4)});
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    S.emit(NdOp::FLOAT_DIV, Dst, {A, B});
    break;
  }
  case ARM_INS_VNEG: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
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
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emit(NdOp::FLOAT_SQRT, Dst, {Src});
    break;
  }
  case ARM_INS_VMOV:
  case ARM_INS_VMOVX: {
    if (ARM.op_count < 2)
      break;
    auto &DstOp = ARM.operands[0];
    auto &SrcOp = ARM.operands[ARM.op_count - 1];
    NdVar Src = operandRead(S, SrcOp);

    int DstLane = DstOp.neon_lane >= 0 ? DstOp.neon_lane : DstOp.vector_index;
    if (DstOp.type == ARM_OP_REG && DstLane >= 0) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(DstOp.reg));
      if (RI.Size > 0) {
        unsigned ElemSz = 0;
        switch (ARM.vector_data) {
        case ARM_VECTORDATA_I8:
        case ARM_VECTORDATA_S8:
        case ARM_VECTORDATA_U8:
          ElemSz = 1;
          break;
        case ARM_VECTORDATA_I16:
        case ARM_VECTORDATA_S16:
        case ARM_VECTORDATA_U16:
          ElemSz = 2;
          break;
        case ARM_VECTORDATA_I32:
        case ARM_VECTORDATA_S32:
        case ARM_VECTORDATA_U32:
        case ARM_VECTORDATA_F32:
          ElemSz = 4;
          break;
        default:
          if (ARM.vector_size > 0)
            ElemSz = ARM.vector_size / 8;
          else
            ElemSz = Src.Size;
          break;
        }
        if (ElemSz == 0)
          ElemSz = Src.Size;
        unsigned ByteOff = static_cast<unsigned>(DstLane) * ElemSz;
        while (ByteOff + ElemSz > RI.Size && ElemSz > 1) {
          ElemSz /= 2;
          ByteOff = static_cast<unsigned>(DstLane) * ElemSz;
        }
        NdVar DReg = NdVar::reg(RI.Offset, RI.Size);
        NdVar Val = Src;
        if (Src.Size > ElemSz) {
          Val = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Val, {Src, NdVar::cst(0, 4)});
        }
        if (ByteOff == 0 && ByteOff + ElemSz == RI.Size) {
          S.emit(NdOp::COPY, DReg, {Val});
        } else if (ByteOff == 0) {
          NdVar Hi = S.makeTemp(RI.Size - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {DReg, NdVar::cst(ElemSz, 4)});
          S.emit(NdOp::CONCAT, DReg, {Hi, Val});
        } else if (ByteOff + ElemSz == RI.Size) {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {DReg, NdVar::cst(0, 4)});
          S.emit(NdOp::CONCAT, DReg, {Val, Lo});
        } else {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {DReg, NdVar::cst(0, 4)});
          NdVar Hi = S.makeTemp(RI.Size - ByteOff - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {DReg, NdVar::cst(ByteOff + ElemSz, 4)});
          NdVar Mid = S.makeTemp(ByteOff + ElemSz);
          S.emit(NdOp::CONCAT, Mid, {Val, Lo});
          S.emit(NdOp::CONCAT, DReg, {Hi, Mid});
        }
        break;
      }
    }
    int SrcLane = SrcOp.neon_lane >= 0 ? SrcOp.neon_lane : SrcOp.vector_index;
    if (SrcOp.type == ARM_OP_REG && SrcLane >= 0) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(SrcOp.reg));
      unsigned ElemSz = 0;
      switch (ARM.vector_data) {
      case ARM_VECTORDATA_I8:
      case ARM_VECTORDATA_S8:
      case ARM_VECTORDATA_U8:
        ElemSz = 1;
        break;
      case ARM_VECTORDATA_I16:
      case ARM_VECTORDATA_S16:
      case ARM_VECTORDATA_U16:
        ElemSz = 2;
        break;
      case ARM_VECTORDATA_I32:
      case ARM_VECTORDATA_S32:
      case ARM_VECTORDATA_U32:
      case ARM_VECTORDATA_F32:
        ElemSz = 4;
        break;
      default:
        if (ARM.vector_size > 0)
          ElemSz = ARM.vector_size / 8;
        else
          ElemSz = (RI.Size > 0 && SrcLane > 0)
                       ? RI.Size / (static_cast<unsigned>(SrcLane) + 1)
                       : Src.Size;
        break;
      }
      if (ElemSz == 0)
        ElemSz = Src.Size;
      while (static_cast<unsigned>(SrcLane) * ElemSz + ElemSz > RI.Size &&
             ElemSz > 1)
        ElemSz /= 2;
      if (RI.Size > 0 && RI.Size > ElemSz) {
        unsigned ByteOff = static_cast<unsigned>(SrcLane) * ElemSz;
        NdVar DReg = NdVar::reg(RI.Offset, RI.Size);
        NdVar Lane = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Lane, {DReg, NdVar::cst(ByteOff, 4)});
        NdVar Dst = operandWrite(DstOp);
        // `vmov.s16/.s8 rN, dM[i]` sign-extends the lane into the GPR; the
        // unsigned (.u*) form zero-extends.  Previously this always
        // zero-extended, so negative lanes (e.g. in an i16 dot product) lost
        // their sign and produced wrong high bits.
        bool IsSigned = (ARM.vector_data == ARM_VECTORDATA_S8 ||
                         ARM.vector_data == ARM_VECTORDATA_S16 ||
                         ARM.vector_data == ARM_VECTORDATA_S32);
        if (Dst.Size > ElemSz)
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Lane});
        else
          S.emit(NdOp::COPY, Dst, {Lane});
        break;
      }
    }
    // vmov r0, r1, dn — extract two GPRs from a D register
    if (ARM.op_count == 3 && DstOp.type == ARM_OP_REG &&
        ARM.operands[1].type == ARM_OP_REG && SrcOp.type == ARM_OP_REG) {
      auto RI0 = mapCapstoneReg(static_cast<arm_reg>(DstOp.reg));
      auto RI1 = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[1].reg));
      if (RI0.Size == 4 && RI1.Size == 4 && Src.Size == 8) {
        NdVar Lo = NdVar::reg(RI0.Offset, 4);
        NdVar Hi = NdVar::reg(RI1.Offset, 4);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        S.emit(NdOp::SUBBYTES, Hi, {Src, NdVar::cst(4, 4)});
        break;
      }
      // vmov dN, rLo, rHi — combine two GPRs into a D register (VMOVDRR, the
      // soft-float ABI's way to materialise a double from a register pair).
      // Without this the generic fallthrough copied only the last source GPR
      // (the high half) into the 8-byte D reg and dropped the low half.
      if (RI0.Size == 8 && RI1.Size == 4) {
        auto RI2 = mapCapstoneReg(static_cast<arm_reg>(SrcOp.reg));
        if (RI2.Size == 4) {
          NdVar Lo = NdVar::reg(RI1.Offset, 4);
          NdVar Hi = NdVar::reg(RI2.Offset, 4);
          S.emit(NdOp::CONCAT, NdVar::reg(RI0.Offset, 8), {Hi, Lo});
          break;
        }
      }
    }
    NdVar Dst = operandWrite(DstOp);
    // vmov.iN qn, #imm — broadcast immediate to all lanes
    if (SrcOp.type == ARM_OP_IMM && Dst.Size > 4) {
      unsigned LaneSz = 0;
      auto VD = ARM.vector_data;
      if (VD == ARM_VECTORDATA_I8 || VD == ARM_VECTORDATA_S8 ||
          VD == ARM_VECTORDATA_U8)
        LaneSz = 1;
      else if (VD == ARM_VECTORDATA_I16 || VD == ARM_VECTORDATA_S16 ||
               VD == ARM_VECTORDATA_U16)
        LaneSz = 2;
      else if (VD == ARM_VECTORDATA_I32 || VD == ARM_VECTORDATA_S32 ||
               VD == ARM_VECTORDATA_U32 || VD == ARM_VECTORDATA_F32)
        LaneSz = 4;
      if (LaneSz == 0) {
        // Capstone often leaves vector_data INVALID for `vmov.iN dN/qN, #imm`;
        // recover the element width from the mnemonic suffix
        // (".i16"/".i8"/...). Without this the broadcast was skipped entirely,
        // so the immediate landed only in lane 0 (e.g. `vmov.i16 d,#1` for a
        // rounding `+1`).
        llvm::StringRef M(Insn->mnemonic);
        size_t Dot = M.rfind('.');
        if (Dot != llvm::StringRef::npos) {
          llvm::StringRef Suf = M.substr(Dot + 1);
          while (!Suf.empty() && (Suf[0] == 'i' || Suf[0] == 's' ||
                                  Suf[0] == 'u' || Suf[0] == 'f'))
            Suf = Suf.drop_front();
          unsigned Bits = 0;
          if (!Suf.getAsInteger(10, Bits) && Bits >= 8)
            LaneSz = Bits / 8;
        }
      }
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        // A 64-bit lane mask via `(1ULL << 64) - 1` is shift-by-bitwidth UB
        // (evaluates to 0 on the host), which silently zeroed `vmov.i64` lanes.
        uint64_t LaneMask =
            (LaneSz >= 8) ? ~0ULL : ((1ULL << (LaneSz * 8)) - 1);
        uint64_t Imm = static_cast<uint64_t>(SrcOp.imm) & LaneMask;
        uint64_t Lo = 0, Hi = 0;
        unsigned NLanes = Dst.Size / LaneSz;
        for (unsigned I = 0; I < NLanes && I * LaneSz < 8; ++I)
          Lo |= Imm << (I * LaneSz * 8);
        for (unsigned I = 0; I * LaneSz < Dst.Size; ++I) {
          if (I * LaneSz >= 8)
            Hi |= Imm << ((I * LaneSz - 8) * 8);
        }
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
    }
    if (SrcOp.type == ARM_OP_FP && Dst.Size < Src.Size) {
      if (Dst.Size == 4) {
        float fval = static_cast<float>(SrcOp.fp);
        uint32_t fbits;
        __builtin_memcpy(&fbits, &fval, 4);
        S.emit(NdOp::COPY, Dst, {NdVar::cst(fbits, 4)});
      } else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VCMP:
  case ARM_INS_VCMPE: {
    if (ARM.op_count < 2)
      break;
    NdVar A = operandRead(S, ARM.operands[0]);
    NdVar B = operandRead(S, ARM.operands[1]);
    // VCMP writes the FPSCR flags (not the CPSR flags); a later VMRS commits
    // them to CPSR.  Writing CPSR directly here would let a second VCMP clobber
    // the flags a pending conditional still needs (d_minmax: two vcmp before
    // the first conditional move consumes the first vcmp's result).
    // N = a < b
    S.emit(NdOp::FLOAT_LESS, NdVar::reg(armreg::FP_NFLAG, 1), {A, B});
    // Z = a == b
    S.emit(NdOp::FLOAT_EQUAL, NdVar::reg(armreg::FP_ZFLAG, 1), {A, B});
    // C = !(a < b) (i.e., a >= b or unordered)
    NdVar Lt = S.makeTemp(1);
    S.emit(NdOp::FLOAT_LESS, Lt, {A, B});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(armreg::FP_CFLAG, 1), {Lt});
    // V = unordered (either operand is NaN)
    NdVar NanA = S.makeTemp(1);
    NdVar NanB = S.makeTemp(1);
    S.emit(NdOp::FLOAT_ISNAN, NanA, {A});
    S.emit(NdOp::FLOAT_ISNAN, NanB, {B});
    S.emit(NdOp::BOOL_OR, NdVar::reg(armreg::FP_VFLAG, 1), {NanA, NanB});
    break;
  }
  case ARM_INS_VCVT:
  case ARM_INS_VCVTB:
  case ARM_INS_VCVTT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdOp Op = NdOp::FLOAT_FLOAT2FLOAT;
    bool Is32 = false; // 32-bit-element int<->float conversion (NEON per-lane)
    switch (ARM.vector_data) {
    case ARM_VECTORDATA_F32S32:
      Is32 = true;
      [[fallthrough]];
    case ARM_VECTORDATA_F32S16:
    case ARM_VECTORDATA_F64S32:
    case ARM_VECTORDATA_F64S16:
      Op = NdOp::FLOAT_INT2FLOAT;
      break;
    case ARM_VECTORDATA_F32U32:
      Is32 = true;
      [[fallthrough]];
    case ARM_VECTORDATA_F32U16:
    case ARM_VECTORDATA_F64U32:
    case ARM_VECTORDATA_F64U16:
      // Unsigned int -> float must use the unsigned conversion (the old code
      // used the signed FLOAT_INT2FLOAT for `vcvt.f32.u32`).
      Op = NdOp::FLOAT_UINT2FLOAT;
      break;
    case ARM_VECTORDATA_S32F32:
      Is32 = true;
      Op = NdOp::FLOAT_TRUNC;
      break;
    case ARM_VECTORDATA_U32F32:
      // Unsigned float -> int must truncate as unsigned (FPToUI); the signed
      // FLOAT_TRUNC (FPToSI) saturates values in [2^31, 2^32) to INT32_MAX.
      Is32 = true;
      Op = NdOp::FLOAT_FLOAT2UINT;
      break;
    case ARM_VECTORDATA_S32F64:
    case ARM_VECTORDATA_S16F32:
    case ARM_VECTORDATA_S16F64:
      Op = NdOp::FLOAT_TRUNC;
      break;
    case ARM_VECTORDATA_U32F64:
    case ARM_VECTORDATA_U16F32:
    case ARM_VECTORDATA_U16F64:
      Op = NdOp::FLOAT_FLOAT2UINT;
      break;
    default:
      Op = NdOp::FLOAT_FLOAT2FLOAT;
      break;
    }
    // NEON `vcvt.f32.s32 q,q` / `vcvt.s32.f32 q,q` etc. are PER-LANE 32-bit
    // conversions; a single op on the whole D/Q register converts the entire
    // 64/128-bit value as one scalar (VectorAlgo8 ARM32 FP read garbage).
    if (Is32 && Dst.Size > 4) {
      unsigned LaneSz = 4;
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Cvt = S.makeTemp(LaneSz);
        S.emit(Op, Cvt, {Lane});
        if (I == 0) {
          Acc = Cvt;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Cvt, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(Op, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VCVTR:
  case ARM_INS_VJCVT: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Src});
    break;
  }
  case ARM_INS_VCVTA:
  case ARM_INS_VCVTN: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Rounded = S.makeTemp(Src.Size);
    // VCVTA rounds ties away from zero; VCVTN ties to even (IEEE default).
    S.emit(Insn->id == ARM_INS_VCVTA ? NdOp::FLOAT_ROUND
                                     : NdOp::FLOAT_ROUNDEVEN,
           Rounded, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Rounded});
    break;
  }
  case ARM_INS_VCVTM: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Floored = S.makeTemp(Src.Size);
    S.emit(NdOp::FLOAT_FLOOR, Floored, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Floored});
    break;
  }
  case ARM_INS_VCVTP: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Ceiled = S.makeTemp(Src.Size);
    S.emit(NdOp::FLOAT_CEIL, Ceiled, {Src});
    S.emit(NdOp::FLOAT_FLOAT2INT, Dst, {Ceiled});
    break;
  }
  case ARM_INS_VFMA:
  case ARM_INS_VFMAB:
  case ARM_INS_VFMAT:
  case ARM_INS_VFMAS:
  case ARM_INS_VFMAL:
  case ARM_INS_VFMS:
  case ARM_INS_VFMSL: {
    if (ARM.op_count < 3)
      break;
    bool IsSub = (Insn->id == ARM_INS_VFMS || Insn->id == ARM_INS_VFMSL);
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Old = NdVar::reg(Dst.Offset, Dst.Size);
    // NEON `vfma.f32 q,q,q` / `d,d,d` fuse multiply-add PER-LANE; a single
    // FLOAT_MULT/ADD on the whole D/Q register does one scalar FP op on the
    // packed bits (VectorAlgo8 ARM32 fdot/fmla read 0).
    unsigned LaneSz = (ARM.vector_data == ARM_VECTORDATA_F64) ? 8 : 4;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        unsigned Off = I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz),
                Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Ld, {Old, NdVar::cst(Off, 4)});
        // VFMA/VFMS are FUSED (single rounding): Ld + La*Lb = fma(La,Lb,Ld),
        // Ld - La*Lb = fma(-La,Lb,Ld).  Separate FMUL+FADD would round twice.
        NdVar Mul = La;
        if (IsSub) {
          Mul = S.makeTemp(LaneSz);
          S.emit(NdOp::FLOAT_NEG, Mul, {La});
        }
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_FMA, Lr, {Mul, Lb, Ld});
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
      NdVar Mul = A;
      if (IsSub) {
        Mul = S.makeTemp(Dst.Size);
        S.emit(NdOp::FLOAT_NEG, Mul, {A});
      }
      S.emit(NdOp::FLOAT_FMA, Dst, {Mul, B, Old});
    }
    break;
  }
  case ARM_INS_VNMUL: {
    // VNMUL: Dst = -(a * b)
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_NEG, Dst, {Prod});
    break;
  }
  case ARM_INS_VFNMA: {
    // VFNMA is FUSED: Dst = -(Dst + a*b) = -fma(a,b,Dst) (single rounding).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Sum = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_FMA, Sum, {A, B, NdVar::reg(Dst.Offset, Dst.Size)});
    S.emit(NdOp::FLOAT_NEG, Dst, {Sum});
    break;
  }
  case ARM_INS_VFNMS: {
    // VFNMS is FUSED: Dst = a*b - Dst = fma(a,b,-Dst) (single rounding).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar NegOld = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NegOld, {NdVar::reg(Dst.Offset, Dst.Size)});
    S.emit(NdOp::FLOAT_FMA, Dst, {A, B, NegOld});
    break;
  }
  case ARM_INS_VNMLA: {
    // VNMLA is NOT fused (two roundings): Dst = -(Dst + a*b).
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    NdVar NegOld = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NegOld, {NdVar::reg(Dst.Offset, Dst.Size)});
    S.emit(NdOp::FLOAT_SUB, Dst, {NegOld, Prod});
    break;
  }
  case ARM_INS_VNMLS: {
    // VNMLS is NOT fused (two roundings): Dst = a*b - Dst.
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_MULT, Prod, {A, B});
    S.emit(NdOp::FLOAT_SUB, Dst, {Prod, NdVar::reg(Dst.Offset, Dst.Size)});
    break;
  }
  case ARM_INS_VSELEQ:
  case ARM_INS_VSELGE:
  case ARM_INS_VSELGT:
  case ARM_INS_VSELVS: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Cond = S.makeTemp(1);
    switch (Insn->id) {
    case ARM_INS_VSELEQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::ZFLAG, 1)});
      break;
    case ARM_INS_VSELGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
      break;
    case ARM_INS_VSELGT: {
      NdVar NZ = S.makeTemp(1);
      NdVar NvEq = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(armreg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, NvEq,
             {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, NvEq});
      break;
    }
    case ARM_INS_VSELVS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::VFLAG, 1)});
      break;
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }
    S.emit(NdOp::SELECT, Dst, {Cond, A, B});
    break;
  }
  // VMAX/VMIN and the NaN-aware VMAXNM/VMINNM.  These are per-lane and may be
  // integer (.s8/.u16/.s32/...) or floating-point (.f32/.f64).  The previous
  // implementation unconditionally used a single full-width FLOAT_LESSEQUAL,
  // which is wrong for integer data (e.g. `vmin.s32 q,q,q`) on two counts: it
  // reinterprets integer lanes as floats and it does not isolate the lanes.
  case ARM_INS_VMAXNM:
  case ARM_INS_VMAXNMA:
  case ARM_INS_VMAX:
  case ARM_INS_VMAXA:
  case ARM_INS_VMINNM:
  case ARM_INS_VMINNMA:
  case ARM_INS_VMIN:
  case ARM_INS_VMINA: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    bool IsMin = (Insn->id == ARM_INS_VMIN || Insn->id == ARM_INS_VMINA ||
                  Insn->id == ARM_INS_VMINNM || Insn->id == ARM_INS_VMINNMA);
    bool IsNM = (Insn->id == ARM_INS_VMAXNM || Insn->id == ARM_INS_VMAXNMA ||
                 Insn->id == ARM_INS_VMINNM || Insn->id == ARM_INS_VMINNMA);
    unsigned LaneSz = 0;
    bool IsSigned = false, IsFloat = IsNM;
    switch (ARM.vector_data) {
    case ARM_VECTORDATA_S8:
      LaneSz = 1;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U8:
    case ARM_VECTORDATA_I8:
      LaneSz = 1;
      break;
    case ARM_VECTORDATA_S16:
      LaneSz = 2;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U16:
    case ARM_VECTORDATA_I16:
      LaneSz = 2;
      break;
    case ARM_VECTORDATA_S32:
      LaneSz = 4;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U32:
    case ARM_VECTORDATA_I32:
      LaneSz = 4;
      break;
    case ARM_VECTORDATA_S64:
      LaneSz = 8;
      IsSigned = true;
      break;
    case ARM_VECTORDATA_U64:
    case ARM_VECTORDATA_I64:
      LaneSz = 8;
      break;
    case ARM_VECTORDATA_F32:
      LaneSz = 4;
      IsFloat = true;
      break;
    case ARM_VECTORDATA_F64:
      LaneSz = 8;
      IsFloat = true;
      break;
    default:
      break;
    }
    // Capstone leaves vector_data INVALID for VMINNM/VMAXNM, so the f32/f64
    // element width is only in the mnemonic suffix; a size-based guess wrongly
    // picked f64 (8) for the 16-byte Q-register vminnm.f32 form.
    if (IsFloat && LaneSz == 0)
      LaneSz = llvm::StringRef(Insn->mnemonic).contains(".f64") ? 8 : 4;
    auto cmpOp = [&]() { return IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS; };
    // Float VMIN/VMAX are IEEE minimum/maximum (NaN-propagating, -0 < +0); the
    // NM variants are minNum/maxNum (NaN-suppressing).  A bare
    // FLOAT_LESS+SELECT got both NaN and signed-zero wrong.  Integer lanes keep
    // the compare/select.
    NdOp FMM = IsNM ? (IsMin ? NdOp::FLOAT_MINNUM : NdOp::FLOAT_MAXNUM)
                    : (IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX);
    auto emitLane = [&](NdVar Out, NdVar La, NdVar Lb) {
      if (IsFloat) {
        S.emit(FMM, Out, {La, Lb});
      } else {
        NdVar Cond = S.makeTemp(1);
        if (IsMin)
          S.emit(cmpOp(), Cond, {La, Lb});
        else
          S.emit(cmpOp(), Cond, {Lb, La});
        S.emit(NdOp::SELECT, Out, {Cond, La, Lb});
      }
    };
    if (LaneSz > 0 && Dst.Size >= LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Sel = S.makeTemp(LaneSz);
        emitLane(Sel, La, Lb);
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      emitLane(Dst, A, B);
    }
    break;
  }
  // VRINT{A,M,N,P,R,X,Z} — round float to integral float.  A ties away, N/R/X
  // tie to even (R/X follow FPSCR, default even), M floor, P ceil, Z toward
  // zero.  Each had been collapsed to round-half-away, which is wrong for all
  // but VRINTA.
  case ARM_INS_VRINTA:
  case ARM_INS_VRINTM:
  case ARM_INS_VRINTN:
  case ARM_INS_VRINTP:
  case ARM_INS_VRINTR:
  case ARM_INS_VRINTX:
  case ARM_INS_VRINTZ: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    if (Insn->id == ARM_INS_VRINTZ) {
      NdVar Fl = S.makeTemp(Dst.Size), Ce = S.makeTemp(Dst.Size),
              IsNeg = S.makeTemp(1);
      S.emit(NdOp::FLOAT_FLOOR, Fl, {Src});
      S.emit(NdOp::FLOAT_CEIL, Ce, {Src});
      S.emit(NdOp::FLOAT_LESS, IsNeg, {Src, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::SELECT, Dst, {IsNeg, Ce, Fl});
    } else {
      NdOp Ro;
      switch (Insn->id) {
      case ARM_INS_VRINTM:
        Ro = NdOp::FLOAT_FLOOR;
        break;
      case ARM_INS_VRINTP:
        Ro = NdOp::FLOAT_CEIL;
        break;
      case ARM_INS_VRINTA:
        Ro = NdOp::FLOAT_ROUND;
        break;
      default:
        Ro = NdOp::FLOAT_ROUNDEVEN;
        break;
      }
      S.emit(Ro, Dst, {Src});
    }
    break;
  }
  case ARM_INS_VMRS:
    // `vmrs APSR_nzcv, fpscr` (a.k.a. fmstat) commits the FPSCR flags VCMP
    // produced into the CPSR flags that conditional instructions read.
    S.emit(NdOp::COPY, NdVar::reg(armreg::NFLAG, 1),
           {NdVar::reg(armreg::FP_NFLAG, 1)});
    S.emit(NdOp::COPY, NdVar::reg(armreg::ZFLAG, 1),
           {NdVar::reg(armreg::FP_ZFLAG, 1)});
    S.emit(NdOp::COPY, NdVar::reg(armreg::CFLAG, 1),
           {NdVar::reg(armreg::FP_CFLAG, 1)});
    S.emit(NdOp::COPY, NdVar::reg(armreg::VFLAG, 1),
           {NdVar::reg(armreg::FP_VFLAG, 1)});
    break;
  case ARM_INS_VMSR:
    // FPSCR writes are not modeled separately.
    break;

  // VFP load/store — use operandEffAddr to avoid double-LOAD
  case ARM_INS_VLDR:
  case ARM_INS_VLDRB:
  case ARM_INS_VLDRH:
  case ARM_INS_VLDRW:
  case ARM_INS_VLDRD: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[1])
                     : operandRead(S, ARM.operands[1]);
    S.emit(NdOp::LOAD, Dst, {EA});
    break;
  }
  case ARM_INS_VSTR:
  case ARM_INS_VSTRB:
  case ARM_INS_VSTRH:
  case ARM_INS_VSTRW:
  case ARM_INS_VSTRD: {
    if (ARM.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM.operands[0]);
    NdVar EA = (ARM.operands[1].type == ARM_OP_MEM)
                     ? operandEffAddr(S, ARM.operands[1])
                     : operandRead(S, ARM.operands[1]);
    S.emit(NdOp::STORE, {}, {EA, Src});
    break;
  }
  case ARM_INS_VLDMIA:
  case ARM_INS_VLDMDB:
  case ARM_INS_FLDMIAX:
  case ARM_INS_FLDMDBX: {
    // capstone decodes `vpop {regs}` (= vldmia sp!, {regs}) as VLDMIA with
    // mnemonic "vpop" and NO explicit base operand: SP is the implicit base and
    // every operand is a list register.  The generic path below would treat the
    // first restored register as the base, corrupting it.
    if (llvm::StringRef(Insn->mnemonic) == "vpop") {
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      unsigned Off = 0;
      for (int I = 0; I < ARM.op_count; I++) {
        NdVar Dst = operandWrite(ARM.operands[I]);
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(Off, 4)});
        S.emit(NdOp::LOAD, Dst, {Slot});
        Off += Dst.Size;
      }
      S.emit(NdOp::INT_ADD, Sp, {Sp, NdVar::cst(Off, 4)});
      break;
    }
    // VLDM loads a LIST of VFP/NEON registers (S=4B / D=8B) from consecutive
    // memory.  The old handler loaded only operands[1], dropping the rest of
    // the list and the per-register address increment (e.g. `vldmia sp,
    // {d16,d17}` left d17 undefined — garbage in dot-product / VFP spill-reload
    // sequences). DB (decrement-before) starts at base - totalBytes.
    if (ARM.op_count < 2)
      break;
    NdVar Base = operandRead(S, ARM.operands[0]);
    bool IsDB = (Insn->id == ARM_INS_VLDMDB || Insn->id == ARM_INS_FLDMDBX);
    unsigned Total = 0;
    for (int I = 1; I < ARM.op_count; I++)
      Total += operandWrite(ARM.operands[I]).Size;
    int Cur = IsDB ? -static_cast<int>(Total) : 0;
    for (int I = 1; I < ARM.op_count; I++) {
      NdVar Dst = operandWrite(ARM.operands[I]);
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Cur)), 4)});
      S.emit(NdOp::LOAD, Dst, {EA});
      Cur += static_cast<int>(Dst.Size);
    }
    if (Insn->detail->writeback && ARM.operands[0].type == ARM_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
      NdVar BaseReg = NdVar::reg(RI.Offset, 4);
      S.emit(IsDB ? NdOp::INT_SUB : NdOp::INT_ADD, BaseReg,
             {BaseReg, NdVar::cst(Total, 4)});
    }
    break;
  }
  case ARM_INS_VSTMIA:
  case ARM_INS_VSTMDB:
  case ARM_INS_FSTMIAX:
  case ARM_INS_FSTMDBX: {
    // capstone decodes `vpush {regs}` (= vstmdb sp!, {regs}) as VSTMDB with
    // mnemonic "vpush" and NO explicit base operand: SP is the implicit base
    // and every operand is a list register (saved low-to-high at the
    // decremented SP).
    if (llvm::StringRef(Insn->mnemonic) == "vpush") {
      unsigned Total = 0;
      for (int I = 0; I < ARM.op_count; I++)
        Total += operandWrite(ARM.operands[I]).Size;
      NdVar Sp = NdVar::reg(armreg::SP, 4);
      S.emit(NdOp::INT_SUB, Sp, {Sp, NdVar::cst(Total, 4)});
      unsigned Off = 0;
      for (int I = 0; I < ARM.op_count; I++) {
        NdVar R = operandRead(S, ARM.operands[I]);
        NdVar Slot = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Slot, {Sp, NdVar::cst(Off, 4)});
        S.emit(NdOp::STORE, {}, {Slot, R});
        Off += R.Size;
      }
      break;
    }
    if (ARM.op_count < 2)
      break;
    NdVar Base = operandRead(S, ARM.operands[0]);
    bool IsDB = (Insn->id == ARM_INS_VSTMDB || Insn->id == ARM_INS_FSTMDBX);
    unsigned Total = 0;
    for (int I = 1; I < ARM.op_count; I++)
      Total += operandWrite(ARM.operands[I]).Size;
    int Cur = IsDB ? -static_cast<int>(Total) : 0;
    for (int I = 1; I < ARM.op_count; I++) {
      NdVar Src = operandRead(S, ARM.operands[I]);
      NdVar EA = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, EA,
             {Base, NdVar::cst(
                        static_cast<uint64_t>(static_cast<uint32_t>(Cur)), 4)});
      S.emit(NdOp::STORE, {}, {EA, Src});
      Cur += static_cast<int>(Src.Size);
    }
    if (Insn->detail->writeback && ARM.operands[0].type == ARM_OP_REG) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(ARM.operands[0].reg));
      NdVar BaseReg = NdVar::reg(RI.Offset, 4);
      S.emit(IsDB ? NdOp::INT_SUB : NdOp::INT_ADD, BaseReg,
             {BaseReg, NdVar::cst(Total, 4)});
    }
    break;
  }
  case ARM_INS_VLLDM:
  case ARM_INS_VLSTM:
    S.emitIntrinsic(Intrinsic::ArmVscclrm);
    break;
  case ARM_INS_VSCCLRM:
    S.emitIntrinsic(Intrinsic::ArmVscclrm);
    break;

  default:
    return false;
  }
  return true;
}

} // namespace neverd
