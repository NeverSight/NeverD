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
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

class JumpTableProposalLFP : public NeverDLiftTest {};

fs::path proposalLFPObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_proposal_lfp.o";
}

fs::path singletonClosedWorldObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_singleton_closed_world.o";
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

const neverd::LowOp *findOpcodeAtAddress(const neverd::LowFunc &Function,
                                         neverd::va_t Address,
                                         neverd::NdOp Opcode) {
  const neverd::LowOp *Found = nullptr;
  for (const neverd::LowBlock &Block : Function.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Addr != Address || Op.Opcode != Opcode)
        continue;
      if (Found)
        return nullptr;
      Found = &Op;
    }
  return Found;
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

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
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

TEST_F(JumpTableProposalLFP, SizedAuthoritativeSelfDispatchStaysOpaqueBranch) {
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
      << "the regression requires an authoritative sized function body";
  ASSERT_EQ(Table->Size, 2u * 8u)
      << "the regression requires an authoritative sized table object";
  ASSERT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            2)
      << "both self-callback entries must be authenticated code roots";

  const neverd::LowFunc Low = buildLow(Image, *Function);
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());

  const RunResult Run = liftToLLVMIR(proposalLFPObj());
  ASSERT_EQ(Run.exitCode, 0) << Run.err;
  const std::string Body =
      llvmFunctionBody(Run.out, "jt_lfp_sized_self_callback");
  ASSERT_FALSE(Body.empty()) << Run.out;
  EXPECT_EQ(Body.find("call i64"), std::string::npos) << Body;
  EXPECT_NE(Body.find("llvm.trap"), std::string::npos) << Body;
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
  constexpr size_t KnownPassingBudget =
      neverd::limits::kMaxJumpTableProposalStageEvidenceWork;
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
  std::set<neverd::va_t> OpaqueSiblingBranches;
  for (const neverd::LowBlock &Block : Exhausted.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Opcode == neverd::NdOp::INDIR_BR)
        OpaqueSiblingBranches.insert(Op.Addr);
  EXPECT_EQ(OpaqueSiblingBranches.size(), 5u)
      << "every sibling must remain an indirect branch after rollback";
  EXPECT_EQ(Exhausted.UnsafeIndirectBranchAddresses, OpaqueSiblingBranches)
      << "rollback must retain both entry markers and incomplete evidence "
         "discovered by later sibling candidates";
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
       UntrackedNestedResourceExhaustionRollsBackItsStage) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol("jt_lfp_nested_relative");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  Builder.setExhaustUntrackedJumpTableCandidateForTesting(true);
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  EXPECT_TRUE(Builder.untrackedJumpTableCandidateRollbackObservedForTesting())
      << "a recursively discovered candidate's resource failure must abort "
         "the immutable stage that owns its provisional parent edges";
  EXPECT_TRUE(
      Builder.untrackedJumpTableCandidateProvisionalStateObservedForTesting())
      << "the injected failure must occur after the nested resolver published "
         "state, otherwise it does not cover the post-mutation seam";
  EXPECT_TRUE(
      Builder.untrackedJumpTableCandidateStateClearedOnRollbackForTesting())
      << "the same rollback must clear the exact nested address before any "
         "later retry can hide leaked provisional state";
  EXPECT_FALSE(Builder.hasProvisionalRelativeEdgesForTesting());
  EXPECT_FALSE(Builder.hasMaskFixedPointExplorationTargetsForTesting());
  EXPECT_EQ(Low.JumpTables.size(), 2u)
      << "the one-shot failed stage must be retryable from clean state";
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JumpTableProposalLFP, ProvisionalRelativeEdgesCloseExactSameTableCycle) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_lfp_relative_occurrence_cycle");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_lfp_relative_occurrence_cycle_table");
  const neverd::Symbol *EntryBranch =
      Image.findSymbol("jt_lfp_relative_occurrence_entry_branch");
  const neverd::Symbol *LoopBranch =
      Image.findSymbol("jt_lfp_relative_occurrence_loop_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_NE(EntryBranch, nullptr);
  ASSERT_NE(LoopBranch, nullptr);
  ASSERT_EQ(Table->Size, 4u * sizeof(uint32_t));
  ASSERT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            4)
      << "the provisional-edge certificate requires one complete relative "
         "relocation run";

  std::set<neverd::va_t> ExpectedTargets;
  neverd::va_t EntryTarget = neverd::InvalidVA;
  for (const char *Name :
       {"jt_lfp_relative_occurrence_t0", "jt_lfp_relative_occurrence_t1",
        "jt_lfp_relative_occurrence_t2", "jt_lfp_relative_occurrence_t3"}) {
    const neverd::Symbol *Target = Image.findSymbol(Name);
    ASSERT_NE(Target, nullptr) << Name;
    if (EntryTarget == neverd::InvalidVA)
      EntryTarget = Target->Addr;
    ExpectedTargets.insert(Target->Addr);
  }
  ASSERT_EQ(ExpectedTargets.size(), 4u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 2u)
      << "the loop dispatch must be published in the same proposal fixed "
         "point as the literal entry dispatch";
  std::set<neverd::va_t> MissingBranches = {EntryBranch->Addr,
                                            LoopBranch->Addr};
  std::set<std::pair<neverd::va_t, int>> LoadOccurrences;
  for (const neverd::JumpTable &Recovered : Low.JumpTables) {
    EXPECT_EQ(MissingBranches.erase(Recovered.InsnAddr), 1u);
    EXPECT_TRUE(Recovered.HasBaseAddr);
    EXPECT_EQ(Recovered.BaseAddr, Table->Addr);
    EXPECT_TRUE(Recovered.IsRelative);
    EXPECT_EQ(Recovered.EntrySize, sizeof(uint32_t));
    if (Recovered.InsnAddr == EntryBranch->Addr) {
      EXPECT_EQ(Recovered.Targets, std::vector<neverd::va_t>{EntryTarget});
      EXPECT_EQ(Recovered.CaseLabels, std::vector<int64_t>{0});
      EXPECT_EQ(Recovered.SlotIndices, std::vector<uint32_t>{0});
      ASSERT_EQ(Recovered.StorageRanges.size(), 1u);
      EXPECT_EQ(Recovered.StorageRanges.front(),
                (neverd::JumpTableStorageRange{Table->Addr, sizeof(uint32_t),
                                               sizeof(uint32_t), 1}));
      EXPECT_EQ(Recovered.SelectorUseRefs.size(), 1u)
          << "the exact finite selector occurrence must survive lowering";
    } else {
      EXPECT_EQ(std::set<neverd::va_t>(Recovered.Targets.begin(),
                                       Recovered.Targets.end()),
                ExpectedTargets);
      EXPECT_EQ(Recovered.CaseLabels, (std::vector<int64_t>{0, 1, 2, 3}));
      EXPECT_EQ(Recovered.SlotIndices, (std::vector<uint32_t>{0, 1, 2, 3}));
    }
    ASSERT_EQ(Recovered.AuthenticatedTableLoads.size(), 1u);
    const neverd::JumpTableOpOccurrence &Load =
        Recovered.AuthenticatedTableLoads.front();
    EXPECT_EQ(Load.Size, sizeof(uint32_t));
    EXPECT_TRUE(LoadOccurrences.insert({Load.Addr, Load.Seq}).second)
        << "each branch must retain its own exact table LOAD occurrence";
  }
  EXPECT_TRUE(MissingBranches.empty());
  EXPECT_EQ(LoadOccurrences.size(), 2u);
  const std::optional<bool> RequiresCompleteCFG =
      Builder.resolvedJumpTableRequiresCompleteCFGProofForTesting(
          EntryBranch->Addr);
  ASSERT_TRUE(RequiresCompleteCFG.has_value());
  EXPECT_TRUE(*RequiresCompleteCFG)
      << "the singleton consumer borrowed its sibling's runtime certificate "
         "without persisting complete-CFG replay";
  EXPECT_FALSE(Builder.hasProvisionalRelativeEdgesForTesting());
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));

  struct BudgetedCycleBuild {
    neverd::LowFunc Low;
    bool RecordedCompleteRuntimeStorageCertificate = false;
    bool HasProvisionalEdges = false;
    bool HasPendingExploration = false;
  };
  auto BuildWithBudget = [&](size_t Budget) {
    neverd::Decoder BudgetDecoder;
    EXPECT_TRUE(BudgetDecoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder BudgetBuilder;
    BudgetBuilder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    BudgetedCycleBuild Result;
    Result.Low = BudgetBuilder.build(Image, BudgetDecoder, Function->Addr,
                                     Function->Name);
    Result.RecordedCompleteRuntimeStorageCertificate =
        BudgetBuilder.recordedCompleteRuntimeStorageCertificateForTesting();
    Result.HasProvisionalEdges =
        BudgetBuilder.hasProvisionalRelativeEdgesForTesting();
    Result.HasPendingExploration =
        BudgetBuilder.hasMaskFixedPointExplorationTargetsForTesting();
    return Result;
  };
  auto CompletesCycle = [&](const BudgetedCycleBuild &Result) {
    return Result.Low.JumpTables.size() == 2u &&
           Result.Low.UnsafeIndirectBranchAddresses.empty() &&
           !Result.HasProvisionalEdges && !Result.HasPendingExploration;
  };

  size_t FailingBudget = 0;
  size_t PassingBudget = neverd::limits::kMaxJumpTableProposalStageEvidenceWork;
  ASSERT_TRUE(CompletesCycle(BuildWithBudget(PassingBudget)))
      << "the production proposal-stage cap must close the certified cycle";
  while (FailingBudget + 1 < PassingBudget) {
    const size_t Midpoint = FailingBudget + (PassingBudget - FailingBudget) / 2;
    if (CompletesCycle(BuildWithBudget(Midpoint)))
      PassingBudget = Midpoint;
    else
      FailingBudget = Midpoint;
  }
  ASSERT_EQ(FailingBudget + 1, PassingBudget);
  const BudgetedCycleBuild BoundaryFailure = BuildWithBudget(FailingBudget);
  EXPECT_TRUE(BoundaryFailure.RecordedCompleteRuntimeStorageCertificate)
      << "the failing boundary must retire a certificate that was actually "
         "recorded earlier in this build";
  EXPECT_TRUE(BoundaryFailure.Low.JumpTables.empty())
      << "one unit below the first complete budget must publish no partial "
         "same-table cycle";
  EXPECT_TRUE(hasOpcode(BoundaryFailure.Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(BoundaryFailure.Low, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(BoundaryFailure.Low.UnsafeIndirectBranchAddresses.empty());
  EXPECT_FALSE(BoundaryFailure.HasProvisionalEdges);
  EXPECT_FALSE(BoundaryFailure.HasPendingExploration);
}

TEST_F(JumpTableProposalLFP, OpenConsumerCannotBorrowSiblingRuntimeDomain) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_lfp_relative_open_sibling");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_lfp_relative_open_sibling_table");
  const neverd::Symbol *EntryBranch =
      Image.findSymbol("jt_lfp_relative_open_sibling_entry_branch");
  const neverd::Symbol *LoopBranch =
      Image.findSymbol("jt_lfp_relative_open_sibling_loop_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_NE(EntryBranch, nullptr);
  ASSERT_NE(LoopBranch, nullptr);
  ASSERT_EQ(Table->Size, 4u * sizeof(uint32_t));
  ASSERT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            4)
      << "the negative must contain a real complete sibling certificate";

  std::set<neverd::va_t> ExpectedTargets;
  for (const char *Name :
       {"jt_lfp_relative_open_sibling_t0", "jt_lfp_relative_open_sibling_t1",
        "jt_lfp_relative_open_sibling_t2", "jt_lfp_relative_open_sibling_t3"}) {
    const neverd::Symbol *Target = Image.findSymbol(Name);
    ASSERT_NE(Target, nullptr) << Name;
    ExpectedTargets.insert(Target->Addr);
  }
  ASSERT_EQ(ExpectedTargets.size(), 4u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  EXPECT_TRUE(Low.JumpTables.empty())
      << "same-storage publication is transactional: an open singleton must "
         "not borrow its sibling's dense selector domain or leave that sibling "
         "published from the rejected batch";
  EXPECT_TRUE(Builder.recordedCompleteRuntimeStorageCertificateForTesting())
      << "the producer certificate must exist or this negative missed its seam";
  EXPECT_FALSE(Builder
                   .resolvedJumpTableRequiresCompleteCFGProofForTesting(
                       EntryBranch->Addr)
                   .has_value());
  EXPECT_FALSE(
      Builder
          .resolvedJumpTableRequiresCompleteCFGProofForTesting(LoopBranch->Addr)
          .has_value());
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(EntryBranch->Addr), 1u);
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(LoopBranch->Addr), 1u);
  EXPECT_FALSE(Builder.hasProvisionalRelativeEdgesForTesting());
}

