//===- LowIRConcolicIntegrationTests.cpp - Native format matrix ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Loads and lifts the same dependency-free branch from ELF, Mach-O, and PE
/// images for x86-64 and AArch64.  Every matrix entry is mandatory: a verified
/// candidate must flip value 0 to the unique satisfying value 7, then survive
/// an independent concrete replay at the same decision occurrence.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/concolic/LowIRConcolic.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/symbolic/SymConcrete.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef CONCOLIC_FIXTURE_ROOT
#error "CONCOLIC_FIXTURE_ROOT must name the checked-in concolic fixtures"
#endif

using namespace neverd;
using namespace neverd::concolic;
using namespace neverd::symbolic;

namespace {

struct FixtureSpec {
  const char *TestName;
  const char *FileName;
  BinaryFormat Format;
  Arch Architecture;
  uint64_t InputRegister;
};

void PrintTo(const FixtureSpec &Spec, std::ostream *Out) {
  *Out << Spec.TestName;
}

struct FunctionIdentity {
  va_t Address = InvalidVA;
  std::string Name;
};

std::filesystem::path fixturePath(const FixtureSpec &Spec) {
  return std::filesystem::path(CONCOLIC_FIXTURE_ROOT) / Spec.FileName;
}

bool isFixtureName(llvm::StringRef Name) {
  return stripLeadingUnderscores(Name) == "concolic_branch";
}

std::optional<FunctionIdentity> findFixtureFunction(const BinaryImage &Image) {
  std::set<va_t> MatchingAddresses;
  for (const Symbol &Candidate : Image.Symbols)
    if (Candidate.IsFunc && isFixtureName(Candidate.Name))
      MatchingAddresses.insert(Candidate.Addr);
  for (const Export &Candidate : Image.Exports)
    if (isFixtureName(Candidate.Name))
      MatchingAddresses.insert(Candidate.Addr);
  if (MatchingAddresses.size() != 1)
    return std::nullopt;
  return FunctionIdentity{*MatchingAddresses.begin(), "concolic_branch"};
}

bool hasForbiddenEffect(const LowFunc &Function) {
  for (const LowBlock &Block : Function.Blocks) {
    for (const LowOp &Op : Block.Ops) {
      switch (Op.Opcode) {
      case NdOp::LOAD:
      case NdOp::STORE:
      case NdOp::ATOMIC_XCHG:
      case NdOp::ATOMIC_ADD:
      case NdOp::ATOMIC_CMPXCHG:
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::INTRINSIC:
        return true;
      default:
        break;
      }
    }
  }
  return false;
}

size_t countOpcode(const LowFunc &Function, NdOp Opcode) {
  size_t Count = 0;
  for (const LowBlock &Block : Function.Blocks)
    Count += static_cast<size_t>(
        std::count_if(Block.Ops.begin(), Block.Ops.end(),
                      [&](const LowOp &Op) { return Op.Opcode == Opcode; }));
  return Count;
}

void expectSameSeed(llvm::ArrayRef<SymConcreteRegister> Left,
                    llvm::ArrayRef<SymConcreteRegister> Right) {
  ASSERT_EQ(Left.size(), Right.size());
  for (size_t I = 0; I < Left.size(); ++I) {
    EXPECT_EQ(Left[I].Offset, Right[I].Offset);
    EXPECT_EQ(Left[I].Bytes, Right[I].Bytes);
    EXPECT_EQ(Left[I].Value, Right[I].Value);
  }
}

void expectDeterministicReport(const LowIRConcolicReport &Left,
                               const LowIRConcolicReport &Right) {
  EXPECT_EQ(Left.Version, Right.Version);
  EXPECT_EQ(Left.FunctionEntry, Right.FunctionEntry);
  EXPECT_EQ(Left.FunctionName, Right.FunctionName);
  expectSameSeed(Left.InitialSeed, Right.InitialSeed);
  EXPECT_EQ(Left.TraceOutcome, Right.TraceOutcome);
  EXPECT_EQ(Left.LiftComplete, Right.LiftComplete);
  EXPECT_EQ(Left.TraceComplete, Right.TraceComplete);
  EXPECT_EQ(Left.TraceExact, Right.TraceExact);
  EXPECT_EQ(Left.Exhaustive, Right.Exhaustive);
  EXPECT_EQ(Left.TraceReason, Right.TraceReason);
  EXPECT_EQ(Left.ExecutedSteps, Right.ExecutedSteps);
  EXPECT_EQ(Left.UnmodelledOps, Right.UnmodelledOps);
  EXPECT_EQ(Left.OpaqueOps, Right.OpaqueOps);
  EXPECT_EQ(Left.CallHavocs, Right.CallHavocs);
  EXPECT_EQ(Left.MemoryHavocs, Right.MemoryHavocs);
  EXPECT_EQ(Left.Blocks, Right.Blocks);
  EXPECT_EQ(Left.FlipAttempts, Right.FlipAttempts);
  EXPECT_EQ(Left.FlipBudgetHit, Right.FlipBudgetHit);
  EXPECT_EQ(Left.CandidateBudgetHit, Right.CandidateBudgetHit);

  ASSERT_EQ(Left.Decisions.size(), Right.Decisions.size());
  for (size_t I = 0; I < Left.Decisions.size(); ++I) {
    EXPECT_EQ(Left.Decisions[I].Index, Right.Decisions[I].Index);
    EXPECT_EQ(Left.Decisions[I].Occurrence, Right.Decisions[I].Occurrence);
    EXPECT_EQ(Left.Decisions[I].Taken, Right.Decisions[I].Taken);
    EXPECT_EQ(Left.Decisions[I].ConstraintPrefix,
              Right.Decisions[I].ConstraintPrefix);
    EXPECT_EQ(Left.Decisions[I].Concrete, Right.Decisions[I].Concrete);
  }

  ASSERT_EQ(Left.Flips.size(), Right.Flips.size());
  for (size_t I = 0; I < Left.Flips.size(); ++I) {
    EXPECT_EQ(Left.Flips[I].DecisionIndex, Right.Flips[I].DecisionIndex);
    EXPECT_EQ(Left.Flips[I].Occurrence, Right.Flips[I].Occurrence);
    EXPECT_EQ(Left.Flips[I].OriginalTaken, Right.Flips[I].OriginalTaken);
    EXPECT_EQ(Left.Flips[I].ConstraintPrefix, Right.Flips[I].ConstraintPrefix);
    EXPECT_EQ(Left.Flips[I].Status, Right.Flips[I].Status);
    EXPECT_EQ(Left.Flips[I].SolverResult, Right.Flips[I].SolverResult);
    EXPECT_EQ(Left.Flips[I].EncodingError, Right.Flips[I].EncodingError);
    EXPECT_EQ(Left.Flips[I].ProjectionReason, Right.Flips[I].ProjectionReason);
    EXPECT_EQ(Left.Flips[I].ReplayReason, Right.Flips[I].ReplayReason);
    EXPECT_EQ(Left.Flips[I].CandidateIndex, Right.Flips[I].CandidateIndex);
  }

  ASSERT_EQ(Left.Candidates.size(), Right.Candidates.size());
  for (size_t I = 0; I < Left.Candidates.size(); ++I)
    expectSameSeed(Left.Candidates[I].Seed, Right.Candidates[I].Seed);
}

class LowIRConcolicIntegration : public ::testing::TestWithParam<FixtureSpec> {
};

TEST_P(LowIRConcolicIntegration,
       FindsUniqueVerifiedFlipAndReplaysExactOccurrence) {
  const FixtureSpec &Spec = GetParam();
  const std::filesystem::path Path = fixturePath(Spec);
  ASSERT_TRUE(std::filesystem::is_regular_file(Path)) << Path;

  llvm::Expected<BinaryImage> Loaded = loadBinary(Path);
  ASSERT_TRUE(static_cast<bool>(Loaded)) << llvm::toString(Loaded.takeError());
  BinaryImage Image = std::move(*Loaded);
  ASSERT_EQ(Image.Format, Spec.Format);
  ASSERT_EQ(Image.Arch, Spec.Architecture);
  ASSERT_EQ(Image.Bits, Bitness::Bits64);
  ASSERT_TRUE(Image.Imports.empty());

  const std::optional<FunctionIdentity> Identity = findFixtureFunction(Image);
  ASSERT_TRUE(Identity.has_value());
  ASSERT_NE(Identity->Address, InvalidVA);

  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image.Arch, Image.Mode));
  CFGBuilder Builder;
  const LowFunc Function =
      Builder.build(Image, Decode, Identity->Address, Identity->Name);

  ASSERT_EQ(Function.Entry, Identity->Address);
  ASSERT_FALSE(Function.Blocks.empty());
  ASSERT_TRUE(Function.hasCompleteInstructionLift());
  ASSERT_TRUE(Function.TruncatedPathAddresses.empty());
  ASSERT_EQ(Function.DecodedInstructionCount, Function.LiftedInstructionCount);
  ASSERT_EQ(countOpcode(Function, NdOp::COND_BR), 1u);
  ASSERT_GE(countOpcode(Function, NdOp::RETURN), 1u);
  ASSERT_FALSE(hasForbiddenEffect(Function));

  LowIRConcolicOptions Options;
  Options.ByteOrder = llvm::endianness::little;
  Options.InitialSeed.push_back({Spec.InputRegister, 4, 0});

  const LowIRConcolicReport Report = runLowIRConcolic(Function, Options);
  ASSERT_EQ(Report.TraceReason, ConcolicTraceReason::None);
  ASSERT_EQ(Report.TraceOutcome, PathOutcome::Returned);
  ASSERT_TRUE(Report.LiftComplete);
  ASSERT_TRUE(Report.TraceComplete);
  ASSERT_TRUE(Report.TraceExact);
  ASSERT_FALSE(Report.Exhaustive);
  ASSERT_GT(Report.ExecutedSteps, 0u);
  ASSERT_EQ(Report.UnmodelledOps, 0u);
  ASSERT_EQ(Report.OpaqueOps, 0u);
  ASSERT_EQ(Report.CallHavocs, 0u);
  ASSERT_EQ(Report.MemoryHavocs, 0u);
  ASSERT_FALSE(Report.FlipBudgetHit);
  ASSERT_FALSE(Report.CandidateBudgetHit);
  ASSERT_EQ(Report.FlipAttempts, 1u);

  ASSERT_EQ(Report.Decisions.size(), 1u);
  const LowIRConcolicDecision &Original = Report.Decisions.front();
  ASSERT_EQ(Original.Index, 0u);
  ASSERT_EQ(Original.Occurrence.Kind, SymDecisionKind::ConditionalBranch);
  ASSERT_TRUE(Original.Concrete);
  ASSERT_TRUE(Original.Taken);

  ASSERT_EQ(Report.Flips.size(), 1u);
  const LowIRConcolicFlip &Flip = Report.Flips.front();
  ASSERT_EQ(Flip.DecisionIndex, 0u);
  ASSERT_EQ(Flip.Occurrence, Original.Occurrence);
  ASSERT_TRUE(Flip.OriginalTaken);
  ASSERT_EQ(Flip.Status, ConcolicFlipStatus::Verified);
  ASSERT_EQ(Flip.SolverResult, solver::SatResult::Sat);
  ASSERT_EQ(Flip.EncodingError, solver::BlastError::None);
  ASSERT_EQ(Flip.ProjectionReason, ConcolicProjectionReason::None);
  ASSERT_EQ(Flip.ReplayReason, ConcolicReplayReason::None);
  ASSERT_EQ(Flip.CandidateIndex, 0u);

  ASSERT_EQ(Report.Candidates.size(), 1u);
  const std::vector<SymConcreteRegister> &Candidate =
      Report.Candidates.front().Seed;
  ASSERT_EQ(Candidate.size(), 1u);
  ASSERT_EQ(Candidate.front().Offset, Spec.InputRegister);
  ASSERT_EQ(Candidate.front().Bytes, 4u);
  ASSERT_EQ(Candidate.front().Value, 7u);

  SymContext ReplayContext;
  SymExecConcreteShadow ReplayShadow;
  ExploreOptions ReplayOptions;
  ReplayOptions.ByteOrder = llvm::endianness::little;
  ReplayOptions.Concolic = &ReplayShadow;
  ReplayOptions.ConcolicSeed = Candidate;
  const SymExploration Replay =
      explorePathsDetailed(ReplayContext, Function, ReplayOptions);
  ASSERT_EQ(Replay.Paths.size(), 1u);
  ASSERT_EQ(Replay.Paths.front().Outcome, PathOutcome::Returned);
  ASSERT_TRUE(Replay.Paths.front().ConcreteComplete);
  ASSERT_EQ(Replay.Paths.front().BranchDecisions.size(), 1u);
  const SymBranchDecision &Replayed =
      Replay.Paths.front().BranchDecisions.front();
  ASSERT_TRUE(Replayed.Concrete);
  ASSERT_EQ(Replayed.Occurrence, Original.Occurrence);
  ASSERT_FALSE(Replayed.Taken);

  const LowIRConcolicReport Repeated = runLowIRConcolic(Function, Options);
  expectDeterministicReport(Report, Repeated);
}

