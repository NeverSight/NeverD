//===- MedLLVMX86CryptoEmitter.cpp - x86 AES/SHA/PCLMUL emission -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 cryptographic intrinsic emission: AES-NI (AESENC, AESDEC, AESIMC,
/// AESKEYGENASSIST), SHA-NI (SHA1/SHA256 rounds and message schedule), and
/// PCLMULQDQ carry-less multiplication, and GFNI byte-field operations.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-x86-crypto"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// AES / PCLMULQDQ intrinsics
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitAesIntrinsic(const MedOp &Op, Intrinsic IC,
                                              llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  auto *V2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);

  auto GetInput = [&](unsigned Idx) { return getVar(Op.Inputs[Idx], Builder); };

  struct AesPair {
    I Code;
    llvm::Intrinsic::ID IID;
    const char *Name;
  };
  static const AesPair Pairs[] = {
      {I::AesEnc, llvm::Intrinsic::x86_aesni_aesenc, "aesenc"},
      {I::AesEncLast, llvm::Intrinsic::x86_aesni_aesenclast, "aesenclast"},
      {I::AesDec, llvm::Intrinsic::x86_aesni_aesdec, "aesdec"},
      {I::AesDecLast, llvm::Intrinsic::x86_aesni_aesdeclast, "aesdeclast"},
  };
  if (Op.Output.Size == 16 && Op.NumInputs >= 3) {
    auto *A = widenToI128(GetInput(1), Builder);
    auto *B = widenToI128(GetInput(2), Builder);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128)) {
      for (auto &P : Pairs) {
        if (IC == P.Code) {
          auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, P.IID);
          auto *R = Builder.CreateCall(
              Fn, {toVec(A, V2I64, Builder), toVec(B, V2I64, Builder)}, P.Name);
          return fromVec(R, Builder);
        }
      }
    }
  }

  if (Op.Output.Size == 16 && Op.NumInputs >= 2 && IC == I::AesImc) {
    auto *Src = widenToI128(GetInput(1), Builder);
    if (Src->getType()->isIntegerTy(128)) {
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::x86_aesni_aesimc);
      return fromVec(
          Builder.CreateCall(Fn, {toVec(Src, V2I64, Builder)}, "aesimc"),
          Builder);
    }
  }

  if (Op.Output.Size == 16 && Op.NumInputs >= 3 && IC == I::AesKeyGenAssist &&
      Op.Inputs[Op.NumInputs - 1].isConst()) {
    auto *Src = widenToI128(GetInput(1), Builder);
    if (Src->getType()->isIntegerTy(128)) {
      auto *Rcon = llvm::ConstantInt::get(
          llvm::Type::getInt8Ty(*Ctx),
          static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal));
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::x86_aesni_aeskeygenassist);
      return fromVec(
          Builder.CreateCall(Fn, {toVec(Src, V2I64, Builder), Rcon}, "aeskga"),
          Builder);
    }
  }

  if (IC == I::Pclmulqdq && Op.Output.Size == 16 && Op.NumInputs >= 4) {
    auto *A = widenToI128(GetInput(1), Builder);
    auto *B = widenToI128(GetInput(2), Builder);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128) &&
        Op.Inputs[Op.NumInputs - 1].isConst()) {
      auto *Imm = llvm::ConstantInt::get(
          llvm::Type::getInt8Ty(*Ctx),
          static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal));
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::x86_pclmulqdq);
      return fromVec(Builder.CreateCall(Fn,
                                        {toVec(A, V2I64, Builder),
                                         toVec(B, V2I64, Builder), Imm},
                                        "pclmulqdq"),
                     Builder);
    }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// SHA intrinsics
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitShaIntrinsic(const MedOp &Op, Intrinsic IC,
                                              llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);

  // An xmm operand fed by a zero-extending gpr->xmm move is propagated to the
  // narrow GPR, so widen it back to i128 (high bits zero) before the i128 gate.
  auto GetInput = [&](unsigned Idx) {
    return widenToI128(getVar(Op.Inputs[Idx], Builder), Builder);
  };

  if (Op.Output.Size != 16)
    return nullptr;

  struct ShaPair {
    I Code;
    llvm::Intrinsic::ID IID;
  };
  static const ShaPair TwoOp[] = {
      {I::Sha1Nexte, llvm::Intrinsic::x86_sha1nexte},
      {I::Sha1Msg1, llvm::Intrinsic::x86_sha1msg1},
      {I::Sha1Msg2, llvm::Intrinsic::x86_sha1msg2},
      {I::Sha256Msg1, llvm::Intrinsic::x86_sha256msg1},
      {I::Sha256Msg2, llvm::Intrinsic::x86_sha256msg2},
  };
  if (Op.NumInputs >= 3) {
    auto *A = GetInput(1);
    auto *B = GetInput(2);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128)) {
      for (auto &P : TwoOp) {
        if (IC == P.Code) {
          auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(Mod, P.IID);
          return fromVec(Builder.CreateCall(Fn,
                                            {toVec(A, V4I32, Builder),
                                             toVec(B, V4I32, Builder)},
                                            "sha"),
                         Builder);
        }
      }
    }
  }

  if (IC == I::Sha256Rnds2 && Op.NumInputs >= 4) {
    auto *A = GetInput(1);
    auto *B = GetInput(2);
    auto *C = GetInput(3);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128) &&
        C->getType()->isIntegerTy(128)) {
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::x86_sha256rnds2);
      return fromVec(Builder.CreateCall(Fn,
                                        {toVec(A, V4I32, Builder),
                                         toVec(B, V4I32, Builder),
                                         toVec(C, V4I32, Builder)},
                                        "sha256rnds2"),
                     Builder);
    }
  }

  if (IC == I::Sha1Rnds4 && Op.NumInputs >= 4 &&
      Op.Inputs[Op.NumInputs - 1].isConst()) {
    auto *A = GetInput(1);
    auto *B = GetInput(2);
    if (A->getType()->isIntegerTy(128) && B->getType()->isIntegerTy(128)) {
      auto *Imm = llvm::ConstantInt::get(
          llvm::Type::getInt8Ty(*Ctx),
          static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal));
      auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::x86_sha1rnds4);
      return fromVec(Builder.CreateCall(Fn,
                                        {toVec(A, V4I32, Builder),
                                         toVec(B, V4I32, Builder), Imm},
                                        "sha1rnds4"),
                     Builder);
    }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// GFNI intrinsics
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitGfniIntrinsic(const MedOp &Op, Intrinsic IC,
                                               llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  if (IC != I::Gf2p8MulB && IC != I::Gf2p8AffineQb &&
      IC != I::Gf2p8AffineInvQb)
    return nullptr;
  if (Op.Output.Size != 16 && Op.Output.Size != 32)
    return nullptr;

  const bool Is256 = Op.Output.Size == 32;
  const unsigned NumBytes = Is256 ? 32 : 16;
  auto *VTy =
      llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), NumBytes);
  auto GetInput = [&](unsigned Idx) {
    return toVec(getVar(Op.Inputs[Idx], Builder), VTy, Builder);
  };
  auto Splat = [&](uint8_t Value) {
    return llvm::ConstantVector::getSplat(
        llvm::ElementCount::getFixed(NumBytes),
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(*Ctx), Value));
  };

  // Lower in target-independent integer IR.  Emitting the x86 GFNI LLVM
  // intrinsic directly would make recompilation depend on a +gfni subtarget;
  // the lifted program must remain executable on the baseline x86 target used
  // by the semantic harness.
  auto GfMul = [&](llvm::Value *A, llvm::Value *B) {
    llvm::Value *Result = Splat(0);
    for (unsigned Bit = 0; Bit < 8; ++Bit) {
      auto *UseA = Builder.CreateICmpNE(Builder.CreateAnd(B, Splat(1)),
                                        Splat(0), "gfni.mul.bit");
      Result = Builder.CreateXor(
          Result, Builder.CreateSelect(UseA, A, Splat(0)), "gfni.mul.acc");

      auto *Reduce = Builder.CreateICmpNE(
          Builder.CreateAnd(A, Splat(0x80)), Splat(0), "gfni.mul.reduce");
      A = Builder.CreateShl(A, Splat(1), "gfni.mul.shift");
      A = Builder.CreateXor(
          A, Builder.CreateSelect(Reduce, Splat(0x1b), Splat(0)),
          "gfni.mul.poly");
      B = Builder.CreateLShr(B, Splat(1), "gfni.mul.next");
    }
    return Result;
  };

  if (IC == I::Gf2p8MulB) {
    if (Op.NumInputs < 3)
      return nullptr;
    return fromVec(GfMul(GetInput(1), GetInput(2)), Builder);
  }

  if (Op.NumInputs < 4 || !Op.Inputs[Op.NumInputs - 1].isConst())
    return nullptr;
  llvm::Value *X = GetInput(1);
  llvm::Value *Matrix = GetInput(2);

  if (IC == I::Gf2p8AffineInvQb) {
    // In GF(2^8), x^-1 = x^254 (and the same chain naturally maps 0 to 0).
    llvm::Value *Inverse = Splat(1);
    llvm::Value *Base = X;
    unsigned Exponent = 254;
    while (Exponent) {
      if (Exponent & 1)
        Inverse = GfMul(Inverse, Base);
      Base = GfMul(Base, Base);
      Exponent >>= 1;
    }
    X = Inverse;
  }

  const uint8_t Imm =
      static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal);
  llvm::Value *Result = Splat(0);
  for (unsigned Bit = 0; Bit < 8; ++Bit) {
    // Each 64-bit lane contains eight matrix rows.  Output bit N uses row
    // byte 7-N, broadcast independently within every eight-byte lane.
    std::vector<int> RowMask(NumBytes);
    for (unsigned Byte = 0; Byte < NumBytes; ++Byte)
      RowMask[Byte] = static_cast<int>((Byte & ~7u) + (7u - Bit));
    llvm::Value *Row =
        Builder.CreateShuffleVector(Matrix, Matrix, RowMask, "gfni.row");
    llvm::Value *Parity = Builder.CreateAnd(Row, X, "gfni.dot");
    Parity = Builder.CreateXor(
        Parity, Builder.CreateLShr(Parity, Splat(4)), "gfni.parity4");
    Parity = Builder.CreateXor(
        Parity, Builder.CreateLShr(Parity, Splat(2)), "gfni.parity2");
    Parity = Builder.CreateXor(
        Parity, Builder.CreateLShr(Parity, Splat(1)), "gfni.parity1");
    Parity = Builder.CreateAnd(Parity, Splat(1));
    if ((Imm >> Bit) & 1)
      Parity = Builder.CreateXor(Parity, Splat(1));
    Result = Builder.CreateOr(Result,
                              Builder.CreateShl(Parity, Splat(Bit)),
                              "gfni.affine.acc");
  }
  return fromVec(Result, Builder);
}

} // namespace neverd