TEST_F(JumpTableProposalLFP, LowerRankRelativeEdgeClosesPreciseGuardSuccessor) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_lfp_relative_guard_cycle");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_lfp_relative_guard_cycle_table");
  const neverd::Symbol *EntryBranch =
      Image.findSymbol("jt_lfp_relative_guard_entry_branch");
  const neverd::Symbol *LoopBranch =
      Image.findSymbol("jt_lfp_relative_guard_loop_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_NE(EntryBranch, nullptr);
  ASSERT_NE(LoopBranch, nullptr);
  ASSERT_EQ(Table->Size, 4u * sizeof(uint32_t));
  ASSERT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            4)
      << "the lower-rank certificate requires one exact relative object";

  std::set<neverd::va_t> ExpectedTargets;
  neverd::va_t EntryTarget = neverd::InvalidVA;
  for (const char *Name :
       {"jt_lfp_relative_guard_t0", "jt_lfp_relative_guard_t1",
        "jt_lfp_relative_guard_t2", "jt_lfp_relative_guard_t3"}) {
    const neverd::Symbol *Target = Image.findSymbol(Name);
    ASSERT_NE(Target, nullptr) << Name;
    if (EntryTarget == neverd::InvalidVA)
      EntryTarget = Target->Addr;
    ExpectedTargets.insert(Target->Addr);
  }
  ASSERT_EQ(ExpectedTargets.size(), 4u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  ASSERT_EQ(Low.JumpTables.size(), 2u)
      << "the cmp/ja successor must consume the lower-rank edge overlay";
  const neverd::JumpTable *Entry = nullptr;
  const neverd::JumpTable *Loop = nullptr;
  for (const neverd::JumpTable &Recovered : Low.JumpTables) {
    if (Recovered.InsnAddr == EntryBranch->Addr)
      Entry = &Recovered;
    if (Recovered.InsnAddr == LoopBranch->Addr)
      Loop = &Recovered;
  }
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Loop, nullptr);
  EXPECT_EQ(Entry->Targets, std::vector<neverd::va_t>{EntryTarget});
  EXPECT_EQ(std::set<neverd::va_t>(Loop->Targets.begin(), Loop->Targets.end()),
            ExpectedTargets);
  EXPECT_EQ(Loop->BaseAddr, Table->Addr);
  EXPECT_TRUE(Loop->IsRelative);
  EXPECT_EQ(Loop->EntrySize, sizeof(uint32_t));
  EXPECT_FALSE(Builder.hasProvisionalRelativeEdgesForTesting())
      << "stable publication must replay after retiring the overlay";
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JumpTableProposalLFP,
       PreciseGuardOverlayRejectsEqualAndHigherRankEdges) {
  constexpr neverd::va_t Candidate = 0x4000;
  constexpr neverd::va_t Proposal = 0x5000;
  EXPECT_TRUE(neverd::detail::canBorrowProvisionalRelativeEdge(
      Candidate, /*CandidateProofRank=*/4, Proposal,
      /*ProposalProofRank=*/3));
  EXPECT_FALSE(neverd::detail::canBorrowProvisionalRelativeEdge(
      Candidate, /*CandidateProofRank=*/4, Proposal,
      /*ProposalProofRank=*/4));
  EXPECT_FALSE(neverd::detail::canBorrowProvisionalRelativeEdge(
      Candidate, /*CandidateProofRank=*/4, Proposal,
      /*ProposalProofRank=*/5));
  EXPECT_FALSE(neverd::detail::canBorrowProvisionalRelativeEdge(
      Candidate, /*CandidateProofRank=*/4, Candidate,
      /*ProposalProofRank=*/3));
}

