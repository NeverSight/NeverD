//===- MedLLVMX86SimdEmitter.cpp - x86 SIMD misc intrinsics ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 SIMD intrinsic emission: vector type conversion helpers (toVec,
/// fromVec, widenToI128), packed shifts (PSLL/PSRL/PSRA), and miscellaneous
/// SIMD operations (PMOVMSKB, DPPS, MPSADBW, PDEP/PEXT, VPTERNLOG,
/// masked load/store, PCMPxSTRx).
///
/// Crypto intrinsics (AES/SHA/PCLMUL) live in MedLLVMX86CryptoEmitter.cpp.
/// Shuffle/blend/broadcast intrinsics live in MedLLVMX86ShuffleEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-simd"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Vector type conversion helpers
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::toVec(llvm::Value *V, llvm::Type *VTy,
                                   llvm::IRBuilder<> &Builder) {
  if (V->getType() == VTy)
    return V;
  unsigned VBits = VTy->getPrimitiveSizeInBits();
  if (V->getType()->isIntegerTy() &&
      V->getType()->getIntegerBitWidth() == VBits)
    return Builder.CreateBitCast(V, VTy);
  if (V->getType()->isIntegerTy() && V->getType()->getIntegerBitWidth() < VBits)
    V = Builder.CreateZExt(V, llvm::IntegerType::get(*Ctx, VBits));
  return Builder.CreateBitCast(V, VTy);
}

llvm::Value *MedLLVMEmitter::fromVec(llvm::Value *V,
                                     llvm::IRBuilder<> &Builder) {
  unsigned Bits = V->getType()->getPrimitiveSizeInBits();
  auto *IntTy = llvm::IntegerType::get(*Ctx, Bits);
  if (V->getType() == IntTy)
    return V;
  return Builder.CreateBitCast(V, IntTy);
}

llvm::Value *MedLLVMEmitter::widenToI128(llvm::Value *V,
                                         llvm::IRBuilder<> &Builder) {
  if (V->getType()->isIntegerTy(128))
    return V;
  if (V->getType()->isIntegerTy() && V->getType()->getIntegerBitWidth() < 128)
    return Builder.CreateZExt(V, llvm::IntegerType::get(*Ctx, 128));
  return V;
}

//===----------------------------------------------------------------------===//
// Packed shift intrinsics (PSLL/PSRL/PSRA)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPackedShift(const MedOp &Op, Intrinsic IC,
                                             llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  if (Op.Output.Size != 16 || Op.NumInputs < 3)
    return nullptr;

  // The data operand may be propagated to a narrow GPR by a zero-extending
  // gpr->xmm move; widen it back to i128 (high bits zero) before the gate.  The
  // shift count keeps its own narrow path below (scalar/immediate counts).
  auto *Data = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  auto *Amt = getVar(Op.Inputs[2], Builder);
  if (!Data->getType()->isIntegerTy(128))
    return nullptr;

  auto *V8I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 8);
  auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);
  auto *V2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);

  llvm::Type *VTy = nullptr;
  bool IsLeft = false, IsArith = false;
  llvm::Intrinsic::ID IID = llvm::Intrinsic::not_intrinsic;
  switch (IC) {
  case I::PsllW:
    VTy = V8I16;
    IsLeft = true;
    IID = llvm::Intrinsic::x86_sse2_psll_w;
    break;
  case I::PsllD:
    VTy = V4I32;
    IsLeft = true;
    IID = llvm::Intrinsic::x86_sse2_psll_d;
    break;
  case I::PsllQ:
    VTy = V2I64;
    IsLeft = true;
    IID = llvm::Intrinsic::x86_sse2_psll_q;
    break;
  case I::PsrlW:
    VTy = V8I16;
    IID = llvm::Intrinsic::x86_sse2_psrl_w;
    break;
  case I::PsrlD:
    VTy = V4I32;
    IID = llvm::Intrinsic::x86_sse2_psrl_d;
    break;
  case I::PsrlQ:
    VTy = V2I64;
    IID = llvm::Intrinsic::x86_sse2_psrl_q;
    break;
  case I::PsraW:
    VTy = V8I16;
    IsArith = true;
    IID = llvm::Intrinsic::x86_sse2_psra_w;
    break;
  case I::PsraD:
    VTy = V4I32;
    IsArith = true;
    IID = llvm::Intrinsic::x86_sse2_psra_d;
    break;
  default:
    return nullptr;
  }

  if (Amt->getType()->isIntegerTy(128)) {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
    return fromVec(Builder.CreateCall(
                       Fn,
                       {toVec(Data, VTy, Builder), toVec(Amt, VTy, Builder)},
                       "pshift"),
                   Builder);
  }

  auto *FVTy = llvm::cast<llvm::FixedVectorType>(VTy);
  unsigned ElemBits = FVTy->getScalarSizeInBits();
  unsigned NumElems = FVTy->getNumElements();
  auto *ETy = llvm::IntegerType::get(*Ctx, ElemBits);
  auto *DataVec = toVec(Data, VTy, Builder);

  llvm::Value *ShiftVal = Amt;
  if (ShiftVal->getType() != ETy)
    ShiftVal = Builder.CreateZExtOrTrunc(ShiftVal, ETy);

  auto *MaxShift = llvm::ConstantInt::get(ETy, ElemBits);
  auto *TooBig = Builder.CreateICmpUGE(ShiftVal, MaxShift);
  auto *Clamped = Builder.CreateSelect(
      TooBig, llvm::ConstantInt::get(ETy, IsArith ? (ElemBits - 1) : 0),
      ShiftVal);
  auto *SplatVec = Builder.CreateVectorSplat(NumElems, Clamped);

  llvm::Value *Shifted;
  if (IsLeft)
    Shifted = Builder.CreateShl(DataVec, SplatVec, "psll_imm");
  else if (IsArith)
    Shifted = Builder.CreateAShr(DataVec, SplatVec, "psra_imm");
  else
    Shifted = Builder.CreateLShr(DataVec, SplatVec, "psrl_imm");

  if (!IsArith) {
    auto *ZeroVec = llvm::Constant::getNullValue(VTy);
    auto *TooBigSplat = Builder.CreateVectorSplat(NumElems, TooBig);
    Shifted = Builder.CreateSelect(TooBigSplat, ZeroVec, Shifted);
  }

  return fromVec(Shifted, Builder);
}

