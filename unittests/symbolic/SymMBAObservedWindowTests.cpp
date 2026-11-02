//===- SymMBAObservedWindowTests.cpp - Symbolic regressions
//----------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymMBA.h"

#include <algorithm>
#include <set>

using namespace neverd::symbolic;

static SymRef simplifyWindow(SymContext &C, SymRef Root,
                             MBAOptions Options = {}) {
  return simplifyMBADeep(C, Root, Options).Expr;
}

TEST(SymMBAObservedWindow, ExhaustedRewriteWorkKeepsCompletedChildReductions) {
  SymContext C;
  auto High = C.mkVar("unobserved_high", 46);
  auto Flag = C.mkVar("flag", 1);
  auto Low = C.mkVar("unobserved_low", 17);
  auto Packed = C.mkConcat({High, Flag, Low});
  auto Read = C.mkExtract(C.mkLShr(Packed, C.mkConst(64, 17)), 0, 1);
  auto Tail = C.mkVar("tail", 64), Divisor = C.mkVar("divisor", 64);
  for (unsigned I = 0; I < 1000; ++I)
    Tail = C.mkUDiv(Tail, Divisor);
  auto Root = C.mkXor(C.mkZExt(Read, 64), Tail);
  MBAOptions Options;
  Options.MaxWork = 100;
  EXPECT_EQ(simplifyWindow(C, Root, Options),
            C.mkXor(C.mkZExt(Flag, 64), Tail));
}

TEST(SymMBAObservedWindow, MaskedFlagReadDropsOtherPackedLanes) {
  SymContext C;
  std::vector<SymRef> Lanes;
  for (unsigned Bit = 0; Bit < 16; ++Bit)
    Lanes.push_back(
        C.mkShl(C.mkZExt(C.mkFreshVar(1, "flag_"), 64), C.mkConst(64, Bit)));
  auto Word = C.mkOr(Lanes);
  auto Read = C.mkAnd(C.mkLShr(Word, C.mkConst(64, 6)), C.mkConst(64, 1));
  auto Expected = C.mkZExt(C.mkExtract(Lanes[6], 6, 1), 64);
  EXPECT_EQ(simplifyWindow(C, Read), simplifyWindow(C, Expected));
  neverd::solver::BitVectorSolver Proof(C);
  EXPECT_EQ(Proof.check(C.mkNe(Read, simplifyWindow(C, Read))),
            neverd::solver::SatResult::Unsat);
}

TEST(SymMBAObservedWindow, SelectedPackedFlagDropsUnobservedInputs) {
  for (unsigned Width : {32U, 64U, 128U, 256U}) {
    SymContext C;
    auto High = C.mkVar("unobserved_high", Width - 18);
    auto Flag = C.mkVar("selected_flag", 1);
    auto Low = C.mkVar("unobserved_low", 17);
    auto Word = C.mkConcat({High, Flag, Low});
    auto Read = C.mkExtract(C.mkLShr(Word, C.mkConst(64, 17)), 0, 1);
    EXPECT_EQ(simplifyWindow(C, Read), Flag) << Width;
  }
}

TEST(SymMBAObservedWindow, UnshiftedSingleBitMaskDropsOtherPackedFlags) {
  for (unsigned Width : {32U, 64U, 128U, 256U}) {
    SymContext C;
    llvm::SmallVector<SymRef> Lanes, Flags;
    for (unsigned Bit = 0; Bit < 16; ++Bit) {
      Flags.push_back(C.mkFreshVar(1, "flag_"));
      Lanes.push_back(C.mkMul(C.mkConst(llvm::APInt::getOneBitSet(Width, Bit)),
                              C.mkZExt(Flags.back(), Width)));
    }
    auto Word = C.mkOr(Lanes);
    auto Read = C.mkAnd(Word, C.mkConst(Width, 64));
    auto Expected = C.mkMul(C.mkConst(Width, 64), C.mkZExt(Flags[6], Width));
    EXPECT_EQ(simplifyWindow(C, Read), Expected) << Width;
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Read, simplifyWindow(C, Read))),
              neverd::solver::SatResult::Unsat);
  }
}