TEST_F(JumpTableProposalLFP,
       RuntimeCertificateRejectsSelfShapeRangeAndPartialPayloads) {
  using Match = neverd::detail::ProvisionalRelativeRuntimeCertificateMatch;
  using Shape = neverd::detail::ProvisionalRelativeRuntimeCertificateShape;
  const Shape Candidate{0x4000, neverd::JumpTableStorageRange{0x8000, 4, 4, 0},
                        0x8000,
                        /*EntryScale=*/1, /*IsSigned=*/true};
  Shape Proposal{0x5000, neverd::JumpTableStorageRange{0x8000, 4, 4, 1}, 0x8000,
                 /*EntryScale=*/1, /*IsSigned=*/true};
  const neverd::JumpTableStorageRange DenseRange{0x8000, 4, 4, 4};
  const std::vector<neverd::va_t> Targets{0x1000, 0x1010, 0x1020, 0x1030};
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, Proposal, &DenseRange, Targets),
            Match::Match);
  EXPECT_TRUE(
      neverd::detail::
          provisionalRelativeRuntimeCertificateRequiresCompleteCFGReplay(
              Match::Match));
  EXPECT_FALSE(
      neverd::detail::
          provisionalRelativeRuntimeCertificateRequiresCompleteCFGReplay(
              Match::Malformed));
  EXPECT_FALSE(
      neverd::detail::
          provisionalRelativeRuntimeCertificateRequiresCompleteCFGReplay(
              Match::NotApplicable));

  Shape Self = Proposal;
  Self.BranchAddr = Candidate.BranchAddr;
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, Self, &DenseRange, Targets),
            Match::NotApplicable);
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, Proposal, nullptr, Targets),
            Match::NotApplicable);

  Shape WrongShape = Proposal;
  WrongShape.TargetAnchor += 4;
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, WrongShape, &DenseRange, Targets),
            Match::NotApplicable);
  WrongShape = Proposal;
  WrongShape.Storage.EntryStride = 8;
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, WrongShape, &DenseRange, Targets),
            Match::NotApplicable);

  neverd::JumpTableStorageRange WrongRange = DenseRange;
  WrongRange.BaseAddr += 4;
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, Proposal, &WrongRange, Targets),
            Match::Malformed);
  WrongRange = DenseRange;
  WrongRange.PhysicalSlotCount = 3;
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, Proposal, &WrongRange, Targets),
            Match::Malformed);
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                Candidate, Proposal, &DenseRange,
                llvm::ArrayRef(Targets).drop_back()),
            Match::Malformed);
  WrongRange = DenseRange;
  WrongRange.BaseAddr = neverd::InvalidVA - 4;
  Shape OverflowCandidate = Candidate;
  Shape OverflowProposal = Proposal;
  OverflowCandidate.Storage.BaseAddr = WrongRange.BaseAddr;
  OverflowProposal.Storage.BaseAddr = WrongRange.BaseAddr;
  OverflowCandidate.TargetAnchor = WrongRange.BaseAddr;
  OverflowProposal.TargetAnchor = WrongRange.BaseAddr;
  EXPECT_EQ(neverd::detail::matchProvisionalRelativeRuntimeCertificate(
                OverflowCandidate, OverflowProposal, &WrongRange, Targets),
            Match::Malformed);

  const std::vector<uint32_t> DenseSlots{0, 1, 2, 3};
  const std::vector<uint32_t> SparseSlots{0, 1, 3, 4};
  EXPECT_TRUE(neverd::detail::isDenseRelativeRuntimeCertificatePayload(
      DenseRange, DenseSlots, Targets));
  EXPECT_FALSE(neverd::detail::isDenseRelativeRuntimeCertificatePayload(
      DenseRange, SparseSlots, Targets));
  EXPECT_FALSE(neverd::detail::isDenseRelativeRuntimeCertificatePayload(
      DenseRange, llvm::ArrayRef(DenseSlots).drop_back(), Targets));
}

