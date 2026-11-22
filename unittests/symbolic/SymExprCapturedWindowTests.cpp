//===- SymExprCapturedWindowTests.cpp - Exact constructor regressions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bitvector evaluations and constructor refusal controls.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <vector>

using namespace neverd::symbolic;

TEST(SymExpr, SelectedContiguousSlicesReconstructTheCapturedWord) {
  for (unsigned Width : {16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto Test = C.mkVar("test", 1), Inner = C.mkVar("inner", 1);
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto One = C.mkConst(Width, 1);
    auto Selected = C.mkIte(Test, C.mkIte(Inner, One, X), Y);
    std::vector<SymRef> Lanes;
    for (unsigned End = Width; End; End -= 8)
      Lanes.push_back(C.mkIte(Test,
                              C.mkIte(Inner, C.mkExtract(One, End - 8, 8),
                                      C.mkExtract(X, End - 8, 8)),
                              C.mkExtract(Y, End - 8, 8)));
    const auto Packed = C.mkConcat(Lanes);
    EXPECT_EQ(Packed, Selected);
    for (unsigned Flags = 0; Flags != 4; ++Flags) {
      std::vector<llvm::APInt> Values{llvm::APInt(1, Flags & 1),
                                      llvm::APInt(1, Flags >> 1),
                                      llvm::APInt::getSignMask(Width) | 0x93,
                                      llvm::APInt::getAllOnes(Width) ^ 0x17};
      EXPECT_EQ(C.eval(Packed, Values),
                (Flags & 1) ? (Flags & 2 ? llvm::APInt(Width, 1) : Values[2])
                            : Values[3]);
    }
    // A different guard or a reversed byte is a different value.
    std::swap(Lanes[0], Lanes[1]);
    EXPECT_NE(C.mkConcat(Lanes), Selected);
  }
}

TEST(SymExpr, AdjacentSlicesOfProjectedArithmeticPrefixesRejoinTheFullWord) {
  for (unsigned Width : {16u, 32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    for (auto Word : {C.mkAdd(X, Y), C.mkMul(X, Y), C.mkOr(X, Y)}) {
      std::vector<SymRef> Bytes;
      for (unsigned End = Width; End; End -= 8)
        Bytes.push_back(C.mkExtract(C.mkExtract(Word, 0, End), End - 8, 8));
      EXPECT_EQ(C.mkConcat(Bytes), Word);
      Bytes[0] = C.mkXor(Bytes[0], C.mkConst(8, 1));
      EXPECT_NE(C.mkConcat(Bytes), Word);
    }
  }
}

TEST(SymExpr, ConstantShiftSlicesUseTheSourceBitCoordinates) {
  for (unsigned Width : {8u, 32u, 64u, 128u}) {
    SymContext C;
    auto X = C.mkVar("x", Width);
    for (unsigned Amount : {0u, 1u, 7u, 8u, Width - 1, Width, Width + 1})
      for (unsigned Low : {0u, Width / 2, Width - 1})
        for (unsigned Bits : {1u, Width - Low}) {
          const auto Shift = C.mkConst(Width, Amount);
          for (auto Op : {SymOp::Shl, SymOp::LShr, SymOp::AShr}) {
            SCOPED_TRACE(::testing::Message()
                         << Width << ' ' << Amount << ' ' << Low << ' ' << Bits
                         << ' ' << unsigned(Op));
            auto Shifted = Op == SymOp::Shl    ? C.mkShl(X, Shift)
                           : Op == SymOp::LShr ? C.mkLShr(X, Shift)
                                               : C.mkAShr(X, Shift);
            const auto Slice = C.mkExtract(Shifted, Low, Bits);
            // Byte windows of byte shifts read the source directly. Other
            // narrowed shifts keep their count for the consumers that read it.
            const bool ByteWindow =
                Amount % 8 == 0 && Low % 8 == 0 && Bits % 8 == 0;
            if (Op != SymOp::Shl && Low + Amount + Bits <= Width)
              EXPECT_EQ(Slice == C.mkExtract(X, Low + Amount, Bits),
                        ByteWindow || Amount == 0);
            for (uint64_t V : {0u, 1u, 0x53u, 0x80u, 0xffu}) {
              std::vector<llvm::APInt> Values{llvm::APInt::getSignMask(Width) |
                                              V};
              const unsigned Count = std::min(Amount, Width);
              auto Want = Op == SymOp::Shl    ? Values[0].shl(Count)
                          : Op == SymOp::LShr ? Values[0].lshr(Count)
                                              : Values[0].ashr(Count);
              EXPECT_EQ(C.eval(Slice, Values), Want.extractBits(Bits, Low));
            }
          }
        }
  }
}

TEST(SymExpr, ShiftedByteDisjunctionReusesSelectedWordReconstruction) {
  for (unsigned Width : {16u, 32u, 64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto Test = C.mkVar("test", 1), Inner = C.mkVar("inner", 1);
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto One = C.mkConst(Width, 1);
    std::vector<SymRef> Parts;
    for (unsigned Low = 0; Low < Width; Low += 8) {
      auto Byte = C.mkIte(
          Test,
          C.mkIte(Inner, C.mkExtract(One, Low, 8),
                  C.mkExtract(C.mkLShr(X, C.mkConst(Width, Low)), 0, 8)),
          C.mkExtract(C.mkLShr(Y, C.mkConst(Width, Low)), 0, 8));
      Parts.push_back(C.mkShl(C.mkZExt(Byte, Width), C.mkConst(Width, Low)));
    }
    const auto Packed = C.mkOr(Parts);
    const auto Want = C.mkIte(Test, C.mkIte(Inner, One, X), Y);
    EXPECT_EQ(Packed, Want);
    std::reverse(Parts.begin(), Parts.end());
    EXPECT_EQ(C.mkOr(Parts), Want);
    // Nonzero literal bits that overlap a data lane still participate.
    Parts.push_back(C.mkConst(Width, 0x80));
    auto WithBits = C.mkOr(Parts);
    for (unsigned Flags = 0; Flags != 4; ++Flags) {
      std::vector<llvm::APInt> Values{llvm::APInt(1, Flags & 1),
                                      llvm::APInt(1, Flags >> 1),
                                      llvm::APInt::getSignMask(Width) | 0x53,
                                      llvm::APInt::getAllOnes(Width) ^ 0x97};
      EXPECT_EQ(C.eval(Packed, Values), C.eval(Want, Values));
      EXPECT_EQ(C.eval(WithBits, Values), C.eval(Want, Values) | 0x80);
    }
  }
}

TEST(SymExpr, WideningAQuotientSliceRetainsItsProvedZeroPadding) {
  for (unsigned Width : {8u, 32u, 64u, 128u}) {
    SymContext C;
    auto X = C.mkVar("x", Width);
    auto Q = C.mkUDiv(X, C.mkConst(Width, 25));
    const auto Maximum = llvm::APInt::getAllOnes(Width).udiv(25);
    const auto Bits = Maximum.getActiveBits();
    EXPECT_EQ(C.mkZExt(C.mkExtract(Q, 0, Bits), Width), Q);
    auto Truncated = C.mkZExt(C.mkExtract(X, 0, Bits), Width);
    EXPECT_NE(Truncated, X);
    const llvm::APInt Values[]{llvm::APInt(Width, 0),
                               llvm::APInt::getAllOnes(Width),
                               llvm::APInt::getSignMask(Width)};
    for (auto V : Values)
      EXPECT_EQ(C.eval(Truncated, {V}), V.trunc(Bits).zext(Width));
  }
}

TEST(SymExpr, TwoWordFunnelSlicesMatchOriginalBitOperations) {
  constexpr unsigned W = 6, Cut = 3, Bits = 3;
  SymContext C;
  const auto X = C.mkVar("x", W), Y = C.mkVar("y", W);
  const auto Count = llvm::APInt::getOneBitSet(73, 67) + 3;
  const auto TurnedX = C.mkRor(X, C.mkConst(Count));
  for (unsigned Form = 0; Form != 4; ++Form) {
    const auto High =
        C.mkShl(Form == 2 ? C.mkMul(TurnedX, Y) : TurnedX, C.mkConst(W, Cut));
    const unsigned Right = W - Cut - (Form == 1) + (Form == 3);
    const auto Low = C.mkLShr(Y, C.mkConst(W, Right));
    for (const auto Op : {SymOp::Or, SymOp::Xor, SymOp::Add}) {
      const auto Join = Op == SymOp::Or    ? C.mkOr(High, Low)
                        : Op == SymOp::Xor ? C.mkXor(High, Low)
                                           : C.mkAdd(High, Low);
      if (Form == 0) {
        EXPECT_EQ(C.mkExtract(Join, 0, Bits), C.mkExtract(Y, Right, Bits));
        EXPECT_EQ(
            C.mkExtract(Join, Cut, Bits),
            C.mkExtract(C.mkRor(X, C.mkConst(W, Count.urem(W))), 0, Bits));
      }
      if (Form == 1 || Form == 3) {
        // Reading the top two bits of an overlapping sum can observe a
        // carry. Bitwise joins and a gapped sum keep this upper slice.
        EXPECT_EQ(C.mkExtract(Join, 0, 2), C.mkExtract(Y, Right, 2));
        const auto Source = C.mkExtract(X, (1 + Count.urem(W)) % W, 2);
        const auto Top = C.mkExtract(Join, 4, 2);
        if (Form == 3 || Op != SymOp::Add)
          EXPECT_EQ(Top, Source);
        else
          EXPECT_NE(Top, Source);
      }
      for (unsigned Turn : {0u, 1u, W - 1}) {
        const auto Packed = C.mkRol(Join, C.mkConst(W, Turn));
        for (unsigned At = 0; At + Bits <= W; ++At) {
          SCOPED_TRACE(::testing::Message() << Form << ',' << unsigned(Op)
                                            << ',' << Turn << ',' << At);
          SymEvalPlan Field(C, C.mkExtract(Packed, At, Bits));
          for (unsigned A = 0; A < (1u << W); ++A) {
            const auto Rot = llvm::APInt(W, A).rotr(Count.urem(W));
            for (unsigned B = 0; B < (1u << W); ++B) {
              const llvm::APInt Other(W, B);
              const auto H = (Form == 2 ? Rot * Other : Rot).shl(Cut);
              const auto L = Other.lshr(Right);
              const auto Word = Op == SymOp::Or    ? H | L
                                : Op == SymOp::Xor ? H ^ L
                                                   : H + L;
              const std::array<llvm::APInt, 2> Inputs{llvm::APInt(W, A), Other};
              ASSERT_EQ(Field.eval(Inputs).getZExtValue(),
                        Word.rotl(Turn).extractBitsAsZExtValue(Bits, At));
            }
          }
        }
      }
    }
  }
}

TEST(SymExpr, SignedAndMaskedJoinedWindowsKeepEveryLiveBit) {
  constexpr unsigned Width = 6, Cut = 3;
  SymContext C;
  const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  for (bool Arithmetic : {false, true}) {
    for (unsigned Shift : {2u, 3u, 4u}) {
      const auto Low = Arithmetic ? C.mkAShr(X, C.mkConst(Width, Shift))
                                  : C.mkLShr(X, C.mkConst(Width, Shift));
      for (bool Masked : {false, true}) {
        // Both literals have three low zeros, with an additional set bit.
        const auto K = C.mkConst(Width, 0x28);
        const auto High = Masked ? C.mkAnd(K, Y) : C.mkMul(K, Y);
        for (const auto Op : {SymOp::Add, SymOp::Xor, SymOp::Or}) {
          const auto Joined = Op == SymOp::Add   ? C.mkAdd(High, Low)
                              : Op == SymOp::Xor ? C.mkXor(High, Low)
                                                 : C.mkOr(High, Low);
          const auto ExpectedLow = Shift + Cut <= Width
                                       ? C.mkExtract(X, Shift, Cut)
                                       : C.mkExtract(Low, 0, Cut);
          EXPECT_EQ(C.mkExtract(Joined, 0, Cut), ExpectedLow);
          for (unsigned At = 0; At + 3 <= Width; ++At) {
            SCOPED_TRACE(::testing::Message()
                         << Arithmetic << ',' << Shift << ',' << Masked << ','
                         << unsigned(Op) << ',' << At);
            SymEvalPlan Field(C, C.mkExtract(Joined, At, 3));
            for (unsigned A = 0; A < 64; ++A) {
              for (unsigned B = 0; B < 64; ++B) {
                const std::array<llvm::APInt, 2> Inputs{llvm::APInt(Width, A),
                                                        llvm::APInt(Width, B)};
                const auto H = Masked ? Inputs[1] & 0x28 : Inputs[1] * 0x28;
                const auto L =
                    Arithmetic ? Inputs[0].ashr(Shift) : Inputs[0].lshr(Shift);
                const auto Value = Op == SymOp::Add   ? H + L
                                   : Op == SymOp::Xor ? H ^ L
                                                      : H | L;
                ASSERT_EQ(Field.eval(Inputs), Value.extractBits(3, At));
              }
            }
          }
        }
      }
    }
  }
}

TEST(SymExpr, ExtractionAboveAZeroExtensionKeepsOnlyTheOtherWord) {
  SymContext C;
  const auto X = C.mkVar("x", 8), Y = C.mkVar("y", 3);
  const auto Extended = C.mkZExt(Y, 8);
  for (auto Op : {SymOp::Or, SymOp::Xor, SymOp::Add}) {
    const auto Joined = Op == SymOp::Or    ? C.mkOr(Extended, X)
                        : Op == SymOp::Xor ? C.mkXor(Extended, X)
                                           : C.mkAdd(Extended, X);
    if (Op != SymOp::Add)
      EXPECT_EQ(C.mkExtract(Joined, 3, 4), C.mkExtract(X, 3, 4));
    for (unsigned At : {2u, 3u}) {
      SymEvalPlan Plan(C, C.mkExtract(Joined, At, 4));
      for (uint64_t A = 0; A < 256; ++A)
        for (uint64_t B = 0; B < 8; ++B) {
          const auto Expected = Op == SymOp::Or    ? A | B
                                : Op == SymOp::Xor ? A ^ B
                                                   : A + B;
          ASSERT_EQ(Plan.evalU64({A, B}), (Expected >> At) & 15);
        }
    }
  }
}

TEST(SymExpr, JoinedPrefixRetainsCarriesAndSignPadding) {
  constexpr unsigned W = 6;
  SymContext C;
  const auto X = C.mkVar("x", W), Y = C.mkVar("y", W), Z = C.mkVar("z", W);
  for (auto Op : {SymOp::Add, SymOp::Or, SymOp::Xor}) {
    for (bool Signed : {false, true}) {
      const auto Count = C.mkConst(W, 2);
      const auto Moved = Signed ? C.mkAShr(X, Count) : C.mkLShr(X, Count);
      const auto Masked = C.mkAnd(C.mkConst(W, 0x38), Y);
      for (unsigned Shift : {0u, 2u, 3u}) {
        const auto Scaled = C.mkMul(C.mkConst(W, 1u << Shift), Z);
        const auto Packed = Op == SymOp::Add ? C.mkAdd({Moved, Masked, Scaled})
                            : Op == SymOp::Or
                                ? C.mkOr({Moved, Masked, Scaled})
                                : C.mkXor({Moved, Masked, Scaled});
        const auto Rotated = C.mkRor(Packed, C.mkConst(W, 4));
        const auto Field = C.mkExtract(Rotated, 3, 2);
        if (Shift == 3)
          EXPECT_EQ(Field, C.mkExtract(X, 3, 2));
        SymEvalPlan Plan(C, Field);
        SymEvalPlan Padded(C, C.mkExtract(Packed, 3, 2));
        for (unsigned A = 0; A < 64; ++A)
          for (unsigned B : {0u, 8u, 24u, 63u})
            for (unsigned D = 0; D < 64; ++D) {
              const unsigned Low = (A >> 2) | (Signed && (A & 32) ? 48 : 0);
              const unsigned H = B & 0x38, T = D << Shift;
              const unsigned Value = Op == SymOp::Add  ? Low + H + T
                                     : Op == SymOp::Or ? Low | H | T
                                                       : Low ^ H ^ T;
              const std::array<uint64_t, 3> Inputs{A, B, D};
              EXPECT_EQ(Plan.evalU64(Inputs), (Value >> 1) & 3);
              EXPECT_EQ(Padded.evalU64(Inputs), (Value >> 3) & 3);
            }
      }
    }
  }
}

TEST(SymExpr, InteriorMaskedWindowsKeepCarriesAndOverlap) {
  constexpr unsigned W = 193;
  SymContext C;
  const auto X = C.mkVar("x", W), Y = C.mkVar("y", W), Z = C.mkVar("z", W);
  const auto Shift = C.mkLShr(X, C.mkConst(W, 7));
  for (unsigned At : {17u, 63u, 64u, 71u}) {
    for (unsigned Size : {1u, 64u, 73u}) {
      SCOPED_TRACE(::testing::Message() << At << ',' << Size);
      auto Mask = ~llvm::APInt::getBitsSet(W, At, At + Size);
      const auto Good = C.mkAnd(C.mkConst(Mask), Y);
      const auto Noise = C.mkAnd(C.mkConst(W, 1), Z);
      const auto Expected = C.mkExtract(X, At + 7, Size);
      EXPECT_EQ(C.mkExtract(C.mkXor(Good, Shift), At, Size), Expected);
      EXPECT_EQ(C.mkExtract(C.mkOr({Noise, Good, Shift}), At, Size), Expected);

      // A carry from bit At-1 can change the first selected bit.
      const auto Below = llvm::APInt::getLowBitsSet(W, At);
      std::array<llvm::APInt, 3> Inputs{Below.shl(7), llvm::APInt(W, 1),
                                        llvm::APInt(W, 0)};
      auto Sum = C.mkExtract(C.mkAdd({Noise, Good, Shift}), At, Size);
      EXPECT_EQ(C.eval(Sum, Inputs), llvm::APInt(Size, 1));

      Mask.setBit(At + Size - 1);
      Inputs[0].clearAllBits();
      Inputs[1] = llvm::APInt::getAllOnes(W);
      const auto Live = C.mkAnd(C.mkConst(Mask), Y);
      EXPECT_EQ(C.eval(C.mkExtract(C.mkXor(Shift, Live), At, Size), Inputs),
                llvm::APInt::getSignMask(Size));
      EXPECT_EQ(
          C.eval(C.mkExtract(C.mkOr({Noise, Shift, Live}), At, Size), Inputs),
          llvm::APInt::getSignMask(Size));

      // An interior hole in a multiplier does not imply a hole in its product.
      const auto Scaled = C.mkMul(C.mkConst(Mask), Y);
      Inputs[1] = llvm::APInt::getOneBitSet(W, At);
      EXPECT_EQ(
          C.eval(C.mkExtract(C.mkOr({Scaled, Shift, Noise}), At, Size), Inputs),
          (Mask * Inputs[1]).extractBits(Size, At));
    }
  }
}

TEST(SymExpr, OverlappingFieldsReuseTheSameSliceAcrossDifferentMasks) {
  SymContext C;
  constexpr unsigned Width = 193, At = 67, Size = 65;
  const auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  auto L = llvm::APInt::getBitsSet(Width, At - 5, At + Size);
  auto R = llvm::APInt::getBitsSet(Width, At, At + Size);
  L.clearBit(At + 2);
  R.clearBit(At + 17);
  R.setBit(At - 5);
  const auto A = C.mkAnd(X, C.mkConst(L)), B = C.mkAnd(Y, C.mkConst(R));
  auto Field = [&](SymRef V) { return C.mkExtract(V, At, Size); };
  for (auto Op : {SymOp::Add, SymOp::Or, SymOp::Xor}) {
    auto Join = [&](SymRef Left, SymRef Right) {
      if (Op == SymOp::Add)
        return C.mkAdd(Left, Right);
      return Op == SymOp::Or ? C.mkOr(Left, Right) : C.mkXor(Left, Right);
    };
    const auto Expected = Field(Join(A, B));
    auto L2 = L, R2 = R;
    L2.setBit(At - 7);
    R2.setBit(At - 7);
    R2.setBit(At + Size + 1);
    const auto Alternate =
        Join(C.mkAnd(Y, C.mkConst(R2)), C.mkAnd(X, C.mkConst(L2)));
    EXPECT_EQ(Field(Alternate), Expected);
    EXPECT_EQ(
        C.mkExtract(C.mkRor(Alternate, C.mkConst(Width, 53)), At - 53, Size),
        Expected);
    const std::array<llvm::APInt, 2> Ones{llvm::APInt::getAllOnes(Width),
                                          llvm::APInt::getAllOnes(Width)};
    const auto Value =
        Op == SymOp::Add ? L + R : (Op == SymOp::Or ? L | R : L ^ R);
    EXPECT_EQ(C.eval(Expected, Ones), Value.extractBits(Size, At));
  }
}

TEST(SymExpr, SharedMaskedSumsKeepNestedFieldProjectionBounded) {
  constexpr unsigned Width = 193, Depth = 36;
  for (bool SameSource : {true, false}) {
    SymContext C;
    auto Prev = C.mkVar("x", Width), Sum = C.mkVar("y", Width);
    for (unsigned I = 0; I < Depth; ++I) {
      auto A = llvm::APInt::getBitsSet(Width, 64, 80);
      auto B = A;
      A.clearBit(65 + I % 7);
      B.clearBit(72 + I % 7);
      A.setBit(I % 41);
      B.setBit(120 + I % 30);
      const auto Next = C.mkAdd(C.mkAnd(Sum, C.mkConst(A)),
                                C.mkAnd(SameSource ? Sum : Prev, C.mkConst(B)));
      Prev = Sum;
      Sum = Next;
    }
    const auto Field = C.mkExtract(Sum, 64, 16);
    SymEvalPlan Original(C, Sum), Actual(C, Field);
    for (unsigned X = 1; X < 32; ++X) {
      const std::array<llvm::APInt, 2> Inputs{
          llvm::APInt(Width, X * 0x1349u).shl(64),
          llvm::APInt(Width, X * 0x7315u).shl(64)};
      EXPECT_EQ(Actual.eval(Inputs), Original.eval(Inputs).extractBits(16, 64));
    }
  }
}