TEST(SymMBAObservedWindow, FinishesFlagWindowsExposedByArithmeticMeasurement) {
  for (unsigned Width : {32u, 64u, 128u, 256u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("reason", Width);
    auto Selected = C.mkEq(X, C.mkOne(Width));
    llvm::SmallVector<SymRef, 8> Lanes;
    for (unsigned Bit : {0u, 2u, 4u, 6u, 7u, 11u}) {
      auto Flag = Bit == 6 ? Selected : C.mkFreshVar(1, "other_flag_");
      Lanes.push_back(C.mkMul(C.mkConst(llvm::APInt::getOneBitSet(Width, Bit)),
                              C.mkZExt(Flag, Width)));
    }
    auto Word = C.mkOr(Lanes);
    auto Root = C.mkEq(C.mkSub(C.mkOr(Word, C.mkConst(Width, 64)), Word),
                       C.mkZero(Width));
    auto Result = simplifyMBADeep(C, Root);
    EXPECT_EQ(Result.Expr, Selected) << C.toString(Result.Expr);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
              neverd::solver::SatResult::Unsat);
    auto Independent = C.mkFreshVar(1, "independent_");
    auto Combined = C.mkAnd(Root, Independent);
    auto CombinedResult = simplifyMBADeep(C, Combined);
    EXPECT_EQ(CombinedResult.Expr, C.mkAnd(Selected, Independent))
        << C.toString(CombinedResult.Expr);
    EXPECT_EQ(Proof.check(C.mkNe(Combined, CombinedResult.Expr)),
              neverd::solver::SatResult::Unsat);
    auto ComplementMask = C.mkConst(~llvm::APInt(Width, 64));
    auto Complemented =
        C.mkEq(C.mkNot(C.mkOr(ComplementMask, C.mkNot(Word))), C.mkZero(Width));
    MBAOptions Options;
    Options.MaxWork = 65536;
    auto ComplementedResult = simplifyMBADeep(C, Complemented, Options);
    EXPECT_EQ(ComplementedResult.Expr, C.mkNot(Selected))
        << C.toString(ComplementedResult.Expr);
    EXPECT_LE(ComplementedResult.Work, Options.MaxWork);
  }
}

