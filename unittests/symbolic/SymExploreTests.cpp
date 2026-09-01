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

#include "neverd/symbolic/SymConcrete.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr va_t kBase = 0x400000;
constexpr uint64_t kBlockSize = 4;
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

  int instruction(
      std::vector<LowOp> Ops, std::vector<int> Succs, InstructionMode Mode,
      LowInstructionControl Control, LowInstructionControlFlag Flags,
      LowInstructionTargetMode TargetMode = LowInstructionTargetMode::Preserve,
      std::optional<uint64_t> Immediate = std::nullopt) {
    const int Id = block(std::move(Ops), std::move(Succs));
    LowBlock &B = Func.Blocks[static_cast<size_t>(Id)];
    for (LowOp &Op : B.Ops)
      Op.Addr = B.StartAddr;
    B.InstructionBoundaries.push_back(
        {B.StartAddr, static_cast<uint16_t>(B.EndAddr - B.StartAddr), 0,
         static_cast<uint64_t>(B.Ops.size()), Mode, Control, Flags, TargetMode,
         Immediate});
    return Id;
  }

  /// Add the single-instruction shape ARM uses for predicated control: a guard
  /// branches over the remainder of the same decoded instruction.  Keeping the
  /// real boundary in the fixture makes this a test of the public LowIR
  /// contract, not of an address-based implementation detail.
  int predicatedInstruction(std::vector<LowOp> Ops, std::vector<int> Succs,
                            LowInstructionControl Control,
                            LowInstructionControlFlag Flags,
                            std::optional<uint64_t> Immediate = std::nullopt) {
    return instruction(std::move(Ops), std::move(Succs), InstructionMode::ARM,
                       Control, Flags, LowInstructionTargetMode::Preserve,
                       Immediate);
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

/// A loop nothing bounds: the header tests rax and the body increments it, so
/// there is an exit on every iteration and no last one.
void buildCountingLoop(FunctionBuilder &B) {
  constexpr int Header = 0, Body = 1, Exit = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Body), NdVar::reg(kFlag, 1)})},
          {Body, Exit});
  B.block({op(NdOp::INT_ADD, NdVar::reg(kRax, 8),
              {NdVar::reg(kRax, 8), NdVar::cst(1, 8)}),
           op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Header)})},
          {Header});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
}

/// \p Count diamonds laid head to tail, with the return block last.  Both arms
/// of each rejoin at the next head and leave the state exactly as they found
/// it, so the number of routes doubles per diamond while the number of
/// distinct outcomes stays at one.
void buildDiamondChain(FunctionBuilder &B, int Count) {
  auto head = [](int I) { return 3 * I; };
  auto taken = [](int I) { return 3 * I + 1; };
  auto other = [](int I) { return 3 * I + 2; };
  const int Tail = 3 * Count;

  for (int I = 0; I < Count; ++I) {
    // Compared against one upwards: nothing is below zero unsigned, so `< 0`
    // would be a decided branch and this would be measuring the folding
    // rather than the walk.
    B.block({compareRax(uint64_t(I) + 1),
             op(NdOp::COND_BR, NdVar{},
                {FunctionBuilder::addressOf(taken(I)), NdVar::reg(kFlag, 1)})},
            {taken(I), other(I)});
    const int Next = I + 1 < Count ? head(I + 1) : Tail;
    B.block({op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Next)})},
            {Next});
    B.block({op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Next)})},
            {Next});
  }
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
}

unsigned countOutcome(const std::vector<SymPath> &Paths, PathOutcome Wanted) {
  unsigned Count = 0;
  for (const SymPath &P : Paths)
    Count += P.Outcome == Wanted;
  return Count;
}

class BranchShadow final : public ConcreteShadow {
public:
  explicit BranchShadow(std::optional<uint64_t> Decision,
                        std::optional<unsigned> FirstFailingStep = std::nullopt)
      : Decision(Decision), FirstFailingStep(FirstFailingStep) {}

  bool reset(llvm::endianness) override {
    Steps = 0;
    return true;
  }
  bool setRegister(uint64_t, uint16_t, uint64_t) override { return true; }
  bool step(const LowOp &) override {
    return !FirstFailingStep || Steps++ < *FirstFailingStep;
  }

  std::optional<uint64_t> value(const NdVar &V) override {
    if (V.isConst())
      return V.Offset;
    return Decision;
  }

private:
  std::optional<uint64_t> Decision;
  std::optional<unsigned> FirstFailingStep;
  unsigned Steps = 0;
};

class RejectingResetShadow final : public ConcreteShadow {
public:
  bool reset(llvm::endianness) override { return false; }
  bool setRegister(uint64_t, uint16_t, uint64_t) override {
    ++SeedCalls;
    return true;
  }
  bool step(const LowOp &) override { return false; }
  std::optional<uint64_t> value(const NdVar &) override { return std::nullopt; }

  unsigned SeedCalls = 0;
};

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

TEST(SymExplore, TracksTheCallResultUsedByLaterBranches) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int NonNull = 1, Null = 2;
  B.block({op(NdOp::CALL, NdVar{}, {NdVar::cst(0x401500, 8)}),
           op(NdOp::INT_NOTEQUAL, NdVar::reg(kFlag, 1),
              {NdVar::reg(kRax, 8), NdVar::cst(0, 8)}),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(NonNull), NdVar::reg(kFlag, 1)})},
          {NonNull, Null});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  ExploreOptions Opts;
  Opts.TrackedCallResultRegister = SymRegisterRange{kRax, 8};

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);
  ASSERT_EQ(Exploration.Paths.size(), 2u);
  for (SymPath &Path : Exploration.Paths) {
    ASSERT_EQ(Path.CallResults.size(), 1u);
    EXPECT_EQ(Path.CallResults[0].Value,
              Path.State.read(SymSpace::Register, kRax, 8));
  }

  EXPECT_EQ(Exploration.Paths[0].Blocks, (std::vector<int>{0, NonNull}));
  EXPECT_EQ(Exploration.Paths[1].Blocks, (std::vector<int>{0, Null}));
  EXPECT_EQ(Ctx.mkNot(Exploration.Paths[0].predicate(Ctx)),
            Exploration.Paths[1].predicate(Ctx));
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
  ASSERT_EQ(Paths[0].BranchDecisions.size(), 1u);
  ASSERT_EQ(Paths[1].BranchDecisions.size(), 1u);
  EXPECT_EQ(Paths[0].BranchDecisions[0].Condition, Paths[0].Constraints[0]);
  EXPECT_EQ(Paths[1].BranchDecisions[0].Condition, Paths[0].Constraints[0]);
  EXPECT_TRUE(Paths[0].BranchDecisions[0].Taken);
  EXPECT_FALSE(Paths[1].BranchDecisions[0].Taken);
  EXPECT_EQ(Paths[0].BranchDecisions[0].ConstraintPrefix, 0u);
  EXPECT_EQ(Paths[1].BranchDecisions[0].ConstraintPrefix, 0u);
  EXPECT_FALSE(Paths[0].BranchDecisions[0].Concrete);
  EXPECT_FALSE(Paths[1].BranchDecisions[0].Concrete);
  EXPECT_FALSE(Paths[0].ConcreteComplete);
  EXPECT_FALSE(Paths[1].ConcreteComplete);
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