TEST_F(JumpTableProposalLFP,
       StableRuntimeCertificateRequiresExactPublishedTargets) {
  const neverd::JumpTableStorageRange Range{0x8000, 4, 4, 4};
  const std::vector<neverd::va_t> Targets{0x1000, 0x1010, 0x1020, 0x1030};
  std::vector<neverd::va_t> DifferentTargets = Targets;
  DifferentTargets.back() += 4;
  EXPECT_TRUE(neverd::detail::isStableProvisionalRelativeRuntimeCertificate(
      &Range, Targets, Targets));
  EXPECT_FALSE(neverd::detail::isStableProvisionalRelativeRuntimeCertificate(
      &Range, Targets, llvm::ArrayRef<neverd::va_t>()));
  EXPECT_FALSE(neverd::detail::isStableProvisionalRelativeRuntimeCertificate(
      &Range, Targets, DifferentTargets));
  EXPECT_FALSE(neverd::detail::sameProvisionalRelativeRuntimeCertificatePayload(
      Range, Targets, Range, DifferentTargets));
}

TEST_F(JumpTableProposalLFP,
       ProvisionalProposalComparisonPreflightChargesDualRuntimeRanges) {
  // Primary range (4) + flags (3) + optional presence (1) + runtime range
  // (4) + anchor/scale/sign, vector sizes, and proof rank (6) = 18.
  constexpr size_t DualRuntimeFixedWork = 18;
  const auto Work =
      neverd::detail::provisionalRelativeProposalComparisonWork(0, 0, 0, 0);
  ASSERT_EQ(Work, std::optional<size_t>{DualRuntimeFixedWork});
  EXPECT_FALSE(
      neverd::detail::comparisonWorkFitsBudget(Work, DualRuntimeFixedWork - 1));
  EXPECT_TRUE(
      neverd::detail::comparisonWorkFitsBudget(Work, DualRuntimeFixedWork));
}

