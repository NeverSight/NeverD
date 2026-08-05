//===- MedLLVMX86ShuffleEmitter.cpp - x86 shuffle/blend emission -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86 vector permutation intrinsic emission: shuffle (PSHUFD, PSHUFLW,
/// PSHUFHW, PSHUFB, SHUFPS, SHUFPD, PSHUFW), unpack (PUNPCKL*, PUNPCKH*,
/// UNPCKLPS, etc.), blend (BLENDD, PALIGNR), permute (PERM2F128, PERMD),
/// broadcast (VPBROADCAST*), and duplicate (MOVDDUP, MOVSHDUP, MOVSLDUP).
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-x86-shuffle"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// PSHUFB (SSSE3)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPshufb(const MedOp &Op,
                                        llvm::IRBuilder<> &Builder) {
  if (Op.NumInputs < 3)
    return nullptr;
  auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  auto *B = widenToI128(getVar(Op.Inputs[2], Builder), Builder);

  // 256-bit AVX2 PSHUFB: applies within each 128-bit lane (control byte's
  // in-lane index), which is exactly the semantics of @llvm.x86.avx2.pshuf.b.
  // Without this the 256-bit form falls through unhandled (result = 0).
  if (Op.Output.Size == 32 && A->getType()->isIntegerTy(256) &&
      B->getType()->isIntegerTy(256)) {
    auto *V32I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 32);
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::x86_avx2_pshuf_b);
    return fromVec(Builder.CreateCall(Fn,
                                      {toVec(A, V32I8, Builder),
                                       toVec(B, V32I8, Builder)},
                                      "pshufb256"),
                   Builder);
  }

  if (!A->getType()->isIntegerTy(128) || !B->getType()->isIntegerTy(128) ||
      Op.Output.Size != 16)
    return nullptr;
  auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::x86_ssse3_pshuf_b_128);
  return fromVec(
      Builder.CreateCall(
          Fn, {toVec(A, V16I8, Builder), toVec(B, V16I8, Builder)}, "pshufb"),
      Builder);
}

//===----------------------------------------------------------------------===//
// Unpack intrinsics (SSE2)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitUnpackShuffle(const MedOp &Op, Intrinsic IC,
                                               llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  // 16-byte (SSE, one 128-bit lane) or 32-byte (AVX2, two 128-bit lanes).  The
  // unpack applies INDEPENDENTLY within each 128-bit lane, so the 256-bit form
  // interleaves the low/high half of A's lane with B's lane for both lanes.
  bool Is256 = (Op.Output.Size == 32);
  if ((Op.Output.Size != 16 && !Is256) || Op.NumInputs < 3)
    return nullptr;
  auto *A = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  auto *B = widenToI128(getVar(Op.Inputs[2], Builder), Builder);
  unsigned WantBits = Is256 ? 256 : 128;
  if (!A->getType()->isIntegerTy(WantBits) ||
      !B->getType()->isIntegerTy(WantBits))
    return nullptr;

  // Element width (bytes) and whether this is the "high" half unpack.
  unsigned ElemBytes = 0;
  bool IsHigh = false;
  switch (IC) {
  case I::Punpcklbw: ElemBytes = 1; IsHigh = false; break;
  case I::Punpckhbw: ElemBytes = 1; IsHigh = true;  break;
  case I::Punpcklwd: ElemBytes = 2; IsHigh = false; break;
  case I::Punpckhwd: ElemBytes = 2; IsHigh = true;  break;
  case I::Punpckldq:
  case I::Unpcklps:  ElemBytes = 4; IsHigh = false; break;
  case I::Punpckhdq:
  case I::Unpckhps:  ElemBytes = 4; IsHigh = true;  break;
  case I::Punpcklqdq:
  case I::Unpcklpd:  ElemBytes = 8; IsHigh = false; break;
  case I::Punpckhqdq:
  case I::Unpckhpd:  ElemBytes = 8; IsHigh = true;  break;
  default:
    return nullptr;
  }

  unsigned E = 16 / ElemBytes;             // elements per 128-bit lane
  unsigned Lanes = Is256 ? 2 : 1;
  unsigned Total = E * Lanes;              // total elements across both operands
  auto *ElemTy = llvm::IntegerType::get(*Ctx, ElemBytes * 8);
  auto *VTy = llvm::FixedVectorType::get(ElemTy, Total);

  // shufflevector(A, B): A occupies indices [0, Total), B [Total, 2*Total).
  // Within lane L, A's elements start at L*E and B's at Total + L*E.  Each lane
  // interleaves E/2 elements from the low (or high) half of that lane.
  std::vector<int> Mask(Total);
  unsigned Half = E / 2;
  unsigned HalfBase = IsHigh ? Half : 0;
  for (unsigned L = 0; L < Lanes; ++L) {
    unsigned BaseA = L * E;
    unsigned BaseB = Total + L * E;
    for (unsigned K = 0; K < Half; ++K) {
      Mask[L * E + 2 * K]     = BaseA + HalfBase + K;
      Mask[L * E + 2 * K + 1] = BaseB + HalfBase + K;
    }
  }
  return fromVec(Builder.CreateShuffleVector(toVec(A, VTy, Builder),
                                             toVec(B, VTy, Builder), Mask,
                                             "unpack"),
                 Builder);
}