TEST(SymExplore, AConcolicBranchRecordsItsDecisionAndCompleteShadow) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Then = 1, Else = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Then), NdVar::reg(kFlag, 1)})},
          {Then, Else});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  BranchShadow Shadow(1);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_FALSE(Exploration.Complete)
      << "one concrete trace is not an exhaustive symbolic exploration";
  EXPECT_EQ(Path.Blocks, (std::vector<int>{0, Then}));
  EXPECT_TRUE(Path.ConcreteComplete);
  EXPECT_EQ(Path.ConcreteBranches, 1u);
  ASSERT_EQ(Path.Constraints.size(), 1u);
  ASSERT_EQ(Path.BranchDecisions.size(), 1u);
  const SymBranchDecision &Decision = Path.BranchDecisions.front();
  EXPECT_EQ(Decision.Condition, Path.Constraints.front());
  EXPECT_TRUE(Decision.Taken);
  EXPECT_EQ(Decision.ConstraintPrefix, 0u);
  EXPECT_TRUE(Decision.Concrete);
  const SymRef PositiveCondition = Decision.Condition;

  BranchShadow OtherShadow(0);
  Opts.Concolic = &OtherShadow;
  Exploration = explorePathsDetailed(Ctx, B.function(), Opts);
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &OtherPath = Exploration.Paths.front();
  EXPECT_EQ(OtherPath.Blocks, (std::vector<int>{0, Else}));
  EXPECT_TRUE(OtherPath.ConcreteComplete);
  ASSERT_EQ(OtherPath.Constraints.size(), 1u);
  ASSERT_EQ(OtherPath.BranchDecisions.size(), 1u);
  const SymBranchDecision &OtherDecision = OtherPath.BranchDecisions.front();
  EXPECT_EQ(OtherDecision.Condition, PositiveCondition);
  EXPECT_FALSE(OtherDecision.Taken);
  EXPECT_EQ(OtherDecision.ConstraintPrefix, 0u);
  EXPECT_TRUE(OtherDecision.Concrete);
  EXPECT_EQ(OtherPath.Constraints.front(), Ctx.mkNot(OtherDecision.Condition));
}

TEST(SymExplore, ARejectedShadowResetDoesNotReceiveSeeds) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  RejectingResetShadow Shadow;
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  Opts.ConcolicSeed.push_back({kRax, 8, 1});
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  EXPECT_EQ(Shadow.SeedCalls, 0u);
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  EXPECT_FALSE(Exploration.Paths.front().ConcreteComplete);
}

TEST(SymExplore, ALoopGivesEachConcreteDecisionAStableInvocation) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Header = 0, Body = 1, Exit = 2;
  LowOp Terminator =
      op(NdOp::COND_BR, NdVar{},
         {FunctionBuilder::addressOf(Body), NdVar::reg(kFlag, 1)});
  Terminator.Addr = kBase;
  Terminator.Seq = 7;
  B.block({compareRax(2), Terminator}, {Body, Exit});
  B.block({op(NdOp::INT_ADD, NdVar::reg(kRax, 8),
              {NdVar::reg(kRax, 8), NdVar::cst(1, 8)}),
           op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Header)})},
          {Header});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  SymExecConcreteShadow Shadow;
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  Opts.ConcolicSeed.push_back({kRax, 8, 0});
  Opts.MaxLoopIterations = 4;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::Returned);
  EXPECT_TRUE(Path.ConcreteComplete);
  EXPECT_FALSE(Exploration.Complete);
  ASSERT_EQ(Path.BranchDecisions.size(), 3u);
  for (unsigned Invocation = 0; Invocation < 3; ++Invocation) {
    const SymDecisionOccurrence &Occurrence =
        Path.BranchDecisions[Invocation].Occurrence;
    EXPECT_EQ(Occurrence.VA, kBase);
    EXPECT_EQ(Occurrence.Seq, 7);
    EXPECT_EQ(Occurrence.BlockId, Header);
    EXPECT_EQ(Occurrence.OpIndex, 1u);
    EXPECT_EQ(Occurrence.Invocation, Invocation);
    EXPECT_EQ(Occurrence.Kind, SymDecisionKind::ConditionalBranch);
  }
  EXPECT_TRUE(Path.BranchDecisions[0].Taken);
  EXPECT_TRUE(Path.BranchDecisions[1].Taken);
  EXPECT_FALSE(Path.BranchDecisions[2].Taken);

  llvm::SmallVector<uint32_t, 2> Variables;
  Ctx.collectVars(Path.BranchDecisions.front().Condition, Variables);
  ASSERT_EQ(Variables.size(), 1u);
  const SymVarInfo &Input = Ctx.varInfo(Variables.front());
  ASSERT_TRUE(Input.InputOrigin.has_value());
  EXPECT_EQ(*Input.InputOrigin,
            (SymInputOrigin{SymInputKind::Register, kRax, 8, 0}))
      << "the concrete seed belongs only to the private shadow state";
}

