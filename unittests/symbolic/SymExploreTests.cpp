//===- SymExploreTests.cpp - Walking every path a function can take -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins which paths the walk finds and, as much, which ones it declines to.
///
/// A branch the code has already decided must not become two paths — that is
/// what keeps an opaque predicate from doubling the work and the output — and
/// a loop with no known trip count must stop rather than not.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymExplore.h"

#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr va_t kBase = 0x400000;
constexpr uint64_t kBlockSize = 0x10;
constexpr uint64_t kRax = 0;
constexpr uint64_t kRbx = 8;
constexpr uint64_t kFlag = 64;

LowOp op(NdOp Opcode, NdVar Output, std::vector<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const NdVar &In : Inputs)
    Result.addInput(In);
  return Result;
}

/// Assembles a function out of blocks laid end to end, so a block's address is
/// its index and a branch can name where it goes by number.
class FunctionBuilder {
public:
  /// Add a block and return its index.  \p Succs are the blocks control can
  /// reach from it, which is what the walk consults for a fallthrough.
  int block(std::vector<LowOp> Ops, std::vector<int> Succs) {
    LowBlock B;
    B.Id = static_cast<int>(Func.Blocks.size());
    B.StartAddr = kBase + uint64_t(B.Id) * kBlockSize;
    B.EndAddr = B.StartAddr + kBlockSize;
    B.Ops = std::move(Ops);
    B.Succs = std::move(Succs);
    Func.Blocks.push_back(std::move(B));
    return Func.Blocks.back().Id;
  }

  static NdVar addressOf(int BlockId) {
    return NdVar::cst(kBase + uint64_t(BlockId) * kBlockSize, 8);
  }

  const LowFunc &function() {
    Func.Entry = kBase;
    return Func;
  }

private:
  LowFunc Func;
};

/// A comparison of rax against a constant, left in a one-byte flag.
LowOp compareRax(uint64_t Against) {
  return op(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
            {NdVar::reg(kRax, 8), NdVar::cst(Against, 8)});
}

TEST(SymExplore, AStraightLineFunctionIsOnePath) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::INT_ADD, NdVar::reg(kRbx, 8),
              {NdVar::reg(kRax, 8), NdVar::cst(1, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 1u);
  EXPECT_EQ(Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_TRUE(Paths[0].Constraints.empty());
  EXPECT_EQ(Paths[0].Blocks, std::vector<int>{0});
}

TEST(SymExplore, TargetByteOrderConfiguresTheInitialState) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::COPY, NdVar::reg(kRax, 4), {NdVar::cst(0xAABBCCDD, 4)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});
  ExploreOptions Opts;
  Opts.ByteOrder = llvm::endianness::big;

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function(), Opts);
  ASSERT_EQ(Paths.size(), 1u);
  SymRef FirstByte = Paths[0].State.read(SymSpace::Register, kRax, 1);
  ASSERT_TRUE(Ctx.isConst(FirstByte));
  EXPECT_EQ(Ctx.constValue(FirstByte).getZExtValue(), 0xAAu);
}

TEST(SymExplore, TargetCallingConventionPreservesDeclaredRegisterBytes) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(2, 8)}),
           op(NdOp::CALL, NdVar{}, {NdVar::cst(0x401500, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});
  ExploreOptions Opts;
  Opts.CallPreservedRegisters.push_back({kRbx, 8});

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);
  std::vector<SymPath> &Paths = Exploration.Paths;
  ASSERT_EQ(Paths.size(), 1u);
  EXPECT_EQ(Exploration.UnmodelledOps, 1u);
  EXPECT_EQ(Paths[0].UnmodelledOps, 1u);
  SymRef Preserved = Paths[0].State.read(SymSpace::Register, kRbx, 8);
  ASSERT_TRUE(Ctx.isConst(Preserved));
  EXPECT_EQ(Ctx.constValue(Preserved).getZExtValue(), 2u);
}

