//===- SymMBAParityTests.cpp - Parity-separated Boolean kernels ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/symbolic/mba/SymMBADetail.h"
#include "SymMBATestsDetail.h"

#include "neverd/symbolic/SymBitwise.h"

namespace {
using namespace neverd::symbolic;

TruthTable kernelTable(unsigned Count, bool Complement = false) {
  TruthTable Result = atomTruthTable(0, Count);
  Result ^= atomTruthTable(1, Count);
  Result |= atomTruthTable(2, Count);
  for (unsigned I = 3; I < Count; ++I)
    Result ^= atomTruthTable(I, Count);
  return Complement ? ~Result : Result;
}

llvm::SmallVector<SymRef, 8> variables(SymContext &Ctx, unsigned Count,
                                       unsigned Width) {
  llvm::SmallVector<SymRef, 8> Atoms;
  for (unsigned I = 0; I < Count; ++I)
    Atoms.push_back(Ctx.mkVar("v" + std::to_string(I), Width));
  return Atoms;
}

SymRef expectedKernel(SymContext &Ctx, llvm::ArrayRef<SymRef> Atoms,
                      bool Complement = false) {
  llvm::SmallVector<SymRef, 8> Terms{
      Ctx.mkOr(Ctx.mkXor(Atoms[0], Atoms[1]), Atoms[2])};
  Terms.append(Atoms.begin() + 3, Atoms.end());
  SymRef Result = Ctx.mkXor(Terms);
  return Complement ? Ctx.mkNot(Result) : Result;
}

TEST(SymMBAParity, SeparatesAnIndependentParityFromAThreeInputKernel) {
  for (unsigned Width : {1u, 3u, 8u, 32u, 64u, 128u, 256u}) {
    for (bool Deep : {false, true}) {
      SymContext Ctx;
      auto Input = parseSymExpr(Ctx, "p ^ q ^ r ^ s ^ (p&r) ^ (q&r)", Width);
      auto Expected = parseSymExpr(Ctx, "s ^ ((p^q)|r)", Width);
      ASSERT_TRUE(Input.ok());
      ASSERT_TRUE(Expected.ok());
      MBAOptions Opts;
      Opts.VerifySamples = 0;
      auto Result = Deep ? simplifyMBADeep(Ctx, Input.Root, Opts)
                         : simplifyMBA(Ctx, Input.Root, Opts);
      EXPECT_EQ(Result.Expr, Expected.Root) << Ctx.toString(Result.Expr);
      EXPECT_LE(Result.Work, Opts.MaxWork);
      EXPECT_LT(Result.SizeAfter, Result.SizeBefore);
      EXPECT_EQ(Result.Evidence, MBAEvidence::Derivation);
    }
  }
}

TEST(SymMBAParity, RetainsMultipleArmsAndTheConstantTerm) {
  for (unsigned Count : {4u, 5u, 6u, 8u}) {
    for (bool Complement : {false, true}) {
      SymContext Ctx;
      auto Atoms = variables(Ctx, Count, 32);
      auto Result =
          synthesizeBitwise(Ctx, kernelTable(Count, Complement), Atoms);
      ASSERT_TRUE(Result.has_value());
      SymRef Expected = expectedKernel(Ctx, Atoms, Complement);
      EXPECT_LE(Ctx.readabilityCost(*Result), Ctx.readabilityCost(Expected));
      SymEvalPlan Plan(Ctx, *Result);
      llvm::SmallVector<uint64_t, 8> Values(Count, 0);
      for (unsigned K = 0; K < (1u << Count); ++K) {
        for (unsigned I = 0; I < Count; ++I)
          Values[I] = K & (1u << I) ? 0xffffffffu : 0;
        EXPECT_EQ(Plan.evalU64(Values),
                  kernelTable(Count, Complement).at(K) ? 0xffffffffu : 0);
      }
    }
  }
}

TEST(SymMBAParity, ExhaustsEverySmallKernelAndNearbyCoupledArm) {
  for (unsigned Packed = 0; Packed < 256; ++Packed) {
    for (bool Coupled : {false, true}) {
      SymContext Ctx;
      auto Atoms = variables(Ctx, 4, 1);
      TruthTable Table = TruthTable::zero(4);
      for (unsigned K = 0; K < 16; ++K) {
        bool Value = ((Packed >> (K & 7)) & 1) ^ ((K >> 3) & 1);
        if (Coupled)
          Value ^= (K & 9) == 9;
        Table.setValue(K, Value);
      }
      auto Result = synthesizeBitwise(Ctx, Table, Atoms);
      ASSERT_TRUE(Result.has_value());
      SymEvalPlan Plan(Ctx, *Result);
      llvm::SmallVector<uint64_t, 4> Values(4, 0);
      for (unsigned K = 0; K < 16; ++K) {
        for (unsigned I = 0; I < 4; ++I)
          Values[I] = (K >> I) & 1;
        EXPECT_EQ(Plan.evalU64(Values), Table.at(K))
            << Packed << ": " << Coupled << ": " << K;
      }
    }
  }
}

TEST(SymMBAParity, NeverPeelsAnArmUsedInALargerMonomial) {
  for (unsigned Width : {1u, 3u}) {
    SymContext Ctx;
    auto Input =
        parseSymExpr(Ctx, "p ^ q ^ r ^ s ^ (p&r) ^ (q&r) ^ (p&s)", Width);
    ASSERT_TRUE(Input.ok());
    MBAOptions Opts;
    Opts.VerifySamples = 0;
    auto Result = simplifyMBA(Ctx, Input.Root, Opts);
    SymEvalPlan Before(Ctx, Input.Root), After(Ctx, Result.Expr);
    llvm::SmallVector<uint64_t, 4> Values(4, 0);
    for (unsigned K = 0; K < (1u << (4 * Width)); ++K) {
      for (unsigned I = 0; I < 4; ++I)
        Values[I] = (K >> (I * Width)) & ((1u << Width) - 1);
      EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
    }
  }
}

TEST(SymMBAParity, SharesTheANFWorkLimitBeforeConstructingAKernel) {
  // 64 ANF entries, six terms, four support bits, 72 projection operations,
  // 49 recipe operations and 512 bounded memo slots share this allowance.
  for (size_t Work : {0u, 64u, 195u, 706u}) {
    SymContext Ctx;
    auto Atoms = variables(Ctx, 4, 32);
    BitwiseSynthesisLimits Limits;
    Limits.MaxCost = 7;
    Limits.MaxWork = Work;
    const size_t Before = Ctx.numNodes();
    EXPECT_FALSE(synthesizeBitwise(Ctx, kernelTable(4), Atoms, Limits));
    EXPECT_EQ(Ctx.numNodes(), Before);
  }
  SymContext Ctx;
  auto Atoms = variables(Ctx, 4, 32);
  BitwiseSynthesisLimits Limits;
  Limits.MaxCost = 7;
  Limits.MaxWork = 707;
  auto Result = synthesizeBitwise(Ctx, kernelTable(4), Atoms, Limits);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, expectedKernel(Ctx, Atoms));
}