TEST(SymMBAObservedWindow, UnreducedWindowsKeepTheirComplementedWord) {
  for (unsigned Width : {8u, 16u, 64u, 128u, 257u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto Y = C.mkVar("y", Width);
    auto K = [&](int64_t Value) {
      return C.mkConst(llvm::APInt(Width, Value, /*isSigned=*/true));
    };
    auto Low = C.mkConst(~llvm::APInt::getOneBitSet(Width, 7));
    // A window of ~(K|y) drops no lane. Keep the complement, which the
    // enclosing XOR folds into its literal instead of an extract per input.
    auto Result = simplifyMBADeep(
        C, C.mkXor(C.mkConst(Width, 8), C.mkNot(C.mkOr(Low, Y))));
    EXPECT_EQ(Result.Expr,
              C.mkXor(C.mkConst(~llvm::APInt(Width, 8)), C.mkOr(Low, Y)))
        << C.toString(Result.Expr);
    // A longer root window must not discard the shortened word beneath it.
    auto Word = C.mkSub(K(-2), C.mkAdd(K(-91), C.mkXor(K(50), Y)));
    EXPECT_EQ(simplifyWindow(C, C.mkNot(C.mkOr(K(-5), Word))),
              C.mkNot(C.mkOr(K(-5), C.mkAdd(K(90), C.mkXor(K(-51), Y)))));
  }
  SymContext C;
  auto Y = C.mkVar("y", 8);
  auto Root = C.mkXor(C.mkConst(8, 8), C.mkNot(C.mkOr(C.mkConst(8, 0x7f), Y)));
  for (unsigned Work : {0u, 1u, 8u, 64u, 512u, 65536u}) {
    MBAOptions Options;
    Options.MaxWork = Work;
    auto Result = simplifyMBADeep(C, Root, Options);
    EXPECT_LE(Result.Work, Work);
    for (unsigned Input = 0; Input != 256; ++Input)
      EXPECT_EQ(C.evalU64(Result.Expr, {Input}), (8 ^ ~(0x7fu | Input)) & 255);
  }
}

TEST(SymMBAObservedWindow, FinishedReadingsDoNotReplaceShorterWords) {
  for (unsigned Width : {8u, 16u, 64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto X = C.mkVar("x", Width);
    auto P = C.mkVar("p", 1);
    auto K = [&](uint64_t Value) { return C.mkConst(Width, Value); };
    auto Flag = C.mkMul(K(32), C.mkZExt(P, Width));
    // The comparison is finished locally after the walk. Its re-projected
    // masked terms must not replace the shortened words beneath it.
    auto Root =
        C.mkSlt(Flag, C.mkAdd(C.mkOr(K(64), C.mkAdd(K(98), C.mkNot(X))),
                              C.mkAnd(K(64), C.mkAdd(K(3), C.mkNot(X)))));
    auto Result = simplifyMBADeep(C, Root);
    EXPECT_EQ(Result.Expr,
              C.mkSlt(Flag, C.mkAdd(C.mkOr(K(64), C.mkSub(K(97), X)),
                                    C.mkAnd(K(64), C.mkSub(K(2), X)))))
        << C.toString(Result.Expr);
    EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
  }
}

TEST(SymMBAObservedWindow, LongerWindowsLeaveMasksToKnownBits) {
  // Widths within kSymKnownBitsMaxWidth, where the exact fact fold applies.
  for (unsigned Width : {16u, 64u, 128u}) {
    SCOPED_TRACE(Width);
    SymContext C;
    auto P = C.mkVar("p", 1), Q = C.mkVar("q", 1);
    // At most five: bit 13 is a known zero, not a lane to project.
    auto Small = C.mkAdd(C.mkMul(C.mkConst(Width, 4), C.mkZExt(Q, Width)),
                         C.mkZExt(P, Width));
    EXPECT_EQ(simplifyWindow(C, C.mkAnd(C.mkConst(Width, 8192), Small)),
              C.mkZero(Width));
    // (K | s) - K is s & ~K: the known-OR subtraction leaves the same mask.
    auto K = C.mkConst(~llvm::APInt(Width, 8192));
    EXPECT_EQ(simplifyWindow(C, C.mkSub(C.mkOr(K, Small), K)), C.mkZero(Width));
  }
}

TEST(SymMBAObservedWindow, SingleBitObservationKeepsArithmeticCarry) {
  SymContext C;
  auto X = C.mkVar("x", 8), Y = C.mkVar("y", 8);
  auto Read = C.mkAnd(C.mkAdd(X, Y), C.mkConst(8, 64));
  auto Reduced = simplifyWindow(C, Read);
  for (unsigned A = 0; A < 256; ++A)
    for (unsigned B = 0; B < 256; ++B)
      EXPECT_EQ(C.eval(Reduced, {llvm::APInt(8, A), llvm::APInt(8, B)}),
                llvm::APInt(8, (A + B) & 64));
}

TEST(SymMBAObservedWindow, LowObservedBitsIgnoreAnUnrelatedHighAddend) {
  for (unsigned Width : {32U, 64U, 128U, 256U}) {
    SymContext C;
    auto High = C.mkVar("high_flag_bits", Width);
    auto Mask = ~llvm::APInt::getLowBitsSet(Width, 16);
    auto Word =
        C.mkAdd(C.mkAnd(High, C.mkConst(Mask)), C.mkConst(Width, 0x297));
    EXPECT_EQ(simplifyWindow(C, C.mkExtract(Word, 6, 1)), C.mkZero(1));
    EXPECT_EQ(simplifyWindow(C, C.mkExtract(Word, 0, 8)), C.mkConst(8, 0x97));
    auto HighRead = C.mkExtract(Word, 16, 1);
    EXPECT_FALSE(C.isConst(simplifyWindow(C, HighRead)));
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(HighRead, simplifyWindow(C, HighRead))),
              neverd::solver::SatResult::Unsat);
  }
}