TEST(SymExplore, AConstantOccurrenceStillAdvancesTheDecisionInvocation) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Header = 1, Body = 2, Exit = 3;
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 1), {NdVar::cst(1, 1)}),
           op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Header)})},
          {Header});
  LowOp Terminator =
      op(NdOp::COND_BR, NdVar{},
         {FunctionBuilder::addressOf(Body), NdVar::reg(kRbx, 1)});
  Terminator.Addr = kBase + kBlockSize;
  Terminator.Seq = 9;
  B.block({Terminator}, {Body, Exit});
  B.block({compareRax(1),
           op(NdOp::COPY, NdVar::reg(kRbx, 1), {NdVar::reg(kFlag, 1)}),
           op(NdOp::BRANCH, NdVar{}, {FunctionBuilder::addressOf(Header)})},
          {Header});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  SymExecConcreteShadow Shadow;
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  Opts.ConcolicSeed.push_back({kRax, 8, 1});
  Opts.MaxLoopIterations = 3;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  ASSERT_EQ(Path.BranchDecisions.size(), 1u)
      << "the first, constant occurrence has no decision record";
  EXPECT_EQ(Path.BranchDecisions.front().Occurrence.Invocation, 1u);
  EXPECT_FALSE(Path.BranchDecisions.front().Taken);
  EXPECT_TRUE(Path.ConcreteComplete);
}

TEST(SymExplore, BranchDecisionsPointToTheirConstraintPrefixes) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int FirstTaken = 1, FirstOther = 2;
  constexpr int SecondTaken = 3, SecondOther = 4;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(FirstTaken), NdVar::reg(kFlag, 1)})},
          {FirstTaken, FirstOther});
  B.block({compareRax(20),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(SecondTaken), NdVar::reg(kFlag, 1)})},
          {SecondTaken, SecondOther});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  const auto Deep = std::find_if(Paths.begin(), Paths.end(), [](const auto &P) {
    return P.Blocks == std::vector<int>({0, FirstTaken, SecondTaken});
  });
  ASSERT_NE(Deep, Paths.end());
  ASSERT_EQ(Deep->Constraints.size(), 2u);
  ASSERT_EQ(Deep->BranchDecisions.size(), 2u);
  EXPECT_EQ(Deep->BranchDecisions[0].ConstraintPrefix, 0u);
  EXPECT_EQ(Deep->BranchDecisions[1].ConstraintPrefix, 1u);
  EXPECT_EQ(Deep->BranchDecisions[0].Condition, Deep->Constraints[0]);
  EXPECT_EQ(Deep->BranchDecisions[1].Condition, Deep->Constraints[1]);
}

TEST(SymExplore, LosingTheShadowBeforeABranchMarksFallbackPathsIncomplete) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Then = 1, Else = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Then), NdVar::reg(kFlag, 1)})},
          {Then, Else});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  BranchShadow Shadow(1, 0);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 2u);
  for (const SymPath &Path : Exploration.Paths) {
    EXPECT_FALSE(Path.ConcreteComplete);
    EXPECT_EQ(Path.ConcreteBranches, 0u);
    ASSERT_EQ(Path.BranchDecisions.size(), 1u);
    EXPECT_FALSE(Path.BranchDecisions.front().Concrete);
  }
}

TEST(SymExplore, MissingConcreteBranchValueMarksFallbackPathsIncomplete) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Then = 1, Else = 2;
  B.block({compareRax(10),
           op(NdOp::COND_BR, NdVar{},
              {FunctionBuilder::addressOf(Then), NdVar::reg(kFlag, 1)})},
          {Then, Else});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  BranchShadow Shadow(std::nullopt);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 2u);
  for (const SymPath &Path : Exploration.Paths) {
    EXPECT_FALSE(Path.ConcreteComplete);
    EXPECT_EQ(Path.ConcreteBranches, 0u);
    ASSERT_EQ(Path.BranchDecisions.size(), 1u);
    EXPECT_FALSE(Path.BranchDecisions.front().Concrete);
  }
}

TEST(SymExplore, PredicatedDirectCallExecutesOnlyOnThePredicatePath) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr uint64_t Callee = 0x500000;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(9, 8)}),
       op(NdOp::CALL, NdVar{}, {NdVar::cst(Callee, 8)})},
      {1}, LowInstructionControl::ConditionalCall,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Call |
          LowInstructionControlFlag::InstructionGuard,
      Callee);
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  ExploreOptions Opts;
  Opts.CallPreservedRegisters.push_back({kRbx, 8});
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 2u);
  EXPECT_EQ(Exploration.Paths[0].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Exploration.Paths[0].UnmodelledOps, 0u)
      << "the guard-taken path skips the call";
  EXPECT_EQ(Exploration.Paths[1].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Exploration.Paths[1].UnmodelledOps, 1u)
      << "the guard-false path executes the call";
  ASSERT_EQ(Exploration.Paths[0].Constraints.size(), 1u);
  ASSERT_EQ(Exploration.Paths[1].Constraints.size(), 1u);
  EXPECT_EQ(Ctx.mkNot(Exploration.Paths[0].Constraints[0]),
            Exploration.Paths[1].Constraints[0]);

  SymRef Skipped = Exploration.Paths[0].State.read(SymSpace::Register, kRbx, 8);
  SymRef Called = Exploration.Paths[1].State.read(SymSpace::Register, kRbx, 8);
  EXPECT_FALSE(Ctx.isConst(Skipped));
  ASSERT_TRUE(Ctx.isConst(Called));
  EXPECT_EQ(Ctx.constValue(Called).getZExtValue(), 9u);
}

TEST(SymExplore, PredicatedNoReturnCallEndsOnlyItsExecutingPath) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr uint64_t Callee = 0x500000;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::CALL, NdVar{}, {NdVar::cst(Callee, 8)})},
      {1}, LowInstructionControl::ConditionalCall,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Call |
          LowInstructionControlFlag::NoReturn |
          LowInstructionControlFlag::InstructionGuard,
      Callee);
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_EQ(countOutcome(Paths, PathOutcome::Returned), 1u);
  EXPECT_EQ(countOutcome(Paths, PathOutcome::LeftFunction), 1u);
  const auto CallPath =
      std::find_if(Paths.begin(), Paths.end(), [](const SymPath &Path) {
        return Path.Outcome == PathOutcome::LeftFunction;
      });
  ASSERT_NE(CallPath, Paths.end());
  ASSERT_TRUE(CallPath->Target.isValid());
  ASSERT_TRUE(Ctx.isConst(CallPath->Target));
  EXPECT_EQ(Ctx.constValue(CallPath->Target).getZExtValue(), Callee);
  EXPECT_EQ(CallPath->Blocks, std::vector<int>{0});
}

