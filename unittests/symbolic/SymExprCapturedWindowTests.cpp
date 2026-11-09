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
