//===- LowIRConcolicTests.cpp - LowIR concolic branch flips ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LowIRConcolicDetail.h"
#include "gtest/gtest.h"

#include "neverd/concolic/LowIRConcolic.h"
#include "neverd/symbolic/SymConcrete.h"

#include "llvm/ADT/APInt.h"

#include <limits>

using namespace neverd;
using namespace neverd::concolic;
using namespace neverd::symbolic;

namespace {

constexpr va_t kBase = 0x400000;
constexpr uint64_t kRax = 0;
constexpr uint64_t kFlag = 64;

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
         va_t Address, int Seq) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.Addr = Address;
  Result.Seq = Seq;
  for (const NdVar &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

LowFunc oneBranchFunction() {
  LowFunc Function;
  Function.Entry = kBase;
  Function.Name = "one_branch";

  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = kBase;
  Entry.EndAddr = kBase + 4;
  Entry.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
         {NdVar::reg(kRax, 8), NdVar::cst(10, 8)}, kBase, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 4, 8), NdVar::reg(kFlag, 1)}, kBase, 1),
  };
  Entry.Succs = {1, 2};

  LowBlock Taken;
  Taken.Id = 1;
  Taken.StartAddr = kBase + 4;
  Taken.EndAddr = kBase + 8;
  Taken.Ops = {op(NdOp::RETURN, NdVar{}, {}, kBase + 4, 0)};

  LowBlock Other;
  Other.Id = 2;
  Other.StartAddr = kBase + 8;
  Other.EndAddr = kBase + 12;
  Other.Ops = {op(NdOp::RETURN, NdVar{}, {}, kBase + 8, 0)};

  Function.Blocks = {std::move(Entry), std::move(Taken), std::move(Other)};
  return Function;
}

LowBlock returnedBlock(int Id, va_t Address) {
  LowBlock Block;
  Block.Id = Id;
  Block.StartAddr = Address;
  Block.EndAddr = Address + 4;
  Block.Ops = {op(NdOp::RETURN, NdVar{}, {}, Address, 0)};
  return Block;
}

LowFunc twoBranchFunction() {
  LowFunc Function;
  Function.Entry = kBase;
  Function.Name = "two_branch";

  LowBlock First;
  First.Id = 0;
  First.StartAddr = kBase;
  First.EndAddr = kBase + 4;
  First.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
         {NdVar::reg(kRax, 8), NdVar::cst(10, 8)}, kBase, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 4, 8), NdVar::reg(kFlag, 1)}, kBase, 1),
  };
  First.Succs = {1, 2};

  LowBlock Second;
  Second.Id = 1;
  Second.StartAddr = kBase + 4;
  Second.EndAddr = kBase + 8;
  Second.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag + 1, 1),
         {NdVar::reg(kRax, 8), NdVar::cst(20, 8)}, kBase + 4, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 12, 8), NdVar::reg(kFlag + 1, 1)}, kBase + 4, 1),
  };
  Second.Succs = {3, 4};

  Function.Blocks = {std::move(First), std::move(Second),
                     returnedBlock(2, kBase + 8), returnedBlock(3, kBase + 12),
                     returnedBlock(4, kBase + 16)};
  return Function;
}

LowFunc impossibleBranchFunction() {
  LowFunc Function = oneBranchFunction();
  Function.Name = "impossible_branch";
  LowBlock &Entry = Function.Blocks.front();
  Entry.Ops = {
      op(NdOp::INT_MULT, NdVar::tmp(0, 8),
         {NdVar::reg(kRax, 8), NdVar::cst(2, 8)}, kBase, 0),
      op(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
         {NdVar::tmp(0, 8), NdVar::cst(1, 8)}, kBase, 1),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 4, 8), NdVar::reg(kFlag, 1)}, kBase, 2),
  };
  return Function;
}

LowFunc byteBranchFunction() {
  LowFunc Function = oneBranchFunction();
  Function.Name = "byte_branch";
  LowBlock &Entry = Function.Blocks.front();
  Entry.Ops = {
      op(NdOp::INT_EQUAL, NdVar::reg(kFlag, 1),
         {NdVar::reg(kRax + 1, 1), NdVar::cst(0xab, 1)}, kBase, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 4, 8), NdVar::reg(kFlag, 1)}, kBase, 1),
  };
  return Function;
}

