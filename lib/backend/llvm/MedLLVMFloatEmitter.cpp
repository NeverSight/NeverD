//===- MedLLVMFloatEmitter.cpp - Floating-point op emission ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Floating-point MedOp emission: arithmetic, comparisons, conversions,
/// and unary math intrinsics (abs, sqrt, ceil, floor, round, nan).
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"

#include <cmath>

namespace neverd {

namespace {

// Scalar FP type for a value byte width: 2=half, 4=float, 8=double, 10=x87
// 80-bit extended precision (`long double`).  Other widths fall back to double.
llvm::Type *scalarFPTypeForSize(llvm::LLVMContext &Ctx, unsigned Size) {
  switch (Size) {
  case 2:
    return llvm::Type::getHalfTy(Ctx);
  case 4:
    return llvm::Type::getFloatTy(Ctx);
  case 10:
    return llvm::Type::getX86_FP80Ty(Ctx);
  default:
    return llvm::Type::getDoubleTy(Ctx);
  }
}

llvm::Type *inferFloatTy(llvm::LLVMContext &Ctx, llvm::Value *V) {
  if (V->getType()->isFloatingPointTy())
    return V->getType();
  unsigned Bits = V->getType()->getIntegerBitWidth();
  // A 16-bit operand is an IEEE half (FEAT_FP16); reinterpreting its bits as a
  // float32 (the old `Bits <= 32` path) turned e.g. 1.0h (0x3C00) into a tiny
  // denormal, breaking all half-precision arithmetic.
  if (Bits == 16)
    return llvm::Type::getHalfTy(Ctx);
  // An 80-bit operand is an x87 extended-precision value (long double).
  if (Bits == 80)
    return llvm::Type::getX86_FP80Ty(Ctx);
  return (Bits <= 32) ? llvm::Type::getFloatTy(Ctx)
                      : llvm::Type::getDoubleTy(Ctx);
}

llvm::Value *bitcastToFloat(llvm::IRBuilder<> &Builder, llvm::LLVMContext &Ctx,
                            llvm::Value *V) {
  if (V->getType()->isFloatingPointTy())
    return V;
  auto *FTy = inferFloatTy(Ctx, V);
  unsigned FPBits = FTy->getPrimitiveSizeInBits();
  unsigned IntBits = V->getType()->getIntegerBitWidth();
  if (IntBits > FPBits)
    V = Builder.CreateTrunc(V, llvm::Type::getIntNTy(Ctx, FPBits));
  else if (IntBits < FPBits)
    V = Builder.CreateZExt(V, llvm::Type::getIntNTy(Ctx, FPBits));
  return Builder.CreateBitCast(V, FTy);
}

// Round an x87 80-bit value to an integral value with the hardware `frndint`
// (rounds per the FPU control word, default nearest-even).  All of
// llvm.roundeven/rint/nearbyint.f80 lower to a libm libcall (roundevenl/rintl)
// the recompiled object cannot resolve under Unicorn, so emit the genuine x87
// instruction the original ran instead of lifting to a library function.
llvm::Value *emitX87Frndint(llvm::IRBuilder<> &Builder, llvm::LLVMContext &Ctx,
                            llvm::Value *FV) {
  auto *F80Ty = llvm::Type::getX86_FP80Ty(Ctx);
  auto *FnTy = llvm::FunctionType::get(F80Ty, {F80Ty}, false);
  auto *IA = llvm::InlineAsm::get(FnTy, "frndint",
                                  "=&{st},0,~{dirflag},~{fpsr},~{flags}",
                                  /*hasSideEffects=*/true);
  return Builder.CreateCall(IA, {FV}, "x87rndint");
}

llvm::Value *emitUnaryFPIntrinsic(llvm::IRBuilder<> &Builder, llvm::Module *Mod,
                                  llvm::LLVMContext &Ctx,
                                  llvm::Intrinsic::ID IID, llvm::Value *Raw,
                                  const char *Name) {
  auto *FV = bitcastToFloat(Builder, Ctx, Raw);
  auto *FTy = FV->getType();
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {FTy});
  return Builder.CreateCall(Fn, {FV}, Name);
}

llvm::Value *emitBinaryFPIntrinsic(llvm::IRBuilder<> &Builder,
                                   llvm::Module *Mod, llvm::LLVMContext &Ctx,
                                   llvm::Intrinsic::ID IID, llvm::Value *RawL,
                                   llvm::Value *RawR, const char *Name) {
  auto *FL = bitcastToFloat(Builder, Ctx, RawL);
  auto *FR = bitcastToFloat(Builder, Ctx, RawR);
  if (FL->getType() != FR->getType()) {
    if (FL->getType()->getPrimitiveSizeInBits() <
        FR->getType()->getPrimitiveSizeInBits())
      FL = Builder.CreateFPExt(FL, FR->getType(), "fpext_l");
    else
      FR = Builder.CreateFPExt(FR, FL->getType(), "fpext_r");
  }
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID, {FL->getType()});
  return Builder.CreateCall(Fn, {FL, FR}, Name);
}