TEST(SymExplore, PredicatedIndirectCallPreservesBothTruthPaths) {
  SymContext Ctx;
  FunctionBuilder B;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(11, 8)}),
       op(NdOp::INDIR_CALL, NdVar{}, {NdVar::reg(kRax, 8)})},
      {1}, LowInstructionControl::ConditionalCall,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Call |
          LowInstructionControlFlag::Indirect |
          LowInstructionControlFlag::InstructionGuard);
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  ExploreOptions Opts;
  Opts.CallPreservedRegisters.push_back({kRbx, 8});
  std::vector<SymPath> Paths = explorePaths(Ctx, B.function(), Opts);

  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_EQ(Paths[0].UnmodelledOps, 0u);
  EXPECT_EQ(Paths[1].UnmodelledOps, 1u);
  SymRef Called = Paths[1].State.read(SymSpace::Register, kRbx, 8);
  ASSERT_TRUE(Ctx.isConst(Called));
  EXPECT_EQ(Ctx.constValue(Called).getZExtValue(), 11u);
}

TEST(SymExplore, PredicatedIndirectBranchExecutesItsFalseGuardContinuation) {
  SymContext Ctx;
  FunctionBuilder B;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::COPY, NdVar::tmp(0, 8), {FunctionBuilder::addressOf(2)}),
       op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(0, 8)})},
      {1, 2}, LowInstructionControl::Branch,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Indirect |
          LowInstructionControlFlag::InstructionGuard);
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(1, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(2, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 2u);
  ASSERT_EQ(Paths[0].Blocks, (std::vector<int>{0, 1}));
  ASSERT_EQ(Paths[1].Blocks, (std::vector<int>{0, 2}));
  for (size_t I = 0; I < Paths.size(); ++I) {
    SymRef Value = Paths[I].State.read(SymSpace::Register, kRbx, 8);
    ASSERT_TRUE(Ctx.isConst(Value));
    EXPECT_EQ(Ctx.constValue(Value).getZExtValue(), I + 1);
  }
}

TEST(SymExplore, PredicatedUnresolvedBranchIsReportedOnlyOnItsExecutingPath) {
  SymContext Ctx;
  FunctionBuilder B;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(kRax, 8)})},
      {1}, LowInstructionControl::Branch,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Indirect |
          LowInstructionControlFlag::InstructionGuard);
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_EQ(Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Paths[0].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Paths[1].Outcome, PathOutcome::UnresolvedBranch);
  EXPECT_EQ(Paths[1].Blocks, (std::vector<int>{0}));
}

TEST(SymExplore, PredicatedReturnDoesNotEraseItsSkipPath) {
  SymContext Ctx;
  FunctionBuilder B;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::RETURN, NdVar{}, {})},
      {1}, LowInstructionControl::ConditionalReturn,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Return |
          LowInstructionControlFlag::InstructionGuard);
  B.block({op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(7, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 2u);
  EXPECT_EQ(Paths[0].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Paths[1].Blocks, (std::vector<int>{0}));
  EXPECT_EQ(Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Paths[1].Outcome, PathOutcome::Returned);
  SymRef Continued = Paths[0].State.read(SymSpace::Register, kRbx, 8);
  ASSERT_TRUE(Ctx.isConst(Continued));
  EXPECT_EQ(Ctx.constValue(Continued).getZExtValue(), 7u);
}

TEST(SymExplore, MalformedInstructionGuardMetadataFailsClosed) {
  SymContext Ctx;
  FunctionBuilder B;
  B.predicatedInstruction(
      {op(NdOp::COND_BR, NdVar{},
          {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}),
       op(NdOp::RETURN, NdVar{}, {})},
      {1}, LowInstructionControl::ConditionalReturn,
      LowInstructionControlFlag::Branch |
          LowInstructionControlFlag::Conditional |
          LowInstructionControlFlag::Return);
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function());
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  EXPECT_EQ(Exploration.Paths.front().Outcome,
            PathOutcome::InvalidControlTarget);
  EXPECT_FALSE(Exploration.Complete);
}

TEST(SymExplore, PredicatedMemoryEffectsSkipExecuteAndForkWhenUnknown) {
  SymContext Ctx;
  auto Walk = [&](std::optional<uint64_t> GuardValue) {
    FunctionBuilder B;
    std::vector<LowOp> Ops{
        op(NdOp::COPY, NdVar::reg(kRax, 4), {NdVar::cst(7, 4)})};
    if (GuardValue)
      Ops.push_back(
          op(NdOp::COPY, NdVar::reg(kFlag, 1), {NdVar::cst(*GuardValue, 1)}));
    Ops.push_back(op(NdOp::COND_BR, NdVar{},
                     {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)}));
    Ops.push_back(op(NdOp::LOAD, NdVar::reg(kRax, 4),
                     {NdVar::cst(0, 8), NdVar::cst(0x9010, 8)}));
    Ops.push_back(
        op(NdOp::STORE, NdVar{},
           {NdVar::cst(0, 8), NdVar::cst(0x9020, 8), NdVar::cst(42, 4)}));
    B.instruction(std::move(Ops), {1}, InstructionMode::ARM,
                  LowInstructionControl::Branch,
                  LowInstructionControlFlag::Branch |
                      LowInstructionControlFlag::Conditional |
                      LowInstructionControlFlag::InstructionGuard,
                  LowInstructionTargetMode::Preserve, kBase + kBlockSize);
    B.block({op(NdOp::LOAD, NdVar::reg(kRbx, 4),
                {NdVar::cst(0, 8), NdVar::cst(0x9020, 8)}),
             op(NdOp::RETURN, NdVar{}, {})},
            {});
    return explorePaths(Ctx, B.function());
  };

  std::vector<SymPath> Skipped = Walk(1);
  ASSERT_EQ(Skipped.size(), 1u);
  SymRef SkippedLoad = Skipped.front().State.read(SymSpace::Register, kRax, 4);
  SymRef SkippedStore = Skipped.front().State.read(SymSpace::Register, kRbx, 4);
  ASSERT_TRUE(Ctx.isConst(SkippedLoad));
  EXPECT_EQ(Ctx.constValue(SkippedLoad).getZExtValue(), 7u);
  EXPECT_FALSE(Ctx.isConst(SkippedStore));

  std::vector<SymPath> Executed = Walk(0);
  ASSERT_EQ(Executed.size(), 1u);
  SymRef ExecutedLoad =
      Executed.front().State.read(SymSpace::Register, kRax, 4);
  SymRef ExecutedStore =
      Executed.front().State.read(SymSpace::Register, kRbx, 4);
  EXPECT_FALSE(Ctx.isConst(ExecutedLoad));
  ASSERT_TRUE(Ctx.isConst(ExecutedStore));
  EXPECT_EQ(Ctx.constValue(ExecutedStore).getZExtValue(), 42u);

  std::vector<SymPath> Unknown = Walk(std::nullopt);
  ASSERT_EQ(Unknown.size(), 2u);
  unsigned Stored = 0;
  unsigned NotStored = 0;
  for (SymPath &Path : Unknown) {
    SymRef Value = Path.State.read(SymSpace::Register, kRbx, 4);
    if (Ctx.isConst(Value) && Ctx.constValue(Value).getZExtValue() == 42)
      ++Stored;
    else
      ++NotStored;
  }
  EXPECT_EQ(Stored, 1u);
  EXPECT_EQ(NotStored, 1u);
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