LowFunc suffixIncompleteFunction() {
  LowFunc Function;
  Function.Entry = kBase;
  Function.Name = "suffix_incomplete";

  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = kBase;
  Entry.EndAddr = kBase + 4;
  Entry.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
         {NdVar::reg(kRax, 8), NdVar::cst(10, 8)}, kBase, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 4, 8), NdVar::reg(kFlag, 1)}, kBase, 1),
  };
  Entry.Succs = {1, 2};

  LowBlock Unseeded;
  Unseeded.Id = 2;
  Unseeded.StartAddr = kBase + 8;
  Unseeded.EndAddr = kBase + 12;
  Unseeded.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag + 1, 1),
         {NdVar::reg(kRax + 8, 8), NdVar::cst(10, 8)}, kBase + 8, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 12, 8), NdVar::reg(kFlag + 1, 1)}, kBase + 8, 1),
  };
  Unseeded.Succs = {3, 4};

  Function.Blocks = {std::move(Entry), returnedBlock(1, kBase + 4),
                     std::move(Unseeded), returnedBlock(3, kBase + 12),
                     returnedBlock(4, kBase + 16)};
  return Function;
}

LowFunc countingLoopFunction() {
  LowFunc Function;
  Function.Entry = kBase;
  Function.Name = "counting_loop";

  LowBlock Header;
  Header.Id = 0;
  Header.StartAddr = kBase;
  Header.EndAddr = kBase + 4;
  Header.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
         {NdVar::reg(kRax, 8), NdVar::cst(2, 8)}, kBase, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 4, 8), NdVar::reg(kFlag, 1)}, kBase, 1),
  };
  Header.Succs = {1, 2};

  LowBlock Body;
  Body.Id = 1;
  Body.StartAddr = kBase + 4;
  Body.EndAddr = kBase + 8;
  Body.Ops = {
      op(NdOp::INT_ADD, NdVar::reg(kRax, 8),
         {NdVar::reg(kRax, 8), NdVar::cst(1, 8)}, kBase + 4, 0),
      op(NdOp::BRANCH, NdVar{}, {NdVar::cst(kBase, 8)}, kBase + 4, 1),
  };
  Body.Succs = {0};

  Function.Blocks = {std::move(Header), std::move(Body),
                     returnedBlock(2, kBase + 8)};
  return Function;
}

LowFunc callHavocFunction() {
  LowFunc Function;
  Function.Entry = kBase;
  Function.Name = "call_havoc";
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = kBase;
  Entry.EndAddr = kBase + 4;
  Entry.Ops = {
      op(NdOp::CALL, NdVar{}, {NdVar::cst(0x401500, 8)}, kBase, 0),
      op(NdOp::RETURN, NdVar{}, {}, kBase, 1),
  };
  Function.Blocks.push_back(std::move(Entry));
  return Function;
}

LowFunc indirectPrefixFunction() {
  constexpr uint64_t kRbx = kRax + 8;
  LowFunc Function;
  Function.Entry = kBase;
  Function.Name = "indirect_prefix";

  LowBlock Dispatch;
  Dispatch.Id = 0;
  Dispatch.StartAddr = kBase;
  Dispatch.EndAddr = kBase + 4;
  Dispatch.Ops = {op(NdOp::INDIR_BR, NdVar{}, {NdVar::reg(kRax, 8)}, kBase, 0)};
  Dispatch.Succs = {1};

  LowBlock Guard;
  Guard.Id = 1;
  Guard.StartAddr = kBase + 4;
  Guard.EndAddr = kBase + 8;
  Guard.Ops = {
      op(NdOp::INT_LESS, NdVar::reg(kFlag, 1),
         {NdVar::reg(kRbx, 8), NdVar::cst(10, 8)}, kBase + 4, 0),
      op(NdOp::COND_BR, NdVar{},
         {NdVar::cst(kBase + 8, 8), NdVar::reg(kFlag, 1)}, kBase + 4, 1),
  };
  Guard.Succs = {2, 3};

  Function.Blocks = {std::move(Dispatch), std::move(Guard),
                     returnedBlock(2, kBase + 8), returnedBlock(3, kBase + 12)};
  return Function;
}

std::vector<uint64_t> seedValues(const std::vector<SymConcreteRegister> &Seed) {
  std::vector<uint64_t> Result;
  Result.reserve(Seed.size() * 3);
  for (const SymConcreteRegister &Range : Seed) {
    Result.push_back(Range.Offset);
    Result.push_back(Range.Bytes);
    Result.push_back(Range.Value);
  }
  return Result;
}