INSTANTIATE_TEST_SUITE_P(
    SixFormatMatrix, LowIRConcolicIntegration,
    ::testing::Values(FixtureSpec{"ELF_X64", "lowir_concolic_elf_x64",
                                  BinaryFormat::ELF, Arch::X64, x86reg::RDI},
                      FixtureSpec{"ELF_AArch64", "lowir_concolic_elf_arm64",
                                  BinaryFormat::ELF, Arch::AArch64, a64reg::X0},
                      FixtureSpec{"MachO_X64", "lowir_concolic_macho_x64",
                                  BinaryFormat::MachO, Arch::X64, x86reg::RDI},
                      FixtureSpec{"MachO_AArch64", "lowir_concolic_macho_arm64",
                                  BinaryFormat::MachO, Arch::AArch64,
                                  a64reg::X0},
                      FixtureSpec{"PE_X64", "lowir_concolic_pe_x64",
                                  BinaryFormat::COFF, Arch::X64, x86reg::RCX},
                      FixtureSpec{"PE_AArch64", "lowir_concolic_pe_arm64",
                                  BinaryFormat::COFF, Arch::AArch64,
                                  a64reg::X0}),
    [](const ::testing::TestParamInfo<FixtureSpec> &Info) {
      return Info.param.TestName;
    });

} // namespace