TEST(SymExplore, ExecutedOperationTraceStopsAtTheStepBudget) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::COPY, NdVar::reg(kRax, 8), {NdVar::cst(1, 8)}),
           op(NdOp::COPY, NdVar::reg(kRbx, 8), {NdVar::cst(2, 8)}),
           op(NdOp::RETURN, NdVar{}, {})},
          {});
  ExploreOptions Opts;
  Opts.MaxSteps = 1;

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::StepBudget);
  ASSERT_EQ(Path.ExecutedOps.size(), 1u);
  EXPECT_EQ(Path.ExecutedOps.front().BlockId, 0);
  EXPECT_EQ(Path.ExecutedOps.front().OpIndex, 0u);
  EXPECT_EQ(Path.ExecutedOps.front().Seq, 0);
  EXPECT_EQ(Path.ExecutedOps.front().Opcode, NdOp::COPY);
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
  EXPECT_TRUE(Paths[0].BranchDecisions.empty());
}

TEST(SymExplore, ALoopWithNoKnownTripCountStopsAtItsBound) {
  SymContext Ctx;
  FunctionBuilder B;
  buildCountingLoop(B);

  ExploreOptions Opts;
  Opts.MaxBlockVisits = 3;
  std::vector<SymPath> Paths = explorePaths(Ctx, B.function(), Opts);

  // Three trips round before conceding, and one exit found on each of them.
  ASSERT_FALSE(Paths.empty());
  EXPECT_EQ(countOutcome(Paths, PathOutcome::Returned), 3u);
  EXPECT_EQ(countOutcome(Paths, PathOutcome::LoopBudget), 1u);

  // Depth first, so the path that kept going round comes back first and the
  // exits follow in the reverse of the order they were passed: three trips
  // assumed, then two, then one.
  EXPECT_EQ(Paths.front().Outcome, PathOutcome::LoopBudget);
  EXPECT_EQ(Paths.front().Constraints.size(), 3u);
  EXPECT_EQ(Paths.back().Constraints.size(), 1u);
}

TEST(SymExplore, ADeeperLoopBoundReachesIterationsTheDefaultHides) {
  // The default is chosen to be fast, and a bound that is chosen to be fast is
  // a bound that is wrong about something.  Which iteration the interesting
  // thing happens on is not the analysis's decision to make, so the bound is
  // the caller's.
  SymContext Ctx;
  FunctionBuilder B;
  buildCountingLoop(B);

  auto exits = [&](const ExploreOptions &Opts) {
    return countOutcome(explorePaths(Ctx, B.function(), Opts),
                        PathOutcome::Returned);
  };

  EXPECT_EQ(exits(ExploreOptions{}), 3u);

  // Only the general per-block allowance is lifted.  The loop is still
  // bounded, and bounded by the number that says it is.
  ExploreOptions Deep;
  Deep.MaxBlockVisits = kUnbounded;
  Deep.MaxLoopIterations = 8;
  EXPECT_EQ(exits(Deep), 8u);
}

TEST(SymExplore, AWalkWithEveryBudgetLiftedStillStopsAtTheLoopBound) {
  // Every allowance gone at once.  What still ends this is structural: a cycle
  // has to pass through a loop header, and re-entering a header is what the
  // remaining bound counts.
  SymContext Ctx;
  FunctionBuilder B;
  buildCountingLoop(B);

  ExploreOptions Unbounded;
  Unbounded.MaxPaths = kUnbounded;
  Unbounded.MaxSteps = kUnbounded;
  Unbounded.MaxBlockVisits = kUnbounded;
  Unbounded.MaxLoopIterations = 4;

  SymExploration Exploration =
      explorePathsDetailed(Ctx, B.function(), Unbounded);
  EXPECT_EQ(countOutcome(Exploration.Paths, PathOutcome::Returned), 4u);
  EXPECT_EQ(countOutcome(Exploration.Paths, PathOutcome::LoopBudget), 1u);
  // And it says so, rather than handing back four exits as though that were
  // all of them.
  EXPECT_FALSE(Exploration.Complete);
}

TEST(SymExplore, PathsThatMeetHoldingTheSameValuesGoOnAsOne) {
  // Sixteen routes through four diamonds, and one thing that happens.  Every
  // arm leaves the state as it found it, so the two ways of arriving at each
  // head are the same way of arriving and only one of them needs walking on.
  SymContext Ctx;
  FunctionBuilder B;
  buildDiamondChain(B, 4);

  EXPECT_EQ(explorePaths(Ctx, B.function()).size(), 16u);

  ExploreOptions Merging;
  Merging.MergeEquivalentPaths = true;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Merging);
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  EXPECT_EQ(Exploration.Paths[0].Outcome, PathOutcome::Returned);
  // Sixteen routes, one reported and the other fifteen standing behind it.
  EXPECT_EQ(Exploration.Paths[0].MergedPaths, 15u);
  EXPECT_EQ(Exploration.MergedPaths, 15u);
  EXPECT_TRUE(Exploration.Complete);

  // Every choice was made both ways, so between them the paths assume nothing
  // — which is what the disjunction of the two arms of a branch comes to.
  EXPECT_EQ(Exploration.Paths[0].predicate(Ctx), Ctx.mkTrue());
  EXPECT_TRUE(Exploration.Paths[0].BranchDecisions.empty())
      << "a merged predicate has no single divergent decision history";
}

