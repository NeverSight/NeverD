//===- JumpTableProposalLFPTests.cpp - proposal fixed-point tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

class JumpTableProposalLFP : public NeverDLiftTest {};

fs::path proposalLFPObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_proposal_lfp.o";
}

fs::path lostPublishedLFPObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_lost_published.o";
}

bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
  for (const neverd::LowBlock &Block : Function.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Opcode == Opcode)
        return true;
  return false;
}

const neverd::LowOp *findExactOp(const neverd::LowFunc &Function,
                                 neverd::va_t Address, int Sequence,
                                 neverd::NdOp Opcode) {
  const neverd::LowOp *Found = nullptr;
  for (const neverd::LowBlock &Block : Function.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Addr != Address || Op.Seq != Sequence || Op.Opcode != Opcode)
        continue;
      if (Found)
        return nullptr;
      Found = &Op;
    }
  return Found;
}

std::string llvmFunctionBody(const std::string &IR, const std::string &Name) {
  std::string::size_type Begin = IR.find("@" + Name + "(");
  if (Begin == std::string::npos)
    return {};
  Begin = IR.rfind("define ", Begin);
  if (Begin == std::string::npos)
    return {};
  const std::string::size_type End = IR.find("\n}", Begin);
  return End == std::string::npos ? IR.substr(Begin)
                                  : IR.substr(Begin, End + 2 - Begin);
}

neverd::LowFunc
buildLow(const neverd::BinaryImage &Image, const neverd::Symbol &Function,
         const std::function<void(neverd::CFGBuilder &)> &Configure = {}) {
  neverd::Decoder Decoder;
  EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  if (Configure)
    Configure(Builder);
  return Builder.build(Image, Decoder, Function.Addr, Function.Name);
}

struct SiblingRange {
  const char *Begin;
  const char *End;
};

void expectCompleteSiblingBatch(const neverd::BinaryImage &Image,
                                const char *FunctionName, const char *TableName,
                                const std::vector<const char *> &TargetNames,
                                const std::vector<SiblingRange> &Ranges) {
  const neverd::Symbol *Function = Image.findSymbol(FunctionName);
  const neverd::Symbol *Table = Image.findSymbol(TableName);
  ASSERT_NE(Function, nullptr) << FunctionName;
  ASSERT_NE(Table, nullptr) << TableName;
  ASSERT_EQ(TargetNames.size(), 4u);
  ASSERT_EQ(Ranges.size(), 5u);
  ASSERT_EQ(Table->Size, 4u * 8u);

  const size_t RelocationCount = std::count_if(
      Image.CodePtrRelocSlots.begin(), Image.CodePtrRelocSlots.end(),
      [&](neverd::va_t Slot) {
        return Slot >= Table->Addr && Slot < Table->Addr + Table->Size;
      });
  ASSERT_EQ(RelocationCount, 4u)
      << "the shared table must expose four authenticated code roots";

  std::set<neverd::va_t> ExpectedTargets;
  for (const char *Name : TargetNames) {
    const neverd::Symbol *Target = Image.findSymbol(Name);
    ASSERT_NE(Target, nullptr) << Name;
    ExpectedTargets.insert(Target->Addr);
  }
  ASSERT_EQ(ExpectedTargets.size(), 4u);

  const neverd::LowFunc Low = buildLow(Image, *Function);
  ASSERT_EQ(Low.JumpTables.size(), Ranges.size())
      << FunctionName
      << " must publish every same-storage sibling in one least fixed point";
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());

  std::set<std::pair<neverd::va_t, int>> SelectorOccurrences;
  std::set<std::pair<neverd::va_t, int>> LoadOccurrences;
  for (const SiblingRange &Range : Ranges) {
    const neverd::Symbol *Begin = Image.findSymbol(Range.Begin);
    const neverd::Symbol *End = Image.findSymbol(Range.End);
    ASSERT_NE(Begin, nullptr) << Range.Begin;
    ASSERT_NE(End, nullptr) << Range.End;
    ASSERT_LT(Begin->Addr, End->Addr);

    const neverd::JumpTable *Found = nullptr;
    for (const neverd::JumpTable &TableInfo : Low.JumpTables) {
      if (TableInfo.InsnAddr < Begin->Addr || TableInfo.InsnAddr >= End->Addr)
        continue;
      ASSERT_EQ(Found, nullptr)
          << "one sibling range must own exactly one indirect dispatch";
      Found = &TableInfo;
    }
    ASSERT_NE(Found, nullptr) << Range.Begin;
    EXPECT_TRUE(Found->HasBaseAddr);
    EXPECT_EQ(Found->BaseAddr, Table->Addr);
    EXPECT_EQ(
        std::set<neverd::va_t>(Found->Targets.begin(), Found->Targets.end()),
        ExpectedTargets);

    ASSERT_EQ(Found->SelectorUseRefs.size(), 1u);
    const neverd::JumpTableSelectorUseRef &Selector =
        Found->SelectorUseRefs.front();
    EXPECT_GE(Selector.Addr, Begin->Addr);
    EXPECT_LT(Selector.Addr, End->Addr);
    EXPECT_NE(
        findExactOp(Low, Selector.Addr, Selector.Seq, Selector.ExpectedOpcode),
        nullptr);
    EXPECT_TRUE(
        SelectorOccurrences.insert({Selector.Addr, Selector.Seq}).second)
        << "same-storage siblings must retain distinct selector occurrences";

    ASSERT_EQ(Found->AuthenticatedTableLoads.size(), 1u);
    const neverd::JumpTableOpOccurrence &Load =
        Found->AuthenticatedTableLoads.front();
    EXPECT_GE(Load.Addr, Begin->Addr);
    EXPECT_LT(Load.Addr, End->Addr);
    EXPECT_NE(findExactOp(Low, Load.Addr, Load.Seq, neverd::NdOp::LOAD),
              nullptr)
        << "a non-LOAD occurrence must not borrow sibling authentication";
    EXPECT_TRUE(LoadOccurrences.insert({Load.Addr, Load.Seq}).second)
        << "same-storage siblings must retain distinct LOAD occurrences";
  }
}