TEST(SymMBAObservedWindow, ExtensionWindowsKeepOnlyTheirOwnedBits) {
  SymContext C;
  auto X = C.mkVar("byte", 8);
  for (bool Signed : {false, true}) {
    auto Extended = Signed ? C.mkSExt(X, 64) : C.mkZExt(X, 64);
    EXPECT_EQ(simplifyWindow(C, C.mkExtract(Extended, 4, 4)),
              C.mkExtract(X, 4, 4));
    EXPECT_EQ(simplifyWindow(C, C.mkExtract(Extended, 16, 8)),
              Signed ? C.mkSExt(C.mkExtract(X, 7, 1), 8) : C.mkZero(8));
    auto Crossing = simplifyWindow(C, C.mkExtract(Extended, 4, 8));
    EXPECT_EQ(Crossing, Signed ? C.mkSExt(C.mkExtract(X, 4, 4), 8)
                               : C.mkZExt(C.mkExtract(X, 4, 4), 8));
  }
}

TEST(SymMBAObservedWindow, ShiftWindowsMatchAnExhaustiveIndependentOracle) {
  for (unsigned SourceWidth = 2; SourceWidth <= 5; ++SourceWidth) {
    SymContext C;
    auto X = C.mkVar("source", SourceWidth);
    for (unsigned Kind = 0; Kind < 3; ++Kind)
      for (unsigned Shift = 0; Shift <= SourceWidth + 1; ++Shift) {
        auto Count = C.mkConst(64, Shift);
        auto Value = Kind == 0   ? C.mkLShr(X, Count)
                     : Kind == 1 ? C.mkAShr(X, Count)
                                 : C.mkShl(X, Count);
        for (unsigned Low = 0; Low < SourceWidth; ++Low)
          for (unsigned Width = 1; Width <= SourceWidth - Low; ++Width) {
            auto Result = simplifyWindow(C, C.mkExtract(Value, Low, Width));
            for (unsigned Input = 0; Input < (1U << SourceWidth); ++Input) {
              llvm::APInt A(SourceWidth, Input);
              auto Expected =
                  Kind == 1 ? A.ashr(std::min(Shift, SourceWidth - 1))
                  : Shift >= SourceWidth ? llvm::APInt(SourceWidth, 0)
                  : Kind == 0            ? A.lshr(Shift)
                                         : A.shl(Shift);
              EXPECT_EQ(C.eval(Result, {A}), Expected.extractBits(Width, Low));
            }
          }
      }
  }
}

TEST(SymMBAObservedWindow, SymbolicCountsAndBudgetExhaustionKeepAllInputs) {
  SymContext C;
  auto X = C.mkVar("source", 64), Count = C.mkVar("count", 256);
  auto Root = C.mkExtract(C.mkLShr(X, Count), 3, 8);
  auto Reduced = simplifyWindow(C, Root);
  for (const auto &Shift :
       {llvm::APInt(256, 4), llvm::APInt::getOneBitSet(256, 200)}) {
    auto Input = llvm::APInt::getAllOnes(64);
    EXPECT_EQ(C.eval(Reduced, {Input, Shift}),
              Shift.uge(64)
                  ? llvm::APInt(8, 0)
                  : Input.lshr(Shift.getZExtValue()).extractBits(8, 3));
  }
  MBAOptions Limits;
  Limits.MaxTableBytes = 0;
  const auto Before = C.numNodes();
  EXPECT_EQ(simplifyWindow(C, Root, Limits), Root);
  EXPECT_EQ(C.numNodes(), Before);
  Limits.MaxTableBytes = size_t(1) << 28;
  Limits.MaxWork = 0;
  EXPECT_EQ(simplifyWindow(C, Root, Limits), Root);
}