TEST(SymExplore, AJoinKeepsOnlyTheCommonConcreteDecisionPrefix) {
  SymContext Ctx;
  FunctionBuilder B;
  buildDiamondChain(B, 2);

  BranchShadow Shadow(1, 1);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  Opts.MergeEquivalentPaths = true;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_FALSE(Path.ConcreteComplete);
  EXPECT_EQ(Path.ConcreteBranches, 1u);
  ASSERT_EQ(Path.BranchDecisions.size(), 1u);
  EXPECT_TRUE(Path.BranchDecisions.front().Concrete);
  EXPECT_TRUE(Path.BranchDecisions.front().Taken);
  EXPECT_EQ(Path.BranchDecisions.front().ConstraintPrefix, 0u);
  ASSERT_FALSE(Path.Constraints.empty());
  EXPECT_EQ(Path.BranchDecisions.front().Condition, Path.Constraints.front());
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

TEST(SymExplore, MissingConcreteIndirectTargetMarksThePathIncomplete) {
  SymContext Ctx;
  FunctionBuilder B;
  B.block({op(NdOp::INT_MULT, NdVar::tmp(0, 8),
              {NdVar::reg(kRax, 8), NdVar::cst(8, 8)}),
           op(NdOp::INDIR_BR, NdVar{}, {NdVar::tmp(0, 8)})},
          {});

  BranchShadow Shadow(std::nullopt);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  EXPECT_EQ(Exploration.Paths.front().Outcome, PathOutcome::UnresolvedBranch);
  EXPECT_FALSE(Exploration.Paths.front().ConcreteComplete);
}

TEST(SymExplore, ConcreteIndirectTargetAddsTargetEqualityConstraint) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Destination = 1;
  B.block({op(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(kRax, 8)})}, {Destination});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  BranchShadow Shadow(FunctionBuilder::addressOf(Destination).Offset);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::Returned);
  EXPECT_EQ(Path.Blocks, (std::vector<int>{0, Destination}));
  EXPECT_TRUE(Path.ConcreteComplete);
  EXPECT_EQ(Path.ConcreteBranches, 1u);

  ASSERT_EQ(Path.Constraints.size(), 1u);
  SymRef SymbolicTarget =
      Path.State.read(SymSpace::Register, kRax, sizeof(uint64_t));
  SymRef TargetEquality =
      Ctx.mkEq(SymbolicTarget,
               Ctx.mkConst(Ctx.width(SymbolicTarget),
                           FunctionBuilder::addressOf(Destination).Offset));
  EXPECT_EQ(Path.Constraints.front(), TargetEquality);
  EXPECT_EQ(Path.predicate(Ctx), TargetEquality);

  ASSERT_EQ(Path.BranchDecisions.size(), 1u);
  const SymBranchDecision &Decision = Path.BranchDecisions.front();
  EXPECT_EQ(Decision.Condition, TargetEquality);
  EXPECT_TRUE(Decision.Taken);
  EXPECT_EQ(Decision.ConstraintPrefix, 0u);
  EXPECT_TRUE(Decision.Concrete);
  EXPECT_EQ(Decision.Occurrence.VA, 0u);
  EXPECT_EQ(Decision.Occurrence.Seq, 0);
  EXPECT_EQ(Decision.Occurrence.BlockId, 0);
  EXPECT_EQ(Decision.Occurrence.OpIndex, 0u);
  EXPECT_EQ(Decision.Occurrence.Invocation, 0u);
  EXPECT_EQ(Decision.Occurrence.Kind, SymDecisionKind::IndirectBranchTarget);
}

TEST(SymExplore, ContradictoryConcreteIndirectTargetIsInfeasible) {
  SymContext Ctx;
  FunctionBuilder B;
  constexpr int Destination = 1;
  const NdVar DestinationAddress = FunctionBuilder::addressOf(Destination);
  B.block({op(NdOp::CALL, NdVar{}, {NdVar::cst(0x401500, 8)}),
           op(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(kRax, 8)})},
          {Destination});
  B.block({op(NdOp::RETURN, NdVar{}, {})}, {});

  BranchShadow Shadow(DestinationAddress.Offset);
  ExploreOptions Opts;
  Opts.Concolic = &Shadow;
  Opts.CallPreservedRegisters.push_back({kRax, 8});
  Opts.CallEffects =
      [ConcreteTarget = DestinationAddress.Offset](
          SymContext &CallCtx, SymState &State, const LowOp &,
          const SymCallOccurrence &) -> std::optional<SymCallEffect> {
    SymRef SymbolicTarget =
        State.read(SymSpace::Register, kRax, sizeof(uint64_t));
    SymCallEffect Effect;
    Effect.Memory = SymCallMemoryEffect::Preserve;
    Effect.Constraints.push_back(CallCtx.mkNe(
        SymbolicTarget,
        CallCtx.mkConst(CallCtx.width(SymbolicTarget), ConcreteTarget)));
    return Effect;
  };
  SymExploration Exploration = explorePathsDetailed(Ctx, B.function(), Opts);

  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::Infeasible);
  EXPECT_EQ(Path.Blocks, (std::vector<int>{0}));
  EXPECT_EQ(Path.ConcreteBranches, 1u);
  ASSERT_EQ(Path.Constraints.size(), 2u);
  EXPECT_EQ(Path.predicate(Ctx), Ctx.mkFalse());
  ASSERT_EQ(Path.BranchDecisions.size(), 1u);
  EXPECT_EQ(Path.BranchDecisions.back().Condition, Path.Constraints.back());
  EXPECT_TRUE(Path.BranchDecisions.back().Taken);
  EXPECT_EQ(Path.BranchDecisions.back().ConstraintPrefix, 1u);
  EXPECT_TRUE(Path.BranchDecisions.back().Concrete);
}