TEST(LowIRConcolic, ProductionDefaultsBoundEverySolverResource) {
  const LowIRConcolicOptions Options;
  EXPECT_EQ(Options.Solver.Sat.MaxConflicts, uint64_t{1} << 18);
  EXPECT_EQ(Options.Solver.Sat.MaxPropagations, uint64_t{1} << 24);
  EXPECT_EQ(Options.Solver.Sat.MaxWatchVisits, uint64_t{1} << 26);
  EXPECT_EQ(Options.Solver.Blast.MaxWidth, 256u);
  EXPECT_EQ(Options.Solver.Blast.MaxGates, size_t{1} << 22);
}

TEST(LowIRConcolic, UnsupportedCallEffectsHaveASpecificTraceReason) {
  const LowIRConcolicReport Report = runLowIRConcolic(callHavocFunction(), {});
  EXPECT_FALSE(Report.TraceComplete);
  EXPECT_FALSE(Report.TraceExact);
  EXPECT_EQ(Report.TraceReason, ConcolicTraceReason::UnsupportedEffects);
  EXPECT_GT(Report.CallHavocs, 0u);
  EXPECT_TRUE(Report.Flips.empty());
}

TEST(LowIRConcolic, MissingRequiredSeedIsConcreteIncompleteNotUnsupported) {
  const LowIRConcolicReport Report = runLowIRConcolic(oneBranchFunction(), {});
  EXPECT_FALSE(Report.TraceComplete);
  EXPECT_FALSE(Report.TraceExact);
  EXPECT_EQ(Report.TraceReason, ConcolicTraceReason::IncompleteConcreteTrace);
  EXPECT_EQ(Report.UnmodelledOps, 0u);
  EXPECT_EQ(Report.OpaqueOps, 0u);
  EXPECT_EQ(Report.CallHavocs, 0u);
  EXPECT_EQ(Report.MemoryHavocs, 0u);
  EXPECT_TRUE(Report.Flips.empty());
}

TEST(LowIRConcolic, OneRegisterBranchProducesAReplayVerifiedFlip) {
  const LowFunc Function = oneBranchFunction();
  LowIRConcolicOptions Options;
  Options.InitialSeed.push_back({kRax, 8, 3});

  const LowIRConcolicReport Report = runLowIRConcolic(Function, Options);

  ASSERT_TRUE(Report.TraceComplete);
  ASSERT_TRUE(Report.TraceExact);
  EXPECT_FALSE(Report.Exhaustive);
  ASSERT_EQ(Report.Decisions.size(), 1u);
  ASSERT_EQ(Report.Flips.size(), 1u);
  const LowIRConcolicFlip &Flip = Report.Flips.front();
  EXPECT_EQ(Flip.Status, ConcolicFlipStatus::Verified);
  ASSERT_TRUE(Flip.SolverResult.has_value());
  EXPECT_EQ(*Flip.SolverResult, solver::SatResult::Sat);
  EXPECT_EQ(Flip.ProjectionReason, ConcolicProjectionReason::None);
  EXPECT_EQ(Flip.ReplayReason, ConcolicReplayReason::None);
  ASSERT_TRUE(Flip.CandidateIndex.has_value());
  ASSERT_LT(*Flip.CandidateIndex, Report.Candidates.size());

  // Verify the published seed through a new context and concrete shadow.  The
  // core's replay receipt is useful only if a consumer can independently
  // reproduce the exact occurrence and opposite polarity.
  SymContext ReplayContext;
  SymExecConcreteShadow ReplayShadow;
  ExploreOptions ReplayOptions;
  ReplayOptions.Concolic = &ReplayShadow;
  ReplayOptions.ConcolicSeed = Report.Candidates[*Flip.CandidateIndex].Seed;
  SymExploration Replay =
      explorePathsDetailed(ReplayContext, Function, ReplayOptions);
  ASSERT_EQ(Replay.Paths.size(), 1u);
  ASSERT_EQ(Replay.Paths.front().BranchDecisions.size(), 1u);
  const SymBranchDecision &Replayed =
      Replay.Paths.front().BranchDecisions.front();
  EXPECT_TRUE(Replayed.Concrete);
  EXPECT_EQ(Replayed.Occurrence, Report.Decisions.front().Occurrence);
  EXPECT_NE(Replayed.Taken, Report.Decisions.front().Taken);
}

TEST(LowIRConcolic, NegatesEachDecisionUnderItsExactEarlierPrefix) {
  LowIRConcolicOptions Options;
  Options.InitialSeed.push_back({kRax, 8, 3});

  const LowIRConcolicReport Report =
      runLowIRConcolic(twoBranchFunction(), Options);

  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Decisions.size(), 2u);
  EXPECT_EQ(Report.Decisions[0].ConstraintPrefix, 0u);
  EXPECT_EQ(Report.Decisions[1].ConstraintPrefix, 1u);
  ASSERT_EQ(Report.Flips.size(), 2u);
  EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::Verified);
  EXPECT_EQ(Report.Flips[1].Status, ConcolicFlipStatus::Unsat)
      << "the second opposite (rax >= 20) contradicts the exact rax < 10 "
         "prefix";
  ASSERT_TRUE(Report.Flips[1].SolverResult.has_value());
  EXPECT_EQ(*Report.Flips[1].SolverResult, solver::SatResult::Unsat);
}