//===----------------------------------------------------------------------===//
// Immediate-controlled shuffles (PSHUFD, PSHUFLW, PSHUFHW, SHUFPS,
// SHUFPD, PALIGNR, BLENDD, PERM2F128)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitImmShuffle(const MedOp &Op, Intrinsic IC,
                                            llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  if (Op.Output.Size != 16 && Op.Output.Size != 32)
    return nullptr;
  int Last = Op.NumInputs - 1;
  if (Last < 1 || !Op.Inputs[Last].isConst() || Op.Inputs[Last].Size > 1)
    return nullptr;

  uint8_t Imm = static_cast<uint8_t>(Op.Inputs[Last].ConstVal);

  auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);
  auto *V2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);
  auto *V4I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 4);
  auto *V8I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 8);
  auto *V16I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 16);
  auto *V8I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 8);
  auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);

  auto *Src = (Op.NumInputs >= 2)
                  ? widenToI128(getVar(Op.Inputs[1], Builder), Builder)
                  : nullptr;
  bool Wide = Src && Src->getType()->isIntegerTy() &&
              (Src->getType()->getIntegerBitWidth() == 128 ||
               Src->getType()->getIntegerBitWidth() == 256);

  //--- PSHUFD ---
  // imm8 selects 4 source dwords; on ymm it applies independently within each
  // 128-bit lane (NOT a cross-lane qword permute), so build an 8-dword mask
  // with the same per-lane pattern offset by 4 for the high lane.
  if (Wide && IC == I::Pshufd) {
    bool Is256 = (Op.Output.Size == 32);
    auto *SV =
        toVec(Src, Is256 ? (llvm::Type *)V8I32 : (llvm::Type *)V4I32, Builder);
    int N = Is256 ? 8 : 4;
    std::vector<int> Mask(N);
    for (int J = 0; J < N; ++J) {
      int Lane = J / 4;
      Mask[J] = Lane * 4 + ((Imm >> ((J & 3) * 2)) & 3);
    }
    return fromVec(Builder.CreateShuffleVector(SV, SV, Mask, "pshufd"),
                   Builder);
  }

  //--- PSHUFLW ---
  // imm8 permutes the low 4 words of each 128-bit lane; the high 4 words pass
  // through.  On ymm the pattern repeats independently in each 128-bit lane
  // (8 words each), so build a per-lane mask over <16 x i16> for the 256 form.
  if (Wide && IC == I::Pshuflw) {
    bool Is256 = (Op.Output.Size == 32);
    auto *SV =
        toVec(Src, Is256 ? (llvm::Type *)V16I16 : (llvm::Type *)V8I16, Builder);
    int Lanes = Is256 ? 2 : 1;
    std::vector<int> Mask(Lanes * 8);
    for (int L = 0; L < Lanes; ++L) {
      int Base = L * 8;
      for (int J = 0; J < 4; ++J)
        Mask[Base + J] = Base + ((Imm >> (J * 2)) & 3);
      for (int J = 4; J < 8; ++J)
        Mask[Base + J] = Base + J;
    }
    return fromVec(Builder.CreateShuffleVector(SV, SV, Mask, "pshuflw"),
                   Builder);
  }

  //--- PSHUFHW ---
  // imm8 permutes the high 4 words of each 128-bit lane; the low 4 words pass
  // through.  On ymm the pattern repeats independently in each 128-bit lane.
  if (Wide && IC == I::Pshufhw) {
    bool Is256 = (Op.Output.Size == 32);
    auto *SV =
        toVec(Src, Is256 ? (llvm::Type *)V16I16 : (llvm::Type *)V8I16, Builder);
    int Lanes = Is256 ? 2 : 1;
    std::vector<int> Mask(Lanes * 8);
    for (int L = 0; L < Lanes; ++L) {
      int Base = L * 8;
      for (int J = 0; J < 4; ++J)
        Mask[Base + J] = Base + J;
      for (int J = 0; J < 4; ++J)
        Mask[Base + 4 + J] = Base + 4 + ((Imm >> (J * 2)) & 3);
    }
    return fromVec(Builder.CreateShuffleVector(SV, SV, Mask, "pshufhw"),
                   Builder);
  }

  //--- Two-source immediate shuffles: SHUFPS, SHUFPD, PALIGNR, BLENDD ---
  if (Op.NumInputs >= 4) {
    auto *A2 = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
    auto *B2 = widenToI128(getVar(Op.Inputs[2], Builder), Builder);
    auto *V32I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 32);

    // 256-bit YMM: SHUFPS/SHUFPD/PALIGNR apply the immediate INDEPENDENTLY
    // within each 128-bit lane (the same in-lane pattern, offset by one lane).
    // Without this, the operands stay 256-bit, the isIntegerTy(128) path below
    // is skipped, and the INTRINSIC falls through unhandled (result = 0).
    if (Op.Output.Size == 32 && A2->getType()->isIntegerTy(256) &&
        B2->getType()->isIntegerTy(256)) {
      if (IC == I::Shufps) {
        // Per lane: 2 dwords from src1 (A) then 2 from src2 (B).  In the
        // <8 x i32>||<8 x i32> concat, A is 0..7 and B is 8..15.
        std::vector<int> Mask(8);
        for (int Lane = 0; Lane < 2; ++Lane) {
          int AB = Lane * 4;      // A base for this lane
          int BB = 8 + Lane * 4;  // B base for this lane
          Mask[Lane * 4 + 0] = AB + ((Imm >> 0) & 3);
          Mask[Lane * 4 + 1] = AB + ((Imm >> 2) & 3);
          Mask[Lane * 4 + 2] = BB + ((Imm >> 4) & 3);
          Mask[Lane * 4 + 3] = BB + ((Imm >> 6) & 3);
        }
        return fromVec(Builder.CreateShuffleVector(toVec(A2, V8I32, Builder),
                                                   toVec(B2, V8I32, Builder),
                                                   Mask, "shufps256"),
                       Builder);
      }
      if (IC == I::Shufpd) {
        // Per lane: qword0 from src1 (A), qword1 from src2 (B); imm bits
        // {0,1} select lane 0, bits {2,3} select lane 1.  In the
        // <4 x i64>||<4 x i64> concat, A is 0..3 and B is 4..7.
        std::vector<int> Mask(4);
        Mask[0] = 0 + ((Imm >> 0) & 1);
        Mask[1] = 4 + ((Imm >> 1) & 1);
        Mask[2] = 2 + ((Imm >> 2) & 1);
        Mask[3] = 6 + ((Imm >> 3) & 1);
        return fromVec(Builder.CreateShuffleVector(toVec(A2, V4I64, Builder),
                                                   toVec(B2, V4I64, Builder),
                                                   Mask, "shufpd256"),
                       Builder);
      }
      if (IC == I::Palignr) {
        // Per 128-bit lane: result = (A.lane : B.lane) >> (imm*8), low 16 bytes.
        // shufflevector(B32, A32): B is 0..31 (low), A is 32..63 (high); within
        // a lane the byte window is [laneBase, laneBase+16).
        int Shift = (Imm > 32) ? 32 : static_cast<int>(Imm);
        std::vector<int> Mask(32);
        for (int Lane = 0; Lane < 2; ++Lane) {
          int BB = Lane * 16;       // B lane bytes in B32
          int AB = 32 + Lane * 16;  // A lane bytes in A32
          for (int J = 0; J < 16; ++J) {
            int Idx = J + Shift;
            if (Idx < 16)
              Mask[Lane * 16 + J] = BB + Idx;
            else if (Idx < 32)
              Mask[Lane * 16 + J] = AB + (Idx - 16);
            else
              Mask[Lane * 16 + J] = -1;
          }
        }
        return fromVec(Builder.CreateShuffleVector(toVec(B2, V32I8, Builder),
                                                   toVec(A2, V32I8, Builder),
                                                   Mask, "palignr256"),
                       Builder);
      }
    }

    if (A2->getType()->isIntegerTy(128) && B2->getType()->isIntegerTy(128)) {
      if (IC == I::Shufps) {
        std::vector<int> Mask = {(Imm >> 0) & 3, (Imm >> 2) & 3,
                                 4 + ((Imm >> 4) & 3), 4 + ((Imm >> 6) & 3)};
        return fromVec(Builder.CreateShuffleVector(toVec(A2, V4I32, Builder),
                                                   toVec(B2, V4I32, Builder),
                                                   Mask, "shufps"),
                       Builder);
      }
      if (IC == I::Shufpd) {
        std::vector<int> Mask = {Imm & 1, 2 + ((Imm >> 1) & 1)};
        return fromVec(Builder.CreateShuffleVector(toVec(A2, V2I64, Builder),
                                                   toVec(B2, V2I64, Builder),
                                                   Mask, "shufpd"),
                       Builder);
      }
      if (IC == I::Palignr) {
        // PALIGNR: result = (A2:B2) >> (imm*8), low 128 bits.
        // shufflevector(B2, A2, mask): indices 0-15 = B2 (low), 16-31 = A2
        // (high).
        int Shift = (Imm > 32) ? 32 : Imm;
        std::vector<int> Mask(16);
        for (int J = 0; J < 16; ++J) {
          int Idx = J + Shift;
          Mask[J] = (Idx >= 32) ? -1 : Idx;
        }
        return fromVec(Builder.CreateShuffleVector(toVec(B2, V16I8, Builder),
                                                   toVec(A2, V16I8, Builder),
                                                   Mask, "palignr"),
                       Builder);
      }
      if (IC == I::Blendd) {
        bool Is256 = (Op.Output.Size == 32);
        int N = Is256 ? 8 : 4;
        auto *VTy = Is256 ? (llvm::Type *)V8I32 : (llvm::Type *)V4I32;
        std::vector<int> Mask(N);
        for (int J = 0; J < N; ++J)
          Mask[J] = (Imm & (1 << J)) ? (N + J) : J;
        return fromVec(Builder.CreateShuffleVector(toVec(A2, VTy, Builder),
                                                   toVec(B2, VTy, Builder),
                                                   Mask, "blendd"),
                       Builder);
      }
    }
  }

  //--- VPERM2F128 ---
  if (Op.NumInputs >= 4 && IC == I::Perm2f128 && Op.Output.Size == 32) {
    auto *A = toVec(getVar(Op.Inputs[1], Builder), V4I64, Builder);
    auto *B = toVec(getVar(Op.Inputs[2], Builder), V4I64, Builder);
    auto Lane = [&](int Ctrl) -> std::pair<int, int> {
      if (Ctrl & 0x8)
        return {-1, -1};
      int S = Ctrl & 3;
      int Base = (S < 2) ? (S * 2) : (4 + (S - 2) * 2);
      return {Base, Base + 1};
    };
    auto [L0, L1] = Lane(Imm & 0xF);
    auto [H0, H1] = Lane((Imm >> 4) & 0xF);
    return fromVec(Builder.CreateShuffleVector(
                       A, B, std::vector<int>{L0, L1, H0, H1}, "perm2f128"),
                   Builder);
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// Single-source duplicates (MOVDDUP, MOVSHDUP, MOVSLDUP)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitMovDup(const MedOp &Op, Intrinsic IC,
                                        llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  bool Is256 = (Op.Output.Size == 32);
  if ((Op.Output.Size != 16 && !Is256) || Op.NumInputs < 2)
    return nullptr;
  auto *Src = widenToI128(getVar(Op.Inputs[1], Builder), Builder);
  unsigned WantBits = Is256 ? 256 : 128;
  if (!Src->getType()->isIntegerTy(WantBits))
    return nullptr;

  auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);
  auto *V2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);
  auto *V8I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 8);
  auto *V4I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 4);

  // MOVDDUP duplicates even qwords; MOVSHDUP/MOVSLDUP duplicate odd/even dwords.
  // On 256-bit the pattern repeats independently in each 128-bit lane.
  llvm::Type *VTy = nullptr;
  std::vector<int> Mask;
  const char *Name = nullptr;
  if (IC == I::Movddup) {
    VTy = Is256 ? (llvm::Type *)V4I64 : (llvm::Type *)V2I64;
    Mask = Is256 ? std::vector<int>{0, 0, 2, 2} : std::vector<int>{0, 0};
    Name = "movddup";
  } else if (IC == I::Movshdup) {
    VTy = Is256 ? (llvm::Type *)V8I32 : (llvm::Type *)V4I32;
    Mask = Is256 ? std::vector<int>{1, 1, 3, 3, 5, 5, 7, 7}
                 : std::vector<int>{1, 1, 3, 3};
    Name = "movshdup";
  } else if (IC == I::Movsldup) {
    VTy = Is256 ? (llvm::Type *)V8I32 : (llvm::Type *)V4I32;
    Mask = Is256 ? std::vector<int>{0, 0, 2, 2, 4, 4, 6, 6}
                 : std::vector<int>{0, 0, 2, 2};
    Name = "movsldup";
  }
  if (!VTy)
    return nullptr;
  return fromVec(Builder.CreateShuffleVector(toVec(Src, VTy, Builder),
                                             toVec(Src, VTy, Builder), Mask,
                                             Name),
                 Builder);
}