TEST(SymExplore, ARealBranchBecomesTwoPathsWithOppositeConditions) {
  SymContext Ctx;
  FunctionBuilder B;
  int Then = 1, Else = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)})},
          {Then, Else});
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(1, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(2, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 2u);

  // Depth first, so the taken side finishes first.
  EXPECT_EQ(Paths[0].Blocks, (std::vector<int>{0, Then}));
  EXPECT_EQ(Paths[1].Blocks, (std::vector<int>{0, Else}));
  ASSERT_EQ(Paths[0].Constraints.size(), 1u);
  ASSERT_EQ(Paths[1].Constraints.size(), 1u);
  // The two paths assume opposite things, so together they assume nothing.
  EXPECT_EQ(Ctx.mkNot(Paths[0].Constraints[0]), Paths[1].Constraints[0]);
  EXPECT_EQ(Ctx.mkAnd(Paths[0].predicate(Ctx), Paths[1].predicate(Ctx)),
            Ctx.mkFalse());

  // Each path knows what it left behind.
  SymRef ThenValue = Paths[0].State.read(SymSpace::Register, kRbx, 8);
  SymRef ElseValue = Paths[1].State.read(SymSpace::Register, kRbx, 8);
  ASSERT_TRUE(Ctx.isConst(ThenValue));
  ASSERT_TRUE(Ctx.isConst(ElseValue));
  EXPECT_EQ(Ctx.constValue(ThenValue).getZExtValue(), 1u);
  EXPECT_EQ(Ctx.constValue(ElseValue).getZExtValue(), 2u);
}

TEST(SymExplore, UnknownsCreatedAfterAForkRemainPathLocal) {
  SymContext Ctx;
  FunctionBuilder B;
  int Then = 1, Else = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Then), NdVar::reg(kFlag, 1)})},
          {Then, Else});
  B.block({op(NdOp::FLOAT_SQRT, NdVar::reg(kRbx, 8), {NdVar::reg(kRax, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});
  B.block({op(NdOp::FLOAT_SQRT, NdVar::reg(kRbx, 8), {NdVar::reg(kRax, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_NE(Paths[0].State.read(SymSpace::Register, kRbx, 8),
            Paths[1].State.read(SymSpace::Register, kRbx, 8));
  EXPECT_EQ(Paths[0].UnmodelledOps, 1u);
  EXPECT_EQ(Paths[1].UnmodelledOps, 1u);
}

TEST(SymExplore, ContradictoryPathConditionsAreNotExecuted) {
  SymContext Ctx;
  FunctionBuilder B;
  int Repeat = 1, Taken = 2, Else = 3;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Repeat), NdVar::reg(kFlag, 1)})},
          {Repeat, Else});
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Taken), NdVar::reg(kFlag, 1)})},
          {Taken, Else});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  ExploreOptions Opts;
  Opts.MaxPaths = 2;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);
  const std::vector<SymPath> &Paths = Exploration.Paths;
  EXPECT_TRUE(Exploration.Complete);
  EXPECT_EQ(Exploration.ReachablePaths, 2u);
  ASSERT_EQ(Paths.size(), 3u);
  EXPECT_EQ(Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Paths[0].Blocks, (std::vector<int>{0, Repeat, Taken}));
  EXPECT_EQ(Paths[1].Outcome, PathOutcome::Infeasible);
  EXPECT_EQ(Paths[1].Blocks, (std::vector<int>{0, Repeat}));
  EXPECT_EQ(Paths[1].predicate(Ctx), Ctx.mkFalse());
  EXPECT_EQ(Paths[2].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Paths[2].Blocks, (std::vector<int>{0, Else}));
}

TEST(SymExplore, StepBudgetsApplyIndependentlyToEachPath) {
  SymContext Ctx;
  FunctionBuilder B;
  int Then = 1, Else = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Then), NdVar::reg(kFlag, 1)})},
          {Then, Else});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  ExploreOptions Opts;
  Opts.MaxSteps = 3;

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);
  const std::vector<SymPath> &Paths = Exploration.Paths;
  EXPECT_TRUE(Exploration.Complete);
  EXPECT_EQ(Exploration.ExecutedSteps, 4u);
  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_EQ(Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Paths[1].Outcome, PathOutcome::Returned);
}