TEST_F(JumpTableProposalLFP, SizedAuthoritativeSelfCallbackStaysIndirectCall) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_lfp_sized_self_callback");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_lfp_sized_self_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_GT(Function->Size, 0u)
      << "the negative requires an authoritative sized function body";
  ASSERT_EQ(Table->Size, 2u * 8u)
      << "the negative requires an authoritative sized table object";
  ASSERT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            2)
      << "both self-callback entries must be authenticated code roots";

  const neverd::LowFunc Low = buildLow(Image, *Function);
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());

  const RunResult Run = liftToLLVMIR(proposalLFPObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  const std::string Body =
      llvmFunctionBody(Run.out, "jt_lfp_sized_self_callback");
  ASSERT_FALSE(Body.empty()) << Run.out;
  EXPECT_NE(Body.find("call"), std::string::npos) << Body;
  EXPECT_NE(Body.find("ret"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("llvm.trap"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("switch i"), std::string::npos) << Body;
}

TEST_F(JumpTableProposalLFP, SameStorageSiblingPublicationIsOrderIndependent) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;

  expectCompleteSiblingBatch(
      Image, "jt_lfp_siblings_forward", "jt_lfp_fwd_table",
      {"jt_lfp_fwd_t0_begin", "jt_lfp_fwd_t1_begin", "jt_lfp_fwd_t2_begin",
       "jt_lfp_fwd_t3_begin"},
      {{"jt_lfp_fwd_entry_begin", "jt_lfp_fwd_entry_end"},
       {"jt_lfp_fwd_t0_begin", "jt_lfp_fwd_t0_end"},
       {"jt_lfp_fwd_t1_begin", "jt_lfp_fwd_t1_end"},
       {"jt_lfp_fwd_t2_begin", "jt_lfp_fwd_t2_end"},
       {"jt_lfp_fwd_t3_begin", "jt_lfp_fwd_t3_end"}});

  expectCompleteSiblingBatch(
      Image, "jt_lfp_siblings_reverse", "jt_lfp_rev_table",
      {"jt_lfp_rev_t0_begin", "jt_lfp_rev_t1_begin", "jt_lfp_rev_t2_begin",
       "jt_lfp_rev_t3_begin"},
      {{"jt_lfp_rev_entry_begin", "jt_lfp_rev_entry_end"},
       {"jt_lfp_rev_t3_begin", "jt_lfp_rev_t3_end"},
       {"jt_lfp_rev_t2_begin", "jt_lfp_rev_t2_end"},
       {"jt_lfp_rev_t1_begin", "jt_lfp_rev_t1_end"},
       {"jt_lfp_rev_t0_begin", "jt_lfp_rev_t0_end"}});
}

TEST_F(JumpTableProposalLFP,
       SharedStageBudgetIsTransactionalAcrossSiblingBatch) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol("jt_lfp_siblings_forward");
  ASSERT_NE(Function, nullptr);

  auto BuildWithBudget = [&](size_t Budget, bool &HasPendingExploration) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    neverd::LowFunc Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);
    HasPendingExploration =
        Builder.hasMaskFixedPointExplorationTargetsForTesting();
    return Low;
  };

  constexpr size_t KnownFailingBudget = size_t{1} << 20;
  constexpr size_t KnownPassingBudget = size_t{2} << 20;
  auto CompletesBatch = [&](size_t Budget) {
    bool HasPendingExploration = true;
    const neverd::LowFunc Low = BuildWithBudget(Budget, HasPendingExploration);
    return Low.JumpTables.size() == 5u &&
           Low.UnsafeIndirectBranchAddresses.empty() && !HasPendingExploration;
  };

  ASSERT_FALSE(CompletesBatch(KnownFailingBudget));
  ASSERT_TRUE(CompletesBatch(KnownPassingBudget));
  size_t FailingBudget = KnownFailingBudget;
  size_t PassingBudget = KnownPassingBudget;
  while (FailingBudget + 1 < PassingBudget) {
    const size_t Midpoint = FailingBudget + (PassingBudget - FailingBudget) / 2;
    if (CompletesBatch(Midpoint))
      PassingBudget = Midpoint;
    else
      FailingBudget = Midpoint;
  }
  ASSERT_EQ(FailingBudget + 1, PassingBudget);

  bool HasPendingExploration = true;
  const neverd::LowFunc Exhausted =
      BuildWithBudget(FailingBudget, HasPendingExploration);
  EXPECT_TRUE(Exhausted.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Exhausted, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Exhausted, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Exhausted.UnsafeIndirectBranchAddresses.empty());
  EXPECT_FALSE(HasPendingExploration)
      << "one exhausted stage must not leak provisional case targets into the "
         "next fixed-point stage";

  HasPendingExploration = true;
  const neverd::LowFunc Complete =
      BuildWithBudget(PassingBudget, HasPendingExploration);
  EXPECT_EQ(Complete.JumpTables.size(), 5u);
  EXPECT_FALSE(hasOpcode(Complete, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Complete.UnsafeIndirectBranchAddresses.empty());
  EXPECT_FALSE(HasPendingExploration);
}