TEST_F(JumpTableProposalLFP,
       ProposalComparisonPreflightRejectsExactBudgetMinusOne) {
  auto ExpectExactBoundary = [](std::optional<size_t> Work, size_t Expected) {
    ASSERT_TRUE(Work.has_value());
    ASSERT_EQ(*Work, Expected);
    ASSERT_GT(Expected, 0u);
    EXPECT_FALSE(neverd::detail::comparisonWorkFitsBudget(Work, Expected - 1));
    EXPECT_TRUE(neverd::detail::comparisonWorkFitsBudget(Work, Expected));
  };

  ExpectExactBoundary(
      neverd::detail::strongJumpTableLoadRoleVectorComparisonWork(2, 3), 28);
  ExpectExactBoundary(
      neverd::detail::strongJumpTableLoadRoleScanComparisonWork(3), 27);
  ExpectExactBoundary(neverd::detail::strongJumpTableProposalComparisonWork(
                          /*LeftStorageCount=*/2, /*RightStorageCount=*/3,
                          /*LeftRoleCount=*/2, /*RightRoleCount=*/3,
                          /*LeftRelocationCount=*/4,
                          /*RightRelocationCount=*/1),
                      52);
  ExpectExactBoundary(neverd::detail::provisionalRelativeProposalComparisonWork(
                          /*LeftRoleCount=*/2, /*RightRoleCount=*/3,
                          /*LeftTargetCount=*/4, /*RightTargetCount=*/1),
                      49);
  ExpectExactBoundary(neverd::detail::priorOccurrenceCertificateComparisonWork(
                          /*StorageCount=*/3, /*RoleCount=*/2),
                      52);
  ExpectExactBoundary(neverd::detail::siblingStorageIdentityComparisonWork(
                          /*LeftStorageCount=*/2, /*RightStorageCount=*/3),
                      20);
  ExpectExactBoundary(neverd::detail::disjointStorageRangeComparisonWork(
                          /*LeftStorageCount=*/2, /*RightStorageCount=*/3),
                      87);
  ExpectExactBoundary(neverd::detail::preciseGuardKeyComparisonWork(
                          /*LeftAlternativeCount=*/2,
                          /*RightAlternativeCount=*/3, /*LeftRootCount=*/4,
                          /*RightRootCount=*/1),
                      43);
  ExpectExactBoundary(neverd::detail::proofRootSetComparisonWork(4, 1), 5);
  ExpectExactBoundary(neverd::detail::indexExtensionComparisonWork(3), 6);
  ExpectExactBoundary(neverd::detail::decoupledRelayTargetLoadComparisonWork(3),
                      6);
  ExpectExactBoundary(neverd::detail::storageOwnershipComparisonWork(3), 30);
  ExpectExactBoundary(
      neverd::detail::authenticatedStorageConsumerComparisonWork(3), 24);
  ExpectExactBoundary(neverd::detail::storageEndComparisonWork(3), 27);
  ExpectExactBoundary(neverd::detail::defaultAddressSpaceLoadComparisonWork(4),
                      35);
  ExpectExactBoundary(
      neverd::detail::orderedProposalMapMergeComparisonWork(2, 3), 16);
  ExpectExactBoundary(
      neverd::detail::orderedProposalMapMergeComparisonWork(0, 0), 1);
  ExpectExactBoundary(neverd::detail::physicalCodePointerSlotScanWork(
                          /*StorageRangeCount=*/2, /*PhysicalSlotCount=*/3,
                          /*RelocationLookupWork=*/4),
                      26);
  ExpectExactBoundary(neverd::detail::staticSourceRelocationComparisonWork(
                          /*InitializerCount=*/2, /*StaticSourceCount=*/3),
                      37);
  ExpectExactBoundary(
      neverd::detail::staticSourceLowOccurrenceComparisonWork(
          /*InitializerCount=*/2, /*StaticSourceCount=*/3, /*InputCount=*/4),
      162);
  ExpectExactBoundary(neverd::detail::relocationAllowlistRefinementWork(
                          /*AllowlistCount=*/3,
                          /*DirectReadLookupWork=*/4),
                      34);
  ExpectExactBoundary(neverd::detail::scalarVectorComparisonWork(2, 3), 4);
  ExpectExactBoundary(neverd::detail::denseSlotIndexComparisonWork(
                          /*SlotCount=*/3, /*FixedWork=*/2),
                      5);
  ExpectExactBoundary(neverd::detail::maskDomainComparisonWork(
                          /*LeftCoordinateCount=*/2,
                          /*RightCoordinateCount=*/3,
                          /*LeftWitnessCount=*/2,
                          /*RightWitnessCount=*/1),
                      49);
  ExpectExactBoundary(neverd::detail::maskSuppressionSlotMaterializationWork(
                          /*RuntimeSlotCount=*/3, /*RelocationLookupWork=*/4,
                          /*ProtectedLookupWork=*/5),
                      40);
  ExpectExactBoundary(
      neverd::detail::denseSuppressionSlotMaterializationWork(
          /*RuntimeSlotCount=*/2, /*SegmentCount=*/3, /*SectionCount=*/4,
          /*RelocationLookupWork=*/5, /*ProtectedLookupWork=*/6),
      164);
  ExpectExactBoundary(neverd::detail::protectedRelocationFilterWork(
                          /*SlotCount=*/3, /*ProtectedLookupWork=*/4),
                      15);
  ExpectExactBoundary(neverd::detail::jumpTableProofRootConstructionWork(
                          /*PersistentRootCount=*/2,
                          /*PriorProposalCount=*/1,
                          /*StorageRangeCount=*/3,
                          /*ExplicitTargetCount=*/2,
                          /*SuppressibleSlotCount=*/4,
                          /*ProtectedSlotCount=*/1, /*SegmentCount=*/2,
                          /*CodePointerRelocationCount=*/5,
                          /*RelocationTargetCount=*/3,
                          /*RelocationSourceCount=*/6,
                          /*DurableRootCount=*/2),
                      418);
  ExpectExactBoundary(
      std::optional<size_t>{
          neverd::detail::kTargetRoleCertificateFixedComparisonWork},
      19);
  ExpectExactBoundary(
      neverd::detail::provisionalRelativeProposalComparisonWork(0, 0, 0, 0),
      18);

  EXPECT_FALSE(neverd::detail::provisionalRelativeProposalComparisonWork(
                   std::numeric_limits<size_t>::max(), 0, 0, 0)
                   .has_value());
  EXPECT_FALSE(neverd::detail::disjointStorageRangeComparisonWork(
                   std::numeric_limits<size_t>::max(), 2)
                   .has_value());
  EXPECT_FALSE(neverd::detail::denseSuppressionSlotMaterializationWork(
                   std::numeric_limits<size_t>::max(), 1, 1, 1, 1)
                   .has_value());
  EXPECT_FALSE(neverd::detail::maskSuppressionSlotMaterializationWork(
                   std::numeric_limits<size_t>::max(), 1, 1)
                   .has_value());
  EXPECT_FALSE(neverd::detail::protectedRelocationFilterWork(
                   1, std::numeric_limits<size_t>::max())
                   .has_value());
  EXPECT_FALSE(
      neverd::detail::jumpTableProofRootConstructionWork(
          0, 0, 0, std::numeric_limits<size_t>::max(), 1, 0, 0, 0, 0, 0, 0)
          .has_value());
}