TEST(SymMBAParity, UnavailableANFIsNotZeroWithAnUnlimitedCostLimit) {
  SymContext Ctx;
  auto Atoms = variables(Ctx, 4, 32);
  BitwiseSynthesisLimits Limits;
  Limits.MaxWork = 0;
  const size_t Before = Ctx.numNodes();
  EXPECT_FALSE(synthesizeBitwise(Ctx, kernelTable(4), Atoms, Limits));
  EXPECT_EQ(Ctx.numNodes(), Before);
}

TEST(SymMBAParity, RespectsCostAndOptimalArityLimitsWithoutCandidateNodes) {
  for (size_t Cost : {0u, 1u, 4u, 6u}) {
    SymContext Ctx;
    auto Atoms = variables(Ctx, 4, 32);
    BitwiseSynthesisLimits Limits;
    Limits.MaxCost = Cost;
    const size_t Before = Ctx.numNodes();
    EXPECT_FALSE(synthesizeBitwise(Ctx, kernelTable(4), Atoms, Limits));
    EXPECT_EQ(Ctx.numNodes(), Before);
  }
  for (unsigned Ceiling : {0u, 1u, 2u}) {
    SymContext Ctx;
    auto Atoms = variables(Ctx, 4, 32);
    BitwiseSynthesisLimits Limits;
    Limits.MaxCost = 7;
    Limits.MaxOptimalAtoms = Ceiling;
    const size_t Before = Ctx.numNodes();
    EXPECT_FALSE(synthesizeBitwise(Ctx, kernelTable(4), Atoms, Limits));
    EXPECT_EQ(Ctx.numNodes(), Before);
  }
}

TEST(SymMBAParity, PreservesTheRemainingWholeRegionBudget) {
  for (size_t Work : {0u, 1u, 32u, 128u, 1024u}) {
    SymContext Ctx;
    auto Input = parseSymExpr(Ctx, "p ^ q ^ r ^ s ^ (p&r) ^ (q&r)", 3);
    ASSERT_TRUE(Input.ok());
    MBAOptions Opts;
    Opts.MaxWork = Work;
    Opts.VerifySamples = 0;
    auto Result = simplifyMBADeep(Ctx, Input.Root, Opts);
    EXPECT_LE(Result.Work, Work);
    EXPECT_LE(Result.SizeAfter, Result.SizeBefore);
    SymEvalPlan Before(Ctx, Input.Root), After(Ctx, Result.Expr);
    llvm::SmallVector<uint64_t, 4> Values(4, 0);
    for (unsigned K = 0; K < 4096; ++K) {
      for (unsigned I = 0; I < 4; ++I)
        Values[I] = (K >> (3 * I)) & 7;
      EXPECT_EQ(Before.evalU64(Values), After.evalU64(Values));
    }
  }
}

