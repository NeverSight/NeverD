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
  EXPECT_FALSE(Builder.hasMaskFixedPointExplorationTargetsForTesting());
  EXPECT_EQ(Low.JumpTables.size(), 2u)
      << "the one-shot failed stage must be retryable from clean state";
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
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
  EXPECT_NE(findOpcodeAtAddress(Exhausted.Low, ClaimedBranch->Addr,
                                neverd::NdOp::INDIR_BR),
            nullptr);
  EXPECT_EQ(findOpcodeAtAddress(Exhausted.Low, ClaimedBranch->Addr,
                                neverd::NdOp::INDIR_CALL),
            nullptr);
  EXPECT_TRUE(
      Exhausted.Low.UnsafeIndirectBranchAddresses.count(ClaimedBranch->Addr))
      << "the exact branch whose local relocation model was claimed must stay "
         "fail-closed when its group inventory runs out";
  EXPECT_NE(findOpcodeAtAddress(Exhausted.Low, UnclaimedSibling->Addr,
                                neverd::NdOp::INDIR_CALL),
            nullptr);
  EXPECT_EQ(findOpcodeAtAddress(Exhausted.Low, UnclaimedSibling->Addr,
                                neverd::NdOp::INDIR_BR),
            nullptr);
  EXPECT_FALSE(
      Exhausted.Low.UnsafeIndirectBranchAddresses.count(UnclaimedSibling->Addr))
      << "a sibling not reached by the bounded inventory owns no borrowed "
         "shape certificate and remains eligible for callback lowering";

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
        Builder
            .proposalStageForcedCommitTailRollbackPreservedStateForTesting();
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