TEST_F(JumpTableProposalLFP, UnclosedReentryKeepsRelativeSingletonUnpublished) {
  auto ImageOrErr = neverd::loadBinary(singletonClosedWorldObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  struct UnclosedFixture {
    const char *Name;
    const char *Function;
    const char *Table;
    const char *Dispatch;
  };
  for (const UnclosedFixture &Fixture :
       {UnclosedFixture{"opaque indirect reentry",
                        "jt_lfp_relative_singleton_opaque_reentry",
                        "jt_lfp_relative_singleton_opaque_reentry_table",
                        "jt_lfp_relative_singleton_opaque_reentry_dispatch"},
        UnclosedFixture{"owner-boundary fallthrough",
                        "jt_lfp_relative_singleton_owner_gap",
                        "jt_lfp_relative_singleton_owner_gap_table",
                        "jt_lfp_relative_singleton_owner_gap_dispatch"},
        UnclosedFixture{"direct-call callback reentry",
                        "jt_lfp_relative_singleton_call_reentry",
                        "jt_lfp_relative_singleton_call_reentry_table",
                        "jt_lfp_relative_singleton_call_reentry_dispatch"}}) {
    SCOPED_TRACE(Fixture.Name);
    const neverd::Symbol *Function = Image.findSymbol(Fixture.Function);
    const neverd::Symbol *Table = Image.findSymbol(Fixture.Table);
    const neverd::Symbol *Dispatch = Image.findSymbol(Fixture.Dispatch);
    ASSERT_NE(Function, nullptr);
    ASSERT_NE(Table, nullptr);
    ASSERT_NE(Dispatch, nullptr);
    ASSERT_EQ(Table->Size, 2u * sizeof(uint32_t));
    ASSERT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                            Image.RelCodeRelocSlots.end(),
                            [&](neverd::va_t Slot) {
                              return Slot >= Table->Addr &&
                                     Slot < Table->Addr + Table->Size;
                            }),
              2)
        << "the negative requires a complete authenticated relative object";

    neverd::Decoder Decoder;
    ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    const neverd::LowFunc Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);

    EXPECT_TRUE(std::none_of(Low.JumpTables.begin(), Low.JumpTables.end(),
                             [&](const neverd::JumpTable &Recovered) {
                               return Recovered.InsnAddr == Dispatch->Addr;
                             }))
        << "an unmodeled transfer may re-enter with selector one";
    size_t CallOps = 0;
    for (const neverd::LowBlock &Block : Low.Blocks)
      for (const neverd::LowOp &Op : Block.Ops)
        if (Op.Addr == Dispatch->Addr) {
          CallOps += Op.Opcode == neverd::NdOp::INDIR_CALL;
        }
    EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(Dispatch->Addr), 1u)
        << "the unclosed singleton must remain classified as an unsafe "
           "indirect branch even when final block pruning removes its op";
    EXPECT_EQ(CallOps, 0u);
    EXPECT_FALSE(Builder.hasProvisionalRelativeEdgesForTesting());
  }
}

TEST_F(JumpTableProposalLFP,
       NestedMutationTrackingBudgetExhaustionPreservesExactBranch) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol("jt_lfp_nested_relative");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder ExhaustedDecoder;
  ASSERT_TRUE(ExhaustedDecoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder ExhaustedBuilder;
  ExhaustedBuilder.setNestedMutationTrackingStageAllowanceForTesting(0);
  const neverd::LowFunc Exhausted = ExhaustedBuilder.build(
      Image, ExhaustedDecoder, Function->Addr, Function->Name);
  ASSERT_TRUE(
      ExhaustedBuilder.nestedMutationTrackingEvidenceExhaustedForTesting())
      << "the measured stage boundary must fail inside nested mutation "
         "tracking, not during an unrelated preflight; tables="
      << Exhausted.JumpTables.size()
      << " unsafe=" << Exhausted.UnsafeIndirectBranchAddresses.size();
  const neverd::va_t NestedAddr =
      ExhaustedBuilder.nestedMutationTrackingEvidenceExhaustedAddrForTesting();
  ASSERT_NE(NestedAddr, neverd::InvalidVA);

  bool SawOp = false;
  bool SawBranch = false;
  bool SawCall = false;
  for (const neverd::LowBlock &Block : Exhausted.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Addr == NestedAddr) {
        SawOp = true;
        SawBranch |= Op.Opcode == neverd::NdOp::INDIR_BR;
        SawCall |= Op.Opcode == neverd::NdOp::INDIR_CALL;
      }
  // Transaction rollback also removes the parent table edge that first made
  // this nested block reachable.  Do not promote that untrusted target to a
  // persistent CFG root merely to keep an operation in the final LowIR; if a
  // separate path retains it, however, it must remain a branch rather than be
  // rewritten as a call.
  EXPECT_TRUE(!SawOp || SawBranch);
  EXPECT_FALSE(SawCall);
  EXPECT_TRUE(Exhausted.UnsafeIndirectBranchAddresses.count(NestedAddr))
      << "a nested branch whose transaction could not be inventoried must "
         "retain exact fail-closed identity across bounded retries";
}