// FP->int conversion with architecture-correct out-of-range / NaN behavior.
// Raw fptosi/fptoui is UB (poison) when the value does not fit or is NaN; a
// compile-time-constant over-range operand then folds to `poison`, which the
// optimizer miscompiles.  Hardware is well-defined: AArch64 fcvtz{s,u} / ARM
// vcvt saturate (NaN->0); x86 cvtts{s,d}2si returns the "integer indefinite"
// INT_MIN for any out-of-range / NaN.  Both are modeled poison-free via
// llvm.fptosi.sat / llvm.fptoui.sat (which lower to the native fcvtz on ARM).
llvm::Value *emitFPToInt(llvm::IRBuilder<> &Builder, llvm::Module *Mod,
                         llvm::Value *FV, llvm::Type *DstTy, bool IsUnsigned,
                         Arch TargetArch) {
  llvm::Intrinsic::ID SatID =
      IsUnsigned ? llvm::Intrinsic::fptoui_sat : llvm::Intrinsic::fptosi_sat;
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, SatID,
                                                     {DstTy, FV->getType()});
  llvm::Value *Sat = Builder.CreateCall(Fn, {FV}, "fptoint_sat");
  // ARM/AArch64 saturate exactly; x86 has no defined unsigned FP->int convert.
  if ((TargetArch != Arch::X86 && TargetArch != Arch::X64) || IsUnsigned)
    return Sat;
  // x86 signed: positive overflow and NaN -> INT_MIN (negative overflow already
  // saturates to INT_MIN, matching).  `fcmp uge x, 2^(B-1)` is true for both
  // positive overflow and NaN (unordered); the bound is +Inf if it overflows
  // the source float type, so only +Inf/NaN trigger it there.
  unsigned Bits = DstTy->getIntegerBitWidth();
  llvm::Value *IntMin =
      llvm::ConstantInt::get(DstTy, llvm::APInt::getSignedMinValue(Bits));
  llvm::Value *Bound =
      llvm::ConstantFP::get(FV->getType(), std::ldexp(1.0, (int)Bits - 1));
  llvm::Value *Overflow = Builder.CreateFCmpUGE(FV, Bound, "x86_cvt_ovf");
  return Builder.CreateSelect(Overflow, IntMin, Sat, "x86_cvt");
}

} // anonymous namespace