//===----------------------------------------------------------------------===//
// Bit manipulation SIMD (PDEP/PEXT, VPTERNLOG)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitBitManipSimd(const MedOp &Op, Intrinsic IC,
                                              llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  auto *OutTy = sizeToType(Op.Output.Size);

  //--- PDEP / PEXT (BMI2) ---
  if ((IC == I::Pdep || IC == I::Pext) && Op.NumInputs >= 3 &&
      Op.Output.Size > 0) {
    auto *S = getVar(Op.Inputs[1], Builder);
    auto *M = getVar(Op.Inputs[2], Builder);
    bool Is64 = (Op.Output.Size == 8);
    auto *Ty =
        Is64 ? llvm::Type::getInt64Ty(*Ctx) : llvm::Type::getInt32Ty(*Ctx);
    if (S->getType() != Ty)
      S = Builder.CreateZExtOrTrunc(S, Ty);
    if (M->getType() != Ty)
      M = Builder.CreateZExtOrTrunc(M, Ty);
    auto IID = (IC == I::Pdep) ? (Is64 ? llvm::Intrinsic::x86_bmi_pdep_64
                                       : llvm::Intrinsic::x86_bmi_pdep_32)
                               : (Is64 ? llvm::Intrinsic::x86_bmi_pext_64
                                       : llvm::Intrinsic::x86_bmi_pext_32);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
    auto *R = Builder.CreateCall(Fn, {S, M}, IC == I::Pdep ? "pdep" : "pext");
    return (R->getType() != OutTy) ? Builder.CreateZExtOrTrunc(R, OutTy) : R;
  }

  //--- VPTERNLOG ---
  if (IC == I::Vpternlog && Op.NumInputs >= 5 &&
      (Op.Output.Size == 16 || Op.Output.Size == 32 || Op.Output.Size == 64)) {
    auto *A = getVar(Op.Inputs[1], Builder);
    auto *B = getVar(Op.Inputs[2], Builder);
    auto *C = getVar(Op.Inputs[3], Builder);
    uint8_t Imm =
        Op.Inputs[Op.NumInputs - 1].isConst()
            ? static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal)
            : 0;
    if (A->getType() == OutTy && B->getType() == OutTy &&
        C->getType() == OutTy && OutTy->isIntegerTy()) {
      llvm::Value *R = llvm::ConstantInt::get(OutTy, 0);
      for (int Bit = 0; Bit < 8; ++Bit) {
        if (!(Imm & (1 << Bit)))
          continue;
        auto *AT = (Bit & 4) ? A : Builder.CreateNot(A);
        auto *BT = (Bit & 2) ? B : Builder.CreateNot(B);
        auto *CT = (Bit & 1) ? C : Builder.CreateNot(C);
        R = Builder.CreateOr(R,
                             Builder.CreateAnd(AT, Builder.CreateAnd(BT, CT)));
      }
      return R;
    }
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// Masked memory operations (VPMASKMOV load/store)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitMaskedMemOp(const MedOp &Op, Intrinsic IC,
                                             llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  //--- Masked load ---
  if (Op.NumInputs >= 3 && (IC == I::MaskedLoadD || IC == I::MaskedLoadQ)) {
    auto *Addr = Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
                     ? getVar(Op.Inputs[1], Builder)
                     : getRawSegmentOffset(Op.Inputs[1], Builder);
    auto *MaskVal = getVar(Op.Inputs[2], Builder);
    bool IsQ = (IC == I::MaskedLoadQ);
    unsigned Elem = IsQ ? 64 : 32;
    unsigned N = (Op.Output.Size * 8) / Elem;
    auto *ETy = llvm::IntegerType::get(*Ctx, Elem);
    auto *VTy = llvm::FixedVectorType::get(ETy, N);
    auto *MV = toVec(MaskVal, VTy, Builder);
    auto *ZV = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(N),
                                              llvm::ConstantInt::get(ETy, 0));
    auto *Cmp = Builder.CreateICmpSLT(MV, ZV, "msb");
    unsigned AddressSpace = 0;
    if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
      AddressSpace = 257;
    else if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86GS)
      AddressSpace = 256;
    auto *Ptr = Builder.CreateIntToPtr(
        Addr, llvm::PointerType::get(*Ctx, AddressSpace));
    return fromVec(Builder.CreateMaskedLoad(VTy, Ptr, llvm::Align(1), Cmp,
                                            llvm::Constant::getNullValue(VTy),
                                            "mload"),
                   Builder);
  }

  //--- Masked store ---
  if (Op.NumInputs >= 4 && (IC == I::MaskedStoreD || IC == I::MaskedStoreQ ||
                            IC == I::MaskedStoreB)) {
    auto *Addr = Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
                     ? getVar(Op.Inputs[1], Builder)
                     : getRawSegmentOffset(Op.Inputs[1], Builder);
    auto *MaskVal = getVar(Op.Inputs[2], Builder);
    auto *StoreData = getVar(Op.Inputs[3], Builder);
    bool IsQ = (IC == I::MaskedStoreQ);
    unsigned Elem = IC == I::MaskedStoreB ? 8 : (IsQ ? 64 : 32);
    unsigned Bits = StoreData->getType()->isIntegerTy()
                        ? StoreData->getType()->getIntegerBitWidth()
                        : 128;
    unsigned N = Bits / Elem;
    auto *ETy = llvm::IntegerType::get(*Ctx, Elem);
    auto *VTy = llvm::FixedVectorType::get(ETy, N);
    auto *ZV = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(N),
                                              llvm::ConstantInt::get(ETy, 0));
    auto *Cmp = Builder.CreateICmpSLT(toVec(MaskVal, VTy, Builder), ZV, "msb");
    unsigned AddressSpace = 0;
    if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
      AddressSpace = 257;
    else if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86GS)
      AddressSpace = 256;
    auto *Ptr = Builder.CreateIntToPtr(
        Addr, llvm::PointerType::get(*Ctx, AddressSpace));
    Builder.CreateMaskedStore(toVec(StoreData, VTy, Builder), Ptr,
                              llvm::Align(1), Cmp);
    return nullptr;
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// PCMPxSTRx (SSE4.2 string comparison)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPcmpStr(const MedOp &Op, Intrinsic IC,
                                         llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
  auto *OutTy = sizeToType(Op.Output.Size);

  //--- PCMPISTRx (implicit-length) ---
  if (Op.Output.Size > 0 && Op.NumInputs >= 4 &&
      Op.Inputs[Op.NumInputs - 1].isConst() &&
      (IC == I::Pcmpistri || IC == I::Pcmpistrm)) {
    uint8_t Imm = static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal);
    // An xmm operand fed by a zero-extending gpr->xmm move (movq/movd) is
    // propagated to the narrow GPR, so widen it back to i128 (high bits zero).
    auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
    auto *B = widenToI128(getVar(Op.Inputs[2], Builder), Builder);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128)) {
      llvm::Intrinsic::ID IID = (IC == I::Pcmpistri)
                                    ? llvm::Intrinsic::x86_sse42_pcmpistri128
                                    : llvm::Intrinsic::x86_sse42_pcmpistrm128;
      auto *IV = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), Imm);
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
      auto *R = Builder.CreateCall(
          Fn, {toVec(A, V16I8, Builder), toVec(B, V16I8, Builder), IV},
          IC == I::Pcmpistri ? "pcmpistri" : "pcmpistrm");
      if (R->getType() == OutTy)
        return R;
      if (R->getType()->isIntegerTy() && OutTy->isIntegerTy())
        return Builder.CreateZExtOrTrunc(R, OutTy);
      return fromVec(R, Builder);
    }
  }

  //--- PCMPESTRx (explicit-length) ---
  if (Op.Output.Size > 0 && Op.NumInputs >= 6 &&
      Op.Inputs[Op.NumInputs - 1].isConst() &&
      (IC == I::Pcmpestri || IC == I::Pcmpestrm)) {
    auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
    auto *LA = getVar(Op.Inputs[2], Builder);
    auto *B = widenToI128(getVar(Op.Inputs[3], Builder), Builder);
    auto *LB = getVar(Op.Inputs[4], Builder);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128)) {
      auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
      if (LA->getType() != I32Ty)
        LA = Builder.CreateTrunc(LA, I32Ty);
      if (LB->getType() != I32Ty)
        LB = Builder.CreateTrunc(LB, I32Ty);
      uint8_t Imm = static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal);
      auto IID = (IC == I::Pcmpestri) ? llvm::Intrinsic::x86_sse42_pcmpestri128
                                      : llvm::Intrinsic::x86_sse42_pcmpestrm128;
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
      auto *R = Builder.CreateCall(
          Fn,
          {toVec(A, V16I8, Builder), LA, toVec(B, V16I8, Builder), LB,
           llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), Imm)},
          IC == I::Pcmpestri ? "pcmpestri" : "pcmpestrm");
      if (R->getType() == OutTy)
        return R;
      if (R->getType()->isIntegerTy() && OutTy->isIntegerTy())
        return Builder.CreateZExtOrTrunc(R, OutTy);
      return fromVec(R, Builder);
    }
  }

  //--- PCMPxSTRx status flags ---
  // The control imm and the flag selector are packed into one operand
  // (bits[7:0]=imm8, bits[9:8]=selector 0=CF/1=ZF/2=SF/3=OF) so the explicit
  // form fits the 6-input INTRINSIC limit.  Layout: implicit {code, A, B,
  // immsel}; explicit {code, A, LA, B, LB, immsel}.  Each maps to the dedicated
  // LLVM flag intrinsic so the backend lowers it to pcmp*str* + setcc, matching
  // hardware bit-for-bit.
  if (Op.Output.Size > 0 && (IC == I::PcmpistrFlag || IC == I::PcmpestrFlag)) {
    bool IsExplicit = (IC == I::PcmpestrFlag);
    unsigned Need = IsExplicit ? 6u : 4u;
    if (Op.NumInputs >= Need && Op.Inputs[Op.NumInputs - 1].isConst()) {
      uint64_t Packed = Op.Inputs[Op.NumInputs - 1].ConstVal;
      uint8_t Imm = static_cast<uint8_t>(Packed & 0xFF);
      uint8_t Sel = static_cast<uint8_t>((Packed >> 8) & 3);
      auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
      llvm::Value *B = nullptr, *LA = nullptr, *LB = nullptr;
      if (IsExplicit) {
        LA = getVar(Op.Inputs[2], Builder);
        B = widenToI128(getVar(Op.Inputs[3], Builder), Builder);
        LB = getVar(Op.Inputs[4], Builder);
      } else {
        B = widenToI128(getVar(Op.Inputs[2], Builder), Builder);
      }
      if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128)) {
        static const llvm::Intrinsic::ID Implicit[4] = {
            llvm::Intrinsic::x86_sse42_pcmpistric128,
            llvm::Intrinsic::x86_sse42_pcmpistriz128,
            llvm::Intrinsic::x86_sse42_pcmpistris128,
            llvm::Intrinsic::x86_sse42_pcmpistrio128};
        static const llvm::Intrinsic::ID Explicit[4] = {
            llvm::Intrinsic::x86_sse42_pcmpestric128,
            llvm::Intrinsic::x86_sse42_pcmpestriz128,
            llvm::Intrinsic::x86_sse42_pcmpestris128,
            llvm::Intrinsic::x86_sse42_pcmpestrio128};
        auto *IV = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), Imm);
        auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
            Mod, IsExplicit ? Explicit[Sel] : Implicit[Sel]);
        llvm::Value *R;
        if (IsExplicit) {
          auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
          if (LA->getType() != I32Ty)
            LA = Builder.CreateTrunc(LA, I32Ty);
          if (LB->getType() != I32Ty)
            LB = Builder.CreateTrunc(LB, I32Ty);
          R = Builder.CreateCall(
              Fn,
              {toVec(A, V16I8, Builder), LA, toVec(B, V16I8, Builder), LB, IV},
              "pcmpflag");
        } else {
          R = Builder.CreateCall(
              Fn, {toVec(A, V16I8, Builder), toVec(B, V16I8, Builder), IV},
              "pcmpflag");
        }
        if (R->getType() == OutTy)
          return R;
        if (R->getType()->isIntegerTy() && OutTy->isIntegerTy())
          return Builder.CreateZExtOrTrunc(R, OutTy);
      }
    }
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// PMOVMSKB (SSE2)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPmovmskb(const MedOp &Op,
                                          llvm::IRBuilder<> &Builder) {
  if (Op.NumInputs < 2)
    return nullptr;
  auto *Src = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  auto *OutTy = sizeToType(Op.Output.Size);

  // 256-bit VPMOVMSKB: 32 byte sign bits (16 per 128-bit lane) collected into a
  // GPR (bit i = sign of byte i).  @llvm.x86.avx2.pmovmskb has exactly this
  // semantics; without it the ymm form fell through unhandled (result = 0).
  llvm::Value *R = nullptr;
  if (Src->getType()->isIntegerTy(256)) {
    auto *V32I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 32);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_avx2_pmovmskb);
    R = Builder.CreateCall(Fn, {toVec(Src, V32I8, Builder)}, "pmovmskb256");
  } else if (Src->getType()->isIntegerTy(128)) {
    auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_sse2_pmovmskb_128);
    R = Builder.CreateCall(Fn, {toVec(Src, V16I8, Builder)}, "pmovmskb");
  } else {
    return nullptr;
  }
  if (R->getType() != OutTy) {
    if (R->getType()->getIntegerBitWidth() > OutTy->getIntegerBitWidth())
      R = Builder.CreateTrunc(R, OutTy);
    else
      R = Builder.CreateZExt(R, OutTy);
  }
  return R;
}