TEST(LowIRConcolic, IndirectTargetEqualityRemainsInLaterFlipPrefixes) {
  constexpr uint64_t kRbx = kRax + 8;
  LowIRConcolicOptions Options;
  Options.InitialSeed = {{kRax, 8, kBase + 4}, {kRbx, 8, 3}};
  const LowIRConcolicReport Report =
      runLowIRConcolic(indirectPrefixFunction(), Options);

  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Decisions.size(), 2u);
  EXPECT_EQ(Report.Decisions[0].Occurrence.Kind,
            SymDecisionKind::IndirectBranchTarget);
  EXPECT_EQ(Report.Decisions[1].Occurrence.Kind,
            SymDecisionKind::ConditionalBranch);
  EXPECT_EQ(Report.Decisions[1].ConstraintPrefix, 1u);
  ASSERT_EQ(Report.Flips.size(), 1u);
  EXPECT_EQ(Report.Flips[0].DecisionIndex, 1u);
  EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::Verified);
  ASSERT_EQ(Report.Candidates.size(), 1u);
  ASSERT_EQ(Report.Candidates[0].Seed.size(), 2u);
  EXPECT_EQ(Report.Candidates[0].Seed[0].Offset, kRax);
  EXPECT_EQ(Report.Candidates[0].Seed[0].Value, kBase + 4)
      << "the concrete indirect target is part of the exact prefix";
}

TEST(LowIRConcolic, KeepsUnsatAndUnknownTypedAndCandidateFree) {
  LowIRConcolicOptions UnsatOptions;
  UnsatOptions.InitialSeed.push_back({kRax, 8, 0});
  const LowIRConcolicReport Unsat =
      runLowIRConcolic(impossibleBranchFunction(), UnsatOptions);
  ASSERT_TRUE(Unsat.TraceExact);
  ASSERT_EQ(Unsat.Flips.size(), 1u);
  EXPECT_EQ(Unsat.Flips[0].Status, ConcolicFlipStatus::Unsat);
  EXPECT_TRUE(Unsat.Candidates.empty());

  LowIRConcolicOptions UnknownOptions;
  UnknownOptions.InitialSeed.push_back({kRax, 8, 3});
  UnknownOptions.Solver.Blast.MaxWidth = 4;
  const LowIRConcolicReport Unknown =
      runLowIRConcolic(oneBranchFunction(), UnknownOptions);
  ASSERT_TRUE(Unknown.TraceExact);
  ASSERT_EQ(Unknown.Flips.size(), 1u);
  EXPECT_EQ(Unknown.Flips[0].Status, ConcolicFlipStatus::SolverUnknown);
  ASSERT_TRUE(Unknown.Flips[0].SolverResult.has_value());
  EXPECT_EQ(*Unknown.Flips[0].SolverResult, solver::SatResult::Unknown);
  EXPECT_EQ(Unknown.Flips[0].EncodingError, solver::BlastError::WidthTooLarge);
  EXPECT_TRUE(Unknown.Candidates.empty());
}

TEST(LowIRConcolic, RejectsInvalidOverlappingAndOutOfRangeSeeds) {
  auto Rejects = [](std::vector<SymConcreteRegister> Seed) {
    LowIRConcolicOptions Options;
    Options.InitialSeed = std::move(Seed);
    const LowIRConcolicReport Report =
        runLowIRConcolic(oneBranchFunction(), Options);
    EXPECT_FALSE(Report.TraceComplete);
    EXPECT_FALSE(Report.TraceExact);
    EXPECT_EQ(Report.TraceReason, ConcolicTraceReason::InvalidInitialSeed);
    EXPECT_TRUE(Report.Flips.empty());
  };

  Rejects({{kRax, 0, 0}});
  Rejects({{kRax, 9, 0}});
  Rejects({{std::numeric_limits<uint64_t>::max(), 2, 0}});
  Rejects({{kRax, 2, 0}, {kRax + 1, 1, 0}});
  Rejects({{kRax, 1, 0x100}});
}