TEST(SymExplore, OddInterworkingTargetFindsItsCanonicalThumbBlock) {
  SymContext Ctx;
  FunctionBuilder B;
  B.instruction(
      {op(NdOp::INDIR_BR, NdVar{}, {NdVar::cst(kBase + kBlockSize + 1, 8)})},
      {1}, InstructionMode::ARM, LowInstructionControl::Branch,
      LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect,
      LowInstructionTargetMode::FromTargetBit0);
  B.instruction({op(NdOp::RETURN, NdVar{}, {NdVar::cst(0x7001, 8)})}, {},
                InstructionMode::Thumb, LowInstructionControl::Return,
                LowInstructionControlFlag::Return,
                LowInstructionTargetMode::FromTargetBit0);

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function());
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::Returned);
  EXPECT_EQ(Path.Blocks, (std::vector<int>{0, 1}));
  ASSERT_TRUE(Path.SourceMode.has_value());
  EXPECT_EQ(*Path.SourceMode, InstructionMode::Thumb);
  ASSERT_TRUE(Path.DestinationMode.has_value());
  EXPECT_EQ(*Path.DestinationMode, InstructionMode::Thumb);
  ASSERT_TRUE(Path.Target.isValid());
  ASSERT_TRUE(Ctx.isConst(Path.Target));
  EXPECT_EQ(Ctx.constValue(Path.Target).getZExtValue(), 0x7000u);
  EXPECT_TRUE(Exploration.Complete);
}

TEST(SymExplore, InterworkingDoesNotEnterABlockDecodedInTheWrongMode) {
  SymContext Ctx;
  FunctionBuilder B;
  B.instruction(
      {op(NdOp::INDIR_BR, NdVar{}, {NdVar::cst(kBase + kBlockSize + 1, 8)})},
      {1}, InstructionMode::ARM, LowInstructionControl::Branch,
      LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect,
      LowInstructionTargetMode::FromTargetBit0);
  B.instruction({op(NdOp::RETURN, NdVar{}, {})}, {}, InstructionMode::ARM,
                LowInstructionControl::Return,
                LowInstructionControlFlag::Return);

  std::vector<SymPath> Paths = explorePaths(Ctx, B.function());
  ASSERT_EQ(Paths.size(), 1u);
  EXPECT_EQ(Paths.front().Outcome, PathOutcome::InvalidControlTarget);
  EXPECT_EQ(Paths.front().Blocks, (std::vector<int>{0}));
  ASSERT_TRUE(Paths.front().DestinationMode.has_value());
  EXPECT_EQ(*Paths.front().DestinationMode, InstructionMode::Thumb);
}

TEST(SymExplore, ReportsCanonicalModeForAnExternalOddTarget) {
  SymContext Ctx;
  FunctionBuilder B;
  B.instruction({op(NdOp::INDIR_BR, NdVar{}, {NdVar::cst(0x7001, 8)})}, {},
                InstructionMode::ARM, LowInstructionControl::Branch,
                LowInstructionControlFlag::Branch |
                    LowInstructionControlFlag::Indirect,
                LowInstructionTargetMode::FromTargetBit0);

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function());
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::LeftFunction);
  ASSERT_TRUE(Path.SourceMode.has_value());
  EXPECT_EQ(*Path.SourceMode, InstructionMode::ARM);
  ASSERT_TRUE(Path.DestinationMode.has_value());
  EXPECT_EQ(*Path.DestinationMode, InstructionMode::Thumb);
  ASSERT_TRUE(Ctx.isConst(Path.Target));
  EXPECT_EQ(Ctx.constValue(Path.Target).getZExtValue(), 0x7000u);
  EXPECT_TRUE(Exploration.Complete);
}

TEST(SymExplore, InvalidInterworkingTargetFailsClosed) {
  SymContext Ctx;
  FunctionBuilder B;
  B.instruction({op(NdOp::INDIR_BR, NdVar{}, {NdVar::cst(0x7002, 8)})}, {},
                InstructionMode::ARM, LowInstructionControl::Branch,
                LowInstructionControlFlag::Branch |
                    LowInstructionControlFlag::Indirect,
                LowInstructionTargetMode::FromTargetBit0);

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function());
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::InvalidControlTarget);
  ASSERT_TRUE(Path.SourceMode.has_value());
  EXPECT_EQ(*Path.SourceMode, InstructionMode::ARM);
  ASSERT_TRUE(Path.DestinationMode.has_value());
  EXPECT_EQ(*Path.DestinationMode, InstructionMode::ARM);
  ASSERT_TRUE(Ctx.isConst(Path.Target));
  EXPECT_EQ(Ctx.constValue(Path.Target).getZExtValue(), 0x7002u);
  EXPECT_FALSE(Exploration.Complete);
}

TEST(SymExplore, BoundaryAtZeroVirtualAddressRemainsAuthoritative) {
  SymContext Ctx;
  LowFunc Func;
  Func.Entry = 0;

  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0;
  Entry.EndAddr = 4;
  Entry.Ops = {op(NdOp::BRANCH, NdVar{}, {NdVar::cst(4, 8)})};
  Entry.Ops.front().Addr = 0;
  Entry.InstructionBoundaries.push_back(
      {0, 4, 0, 1, InstructionMode::ARM, LowInstructionControl::Branch,
       LowInstructionControlFlag::Branch, LowInstructionTargetMode::Preserve,
       4});
  Entry.Succs = {1};
  Func.Blocks.push_back(std::move(Entry));

  LowBlock Exit;
  Exit.Id = 1;
  Exit.StartAddr = 4;
  Exit.EndAddr = 8;
  Exit.Ops = {op(NdOp::RETURN, NdVar{}, {})};
  Exit.Ops.front().Addr = 4;
  Exit.InstructionBoundaries.push_back(
      {4, 4, 0, 1, InstructionMode::ARM, LowInstructionControl::Return,
       LowInstructionControlFlag::Return, LowInstructionTargetMode::Preserve,
       std::nullopt});
  Func.Blocks.push_back(std::move(Exit));

  std::vector<SymPath> Paths = explorePaths(Ctx, Func);
  ASSERT_EQ(Paths.size(), 1u);
  EXPECT_EQ(Paths.front().Outcome, PathOutcome::Returned);
  EXPECT_EQ(Paths.front().Blocks, (std::vector<int>{0, 1}));
  ASSERT_TRUE(Paths.front().SourceMode.has_value());
  EXPECT_EQ(*Paths.front().SourceMode, InstructionMode::ARM);
}