//===----------------------------------------------------------------------===//
// PHMINPOSUW (SSE4.1)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPhminposuw(const MedOp &Op,
                                            llvm::IRBuilder<> &Builder) {
  if (Op.Output.Size != 16 || Op.NumInputs < 2)
    return nullptr;
  auto *Src = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  if (!Src->getType()->isIntegerTy(128))
    return nullptr;

  auto *V8I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 8);
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::x86_sse41_phminposuw);
  return fromVec(
      Builder.CreateCall(Fn, {toVec(Src, V8I16, Builder)}, "phminposuw"),
      Builder);
}

//===----------------------------------------------------------------------===//
// DPPS (SSE4.1)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitDpps(const MedOp &Op,
                                      llvm::IRBuilder<> &Builder) {
  if (Op.Output.Size != 16 || Op.NumInputs < 4 ||
      !Op.Inputs[Op.NumInputs - 1].isConst())
    return nullptr;
  auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  auto *B = widenToI128(getVar(Op.Inputs[2], Builder), Builder);
  if (!A->getType()->isIntegerTy(128) || !B->getType()->isIntegerTy(128))
    return nullptr;

  auto *V4F = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*Ctx), 4);
  auto *Imm = llvm::ConstantInt::get(
      llvm::Type::getInt8Ty(*Ctx),
      static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal));
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::x86_sse41_dpps);
  auto *R = Builder.CreateCall(
      Fn, {Builder.CreateBitCast(A, V4F), Builder.CreateBitCast(B, V4F), Imm},
      "dpps");
  return Builder.CreateBitCast(R, llvm::IntegerType::get(*Ctx, 128));
}