TEST(SymMBAParity, KeepsOpaqueInputsSharedAcrossManySourceVariables) {
  for (unsigned Count : {4u, 8u, 16u, 32u, 64u}) {
    SymContext Ctx;
    auto Inputs = variables(Ctx, Count, 32);
    llvm::SmallVector<SymRef, 4> Atoms;
    for (unsigned I = 0; I < 4; ++I)
      Atoms.push_back(
          Ctx.mkMul(llvm::ArrayRef(Inputs).slice(I * (Count / 4), Count / 4)));
    auto Result = synthesizeBitwise(Ctx, kernelTable(4), Atoms);
    ASSERT_TRUE(Result.has_value());
    EXPECT_EQ(*Result, expectedKernel(Ctx, Atoms));
  }
}

TEST(SymMBAParity, DoesNotRecurseThroughDeepSharedInputs) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  SymRef Tail = X;
  for (unsigned I = 0; I < 4096; ++I)
    Tail = Ctx.mkUDiv(Tail, Ctx.mkAdd(Y, Ctx.mkConst(32, I + 1)));
  llvm::SmallVector<SymRef, 4> Atoms;
  for (unsigned I = 0; I < 4; ++I)
    Atoms.push_back(Ctx.mkUDiv(Tail, Ctx.mkVar("z" + std::to_string(I), 32)));
  const size_t Before = Ctx.numNodes();
  auto Result = synthesizeBitwise(Ctx, kernelTable(4), Atoms);
  ASSERT_TRUE(Result.has_value());
  EXPECT_LE(Ctx.numNodes() - Before, 6u);
  EXPECT_EQ(*Result, expectedKernel(Ctx, Atoms));
}

TEST(SymMBAParity, KeepsAFormThatCollapsesThroughRepeatedAtoms) {
  SymContext Ctx;
  SymRef X = Ctx.mkVar("x", 32), Y = Ctx.mkVar("y", 32);
  llvm::SmallVector<SymRef, 4> Atoms{X, X, Y, Y};
  auto Result = synthesizeBitwise(Ctx, kernelTable(4), Atoms);
  ASSERT_TRUE(Result.has_value());
  EXPECT_EQ(*Result, Ctx.mkZero(32));
}

TEST(SymMBAParity, RefusesAnUnrepresentableTruthTableWithoutExpansion) {
  SymContext Ctx;
  auto Atoms = variables(Ctx, 64, 32);
  SymRef Input = Ctx.mkXor(Atoms);
  MBAOptions Opts;
  Opts.MaxWork = 1024;
  Opts.VerifySamples = 0;
  const size_t Before = Ctx.numNodes();
  auto Result = simplifyMBA(Ctx, Input, Opts);
  EXPECT_EQ(Result.Expr, Input);
  EXPECT_LE(Result.Work, Opts.MaxWork);
  EXPECT_EQ(Ctx.numNodes(), Before);
}

TEST(SymMBAParity, CompletedValuationsReuseStableSlotsAndFallBackLocally) {
  for (unsigned Width : {8u, 64u, 257u}) {
    for (size_t Capacity : {size_t(1), size_t(8), size_t(64)}) {
      SymContext C;
      detail::WorkBudget Budget(16384);
      Budget.MemoOwner = &C;
      Budget.MemoCapacity = Capacity;
      const auto Scale = C.mkConst(llvm::APInt(Width, 1).shl(Width - 8));
      for (unsigned I = 0; I != 7; ++I) {
        auto X = C.mkVar("x" + std::to_string(I), Width);
        auto Make = [&](unsigned Power) {
          llvm::SmallVector<SymRef, 32> F(Power, X);
          F.append(Power, C.mkNot(X));
          F.push_back(Scale);
          return C.mkMul(F);
        };
        const auto Input = Make(8), Near = Make(7);
        size_t Storage = MBAOptions{}.MaxTableBytes;
        EXPECT_EQ(detail::foldProductValuations(C, Input, Budget, Storage),
                  C.mkZero(Width));
        const size_t Start = Budget.used();
        Storage = MBAOptions{}.MaxTableBytes;
        EXPECT_EQ(detail::foldProductValuations(C, Input, Budget, Storage),
                  C.mkZero(Width));
        if (Capacity == 64 && I < 6)
          EXPECT_EQ(Budget.used() - Start, 1u);
        Storage = MBAOptions{}.MaxTableBytes;
        EXPECT_EQ(detail::foldProductValuations(C, Near, Budget, Storage),
                  Near);
        llvm::SmallVector<llvm::APInt, 8> Values(C.numVars(),
                                                 llvm::APInt(Width, 2));
        EXPECT_EQ(C.eval(Near, Values), llvm::APInt::getSignMask(Width));
      }
      EXPECT_FALSE(Budget.exhausted());
    }
  }
}

} // namespace