TEST_F(JumpTableProposalLFP,
       CleanupReservationsPrecedeCandidateMutationAtExactBoundaries) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol("jt_lfp_siblings_forward");
  ASSERT_NE(Function, nullptr);

  struct BudgetedBuild {
    neverd::LowFunc Low;
    neverd::CFGBuilder::ProposalCleanupEvidenceStateForTesting Cleanup;
    bool HasPendingExploration = false;
  };
  auto BuildWithBudget = [&](size_t Budget,
                             bool ExhaustOldStateCleanup = false) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    Builder.setProposalOldStateCleanupEvidenceExhaustionForTesting(
        ExhaustOldStateCleanup);
    BudgetedBuild Result;
    Result.Low = Builder.build(Image, Decoder, Function->Addr, Function->Name);
    Result.Cleanup = Builder.proposalCleanupEvidenceStateForTesting();
    Result.HasPendingExploration =
        Builder.hasMaskFixedPointExplorationTargetsForTesting();
    return Result;
  };

  auto FirstReservedBudget = [&](const auto &IsReserved) {
    size_t Failing = 0;
    size_t Passing = neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
    if (!IsReserved(BuildWithBudget(Passing).Cleanup)) {
      ADD_FAILURE() << "production allowance never reached cleanup phase";
      return Passing;
    }
    while (Failing + 1 < Passing) {
      const size_t Midpoint = Failing + (Passing - Failing) / 2;
      if (IsReserved(BuildWithBudget(Midpoint).Cleanup))
        Passing = Midpoint;
      else
        Failing = Midpoint;
    }
    return Passing;
  };
  auto ExpectTransactionalFailure =
      [&](const char *Phase, size_t Budget, const BudgetedBuild &Result,
          bool PhaseExhausted, bool RequiresOpaqueFinalState,
          bool RequiresStrongIdentity) {
        SCOPED_TRACE(::testing::Message()
                     << Phase << " boundary budget=" << Budget);
        EXPECT_TRUE(PhaseExhausted);
        EXPECT_FALSE(Result.Cleanup.MutationObservedBeforeReservation);
        if (RequiresOpaqueFinalState) {
          EXPECT_TRUE(Result.Low.JumpTables.empty());
          EXPECT_TRUE(hasOpcode(Result.Low, neverd::NdOp::INDIR_BR));
          if (RequiresStrongIdentity) {
            EXPECT_FALSE(hasOpcode(Result.Low, neverd::NdOp::INDIR_CALL));
            EXPECT_FALSE(Result.Low.UnsafeIndirectBranchAddresses.empty());
          }
        } else {
          EXPECT_EQ(Result.Low.JumpTables.size(), 5u);
          EXPECT_TRUE(Result.Low.UnsafeIndirectBranchAddresses.empty());
        }
        EXPECT_FALSE(Result.HasPendingExploration);
      };

  const size_t OldStateBudget = FirstReservedBudget(
      [](const auto &State) { return State.OldStateReserved; });
  ASSERT_GT(OldStateBudget, 0u);
  const BudgetedBuild Production =
      BuildWithBudget(neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork);
  EXPECT_EQ(Production.Low.JumpTables.size(), 5u);
  EXPECT_TRUE(Production.Low.UnsafeIndirectBranchAddresses.empty());
  EXPECT_FALSE(Production.HasPendingExploration);
  EXPECT_TRUE(Production.Cleanup.OldStateReserved);
  EXPECT_TRUE(Production.Cleanup.NewTargetsReserved);
  EXPECT_TRUE(Production.Cleanup.NewInfoReserved);
  EXPECT_FALSE(Production.Cleanup.MutationObservedBeforeReservation);
  const BudgetedBuild OldStateBoundary =
      BuildWithBudget(neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork,
                      /*ExhaustOldStateCleanup=*/true);
  ExpectTransactionalFailure(
      "old-state injected pre-move",
      neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork, OldStateBoundary,
      OldStateBoundary.Cleanup.OldStateExhausted,
      /*RequiresOpaqueFinalState=*/false,
      /*RequiresStrongIdentity=*/true);

  const size_t NewTargetsBudget = FirstReservedBudget(
      [](const auto &State) { return State.NewTargetsReserved; });
  ASSERT_GT(NewTargetsBudget, 0u);
  const BudgetedBuild NewTargetsBoundary =
      BuildWithBudget(NewTargetsBudget - 1);
  ExpectTransactionalFailure("new-targets", NewTargetsBudget - 1,
                             NewTargetsBoundary,
                             NewTargetsBoundary.Cleanup.NewTargetsExhausted,
                             /*RequiresOpaqueFinalState=*/true,
                             /*RequiresStrongIdentity=*/false);

  const size_t NewInfoBudget = FirstReservedBudget(
      [](const auto &State) { return State.NewInfoReserved; });
  ASSERT_GT(NewInfoBudget, 0u);
  const BudgetedBuild NewInfoBoundary = BuildWithBudget(NewInfoBudget - 1);
  ExpectTransactionalFailure("new-info", NewInfoBudget - 1, NewInfoBoundary,
                             NewInfoBoundary.Cleanup.NewInfoExhausted,
                             /*RequiresOpaqueFinalState=*/true,
                             /*RequiresStrongIdentity=*/false);
}