//===----------------------------------------------------------------------===//
// MPSADBW (SSE4.1)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitMpsadbw(const MedOp &Op,
                                         llvm::IRBuilder<> &Builder) {
  if ((Op.Output.Size != 16 && Op.Output.Size != 32) || Op.NumInputs < 4 ||
      !Op.Inputs[Op.NumInputs - 1].isConst())
    return nullptr;
  auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  auto *B = widenToI128(getVar(Op.Inputs[2], Builder), Builder);
  auto *Imm = llvm::ConstantInt::get(
      llvm::Type::getInt8Ty(*Ctx),
      static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal));

  // 256-bit VMPSADBW: 8 overlapping 4-byte SADs per 128-bit lane; the two lanes
  // use DIFFERENT imm8 sub-fields (low lane imm8[2:0], high lane imm8[5:3]).
  // @llvm.x86.avx2.mpsadbw implements exactly this; without it the ymm form
  // fell through unhandled (result = 0).
  if (Op.Output.Size == 32 && A->getType()->isIntegerTy(256) &&
      B->getType()->isIntegerTy(256)) {
    auto *V32I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 32);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_avx2_mpsadbw);
    return fromVec(Builder.CreateCall(Fn,
                                      {toVec(A, V32I8, Builder),
                                       toVec(B, V32I8, Builder), Imm},
                                      "mpsadbw256"),
                   Builder);
  }

  if (!A->getType()->isIntegerTy(128) || !B->getType()->isIntegerTy(128) ||
      Op.Output.Size != 16)
    return nullptr;

  auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::x86_sse41_mpsadbw);
  return fromVec(Builder.CreateCall(
                     Fn,
                     {toVec(A, V16I8, Builder), toVec(B, V16I8, Builder), Imm},
                     "mpsadbw"),
                 Builder);
}