TEST(LowIRConcolic, PreservesBaselineBytesAndRangeShapeInBothEndiannesses) {
  auto Check = [](llvm::endianness Order, uint64_t Initial,
                  uint64_t PreservedMask, uint64_t PreservedValue) {
    LowIRConcolicOptions Options;
    Options.ByteOrder = Order;
    Options.InitialSeed.push_back({kRax, 2, Initial});
    const LowIRConcolicReport Report =
        runLowIRConcolic(byteBranchFunction(), Options);
    ASSERT_TRUE(Report.TraceExact);
    ASSERT_EQ(Report.Candidates.size(), 1u);
    ASSERT_EQ(Report.Candidates[0].Seed.size(), 1u);
    const SymConcreteRegister &Candidate = Report.Candidates[0].Seed[0];
    EXPECT_EQ(Candidate.Offset, kRax);
    EXPECT_EQ(Candidate.Bytes, 2u);
    EXPECT_EQ(Candidate.Value & PreservedMask, PreservedValue);
    EXPECT_NE(Candidate.Value, Initial);
  };

  // Both values describe address bytes [0x03, 0xab]. Only address byte one is
  // in the query, so address byte zero must survive projection.
  Check(llvm::endianness::little, 0xab03, 0x00ff, 0x0003);
  Check(llvm::endianness::big, 0x03ab, 0xff00, 0x0300);

  LowIRConcolicOptions Split;
  Split.InitialSeed = {{kRax + 1, 1, 0xab}, {kRax, 1, 3}};
  const LowIRConcolicReport SplitReport =
      runLowIRConcolic(byteBranchFunction(), Split);
  ASSERT_EQ(SplitReport.Candidates.size(), 1u);
  ASSERT_EQ(SplitReport.Candidates[0].Seed.size(), 2u);
  EXPECT_EQ(SplitReport.Candidates[0].Seed[0].Offset, kRax);
  EXPECT_EQ(SplitReport.Candidates[0].Seed[0].Value, 3u);
  EXPECT_EQ(SplitReport.Candidates[0].Seed[1].Offset, kRax + 1);
}

TEST(LowIRConcolic, CallerCannotDisableTheRequiredProjectionModel) {
  LowIRConcolicOptions Options;
  Options.InitialSeed.push_back({kRax, 8, 3});
  Options.Solver.BuildModel = false;
  const LowIRConcolicReport Report =
      runLowIRConcolic(oneBranchFunction(), Options);
  ASSERT_EQ(Report.Flips.size(), 1u);
  EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::Verified);
}

TEST(LowIRConcolicDetail, RejectsEveryUnsupportedProjectionProvenance) {
  const std::vector<SymConcreteRegister> Baseline = {{0, 2, 0x0102}};
  auto Project = [&](SymContext &Ctx, SymRef Query,
                     const solver::BitVectorModel &Model) {
    return detail::projectRegisterModel(Ctx, Query, Model, Baseline,
                                        llvm::endianness::little)
        .Reason;
  };

  {
    SymContext Ctx;
    solver::BitVectorModel Model;
    EXPECT_EQ(Project(Ctx, SymRef(), Model),
              ConcolicProjectionReason::InvalidQuery);
    EXPECT_EQ(Project(Ctx, Ctx.mkConst(8, 1), Model),
              ConcolicProjectionReason::InvalidQuery);
  }
  {
    SymContext Ctx;
    SymRef X = Ctx.mkFreshVar(8, "fresh");
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::FreshVariable);
  }
  {
    SymContext Ctx;
    SymRef X = Ctx.mkVar("plain", 8);
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::MissingInputOrigin);
  }
  {
    SymContext Ctx;
    SymRef X =
        Ctx.mkInputVar("temporary", 8, {SymInputKind::Temporary, 0, 1, 0});
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::UnsupportedInputKind);
  }
  {
    SymContext Ctx;
    SymRef X = Ctx.mkInputVar("epoch", 8, {SymInputKind::Register, 0, 1, 1});
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::NonzeroInputEpoch);
  }
  {
    SymContext Ctx;
    SymRef X =
        Ctx.mkInputVar("bad_width", 8, {SymInputKind::Register, 0, 2, 0});
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::InvalidInputWidth);
  }
  for (const SymInputOrigin Origin : {
           SymInputOrigin{SymInputKind::Register, 0, 0, 0},
           SymInputOrigin{SymInputKind::Register,
                          std::numeric_limits<uint64_t>::max(), 2, 0},
       }) {
    SymContext Ctx;
    SymRef X = Ctx.mkInputVar("bad_range", 8, Origin);
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::InvalidInputWidth);
  }
  {
    SymContext Ctx;
    SymRef X =
        Ctx.mkInputVar("missing_model", 8, {SymInputKind::Register, 0, 1, 0});
    solver::BitVectorModel Model;
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::MissingModelValue);
  }
  {
    SymContext Ctx;
    SymRef X = Ctx.mkInputVar("missing_baseline", 8,
                              {SymInputKind::Register, 2, 1, 0});
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(X), llvm::APInt(8, 1));
    EXPECT_EQ(Project(Ctx, Ctx.mkEq(X, Ctx.mkOne(8)), Model),
              ConcolicProjectionReason::MissingBaselineByte);
  }
}