TEST_F(JumpTableProposalLFP,
       ExactFiniteLocalExhaustionPreservesOpaqueBranchIdentity) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol("jt_lfp_nested_relative");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  Builder.setFiniteSetSymbolEvidenceBudgetForTesting(0);
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty())
      << "local finite-symbol exhaustion must remain evidence-incomplete, "
         "not a definitive proof loss eligible for tail-call rewriting";
  EXPECT_FALSE(Builder.hasMaskFixedPointExplorationTargetsForTesting());
  EXPECT_FALSE(Builder.hasProvisionalRelativeEdgesForTesting());
}

TEST_F(JumpTableProposalLFP,
       ConstBaseGroupInventoryExhaustionPreservesOnlyClaimedTableShape) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *TableFunction =
      Image.findSymbol("jt_lfp_constbase_budget");
  const neverd::Symbol *ClaimedBranch =
      Image.findSymbol("jt_lfp_constbase_budget_claimed_branch");
  const neverd::Symbol *UnclaimedSibling =
      Image.findSymbol("jt_lfp_constbase_budget_unclaimed_sibling");
  const neverd::Symbol *CallbackFunction =
      Image.findSymbol("jt_lfp_memory_callback");
  ASSERT_NE(TableFunction, nullptr);
  ASSERT_NE(ClaimedBranch, nullptr);
  ASSERT_NE(UnclaimedSibling, nullptr);
  ASSERT_NE(CallbackFunction, nullptr);

  struct BudgetedBuild {
    neverd::LowFunc Low;
    bool LocalShapeClaimed = false;
    bool PostShapeIncomplete = false;
    neverd::va_t FirstClaimedAddr = neverd::InvalidVA;
    neverd::va_t SecondClaimedAddr = neverd::InvalidVA;
    bool ClaimedAddrOverflow = false;
  };
  auto BuildWithBudget = [&](const neverd::Symbol &Function, size_t Budget) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    BudgetedBuild Result;
    Result.Low = Builder.build(Image, Decoder, Function.Addr, Function.Name);
    Result.LocalShapeClaimed = Builder.constBaseLocalShapeClaimedForTesting();
    Result.PostShapeIncomplete =
        Builder.constBasePostShapeAnalysisIncompleteForTesting();
    Result.FirstClaimedAddr =
        Builder.constBaseFirstLocalShapeClaimedAddrForTesting();
    Result.SecondClaimedAddr =
        Builder.constBaseSecondLocalShapeClaimedAddrForTesting();
    Result.ClaimedAddrOverflow =
        Builder.constBaseLocalShapeClaimedAddrOverflowForTesting();
    return Result;
  };

  std::optional<size_t> ExhaustingBudget;
  BudgetedBuild Exhausted;
  size_t LastUnclaimedBudget = 0;
  size_t FirstClaimedBudget = 256;
  for (; FirstClaimedBudget <=
         neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
       FirstClaimedBudget *= 2) {
    const BudgetedBuild Candidate =
        BuildWithBudget(*TableFunction, FirstClaimedBudget);
    if (Candidate.LocalShapeClaimed)
      break;
    LastUnclaimedBudget = FirstClaimedBudget;
  }
  ASSERT_LE(FirstClaimedBudget,
            neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork);
  while (LastUnclaimedBudget + 1 < FirstClaimedBudget) {
    const size_t Mid =
        LastUnclaimedBudget + (FirstClaimedBudget - LastUnclaimedBudget) / 2;
    const BudgetedBuild Candidate = BuildWithBudget(*TableFunction, Mid);
    if (Candidate.LocalShapeClaimed)
      FirstClaimedBudget = Mid;
    else
      LastUnclaimedBudget = Mid;
  }
  ExhaustingBudget = FirstClaimedBudget;
  Exhausted = BuildWithBudget(*TableFunction, *ExhaustingBudget);
  ASSERT_TRUE(ExhaustingBudget.has_value())
      << "the padded fixture must expose a budget boundary after local model "
         "authentication but before whole-function group completion";
  ASSERT_TRUE(Exhausted.LocalShapeClaimed);
  ASSERT_TRUE(Exhausted.PostShapeIncomplete)
      << "the first budget that can authenticate the minimum relocation "
         "prefix must still stop before the attacker-sized full/group audit; "
         "budget="
      << *ExhaustingBudget;
  EXPECT_TRUE(Exhausted.Low.JumpTables.empty());
  ASSERT_NE(Exhausted.FirstClaimedAddr, neverd::InvalidVA);
  EXPECT_EQ(Exhausted.SecondClaimedAddr, neverd::InvalidVA);
  EXPECT_FALSE(Exhausted.ClaimedAddrOverflow);
  const neverd::Symbol *LocallyClaimed = nullptr;
  const neverd::Symbol *Other = nullptr;
  if (Exhausted.FirstClaimedAddr == ClaimedBranch->Addr) {
    LocallyClaimed = ClaimedBranch;
    Other = UnclaimedSibling;
  } else if (Exhausted.FirstClaimedAddr == UnclaimedSibling->Addr) {
    LocallyClaimed = UnclaimedSibling;
    Other = ClaimedBranch;
  }
  ASSERT_NE(LocallyClaimed, nullptr);
  ASSERT_NE(Other, nullptr);
  EXPECT_NE(findOpcodeAtAddress(Exhausted.Low, LocallyClaimed->Addr,
                                neverd::NdOp::INDIR_BR),
            nullptr);
  EXPECT_EQ(findOpcodeAtAddress(Exhausted.Low, LocallyClaimed->Addr,
                                neverd::NdOp::INDIR_CALL),
            nullptr);
  EXPECT_TRUE(
      Exhausted.Low.UnsafeIndirectBranchAddresses.count(LocallyClaimed->Addr));
  EXPECT_NE(
      findOpcodeAtAddress(Exhausted.Low, Other->Addr, neverd::NdOp::INDIR_CALL),
      nullptr);
  EXPECT_EQ(
      findOpcodeAtAddress(Exhausted.Low, Other->Addr, neverd::NdOp::INDIR_BR),
      nullptr);
  EXPECT_FALSE(Exhausted.Low.UnsafeIndirectBranchAddresses.count(Other->Addr))
      << "the sibling that did not reach its local certificate must remain "
         "eligible for callback lowering";

  const BudgetedBuild Callback =
      BuildWithBudget(*CallbackFunction, *ExhaustingBudget);
  EXPECT_FALSE(Callback.LocalShapeClaimed);
  EXPECT_FALSE(Callback.PostShapeIncomplete);
  EXPECT_TRUE(Callback.Low.JumpTables.empty());
  EXPECT_FALSE(hasOpcode(Callback.Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(Callback.Low, neverd::NdOp::INDIR_CALL))
      << "a generic memory callback must not inherit the absolute-table "
         "shape marker";
}