TEST(SymMBAObservedWindow, DeepDagTraversalDoesNotUseTheHostStack) {
  SymContext C;
  auto X = C.mkVar("source", 64), Y = C.mkVar("divisor", 64);
  for (unsigned I = 0; I < 5000; ++I)
    X = C.mkUDiv(X, Y);
  auto Root = C.mkExtract(X, 3, 8);
  auto Before = C.numNodes();
  EXPECT_EQ(simplifyWindow(C, Root), Root);
  EXPECT_EQ(C.numNodes(), Before);
}

TEST(SymMBAObservedWindow, WideWindowsHaveBitvectorEquivalenceProofs) {
  for (unsigned Width : {8U, 16U, 32U, 64U, 128U, 256U}) {
    SymContext C;
    auto X = C.mkVar("source", Width);
    for (unsigned Shift : {1U, Width / 2, Width - 1, Width, Width + 1})
      for (unsigned Kind = 0; Kind < 3; ++Kind) {
        auto Count = C.mkConst(256, Shift);
        auto Shifted = Kind == 0   ? C.mkLShr(X, Count)
                       : Kind == 1 ? C.mkAShr(X, Count)
                                   : C.mkShl(X, Count);
        auto Root = C.mkExtract(Shifted, Width / 4, Width / 2);
        auto Result = simplifyWindow(C, Root);
        neverd::solver::BitVectorSolver Proof(C);
        EXPECT_EQ(Proof.check(C.mkNe(Root, Result)),
                  neverd::solver::SatResult::Unsat)
            << Width << ":" << Kind << ":" << Shift;
      }
  }
}

TEST(SymMBAObservedWindow, NonLowArithmeticWindowsRetainIncomingCarries) {
  SymContext C;
  auto X = C.mkVar("left", 16), Y = C.mkVar("right", 16);
  auto Sum = C.mkExtract(C.mkAdd(X, Y), 8, 8);
  auto Product = C.mkExtract(C.mkMul(X, Y), 8, 8);
  for (auto Root : {Sum, Product}) {
    auto Reduced = simplifyWindow(C, Root);
    neverd::solver::BitVectorSolver Proof(C);
    EXPECT_EQ(Proof.check(C.mkNe(Root, Reduced)),
              neverd::solver::SatResult::Unsat);
    for (unsigned A : {0U, 1U, 127U, 255U, 256U, 65535U})
      for (unsigned B : {1U, 129U, 255U, 256U, 65535U}) {
        auto Expected = Root == Sum ? A + B : A * B;
        EXPECT_EQ(C.eval(Reduced, {llvm::APInt(16, A), llvm::APInt(16, B)}),
                  llvm::APInt(8, Expected >> 8));
      }
  }
}

TEST(SymMBAObservedWindow, PartialWindowBudgetsPreserveEveryInput) {
  SymContext C;
  auto Tag = C.mkVar("tag", 1), X = C.mkVar("signed_byte", 8);
  auto Y = C.mkVar("other_word", 64);
  auto Root = C.mkExtract(
      C.mkIte(Tag, C.mkSExt(X, 64), C.mkAShr(Y, C.mkConst(256, 3))), 4, 16);
  for (size_t Storage : {0U, 127U, 256U, 4096U})
    for (size_t Work : {0U, 1U, 4U, 128U, 4096U}) {
      MBAOptions Options;
      Options.MaxWork = Work;
      Options.MaxTableBytes = Storage;
      Options.VerifySamples = 0;
      auto Result = simplifyMBADeep(C, Root, Options);
      EXPECT_LE(Result.Work, Work);
      if (!Work || !Storage)
        EXPECT_EQ(Result.Expr, Root);
      neverd::solver::BitVectorSolver Proof(C);
      EXPECT_EQ(Proof.check(C.mkNe(Root, Result.Expr)),
                neverd::solver::SatResult::Unsat);
    }
}
