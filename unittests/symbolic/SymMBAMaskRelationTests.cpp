//===- SymMBAMaskRelationTests.cpp - Related literal mask inputs
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks exact full-word mask relations without assuming partial complements.

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "gtest/gtest.h"

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymMBA.h"

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

TEST(SymMBAMaskRelation, MaskAbstractionOwnsOperandsAcrossArenaGrowth) {
  SymContext Probe;
  auto ProbeX = Probe.mkVar("padding", 32);
  auto Marker = Probe.mkAdd(ProbeX, Probe.mkOne(32));
  auto Pool = reinterpret_cast<uintptr_t>(Probe.operands(Marker).data());
  unsigned Boundary = 0;
  for (unsigned I = 2; I < (1u << 20); ++I) {
    (void)Probe.mkAdd(ProbeX, Probe.mkConst(32, I));
    auto Current = reinterpret_cast<uintptr_t>(Probe.operands(Marker).data());
    if (Current != Pool && I > (1u << 16)) {
      Boundary = I;
      break;
    }
    Pool = Current;
  }
  ASSERT_GT(Boundary, 16u);
  unsigned Growth = 0;
  for (unsigned Parity : {0u, 1u})
    for (unsigned Padding = Boundary - 16; Padding < Boundary + 16; ++Padding) {
      SCOPED_TRACE(Padding);
      SymContext C;
      auto X = C.mkVar("lifetime_x", 32);
      auto Y = C.mkVar("lifetime_y", 32);
      for (unsigned I = 0; I < Padding; ++I)
        (void)C.mkAdd(X, C.mkConst(32, I + 1));
      if (Parity)
        (void)C.mkNot(Y);
      auto Mask = C.mkConst(32, 0x4c6fa0b1);
      auto Root = C.mkXor(C.mkAnd(X, Mask), C.mkOr(Y, C.mkNot(Mask)));
      auto Before = reinterpret_cast<uintptr_t>(C.operands(Root).data());
      auto Abstraction = detail::abstractToMBA(C, Root, false);
      ASSERT_TRUE(Abstraction);
      Growth += Before != reinterpret_cast<uintptr_t>(C.operands(Root).data());
      auto Restored = C.substitute(Abstraction->Body, Abstraction->Hidden);
      for (const auto &[A, B] :
           {std::pair{0u, 0u}, {UINT32_MAX, 1u}, {0xdeadbeefu, 0x11223344u}}) {
        std::vector<llvm::APInt> Inputs(C.numVars(), llvm::APInt(32, 0));
        Inputs[C.varId(X)] = llvm::APInt(32, A);
        Inputs[C.varId(Y)] = llvm::APInt(32, B);
        EXPECT_EQ(C.eval(Restored, Inputs), C.eval(Root, Inputs));
      }
    }
  EXPECT_GT(Growth, 0u);
}