TEST_F(JumpTableProposalLFP,
       ConstBaseExactGroupDoesNotAuthorizeSameFunctionCallback) {
  auto ImageOrErr = neverd::loadBinary(proposalLFPObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  // A failed register-base fold used to fall through with Base==0.  Seed a
  // valid-looking relocation prefix there so this same-function callback
  // proves that only an explicitly resolved (including legitimately zero)
  // base can authenticate the early shape certificate.
  Image.CodePtrRelocSlots.insert(0);
  Image.CodePtrRelocSlots.insert(8);
  const neverd::Symbol *Function =
      Image.findSymbol("jt_lfp_mixed_callback_group");
  const neverd::Symbol *CallbackBegin =
      Image.findSymbol("jt_lfp_mixed_callback_begin");
  const neverd::Symbol *CallbackEnd =
      Image.findSymbol("jt_lfp_mixed_callback_end");
  const neverd::Symbol *IndexedCallback =
      Image.findSymbol("jt_lfp_indexed_memory_callback");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(CallbackBegin, nullptr);
  ASSERT_NE(CallbackEnd, nullptr);
  ASSERT_NE(IndexedCallback, nullptr);
  ASSERT_LT(CallbackBegin->Addr, CallbackEnd->Addr);

  const neverd::LowFunc Low = buildLow(Image, *Function);
  EXPECT_EQ(Low.JumpTables.size(), 2u);
  bool SawCallbackBranch = false;
  bool SawCallbackCall = false;
  bool SawCallbackReturn = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Addr >= CallbackBegin->Addr && Op.Addr < CallbackEnd->Addr) {
        SawCallbackBranch |= Op.Opcode == neverd::NdOp::INDIR_BR;
        SawCallbackCall |= Op.Opcode == neverd::NdOp::INDIR_CALL;
        SawCallbackReturn |= Op.Opcode == neverd::NdOp::RETURN;
      }
  EXPECT_FALSE(SawCallbackBranch);
  EXPECT_TRUE(SawCallbackCall)
      << "a callback must not borrow an absolute-table model from sibling "
         "consumers in the same authoritative function";
  EXPECT_TRUE(SawCallbackReturn)
      << "the same callback must retain ordinary tail-call CALL+RETURN "
         "lowering";
  for (neverd::va_t Addr : Low.UnsafeIndirectBranchAddresses)
    EXPECT_TRUE(Addr < CallbackBegin->Addr || Addr >= CallbackEnd->Addr);

  neverd::Decoder CallbackDecoder;
  ASSERT_TRUE(CallbackDecoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder CallbackBuilder;
  const neverd::LowFunc CallbackLow = CallbackBuilder.build(
      Image, CallbackDecoder, IndexedCallback->Addr, IndexedCallback->Name);
  EXPECT_FALSE(CallbackBuilder.constBaseLocalShapeClaimedForTesting())
      << "an unresolved register base must not borrow a relocation prefix at "
         "the legitimate VA-zero code segment";
  EXPECT_FALSE(hasOpcode(CallbackLow, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(CallbackLow, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(hasOpcode(CallbackLow, neverd::NdOp::RETURN));
  EXPECT_TRUE(CallbackLow.UnsafeIndirectBranchAddresses.empty());
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
    bool ForcedRollbackPreservedState = false;
  };
  auto BuildWithBudget = [&](size_t Budget, bool ExhaustCommitTail = false) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    Builder.setExhaustProposalStageCommitTailForTesting(ExhaustCommitTail);
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
    Result.ForcedRollbackPreservedState =
        Builder.proposalStageForcedCommitTailRollbackPreservedStateForTesting();
    return Result;
  };

  // This fixture first proposes and publishes a locally owned table, then
  // loses that proof after a newly explored case contributes a backedge.  Hit
  // the final transaction boundary directly: first-success-minus-one is not a
  // stable oracle because stricter proof accounting may legitimately move an
  // earlier candidate boundary.
  const size_t CommittingBudget =
      neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
  ASSERT_TRUE(BuildWithBudget(CommittingBudget).HasQuarantinedProposal)
      << "the fixture must reach a prior strong proposal followed by a "
         "definitive local proof loss";

  const BudgetedBuild Boundary =
      BuildWithBudget(CommittingBudget, /*ExhaustCommitTail=*/true);
  EXPECT_TRUE(Boundary.CommitTailExhausted)
      << "the one-shot hook must fail at the atomically prepaid commit tail";
  EXPECT_TRUE(Boundary.ForcedRollbackPreservedState)
      << "the forced rollback must observe the unchanged persistent "
         "quarantine set before the next graph retries";
  EXPECT_FALSE(Boundary.RollbackMutatedQuarantine)
      << "an incomplete stage must restore the quarantine set it observed at "
         "stage entry";
  EXPECT_TRUE(Boundary.HasQuarantinedProposal)
      << "a later complete retry must commit the same definitive loss";
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
