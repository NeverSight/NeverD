//===- SymExprDisjointFieldTests.cpp - Disjoint field observations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks independent packed fields, unknown/overlapping arms and work bounds.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

using namespace neverd::symbolic;

namespace {
TEST(SymExpr, DisjointFieldsKeepTheirOwnEqualityPredicates) {
  for (unsigned Width : {8u, 32u, 64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    const auto Selector = C.mkVar("selector", 8);
    const auto Low = C.mkVar("low", 1);
    const uint64_t Tags[] = {0x82, 0x94, 0xa6, 0xb8, 0xca};
    auto Selected = C.mkConst(Width, Tags[4]);
    for (unsigned I = 4; I-- > 0;)
      Selected = C.mkIte(C.mkEq(Selector, C.mkConst(8, I)),
                         C.mkConst(Width, Tags[I]), Selected);
    const auto Word = C.mkOr(C.mkZExt(Low, Width), Selected);
    const auto Equal = C.mkEq(Word, C.mkConst(Width, Tags[0]));
    EXPECT_EQ(Equal, C.mkAnd(C.mkNot(Low),
                             C.mkEq(Selected, C.mkConst(Width, Tags[0]))));
    EXPECT_EQ(Equal, C.mkEq(C.mkConst(Width, Tags[0]), Word));
    for (unsigned Target = 0; Target < 5; ++Target) {
      const auto E = C.mkEq(Word, C.mkConst(Width, Tags[Target] | 1));
      for (unsigned S = 0; S < 256; ++S)
        for (unsigned L = 0; L < 2; ++L)
          EXPECT_EQ(C.eval(E, {llvm::APInt(8, S), llvm::APInt(1, L)}).isOne(),
                    (Tags[S < 4 ? S : 4] | L) == (Tags[Target] | 1));
    }
  }
}

TEST(SymExpr, DisjointFieldsObserveHighBitsAndRejectOverlappingLanes) {
  SymContext C;
  const auto A = C.mkVar("a", 8), B = C.mkVar("b", 8);
  const auto Low = C.mkZExt(C.mkAnd(A, C.mkConst(8, 3)), 64);
  const auto High = C.mkMul(C.mkZExt(B, 64), C.mkConst(64, 256));
  const auto Word = C.mkOr(Low, High);
  const auto Expected =
      C.mkAnd(C.mkEq(C.mkAnd(A, C.mkConst(8, 3)), C.mkConst(8, 2)),
              C.mkEq(High, C.mkConst(64, 0x700)));
  EXPECT_EQ(C.mkEq(Word, C.mkConst(64, 0x702)), Expected);
  EXPECT_EQ(C.mkEq(Word, C.mkConst(64, 0x10702)), C.mkFalse());
  const auto Separated = C.mkEq(Word, C.mkConst(64, 0x702));
  for (unsigned Av = 0; Av < 256; ++Av)
    for (unsigned Bv = 0; Bv < 256; ++Bv)
      EXPECT_EQ(
          C.eval(Separated, {llvm::APInt(8, Av), llvm::APInt(8, Bv)}).isOne(),
          ((Av & 3) | (Bv << 8)) == 0x702);
  const auto Overlap = C.mkOr(A, C.mkAnd(B, C.mkConst(8, 3)));
  const auto E = C.mkEq(Overlap, C.mkConst(8, 2));
  for (unsigned Av = 0; Av < 256; ++Av)
    for (unsigned Bv = 0; Bv < 16; ++Bv)
      EXPECT_EQ(C.eval(E, {llvm::APInt(8, Av), llvm::APInt(8, Bv)}).isOne(),
                (Av | (Bv & 3)) == 2);
}

TEST(SymExpr, DisjointFieldsKeepUnknownArmsAndBoundConstructorWork) {
  SymContext C;
  const auto Low = C.mkVar("low", 1), Select = C.mkVar("select", 1);
  const auto Unknown = C.mkVar("unknown", 8);
  const auto Word =
      C.mkOr(C.mkZExt(Low, 8), C.mkIte(Select, C.mkConst(8, 0x82), Unknown));
  const auto Literal = C.mkConst(8, 0x82);
  const auto Before = C.numNodes();
  const auto E = C.mkEq(Word, Literal);
  EXPECT_EQ(C.numNodes() - Before, 1u);
  for (unsigned L = 0; L < 2; ++L)
    for (unsigned S = 0; S < 2; ++S)
      for (unsigned U = 0; U < 256; ++U)
        EXPECT_EQ(
            C.eval(E, {llvm::APInt(1, L), llvm::APInt(1, S), llvm::APInt(8, U)})
                .isOne(),
            (L | (S ? 0x82 : U)) == 0x82);

  for (unsigned Width : {128u, 256u}) {
    llvm::SmallVector<SymRef, 16> Fields;
    for (unsigned I = 0; I <= neverd::limits::symbolic::kSymExprCompareMaxTerms;
         ++I) {
      const auto V = C.mkFreshVar(8, "byte_");
      Fields.push_back(C.mkMul(C.mkZExt(V, Width),
                               C.mkConst(llvm::APInt(Width, 1).shl(I * 8))));
    }
    const auto Packed = C.mkOr(Fields), Zero = C.mkZero(Width);
    const auto Nodes = C.numNodes();
    EXPECT_EQ(C.op(C.mkEq(Packed, Zero)), SymOp::Eq);
    EXPECT_EQ(C.numNodes() - Nodes, 1u);
  }
}

} // namespace
