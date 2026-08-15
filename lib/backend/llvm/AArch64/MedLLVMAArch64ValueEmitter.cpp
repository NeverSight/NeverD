//===- MedLLVMAArch64ValueEmitter.cpp - AArch64 value intrinsics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64-specific value-producing intrinsic emission (RBIT).
///
/// Side-effect-only intrinsics live in MedLLVMAArch64Sideeffect.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-aarch64-value"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IntrinsicsAArch64.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Bit reversal (RBIT)
//===----------------------------------------------------------------------===//

llvm::Value *
MedLLVMEmitter::emitAArch64IntrinsicValue(const MedOp &Op, Intrinsic IC,
                                          llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  // Scalar FP16 fixed-point conversions have a single architectural rounding
  // step.  Expanding them into separate conversion and multiply operations can
  // overflow in half precision or double-round through a wider FP type.  Keep
  // the exact instruction in the generated AArch64 code instead.
  if ((IC == I::A64_ScvtfFixed || IC == I::A64_UcvtfFixed ||
       IC == I::A64_FcvtzsFixed || IC == I::A64_FcvtzuFixed) &&
      Op.Output.Size > 0 && Op.NumInputs >= 3) {
    bool IntToFP = (IC == I::A64_ScvtfFixed || IC == I::A64_UcvtfFixed);
    bool IsUnsigned = (IC == I::A64_UcvtfFixed || IC == I::A64_FcvtzuFixed);
    const MedVar &Imm = Op.Inputs[2];
    if (!Imm.isConst())
      return nullptr;

    unsigned FBits = static_cast<unsigned>(Imm.ConstVal);
    unsigned GprBytes = IntToFP ? Op.Inputs[1].Size : Op.Output.Size;
    unsigned FpBytes = IntToFP ? Op.Output.Size : Op.Inputs[1].Size;
    if (FpBytes != 2 || (GprBytes != 4 && GprBytes != 8) || FBits == 0 ||
        FBits > GprBytes * 8)
      return nullptr;

    auto *OutTy = sizeToType(Op.Output.Size);
    auto *GprTy = llvm::IntegerType::get(*Ctx, GprBytes * 8);
    auto *I16Ty = llvm::Type::getInt16Ty(*Ctx);
    auto *HalfTy = llvm::Type::getHalfTy(*Ctx);
    auto coerceInteger = [&](llvm::Value *V,
                             llvm::IntegerType *Ty) -> llvm::Value * {
      if (V->getType()->isPointerTy())
        V = Builder.CreatePtrToInt(V, Ty);
      if (V->getType()->isFloatingPointTy()) {
        auto *BitsTy = llvm::IntegerType::get(
            *Ctx, V->getType()->getPrimitiveSizeInBits());
        V = Builder.CreateBitCast(V, BitsTy);
      }
      return (V->getType() == Ty) ? V : Builder.CreateZExtOrTrunc(V, Ty);
    };

    auto *Scale = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), FBits);
    if (IntToFP) {
      llvm::Intrinsic::ID IID =
          IsUnsigned ? llvm::Intrinsic::aarch64_neverd_ucvtf_fixed
                     : llvm::Intrinsic::aarch64_neverd_scvtf_fixed;
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {GprTy});
      llvm::Value *Src = coerceInteger(getVar(Op.Inputs[1], Builder), GprTy);
      llvm::Value *R = Builder.CreateCall(
          Fn, {Src, Scale}, IsUnsigned ? "ucvtf.fixed" : "scvtf.fixed");
      R = Builder.CreateBitCast(R, I16Ty);
      return (R->getType() == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }

    llvm::Intrinsic::ID IID =
        IsUnsigned ? llvm::Intrinsic::aarch64_neverd_fcvtzu_fixed
                   : llvm::Intrinsic::aarch64_neverd_fcvtzs_fixed;
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {GprTy});
    llvm::Value *Bits = coerceInteger(getVar(Op.Inputs[1], Builder), I16Ty);
    llvm::Value *Src = Builder.CreateBitCast(Bits, HalfTy);
    llvm::Value *R = Builder.CreateCall(
        Fn, {Src, Scale}, IsUnsigned ? "fcvtzu.fixed" : "fcvtzs.fixed");
    return (R->getType() == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
  }

  if (IC == I::A64_Rbit && Op.Output.Size > 0) {
    auto *OutTy = sizeToType(Op.Output.Size);
    if (Op.NumInputs >= 2) {
      auto *Src = getVar(Op.Inputs[1], Builder);
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::bitreverse, {Src->getType()});
      llvm::Value *R = Builder.CreateCall(Fn, {Src}, "rbit");
      if (R->getType() != OutTy)
        R = Builder.CreateZExtOrTrunc(R, OutTy);
      return R;
    }
    return llvm::ConstantInt::get(OutTy, 0);
  }

  if (IC == I::A64_Crc32b || IC == I::A64_Crc32h || IC == I::A64_Crc32w ||
      IC == I::A64_Crc32x || IC == I::A64_Crc32cb || IC == I::A64_Crc32ch ||
      IC == I::A64_Crc32cw || IC == I::A64_Crc32cx) {
    if (Op.NumInputs < 3)
      return nullptr;
    auto *AccRaw = getVar(Op.Inputs[1], Builder);
    auto *SrcRaw = getVar(Op.Inputs[2], Builder);
    auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
    auto *Acc = Builder.CreateTrunc(AccRaw, I32Ty);

    llvm::Intrinsic::ID IID;
    llvm::Type *SrcTy;
    bool IsCastanea = (IC == I::A64_Crc32cb || IC == I::A64_Crc32ch ||
                       IC == I::A64_Crc32cw || IC == I::A64_Crc32cx);
    if (IC == I::A64_Crc32b || IC == I::A64_Crc32cb) {
      IID = IsCastanea ? llvm::Intrinsic::aarch64_crc32cb
                       : llvm::Intrinsic::aarch64_crc32b;
      SrcTy = I32Ty;
    } else if (IC == I::A64_Crc32h || IC == I::A64_Crc32ch) {
      IID = IsCastanea ? llvm::Intrinsic::aarch64_crc32ch
                       : llvm::Intrinsic::aarch64_crc32h;
      SrcTy = I32Ty;
    } else if (IC == I::A64_Crc32w || IC == I::A64_Crc32cw) {
      IID = IsCastanea ? llvm::Intrinsic::aarch64_crc32cw
                       : llvm::Intrinsic::aarch64_crc32w;
      SrcTy = I32Ty;
    } else {
      IID = IsCastanea ? llvm::Intrinsic::aarch64_crc32cx
                       : llvm::Intrinsic::aarch64_crc32x;
      SrcTy = llvm::Type::getInt64Ty(*Ctx);
    }

    llvm::Value *Src;
    if (SrcTy == SrcRaw->getType())
      Src = SrcRaw;
    else if (SrcTy->getPrimitiveSizeInBits() >
             SrcRaw->getType()->getPrimitiveSizeInBits())
      Src = Builder.CreateZExt(SrcRaw, SrcTy);
    else
      Src = Builder.CreateTrunc(SrcRaw, SrcTy);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
    auto *CR = Builder.CreateCall(Fn, {Acc, Src}, "crc32");
    auto *OutTy = sizeToType(Op.Output.Size);
    return Builder.CreateZExtOrBitCast(CR, OutTy);
  }

  // Scalar / 64-bit-lane saturating add & subtract (SQADD/SQSUB/UQADD/UQSUB).
  // The lifter handles small vector lanes with a manual clamp but routes the
  // scalar (b/h/s/d) and 64-bit-lane (.2d) forms here, where a manual clamp
  // would need unrepresentable i128 signed bounds.  Lower to the matching LLVM
  // saturating intrinsic so codegen emits the real sqadd/uqadd/sqsub/uqsub and
  // the recompiled code matches the original bit-for-bit under Unicorn.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::A64Sqadd:
      IID = llvm::Intrinsic::sadd_sat;
      break;
    case I::A64Uqadd:
      IID = llvm::Intrinsic::uadd_sat;
      break;
    case I::A64Sqsub:
      IID = llvm::Intrinsic::ssub_sat;
      break;
    case I::A64Uqsub:
      IID = llvm::Intrinsic::usub_sat;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= 3) {
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *Ty = llvm::IntegerType::get(*Ctx, Op.Output.Size * 8);
      auto coerce = [&](const MedVar &In) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, Ty);
        return Builder.CreateZExtOrTrunc(V, Ty);
      };
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {Ty});
      llvm::Value *R =
          Builder.CreateCall(Fn, {coerce(Op.Inputs[1]), coerce(Op.Inputs[2])});
      return (R->getType() == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
  }

  // NEON saturating / rounding variable shifts (SQSHL/UQSHL/SQSHLU/SQRSHL/
  // UQRSHL/SRSHL/URSHL).  All lower to a (data, per-lane shift vector) AArch64
  // intrinsic; the immediate forms pass a splat shift vector.  Inputs:
  // {data, shiftVec, elemSizeConst}.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::A64_Sqshl:
      IID = llvm::Intrinsic::aarch64_neon_sqshl;
      break;
    case I::A64_Uqshl:
      IID = llvm::Intrinsic::aarch64_neon_uqshl;
      break;
    case I::A64_Sqshlu:
      IID = llvm::Intrinsic::aarch64_neon_sqshlu;
      break;
    case I::A64_Sqrshl:
      IID = llvm::Intrinsic::aarch64_neon_sqrshl;
      break;
    case I::A64_Uqrshl:
      IID = llvm::Intrinsic::aarch64_neon_uqrshl;
      break;
    case I::A64_Srshl:
      IID = llvm::Intrinsic::aarch64_neon_srshl;
      break;
    case I::A64_Urshl:
      IID = llvm::Intrinsic::aarch64_neon_urshl;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= 4) {
      auto *OutTy = sizeToType(Op.Output.Size);
      const MedVar &Last = Op.Inputs[Op.NumInputs - 1];
      unsigned ElemSz =
          Last.isConst() ? static_cast<unsigned>(Last.ConstVal) : 4;
      unsigned Bytes = Op.Output.Size;
      unsigned NLanes = ElemSz ? Bytes / ElemSz : 0;
      if (NLanes < 1 || NLanes * ElemSz != Bytes)
        return llvm::ConstantInt::get(OutTy, 0);
      auto *ElemTy = llvm::IntegerType::get(*Ctx, ElemSz * 8);
      auto *VecTy = llvm::FixedVectorType::get(ElemTy, NLanes);
      auto *IntTy = llvm::IntegerType::get(*Ctx, Bytes * 8);
      auto toVec = [&](const MedVar &In) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, IntTy);
        if (V->getType() != IntTy)
          V = (V->getType()->getPrimitiveSizeInBits() == Bytes * 8)
                  ? Builder.CreateBitCast(V, IntTy)
                  : Builder.CreateZExtOrTrunc(V, IntTy);
        return Builder.CreateBitCast(V, VecTy);
      };
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {VecTy});
      llvm::Value *R =
          Builder.CreateCall(Fn, {toVec(Op.Inputs[1]), toVec(Op.Inputs[2])});
      R = Builder.CreateBitCast(R, IntTy);
      return (IntTy == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
  }

  // NEON narrowing saturating shift-right by immediate (SQSHRN/SQRSHRN/UQSHRN/
  // SQSHRUN/SQRSHRUN/UQRSHRN).  Lower to the AArch64 intrinsic taking the wide
  // vector and an i32 immediate, yielding the narrow vector.  Inputs:
  // {wideData(16 bytes), immConst, narrowElemSizeConst}.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::A64_Sqshrn:
      IID = llvm::Intrinsic::aarch64_neon_sqshrn;
      break;
    case I::A64_Sqrshrn:
      IID = llvm::Intrinsic::aarch64_neon_sqrshrn;
      break;
    case I::A64_Uqshrn:
      IID = llvm::Intrinsic::aarch64_neon_uqshrn;
      break;
    case I::A64_Sqshrun:
      IID = llvm::Intrinsic::aarch64_neon_sqshrun;
      break;
    case I::A64_Sqrshrun:
      IID = llvm::Intrinsic::aarch64_neon_sqrshrun;
      break;
    case I::A64_Uqrshrn:
      IID = llvm::Intrinsic::aarch64_neon_uqrshrn;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= 4) {
      auto *OutTy = sizeToType(Op.Output.Size);
      unsigned Imm = Op.Inputs[2].isConst()
                         ? static_cast<unsigned>(Op.Inputs[2].ConstVal)
                         : 0;
      unsigned NarrowSz = Op.Inputs[3].isConst()
                              ? static_cast<unsigned>(Op.Inputs[3].ConstVal)
                              : 2;
      unsigned WideSz = NarrowSz * 2;
      unsigned NLanes =
          WideSz ? 16u / WideSz : 0; // wide source is a Q register
      if (NLanes < 1)
        return llvm::ConstantInt::get(OutTy, 0);
      auto *WideVecTy = llvm::FixedVectorType::get(
          llvm::IntegerType::get(*Ctx, WideSz * 8), NLanes);
      auto *NarrowVecTy = llvm::FixedVectorType::get(
          llvm::IntegerType::get(*Ctx, NarrowSz * 8), NLanes);
      auto *WideIntTy = llvm::IntegerType::get(*Ctx, 128);
      llvm::Value *W = getVar(Op.Inputs[1], Builder);
      if (W->getType()->isPointerTy())
        W = Builder.CreatePtrToInt(W, WideIntTy);
      if (W->getType() != WideIntTy)
        W = (W->getType()->getPrimitiveSizeInBits() == 128)
                ? Builder.CreateBitCast(W, WideIntTy)
                : Builder.CreateZExtOrTrunc(W, WideIntTy);
      auto *WVec = Builder.CreateBitCast(W, WideVecTy);
      auto *Fn =
          llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {NarrowVecTy});
      llvm::Value *R = Builder.CreateCall(
          Fn,
          {WVec, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), Imm)});
      auto *NarrowIntTy = llvm::IntegerType::get(*Ctx, NLanes * NarrowSz * 8);
      R = Builder.CreateBitCast(R, NarrowIntTy);
      return (NarrowIntTy == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
  }

  // PMULL/PMULL2 — polynomial (carry-less) multiply long.  p8 multiplies eight
  // i8 pairs to <8 x i16> (@llvm.aarch64.neon.pmull); p64 multiplies two i64 to
  // a 128-bit result (@llvm.aarch64.neon.pmull64).  The element width is the
  // trailing constant input (1 = p8, 8 = p64); the 64-bit lane selection ("2"
  // form) was already applied in the lifter.
  if (IC == I::A64_Pmull && Op.Output.Size == 16 && Op.NumInputs >= 4) {
    auto *OutTy = sizeToType(Op.Output.Size);
    const MedVar &Last = Op.Inputs[Op.NumInputs - 1];
    unsigned ElemSz = Last.isConst() ? static_cast<unsigned>(Last.ConstVal) : 1;
    auto *I128Ty = llvm::IntegerType::get(*Ctx, 128);
    if (ElemSz == 8) {
      auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
      auto *A = Builder.CreateZExtOrTrunc(getVar(Op.Inputs[1], Builder), I64Ty);
      auto *B = Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), I64Ty);
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::aarch64_neon_pmull64);
      llvm::Value *R = Builder.CreateCall(Fn, {A, B}, "pmull64");
      R = Builder.CreateBitCast(R, I128Ty);
      return (I128Ty == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
    auto *V8I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 8);
    auto *V8I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 8);
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    auto toV8I8 = [&](const MedVar &In) -> llvm::Value * {
      llvm::Value *V = getVar(In, Builder);
      if (V->getType()->isPointerTy())
        V = Builder.CreatePtrToInt(V, I64Ty);
      V = Builder.CreateZExtOrTrunc(V, I64Ty);
      return Builder.CreateBitCast(V, V8I8);
    };
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_neon_pmull, {V8I16});
    llvm::Value *R = Builder.CreateCall(
        Fn, {toV8I8(Op.Inputs[1]), toV8I8(Op.Inputs[2])}, "pmull");
    R = Builder.CreateBitCast(R, I128Ty);
    return (I128Ty == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
  }

  // PMUL — polynomial (carry-less) multiply, same element width, per-byte
  // (i8 lanes, `.8b`/`.16b`).  GF(2)[x] multiply, NOT ordinary integer
  // multiply; map to @llvm.aarch64.neon.pmul (the SVE2 group had lifted it as
  // INT_MULT).
  if (IC == I::A64_Pmul && Op.Output.Size > 0 && Op.NumInputs >= 3) {
    auto *OutTy = sizeToType(Op.Output.Size);
    unsigned NLanes = Op.Output.Size; // 8 (.8b) or 16 (.16b)
    auto *VecTy =
        llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), NLanes);
    auto *IntTy = llvm::IntegerType::get(*Ctx, NLanes * 8);
    auto toVec = [&](const MedVar &In) -> llvm::Value * {
      llvm::Value *V = getVar(In, Builder);
      if (V->getType()->isPointerTy())
        V = Builder.CreatePtrToInt(V, IntTy);
      if (V->getType() != IntTy)
        V = (V->getType()->getPrimitiveSizeInBits() == NLanes * 8)
                ? Builder.CreateBitCast(V, IntTy)
                : Builder.CreateZExtOrTrunc(V, IntTy);
      return Builder.CreateBitCast(V, VecTy);
    };
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_neon_pmul, {VecTy});
    llvm::Value *R = Builder.CreateCall(
        Fn, {toVec(Op.Inputs[1]), toVec(Op.Inputs[2])}, "pmul");
    R = Builder.CreateBitCast(R, IntTy);
    return (IntTy == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
  }

  // NEON reciprocal / reciprocal-sqrt estimate & step.  These are
  // architecturally-defined approximations that cannot be expressed as plain FP
  // ops; lower to the matching AArch64 NEON intrinsic so codegen emits the real
  // frecpe/frecps/frsqrte/frsqrts (and urecpe/ursqrte) instruction and the
  // recompiled code runs bit-identically under Unicorn (keep the binary
  // instruction; do NOT approximate with a true divide).  The float element
  // width (f32/f64) is passed by the lifter as a trailing constant; the
  // unsigned integer estimates always operate on u32 lanes.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    bool Binary = false;
    bool IsFloat = true;
    switch (IC) {
    case I::A64_Frecpe:
      IID = llvm::Intrinsic::aarch64_neon_frecpe;
      break;
    case I::A64_Frsqrte:
      IID = llvm::Intrinsic::aarch64_neon_frsqrte;
      break;
    case I::A64_Frecps:
      IID = llvm::Intrinsic::aarch64_neon_frecps;
      Binary = true;
      break;
    case I::A64_Frsqrts:
      IID = llvm::Intrinsic::aarch64_neon_frsqrts;
      Binary = true;
      break;
    case I::A64_Fmulx:
      IID = llvm::Intrinsic::aarch64_neon_fmulx;
      Binary = true;
      break;
    case I::A64_Urecpe:
      IID = llvm::Intrinsic::aarch64_neon_urecpe;
      IsFloat = false;
      break;
    case I::A64_Ursqrte:
      IID = llvm::Intrinsic::aarch64_neon_ursqrte;
      IsFloat = false;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0) {
      auto *OutTy = sizeToType(Op.Output.Size);
      unsigned Bytes = Op.Output.Size;
      unsigned ElemSz = 4;
      if (IsFloat && Op.NumInputs >= 1) {
        const MedVar &Last = Op.Inputs[Op.NumInputs - 1];
        if (Last.isConst() &&
            (Last.ConstVal == 2 || Last.ConstVal == 4 || Last.ConstVal == 8))
          ElemSz = static_cast<unsigned>(Last.ConstVal);
      }
      unsigned NLanes = ElemSz ? Bytes / ElemSz : 0;
      unsigned Needed = Binary ? 3u : 2u;
      if (NLanes < 1 || NLanes * ElemSz != Bytes || Op.NumInputs < Needed)
        return llvm::ConstantInt::get(OutTy, 0);
      llvm::Type *ElemTy =
          IsFloat ? (ElemSz == 8   ? (llvm::Type *)llvm::Type::getDoubleTy(*Ctx)
                     : ElemSz == 2 ? (llvm::Type *)llvm::Type::getHalfTy(*Ctx)
                                   : (llvm::Type *)llvm::Type::getFloatTy(*Ctx))
                  : (llvm::Type *)llvm::Type::getInt32Ty(*Ctx);
      llvm::Type *ValTy =
          (NLanes == 1)
              ? ElemTy
              : (llvm::Type *)llvm::FixedVectorType::get(ElemTy, NLanes);
      auto *IntTy = llvm::IntegerType::get(*Ctx, Bytes * 8);
      auto toVal = [&](const MedVar &In) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, IntTy);
        if (V->getType() != IntTy) {
          if (V->getType()->getPrimitiveSizeInBits() == Bytes * 8)
            V = Builder.CreateBitCast(V, IntTy);
          else
            V = Builder.CreateZExtOrTrunc(V, IntTy);
        }
        return Builder.CreateBitCast(V, ValTy);
      };
      llvm::SmallVector<llvm::Value *, 2> Args;
      Args.push_back(toVal(Op.Inputs[1]));
      if (Binary)
        Args.push_back(toVal(Op.Inputs[2]));
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {ValTy});
      llvm::Value *R = Builder.CreateCall(Fn, Args);
      auto *RInt = Builder.CreateBitCast(R, IntTy);
      if (IntTy == OutTy)
        return RInt;
      return Builder.CreateZExtOrTrunc(RInt, OutTy);
    }
  }

  // FJCVTZS (FEAT_JSCVT): JavaScript double->int32.  Map to the real
  // llvm.aarch64.fjcvtzs (returns i32) so codegen emits `fjcvtzs` and the
  // recompiled code wraps modulo-2^32 (and gives 0 for NaN/Inf) exactly like
  // hardware -- plain FPToSI would saturate instead.
  if (IC == I::A64_Fjcvtzs && Op.Output.Size > 0 && Op.NumInputs >= 2) {
    auto *OutTy = sizeToType(Op.Output.Size);
    auto *DblTy = llvm::Type::getDoubleTy(*Ctx);
    auto *I64 = llvm::Type::getInt64Ty(*Ctx);
    llvm::Value *In = getVar(Op.Inputs[1], Builder);
    if (In->getType()->isPointerTy())
      In = Builder.CreatePtrToInt(In, I64);
    if (In->getType() != DblTy) {
      if (In->getType()->getPrimitiveSizeInBits() != 64)
        In = Builder.CreateZExtOrTrunc(In, I64);
      In = Builder.CreateBitCast(In, DblTy);
    }
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_fjcvtzs);
    llvm::Value *R = Builder.CreateCall(Fn, {In}, "fjcvtzs"); // i32
    return Builder.CreateZExtOrTrunc(R, OutTy);
  }

  // FCVTXN: FP inexact narrowing f64->f32 with round-to-odd.  Scalar form (Sd,
  // 4-byte output) uses llvm.aarch64.sisd.fcvtxn (float<-double); the .2s
  // vector form (8-byte output) uses llvm.aarch64.neon.fcvtxn (<2 x float><-<2
  // x double>).  Round-to-odd differs from FLOAT_FLOAT2FLOAT (round-even) on
  // every inexact narrowing, so emit the real instruction.
  if (IC == I::A64_Fcvtxn && Op.Output.Size > 0 && Op.NumInputs >= 2) {
    auto *OutTy = sizeToType(Op.Output.Size);
    auto *I64 = llvm::Type::getInt64Ty(*Ctx);
    if (Op.Output.Size == 4) {
      auto *DblTy = llvm::Type::getDoubleTy(*Ctx);
      llvm::Value *In = getVar(Op.Inputs[1], Builder);
      if (In->getType()->isPointerTy())
        In = Builder.CreatePtrToInt(In, I64);
      if (In->getType() != DblTy) {
        if (In->getType()->getPrimitiveSizeInBits() != 64)
          In = Builder.CreateZExtOrTrunc(In, I64);
        In = Builder.CreateBitCast(In, DblTy);
      }
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::aarch64_sisd_fcvtxn);
      llvm::Value *R = Builder.CreateCall(Fn, {In}, "fcvtxn"); // float
      auto *RInt = Builder.CreateBitCast(R, llvm::Type::getInt32Ty(*Ctx));
      return Builder.CreateZExtOrTrunc(RInt, OutTy);
    }
    // Vector .2s <- .2d.
    auto *I128 = llvm::IntegerType::get(*Ctx, 128);
    auto *V2F64 = llvm::FixedVectorType::get(llvm::Type::getDoubleTy(*Ctx), 2);
    auto *V2F32 = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*Ctx), 2);
    llvm::Value *In = getVar(Op.Inputs[1], Builder);
    if (In->getType()->isPointerTy())
      In = Builder.CreatePtrToInt(In, I128);
    if (In->getType() != I128) {
      if (In->getType()->getPrimitiveSizeInBits() == 128)
        In = Builder.CreateBitCast(In, I128);
      else
        In = Builder.CreateZExtOrTrunc(In, I128);
    }
    auto *InV = Builder.CreateBitCast(In, V2F64);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::aarch64_neon_fcvtxn, {V2F32, V2F64});
    llvm::Value *R = Builder.CreateCall(Fn, {InV}, "fcvtxn"); // <2 x float>
    auto *RInt = Builder.CreateBitCast(R, I64);
    return Builder.CreateZExtOrTrunc(RInt, OutTy);
  }

  // Crypto — AES / SHA1 / SHA256.  The placeholder lift used to emit a generic
  // x86 intrinsic with no AArch64 handler, folding the whole function to 0.
  // Lower each to the matching LLVM AArch64 crypto intrinsic so codegen emits
  // the real aese/aesmc/sha1c/sha256h... and the recompiled code runs bit-for-
  // bit under Unicorn.  Inputs[1..] are the sources in instruction order
  // (destructive ops pass the old Vd first); the crypto intrinsics are not
  // overloaded so they take fixed <16 x i8>/<4 x i32>/i32 operands.
  {
    auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
    auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);
    auto *V2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);
    auto *I32 = llvm::Type::getInt32Ty(*Ctx);
    auto *I64 = llvm::Type::getInt64Ty(*Ctx);
    auto *I128 = llvm::IntegerType::get(*Ctx, 128);

    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    llvm::SmallVector<llvm::Type *, 3> ArgTys;
    // SM3TT* carry a trailing i64 ImmArg (the Vm lane index) after the vectors.
    bool ImmTail = false;
    switch (IC) {
    case I::A64_Aese:
      IID = llvm::Intrinsic::aarch64_crypto_aese;
      ArgTys = {V16I8, V16I8};
      break;
    case I::A64_Aesd:
      IID = llvm::Intrinsic::aarch64_crypto_aesd;
      ArgTys = {V16I8, V16I8};
      break;
    case I::A64_Aesmc:
      IID = llvm::Intrinsic::aarch64_crypto_aesmc;
      ArgTys = {V16I8};
      break;
    case I::A64_Aesimc:
      IID = llvm::Intrinsic::aarch64_crypto_aesimc;
      ArgTys = {V16I8};
      break;
    case I::A64_Sha1c:
      IID = llvm::Intrinsic::aarch64_crypto_sha1c;
      ArgTys = {V4I32, I32, V4I32};
      break;
    case I::A64_Sha1p:
      IID = llvm::Intrinsic::aarch64_crypto_sha1p;
      ArgTys = {V4I32, I32, V4I32};
      break;
    case I::A64_Sha1m:
      IID = llvm::Intrinsic::aarch64_crypto_sha1m;
      ArgTys = {V4I32, I32, V4I32};
      break;
    case I::A64_Sha1h:
      IID = llvm::Intrinsic::aarch64_crypto_sha1h;
      ArgTys = {I32};
      break;
    case I::A64_Sha1su0:
      IID = llvm::Intrinsic::aarch64_crypto_sha1su0;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::A64_Sha1su1:
      IID = llvm::Intrinsic::aarch64_crypto_sha1su1;
      ArgTys = {V4I32, V4I32};
      break;
    case I::A64_Sha256h:
      IID = llvm::Intrinsic::aarch64_crypto_sha256h;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::A64_Sha256h2:
      IID = llvm::Intrinsic::aarch64_crypto_sha256h2;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::A64_Sha256su0:
      IID = llvm::Intrinsic::aarch64_crypto_sha256su0;
      ArgTys = {V4I32, V4I32};
      break;
    case I::A64_Sha256su1:
      IID = llvm::Intrinsic::aarch64_crypto_sha256su1;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    // SHA512 / SM3 / SM4 (ARMv8.2).  SHA512 operates on <2 x i64>; SM3/SM4 on
    // <4 x i32>.  SM3TT* take a trailing i64 lane-index ImmArg.
    case I::A64_Sha512h:
      IID = llvm::Intrinsic::aarch64_crypto_sha512h;
      ArgTys = {V2I64, V2I64, V2I64};
      break;
    case I::A64_Sha512h2:
      IID = llvm::Intrinsic::aarch64_crypto_sha512h2;
      ArgTys = {V2I64, V2I64, V2I64};
      break;
    case I::A64_Sha512su0:
      IID = llvm::Intrinsic::aarch64_crypto_sha512su0;
      ArgTys = {V2I64, V2I64};
      break;
    case I::A64_Sha512su1:
      IID = llvm::Intrinsic::aarch64_crypto_sha512su1;
      ArgTys = {V2I64, V2I64, V2I64};
      break;
    case I::A64_Sm3partw1:
      IID = llvm::Intrinsic::aarch64_crypto_sm3partw1;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::A64_Sm3partw2:
      IID = llvm::Intrinsic::aarch64_crypto_sm3partw2;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::A64_Sm3ss1:
      IID = llvm::Intrinsic::aarch64_crypto_sm3ss1;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::A64_Sm3tt1a:
      IID = llvm::Intrinsic::aarch64_crypto_sm3tt1a;
      ArgTys = {V4I32, V4I32, V4I32};
      ImmTail = true;
      break;
    case I::A64_Sm3tt1b:
      IID = llvm::Intrinsic::aarch64_crypto_sm3tt1b;
      ArgTys = {V4I32, V4I32, V4I32};
      ImmTail = true;
      break;
    case I::A64_Sm3tt2a:
      IID = llvm::Intrinsic::aarch64_crypto_sm3tt2a;
      ArgTys = {V4I32, V4I32, V4I32};
      ImmTail = true;
      break;
    case I::A64_Sm3tt2b:
      IID = llvm::Intrinsic::aarch64_crypto_sm3tt2b;
      ArgTys = {V4I32, V4I32, V4I32};
      ImmTail = true;
      break;
    case I::A64_Sm4e:
      IID = llvm::Intrinsic::aarch64_crypto_sm4e;
      ArgTys = {V4I32, V4I32};
      break;
    case I::A64_Sm4ekey:
      IID = llvm::Intrinsic::aarch64_crypto_sm4ekey;
      ArgTys = {V4I32, V4I32};
      break;
    default:
      break;
    }
    unsigned NeedIns = ArgTys.size() + 1 + (ImmTail ? 1u : 0u);
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= NeedIns) {
      auto *OutTy = sizeToType(Op.Output.Size);
      auto coerce = [&](const MedVar &In, llvm::Type *Ty) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, I128);
        if (Ty->isIntegerTy())
          return Builder.CreateZExtOrTrunc(V, Ty);
        unsigned VBits = Ty->getPrimitiveSizeInBits();
        auto *IntTy = llvm::IntegerType::get(*Ctx, VBits);
        if (V->getType() != IntTy)
          V = (V->getType()->getPrimitiveSizeInBits() == VBits)
                  ? Builder.CreateBitCast(V, IntTy)
                  : Builder.CreateZExtOrTrunc(V, IntTy);
        return Builder.CreateBitCast(V, Ty);
      };
      llvm::SmallVector<llvm::Value *, 4> Args;
      for (size_t I = 0; I < ArgTys.size(); ++I)
        Args.push_back(coerce(Op.Inputs[I + 1], ArgTys[I]));
      if (ImmTail) {
        const MedVar &ImmIn = Op.Inputs[ArgTys.size() + 1];
        uint64_t Idx =
            ImmIn.isConst() ? static_cast<uint64_t>(ImmIn.ConstVal) : 0;
        Args.push_back(llvm::ConstantInt::get(I64, Idx));
      }
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
      llvm::Value *R = Builder.CreateCall(Fn, Args);
      if (R->getType()->isVectorTy())
        R = Builder.CreateBitCast(
            R, llvm::IntegerType::get(*Ctx,
                                      R->getType()->getPrimitiveSizeInBits()));
      return (R->getType() == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
  }

  return nullptr;
}

} // namespace neverd