TEST(SymExplore, APredicateTheCodeAlreadyDecidedDoesNotFork) {
  // `(x | ~x) != 0` holds for every x.  Nothing in the walk knows that; the
  // expression builders fold the condition to a constant and there is then
  // only one side to go down.  This is what an opaque predicate costs here:
  // nothing.
  SymContext Ctx;
  FunctionBuilder B;
  int Real = 1, Dead = 2;
  B.block({op(NdOp::INT_NOT, NdVar::tmp(0, 8), {NdVar::reg(kRax, 8)}),
           op(NdOp::INT_OR, NdVar::tmp(8, 8),
              {NdVar::reg(kRax, 8), NdVar::tmp(0, 8)}),
           op(NdOp::INT_NOTEQUAL, NdVar::reg(kFlag, 1),
              {NdVar::tmp(8, 8), NdVar::cst(0, 8)}),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)})},
          {Real, Dead});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 1u) << "the guarded side is not reachable";
  EXPECT_EQ(Paths[0].Blocks, (std::vector<int>{0, Real}));
  EXPECT_TRUE(Paths[0].Constraints.empty())
      << "a decided branch is not a choice, so it assumes nothing";
}

TEST(SymExplore, ALoopWithNoKnownTripCountStopsAtItsBound) {
  SymContext Ctx;
  FunctionBuilder B;
  int Header = 0, Body = 1, Exit = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)})},
          {Body, Exit});
  B.block({op(NdOp::INT_ADD, NdVar::reg(kRax, 8),
              {NdVar::reg(kRax, 8), NdVar::cst(1, 8)}),
           op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(0)})},
          {Header});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  ExploreOptions Opts;
  Opts.MaxBlockVisits = 3;
  std::vector<SymPath> Paths = explorePaths(Ctx, B.function(), Opts);

  // Three trips round before conceding, and one exit found on each of them.
  ASSERT_FALSE(Paths.empty());
  unsigned Returned = 0, Bounded = 0;
  for (const SymPath &P : Paths) {
    Returned += P.Outcome == PathOutcome::Returned;
    Bounded += P.Outcome == PathOutcome::LoopBudget;
  }
  EXPECT_EQ(Returned, 3u);
  EXPECT_EQ(Bounded, 1u);

  // Depth first, so the path that kept going round comes back first and the
  // exits follow in the reverse of the order they were passed: three trips
  // assumed, then two, then one.
  EXPECT_EQ(Paths.front().Outcome, PathOutcome::LoopBudget);
  EXPECT_EQ(Paths.front().Constraints.size(), 3u);
  EXPECT_EQ(Paths.back().Constraints.size(), 1u);
}

TEST(SymExplore, AnUnresolvedIndirectBranchEndsThePathAndSaysWhereItWasGoing) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::INT_MULT, NdVar::tmp(0, 8),
              {NdVar::reg(kRax, 8), NdVar::cst(8, 8)}),
           op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(0, 8)})},
          {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 1u);
  EXPECT_EQ(Paths[0].Outcome, PathOutcome::UnresolvedBranch);
  // The target survives as an expression, which is what switch recovery needs
  // in order to ask what it becomes for each index.
  EXPECT_TRUE(Paths[0].Target.isValid());
  EXPECT_FALSE(Ctx.isConst(Paths[0].Target));
}

TEST(SymExplore, TheNumberOfPathsIsBounded) {
  // Four diamonds in a row is sixteen paths.  Asking for five gets five, which
  // is the only reason a walk of anything real terminates.
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int kDiamonds = 4;

  // Laid out as head, taken arm, fallthrough arm, repeating, with the return
  // block last.  Both arms rejoin at the next head, so the count doubles per
  // diamond rather than the graph growing.
  auto head = [](int I) { return 3 * I; };
  auto taken = [](int I) { return 3 * I + 1; };
  auto other = [](int I) { return 3 * I + 2; };
  const int Tail = 3 * kDiamonds;

  for (int I = 0; I < kDiamonds; ++I) {
    // Compared against one upwards: nothing is below zero unsigned, so `< 0`
    // would be a decided branch and this test would be measuring the folding
    // rather than the budget.
    B.block({compareRax(uint64_t(I) + 1),
             op(NdOp::COND_BR, NdVar{},
                {FunctionBuilder::addressOf(taken(I)), NdVar::reg(kFlag, 1)})},
            {taken(I), other(I)});
    const int Next = I + 1 < kDiamonds ? head(I + 1) : Tail;
    B.block({op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Next)})},
            {Next});
    B.block({op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Next)})},
            {Next});
  }
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  ExploreOptions Generous;
  Generous.MaxPaths = 64;
  EXPECT_EQ(explorePaths(Ctx, B.function(), Generous).size(), 16u);

  ExploreOptions Tight;
  Tight.MaxPaths = 5;
  EXPECT_EQ(explorePaths(Ctx, B.function(), Tight).size(), 5u);
}

} // namespace