//===----------------------------------------------------------------------===//
// MMX PSHUFW
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPshufw(const MedOp &Op,
                                        llvm::IRBuilder<> &Builder) {
  if (Op.Output.Size != 8 || Op.NumInputs < 3 ||
      !Op.Inputs[Op.NumInputs - 1].isConst())
    return nullptr;
  uint8_t Imm = static_cast<uint8_t>(Op.Inputs[Op.NumInputs - 1].ConstVal);
  auto *Src = getVar(Op.Inputs[1], Builder);
  auto *V4I16Ty = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 4);
  auto *I64Ty = llvm::Type::getInt64Ty(*Ctx);
  auto *SV =
      Builder.CreateBitCast(Builder.CreateZExtOrTrunc(Src, I64Ty), V4I16Ty);
  std::vector<int> Mask = {(Imm >> 0) & 3, (Imm >> 2) & 3, (Imm >> 4) & 3,
                           (Imm >> 6) & 3};
  auto *R = Builder.CreateBitCast(
      Builder.CreateShuffleVector(SV, SV, Mask, "pshufw"), I64Ty);
  return (sizeToType(Op.Output.Size) != I64Ty)
             ? Builder.CreateZExtOrTrunc(R, sizeToType(Op.Output.Size))
             : R;
}

//===----------------------------------------------------------------------===//
// Broadcast (VPBROADCAST{B,W,D,Q,SS,SD})
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitVBroadcast(const MedOp &Op, Intrinsic IC,
                                            llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  if ((Op.Output.Size != 16 && Op.Output.Size != 32) || Op.NumInputs < 2)
    return nullptr;

  auto *V16I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 16);
  auto *V8I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 8);
  auto *V4I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 4);
  auto *V2I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 2);
  auto *V4I64 = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(*Ctx), 4);
  auto *V8I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 8);
  auto *V32I8 = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(*Ctx), 32);
  auto *V16I16 = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*Ctx), 16);

  bool Is256 = (Op.Output.Size == 32);
  llvm::Type *BVTy = nullptr;
  int Cnt = 0;
  switch (IC) {
  case I::BroadcastSS:
  case I::BroadcastD:
    BVTy = Is256 ? V8I32 : V4I32;
    Cnt = Is256 ? 8 : 4;
    break;
  case I::BroadcastSD:
  case I::BroadcastQ:
    BVTy = Is256 ? V4I64 : V2I64;
    Cnt = Is256 ? 4 : 2;
    break;
  case I::BroadcastW:
    BVTy = Is256 ? V16I16 : V8I16;
    Cnt = Is256 ? 16 : 8;
    break;
  case I::BroadcastB:
    BVTy = Is256 ? V32I8 : V16I8;
    Cnt = Is256 ? 32 : 16;
    break;
  default:
    return nullptr;
  }

  auto *BSrc = getVar(Op.Inputs[1], Builder);
  llvm::Value *Vec;
  if (BSrc->getType()->isIntegerTy(128) || BSrc->getType()->isIntegerTy(256))
    Vec = toVec(BSrc, BVTy, Builder);
  else {
    auto *Splat = llvm::UndefValue::get(BVTy);
    Vec = Builder.CreateInsertElement(
        Splat, BSrc, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 0));
    Vec = Builder.CreateBitCast(Vec, BVTy);
  }
  return fromVec(Builder.CreateShuffleVector(Vec, Vec, std::vector<int>(Cnt, 0),
                                             "broadcast"),
                 Builder);
}