//===----------------------------------------------------------------------===//
// Miscellaneous SIMD intrinsics (dispatch)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitMiscSimd(const MedOp &Op, Intrinsic IC,
                                          llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  if (IC == I::Pdep || IC == I::Pext || IC == I::Vpternlog)
    if (auto *R = emitBitManipSimd(Op, IC, Builder))
      return R;

  if (IC == I::MaskedLoadD || IC == I::MaskedLoadQ || IC == I::MaskedStoreD ||
      IC == I::MaskedStoreQ || IC == I::MaskedStoreB)
    return emitMaskedMemOp(Op, IC, Builder);

  if (IC == I::Pcmpistri || IC == I::Pcmpistrm || IC == I::Pcmpestri ||
      IC == I::Pcmpestrm || IC == I::PcmpistrFlag || IC == I::PcmpestrFlag)
    if (auto *R = emitPcmpStr(Op, IC, Builder))
      return R;

  if (IC == I::Pmovmskb)
    if (auto *R = emitPmovmskb(Op, Builder))
      return R;

  if (IC == I::Phminposuw)
    if (auto *R = emitPhminposuw(Op, Builder))
      return R;

  if (IC == I::Dpps)
    if (auto *R = emitDpps(Op, Builder))
      return R;

  if (IC == I::Mpsadbw)
    if (auto *R = emitMpsadbw(Op, Builder))
      return R;

  if (IC == I::X86Crc32b || IC == I::X86Crc32w || IC == I::X86Crc32d ||
      IC == I::X86Crc32q) {
    auto *AccRaw = getVar(Op.Inputs[1], Builder);
    auto *SrcRaw = getVar(Op.Inputs[2], Builder);
    llvm::Intrinsic::ID IID;
    llvm::Type *AccTy, *SrcTy;
    if (IC == I::X86Crc32q) {
      IID = llvm::Intrinsic::x86_sse42_crc32_64_64;
      AccTy = llvm::Type::getInt64Ty(*Ctx);
      SrcTy = llvm::Type::getInt64Ty(*Ctx);
    } else {
      AccTy = llvm::Type::getInt32Ty(*Ctx);
      if (IC == I::X86Crc32b) {
        IID = llvm::Intrinsic::x86_sse42_crc32_32_8;
        SrcTy = llvm::Type::getInt8Ty(*Ctx);
      } else if (IC == I::X86Crc32w) {
        IID = llvm::Intrinsic::x86_sse42_crc32_32_16;
        SrcTy = llvm::Type::getInt16Ty(*Ctx);
      } else {
        IID = llvm::Intrinsic::x86_sse42_crc32_32_32;
        SrcTy = llvm::Type::getInt32Ty(*Ctx);
      }
    }
    auto *Acc = Builder.CreateTrunc(AccRaw, AccTy);
    auto *Src = Builder.CreateTrunc(SrcRaw, SrcTy);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, IID);
    auto *CR = Builder.CreateCall(Fn, {Acc, Src}, "crc32");
    auto *OutTy = sizeToType(Op.Output.Size);
    return Builder.CreateZExtOrBitCast(CR, OutTy);
  }

  return nullptr;
}

} // namespace neverd