TEST_F(JumpTableProposalLFP,
       QuarantineCommitTailExhaustionRollsBackBeforePersistentMutation) {
  auto ImageOrErr = neverd::loadBinary(lostPublishedLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_lost_published_table");
  ASSERT_NE(Function, nullptr);

  struct BudgetedBuild {
    neverd::LowFunc Low;
    bool CommitTailExhausted = false;
    bool RollbackMutatedQuarantine = false;
    bool HasQuarantinedProposal = false;
    bool HasPendingExploration = false;
  };
  auto BuildWithBudget = [&](size_t Budget) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    BudgetedBuild Result;
    Result.Low = Builder.build(Image, Decoder, Function->Addr, Function->Name);
    Result.CommitTailExhausted =
        Builder.proposalStageCommitTailEvidenceExhaustedForTesting();
    Result.RollbackMutatedQuarantine =
        Builder.proposalStageRollbackMutatedQuarantineForTesting();
    Result.HasQuarantinedProposal =
        Builder.hasQuarantinedJumpTableProposalsForTesting();
    Result.HasPendingExploration =
        Builder.hasMaskFixedPointExplorationTargetsForTesting();
    return Result;
  };

  // This fixture first proposes and publishes a locally owned table, then
  // loses that proof after a newly explored case contributes a backedge.  Find
  // the exact aggregate budget at which that definitive loss can first commit
  // to the quarantine set.  The search follows observable transaction state,
  // so accounting changes do not turn the regression into an address- or
  // work-count special case.
  size_t NonCommittingBudget = 0;
  size_t CommittingBudget =
      neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
  ASSERT_FALSE(BuildWithBudget(NonCommittingBudget).HasQuarantinedProposal);
  ASSERT_TRUE(BuildWithBudget(CommittingBudget).HasQuarantinedProposal)
      << "the fixture must reach a prior strong proposal followed by a "
         "definitive local proof loss";
  while (NonCommittingBudget + 1 < CommittingBudget) {
    const size_t Midpoint =
        NonCommittingBudget + (CommittingBudget - NonCommittingBudget) / 2;
    if (BuildWithBudget(Midpoint).HasQuarantinedProposal)
      CommittingBudget = Midpoint;
    else
      NonCommittingBudget = Midpoint;
  }
  ASSERT_EQ(NonCommittingBudget + 1, CommittingBudget);

  const BudgetedBuild Boundary = BuildWithBudget(NonCommittingBudget);
  EXPECT_TRUE(Boundary.CommitTailExhausted)
      << "one unit below the first complete quarantine commit must fail at "
         "the atomically prepaid commit tail";
  EXPECT_FALSE(Boundary.RollbackMutatedQuarantine)
      << "an incomplete stage must restore the quarantine set it observed at "
         "stage entry";
  EXPECT_FALSE(Boundary.HasQuarantinedProposal);
  EXPECT_FALSE(Boundary.HasPendingExploration);
  EXPECT_TRUE(Boundary.Low.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Boundary.Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Boundary.Low, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Boundary.Low.UnsafeIndirectBranchAddresses.empty())
      << "budget exhaustion must preserve the unresolved dispatch as an "
         "opaque unsafe branch";

  const BudgetedBuild Complete = BuildWithBudget(CommittingBudget);
  EXPECT_TRUE(Complete.HasQuarantinedProposal);
  EXPECT_FALSE(Complete.RollbackMutatedQuarantine);
  EXPECT_TRUE(Complete.Low.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Complete.Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Complete.Low, neverd::NdOp::INDIR_CALL));
}

} // namespace