TEST(LowIRConcolicDetail, RejectsOverlappingOriginsAndInvalidWitnesses) {
  const std::vector<SymConcreteRegister> Baseline = {{0, 2, 0}};
  {
    SymContext Ctx;
    SymRef Word = Ctx.mkInputVar("word", 16, {SymInputKind::Register, 0, 2, 0});
    SymRef Byte = Ctx.mkInputVar("byte", 8, {SymInputKind::Register, 1, 1, 1});
    // Simulate inconsistent metadata received from a producer that bypassed
    // SymContext's byte canonicalisation.  The projection boundary must still
    // reject overlapping epoch-zero origins rather than trusting them.
    auto &ByteInfo = const_cast<SymVarInfo &>(Ctx.varInfo(Ctx.varId(Byte)));
    ASSERT_TRUE(ByteInfo.InputOrigin.has_value());
    ByteInfo.InputOrigin->Epoch = 0;
    SymRef Query = Ctx.mkAnd(Ctx.mkEq(Word, Ctx.mkZero(16)),
                             Ctx.mkEq(Byte, Ctx.mkZero(8)));
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(Word), llvm::APInt(16, 0));
    Model.set(Ctx.varId(Byte), llvm::APInt(8, 0));
    EXPECT_EQ(detail::projectRegisterModel(Ctx, Query, Model, Baseline,
                                           llvm::endianness::little)
                  .Reason,
              ConcolicProjectionReason::OverlappingInputOrigins);
  }
  {
    SymContext Ctx;
    SymRef X = Ctx.mkInputVar("witness", 8, {SymInputKind::Register, 0, 1, 0});
    SymRef Query = Ctx.mkEq(X, Ctx.mkOne(8));
    solver::BitVectorModel Wrong;
    Wrong.set(Ctx.varId(X), llvm::APInt(8, 0));
    EXPECT_EQ(detail::projectRegisterModel(Ctx, Query, Wrong, Baseline,
                                           llvm::endianness::little)
                  .Reason,
              ConcolicProjectionReason::CandidateDoesNotSatisfyQuery);
  }
}

TEST(LowIRConcolicDetail, ReplayChecksExactPrefixOccurrenceAndPolarity) {
  const SymDecisionOccurrence FirstOccurrence{
      kBase, 1, 0, 1, 0, SymDecisionKind::ConditionalBranch};
  const SymDecisionOccurrence TargetOccurrence{
      kBase + 4, 1, 1, 1, 0, SymDecisionKind::ConditionalBranch};
  const std::vector<LowIRConcolicDecision> Original = {
      {0, FirstOccurrence, true, 0, true},
      {1, TargetOccurrence, true, 1, true},
  };
  std::vector<LowIRConcolicDecision> Replay = Original;
  Replay[1].Taken = false;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::None);

  EXPECT_EQ(detail::compareReplayDecisions({}, Original, 1),
            ConcolicReplayReason::EarlierDecisionMissing);

  Replay = Original;
  Replay[0].Concrete = false;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::EarlierDecisionNotConcrete);

  Replay = Original;
  ++Replay[0].Occurrence.Invocation;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::EarlierOccurrenceMismatch);

  Replay = Original;
  Replay[0].Taken = false;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::EarlierPolarityMismatch);

  Replay = Original;
  ++Replay[0].ConstraintPrefix;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::EarlierConstraintPrefixMismatch);

  Replay.assign(Original.begin(), Original.begin() + 1);
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::TargetDecisionMissing);

  Replay = Original;
  Replay[1].Taken = false;
  Replay[1].Concrete = false;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::TargetDecisionNotConcrete);

  Replay = Original;
  Replay[1].Taken = false;
  ++Replay[1].Occurrence.Invocation;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::TargetOccurrenceMismatch);

  Replay = Original;
  Replay[1].Taken = false;
  ++Replay[1].ConstraintPrefix;
  EXPECT_EQ(detail::compareReplayDecisions(Replay, Original, 1),
            ConcolicReplayReason::TargetConstraintPrefixMismatch);

  EXPECT_EQ(detail::compareReplayDecisions(Original, Original, 1),
            ConcolicReplayReason::TargetPolarityNotFlipped);
}