llvm::Value *MedLLVMEmitter::emitFloatOp(const MedOp &Op,
                                         llvm::IRBuilder<> &Builder) {
  auto GetInput = [&](uint8_t Idx) -> llvm::Value * {
    if (Idx >= Op.NumInputs)
      return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), 0);
    return getVar(Op.Inputs[Idx], Builder);
  };

  switch (Op.Opcode) {
  case NdOp::FLOAT_TRUNC: {
    auto *Operand = GetInput(0);
    auto *DstTy = sizeToType(Op.Output.Size);
    if (Operand->getType()->isIntegerTy() && DstTy->isIntegerTy()) {
      auto *FV = bitcastToFloat(Builder, *Ctx, Operand);
      return emitFPToInt(Builder, Mod, FV, DstTy, /*IsUnsigned=*/false,
                         TargetArch);
    }
    if (Operand->getType()->isFloatingPointTy()) {
      if (DstTy->isIntegerTy())
        return emitFPToInt(Builder, Mod, Operand, DstTy, /*IsUnsigned=*/false,
                           TargetArch);
      auto *FloatDst = scalarFPTypeForSize(*Ctx, Op.Output.Size);
      if (Operand->getType() == FloatDst)
        return Operand;
      if (Operand->getType()->getPrimitiveSizeInBits() >
          FloatDst->getPrimitiveSizeInBits())
        return Builder.CreateFPTrunc(Operand, FloatDst, "fptrunc");
      return Builder.CreateFPExt(Operand, FloatDst, "fpext");
    }
    return Operand;
  }

  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV: {
    auto *LRaw = GetInput(0);
    auto *RRaw = GetInput(1);
    auto *FL = bitcastToFloat(Builder, *Ctx, LRaw);
    auto *FR = bitcastToFloat(Builder, *Ctx, RRaw);
    if (FL->getType() != FR->getType()) {
      unsigned LBits = FL->getType()->getPrimitiveSizeInBits();
      unsigned RBits = FR->getType()->getPrimitiveSizeInBits();
      if (LBits < RBits)
        FL = Builder.CreateFPExt(FL, FR->getType(), "fpext_l");
      else
        FR = Builder.CreateFPExt(FR, FL->getType(), "fpext_r");
    }
    switch (Op.Opcode) {
    case NdOp::FLOAT_ADD:
      return Builder.CreateFAdd(FL, FR, "fadd");
    case NdOp::FLOAT_SUB:
      return Builder.CreateFSub(FL, FR, "fsub");
    case NdOp::FLOAT_MULT:
      return Builder.CreateFMul(FL, FR, "fmul");
    default:
      return Builder.CreateFDiv(FL, FR, "fdiv");
    }
  }

  case NdOp::FLOAT_FMA: {
    // Fused multiply-add a*b+c with a SINGLE rounding (@llvm.fma).  Used for
    // genuinely fused instructions (AArch64 FMLA/FMADD, ARM VFMA); lifting them
    // as separate FMUL+FADD would round twice and diverge in the low bits.
    auto *A = bitcastToFloat(Builder, *Ctx, GetInput(0));
    auto *B = bitcastToFloat(Builder, *Ctx, GetInput(1));
    auto *C = bitcastToFloat(Builder, *Ctx, GetInput(2));
    auto Unify = [&](llvm::Value *&X, llvm::Type *T) {
      if (X->getType() != T)
        X = Builder.CreateFPExt(X, T, "fpext_fma");
    };
    llvm::Type *Widest = A->getType();
    auto Wider = [&](llvm::Type *T) {
      if (T->getPrimitiveSizeInBits() > Widest->getPrimitiveSizeInBits())
        Widest = T;
    };
    Wider(B->getType());
    Wider(C->getType());
    Unify(A, Widest);
    Unify(B, Widest);
    Unify(C, Widest);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::fma, {Widest});
    return Builder.CreateCall(Fn, {A, B, C}, "fma");
  }

  case NdOp::FLOAT_NEG:
    return Builder.CreateFNeg(bitcastToFloat(Builder, *Ctx, GetInput(0)),
                              "fneg");

  case NdOp::FLOAT_ABS:
    return emitUnaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::fabs,
                                GetInput(0), "fabs");

  case NdOp::FLOAT_SQRT:
    return emitUnaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::sqrt,
                                GetInput(0), "fsqrt");

  case NdOp::FLOAT_CEIL:
    return emitUnaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::ceil,
                                GetInput(0), "fceil");

  case NdOp::FLOAT_FLOOR:
    return emitUnaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::floor,
                                GetInput(0), "ffloor");

  case NdOp::FLOAT_ROUND:
    return emitUnaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::round,
                                GetInput(0), "fround");

  case NdOp::FLOAT_ROUNDEVEN: {
    // x87 FRNDINT / FIST round-to-integer on an 80-bit value: emit the hardware
    // frndint instead of the libcall-only llvm.roundeven.f80.
    auto *FV = bitcastToFloat(Builder, *Ctx, GetInput(0));
    if (FV->getType()->isX86_FP80Ty())
      return emitX87Frndint(Builder, *Ctx, FV);
    return emitUnaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::roundeven,
                                GetInput(0), "froundeven");
  }

  // ARMv8 FMIN/FMAX (NEON VMIN/VMAX.f): IEEE-754 minimum/maximum — propagate
  // NaN and order signed zeros (-0 < +0).  A naive (a<b)?a:b select gets both
  // wrong, so map to the dedicated intrinsics.
  case NdOp::FLOAT_MIN:
    return emitBinaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::minimum,
                                 GetInput(0), GetInput(1), "fmin");

  case NdOp::FLOAT_MAX:
    return emitBinaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::maximum,
                                 GetInput(0), GetInput(1), "fmax");

  // ARMv8 FMINNM/FMAXNM (VMINNM/VMAXNM): IEEE-754 minNum/maxNum — return the
  // numeric operand when exactly one input is NaN (NaN-suppressing).
  case NdOp::FLOAT_MINNUM:
    return emitBinaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::minnum,
                                 GetInput(0), GetInput(1), "fminnm");

  case NdOp::FLOAT_MAXNUM:
    return emitBinaryFPIntrinsic(Builder, Mod, *Ctx, llvm::Intrinsic::maxnum,
                                 GetInput(0), GetInput(1), "fmaxnm");

  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL: {
    auto *LRaw = GetInput(0);
    auto *RRaw = GetInput(1);
    auto *FL = bitcastToFloat(Builder, *Ctx, LRaw);
    auto *FR = bitcastToFloat(Builder, *Ctx, RRaw);
    if (FL->getType() != FR->getType()) {
      unsigned LBits = FL->getType()->getPrimitiveSizeInBits();
      unsigned RBits = FR->getType()->getPrimitiveSizeInBits();
      if (LBits < RBits)
        FL = Builder.CreateFPExt(FL, FR->getType(), "fpext_l");
      else
        FR = Builder.CreateFPExt(FR, FL->getType(), "fpext_r");
    }
    llvm::Value *Cmp = nullptr;
    switch (Op.Opcode) {
    case NdOp::FLOAT_EQUAL:
      Cmp = Builder.CreateFCmpOEQ(FL, FR, "feq");
      break;
    case NdOp::FLOAT_NOTEQUAL:
      Cmp = Builder.CreateFCmpUNE(FL, FR, "fne");
      break;
    case NdOp::FLOAT_LESS:
      Cmp = Builder.CreateFCmpOLT(FL, FR, "flt");
      break;
    default:
      Cmp = Builder.CreateFCmpOLE(FL, FR, "fle");
      break;
    }
    return Builder.CreateZExt(Cmp, llvm::Type::getInt8Ty(*Ctx));
  }

  case NdOp::FLOAT_ISNAN: {
    auto *FV = bitcastToFloat(Builder, *Ctx, GetInput(0));
    auto *IsNan = Builder.CreateFCmpUNO(FV, FV, "isnan");
    return Builder.CreateZExt(IsNan, llvm::Type::getInt8Ty(*Ctx));
  }

  case NdOp::FLOAT_FLOAT2INT: {
    auto *Raw = GetInput(0);
    auto *DstTy = sizeToType(Op.Output.Size);
    auto *FV = bitcastToFloat(Builder, *Ctx, Raw);
    return emitFPToInt(Builder, Mod, FV, DstTy, /*IsUnsigned=*/false,
                       TargetArch);
  }

  case NdOp::FLOAT_FLOAT2UINT: {
    auto *Raw = GetInput(0);
    auto *DstTy = sizeToType(Op.Output.Size);
    auto *FV = bitcastToFloat(Builder, *Ctx, Raw);
    return emitFPToInt(Builder, Mod, FV, DstTy, /*IsUnsigned=*/true,
                       TargetArch);
  }

  case NdOp::FLOAT_INT2FLOAT: {
    auto *DstFTy = scalarFPTypeForSize(*Ctx, Op.Output.Size);
    return Builder.CreateSIToFP(GetInput(0), DstFTy, "sitofp");
  }

  case NdOp::FLOAT_UINT2FLOAT: {
    auto *DstFTy = scalarFPTypeForSize(*Ctx, Op.Output.Size);
    return Builder.CreateUIToFP(GetInput(0), DstFTy, "uitofp");
  }

  case NdOp::FLOAT_FLOAT2FLOAT: {
    auto *FV = bitcastToFloat(Builder, *Ctx, GetInput(0));
    auto *SrcFTy = FV->getType();
    // The destination precision is the output width (h/s/d/x87-f80), not a
    // fixed float<->double toggle — fcvt converts between any pair, including
    // half (FEAT_FP16) and 80-bit extended (x87 FLD/FST widen/narrow).
    llvm::Type *DstFTy = scalarFPTypeForSize(*Ctx, Op.Output.Size);
    if (SrcFTy == DstFTy)
      return FV;
    if (SrcFTy->getPrimitiveSizeInBits() > DstFTy->getPrimitiveSizeInBits())
      return Builder.CreateFPTrunc(FV, DstFTy, "fptrunc");
    return Builder.CreateFPExt(FV, DstFTy, "fpext");
  }

  default:
    return nullptr;
  }
}

} // namespace neverd
