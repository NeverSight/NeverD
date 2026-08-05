//===- MedLLVMARMValueEmitter.cpp - ARM value-producing intrinsics -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM (32-bit) value-producing intrinsic emission (RBIT).
///
/// Side-effect-only intrinsics live in MedLLVMARMSideeffect.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-arm-value"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IntrinsicsARM.h"

namespace neverd {

llvm::Value *MedLLVMEmitter::emitARMIntrinsicValue(const MedOp &Op,
                                                   Intrinsic IC,
                                                   llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  if (IC == I::ArmRbit && Op.Output.Size > 0) {
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

  // CRC32 (ARMv8-A AArch32, optionally Castagnoli `c`): Rd = crc(Inputs[1]
  // accum, Inputs[2] data).  Every variant's ARM intrinsic is (i32, i32) -> i32
  // and uses the low 8/16/32 bits of the data per the b/h/w suffix; truncate
  // the operands to i32 and let the intrinsic select the active bits.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::ArmCrc32b:
      IID = llvm::Intrinsic::arm_crc32b;
      break;
    case I::ArmCrc32h:
      IID = llvm::Intrinsic::arm_crc32h;
      break;
    case I::ArmCrc32w:
      IID = llvm::Intrinsic::arm_crc32w;
      break;
    case I::ArmCrc32cb:
      IID = llvm::Intrinsic::arm_crc32cb;
      break;
    case I::ArmCrc32ch:
      IID = llvm::Intrinsic::arm_crc32ch;
      break;
    case I::ArmCrc32cw:
      IID = llvm::Intrinsic::arm_crc32cw;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= 3) {
      auto *OutTy = sizeToType(Op.Output.Size);
      auto *I32 = llvm::Type::getInt32Ty(*Ctx);
      auto coerce = [&](const MedVar &In) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, I32);
        return Builder.CreateZExtOrTrunc(V, I32);
      };
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
      llvm::Value *R = Builder.CreateCall(
          Fn, {coerce(Op.Inputs[1]), coerce(Op.Inputs[2])}, "crc32");
      return Builder.CreateZExtOrBitCast(R, OutTy);
    }
  }

  // SSAT / USAT — saturate a signed 32-bit value to the signed/unsigned range
  // of `SatBits` bits.  Inputs[1] = value, Inputs[2] = saturation bit count
  // (constant).  Used by the parallel saturating/halving add/sub lifter; the
  // SSAT/USAT instructions themselves clamp inline in the lifter.
  if ((IC == I::ArmSsat || IC == I::ArmUsat) && Op.Output.Size > 0 &&
      Op.NumInputs >= 3 && Op.Inputs[2].isConst()) {
    bool Signed = (IC == I::ArmSsat);
    unsigned SatBits = static_cast<unsigned>(Op.Inputs[2].ConstVal);
    auto *I32 = llvm::Type::getInt32Ty(*Ctx);
    llvm::Value *V = getVar(Op.Inputs[1], Builder);
    if (V->getType()->isPointerTy())
      V = Builder.CreatePtrToInt(V, I32);
    V = Builder.CreateSExtOrTrunc(V, I32);
    int64_t Max =
        Signed ? ((SatBits >= 32) ? 0x7FFFFFFFLL : ((1LL << (SatBits - 1)) - 1))
               : ((SatBits >= 32) ? 0xFFFFFFFFLL : ((1LL << SatBits) - 1));
    int64_t Min =
        Signed ? ((SatBits >= 32) ? -(1LL << 31) : -(1LL << (SatBits - 1))) : 0;
    auto *MaxC = llvm::ConstantInt::getSigned(I32, Max);
    auto *MinC = llvm::ConstantInt::getSigned(I32, Min);
    V = Builder.CreateSelect(Builder.CreateICmpSGT(V, MaxC), MaxC, V);
    V = Builder.CreateSelect(Builder.CreateICmpSLT(V, MinC), MinC, V);
    auto *OutTy = sizeToType(Op.Output.Size);
    return Builder.CreateSExtOrTrunc(V, OutTy);
  }

  // NEON saturating doubling multiply (VQDMULH/VQRDMULH same-width, VQDMULL
  // widening).  Inputs: {a, b, elemSizeConst}.  Map to the ARM NEON intrinsic.
  if ((IC == I::ArmVqdmulh || IC == I::ArmVqrdmulh || IC == I::ArmVqdmull) &&
      Op.Output.Size > 0 && Op.NumInputs >= 4) {
    auto *OutTy = sizeToType(Op.Output.Size);
    const MedVar &Last = Op.Inputs[Op.NumInputs - 1];
    unsigned ElemSz = Last.isConst() ? static_cast<unsigned>(Last.ConstVal) : 4;
    bool Widen = (IC == I::ArmVqdmull);
    unsigned OutBytes = Op.Output.Size;
    unsigned DstElem = ElemSz; // result element width
    unsigned SrcElem = Widen ? ElemSz / 2 : ElemSz;
    unsigned NLanes = DstElem ? OutBytes / DstElem : 0;
    if (NLanes < 1 || SrcElem == 0)
      return llvm::ConstantInt::get(OutTy, 0);
    auto *DstVecTy = llvm::FixedVectorType::get(
        llvm::IntegerType::get(*Ctx, DstElem * 8), NLanes);
    auto *SrcVecTy = llvm::FixedVectorType::get(
        llvm::IntegerType::get(*Ctx, SrcElem * 8), NLanes);
    unsigned SrcBytes = NLanes * SrcElem;
    auto *SrcIntTy = llvm::IntegerType::get(*Ctx, SrcBytes * 8);
    auto toSrc = [&](const MedVar &In) -> llvm::Value * {
      llvm::Value *V = getVar(In, Builder);
      if (V->getType()->isPointerTy())
        V = Builder.CreatePtrToInt(V, SrcIntTy);
      if (V->getType() != SrcIntTy)
        V = (V->getType()->getPrimitiveSizeInBits() == SrcBytes * 8)
                ? Builder.CreateBitCast(V, SrcIntTy)
                : Builder.CreateZExtOrTrunc(V, SrcIntTy);
      return Builder.CreateBitCast(V, SrcVecTy);
    };
    llvm::Intrinsic::ID IID =
        (IC == I::ArmVqdmulh)    ? llvm::Intrinsic::arm_neon_vqdmulh
        : (IC == I::ArmVqrdmulh) ? llvm::Intrinsic::arm_neon_vqrdmulh
                                 : llvm::Intrinsic::arm_neon_vqdmull;
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {DstVecTy});
    llvm::Value *R =
        Builder.CreateCall(Fn, {toSrc(Op.Inputs[1]), toSrc(Op.Inputs[2])});
    auto *IntTy = llvm::IntegerType::get(*Ctx, OutBytes * 8);
    R = Builder.CreateBitCast(R, IntTy);
    return (IntTy == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
  }

  // VMUL.p8 — polynomial (carry-less) multiply, same width, per-byte (i8 lanes,
  // d/q forms).  GF(2)[x] multiply, NOT integer multiply; map to the ARM NEON
  // intrinsic.  The shared VADD/VSUB/VMUL handler had lifted it as INT_MULT.
  if (IC == I::ArmVmulp && Op.Output.Size > 0 && Op.NumInputs >= 3) {
    auto *OutTy = sizeToType(Op.Output.Size);
    unsigned NLanes = Op.Output.Size; // 8 (d) or 16 (q)
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
        Mod, llvm::Intrinsic::arm_neon_vmulp, {VecTy});
    llvm::Value *R = Builder.CreateCall(
        Fn, {toVec(Op.Inputs[1]), toVec(Op.Inputs[2])}, "vmulp");
    R = Builder.CreateBitCast(R, IntTy);
    return (IntTy == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
  }

  // VMULL.p8 — polynomial (carry-less) widening multiply: 8 byte pairs -> a
  // <8 x i16> (q-reg).  Maps to @llvm.arm.neon.vmullp; the integer VMULL
  // handler had lifted it as per-lane INT_MULT (ordinary multiply, wrong for
  // GF(2)).
  if (IC == I::ArmVmullp && Op.Output.Size == 16 && Op.NumInputs >= 3) {
    auto *OutTy = sizeToType(Op.Output.Size);
    auto *V8I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 8);
    auto *V8I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 8);
    auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
    auto *I128Ty = llvm::IntegerType::get(*Ctx, 128);
    auto toV8I8 = [&](const MedVar &In) -> llvm::Value * {
      llvm::Value *V = getVar(In, Builder);
      if (V->getType()->isPointerTy())
        V = Builder.CreatePtrToInt(V, I64Ty);
      V = Builder.CreateZExtOrTrunc(V, I64Ty);
      return Builder.CreateBitCast(V, V8I8);
    };
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::arm_neon_vmullp, {V8I16});
    llvm::Value *R = Builder.CreateCall(
        Fn, {toV8I8(Op.Inputs[1]), toV8I8(Op.Inputs[2])}, "vmullp");
    R = Builder.CreateBitCast(R, I128Ty);
    return (I128Ty == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
  }

  // NEON saturating / rounding variable shift (VQSHL/VQSHLU/VQRSHL).  Inputs:
  // {data, shiftVec, elemSizeConst}; the per-lane shift amount is signed
  // (negative = right).  Map to the ARM NEON intrinsic.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::ArmVqshifts:
      IID = llvm::Intrinsic::arm_neon_vqshifts;
      break;
    case I::ArmVqshiftu:
      IID = llvm::Intrinsic::arm_neon_vqshiftu;
      break;
    case I::ArmVqshiftsu:
      IID = llvm::Intrinsic::arm_neon_vqshiftsu;
      break;
    case I::ArmVqrshifts:
      IID = llvm::Intrinsic::arm_neon_vqrshifts;
      break;
    case I::ArmVqrshiftu:
      IID = llvm::Intrinsic::arm_neon_vqrshiftu;
      break;
    case I::ArmVrshifts:
      IID = llvm::Intrinsic::arm_neon_vrshifts;
      break;
    case I::ArmVrshiftu:
      IID = llvm::Intrinsic::arm_neon_vrshiftu;
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
      if (NLanes < 1)
        return llvm::ConstantInt::get(OutTy, 0);
      auto *VecTy = llvm::FixedVectorType::get(
          llvm::IntegerType::get(*Ctx, ElemSz * 8), NLanes);
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

  // NEON narrowing saturating shift-right (VQSHRN/VQRSHRN + unsigned-result
  // VQSHRUN/VQRSHRUN).  Inputs: {wideData, immConst, narrowElemSizeConst}.  The
  // ARM intrinsic takes a wide per-lane shift vector with negative = right.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    switch (IC) {
    case I::ArmVqshiftns:
      IID = llvm::Intrinsic::arm_neon_vqshiftns;
      break;
    case I::ArmVqshiftnu:
      IID = llvm::Intrinsic::arm_neon_vqshiftnu;
      break;
    case I::ArmVqshiftnsu:
      IID = llvm::Intrinsic::arm_neon_vqshiftnsu;
      break;
    case I::ArmVqrshiftns:
      IID = llvm::Intrinsic::arm_neon_vqrshiftns;
      break;
    case I::ArmVqrshiftnu:
      IID = llvm::Intrinsic::arm_neon_vqrshiftnu;
      break;
    case I::ArmVqrshiftnsu:
      IID = llvm::Intrinsic::arm_neon_vqrshiftnsu;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= 4) {
      auto *OutTy = sizeToType(Op.Output.Size);
      int Imm =
          Op.Inputs[2].isConst() ? static_cast<int>(Op.Inputs[2].ConstVal) : 0;
      unsigned NarrowSz = Op.Inputs[3].isConst()
                              ? static_cast<unsigned>(Op.Inputs[3].ConstVal)
                              : 2;
      unsigned WideSz = NarrowSz * 2;
      unsigned NLanes =
          WideSz ? 16u / WideSz : 0; // wide source is a Q register
      if (NLanes < 1)
        return llvm::ConstantInt::get(OutTy, 0);
      auto *WideElemTy = llvm::IntegerType::get(*Ctx, WideSz * 8);
      auto *WideVecTy = llvm::FixedVectorType::get(WideElemTy, NLanes);
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
      // The intrinsic shifts right by -shift; broadcast the negated immediate.
      auto *ShiftSplat = llvm::ConstantVector::getSplat(
          llvm::ElementCount::getFixed(NLanes),
          llvm::ConstantInt::getSigned(WideElemTy, -Imm));
      auto *Fn =
          llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {NarrowVecTy});
      llvm::Value *R = Builder.CreateCall(Fn, {WVec, ShiftSplat});
      auto *NarrowIntTy = llvm::IntegerType::get(*Ctx, NLanes * NarrowSz * 8);
      R = Builder.CreateBitCast(R, NarrowIntTy);
      return (NarrowIntTy == OutTy) ? R : Builder.CreateZExtOrTrunc(R, OutTy);
    }
  }

  // NEON reciprocal / reciprocal-sqrt estimate & step (float).  These have an
  // architecturally-defined approximation that cannot be expressed as plain FP
  // ops; lower them to the matching LLVM ARM NEON intrinsic so codegen emits
  // the real `vrecpe/vrecps/vrsqrte/vrsqrts.f32` instruction and the recompiled
  // code runs bit-identically to the original under Unicorn (keep the binary
  // instruction; do NOT approximate with a true divide).  vrecpe/vrsqrte are
  // unary; vrecps/vrsqrts are the binary Newton-Raphson step.
  {
    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    bool Binary = false;
    switch (IC) {
    case I::ArmVrecpe:
      IID = llvm::Intrinsic::arm_neon_vrecpe;
      Binary = false;
      break;
    case I::ArmVrecps:
      IID = llvm::Intrinsic::arm_neon_vrecps;
      Binary = true;
      break;
    case I::ArmVrsqrte:
      IID = llvm::Intrinsic::arm_neon_vrsqrte;
      Binary = false;
      break;
    case I::ArmVrsqrts:
      IID = llvm::Intrinsic::arm_neon_vrsqrts;
      Binary = true;
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0) {
      auto *OutTy = sizeToType(Op.Output.Size);
      unsigned Bytes = Op.Output.Size;
      unsigned NLanes = Bytes / 4; // f32 lanes (vrecpe.f32 / vrecps.f32 ...)
      unsigned Needed = Binary ? 3u : 2u;
      if (NLanes < 1 || NLanes * 4 != Bytes || Op.NumInputs < Needed)
        return llvm::ConstantInt::get(OutTy, 0);
      auto *FloatTy = llvm::Type::getFloatTy(*Ctx);
      auto *VecTy = llvm::FixedVectorType::get(FloatTy, NLanes);
      auto *IntTy = llvm::IntegerType::get(*Ctx, Bytes * 8);
      auto toVec = [&](const MedVar &In) -> llvm::Value * {
        llvm::Value *V = getVar(In, Builder);
        if (V->getType()->isPointerTy())
          V = Builder.CreatePtrToInt(V, IntTy);
        else if (V->getType() != IntTy) {
          if (V->getType()->getPrimitiveSizeInBits() == Bytes * 8)
            V = Builder.CreateBitCast(V, IntTy);
          else
            V = Builder.CreateZExtOrTrunc(V, IntTy);
        }
        return Builder.CreateBitCast(V, VecTy);
      };
      llvm::SmallVector<llvm::Value *, 2> Args;
      Args.push_back(toVec(Op.Inputs[1]));
      if (Binary)
        Args.push_back(toVec(Op.Inputs[2]));
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {VecTy});
      llvm::Value *R = Builder.CreateCall(Fn, Args);
      return Builder.CreateBitCast(R, OutTy);
    }
  }

  // Crypto — AES / SHA1 / SHA256 (ARMv8 AArch32).  The placeholder lift used to
  // emit a generic x86 intrinsic with no ARM handler, folding the function to
  // 0. Lower each to the matching LLVM ARM NEON crypto intrinsic so codegen
  // emits the real aese.8/aesmc.8/sha1c.32/sha256h.32... and the recompiled
  // code runs bit-for-bit under Unicorn (max CPU).  Inputs[1..] are the sources
  // in instruction order (destructive ops pass the old Vd first); the crypto
  // intrinsics are not overloaded so they take fixed types.
  {
    auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
    auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);
    auto *I32 = llvm::Type::getInt32Ty(*Ctx);
    auto *I128 = llvm::IntegerType::get(*Ctx, 128);

    llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
    llvm::SmallVector<llvm::Type *, 3> ArgTys;
    switch (IC) {
    case I::ArmAese:
      IID = llvm::Intrinsic::arm_neon_aese;
      ArgTys = {V16I8, V16I8};
      break;
    case I::ArmAesd:
      IID = llvm::Intrinsic::arm_neon_aesd;
      ArgTys = {V16I8, V16I8};
      break;
    case I::ArmAesmc:
      IID = llvm::Intrinsic::arm_neon_aesmc;
      ArgTys = {V16I8};
      break;
    case I::ArmAesimc:
      IID = llvm::Intrinsic::arm_neon_aesimc;
      ArgTys = {V16I8};
      break;
    case I::ArmSha1c:
      IID = llvm::Intrinsic::arm_neon_sha1c;
      ArgTys = {V4I32, I32, V4I32};
      break;
    case I::ArmSha1p:
      IID = llvm::Intrinsic::arm_neon_sha1p;
      ArgTys = {V4I32, I32, V4I32};
      break;
    case I::ArmSha1m:
      IID = llvm::Intrinsic::arm_neon_sha1m;
      ArgTys = {V4I32, I32, V4I32};
      break;
    case I::ArmSha1h:
      IID = llvm::Intrinsic::arm_neon_sha1h;
      ArgTys = {I32};
      break;
    case I::ArmSha1su0:
      IID = llvm::Intrinsic::arm_neon_sha1su0;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::ArmSha1su1:
      IID = llvm::Intrinsic::arm_neon_sha1su1;
      ArgTys = {V4I32, V4I32};
      break;
    case I::ArmSha256h:
      IID = llvm::Intrinsic::arm_neon_sha256h;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::ArmSha256h2:
      IID = llvm::Intrinsic::arm_neon_sha256h2;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    case I::ArmSha256su0:
      IID = llvm::Intrinsic::arm_neon_sha256su0;
      ArgTys = {V4I32, V4I32};
      break;
    case I::ArmSha256su1:
      IID = llvm::Intrinsic::arm_neon_sha256su1;
      ArgTys = {V4I32, V4I32, V4I32};
      break;
    default:
      break;
    }
    if (IID != llvm::Intrinsic::not_intrinsic && Op.Output.Size > 0 &&
        Op.NumInputs >= ArgTys.size() + 1) {
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
      llvm::SmallVector<llvm::Value *, 3> Args;
      for (size_t Idx = 0; Idx < ArgTys.size(); ++Idx)
        Args.push_back(coerce(Op.Inputs[Idx + 1], ArgTys[Idx]));
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