//===----------------------------------------------------------------------===//
// VPERMD (AVX2)
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitPermd(const MedOp &Op,
                                       llvm::IRBuilder<> &Builder) {
  if (Op.Output.Size != 32 || Op.NumInputs < 3)
    return nullptr;
  auto *Idx = getVar(Op.Inputs[1], Builder);
  auto *Src = getVar(Op.Inputs[2], Builder);
  if (!Idx->getType()->isIntegerTy(256) || !Src->getType()->isIntegerTy(256))
    return nullptr;
  auto *V8I32 = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*Ctx), 8);
  auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::x86_avx2_permd);
  return fromVec(Builder.CreateCall(
                     Fn,
                     {toVec(Src, V8I32, Builder), toVec(Idx, V8I32, Builder)},
                     "vpermd"),
                 Builder);
}

//===----------------------------------------------------------------------===//
// Top-level shuffle dispatch
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitShuffleIntrinsic(const MedOp &Op, Intrinsic IC,
                                                  llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;

  if (IC == I::Pshufb)
    if (auto *R = emitPshufb(Op, Builder))
      return R;

  switch (IC) {
  case I::Punpcklbw:
  case I::Punpckhbw:
  case I::Punpcklwd:
  case I::Punpckhwd:
  case I::Punpckldq:
  case I::Punpckhdq:
  case I::Punpcklqdq:
  case I::Punpckhqdq:
  case I::Unpcklps:
  case I::Unpckhps:
  case I::Unpcklpd:
  case I::Unpckhpd:
    if (auto *R = emitUnpackShuffle(Op, IC, Builder))
      return R;
    break;
  default:
    break;
  }

  switch (IC) {
  case I::Pshufd:
  case I::Pshuflw:
  case I::Pshufhw:
  case I::Shufps:
  case I::Shufpd:
  case I::Palignr:
  case I::Blendd:
  case I::Perm2f128:
    if (auto *R = emitImmShuffle(Op, IC, Builder))
      return R;
    break;
  default:
    break;
  }

  switch (IC) {
  case I::Movddup:
  case I::Movshdup:
  case I::Movsldup:
    if (auto *R = emitMovDup(Op, IC, Builder))
      return R;
    break;
  default:
    break;
  }

  if (IC == I::Pshufw)
    if (auto *R = emitPshufw(Op, Builder))
      return R;

  switch (IC) {
  case I::BroadcastSS:
  case I::BroadcastSD:
  case I::BroadcastD:
  case I::BroadcastQ:
  case I::BroadcastW:
  case I::BroadcastB:
    if (auto *R = emitVBroadcast(Op, IC, Builder))
      return R;
    break;
  default:
    break;
  }

  if (IC == I::Permd)
    if (auto *R = emitPermd(Op, Builder))
      return R;

  return nullptr;
}

} // namespace neverd