TEST(LowIRConcolicDetail, ReplayVerifiedDeduplicationPrecedesCandidateBudget) {
  std::vector<LowIRConcolicCandidate> Published;
  detail::CandidatePublication First =
      detail::publishReplayVerifiedSeed(Published, {{0, 1, 7}}, 1);
  EXPECT_EQ(First.Status, ConcolicFlipStatus::Verified);
  ASSERT_EQ(First.CandidateIndex, 0u);
  ASSERT_EQ(Published.size(), 1u);

  detail::CandidatePublication Duplicate =
      detail::publishReplayVerifiedSeed(Published, {{0, 1, 7}}, 1);
  EXPECT_EQ(Duplicate.Status, ConcolicFlipStatus::VerifiedDuplicate);
  ASSERT_EQ(Duplicate.CandidateIndex, 0u);
  EXPECT_EQ(Published.size(), 1u);

  detail::CandidatePublication Full =
      detail::publishReplayVerifiedSeed(Published, {{0, 1, 8}}, 1);
  EXPECT_EQ(Full.Status, ConcolicFlipStatus::CandidateBudgetExceeded);
  EXPECT_FALSE(Full.CandidateIndex.has_value());
  EXPECT_EQ(Published.size(), 1u);
}

TEST(LowIRConcolicDetail, PrefixEncodingFailuresDistinguishLimitsFromBugs) {
  EXPECT_EQ(
      detail::classifyPrefixEncodingFailure(solver::BlastError::WidthTooLarge),
      ConcolicFlipStatus::SolverUnknown);
  EXPECT_EQ(
      detail::classifyPrefixEncodingFailure(solver::BlastError::TooManyGates),
      ConcolicFlipStatus::SolverUnknown);
  EXPECT_EQ(
      detail::classifyPrefixEncodingFailure(solver::BlastError::Malformed),
      ConcolicFlipStatus::InvalidQuery);
  EXPECT_EQ(detail::classifyPrefixEncodingFailure(solver::BlastError::None),
            ConcolicFlipStatus::InvalidQuery);
}

TEST(LowIRConcolic, AcceptsReplayThatBecomesIncompleteOnlyAfterTheTarget) {
  LowIRConcolicOptions Options;
  Options.InitialSeed.push_back({kRax, 8, 3});
  const LowIRConcolicReport Report =
      runLowIRConcolic(suffixIncompleteFunction(), Options);

  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Flips.size(), 1u);
  EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::Verified);
  EXPECT_EQ(Report.Flips[0].ReplayReason, ConcolicReplayReason::None);
  ASSERT_EQ(Report.Candidates.size(), 1u);

  // Confirm this is genuinely a suffix-incomplete candidate rather than a
  // fully concrete replay that happened to pass the same check.
  SymContext Ctx;
  SymExecConcreteShadow Shadow;
  ExploreOptions ReplayOptions;
  ReplayOptions.MaxPaths = 1;
  ReplayOptions.Concolic = &Shadow;
  ReplayOptions.ConcolicSeed = Report.Candidates[0].Seed;
  const SymExploration Replay =
      explorePathsDetailed(Ctx, suffixIncompleteFunction(), ReplayOptions);
  ASSERT_EQ(Replay.Paths.size(), 1u);
  EXPECT_FALSE(Replay.Paths[0].ConcreteComplete);
  ASSERT_FALSE(Replay.Paths[0].BranchDecisions.empty());
  EXPECT_TRUE(Replay.Paths[0].BranchDecisions[0].Concrete);
  EXPECT_NE(Replay.Paths[0].BranchDecisions[0].Taken,
            Report.Decisions[0].Taken);
}

TEST(LowIRConcolic, LoopDecisionsUsePhysicalInvocationIdentity) {
  LowIRConcolicOptions Options;
  Options.InitialSeed.push_back({kRax, 8, 0});
  const LowIRConcolicReport Report =
      runLowIRConcolic(countingLoopFunction(), Options);

  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Decisions.size(), 3u);
  for (unsigned I = 0; I < 3; ++I) {
    EXPECT_EQ(Report.Decisions[I].Occurrence.VA, kBase);
    EXPECT_EQ(Report.Decisions[I].Occurrence.Seq, 1);
    EXPECT_EQ(Report.Decisions[I].Occurrence.BlockId, 0);
    EXPECT_EQ(Report.Decisions[I].Occurrence.OpIndex, 1u);
    EXPECT_EQ(Report.Decisions[I].Occurrence.Invocation, I);
  }
  ASSERT_EQ(Report.Flips.size(), 3u);
  EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::Verified);
  EXPECT_EQ(Report.Flips[1].Status, ConcolicFlipStatus::Verified);
  EXPECT_EQ(Report.Flips[2].Status, ConcolicFlipStatus::Unsat);
}