TEST(SymExplore, SequentialFallthroughRejectsAWrongModeSuccessor) {
  SymContext Ctx;
  FunctionBuilder B;
  B.instruction({op(NdOp::COPY, NdVar::reg(kRax, 4), {NdVar::cst(1, 4)})}, {1},
                InstructionMode::ARM, LowInstructionControl::None,
                LowInstructionControlFlag::None);
  B.instruction({op(NdOp::RETURN, NdVar{}, {})}, {}, InstructionMode::Thumb,
                LowInstructionControl::Return,
                LowInstructionControlFlag::Return);

  SymExploration Exploration = explorePathsDetailed(Ctx, B.function());
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  const SymPath &Path = Exploration.Paths.front();
  EXPECT_EQ(Path.Outcome, PathOutcome::InvalidControlTarget);
  EXPECT_EQ(Path.Blocks, (std::vector<int>{0}));
  ASSERT_TRUE(Path.SourceMode.has_value());
  EXPECT_EQ(*Path.SourceMode, InstructionMode::ARM);
  ASSERT_TRUE(Path.DestinationMode.has_value());
  EXPECT_EQ(*Path.DestinationMode, InstructionMode::ARM);
  EXPECT_FALSE(Exploration.Complete);
}

TEST(SymExplore,
     UnknownConditionalIsolatesAWrongModeFallthroughWithinPathBudget) {
  SymContext Ctx;
  FunctionBuilder B;
  B.instruction({op(NdOp::COND_BR, NdVar{},
                    {FunctionBuilder::addressOf(1), NdVar::reg(kFlag, 1)})},
                {1, 2}, InstructionMode::ARM, LowInstructionControl::Branch,
                LowInstructionControlFlag::Branch |
                    LowInstructionControlFlag::Conditional,
                LowInstructionTargetMode::Preserve, kBase + kBlockSize);
  B.instruction({op(NdOp::RETURN, NdVar{}, {})}, {}, InstructionMode::ARM,
                LowInstructionControl::Return,
                LowInstructionControlFlag::Return);
  B.instruction({op(NdOp::RETURN, NdVar{}, {})}, {}, InstructionMode::Thumb,
                LowInstructionControl::Return,
                LowInstructionControlFlag::Return);

  SymExploration Full = explorePathsDetailed(Ctx, B.function());
  ASSERT_EQ(Full.Paths.size(), 2u);
  EXPECT_EQ(Full.Paths[0].Outcome, PathOutcome::Returned);
  EXPECT_EQ(Full.Paths[0].Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Full.Paths[1].Outcome, PathOutcome::InvalidControlTarget);
  EXPECT_EQ(Full.Paths[1].Blocks, (std::vector<int>{0}));
  ASSERT_TRUE(Full.Paths[1].SourceMode.has_value());
  EXPECT_EQ(*Full.Paths[1].SourceMode, InstructionMode::ARM);
  ASSERT_TRUE(Full.Paths[1].DestinationMode.has_value());
  EXPECT_EQ(*Full.Paths[1].DestinationMode, InstructionMode::ARM);
  EXPECT_EQ(Full.ReachablePaths, 2u);
  EXPECT_FALSE(Full.Complete);

  ExploreOptions LimitedOptions;
  LimitedOptions.MaxPaths = 1;
  SymExploration Limited =
      explorePathsDetailed(Ctx, B.function(), LimitedOptions);
  ASSERT_EQ(Limited.Paths.size(), 1u);
  EXPECT_EQ(Limited.Paths.front().Outcome, PathOutcome::Returned);
  EXPECT_EQ(Limited.Paths.front().Blocks, (std::vector<int>{0, 1}));
  EXPECT_EQ(Limited.ReachablePaths, 1u);
  EXPECT_FALSE(Limited.Complete)
      << "the unvisited invalid fallthrough remains a reachable frontier";
}

TEST(SymExplore, MixedModeBoundaryBlockFailsClosedBeforeExecution) {
  SymContext Ctx;
  LowFunc Func;
  Func.Entry = kBase;

  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = kBase;
  Block.EndAddr = kBase + 8;
  Block.Ops = {op(NdOp::NOP, NdVar{}, {}), op(NdOp::RETURN, NdVar{}, {})};
  Block.Ops[0].Addr = kBase;
  Block.Ops[1].Addr = kBase + 4;
  Block.InstructionBoundaries = {
      {kBase, 4, 0, 1, InstructionMode::ARM, LowInstructionControl::None,
       LowInstructionControlFlag::None, LowInstructionTargetMode::Preserve,
       std::nullopt},
      {kBase + 4, 4, 1, 1, InstructionMode::Thumb,
       LowInstructionControl::Return, LowInstructionControlFlag::Return,
       LowInstructionTargetMode::Preserve, std::nullopt},
  };
  Func.Blocks.push_back(std::move(Block));

  SymExploration Exploration = explorePathsDetailed(Ctx, Func);
  ASSERT_EQ(Exploration.Paths.size(), 1u);
  EXPECT_EQ(Exploration.Paths.front().Outcome,
            PathOutcome::InvalidControlTarget);
  EXPECT_TRUE(Exploration.Paths.front().Blocks.empty());
  ASSERT_TRUE(Exploration.Paths.front().SourceMode.has_value());
  EXPECT_EQ(*Exploration.Paths.front().SourceMode, InstructionMode::ARM);
  EXPECT_FALSE(Exploration.Complete);
}

TEST(SymExplore, TheNumberOfPathsIsBounded) {
  // Four diamonds in a row is sixteen paths.  Asking for five gets five, which
  // is the only reason a walk of anything real terminates.
  SymContext Ctx;
  FunctionBuilder B;
  buildDiamondChain(B, 4);

  ExploreOptions Generous;
  Generous.MaxPaths = 64;
  EXPECT_EQ(explorePaths(Ctx, B.function(), Generous).size(), 16u);

  ExploreOptions Tight;
  Tight.MaxPaths = 5;
  EXPECT_EQ(explorePaths(Ctx, B.function(), Tight).size(), 5u);
}

} // namespace
