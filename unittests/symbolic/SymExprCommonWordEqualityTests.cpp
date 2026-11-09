//===- SymExprCommonWordEqualityTests.cpp - Exact constructor regressions
//-----===//
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

TEST(SymExpr, EqualityCancelsCommonXorInputsBeforeReadingPackedFields) {
  for (unsigned Width : {1u, 8u, 32u, 64u, 128u, 256u}) {
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    auto A = C.mkVar("a", Width), B = C.mkVar("b", Width);
    auto Packed = C.mkOr(X, C.mkAnd(Y, C.mkConst(Width, 0x73)));
    EXPECT_EQ(C.mkEq(Packed, C.mkXor(Packed, A)), C.mkEq(A, C.mkZero(Width)));
    EXPECT_EQ(C.mkEq(C.mkXor({Packed, A, Y}), C.mkXor({B, Y, Packed})),
              C.mkEq(A, B));
    for (uint64_t V : {0u, 1u, 0x53u, 0xffu}) {
      const std::vector<llvm::APInt> Values{
          llvm::APInt(Width, V, false, true),
          llvm::APInt(Width, 0x15, false, true),
          llvm::APInt(Width, 0x23, false, true),
          llvm::APInt(Width, V, false, true)};
      EXPECT_EQ(C.eval(C.mkEq(C.mkXor({Packed, A, Y}), C.mkXor({B, Y, Packed})),
                       Values),
                llvm::APInt(1, Values[2] == Values[3]));
    }
  }
}

TEST(SymExpr, CommonAdditiveEqualityPreservesModularCarries) {
  for (unsigned Width : {4u, 8u, 32u, 64u, 128u, 256u}) {
    SymContext C;
    const auto Shared = C.mkVar("shared", Width), A = C.mkVar("a", Width);
    const auto B = C.mkVar("b", Width);
    const auto Left = C.mkAdd(Shared, A), Right = C.mkAdd(Shared, B);
    EXPECT_EQ(C.mkEq(Left, Right), C.mkEq(A, B));
    const auto Carry = llvm::APInt::getSignMask(Width) | 3;
    for (unsigned Av = 0; Av < 16; ++Av)
      for (unsigned Bv = 0; Bv < 16; ++Bv) {
        const llvm::APInt Inputs[]{Carry, llvm::APInt(Width, Av),
                                   llvm::APInt(Width, Bv)};
        EXPECT_EQ(C.eval(C.mkEq(Left, Right), Inputs).isOne(), Av == Bv);
      }
  }
  SymContext C;
  const auto A = C.mkVar("a", 4), B = C.mkVar("b", 4);
  const auto Equal =
      C.mkEq(C.mkAdd(A, C.mkConst(4, 1)), C.mkAdd(B, C.mkConst(4, 2)));
  for (unsigned Av = 0; Av < 16; ++Av)
    for (unsigned Bv = 0; Bv < 16; ++Bv)
      EXPECT_EQ(C.eval(Equal, {llvm::APInt(4, Av), llvm::APInt(4, Bv)}).isOne(),
                ((Av + 1) & 15) == ((Bv + 2) & 15));
}