TEST(LowIRConcolic, AttemptAndCandidateBudgetsHaveTypedNotRunStates) {
  LowIRConcolicOptions AttemptLimited;
  AttemptLimited.InitialSeed.push_back({kRax, 8, 3});
  AttemptLimited.MaxFlipAttempts = 1;
  const LowIRConcolicReport Attempts =
      runLowIRConcolic(twoBranchFunction(), AttemptLimited);
  ASSERT_EQ(Attempts.Flips.size(), 2u);
  EXPECT_EQ(Attempts.FlipAttempts, 1u);
  EXPECT_TRUE(Attempts.FlipBudgetHit);
  EXPECT_EQ(Attempts.Flips[1].Status,
            ConcolicFlipStatus::AttemptBudgetExceeded);
  EXPECT_FALSE(Attempts.Flips[1].SolverResult.has_value());

  LowIRConcolicOptions CandidateLimited;
  CandidateLimited.InitialSeed.push_back({kRax, 8, 3});
  CandidateLimited.MaxCandidates = 0;
  const LowIRConcolicReport Candidates =
      runLowIRConcolic(oneBranchFunction(), CandidateLimited);
  ASSERT_EQ(Candidates.Flips.size(), 1u);
  EXPECT_EQ(Candidates.Flips[0].Status,
            ConcolicFlipStatus::CandidateBudgetExceeded);
  EXPECT_TRUE(Candidates.Flips[0].SolverResult.has_value());
  EXPECT_EQ(Candidates.Flips[0].ReplayReason, ConcolicReplayReason::None);
  EXPECT_TRUE(Candidates.CandidateBudgetHit);
  EXPECT_TRUE(Candidates.Candidates.empty());
}

TEST(LowIRConcolic, ReportsAreDeterministicAndOwnTheirResults) {
  LowIRConcolicOptions Options;
  Options.InitialSeed = {{kRax + 1, 1, 0xab}, {kRax, 1, 3}};
  const LowIRConcolicReport First =
      runLowIRConcolic(byteBranchFunction(), Options);
  const LowIRConcolicReport Second =
      runLowIRConcolic(byteBranchFunction(), Options);

  ASSERT_EQ(First.Version, Second.Version);
  EXPECT_EQ(First.FunctionName, Second.FunctionName);
  EXPECT_EQ(seedValues(First.InitialSeed), seedValues(Second.InitialSeed));
  EXPECT_EQ(First.Blocks, Second.Blocks);
  ASSERT_EQ(First.Decisions.size(), Second.Decisions.size());
  ASSERT_EQ(First.Flips.size(), Second.Flips.size());
  ASSERT_EQ(First.Candidates.size(), Second.Candidates.size());
  for (size_t I = 0; I < First.Decisions.size(); ++I) {
    EXPECT_EQ(First.Decisions[I].Occurrence, Second.Decisions[I].Occurrence);
    EXPECT_EQ(First.Decisions[I].Taken, Second.Decisions[I].Taken);
    EXPECT_EQ(First.Decisions[I].ConstraintPrefix,
              Second.Decisions[I].ConstraintPrefix);
  }
  for (size_t I = 0; I < First.Flips.size(); ++I) {
    EXPECT_EQ(First.Flips[I].Status, Second.Flips[I].Status);
    EXPECT_EQ(First.Flips[I].SolverResult, Second.Flips[I].SolverResult);
    EXPECT_EQ(First.Flips[I].CandidateIndex, Second.Flips[I].CandidateIndex);
  }
  for (size_t I = 0; I < First.Candidates.size(); ++I)
    EXPECT_EQ(seedValues(First.Candidates[I].Seed),
              seedValues(Second.Candidates[I].Seed));

  const LowIRConcolicReport Owned = [&] {
    LowFunc Temporary = byteBranchFunction();
    Temporary.Name = "destroyed_after_run";
    return runLowIRConcolic(Temporary, Options);
  }();
  EXPECT_EQ(Owned.FunctionName, "destroyed_after_run");
  EXPECT_EQ(Owned.Blocks, std::vector<int>({0, 1}));
  ASSERT_FALSE(Owned.Decisions.empty());
  ASSERT_FALSE(Owned.Candidates.empty());
  EXPECT_FALSE(seedValues(Owned.Candidates[0].Seed).empty());
}

} // namespace
