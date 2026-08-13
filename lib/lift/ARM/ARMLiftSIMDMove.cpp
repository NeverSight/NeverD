//===- ARMLiftSIMDMove.cpp - ARM32 VFP/NEON move lifter ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The VMOV/VMOVX family: lane insert and extract, GPR pair transfer
/// and the broadcast immediate forms.
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

bool liftSIMDMove(ARMLifter &L, ARMLifter::LiftState &S, const cs_insn *Insn,
                  const cs_arm &ARM) {
  switch (Insn->id) {
  case ARM_INS_VMOV:
  case ARM_INS_VMOVX: {
    if (ARM.op_count < 2)
      break;
    auto &DstOp = ARM.operands[0];
    auto &SrcOp = ARM.operands[ARM.op_count - 1];
    NdVar Src = L.operandRead(S, SrcOp);

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
        NdVar Dst = L.operandWrite(DstOp);
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
    NdVar Dst = L.operandWrite(DstOp);
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
        std::memcpy(&fbits, &fval, 4);
        S.emit(NdOp::COPY, Dst, {NdVar::cst(fbits, 4)});
      } else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