TEST(SymMBAMaskRelation, ComplementedOffsetAddress) {
  for (unsigned Width : {8u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width);
    auto K = C.mkConst(Width, 0x093F959E2FCF0961);
    auto D = C.mkConst(Width, 0x4329F7B47E4D590C);
    for (unsigned Mask : {1u, 8u, 0x55u, 0x80u}) {
      SCOPED_TRACE(Mask);
      auto M = C.mkConst(Width, Mask);
      auto P = C.mkSub(C.mkXor(X, K), D);
      auto Q = C.mkAdd(C.mkXor(X, C.mkNot(K)), D);
      auto Root = C.mkAdd(C.mkOr(M, P), C.mkAnd(M, C.mkSub(M, C.mkAnd(M, Q))));
      auto Result = simplifyMBADeep(C, Root);
      EXPECT_EQ(Result.Expr, C.mkAdd(P, M)) << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAMaskRelation, OffsetMaskNeighborsAndBudgetsKeepExactValues) {
  for (unsigned Work : {0u, 1u, 16u, 64u, 256u, 65536u})
    for (unsigned Delta : {0u, 1u, 8u}) {
      SymContext C;
      auto X = C.mkVar("x", 8);
      auto P = C.mkSub(C.mkXor(X, C.mkConst(8, 0x31)), C.mkConst(8, 0x6c));
      auto Q =
          C.mkAdd(C.mkXor(X, C.mkConst(8, 0xce)), C.mkConst(8, 0x6c + Delta));
      auto Root = C.mkAdd(
          C.mkOr(P, C.mkConst(8, 0x55)),
          C.mkAnd(C.mkConst(8, 0x55),
                  C.mkSub(C.mkConst(8, 0x55), C.mkAnd(Q, C.mkConst(8, 0x55)))));
      MBAOptions Options;
      Options.MaxWork = Work;
      auto Result = simplifyMBADeep(C, Root, Options);
      EXPECT_LE(Result.Work, Work);
      for (unsigned Input = 0; Input < 256; ++Input) {
        const unsigned A = (Input ^ 0x31) - 0x6c;
        const unsigned B = (Input ^ 0xce) + 0x6c + Delta;
        const unsigned Expected =
            ((A | 0x55) + (0x55 & (0x55 - (B & 0x55)))) & 255;
        EXPECT_EQ(C.evalU64(Result.Expr, {Input}), Expected);
      }
    }
}

TEST(SymMBAMaskRelation, MaskedSumsFinishTheirRetainedWords) {
  for (unsigned Width : {8u, 32u, 64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width),
         Z = C.mkVar("z", Width);
    auto Sum =
        C.mkAdd(C.mkXor(X, Y), C.mkMul(C.mkConst(Width, 2), C.mkAnd(X, Y)));
    auto M = C.mkConst(Width, 0x5a);
    // The root identity keeps W verbatim; W itself is an obfuscated x+y.
    auto Masked = simplifyMBADeep(C, C.mkAdd(C.mkOr(M, Sum), C.mkAnd(M, Sum)));
    EXPECT_EQ(Masked.Expr, C.mkAdd({M, X, Y})) << C.toString(Masked.Expr);
    EXPECT_EQ(Masked.Evidence, MBAEvidence::Derivation);
    auto Shared =
        simplifyMBADeep(C, C.mkAdd(C.mkAnd(Sum, Z), C.mkAnd(Sum, C.mkNot(Z))));
    EXPECT_EQ(Shared.Expr, C.mkAdd(X, Y)) << C.toString(Shared.Expr);
    EXPECT_EQ(Shared.Evidence, MBAEvidence::Derivation);
  }
  for (unsigned Work : {0u, 1u, 16u, 64u, 256u, 65536u}) {
    SCOPED_TRACE(Work);
    SymContext C;
    auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
    auto Sum = C.mkAdd(C.mkXor(X, Y), C.mkMul(C.mkConst(8, 2), C.mkAnd(X, Y)));
    auto M = C.mkConst(8, 0x5a);
    MBAOptions Options;
    Options.MaxWork = Work;
    auto Result =
        simplifyMBADeep(C, C.mkAdd(C.mkOr(M, Sum), C.mkAnd(M, Sum)), Options);
    EXPECT_LE(Result.Work, Work);
    for (unsigned A = 0; A != 256; ++A)
      for (unsigned B = 0; B != 256; ++B)
        ASSERT_EQ(C.evalU64(Result.Expr, {A, B}), (0x5a + A + B) & 255);
  }
}

namespace {

TEST(SymMBAMaskRelation, SubtractsAnEntireKnownOrMaskWithoutBorrow) {
  std::mt19937_64 Random(0x6f72737562747261);
  for (unsigned Width : {1u, 7u, 8u, 16u, 32u, 64u, 65u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    for (unsigned Sample = 0; Sample != 12; ++Sample) {
      SymContext C;
      auto X = C.mkVar("x", Width);
      llvm::APInt Mask(Width, 0);
      for (unsigned Bit = 0; Bit < Width; Bit += 64)
        Mask |= llvm::APInt(Width, Random()).shl(Bit);
      if (Sample == 0)
        Mask.clearAllBits();
      if (Sample == 1)
        Mask.setAllBits();
      if (Sample == 2)
        Mask = llvm::APInt(Width, 0x28f0);
      auto Literal = C.mkConst(Mask);
      auto Difference = C.mkSub(C.mkOr(Literal, X), Literal);
      auto Input = C.mkAdd(Literal, C.mkOr(X, Difference));
      auto Expected = C.mkAdd(Literal, X);
      MBAOptions Options;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_LE(C.readabilityCost(Result.Expr), C.readabilityCost(Expected))
          << C.toString(Result.Expr);
      if (Sample == 2 && Width >= 16)
        EXPECT_EQ(Result.Expr, Expected) << C.toString(Result.Expr);
      for (unsigned Value = 0; Value != 16; ++Value) {
        llvm::APInt Word(Width, 0);
        for (unsigned Bit = 0; Bit < Width; Bit += 64)
          Word |= llvm::APInt(Width, Random()).shl(Bit);
        const llvm::APInt Values[] = {Word};
        EXPECT_EQ(C.eval(Result.Expr, Values), Mask + Word);
      }
    }
  }
}

TEST(SymMBAMaskRelation, OrMaskSubtractionChecksEveryByteAndNearbyLiteral) {
  for (unsigned Mask = 0; Mask != 256; ++Mask) {
    SCOPED_TRACE(Mask);
    for (unsigned Delta : {0u, 1u}) {
      SymContext C;
      auto X = C.mkVar("x", 8);
      auto Literal = C.mkConst(8, Mask);
      auto Input = C.mkSub(C.mkOr(X, Literal), C.mkConst(8, Mask + Delta));
      auto Result = simplifyMBADeep(C, Input);
      bool Differs = false;
      for (unsigned Word = 0; Word != 256; ++Word) {
        const uint64_t Values[] = {Word};
        auto Oracle = uint8_t((Word | Mask) - (Mask + Delta));
        ASSERT_EQ(C.evalU64(Result.Expr, Values), Oracle);
        Differs |= Oracle != uint8_t(Word & ~Mask);
      }
      EXPECT_EQ(Differs, Delta != 0);
      if (Delta == 0)
        EXPECT_EQ(Result.Expr, C.mkAnd(X, C.mkConst(8, ~Mask)));
    }
  }
}

TEST(SymMBAMaskRelation, OrMaskSubtractionRetainsOtherWordsAndWorkLimits) {
  for (size_t Work :
       {size_t(0), size_t(1), size_t(16), size_t(256), size_t(65536)}) {
    SCOPED_TRACE(Work);
    SymContext C;
    auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
    auto Input =
        C.mkSub(C.mkOr({C.mkConst(8, 0x39), X, Y}), C.mkConst(8, 0x39));
    MBAOptions Options;
    Options.MaxWork = Work;
    auto Result = simplifyMBADeep(C, Input, Options);
    EXPECT_LE(Result.Work, Work);
    for (unsigned A = 0; A != 256; ++A)
      for (unsigned B = 0; B != 256; ++B) {
        const uint64_t Values[] = {A, B};
        ASSERT_EQ(C.evalU64(Result.Expr, Values),
                  uint8_t((A | B | 0x39) - 0x39));
      }
  }
}

std::pair<SymRef, SymRef> maskedComplement(SymContext &C, unsigned Width,
                                           bool NearMiss = false,
                                           bool SecondMask = false) {
  auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
  if (SecondMask)
    Y = C.mkXor(Y, C.mkConst(llvm::APInt(Width, 0x6d)));
  llvm::APInt Mask(Width, 0x39);
  auto A = C.mkXor(X, C.mkConst(Mask));
  auto B = C.mkXor(X, C.mkConst(~Mask ^ llvm::APInt(Width, NearMiss)));
  auto K = C.mkConst(llvm::APInt(Width, 37));
  auto Tail = C.mkConst(llvm::APInt(Width, 0x55));
  auto Input = C.mkOr(C.mkAdd(C.mkMul(K, C.mkAdd(C.mkOr(A, Y), B)), K), Tail);
  auto Expected = C.mkOr(C.mkMul(K, C.mkAnd(C.mkNot(A), Y)), Tail);
  return {Input, Expected};
}

TEST(SymMBAMaskRelation, RecoversComplementedMasksAtTheCompleteWidth) {
  for (bool SecondMask : {false, true})
    for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 257u}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(SecondMask);
      SymContext C;
      auto [Input, Expected] = maskedComplement(C, Width, false, SecondMask);
      MBAOptions Options;
      Options.MaxWork = 1 << 16;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_TRUE(Result.Changed) << C.toString(Result.Expr);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
      EXPECT_LE(C.readabilityCost(Result.Expr), C.readabilityCost(Expected));
      solver::SolverOptions ProofOptions;
      // The production verifier defaults to a 256-bit blasting limit. This
      // test deliberately exercises a wider word with the same finite SAT
      // and gate budgets; an unsupported-width refusal is not a proof failure.
      ProofOptions.Blast.MaxWidth = Width;
      solver::SymSynthVerifier Verify(ProofOptions);
      EXPECT_EQ(Verify(C, Expected, Result.Expr),
                SynthVerification::Equivalent);
      if (Width <= 32)
        EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);

      // Exercise every word, including the partial final word at width 257,
      // against independent APInt arithmetic rather than a truncated u64
      // oracle.
      std::mt19937_64 Random(0x3917);
      for (unsigned Sample = 0; Sample != 128; ++Sample) {
        llvm::SmallVector<llvm::APInt, 8> Values(C.numVars(),
                                                 llvm::APInt(Width, 0));
        for (unsigned V = 0; V != 2; ++V)
          for (unsigned Bit = 0; Bit < Width; Bit += 64)
            Values[V] |= llvm::APInt(Width, Random()).shl(Bit);
        const auto X = Values[0] ^ llvm::APInt(Width, 0x39);
        const auto Y = Values[1] ^ llvm::APInt(Width, SecondMask ? 0x6d : 0);
        const auto Oracle =
            (llvm::APInt(Width, 37) * ((X | Y) + ~X) + llvm::APInt(Width, 37)) |
            llvm::APInt(Width, 0x55);
        EXPECT_EQ(C.eval(Result.Expr, Values), Oracle);
      }
    }
}

TEST(SymMBAMaskRelation, ExhaustiveBytePairsIncludeNearbyNonidentities) {
  for (bool SecondMask : {false, true})
    for (bool NearMiss : {false, true}) {
      SymContext C;
      auto [Input, Expected] = maskedComplement(C, 8, NearMiss, SecondMask);
      auto Result = simplifyMBADeep(C, Input);
      bool DiffersFromComplement = false;
      for (unsigned X = 0; X != 256; ++X)
        for (unsigned Y = 0; Y != 256; ++Y) {
          llvm::SmallVector<uint64_t, 8> Values(C.numVars(), 0);
          Values[0] = X;
          Values[1] = Y;
          const auto Oracle =
              uint8_t((37u * ((X ^ 0x39u) | (Y ^ (SecondMask ? 0x6du : 0u))) +
                       37u * (X ^ (0xc6u ^ unsigned(NearMiss))) + 37u) |
                      0x55u);
          ASSERT_EQ(C.evalU64(Result.Expr, Values), Oracle);
          DiffersFromComplement |= C.evalU64(Expected, Values) != Oracle;
        }
      EXPECT_EQ(DiffersFromComplement, NearMiss);
    }
}

TEST(SymMBAMaskRelation, PreservesSemanticsWithIncompleteRelationBudgets) {
  for (bool SecondMask : {false, true})
    for (size_t Limit : {size_t(0), size_t(1), size_t(32), size_t(256)}) {
      SymContext C;
      auto [Input, Expected] = maskedComplement(C, 32, false, SecondMask);
      MBAOptions Options;
      Options.MaxWork = Limit;
      auto Result = simplifyMBADeep(C, Input, Options);
      EXPECT_LE(Result.Work, Limit);
      solver::SymSynthVerifier Verify;
      EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
    }
}

TEST(SymMBAMaskRelation, IndependentZ3ChecksMaskedArithmeticBeforeScaling) {
  if (!solver::proofBackendAvailable(solver::ProofBackend::Z3))
    GTEST_SKIP() << "Z3 is unavailable";
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width), Y = C.mkVar("y", Width);
    const llvm::APInt Mask(Width, 0x39);
    auto A = C.mkXor(X, C.mkConst(Mask));
    auto B = C.mkXor(Y, C.mkConst(llvm::APInt(Width, 0x6d)));
    auto Input =
        C.mkAdd({C.mkOr(A, B), C.mkXor(X, C.mkConst(~Mask)), C.mkOne(Width)});
    auto Result = simplifyMBADeep(C, Input);
    ASSERT_TRUE(Result.Changed);
    // Prove the complete-width identity before the outer scaling and mask.
    // Congruence preserves it under those operations; the separate wrapped
    // tests exercise composition, high bits and nearby nonidentities.
    solver::SymSynthVerifier Verify({}, solver::ProofBackend::Z3, 5000);
    EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
  }
}

TEST(SymMBAMaskRelation, SmallStorageAllowancesNeverCreateFalseRelations) {
  for (size_t Bytes : {size_t(0), size_t(64), size_t(256)}) {
    SymContext C;
    auto [Input, Expected] = maskedComplement(C, 8, true, true);
    MBAOptions Options;
    Options.MaxTableBytes = Bytes;
    auto Result = simplifyMBADeep(C, Input, Options);
    solver::SymSynthVerifier Verify;
    EXPECT_EQ(Verify(C, Input, Result.Expr), SynthVerification::Equivalent);
  }
}

} // namespace
