//===- JumpTableResolver.cpp - Jump table detection and resolution --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Jump table resolution from indirect branch patterns and metadata
/// extraction into LowFunc::JumpTables.
///
/// The resolver uses a multi-strategy approach with fallback:
///
///   1. **ARM-family detectors** — the architecture-gated recognizers for
///      the ARM TBB/TBH table-branch and the AArch64 compact byte/halfword
///      table.  These are the only target-specific strategies and live in
///      JumpTableResolverARM.cpp; every strategy below is architecture-neutral.
///
///   2. **PIC-relative tables** — handle the common x64 pattern where
///      each table entry is a 32-bit signed offset from the table base:
///        target = base + (int32_t)table[index]
///
///   3. **Symbolic dispatch decomposition** — execute the dispatch with each
///      register input symbolic and recover an exact linear table shape.
///
///   4. **Backward slicing** — trace data flow from the INDIR_BR input
///      through INT_ADD, INT_MULT, LOAD, INT_ZEXT, INT_LEFT, INT_RIGHT,
///      INT_ASHR, INT_SEXT, SUBBYTES, and COPY to identify the base
///      address and entry layout.  Cross-instruction base recovery lives in
///      JumpTableResolverSource.cpp, stack-materialized sources in
///      JumpTableResolverStack.cpp, and composite layouts in
///      JumpTableResolverShapes.cpp.
///
///   5. **Guard analysis** — scan preceding instructions *and* CFG
///      predecessor blocks for comparison/mask ops (INT_LESS,
///      INT_LESSEQUAL, INT_SUB, INT_AND) that bound the switch
///      variable, giving a precise entry count.  Lives in
///      JumpTableResolverGuards.cpp.
///
///   6. **Multi-format entries** — read 1, 2, 4, or 8 byte entries,
///      both signed and unsigned, with tolerance for sparse invalid
///      entries in bounded tables.
///
///   7. **Sanity validation** — each target is checked for executable
///      segment membership, data availability at the target address,
///      reasonable distance from the function, and duplicate-run limits.
///
///   8. **Multi-stage fallback** — when the primary strategy produces
///      too few entries, retry with alternative entry sizes to recover
///      tables that use an unexpected format.  Path collection and dispatch
///      emulation fallbacks live in JumpTableResolverEmu.cpp.
///
/// This file holds the strategy dispatch itself.  The framework pieces it
/// drives are split by responsibility across sibling translation units:
/// backward slicing in JumpTableResolverSlice.cpp, table-base constant folding
/// in JumpTableResolverFold.cpp, index normalization and stride in
/// JumpTableResolverNorm.cpp, guard-free entry-count bounds in
/// JumpTableResolverBounds.cpp, entry decoding and target validation in
/// JumpTableResolverTargets.cpp, and case-label / metadata construction in
/// JumpTableResolverExtract.cpp.
///
/// See CFGBuilder.cpp for the main CFG construction logic.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/symbolic/SymDispatch.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

bool jumpTableTargetLoadUsesDefaultAddressSpace(llvm::ArrayRef<LowOp> Ops,
                                                va_t Address, int Sequence,
                                                const NdVar &Output) {
  bool Found = false;
  bool UsesDefaultAddressSpace = false;
  for (const LowOp &Op : Ops) {
    bool Matches = true;
    Matches &= Op.Addr == Address;
    Matches &= Op.Seq == Sequence;
    Matches &= Op.Opcode == NdOp::LOAD;
    Matches &= Op.Output.Space == Output.Space;
    Matches &= Op.Output.Offset == Output.Offset;
    Matches &= Op.Output.Size == Output.Size;
    Matches &= Op.Output.Provenance == Output.Provenance;
    Matches &= Op.Output.AddressOwnerVA == Output.AddressOwnerVA;
    if (Matches && !Found) {
      Found = true;
      UsesDefaultAddressSpace =
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
    }
  }
  return Found && UsesDefaultAddressSpace;
}

//===----------------------------------------------------------------------===//
// ARM-family target detectors (tryARMTableBranch, tryAArch64CompactTable) live
// in JumpTableResolverARM.cpp.  They are the only architecture-gated table
// recognizers; every other strategy -- the guard/bounds analysis in
// JumpTableResolverGuards.cpp, source/stack/shape detectors in their dedicated
// JumpTableResolver*.cpp files, and the framework below -- is pattern-based and
// architecture-neutral, so there is no corresponding x86 detector to split
// out (LLVM target-dispatch pattern).
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// resolveJumpTable — top-level multi-strategy resolution
//===----------------------------------------------------------------------===//

std::set<va_t> CFGBuilder::jumpTableProofRoots(
    const JumpTableInfo &Info,
    const std::set<va_t> *DecodedTableAnchorsOverride) const {
  std::set<va_t> Roots = PersistentCFGRoots;
  if (!CurrentImg || RelocationCFGRootSources.empty())
    return Roots;
  std::set<va_t> OwnedDecodedTableAnchors;
  if (!DecodedTableAnchorsOverride) {
    OwnedDecodedTableAnchors =
        currentRelocatedInstructionTableAnchors(*CurrentImg);
    DecodedTableAnchorsOverride = &OwnedDecodedTableAnchors;
  }
  const std::set<va_t> &DecodedTableAnchors = *DecodedTableAnchorsOverride;

  std::vector<JumpTableStorageRange> CandidateStorage = Info.StorageRanges;
  if (CandidateStorage.empty() && Info.HasBaseAddr && Info.EntrySize != 0 &&
      Info.PhysicalCapacity != 0 && Info.RelocAbsolute) {
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride >= Info.EntrySize &&
        codePtrRelocRunHasExactBoundary(*CurrentImg, Info.BaseAddr,
                                        PhysicalStride, Info.PhysicalCapacity,
                                        DecodedTableAnchors))
      CandidateStorage.push_back(
          JumpTableStorageRange{Info.BaseAddr, Info.EntrySize, PhysicalStride,
                                Info.PhysicalCapacity});
  }
  for (const auto &[Addr, Proposal] : PriorStrongJumpTableProposals) {
    if (Addr == ActiveJumpTableCandidateAddr ||
        (!ActiveJumpTableConsumerAudit &&
         Proposal.ProofRank >= ActiveJumpTableCandidateDependencyRank))
      continue;
    CandidateStorage.insert(CandidateStorage.end(),
                            Proposal.StorageRanges.begin(),
                            Proposal.StorageRanges.end());
  }
  if (CandidateStorage.empty())
    return Roots;

  std::set<va_t> CandidateTargets(Info.ExplicitTargets.begin(),
                                  Info.ExplicitTargets.end());
  // Physical ownership and relocation suppression are deliberately separate.
  // A sparse dispatch may own one object containing compiler filler while a
  // reachable second consumer still needs one filler relocation as a CFG root
  // and LLVM pointer-mirror field.  Only the candidate-local allowlist may
  // remove such a root; StorageRanges alone never grants that permission.
  std::set<va_t> SuppressibleSlots(Info.SuppressibleRelocationSlots.begin(),
                                   Info.SuppressibleRelocationSlots.end());
  for (const auto &[Addr, Proposal] : PriorStrongJumpTableProposals) {
    if (Addr == ActiveJumpTableCandidateAddr ||
        (!ActiveJumpTableConsumerAudit &&
         Proposal.ProofRank >= ActiveJumpTableCandidateDependencyRank))
      continue;
    SuppressibleSlots.insert(Proposal.SuppressibleRelocationSlots.begin(),
                             Proposal.SuppressibleRelocationSlots.end());
  }
  if (ProtectedJumpTableRelocationSlots)
    for (va_t Slot : *ProtectedJumpTableRelocationSlots)
      SuppressibleSlots.erase(Slot);

  auto ownsWholeRelocationSlot = [&](va_t Slot) {
    const uint32_t PointerSize = CurrentImg->getPointerSize();
    return PointerSize != 0 &&
           std::any_of(
               CandidateStorage.begin(), CandidateStorage.end(),
               [&](const JumpTableStorageRange &Range) {
                 if (Range.EntrySize < PointerSize ||
                     Range.EntryStride < Range.EntrySize ||
                     Range.PhysicalSlotCount == 0 || Slot < Range.BaseAddr)
                   return false;
                 const uint64_t Delta = Slot - Range.BaseAddr;
                 return Delta % Range.EntryStride == 0 &&
                        Delta / Range.EntryStride < Range.PhysicalSlotCount;
               });
  };

  size_t CandidateSlotBudget = limits::kMaxJumpTableEntries;
  for (va_t Slot : SuppressibleSlots) {
    if (CandidateSlotBudget-- == 0 ||
        !CurrentImg->CodePtrRelocSlots.count(Slot) ||
        !ownsWholeRelocationSlot(Slot))
      return Roots;
    const uint8_t *P = CurrentImg->readVA(Slot, CurrentImg->getPointerSize());
    if (!P)
      return Roots;
    CandidateTargets.insert(normalizeCodeAddress(
        readPtr(P, CurrentImg->is64Bit()), CurrentImg->Arch, CurrentImg->Mode));
  }
  if (CandidateTargets.empty())
    return Roots;

  for (const auto &[Target, Sources] : RelocationCFGRootSources) {
    if (DurableCFGRoots.count(Target) || !CandidateTargets.count(Target) ||
        Sources.empty())
      continue;
    if (std::all_of(Sources.begin(), Sources.end(), [&](va_t Slot) {
          return SuppressibleSlots.count(Slot) && ownsWholeRelocationSlot(Slot);
        }))
      Roots.erase(Target);
  }
  return Roots;
}

std::vector<va_t> CFGBuilder::resolveJumpTable(const BinaryImage &Img,
                                               const InsnRecord &Rec) {
  // The stage-start candidate vector cannot contain indirect branches decoded
  // recursively while another candidate explores its provisional targets.
  // Register each such invocation before the resolver erases or publishes any
  // persistent state.  The shared stage account prepays both the tracking node
  // and every lookup/cleanup the eventual rollback will perform.
  if (CandidateProposalStageActive && !CandidateProposalOutcomeTracked) {
    if (NestedMutationTrackingStageAllowanceForTesting)
      CandidateProposalStageEvidenceRemaining =
          std::min(CandidateProposalStageEvidenceRemaining,
                   *NestedMutationTrackingStageAllowanceForTesting);
    auto OrderedLookupWork = [](size_t Count) {
      size_t Work = 1;
      for (size_t N = Count; N > 1; N = N / 2 + N % 2)
        ++Work;
      return Work;
    };
    auto ConsumeStageWork = [&](size_t Amount) {
      if (Amount > CandidateProposalStageEvidenceRemaining) {
        CandidateProposalStageEvidenceRemaining = 0;
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      CandidateProposalStageEvidenceRemaining -= Amount;
      return true;
    };
    auto PreserveNestedIncompleteBranchIdentity = [&]() {
      const size_t Lookup =
          OrderedLookupWork(IndexDomainEvidenceIncompleteBranches.size());
      constexpr size_t NodeAndCleanupWork = 2;
      const size_t Max = std::numeric_limits<size_t>::max();
      const size_t MarkerWork =
          Lookup > Max - NodeAndCleanupWork ? Max : Lookup + NodeAndCleanupWork;
      if (consumeIncompleteBranchMarkerEvidence(MarkerWork))
        IndexDomainEvidenceIncompleteBranches.insert(Rec.Addr);
    };
    auto FailNestedMutationTracking = [&]() {
      CandidateProposalStageEvidenceRemaining = 0;
      CandidateProposalStageEvidenceIncomplete = true;
      NestedMutationTrackingEvidenceExhaustedForTesting = true;
      NestedMutationTrackingEvidenceExhaustedAddrForTesting = Rec.Addr;
      PreserveNestedIncompleteBranchIdentity();
    };
    if (CandidateProposalStageMutationAddrs.size() ==
        std::numeric_limits<size_t>::max()) {
      FailNestedMutationTracking();
      return {};
    }
    const size_t TrackLookup =
        OrderedLookupWork(CandidateProposalStageMutationAddrs.size());
    if (!ConsumeStageWork(TrackLookup)) {
      FailNestedMutationTracking();
      return {};
    }
    if (!CandidateProposalStageMutationAddrs.count(Rec.Addr)) {
      const size_t Max = std::numeric_limits<size_t>::max();
      const size_t RollbackLookupCeiling = OrderedLookupWork(Max);
      const size_t MarkerRollbackLookup = RollbackLookupCeiling + 1;
      const std::array<size_t, 9> Terms{
          // Ordered insertion plus source/node/future destruction.
          OrderedLookupWork(CandidateProposalStageMutationAddrs.size() + 1) + 3,
          RollbackLookupCeiling, RollbackLookupCeiling, RollbackLookupCeiling,
          // The forced transaction gate performs one exact resolved-state
          // lookup after rollback; the exploration lookup above likewise
          // covers its exact post-clear observation.
          RollbackLookupCeiling,
          // Target clear plus the two ordered erases.
          3,
          // Stack/index marker merge and duplicate-node cleanup.
          MarkerRollbackLookup, MarkerRollbackLookup, 2};
      size_t RollbackReservation = 0;
      for (size_t Term : Terms) {
        if (Term > Max - RollbackReservation) {
          FailNestedMutationTracking();
          return {};
        }
        RollbackReservation += Term;
      }
      if (!ConsumeStageWork(RollbackReservation)) {
        FailNestedMutationTracking();
        return {};
      }
      CandidateProposalStageMutationAddrs.insert(Rec.Addr);
    }
  }
  // A revalidation must never leave metadata from the previously published
  // target set behind when the new proof fails or shrinks.
  ResolvedTableInfo.erase(Rec.Addr);
  RequestedCompleteJumpTableProof = false;
  ActiveJumpTableProofRoots.reset();
  if (CandidateProposalStageActive &&
      QuarantinedJumpTableProposals.count(Rec.Addr))
    return {};
  struct ActiveCandidateScope {
    va_t &AddrSlot;
    uint32_t &RankSlot;
    uint32_t &DependencyRankSlot;
    bool &ConsumerAuditSlot;
    const va_t SavedAddr;
    const uint32_t SavedRank;
    const uint32_t SavedDependencyRank;
    const bool SavedConsumerAudit;

    ActiveCandidateScope(va_t &AddrSlot, uint32_t &RankSlot,
                         uint32_t &DependencyRankSlot, bool &ConsumerAuditSlot,
                         va_t Addr)
        : AddrSlot(AddrSlot), RankSlot(RankSlot),
          DependencyRankSlot(DependencyRankSlot),
          ConsumerAuditSlot(ConsumerAuditSlot), SavedAddr(AddrSlot),
          SavedRank(RankSlot), SavedDependencyRank(DependencyRankSlot),
          SavedConsumerAudit(ConsumerAuditSlot) {
      AddrSlot = Addr;
      RankSlot = 0;
      DependencyRankSlot = 0;
      ConsumerAuditSlot = false;
    }
    ~ActiveCandidateScope() {
      AddrSlot = SavedAddr;
      RankSlot = SavedRank;
      DependencyRankSlot = SavedDependencyRank;
      ConsumerAuditSlot = SavedConsumerAudit;
    }
  } CandidateScope{ActiveJumpTableCandidateAddr,
                   ActiveJumpTableCandidateProofRank,
                   ActiveJumpTableCandidateDependencyRank,
                   ActiveJumpTableConsumerAudit, Rec.Addr};
  const size_t RequestedI386GOTOFFEvidenceBudget =
      std::min<size_t>(limits::kMaxI386GOTOFFProposalEvidenceWork,
                       I386GOTOFFProposalEvidenceBudgetForTesting.value_or(
                           limits::kMaxI386GOTOFFProposalEvidenceWork));
  I386GOTOFFProposalEvidenceRemaining = RequestedI386GOTOFFEvidenceBudget;
  I386GOTOFFProposalShapeClaimed = false;
  I386GOTOFFProposalEvidenceIncomplete = false;
  I386GOTOFFAmbiguousModelReach = false;
  I386GOTOFFPrivateFrameModelAuthenticated = false;
  CurrentI386GOTOFFAmbiguityKeys.clear();
  const size_t CandidateEvidenceLimit =
      std::min<size_t>(limits::kMaxJumpTableMaskFixedPointEvidenceWork,
                       MaskFixedPointEvidenceBudgetForTesting.value_or(
                           limits::kMaxJumpTableMaskFixedPointEvidenceWork));
  const size_t InitialCandidateEvidenceBudget =
      CandidateProposalStageActive
          ? std::min(CandidateEvidenceLimit,
                     CandidateProposalStageEvidenceRemaining)
          : CandidateEvidenceLimit;
  size_t CandidateEvidenceBudget = InitialCandidateEvidenceBudget;
  JumpTableInfo Info;
  JumpTableExactConsumerGroup ExactConsumerGroup;
  bool CandidateEvidenceShapeClaimed = false;
  bool CandidateEvidencePublished = false;
  bool CandidateEvidenceAnalysisIncomplete = false;
  bool CandidateEvidenceChargeFailed = false;
  bool CandidateEvidenceExhaustedBeforeDedicatedRefund = false;
  bool IncompleteBranchInsertPrepaid = false;
  bool IncompleteBranchAlreadyInserted = false;
  bool CandidateTargetMaterializationStarted = false;
  bool CandidateValidatedPhysicalTableIdentity = false;
  bool CurrentCandidateIsStrongProposal = false;
  bool CandidateStrongProposalRecorded = false;
  bool CurrentCandidateHasPriorProvisionalRelativeEdge = false;
  bool CurrentCandidateMayReplayOwnProvisionalRelativeEdge = false;
  bool CurrentCandidateReplayedOwnProvisionalRelativeEdge = false;
  bool CandidateCompleteRuntimeCertificateRecorded = false;
  bool CandidateAwaitingSiblingRuntimeCertificate = false;
  struct CandidateEvidenceOutcome {
    std::set<va_t> &IncompleteBranches;
    va_t BranchAddr;
    const size_t Initial;
    const size_t &Remaining;
    const bool &ShapeClaimed;
    const bool &I386GOTOFFShapeClaimed;
    const bool &Published;
    const bool &AnalysisIncomplete;
    const bool &I386GOTOFFIncomplete;
    const bool &I386GOTOFFSemanticAmbiguous;
    const bool &ChargeFailed;
    const bool &ExhaustedBeforeDedicatedRefund;
    const bool &IncompleteInsertPrepaid;
    const bool &IncompleteAlreadyInserted;
    const bool &StrongProposalRecorded;
    const bool &ReplayedOwnProvisionalRelativeEdge;
    const bool &CompleteRuntimeCertificateRecorded;
    const bool &AwaitingSiblingRuntimeCertificate;
    const bool StageActive;
    const bool OutcomeTracked;
    size_t &StageRemaining;
    bool &StageIncomplete;
    std::map<va_t, StrongJumpTableProposalOutcome> &ProposalOutcomes;

    ~CandidateEvidenceOutcome() {
      const bool ResourceIncomplete = I386GOTOFFIncomplete || ChargeFailed ||
                                      ExhaustedBeforeDedicatedRefund ||
                                      (!Published && Remaining == 0);
      const bool EvidenceIncomplete = AnalysisIncomplete || ResourceIncomplete;
      // Resource exhaustion cannot distinguish a callback-shaped tail jump
      // from a table whose proof ran out of work, so preserve the original
      // branch fail-closed.  A completed semantic rejection is different: it
      // may describe a real callback table and receives unsafe branch identity
      // only from the explicit full-object/local-target ownership proof below.
      if ((ShapeClaimed || I386GOTOFFShapeClaimed) && !Published &&
          EvidenceIncomplete && IncompleteInsertPrepaid &&
          !IncompleteAlreadyInserted)
        IncompleteBranches.insert(BranchAddr);
      else if (!StageActive && (Published || !EvidenceIncomplete))
        IncompleteBranches.erase(BranchAddr);
      if (!StageActive)
        return;
      if (Remaining > Initial || Initial - Remaining > StageRemaining) {
        StageRemaining = 0;
        StageIncomplete = true;
      } else {
        StageRemaining -= Initial - Remaining;
      }
      // A nested probe is allowed to defer candidate-local graph/semantic
      // incompleteness until that branch joins the next immutable inventory.
      // It is not allowed to hide real resource exhaustion: the probe shares
      // this stage's account and may already have produced provisional graph
      // or resolver metadata that must be rolled back transactionally.
      StageIncomplete |= ResourceIncomplete;
      if (!OutcomeTracked) {
        // Recursive descent can discover a new indirect branch after the
        // stage-start outcome inventory was frozen.  It still consumes the
        // shared stage account.  Any actual aggregate-account exhaustion was
        // handled above; candidate-local incompleteness merely defers the new
        // branch until the next immutable stage inventories it.
        return;
      }
      auto It = ProposalOutcomes.find(BranchAddr);
      if (It == ProposalOutcomes.end()) {
        StageIncomplete = true;
        return;
      }
      if (I386GOTOFFIncomplete || (!Published && EvidenceIncomplete)) {
        It->second = StrongJumpTableProposalOutcome::EvidenceIncomplete;
        // Resource exhaustion invalidates the shared account immediately.
        // Graph/semantic incompleteness is still recorded as an incomplete
        // outcome, but the frozen candidate inventory must finish: a later
        // candidate can authorize the graph growth needed by an earlier one.
        // Reconciliation below will roll the whole stage back before commit.
      } else if (I386GOTOFFSemanticAmbiguous) {
        It->second = StrongJumpTableProposalOutcome::SemanticOpaque;
      } else if (StrongProposalRecorded) {
        // A self-replay may retire its one-shot edge without closing a selector
        // domain while still re-establishing the independently authenticated
        // storage/LOAD role.  Preserve that strong fact for a distinct sibling
        // to consume in the next immutable universe; the raw edge itself is
        // still not renewed.  Treating the whole candidate as a definitive
        // loss here makes candidate address order decide same-table closure.
        It->second = StrongJumpTableProposalOutcome::StrongProposed;
      } else if (AwaitingSiblingRuntimeCertificate && !Published &&
                 !CompleteRuntimeCertificateRecorded) {
        It->second =
            StrongJumpTableProposalOutcome::AwaitingSiblingRuntimeCertificate;
      } else if (ReplayedOwnProvisionalRelativeEdge && !Published &&
                 !CompleteRuntimeCertificateRecorded) {
        It->second =
            StrongJumpTableProposalOutcome::SelfReplayDefinitiveLocalProofLoss;
      } else {
        It->second = StrongJumpTableProposalOutcome::DefinitiveLocalProofLoss;
      }
    }
  } CandidateOutcome{IndexDomainEvidenceIncompleteBranches,
                     Rec.Addr,
                     InitialCandidateEvidenceBudget,
                     CandidateEvidenceBudget,
                     CandidateEvidenceShapeClaimed,
                     I386GOTOFFProposalShapeClaimed,
                     CandidateEvidencePublished,
                     CandidateEvidenceAnalysisIncomplete,
                     I386GOTOFFProposalEvidenceIncomplete,
                     I386GOTOFFAmbiguousModelReach,
                     CandidateEvidenceChargeFailed,
                     CandidateEvidenceExhaustedBeforeDedicatedRefund,
                     IncompleteBranchInsertPrepaid,
                     IncompleteBranchAlreadyInserted,
                     CandidateStrongProposalRecorded,
                     CurrentCandidateReplayedOwnProvisionalRelativeEdge,
                     CandidateCompleteRuntimeCertificateRecorded,
                     CandidateAwaitingSiblingRuntimeCertificate,
                     CandidateProposalStageActive,
                     CandidateProposalOutcomeTracked,
                     CandidateProposalStageEvidenceRemaining,
                     CandidateProposalStageEvidenceIncomplete,
                     StrongJumpTableProposalOutcomes};
  const bool ForceUntrackedResourceExhaustion =
      CandidateProposalStageActive && !CandidateProposalOutcomeTracked &&
      ExhaustUntrackedJumpTableCandidateForTesting;
  bool ForcedResolvedMutationPrepaid = false;
  if (ForceUntrackedResourceExhaustion) {
    ExhaustUntrackedJumpTableCandidateForTesting = false;
    ForcedUntrackedJumpTableCandidateAddrForTesting = Rec.Addr;
    if (ResolvedTableInfo.size() == std::numeric_limits<size_t>::max()) {
      CandidateEvidenceBudget = 0;
      CandidateEvidenceChargeFailed = true;
    } else {
      size_t LookupWork = 1;
      for (size_t N = ResolvedTableInfo.size() + 1; N > 1; N = N / 2 + N % 2)
        ++LookupWork;
      constexpr size_t NodeAndCleanupWork = 3;
      if (LookupWork <=
              std::numeric_limits<size_t>::max() - NodeAndCleanupWork &&
          LookupWork + NodeAndCleanupWork <= CandidateEvidenceBudget) {
        CandidateEvidenceBudget -= LookupWork + NodeAndCleanupWork;
        ForcedResolvedMutationPrepaid = true;
      } else {
        CandidateEvidenceBudget = 0;
        CandidateEvidenceChargeFailed = true;
      }
    }
  }
  // Run the real nested probe first so the transaction test exercises the
  // exact post-mutation seam.  This guard is declared after CandidateOutcome,
  // therefore it marks resource exhaustion before that outcome is settled.
  struct ForcedUntrackedResourceExhaustionGuard {
    const bool Active;
    bool &ChargeFailed;
    size_t &Remaining;
    bool &ExhaustedThisStage;
    const bool &ResolvedMutationPrepaid;
    std::map<va_t, JumpTableInfo> &ResolvedInfo;
    va_t BranchAddr;
    bool &ProvisionalStateObserved;
    ~ForcedUntrackedResourceExhaustionGuard() {
      if (!Active)
        return;
      if (ResolvedMutationPrepaid) {
        // The real eager resolver runs first.  If that graph shape correctly
        // defers publication, inject one prepaid provisional metadata node at
        // the same post-resolver seam so rollback is still required to prove
        // exact cleanup rather than passing on an empty state.
        ResolvedInfo.try_emplace(BranchAddr);
        ProvisionalStateObserved = true;
      }
      ChargeFailed = true;
      Remaining = 0;
      ExhaustedThisStage = true;
    }
  } ForceUntrackedResourceExhaustionOnExit{
      ForceUntrackedResourceExhaustion,
      CandidateEvidenceChargeFailed,
      CandidateEvidenceBudget,
      UntrackedJumpTableCandidateExhaustedThisStageForTesting,
      ForcedResolvedMutationPrepaid,
      ResolvedTableInfo,
      Rec.Addr,
      UntrackedJumpTableCandidateProvisionalStateObservedForTesting};
  auto consumeCandidateEvidence = [&](size_t Amount = 1) {
    if (Amount > CandidateEvidenceBudget) {
      CandidateEvidenceBudget = 0;
      CandidateEvidenceChargeFailed = true;
      return false;
    }
    CandidateEvidenceBudget -= Amount;
    return true;
  };
  auto consumeCandidateProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count)
            return consumeCandidateEvidence(Max);
          const size_t Product = Count * Cost;
          if (Product > Max - Total)
            return consumeCandidateEvidence(Max);
          Total += Product;
        }
        return consumeCandidateEvidence(Total);
      };
  auto consumeCandidateFactorProduct =
      [&](std::initializer_list<size_t> Factors) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Product = 1;
        for (size_t Factor : Factors) {
          if (Factor != 0 && Product > Max / Factor)
            return consumeCandidateEvidence(Max);
          Product *= Factor;
        }
        return consumeCandidateEvidence(Product);
      };
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = (N + 1) / 2)
      ++Work;
    return Work;
  };
  auto sameStorageRangeNoShortCircuit = [](const JumpTableStorageRange &Left,
                                           const JumpTableStorageRange &Right) {
    bool Same = true;
    Same &= Left.BaseAddr == Right.BaseAddr;
    Same &= Left.EntrySize == Right.EntrySize;
    Same &= Left.EntryStride == Right.EntryStride;
    Same &= Left.PhysicalSlotCount == Right.PhysicalSlotCount;
    return Same;
  };
  auto sameOccurrenceNoShortCircuit =
      [](const JumpTableValueOccurrence &Left,
         const JumpTableValueOccurrence &Right) {
        bool Same = true;
        Same &= Left.Value.Space == Right.Value.Space;
        Same &= Left.Value.Offset == Right.Value.Offset;
        Same &= Left.Value.Size == Right.Value.Size;
        Same &= Left.Value.Provenance == Right.Value.Provenance;
        Same &= Left.Value.AddressOwnerVA == Right.Value.AddressOwnerVA;
        Same &= Left.Addr == Right.Addr;
        Same &= Left.Seq == Right.Seq;
        Same &= Left.DefinedAtPoint == Right.DefinedAtPoint;
        return Same;
      };
  auto sameMaskWitnessNoShortCircuit =
      [&](const JumpTableMaskKnownOneWitness &Left,
          const JumpTableMaskKnownOneWitness &Right) {
        bool Same = true;
        Same &= sameOccurrenceNoShortCircuit(Left.OrOutput, Right.OrOutput);
        Same &= sameOccurrenceNoShortCircuit(Left.MaskOutput, Right.MaskOutput);
        Same &= Left.ConstantOperand.Space == Right.ConstantOperand.Space;
        Same &= Left.ConstantOperand.Offset == Right.ConstantOperand.Offset;
        Same &= Left.ConstantOperand.Size == Right.ConstantOperand.Size;
        Same &=
            Left.ConstantOperand.Provenance == Right.ConstantOperand.Provenance;
        Same &= Left.ConstantOperand.AddressOwnerVA ==
                Right.ConstantOperand.AddressOwnerVA;
        Same &= Left.KnownOneBits == Right.KnownOneBits;
        return Same;
      };
  auto sameLoadRoleNoShortCircuit = [](const auto &Left, const auto &Right) {
    bool Same = true;
    Same &= Left.Load.Value.Space == Right.Load.Value.Space;
    Same &= Left.Load.Value.Offset == Right.Load.Value.Offset;
    Same &= Left.Load.Value.Size == Right.Load.Value.Size;
    Same &= Left.Load.Value.Provenance == Right.Load.Value.Provenance;
    Same &= Left.Load.Value.AddressOwnerVA == Right.Load.Value.AddressOwnerVA;
    Same &= Left.Load.Addr == Right.Load.Addr;
    Same &= Left.Load.Seq == Right.Load.Seq;
    Same &= Left.Load.DefinedAtPoint == Right.Load.DefinedAtPoint;
    Same &= Left.LoadWidth == Right.LoadWidth;
    return Same;
  };
  auto sameLoadRoleVectorsNoShortCircuit = [&](const auto &Left,
                                               const auto &Right) {
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::min(Left.size(), Right.size()); ++I)
      Same &= sameLoadRoleNoShortCircuit(Left[I], Right[I]);
    return Same;
  };
  auto sameStorageRangeVectorsNoShortCircuit = [&](const auto &Left,
                                                   const auto &Right) {
    bool Same = Left.size() == Right.size();
    for (size_t I = 0; I < std::min(Left.size(), Right.size()); ++I)
      Same &= sameStorageRangeNoShortCircuit(Left[I], Right[I]);
    return Same;
  };
  // Reserve the exact branch-scoped incomplete marker before any nested proof
  // can exhaust its allowance.  This bookkeeping must not debit the
  // user-overridable candidate proof budget: budget zero means "no proof", not
  // "forget which branch failed closed".  Every invocation uses this separate
  // bounded account, including indirect branches discovered recursively while
  // a proposal stage is active; those nested candidates are not members of the
  // stage's initial inventory.  The stage rollback reserve is intentionally an
  // additional conservative bound.  If this account is exhausted, a scalar
  // function-level bit preserves all remaining indirect jumps without
  // allocating after exhaustion.
  const size_t LookupWork =
      orderedLookupWork(IndexDomainEvidenceIncompleteBranches.size());
  constexpr size_t NodeAndCleanupWork = 2;
  const size_t MarkerWork =
      LookupWork > std::numeric_limits<size_t>::max() - NodeAndCleanupWork
          ? std::numeric_limits<size_t>::max()
          : LookupWork + NodeAndCleanupWork;
  if (!consumeIncompleteBranchMarkerEvidence(MarkerWork))
    return {};
  IncompleteBranchInsertPrepaid = true;
  auto insertIncompleteBranchOnce = [&]() {
    if (IncompleteBranchAlreadyInserted)
      return;
    IndexDomainEvidenceIncompleteBranches.insert(Rec.Addr);
    IncompleteBranchAlreadyInserted = true;
  };
  // Reserve the nested i386 allowance from the candidate/stage aggregate
  // before the query can traverse the graph or publish cache/set state.  The
  // dedicated value-match cap covers both exact proposal bookkeeping and the
  // bounded whole-CFG query; the scope refunds only the unused tail.
  const size_t InitialI386GOTOFFEvidenceBudget =
      std::min(RequestedI386GOTOFFEvidenceBudget, CandidateEvidenceBudget);
  CandidateEvidenceBudget -= InitialI386GOTOFFEvidenceBudget;
  I386GOTOFFProposalEvidenceRemaining = InitialI386GOTOFFEvidenceBudget;
  struct CandidateI386GOTOFFEvidenceCharge {
    const size_t Initial;
    const size_t &Remaining;
    size_t &CandidateRemaining;
    bool &CandidateChargeFailed;
    bool &CandidateExhaustedBeforeDedicatedRefund;

    ~CandidateI386GOTOFFEvidenceCharge() {
      // The generic candidate account may be consumed exactly, without an
      // overdraw, while this architecture-specific reservation remains
      // unused.  Preserve that exhaustion fact before refunding the unused
      // tail; CandidateEvidenceOutcome must not mistake the post-refund
      // balance for a complete unpublished proof.
      CandidateExhaustedBeforeDedicatedRefund |= CandidateRemaining == 0;
      if (Remaining > Initial ||
          Remaining > std::numeric_limits<size_t>::max() - CandidateRemaining) {
        CandidateRemaining = 0;
        CandidateChargeFailed = true;
        return;
      }
      CandidateRemaining += Remaining;
    }
  } CandidateI386Charge{InitialI386GOTOFFEvidenceBudget,
                        I386GOTOFFProposalEvidenceRemaining,
                        CandidateEvidenceBudget, CandidateEvidenceChargeFailed,
                        CandidateEvidenceExhaustedBeforeDedicatedRefund};
  if (CandidateProposalStageActive) {
    if (!consumeCandidateEvidence(PriorStrongJumpTableProposals.size()) ||
        !consumeCandidateEvidence(PriorProvisionalRelativeEdges.size()) ||
        !consumeCandidateEvidence(
            orderedLookupWork(PriorStrongJumpTableProposals.size())) ||
        !consumeCandidateEvidence(
            orderedLookupWork(PriorProvisionalRelativeEdges.size())))
      return {};
    const auto CurrentStrong = PriorStrongJumpTableProposals.find(Rec.Addr);
    const auto CurrentEdge = PriorProvisionalRelativeEdges.find(Rec.Addr);
    CurrentCandidateHasPriorProvisionalRelativeEdge =
        CurrentEdge != PriorProvisionalRelativeEdges.end();
    if (CurrentStrong != PriorStrongJumpTableProposals.end() ||
        CurrentEdge != PriorProvisionalRelativeEdges.end()) {
      if (CurrentStrong != PriorStrongJumpTableProposals.end() &&
          CurrentEdge != PriorProvisionalRelativeEdges.end() &&
          CurrentStrong->second.ProofRank != CurrentEdge->second.ProofRank) {
        CandidateEvidenceAnalysisIncomplete = true;
        return {};
      }
      const uint32_t PriorRank =
          CurrentStrong != PriorStrongJumpTableProposals.end()
              ? CurrentStrong->second.ProofRank
              : CurrentEdge->second.ProofRank;
      ActiveJumpTableCandidateProofRank = PriorRank;
      ActiveJumpTableCandidateDependencyRank = PriorRank;
      if (CurrentEdge != PriorProvisionalRelativeEdges.end()) {
        const ProvisionalRelativeEdgeProposal &Proposal = CurrentEdge->second;
        const bool CompletePhysicalCoordinateToken =
            Proposal.AuthenticatesPhysicalStorage &&
            Proposal.CompleteDenseRuntimeCoordinates &&
            !Proposal.CompleteRuntimeStorageRange &&
            Proposal.Storage.EntrySize != 0 &&
            Proposal.Storage.EntryStride >= Proposal.Storage.EntrySize &&
            Proposal.Storage.PhysicalSlotCount >=
                limits::kMinJumpTableEntries &&
            Proposal.Storage.PhysicalSlotCount <=
                limits::kMaxJumpTableEntries &&
            Proposal.Storage.storageEnd().has_value() &&
            !Proposal.Targets.empty();
        if (CompletePhysicalCoordinateToken) {
          if (PriorRank == std::numeric_limits<uint32_t>::max()) {
            CandidateEvidenceAnalysisIncomplete = true;
            return {};
          }
          // The producer's own immutable edge is a one-shot dependency at the
          // next rank.  Keep the ordinary borrow ceiling at PriorRank so equal-
          // rank siblings do not become accidental lower-rank authorities.
          ActiveJumpTableCandidateProofRank = PriorRank + 1;
          CurrentCandidateMayReplayOwnProvisionalRelativeEdge = true;
        }
      }
    } else {
      uint32_t MaxRank = 0;
      bool HasPrior = false;
      for (const auto &[Addr, Proposal] : PriorStrongJumpTableProposals) {
        (void)Addr;
        HasPrior = true;
        MaxRank = std::max(MaxRank, Proposal.ProofRank);
      }
      for (const auto &[Addr, Proposal] : PriorProvisionalRelativeEdges) {
        (void)Addr;
        HasPrior = true;
        MaxRank = std::max(MaxRank, Proposal.ProofRank);
      }
      if (HasPrior) {
        if (MaxRank == std::numeric_limits<uint32_t>::max()) {
          CandidateEvidenceAnalysisIncomplete = true;
          return {};
        }
        ActiveJumpTableCandidateProofRank = MaxRank + 1;
        ActiveJumpTableCandidateDependencyRank =
            ActiveJumpTableCandidateProofRank;
      }
    }
  }
  // Prepay every dynamic field that a JumpTableInfo copy or equality walk can
  // retain/visit.  The outer-vector charges happen before inspecting nested
  // vectors, and all nested charges happen before the caller performs the copy
  // or comparison, so an exhausted candidate account cannot allocate first and
  // fail afterward.
  auto consumeJumpTableInfoTraversal = [&](const JumpTableInfo &Candidate) {
    if (!consumeCandidateProducts(
            {{Candidate.AuthenticatedMaskCoordinates.size(), 2},
             {Candidate.AuthenticatedMaskKnownOneWitnesses.size(), 2},
             {Candidate.StorageRanges.size(), 2},
             {Candidate.SuppressibleRelocationSlots.size(), 2},
             {Candidate.IndexValueAlternatives.size(), 2},
             {Candidate.TargetLoads.size(), 2},
             {Candidate.AuthenticatedFrameStorage.Initializers.size(), 2},
             {Candidate.AuthenticatedStorageConsumers.size(), 2},
             {Candidate.LoadRoles.size(), 2},
             {Candidate.EntryIndices.size(), 2},
             {Candidate.RuntimeCaseLabels.size(), 2},
             {Candidate.RuntimeSlotIndices.size(), 2},
             {Candidate.ExplicitTargets.size(), 2}}))
      return false;
    for (const JumpTableFrameInitializerChunk &Initializer :
         Candidate.AuthenticatedFrameStorage.Initializers)
      if (!consumeCandidateProducts({{Initializer.StaticSources.size(), 2}}))
        return false;
    for (const JumpTableLoadRole &Role : Candidate.LoadRoles) {
      if (!consumeCandidateProducts(
              {{Role.AllowedBases.size(), 2},
               {Role.Indices.size(), 2},
               {Role.FrameStorage.Initializers.size(), 2}}))
        return false;
      for (const JumpTableFrameInitializerChunk &Initializer :
           Role.FrameStorage.Initializers)
        if (!consumeCandidateProducts({{Initializer.StaticSources.size(), 2}}))
          return false;
    }
    return consumeCandidateEvidence(1);
  };
  // A published JumpTableInfo is copied into ResolvedTableInfo and can later
  // be removed by an incomplete outer proposal stage.  Reserve that future
  // deep destruction before the map copy exists, so rollback never performs
  // attacker-shaped vector cleanup after the shared balance is exhausted.
  auto consumeJumpTableInfoDestruction =
      [&](const JumpTableInfo &Candidate) {
        if (!consumeCandidateProducts(
                {{Candidate.AuthenticatedMaskCoordinates.size(), 1},
                 {Candidate.AuthenticatedMaskKnownOneWitnesses.size(), 1},
                 {Candidate.StorageRanges.size(), 1},
                 {Candidate.SuppressibleRelocationSlots.size(), 1},
                 {Candidate.IndexValueAlternatives.size(), 1},
                 {Candidate.TargetLoads.size(), 1},
                 {Candidate.AuthenticatedFrameStorage.Initializers.size(), 1},
                 {Candidate.AuthenticatedStorageConsumers.size(), 1},
                 {Candidate.LoadRoles.size(), 1},
                 {Candidate.EntryIndices.size(), 1},
                 {Candidate.RuntimeCaseLabels.size(), 1},
                 {Candidate.RuntimeSlotIndices.size(), 1},
                 {Candidate.ExplicitTargets.size(), 1}}))
          return false;
        for (const JumpTableFrameInitializerChunk &Initializer :
             Candidate.AuthenticatedFrameStorage.Initializers)
          if (!consumeCandidateEvidence(Initializer.StaticSources.size()))
            return false;
        for (const JumpTableLoadRole &Role : Candidate.LoadRoles) {
          if (!consumeCandidateProducts(
                  {{Role.AllowedBases.size(), 1},
                   {Role.Indices.size(), 1},
                   {Role.FrameStorage.Initializers.size(), 1}}))
            return false;
          for (const JumpTableFrameInitializerChunk &Initializer :
               Role.FrameStorage.Initializers)
            if (!consumeCandidateEvidence(Initializer.StaticSources.size()))
              return false;
        }
        return consumeCandidateEvidence(1);
      };
  auto reservePublishedTargetCleanup = [&](size_t Count) {
    if (!CandidateProposalStageActive)
      return true;
    if (!consumeCandidateEvidence(Count)) {
      ProposalCleanupEvidenceForTesting.NewTargetsExhausted = true;
      ProposalCleanupEvidenceForTesting.MutationObservedBeforeReservation |=
          CandidateTargetMaterializationStarted ||
          ResolvedTableInfo.count(Rec.Addr) != 0;
      return false;
    }
    ProposalCleanupEvidenceForTesting.NewTargetsReserved = true;
    return true;
  };
  auto reserveResolvedInfoMaterialization = [&](const JumpTableInfo
                                                    &Candidate) {
    if (!consumeJumpTableInfoTraversal(Candidate) ||
        (CandidateProposalStageActive &&
         !consumeJumpTableInfoDestruction(Candidate))) {
      if (CandidateProposalStageActive) {
        ProposalCleanupEvidenceForTesting.NewInfoExhausted = true;
        ProposalCleanupEvidenceForTesting.MutationObservedBeforeReservation |=
            ResolvedTableInfo.count(Rec.Addr) != 0;
      }
      return false;
    }
    if (CandidateProposalStageActive)
      ProposalCleanupEvidenceForTesting.NewInfoReserved = true;
    return true;
  };
  std::set<va_t> DecodedTableAnchors;
  // The exact-boundary helpers inspect loader inventories whose size is under
  // input control.  Charge their complete owner-query call graph before each
  // use; a failed charge is candidate evidence incompleteness and occurs before
  // the helper can allocate or publish anything.
  auto consumeExactBoundaryInventory = [&](const BinaryImage &Image,
                                           size_t DecodedAnchorCount) {
    const size_t Fields = Image.DataAddressRelocOperands.size();
    if (!consumeCandidateProducts({{Image.Segments.size(), 3},
                                   {Image.Sections.size(), 3},
                                   {Image.RelCodeTableAnchors.size(), 1},
                                   {DecodedAnchorCount, 1},
                                   {Fields, 2}}) ||
        !consumeCandidateFactorProduct({Fields, Image.Segments.size(), 16}) ||
        !consumeCandidateFactorProduct({Fields, Image.Sections.size(), 8}) ||
        !consumeCandidateFactorProduct(
            {Fields, Image.Imports.size(), Image.Segments.size(), 4}) ||
        !consumeCandidateFactorProduct(
            {Fields, Image.Imports.size(), Image.Sections.size(), 4}))
      return false;
    return true;
  };
  // jumpTableProofRoots builds several ordered containers and scans the full
  // relocation-source relation.  Charge a conservative exact upper bound for
  // those visits/retained nodes before entering the helper.  The helper is pure
  // for one resolveJumpTable invocation: Rec and Insns are not mutated here.
  auto budgetedJumpTableProofRoots =
      [&](const JumpTableInfo &Candidate) -> std::optional<std::set<va_t>> {
    size_t StorageCount = Candidate.StorageRanges.size();
    size_t SuppressibleSlotCount = Candidate.SuppressibleRelocationSlots.size();
    if (StorageCount == 0 && Candidate.HasBaseAddr &&
        Candidate.EntrySize != 0 && Candidate.PhysicalCapacity != 0 &&
        Candidate.RelocAbsolute) {
      if (!CurrentImg || !consumeExactBoundaryInventory(
                             *CurrentImg, DecodedTableAnchors.size()))
        return std::nullopt;
      StorageCount = 1;
    }
    // Charge this wrapper's own inventory pass before reading any nested
    // proposal/source sizes.  The pure helper below then reserves the separate
    // work performed by jumpTableProofRoots itself.
    if (!consumeCandidateProducts({{PriorStrongJumpTableProposals.size(), 5},
                                   {RelocationCFGRootSources.size(), 2}}))
      return std::nullopt;
    for (const auto &[Addr, Proposal] : PriorStrongJumpTableProposals) {
      if (Addr == ActiveJumpTableCandidateAddr ||
          (!ActiveJumpTableConsumerAudit &&
           Proposal.ProofRank >= ActiveJumpTableCandidateDependencyRank))
        continue;
      if (Proposal.StorageRanges.size() >
              std::numeric_limits<size_t>::max() - StorageCount ||
          Proposal.SuppressibleRelocationSlots.size() >
              std::numeric_limits<size_t>::max() - SuppressibleSlotCount) {
        consumeCandidateEvidence(std::numeric_limits<size_t>::max());
        return std::nullopt;
      }
      StorageCount += Proposal.StorageRanges.size();
      SuppressibleSlotCount += Proposal.SuppressibleRelocationSlots.size();
    }
    size_t RelocationSourceCount = 0;
    for (const auto &[Target, Sources] : RelocationCFGRootSources) {
      (void)Target;
      if (Sources.size() >
          std::numeric_limits<size_t>::max() - RelocationSourceCount) {
        consumeCandidateEvidence(std::numeric_limits<size_t>::max());
        return std::nullopt;
      }
      RelocationSourceCount += Sources.size();
    }
    const std::optional<size_t> ProofRootWork =
        detail::jumpTableProofRootConstructionWork(
            PersistentCFGRoots.size(), PriorStrongJumpTableProposals.size(),
            StorageCount, Candidate.ExplicitTargets.size(),
            SuppressibleSlotCount,
            ProtectedJumpTableRelocationSlots
                ? ProtectedJumpTableRelocationSlots->size()
                : 0,
            CurrentImg->Segments.size(), CurrentImg->CodePtrRelocSlots.size(),
            RelocationCFGRootSources.size(), RelocationSourceCount,
            DurableCFGRoots.size());
    if (!ProofRootWork || !consumeCandidateEvidence(*ProofRootWork))
      return std::nullopt;
    return jumpTableProofRoots(Candidate, &DecodedTableAnchors);
  };
  const bool ModuleMutationUnsafe =
      UnsafeJumpTableBranches && UnsafeJumpTableBranches->count(Rec.Addr);
  struct PreciseGuardProofKey {
    va_t BranchAddr = InvalidVA;
    NdVar IndexValueAtUse;
    va_t IndexUseAddr = InvalidVA;
    int IndexUseSeq = -1;
    bool IndexValueDefinedAtUse = false;
    std::vector<JumpTableValueOccurrence> IndexValueAlternatives;
    va_t TableLoadAddr = InvalidVA;
    int TableLoadSeq = -1;
    bool ProofContextComplete = false;
    bool UseDefinedAlternativesAsRoots = false;
    std::set<va_t> ProofRoots;
  };
  struct PreciseGuardProofCache {
    PreciseGuardProofKey Key;
    uint32_t GuardBound = 0;
    bool HasControllingGuard = false;
    bool IncompleteGuardDomain = false;
    bool SemanticGuardDomainAmbiguous = false;
    bool Valid = false;
  } PreciseGuardCache;
  auto capturePreciseGuardKey = [&](const JumpTableInfo &State,
                                    bool UseDefinedAlternativesAsRoots)
      -> std::optional<PreciseGuardProofKey> {
    const std::set<va_t> &Roots = ActiveJumpTableProofRoots
                                      ? *ActiveJumpTableProofRoots
                                      : PersistentCFGRoots;
    if (!consumeCandidateProducts({{State.IndexValueAlternatives.size(), 2},
                                   {Roots.size(), 2},
                                   {1, 1}}))
      return std::nullopt;
    PreciseGuardProofKey Key;
    Key.BranchAddr = Rec.Addr;
    Key.IndexValueAtUse = State.IndexValueAtUse;
    Key.IndexUseAddr = State.IndexUseAddr;
    Key.IndexUseSeq = State.IndexUseSeq;
    Key.IndexValueDefinedAtUse = State.IndexValueDefinedAtUse;
    Key.IndexValueAlternatives = State.IndexValueAlternatives;
    Key.TableLoadAddr = State.TableLoadAddr;
    Key.TableLoadSeq = State.TableLoadSeq;
    Key.ProofContextComplete = JumpTableProofContextComplete;
    Key.UseDefinedAlternativesAsRoots = UseDefinedAlternativesAsRoots;
    Key.ProofRoots = Roots;
    return Key;
  };
  auto samePreciseGuardKey = [&](const PreciseGuardProofKey &A,
                                 const PreciseGuardProofKey &B) {
    const std::optional<size_t> ComparisonWork =
        detail::preciseGuardKeyComparisonWork(
            A.IndexValueAlternatives.size(), B.IndexValueAlternatives.size(),
            A.ProofRoots.size(), B.ProofRoots.size());
    if (!consumeCandidateEvidence(
            ComparisonWork.value_or(std::numeric_limits<size_t>::max())))
      return false;
    bool Same = true;
    Same &= A.BranchAddr == B.BranchAddr;
    Same &= A.IndexValueAtUse.Space == B.IndexValueAtUse.Space;
    Same &= A.IndexValueAtUse.Offset == B.IndexValueAtUse.Offset;
    Same &= A.IndexValueAtUse.Size == B.IndexValueAtUse.Size;
    Same &= A.IndexValueAtUse.Provenance == B.IndexValueAtUse.Provenance;
    Same &=
        A.IndexValueAtUse.AddressOwnerVA == B.IndexValueAtUse.AddressOwnerVA;
    Same &= A.IndexUseAddr == B.IndexUseAddr;
    Same &= A.IndexUseSeq == B.IndexUseSeq;
    Same &= A.IndexValueDefinedAtUse == B.IndexValueDefinedAtUse;
    Same &= A.IndexValueAlternatives.size() == B.IndexValueAlternatives.size();
    for (size_t I = 0; I < std::min(A.IndexValueAlternatives.size(),
                                    B.IndexValueAlternatives.size());
         ++I) {
      const JumpTableValueOccurrence &Left = A.IndexValueAlternatives[I];
      const JumpTableValueOccurrence &Right = B.IndexValueAlternatives[I];
      Same &= Left.Value.Space == Right.Value.Space;
      Same &= Left.Value.Offset == Right.Value.Offset;
      Same &= Left.Value.Size == Right.Value.Size;
      Same &= Left.Value.Provenance == Right.Value.Provenance;
      Same &= Left.Value.AddressOwnerVA == Right.Value.AddressOwnerVA;
      Same &= Left.Addr == Right.Addr;
      Same &= Left.Seq == Right.Seq;
      Same &= Left.DefinedAtPoint == Right.DefinedAtPoint;
    }
    Same &= A.TableLoadAddr == B.TableLoadAddr;
    Same &= A.TableLoadSeq == B.TableLoadSeq;
    Same &= A.ProofContextComplete == B.ProofContextComplete;
    Same &= A.UseDefinedAlternativesAsRoots == B.UseDefinedAlternativesAsRoots;
    Same &= A.ProofRoots.size() == B.ProofRoots.size();
    auto LeftRoot = A.ProofRoots.begin();
    auto RightRoot = B.ProofRoots.begin();
    while (LeftRoot != A.ProofRoots.end() && RightRoot != B.ProofRoots.end()) {
      Same &= *LeftRoot == *RightRoot;
      ++LeftRoot;
      ++RightRoot;
    }
    return Same;
  };
  auto provePreciseGuard =
      [&](JumpTableInfo &State, bool UseDefinedAlternativesAsRoots = false,
          const std::map<va_t, std::vector<va_t>> *CertifiedEdgeOverrides =
              nullptr) {
        std::optional<PreciseGuardProofKey> Key;
        if (!CertifiedEdgeOverrides) {
          Key = capturePreciseGuardKey(State, UseDefinedAlternativesAsRoots);
          if (!Key) {
            State.IncompleteGuardDomain = true;
            return false;
          }
        }
        if (!CertifiedEdgeOverrides && PreciseGuardCache.Valid &&
            JumpTableProofContextComplete &&
            samePreciseGuardKey(*Key, PreciseGuardCache.Key)) {
          State.MaxEntries =
              State.MaxEntries == 0
                  ? PreciseGuardCache.GuardBound
                  : std::min(State.MaxEntries, PreciseGuardCache.GuardBound);
          State.HasControllingGuard = PreciseGuardCache.HasControllingGuard;
          State.IncompleteGuardDomain = PreciseGuardCache.IncompleteGuardDomain;
          State.SemanticGuardDomainAmbiguous =
              PreciseGuardCache.SemanticGuardDomainAmbiguous;
          RequestedCompleteJumpTableProof = true;
          return true;
        }
        const uint32_t InputMaxEntries = State.MaxEntries;
        const bool Result = inferBoundsFromPreciseGuards(
            Rec, State, &CandidateEvidenceBudget, UseDefinedAlternativesAsRoots,
            CertifiedEdgeOverrides);
        // Preserve an incomplete guard attempt on State, but do not poison the
        // candidate transaction yet.  A later exact mask or modulo occurrence
        // is an independent complete selector-domain certificate.  The common
        // gate below promotes IncompleteGuardDomain to candidate-wide
        // incompleteness only when no such certificate was recovered. Cache
        // only an unclamped successful proof, so GuardBound is the solver's
        // exact FirstRejected value.  A hit reapplies the current candidate's
        // MaxEntries intersection rather than replaying stale output state.
        if (!CertifiedEdgeOverrides && Result && InputMaxEntries == 0 &&
            State.MaxEntries != 0) {
          PreciseGuardCache.Key = std::move(*Key);
          PreciseGuardCache.GuardBound = State.MaxEntries;
          PreciseGuardCache.HasControllingGuard = State.HasControllingGuard;
          PreciseGuardCache.IncompleteGuardDomain = State.IncompleteGuardDomain;
          PreciseGuardCache.SemanticGuardDomainAmbiguous =
              State.SemanticGuardDomainAmbiguous;
          PreciseGuardCache.Valid = true;
        }
        return Result;
      };
  auto HasOccurrenceMetadata = [](const JumpTableInfo &Candidate) {
    if (!Candidate.LoadRoles.empty())
      return true;
    const bool HasIndex = !Candidate.IndexValueAlternatives.empty() ||
                          (((Candidate.IndexValueAtUse.isReg() ||
                             Candidate.IndexValueAtUse.isTemp() ||
                             Candidate.IndexValueAtUse.isConst()) &&
                            Candidate.IndexValueAtUse.Size != 0 &&
                            (Candidate.IndexValueAtUse.isConst() ||
                             (Candidate.IndexUseAddr != InvalidVA &&
                              Candidate.IndexUseSeq >= 0))));
    return HasIndex && Candidate.TableLoadAddr != InvalidVA &&
           Candidate.TableLoadSeq >= 0 && !Candidate.TargetLoads.empty();
  };
  auto RejectIncompleteCandidate = [&] {
    Info = JumpTableInfo{};
    return false;
  };
  // Strategies are tried most-specific first, falling back to the generic
  // pattern matchers.  Strategies 1 and 1b are the architecture-gated
  // ARM-family detectors (defined in JumpTableResolverARM.cpp); every strategy
  // from 1c onward is architecture-neutral pattern matching that covers x86,
  // x64, ARM32, and AArch64 alike — which is why there is no x86-specific
  // detector to dispatch to here.

  // Strategy 1: ARM32 TBB/TBH table-branch (most specific, check first).
  bool Recovered = tryARMTableBranch(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 1b: AArch64 compact byte/halfword table (separate entry base and
  // code anchor, scaled entries) — must precede the generic relative resolver,
  // which would otherwise latch onto the entry base as the (wrong) target base.
  bool RecoveredAArch64Compact = false;
  if (!Recovered) {
    Recovered = tryAArch64CompactTable(Img, Rec, Info);
    RecoveredAArch64Compact = Recovered;
  }
  // Compact byte/halfword tables have no per-entry relocations, so the generic
  // relocation-run inventory below cannot establish their readable storage
  // ceiling.  Retain the remaining bytes of the exact mapped owner as a
  // capacity only; selector authority still comes exclusively from the guard,
  // mask, or modulo fixed point.  This lets a loop-carried compact selector
  // decode already-authorized targets and monotonically discover the rest
  // without treating adjacent data as a feasible runtime coordinate.
  if (RecoveredAArch64Compact && Info.HasBaseAddr && Info.EntrySize != 0 &&
      Info.PhysicalCapacity == 0) {
    if (!consumeCandidateProducts(
            {{Img.Segments.size(), 8}, {Img.Sections.size(), 16}, {1, 20}}))
      return {};
    // Inline executable bytes are not an authenticated data-table owner.  In
    // particular, a mismatched X-register selector must not turn the following
    // case code into a capacity simply because byte decoding stays in .text.
    if (!Img.isCodeAddress(Info.BaseAddr)) {
      const std::optional<va_t> OwnerEnd =
          Img.mappedObjectOwnerEnd(Info.BaseAddr);
      const uint64_t PhysicalStride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      if (OwnerEnd && Info.EntrySize <= InvalidVA - Info.BaseAddr &&
          *OwnerEnd >= Info.BaseAddr + Info.EntrySize &&
          PhysicalStride >= Info.EntrySize) {
        const uint64_t Span = *OwnerEnd - Info.BaseAddr;
        const uint64_t Slots = 1 + (Span - Info.EntrySize) / PhysicalStride;
        if (Slots <= limits::kMaxJumpTableEntries)
          Info.PhysicalCapacity = static_cast<uint32_t>(Slots);
      }
    }
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 1c: runtime-selected table base (`base = cond ? A : B; jmp
  // *base[idx]`) — two adjacent code-pointer tables merged into one.  Must
  // precede the generic relative/cross-instruction resolvers, which would fold
  // only one arm of the select and recover half the table.
  if (!Recovered) {
    Info = JumpTableInfo{};
    bool TwoTableAnalysisComplete = false;
    Recovered = tryTwoTableSelect(Img, Rec, Info, &CandidateEvidenceBudget,
                                  &TwoTableAnalysisComplete);
    CandidateEvidenceShapeClaimed |=
        Info.CompositeShapeClaimed || !TwoTableAnalysisComplete;
    CandidateEvidenceAnalysisIncomplete |= !TwoTableAnalysisComplete;
    if (!TwoTableAnalysisComplete)
      return {};
    if (Info.CompositeShapeClaimed &&
        (!Recovered || !HasOccurrenceMetadata(Info)))
      return {};
    if (!Recovered)
      Info = JumpTableInfo{};
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    return {};

  // Strategy 1d: two-level index-byte table (`jmptab[idxtab[switchvar]]`, the
  // classic MSVC sparse-switch lowering).  Must precede the generic
  // relative/cross-instruction resolvers, which would otherwise recover only
  // the inner address table (jmptab) and dispatch on the intermediate table
  // index instead of the real switch variable — collapsing the case set and
  // losing the true labels.  It composes the per-case targets into
  // ExplicitTargets, so it short-circuits the single-base machinery below.
  if (!Recovered) {
    Info = JumpTableInfo{};
    Recovered = tryTwoLevelIndexTable(Img, Rec, Info, &CandidateEvidenceBudget);
    CandidateEvidenceShapeClaimed |= Info.CompositeShapeClaimed;
    CandidateEvidenceAnalysisIncomplete |= Info.IncompleteGuardDomain;
    if (Info.CompositeShapeClaimed &&
        (!Recovered || !HasOccurrenceMetadata(Info)))
      return {};
    if (!Recovered)
      Info = JumpTableInfo{};
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    return {};
  if (!Recovered && RequestedCompleteJumpTableProof &&
      !JumpTableProofContextComplete) {
    return {};
  }

  // Strategy 2: PIC-relative table (architecture-neutral; common on x64).
  if (!Recovered)
    Recovered = tryRelativeTable(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 2b: PIC-relative table whose base register is materialised in
  // a preceding instruction (x86 `lea table(%rip),%reg` / ARM32 ADR).  The
  // per-record slice above cannot see the base, so fold it across
  // instructions by emulating the dominating prefix.
  if (!Recovered)
    Recovered = tryCrossInstrRelativeTable(Img, Rec, Info);
  if (StackTableEvidenceIncompleteBranches.count(Rec.Addr)) {
    CandidateEvidenceAnalysisIncomplete = true;
    return {};
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 2c: constant-base absolute table whose load is decoupled from the
  // branch by an -O0 spill/reload relay (`... mov tab(,idx,W),%r; mov
  // %r,[slot];
  // ... mov [slot],%r; jmp *%r`), including a shared multi-site computed-goto
  // dispatch where several goto-site predecessors feed one common table.  The
  // cross-instruction resolver above only reaches a load in the branch's own
  // block or a single-predecessor path, so a many-predecessor shared dispatch
  // reaches none; this recovers the table from the code-pointer relocation run
  // at its constant base regardless of how many goto sites share it.
  if (!Recovered) {
    bool ConstBaseAnalysisComplete = false;
    bool ConstBaseShapeClaimed = false;
    Recovered = tryConstBaseAbsoluteTable(
        Img, Rec, Info, &ExactConsumerGroup, &ConstBaseShapeClaimed,
        &CandidateEvidenceBudget, &ConstBaseAnalysisComplete);
    CandidateEvidenceShapeClaimed |= ConstBaseShapeClaimed;
    ConstBaseLocalShapeClaimedForTesting |= ConstBaseShapeClaimed;
    if (ConstBaseShapeClaimed && MaskFixedPointEvidenceBudgetForTesting) {
      if (ConstBaseFirstLocalShapeClaimedAddrForTesting == InvalidVA) {
        ConstBaseFirstLocalShapeClaimedAddrForTesting = Rec.Addr;
      } else if (ConstBaseFirstLocalShapeClaimedAddrForTesting != Rec.Addr &&
                 ConstBaseSecondLocalShapeClaimedAddrForTesting == InvalidVA) {
        ConstBaseSecondLocalShapeClaimedAddrForTesting = Rec.Addr;
      } else if (ConstBaseFirstLocalShapeClaimedAddrForTesting != Rec.Addr &&
                 ConstBaseSecondLocalShapeClaimedAddrForTesting != Rec.Addr) {
        ConstBaseLocalShapeClaimedAddrOverflowForTesting = true;
      }
    }
    ConstBasePostShapeAnalysisIncompleteForTesting |=
        ConstBaseShapeClaimed && !ConstBaseAnalysisComplete;
    CandidateEvidenceAnalysisIncomplete |= !ConstBaseAnalysisComplete;
    if (!ConstBaseAnalysisComplete)
      return {};
  }
  // A direct memory dispatch can be recognized by an earlier single-consumer
  // strategy before the constant-base detector gets a chance to inventory its
  // tail-duplicated siblings.  Enrich that already recovered absolute-table
  // candidate once, under the same candidate account, and accept the group
  // only when the independent detector names the exact same physical object.
  // This is metadata enrichment, not a second source of table authority: the
  // original strategy remains responsible for the candidate shape itself.
  if (Recovered && Info.RelocAbsolute &&
      ExactConsumerGroup.IndexOccurrences.empty()) {
    JumpTableInfo GroupInfo;
    bool GroupShapeClaimed = false;
    bool GroupAnalysisComplete = false;
    const bool GroupRecovered = tryConstBaseAbsoluteTable(
        Img, Rec, GroupInfo, &ExactConsumerGroup, &GroupShapeClaimed,
        &CandidateEvidenceBudget, &GroupAnalysisComplete,
        /*ScanExactGroup=*/true, /*RequireCurrentBranchLoad=*/false,
        /*RequireExactGroupAnchor=*/true);
    CandidateEvidenceShapeClaimed |= GroupShapeClaimed;
    if (!GroupAnalysisComplete) {
      ExactConsumerGroup.IndexOccurrences.clear();
      ExactConsumerGroup.BranchAddrs.clear();
      // This pass enriches an independently recovered candidate; it is not a
      // prerequisite for that candidate's original role/domain proof.  Only a
      // partially recognized group shape is evidence that must fail closed.
      // A complete scan that finds no group, or an incomplete scan before any
      // group shape exists, simply leaves the optional metadata absent.
      if (GroupShapeClaimed) {
        CandidateEvidenceAnalysisIncomplete = true;
        return {};
      }
    }
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    const uint64_t GroupPhysicalStride = GroupInfo.EntryStride != 0
                                             ? GroupInfo.EntryStride
                                             : GroupInfo.EntrySize;
    const bool SamePhysicalTable =
        GroupAnalysisComplete && GroupRecovered && Info.HasBaseAddr &&
        GroupInfo.HasBaseAddr && Info.BaseAddr == GroupInfo.BaseAddr &&
        Info.EntrySize == GroupInfo.EntrySize &&
        PhysicalStride == GroupPhysicalStride &&
        Info.PhysicalCapacity == GroupInfo.PhysicalCapacity &&
        Info.RelocAbsolute && !Info.IsRelative && GroupInfo.RelocAbsolute &&
        !GroupInfo.IsRelative;
    if (!SamePhysicalTable) {
      ExactConsumerGroup.IndexOccurrences.clear();
      ExactConsumerGroup.BranchAddrs.clear();
    }
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 3: execute the dispatch once with each register input left as the
  // one whole-word unknown.  A successful decomposition is exact: the loaded
  // address itself states the table base, entry width and index stride.  Keep
  // this behind the specialised forms above, whose multi-table and
  // architecture-specific layouts intentionally carry more metadata than one
  // linear expression can describe.
  if (!Recovered) {
    std::set<std::pair<uint64_t, uint16_t>> Candidates;
    for (const LowOp &Op : Rec.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isReg() && Op.Inputs[I].Size != 0)
          Candidates.emplace(Op.Inputs[I].Offset, Op.Inputs[I].Size);

    std::optional<symbolic::DispatchShape> Shape;
    uint64_t ShapeIndex = InvalidVA;
    bool Ambiguous = false;
    for (const auto &[Reg, Bytes] : Candidates) {
      symbolic::SymContext SymCtx;
      std::optional<symbolic::DispatchShape> Candidate =
          symbolic::analyzeDispatch(SymCtx, Rec.Ops, Reg, Bytes);
      if (!Candidate || Candidate->EntryStride == 0 ||
          Candidate->EntryScale > std::numeric_limits<uint32_t>::max())
        continue;
      if (Shape) {
        Ambiguous = true;
        break;
      }
      Shape = *Candidate;
      ShapeIndex = Reg;
    }

    if (Shape && !Ambiguous) {
      Info.setBaseAddr(Shape->TableBase);
      Info.EntrySize = Shape->EntrySize;
      const bool PreScaled = Shape->EntryStride == 1 && Shape->EntrySize > 1;
      Info.EntryStride = PreScaled ? Shape->EntrySize : Shape->EntryStride;
      Info.IndexReg = ShapeIndex;
      Info.IsRelative = Shape->Kind == symbolic::DispatchKind::Relative;
      Info.IsSigned = Shape->EntryIsSigned;
      if (Info.IsRelative)
        Info.setTargetBase(Shape->RelativeBase);
      Info.EntryScale = static_cast<uint32_t>(Shape->EntryScale);
      Info.PreScaledIndex = PreScaled;
      if (PreScaled)
        Info.Stride = Shape->EntrySize;

      // Bind the symbolic shape back to concrete LowIR occurrences.  A
      // numeric linear decomposition is only a candidate; publication still
      // requires an exact LOAD output-to-branch certificate and an exact
      // base/index address role.  Constant-base direct tables are handled here
      // because analyzeTableLoadAddr deliberately requires a register base.
      for (int I = 0; I < static_cast<int>(Rec.Ops.size()); ++I) {
        const LowOp &Load = Rec.Ops[I];
        if (Load.Opcode != NdOp::LOAD || Load.NumInputs < 1 ||
            Load.Output.Size != Info.EntrySize)
          continue;
        const NdVar &LoadAddress = Load.Inputs[Load.NumInputs >= 2 ? 1 : 0];
        int AddIdx = reachingDefIdx(Rec.Ops, I - 1, LoadAddress);
        for (int Guard = 0;
             AddIdx >= 0 && Rec.Ops[AddIdx].Opcode == NdOp::COPY &&
             Rec.Ops[AddIdx].NumInputs >= 1 &&
             Guard < limits::kMaxQuasiCopyDepth;
             ++Guard)
          AddIdx =
              reachingDefIdx(Rec.Ops, AddIdx - 1, Rec.Ops[AddIdx].Inputs[0]);
        if (AddIdx < 0 || Rec.Ops[AddIdx].Opcode != NdOp::INT_ADD ||
            Rec.Ops[AddIdx].NumInputs < 2)
          continue;
        const LowOp &Add = Rec.Ops[AddIdx];
        for (int BaseSide = 0; BaseSide < 2; ++BaseSide) {
          const NdVar &Base = Add.Inputs[BaseSide];
          if (!Base.isConst() || Base.Offset != Info.BaseAddr)
            continue;
          const NdVar &Dynamic = Add.Inputs[1 - BaseSide];
          NdVar ExactIndex;
          va_t IndexUseAddr = InvalidVA;
          int IndexUseSeq = -1;
          uint64_t IndexReg = InvalidVA;
          uint64_t AddressScale = 1;
          if (PreScaled) {
            ExactIndex = Dynamic;
            IndexUseAddr = Add.Addr;
            IndexUseSeq = Add.Seq;
            IndexReg = traceToRegister(Rec.Ops, AddIdx - 1, Dynamic);
          } else {
            IndexReg = scaledIndexReg(Rec.Ops, AddIdx - 1, Dynamic, &ExactIndex,
                                      &IndexUseAddr, &IndexUseSeq);
            AddressScale = Shape->EntryStride;
          }
          if (IndexReg == InvalidVA || ExactIndex.Size == 0 ||
              IndexUseAddr == InvalidVA || IndexUseSeq < 0)
            continue;

          JumpTableValueOccurrence LoadOccurrence{
              Load.Output, Load.Addr, Load.Seq, /*DefinedAtPoint=*/true};
          JumpTableValueOccurrence IndexOccurrence{ExactIndex, IndexUseAddr,
                                                   IndexUseSeq,
                                                   /*DefinedAtPoint=*/false};
          Info.TargetLoads.push_back(LoadOccurrence);
          JumpTableLoadRole Role;
          Role.Load = LoadOccurrence;
          Role.LoadWidth = Info.EntrySize;
          Role.AllowedBases = {Info.BaseAddr};
          Role.Indices = {IndexOccurrence};
          Role.AddressScale = AddressScale;
          Info.LoadRoles.push_back(std::move(Role));
          if (Info.IndexValueAlternatives.empty()) {
            Info.IndexValueAtUse = ExactIndex;
            Info.IndexUseAddr = IndexUseAddr;
            Info.IndexUseSeq = IndexUseSeq;
            Info.TableLoadAddr = Load.Addr;
            Info.TableLoadSeq = Load.Seq;
            Info.IndexReg = IndexReg;
          }
          Info.IndexValueAlternatives.push_back(IndexOccurrence);
          break;
        }
      }
      Recovered = !Info.TargetLoads.empty() && !Info.LoadRoles.empty();
      LLVM_DEBUG(llvm::dbgs() << "  symbolic-dispatch: table=0x"
                              << llvm::utohexstr(Info.BaseAddr)
                              << " entry=" << Info.EntrySize << " index=0x"
                              << llvm::utohexstr(Info.IndexReg) << "\n");
    }
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 4: Backward slicing for absolute tables.
  if (!Recovered) {
    Recovered = sliceBackForTableBase(Rec, Info);
    if (Recovered && !HasOccurrenceMetadata(Info))
      Recovered = RejectIncompleteCandidate();
  }
  CandidateEvidenceShapeClaimed |= I386GOTOFFProposalShapeClaimed;
  if (I386GOTOFFAmbiguousModelReach) {
    // A complete positive MayDepend proof is a semantic local rejection, not
    // resource incompleteness.  Persist only this branch as opaque and let the
    // stage commit independent callback/table candidates normally.
    CandidateEvidenceShapeClaimed = true;
    if (CurrentI386GOTOFFAmbiguityKeys.empty())
      return {};
    if (!consumeCandidateProducts(
            {{1, orderedLookupWork(StageAmbiguousI386GOTPCBranches.size())},
             {1, orderedLookupWork(PendingAmbiguousI386GOTPCBranches.size())},
             // Prepay both set nodes and their eventual rollback/final-clear
             // destruction before either generation-local mutation.
             {1, 4}}))
      return {};
    size_t FuturePendingKeys = PendingAmbiguousI386GOTPCKeys.size();
    for (const I386GOTOFFAmbiguityReplayKey &Key :
         CurrentI386GOTOFFAmbiguityKeys) {
      constexpr size_t KeyWork = 5;
      const size_t PendingLookup = orderedLookupWork(FuturePendingKeys++);
      if (!consumeCandidateProducts({{KeyWork, PendingLookup + 1}, {1, 1}}))
        return {};
    }
    StageAmbiguousI386GOTPCBranches.insert(Rec.Addr);
    PendingAmbiguousI386GOTPCBranches.insert(Rec.Addr);
    for (const I386GOTOFFAmbiguityReplayKey &Key :
         CurrentI386GOTOFFAmbiguityKeys) {
      PendingAmbiguousI386GOTPCKeys.insert(Key);
    }
    return {};
  }
  if (!Recovered) {
    if (I386GOTOFFProposalEvidenceIncomplete) {
      // An exact GOTOFF field/shape reached its model-origin gate, but either
      // whole-graph model completion or the candidate-local reaching query ran
      // out of evidence.  This is already a table-shaped indirect branch; mark
      // the shape before returning so CandidateEvidenceOutcome preserves it
      // opaquely and rolls back the proposal stage instead of treating the
      // missing model as a definitive tail-call proof.
      CandidateEvidenceShapeClaimed = true;
      CandidateEvidenceAnalysisIncomplete = true;
    }
    return {};
  }
  CandidateEvidenceShapeClaimed = true;

  const size_t AnchorOccurrences =
      RelocatedInstructionAddressOccurrences.size();
  if (!consumeCandidateProducts(
          {{AnchorOccurrences,
            orderedLookupWork(PublishedReachableInsns.size())},
           {AnchorOccurrences, orderedLookupWork(Img.RelCodeRelocSlots.size())},
           {AnchorOccurrences, orderedLookupWork(Img.CodePtrRelocSlots.size())},
           {AnchorOccurrences, orderedLookupWork(AnchorOccurrences)},
           {AnchorOccurrences, 2}}))
    return {};
  DecodedTableAnchors = currentRelocatedInstructionTableAnchors(Img);

  // Relative relocation identity is needed before the ordinary target-role
  // proof only for a proposal whose table LOAD lives behind its own provisional
  // edges.  Keep the scan lazy so ordinary candidates retain their existing
  // evidence trajectory, and cache both the raw and anchor-bounded runs so the
  // later capacity/classification phase reuses the exact same loader evidence.
  const uint64_t PhysicalEntryStride =
      Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
  const bool InspectRelativeRun = !Info.RelocAbsolute && Info.IsRelative &&
                                  Info.HasBaseAddr && Info.EntrySize > 0;
  bool RelativeRunInspected = false;
  bool PhysicalRawRelCodeRunComplete = true;
  uint32_t PhysicalRawRelCodeRun = 0;
  uint32_t BoundedRelCodeRun = 0;
  auto InspectBoundedRelativeRelocationRun = [&]() -> std::optional<uint32_t> {
    if (RelativeRunInspected)
      return BoundedRelCodeRun;
    RelativeRunInspected = true;
    if (!InspectRelativeRun)
      return uint32_t{0};
    if (DecodedTableAnchors.size() >
        std::numeric_limits<size_t>::max() - Img.RelCodeTableAnchors.size()) {
      consumeCandidateEvidence(std::numeric_limits<size_t>::max());
      return std::nullopt;
    }
    const size_t MergedAnchorCount =
        Img.RelCodeTableAnchors.size() + DecodedTableAnchors.size();
    if (!consumeCandidateFactorProduct(
            {size_t(limits::kMaxJumpTableEntries) + 1,
             orderedLookupWork(Img.RelCodeRelocSlots.size())}) ||
        !consumeCandidateProducts(
            {{Img.RelCodeTableAnchors.size(), 3},
             {DecodedTableAnchors.size(),
              orderedLookupWork(MergedAnchorCount) + 2},
             {MergedAnchorCount,
              orderedLookupWork(Img.RelCodeRelocSlots.size())}}))
      return std::nullopt;
    PhysicalRawRelCodeRun =
        countRelCodeRelocRun(Img, Info.BaseAddr, PhysicalEntryStride,
                             &PhysicalRawRelCodeRunComplete);
    BoundedRelCodeRun =
        boundRelRunByNextAnchor(Img, Info.BaseAddr, PhysicalEntryStride,
                                PhysicalRawRelCodeRun, DecodedTableAnchors);
    return BoundedRelCodeRun;
  };

  // Bootstrap occurrence proofs with an explicit, candidate-local relocation
  // allowlist.  This list is provisional: exact runtime-domain recovery and a
  // final reachable-consumer audit below may only remove permissions before
  // publication.  Keeping the permission separate from StorageRanges prevents
  // physical object ownership from silently suppressing a live filler/gap
  // relocation in the CFG or LLVM pointer mirror.
  if (Info.StorageRanges.empty() && Info.RelocAbsolute && Info.HasBaseAddr &&
      Info.EntrySize == Img.getPointerSize() && Info.PhysicalCapacity != 0) {
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize)
      return {};
    if (!consumeCandidateProducts(
            {{static_cast<size_t>(Info.PhysicalCapacity), 2}}))
      return {};
    for (uint32_t I = 0; I < Info.PhysicalCapacity; ++I) {
      if (I != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / I)
        return {};
      Info.StorageRanges.push_back(
          JumpTableStorageRange{Info.BaseAddr + uint64_t(I) * PhysicalStride,
                                Info.EntrySize, Info.EntrySize, 1});
    }
  }
  if (Info.SuppressibleRelocationSlots.empty()) {
    const std::optional<size_t> StorageInventoryWork =
        detail::physicalCodePointerSlotScanWork(Info.StorageRanges.size(),
                                                /*PhysicalSlotCount=*/0,
                                                /*RelocationLookupWork=*/0);
    if (!StorageInventoryWork ||
        !consumeCandidateEvidence(*StorageInventoryWork))
      return {};
    size_t StorageSlotCount = 0;
    for (const JumpTableStorageRange &Range : Info.StorageRanges) {
      if (Range.PhysicalSlotCount >
          std::numeric_limits<size_t>::max() - StorageSlotCount)
        return {};
      StorageSlotCount += Range.PhysicalSlotCount;
    }
    const std::optional<size_t> SlotScanWork =
        detail::physicalCodePointerSlotScanWork(
            /*StorageRangeCount=*/0, StorageSlotCount,
            orderedLookupWork(Img.CodePtrRelocSlots.size()));
    if (!SlotScanWork || !consumeCandidateEvidence(*SlotScanWork))
      return {};
    for (const JumpTableStorageRange &Range : Info.StorageRanges) {
      if (Range.EntrySize < Img.getPointerSize() ||
          Range.EntryStride < Range.EntrySize || Range.PhysicalSlotCount == 0)
        continue;
      for (uint32_t I = 0; I < Range.PhysicalSlotCount; ++I) {
        if (I != 0 && Range.EntryStride > (InvalidVA - Range.BaseAddr) / I)
          return {};
        const va_t Slot = Range.BaseAddr + uint64_t(I) * Range.EntryStride;
        if (Img.CodePtrRelocSlots.count(Slot))
          Info.SuppressibleRelocationSlots.push_back(Slot);
      }
    }
    size_t SortPasses = 0;
    for (size_t N = Info.SuppressibleRelocationSlots.size(); N > 1;
         N = (N + 1) / 2)
      ++SortPasses;
    if (!consumeCandidateProducts(
            {{Info.SuppressibleRelocationSlots.size(), SortPasses + 2}}))
      return {};
    std::sort(Info.SuppressibleRelocationSlots.begin(),
              Info.SuppressibleRelocationSlots.end());
    Info.SuppressibleRelocationSlots.erase(
        std::unique(Info.SuppressibleRelocationSlots.begin(),
                    Info.SuppressibleRelocationSlots.end()),
        Info.SuppressibleRelocationSlots.end());
  }
  if (ProtectedJumpTableRelocationSlots) {
    const std::optional<size_t> ProtectedFilterWork =
        detail::protectedRelocationFilterWork(
            Info.SuppressibleRelocationSlots.size(),
            orderedLookupWork(ProtectedJumpTableRelocationSlots->size()));
    if (!ProtectedFilterWork || !consumeCandidateEvidence(*ProtectedFilterWork))
      return {};
    Info.SuppressibleRelocationSlots.erase(
        std::remove_if(Info.SuppressibleRelocationSlots.begin(),
                       Info.SuppressibleRelocationSlots.end(),
                       [&](va_t Slot) {
                         return ProtectedJumpTableRelocationSlots->count(Slot);
                       }),
        Info.SuppressibleRelocationSlots.end());
  }

  // readTableEntries and the ownership classifiers below traverse several
  // loader inventories per target (mapped owners, executable ranges, imports,
  // typed entries, unwind fragments, and decoded-instruction boundaries).
  // Prepay the complete attacker-shaped envelope before any of those scans or
  // target vectors are materialized.  ValidationPasses is four for the strong
  // local-table path: the initial read, sanity validation, final target
  // validation, and authoritative-owner classification each consume a full
  // conservative envelope.  The opaque identity classifier needs one.
  auto PrepayPhysicalTargetInventory = [&](size_t ExpectedTargets,
                                           size_t ValidationPasses) {
    const size_t KnownEntryLookup =
        KnownFuncEntries ? orderedLookupWork(KnownFuncEntries->size()) : 0;
    const size_t RuntimeEntryLookup =
        orderedLookupWork(Img.RuntimeFunctionAddrs.size());
    const size_t VerifiedEntryLookup =
        orderedLookupWork(Img.VerifiedFunctionEntries.size());
    const size_t ImportStubLookup =
        orderedLookupWork(Img.ImportStubIndices.size());
    const size_t InsnLookup = orderedLookupWork(Insns.size());
    const size_t FragmentLookup =
        orderedLookupWork(Img.ExceptionMetadata.Functions.size());
    const size_t FragmentWorkPerEntry =
        FragmentLookup <= (std::numeric_limits<size_t>::max() - 3) / 2
            ? FragmentLookup * 2 + 3
            : std::numeric_limits<size_t>::max();
    return consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, 16}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, Img.Symbols.size(), 4}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, Img.Segments.size(), 16}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, Img.Sections.size(), 8}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses,
                Img.ExceptionMetadata.Functions.size(),
                FragmentWorkPerEntry}) &&
           consumeCandidateFactorProduct({ExpectedTargets, ValidationPasses,
                                          Img.ImportStubRanges.size(), 3}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, Img.Imports.size(), 2}) &&
           consumeCandidateFactorProduct({ExpectedTargets, ValidationPasses,
                                          Img.KnownCodeRanges.size(), 2}) &&
           consumeCandidateFactorProduct({ExpectedTargets, ValidationPasses,
                                          Img.Imports.size(),
                                          Img.Segments.size(), 4}) &&
           consumeCandidateFactorProduct({ExpectedTargets, ValidationPasses,
                                          Img.Imports.size(),
                                          Img.Sections.size(), 4}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, RuntimeEntryLookup, 2}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, VerifiedEntryLookup, 2}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, ImportStubLookup, 2}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, InsnLookup}) &&
           consumeCandidateFactorProduct(
               {ExpectedTargets, ValidationPasses, KnownEntryLookup, 2});
  };

  // An incomplete module-arbitration fallback may preserve an unresolved
  // branch only after the detector has established that it is genuinely a
  // local jump-table candidate.  A pattern match alone is insufficient: an
  // indexed callback array has the same LOAD+INDIR_BR shape but its entries
  // are other function entries and must remain an indirect tail call.  Require
  // authenticated physical storage and a complete, untruncated minimum target
  // set whose entries all belong to this function before recording the
  // independent do-not-tailcall identity.
  auto HasValidatedLocalPhysicalTargetOwnership =
      [&](const JumpTableInfo &Candidate,
          bool RequireWholePhysicalLocalSet) -> bool {
    const std::optional<std::pair<va_t, va_t>> *OwnershipRange =
        &AuthoritativeCurrentFuncRange;
    // Linked Mach-O function symbols do not carry sizes.  Once the detector
    // has removed strict interior label rebases from the function inventory,
    // two exact function-symbol endpoints bound this body.  Use that weaker
    // range only for an immutable, fully authenticated frame-table
    // initializer; ordinary relocation tables and runtime-mutated frame tables
    // still require an authoritative sized/unwind range.
    if (!*OwnershipRange && CurrentFuncRange && !Candidate.MutatedUnsafe &&
        !Candidate.AuthenticatedFrameStorage.Initializers.empty()) {
      if (!consumeCandidateFactorProduct({2, Img.Symbols.size(), 2}))
        return false;
      if (Img.hasFunctionSymbolAt(CurrentFuncEntry) &&
          Img.hasFunctionSymbolAt(CurrentFuncRange->second))
        OwnershipRange = &CurrentFuncRange;
    }
    if (!*OwnershipRange)
      return false;
    if (!consumeCandidateProducts({{Candidate.StorageRanges.size(), 4}}))
      return false;
    size_t PhysicalSlots = 0;
    for (const JumpTableStorageRange &Range : Candidate.StorageRanges) {
      if (Range.EntrySize == 0 || Range.EntryStride < Range.EntrySize ||
          Range.PhysicalSlotCount == 0 ||
          Range.PhysicalSlotCount >
              limits::kMaxJumpTableEntries - PhysicalSlots)
        return false;
      PhysicalSlots += Range.PhysicalSlotCount;
    }

    size_t ExpectedTargets = 0;
    if (!Candidate.ExplicitTargets.empty())
      ExpectedTargets = Candidate.ExplicitTargets.size();
    else if (!Candidate.RuntimeSlotIndices.empty()) {
      if (Candidate.RuntimeSlotIndices.size() !=
          Candidate.RuntimeCaseLabels.size())
        return false;
      ExpectedTargets = Candidate.RuntimeSlotIndices.size();
    } else
      ExpectedTargets = Candidate.MaxEntries;
    if (ExpectedTargets < limits::kMinJumpTableEntries ||
        ExpectedTargets > limits::kMaxJumpTableEntries ||
        PhysicalSlots < ExpectedTargets ||
        (RequireWholePhysicalLocalSet && PhysicalSlots != ExpectedTargets))
      return false;

    // Prepay the candidate target read/copy, checked copy, sanity walk, final
    // target validation, and authoritative-owner walk before any of those
    // vectors are materialized.
    if (!PrepayPhysicalTargetInventory(ExpectedTargets,
                                       /*ValidationPasses=*/4))
      return false;
    std::vector<va_t> CandidateTargets = Candidate.ExplicitTargets;
    if (CandidateTargets.empty())
      CandidateTargets = readTableEntries(Img, Candidate);
    if (CandidateTargets.size() != ExpectedTargets)
      return false;
    std::vector<va_t> Checked = CandidateTargets;
    if (!sanityCheckTargets(Img, Checked) ||
        Checked.size() != CandidateTargets.size())
      return false;
    for (va_t Target : CandidateTargets) {
      if (!isValidTarget(Img, Target, CurrentFuncEntry))
        return false;
      // A strong local-table proposal owns basic-block interiors only.  The
      // current entry is a self callback, while a known/typed entry inside a
      // containing symbol remains a separately callable function.  None may
      // be converted into switch ownership merely because the containing body
      // is sized.
      if (Target == CurrentFuncEntry ||
          (KnownFuncEntries && KnownFuncEntries->count(Target)) ||
          Img.hasFunctionSymbolAt(Target))
        return false;
      const bool InAuthoritativeBody = Target > (*OwnershipRange)->first &&
                                       Target < (*OwnershipRange)->second;
      if (!InAuthoritativeBody || !isOwnedInteriorTarget(Img, Target))
        return false;
    }
    return true;
  };
  // A complete object may legitimately mix local basic-block destinations
  // with foreign tail callbacks.  Rewriting that machine `jmp` as
  // CALL+RETURN is still unsound when even one feasible arm is a local
  // interior: the synthetic call pushes a continuation and changes the stack
  // seen by that arm.  Validate every entry in the exact object, classify
  // callable entries separately, and require at least one authenticated local
  // interior.  This certificate preserves only opaque branch identity; it
  // never authorizes the mixed target set as a switch.
  auto ClassifyPhysicalBranchIdentity =
      [&](const JumpTableInfo &Candidate) -> std::optional<bool> {
    if (!AuthoritativeCurrentFuncRange)
      return std::nullopt;
    if (!consumeCandidateProducts({{Candidate.StorageRanges.size(), 4}}))
      return std::nullopt;
    size_t PhysicalSlots = 0;
    for (const JumpTableStorageRange &Range : Candidate.StorageRanges) {
      if (Range.EntrySize == 0 || Range.EntryStride < Range.EntrySize ||
          Range.PhysicalSlotCount == 0 ||
          Range.PhysicalSlotCount >
              limits::kMaxJumpTableEntries - PhysicalSlots)
        return std::nullopt;
      PhysicalSlots += Range.PhysicalSlotCount;
    }
    const size_t ExpectedTargets = Candidate.MaxEntries;
    if (ExpectedTargets < limits::kMinJumpTableEntries ||
        ExpectedTargets > limits::kMaxJumpTableEntries ||
        PhysicalSlots != ExpectedTargets)
      return std::nullopt;
    // readTableEntries performs mapped-owner/canonicalization work, while the
    // classifier below checks executable ownership and typed callable symbols
    // for every decoded slot.  Prepay all attacker-shaped inventories before
    // scanning any of them; a failed charge is resource incompleteness and is
    // handled transactionally by the candidate outcome.
    if (!PrepayPhysicalTargetInventory(ExpectedTargets,
                                       /*ValidationPasses=*/1))
      return std::nullopt;
    std::vector<va_t> Targets =
        readTableEntries(Img, Candidate, nullptr,
                         JumpTableTargetReadPolicy::PhysicalBranchIdentity);
    if (Targets.size() != ExpectedTargets)
      return std::nullopt;

    bool HasLocalInterior = false;
    const uint32_t Align = getInsnAlignment();
    for (va_t Target : Targets) {
      const auto *Segment = Img.getSegmentFor(Target);
      if (!Segment || !Img.hasExecutableCodeOwnerAt(Target) ||
          (Align > 1 && Target % Align != 0))
        return std::nullopt;
      const size_t Offset = static_cast<size_t>(Target - Segment->VA);
      if (!rangeInBounds(Offset, Align, Segment->Data.size()) ||
          !Img.hasExecutableCodeOwnerRange(Target, Align))
        return std::nullopt;
      const bool InAuthoritativeBody =
          Target > AuthoritativeCurrentFuncRange->first &&
          Target < AuthoritativeCurrentFuncRange->second;
      const bool IsOwnedFragment =
          isExplicitlyOwnedFunctionFragment(Img, CurrentFuncEntry, Target);
      const bool IsCallableEntry =
          Target == CurrentFuncEntry ||
          (KnownFuncEntries && KnownFuncEntries->count(Target)) ||
          Img.hasFunctionSymbolAt(Target);
      if (Target == CurrentFuncEntry || IsOwnedFragment) {
        // A self jump re-enters with the current machine frame.  Turning it
        // into an ordinary indirect CALL would push a continuation and grow
        // the stack on every state-machine iteration.  Unwind/chained
        // ownership has the same frame-preserving semantics and takes
        // precedence over an incidental typed-entry label or overlapping
        // authoritative range.
        HasLocalInterior = true;
      } else if (InAuthoritativeBody && !IsCallableEntry) {
        // The general switch-target predicate repeats the same expensive
        // loader ownership scans.  They were already checked immediately
        // above; retain only its instruction-boundary exclusion here.
        auto AtOrAfter = Insns.upper_bound(Target);
        if (AtOrAfter != Insns.begin()) {
          const auto &Prev = *std::prev(AtOrAfter);
          if (Prev.first < Target &&
              Target < Prev.first + static_cast<va_t>(Prev.second.Size))
            return std::nullopt;
        }
        if (!CurrentFuncRange || Target <= CurrentFuncRange->first ||
            Target >= CurrentFuncRange->second)
          return std::nullopt;
        HasLocalInterior = true;
      } else if (!IsCallableEntry) {
        return std::nullopt;
      }
    }
    // true: at least one arm must retain the current frame; false: every
    // decoded arm is an authenticated callable entry; nullopt: the exact
    // physical object contains a target that cannot be classified safely.
    return HasLocalInterior;
  };
  auto ClaimValidatedPotentialTable = [&] {
    if (Info.IndexDomainAuthenticated &&
        HasValidatedLocalPhysicalTargetOwnership(
            Info, /*RequireWholePhysicalLocalSet=*/false)) {
      if (!consumeCandidateEvidence(
              orderedLookupWork(PotentialJumpTableBranches.size()) + 1))
        return false;
      PotentialJumpTableBranches.insert(Rec.Addr);
      CurrentCandidateIsStrongProposal = true;
    }
    return true;
  };

  {
    std::optional<std::set<va_t>> Roots = budgetedJumpTableProofRoots(Info);
    if (!Roots)
      return {};
    ActiveJumpTableProofRoots = std::move(*Roots);
  }

  // Single-level strategies share the same physical LOAD role once they have
  // produced complete occurrence metadata.  Composite strategies populate
  // their ordered roles themselves.  Do not synthesize a role from a bare
  // register number: an older detector that lacks the exact index use point is
  // intentionally rejected by the publication gate and must defer to a richer
  // strategy.
  if (Info.LoadRoles.empty() && !Info.TwoLevelIndex && !Info.TwoTableSelect &&
      Info.HasBaseAddr && !Info.TargetLoads.empty()) {
    // A frame role owns one exact runtime LOAD/complete-EA occurrence.  Until
    // every additional dispatch site has independently reconstructed that
    // role, never copy a singleton certificate across multiple LOADs.
    if (Info.AuthenticatedFrameStorage.RuntimeBase.Use.Value.Size != 0 &&
        Info.TargetLoads.size() != 1)
      return {};
    if (Info.AuthenticatedDisplacedAddress.RuntimeBaseUse.Value.Size != 0 &&
        Info.TargetLoads.size() != 1)
      return {};
    if (!consumeCandidateProducts({{Info.IndexValueAlternatives.size(), 2}}))
      return {};
    std::vector<JumpTableValueOccurrence> Indices = Info.IndexValueAlternatives;
    if (Indices.empty() &&
        (Info.IndexValueAtUse.isReg() || Info.IndexValueAtUse.isTemp() ||
         Info.IndexValueAtUse.isConst()) &&
        Info.IndexValueAtUse.Size != 0) {
      if (!consumeCandidateEvidence(1))
        return {};
      Indices.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                         Info.IndexUseSeq, Info.IndexValueDefinedAtUse});
    }
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (!Indices.empty() && PhysicalStride != 0) {
      if (!consumeCandidateEvidence(Info.TargetLoads.size()))
        return {};
      for (const JumpTableValueOccurrence &Load : Info.TargetLoads) {
        if (!consumeCandidateProducts(
                {{1, 2},
                 {Indices.size(), 3},
                 {Info.AuthenticatedFrameStorage.Initializers.size(), 2}}))
          return {};
        for (const JumpTableFrameInitializerChunk &Initializer :
             Info.AuthenticatedFrameStorage.Initializers)
          if (!consumeCandidateProducts(
                  {{Initializer.StaticSources.size(), 2}}))
            return {};
        JumpTableLoadRole Role;
        Role.Load = Load;
        Role.LoadWidth = Info.EntrySize;
        Role.AllowedBases = {Info.BaseAddr};
        Role.FrameStorage = Info.AuthenticatedFrameStorage;
        Role.DisplacedAddress = Info.AuthenticatedDisplacedAddress;
        Role.Indices = Indices;
        Role.AddressScale = Info.PreScaledIndex ? 1 : PhysicalStride;
        // A logical selector is commonly narrower than the address
        // container (`w`/`edi` -> zext -> pointer-width scale).  The role
        // query still resolves this exact occurrence through the CFG and only
        // accepts an explicit zero extension; it must not require the public
        // selector witness itself to be widened and thereby lose its guard or
        // source-level case-label width.
        Role.AllowZeroExtension =
            std::any_of(Indices.begin(), Indices.end(), [&](const auto &Index) {
              return Index.Value.Size != 0 &&
                     Index.Value.Size < Img.getPointerSize();
            });
        Info.LoadRoles.push_back(std::move(Role));
      }
    }
  }
  if (!Info.TwoLevelIndex && !Info.TwoTableSelect) {
    if (!consumeCandidateEvidence(Info.LoadRoles.size()))
      return {};
    for (JumpTableLoadRole &Role : Info.LoadRoles)
      if (!consumeCandidateEvidence(
              detail::indexExtensionComparisonWork(Role.Indices.size())
                  .value_or(std::numeric_limits<size_t>::max())))
        return {};
      else {
        bool AllowsZeroExtension = false;
        for (const auto &Index : Role.Indices) {
          bool Narrow = true;
          Narrow &= Index.Value.Size != 0;
          Narrow &= Index.Value.Size < Img.getPointerSize();
          AllowsZeroExtension |= Narrow;
        }
        if (AllowsZeroExtension)
          Role.AllowZeroExtension = true;
      }
  }

  // FS/GS addresses are offsets in a target segment, not process-image VAs.
  // Reject them at the common publication gate after every strategy has
  // materialized its exact physical LOAD inventory.  Shape-specific filters
  // are insufficient: branch-local, symbolic, stack and composite strategies
  // can all produce a superficially ordinary LOAD -> INDIR_BR certificate.
  auto IsDefaultAddressSpaceLoad =
      [&](const JumpTableValueOccurrence &Occurrence) -> std::optional<bool> {
    if (!consumeCandidateEvidence(
            detail::kDefaultAddressSpaceLoadFixedComparisonWork))
      return std::nullopt;
    bool ValidOccurrence = true;
    ValidOccurrence &= Occurrence.Addr != InvalidVA;
    ValidOccurrence &= Occurrence.Seq >= 0;
    if (!ValidOccurrence)
      return std::nullopt;
    if (!consumeCandidateEvidence(orderedLookupWork(Insns.size())))
      return std::nullopt;
    auto Insn = Insns.find(Occurrence.Addr);
    if (Insn == Insns.end())
      return std::nullopt;
    const std::optional<size_t> ComparisonWork =
        detail::defaultAddressSpaceLoadComparisonWork(Insn->second.Ops.size());
    const size_t VariableComparisonWork =
        ComparisonWork ? *ComparisonWork -
                             detail::kDefaultAddressSpaceLoadFixedComparisonWork
                       : std::numeric_limits<size_t>::max();
    if (!consumeCandidateEvidence(VariableComparisonWork))
      return std::nullopt;
    return jumpTableTargetLoadUsesDefaultAddressSpace(
        Insn->second.Ops, Occurrence.Addr, Occurrence.Seq, Occurrence.Value);
  };
  auto RejectSegmentedLoad = [&](const JumpTableValueOccurrence &Occurrence) {
    const std::optional<bool> IsDefault = IsDefaultAddressSpaceLoad(Occurrence);
    if (!IsDefault)
      CandidateEvidenceAnalysisIncomplete = true;
    return !IsDefault || !*IsDefault;
  };
  if (std::any_of(Info.TargetLoads.begin(), Info.TargetLoads.end(),
                  RejectSegmentedLoad) ||
      std::any_of(Info.LoadRoles.begin(), Info.LoadRoles.end(),
                  [&](const JumpTableLoadRole &Role) {
                    return RejectSegmentedLoad(Role.Load);
                  }))
    return {};

  // Shape detection is only a candidate generator.  Before any static,
  // explicit, relocation-bounded, or emulated result can be published, prove
  // that the actual INDIR_BR input is derived from the exact authenticated
  // table LOAD occurrence(s) on every feasible path.  An unrelated prior LOAD
  // at the same addresses, or emulator address co-occurrence, is not evidence.
  struct TargetRoleProofCertificate {
    const InsnRecord *RecordIdentity = nullptr;
    const BinaryImage *ImageIdentity = nullptr;
    va_t BranchAddr = InvalidVA;
    uint32_t PointerSize = 0;
    detail::TargetRoleProofContextKey ProofContext;
    bool HasBaseAddr = false;
    va_t BaseAddr = InvalidVA;
    uint16_t EntrySize = 0;
    bool IsRelative = false;
    bool IsSigned = false;
    uint32_t EntryScale = 0;
    bool HasTargetBase = false;
    va_t TargetBase = InvalidVA;
    bool UsesDefinedOccurrenceRoots = false;
    std::map<va_t, std::vector<va_t>> CertifiedEdgeOverrides;
    std::vector<JumpTableValueOccurrence> TargetLoads;
  };
  auto EffectiveProofRoots = [&]() -> const std::set<va_t> & {
    return ActiveJumpTableProofRoots ? *ActiveJumpTableProofRoots
                                     : PersistentCFGRoots;
  };
  std::optional<TargetRoleProofCertificate> TargetRoleCertificate;
  bool TargetRoleUsesDefinedOccurrenceRoots = false;
  std::map<va_t, std::vector<va_t>> RelativeEdgeOverrides;
  const std::map<va_t, std::vector<va_t>> *TargetRoleEdgeOverrides = nullptr;
  const std::vector<va_t> *CurrentCandidateSelfReplayTargets = nullptr;
  std::optional<ProvisionalRelativeEdgeProposal>
      ProvisionalRelativeEdgeTemplate;
  bool HasExactTargetLoadOccurrences = false;
  if (CandidateProposalStageActive && InspectRelativeRun &&
      PhysicalEntryStride >= Info.EntrySize) {
    if (!consumeCandidateEvidence(3) ||
        !consumeCandidateProducts(
            {{Info.TargetLoads.size(), 8}, {Info.LoadRoles.size(), 1}}) ||
        !consumeCandidateFactorProduct(
            {Info.TargetLoads.size(), Info.LoadRoles.size(), 16}))
      return {};
    HasExactTargetLoadOccurrences =
        !Info.TargetLoads.empty() &&
        Info.TargetLoads.size() == Info.LoadRoles.size() &&
        std::all_of(Info.TargetLoads.begin(), Info.TargetLoads.end(),
                    [&](const JumpTableValueOccurrence &Load) {
                      return Load.Addr != InvalidVA && Load.Seq >= 0 &&
                             Load.DefinedAtPoint &&
                             (Load.Value.isReg() || Load.Value.isTemp()) &&
                             Load.Value.Size == Info.EntrySize;
                    }) &&
        std::all_of(Info.LoadRoles.begin(), Info.LoadRoles.end(),
                    [&](const JumpTableLoadRole &Role) {
                      return Role.LoadWidth == Info.EntrySize &&
                             std::count(Info.TargetLoads.begin(),
                                        Info.TargetLoads.end(), Role.Load) == 1;
                    }) &&
        std::all_of(Info.TargetLoads.begin(), Info.TargetLoads.end(),
                    [&](const JumpTableValueOccurrence &Load) {
                      return std::count_if(Info.LoadRoles.begin(),
                                           Info.LoadRoles.end(),
                                           [&](const JumpTableLoadRole &Role) {
                                             return Role.Load == Load;
                                           }) == 1;
                    });
    if (HasExactTargetLoadOccurrences) {
      const std::optional<uint32_t> RelocationRun =
          InspectBoundedRelativeRelocationRun();
      if (!RelocationRun)
        return {};
      if (PhysicalRawRelCodeRunComplete &&
          *RelocationRun >= limits::kMinJumpTableEntries) {
        Info.ExactBoundedRelativeRelocationSlots = *RelocationRun;
        JumpTableInfo PhysicalProbe;
        PhysicalProbe.setBaseAddr(Info.BaseAddr);
        PhysicalProbe.EntrySize = Info.EntrySize;
        PhysicalProbe.EntryStride = PhysicalEntryStride;
        PhysicalProbe.MaxEntries = *RelocationRun;
        PhysicalProbe.PhysicalCapacity = *RelocationRun;
        PhysicalProbe.ExactBoundedRelativeRelocationSlots = *RelocationRun;
        PhysicalProbe.IsRelative = true;
        PhysicalProbe.IsSigned = Info.IsSigned;
        PhysicalProbe.EntryScale = Info.EntryScale;
        if (Info.HasTargetBase)
          PhysicalProbe.setTargetBase(Info.TargetBase);
        if (!consumeCandidateEvidence(4))
          return {};
        PhysicalProbe.StorageRanges.reserve(1);
        PhysicalProbe.StorageRanges.push_back(
            JumpTableStorageRange{Info.BaseAddr, Info.EntrySize,
                                  PhysicalEntryStride, *RelocationRun});
        if (HasValidatedLocalPhysicalTargetOwnership(
                PhysicalProbe, /*RequireWholePhysicalLocalSet=*/true)) {
          if (!consumeCandidateProducts({{Info.LoadRoles.size(), 3}}))
            return {};
          ProvisionalRelativeEdgeProposal Proposal;
          Proposal.Storage = PhysicalProbe.StorageRanges.front();
          Proposal.AuthenticatesPhysicalStorage = true;
          Proposal.TargetAnchor =
              Info.HasTargetBase ? Info.TargetBase : Info.BaseAddr;
          Proposal.EntryScale = Info.EntryScale;
          Proposal.IsSigned = Info.IsSigned;
          Proposal.LoadRoles.reserve(Info.LoadRoles.size());
          for (const JumpTableLoadRole &Role : Info.LoadRoles)
            Proposal.LoadRoles.push_back({Role.Load, Role.LoadWidth});
          Proposal.ProofRank = ActiveJumpTableCandidateProofRank;
          ProvisionalRelativeEdgeTemplate = std::move(Proposal);
        }
      }
    }
  }
  // A relative table may borrow every immutable, lower-rank edge from the
  // preceding complete proposal round.  These are authenticated CFG facts,
  // not physical-table facts: distinct inline tables may therefore close a
  // mutually dependent graph without claiming one another's storage.  The
  // current stage's exploration map remains excluded so candidate address
  // order cannot affect the proof.
  bool HasDistinctSameStorageSiblingDemand = false;
  bool RetainPriorDurableSelfCertificate = false;
  std::optional<JumpTableStorageRange> DurableDemandStorage;
  if (ProvisionalRelativeEdgeTemplate) {
    DurableDemandStorage = ProvisionalRelativeEdgeTemplate->Storage;
  } else if (CandidateProposalStageActive && InspectRelativeRun &&
             HasExactTargetLoadOccurrences && Info.EntrySize > 0 &&
             PhysicalEntryStride >= Info.EntrySize) {
    // The edge-only template is materialized only after target/address-role
    // proof below, but its immutable storage identity is already exact here.
    // Cache that scalar identity so this paid prior-map traversal can decide
    // sibling demand without a second final scan.
    DurableDemandStorage = JumpTableStorageRange{Info.BaseAddr, Info.EntrySize,
                                                 PhysicalEntryStride, 1};
  }
  if (CandidateProposalStageActive && InspectRelativeRun) {
    if (!consumeCandidateEvidence(PriorProvisionalRelativeEdges.size()))
      return {};
    for (const auto &[Addr, Proposal] : PriorProvisionalRelativeEdges) {
      if (!consumeCandidateEvidence(1))
        return {};
      const bool Durable = Proposal.CompleteRuntimeStorageRange.has_value() ||
                           Proposal.StablePublishedTargetReplay;
      if (Addr == Rec.Addr) {
        // This pass may reconstruct its template only after the prior-edge
        // scan; retain the already-paid self fact independently of that order.
        RetainPriorDurableSelfCertificate |= Durable;
      } else if (!Durable && DurableDemandStorage) {
        // This scan already owns the immutable proposal traversal.  Charge
        // the conservative four-field storage-comparison ceiling before the
        // later durable-certificate gate instead of rescanning the entire map
        // at the final publication boundary.  PhysicalSlotCount is
        // intentionally excluded from this edge-only shape: a raw relocation
        // run may be wider than the sibling's exact runtime domain.
        if (!consumeCandidateEvidence(4))
          return {};
        bool SameStorageShape = true;
        SameStorageShape &=
            DurableDemandStorage->BaseAddr == Proposal.Storage.BaseAddr;
        SameStorageShape &=
            DurableDemandStorage->EntrySize == Proposal.Storage.EntrySize;
        SameStorageShape &=
            DurableDemandStorage->EntryStride == Proposal.Storage.EntryStride;
        HasDistinctSameStorageSiblingDemand |= SameStorageShape;
      }
      bool BorrowEdge = false;
      if (Addr == Rec.Addr) {
        const bool StablePublishedReplay = Proposal.StablePublishedTargetReplay;
        const bool CompleteRuntimeReplay =
            Proposal.CompleteRuntimeStorageRange.has_value();
        const bool DurablePublishedReplay =
            StablePublishedReplay || CompleteRuntimeReplay;
        if ((!CurrentCandidateMayReplayOwnProvisionalRelativeEdge &&
             !DurablePublishedReplay) ||
            !ProvisionalRelativeEdgeTemplate)
          continue;
        const std::optional<size_t> ComparisonWork =
            detail::provisionalRelativeProposalComparisonWork(
                ProvisionalRelativeEdgeTemplate->LoadRoles.size(),
                Proposal.LoadRoles.size(), /*LeftTargetCount=*/0,
                Proposal.Targets.size());
        if (!ComparisonWork || !consumeCandidateEvidence(*ComparisonWork) ||
            !consumeCandidateProducts(
                {{Proposal.Targets.size(),
                  orderedLookupWork(ExploredAddrs.size())},
                 {Proposal.Targets.size(),
                  orderedLookupWork(BlockStarts.size())},
                 {Proposal.Targets.size(), orderedLookupWork(Insns.size())}}))
          return {};
        if (DurablePublishedReplay) {
          const std::optional<size_t> PublishedComparisonWork =
              detail::scalarVectorComparisonWork(Rec.JumpTableTargets.size(),
                                                 Proposal.Targets.size());
          if (!PublishedComparisonWork ||
              !consumeCandidateEvidence(*PublishedComparisonWork))
            return {};
        }
        const ProvisionalRelativeEdgeProposal &Current =
            *ProvisionalRelativeEdgeTemplate;
        bool SameImmutableShape = !Current.CompleteRuntimeStorageRange;
        SameImmutableShape &=
            sameStorageRangeNoShortCircuit(Proposal.Storage, Current.Storage);
        SameImmutableShape &= Proposal.TargetAnchor == Current.TargetAnchor;
        SameImmutableShape &= Proposal.EntryScale == Current.EntryScale;
        SameImmutableShape &= Proposal.IsSigned == Current.IsSigned;
        SameImmutableShape &= sameLoadRoleVectorsNoShortCircuit(
            Proposal.LoadRoles, Current.LoadRoles);
        const bool EveryTargetAlreadyDecoded = std::all_of(
            Proposal.Targets.begin(), Proposal.Targets.end(), [&](va_t Target) {
              return ExploredAddrs.count(Target) && BlockStarts.count(Target) &&
                     Insns.count(Target);
            });
        if (DurablePublishedReplay) {
          bool PublishedTargetsMatch =
              !Rec.JumpTableTargets.empty() &&
              Rec.JumpTableTargets.size() == Proposal.Targets.size();
          for (size_t I = 0; I < std::min(Rec.JumpTableTargets.size(),
                                          Proposal.Targets.size());
               ++I)
            PublishedTargetsMatch &=
                Rec.JumpTableTargets[I] == Proposal.Targets[I];
          bool StableReplayShape =
              SameImmutableShape &&
              Proposal.ProofRank == ActiveJumpTableCandidateProofRank &&
              Proposal.ProofRank == ActiveJumpTableCandidateDependencyRank;
          if (StablePublishedReplay)
            StableReplayShape &= !Proposal.AuthenticatesPhysicalStorage &&
                                 !Proposal.CompleteDenseRuntimeCoordinates &&
                                 !Proposal.CompleteRuntimeStorageRange;
          if (CompleteRuntimeReplay)
            StableReplayShape &=
                !Proposal.StablePublishedTargetReplay &&
                detail::isStableProvisionalRelativeRuntimeCertificate(
                    &*Proposal.CompleteRuntimeStorageRange, Proposal.Targets,
                    Rec.JumpTableTargets);
          const bool ReplayStablePublishedTargets = StableReplayShape &&
                                                    PublishedTargetsMatch &&
                                                    EveryTargetAlreadyDecoded;
          CurrentCandidateReplayedOwnProvisionalRelativeEdge |=
              ReplayStablePublishedTargets;
          if (ReplayStablePublishedTargets)
            CurrentCandidateSelfReplayTargets = &Proposal.Targets;
          continue;
        }
        SameImmutableShape &= Proposal.AuthenticatesPhysicalStorage;
        SameImmutableShape &= Proposal.CompleteDenseRuntimeCoordinates;
        SameImmutableShape &=
            Proposal.ProofRank == ActiveJumpTableCandidateDependencyRank;
        SameImmutableShape &=
            ActiveJumpTableCandidateProofRank == Proposal.ProofRank + 1;
        BorrowEdge = SameImmutableShape && EveryTargetAlreadyDecoded;
        CurrentCandidateReplayedOwnProvisionalRelativeEdge |= BorrowEdge;
      } else {
        if (Proposal.CompleteRuntimeStorageRange ||
            Proposal.StablePublishedTargetReplay)
          continue;
        BorrowEdge = detail::canBorrowProvisionalRelativeEdge(
            Rec.Addr, ActiveJumpTableCandidateDependencyRank, Addr,
            Proposal.ProofRank);
      }
      if (Proposal.CompleteRuntimeStorageRange) {
        // A complete runtime certificate is useful only while its producer's
        // ordinary published CFG edges remain byte-for-byte identical.  Those
        // edges are already in the graph, so never inject the certificate as a
        // provisional overlay.  The sibling scan and stable-stage gate below
        // perform the authoritative published-target comparison.
        continue;
      }
      if (!consumeCandidateEvidence(7))
        return {};
      if (!BorrowEdge || Proposal.Targets.empty())
        continue;
      if (!consumeCandidateProducts({{Proposal.Targets.size(), 3}}) ||
          !consumeCandidateEvidence(
              orderedLookupWork(RelativeEdgeOverrides.size()) + 3))
        return {};
      RelativeEdgeOverrides.emplace(Addr, Proposal.Targets);
    }
    if (!RelativeEdgeOverrides.empty())
      TargetRoleEdgeOverrides = &RelativeEdgeOverrides;
  }
  bool TargetRoleComplete = false;
  bool TargetRole = branchTargetDependsOnTableLoad(
      Rec, Info, &CandidateEvidenceBudget, &TargetRoleComplete,
      /*UseDefinedAlternativesAsRoots=*/false, TargetRoleEdgeOverrides);
  // Occurrence roots are an alternate proof mode, not a generic escape hatch
  // after any failed target query.  Enable them only when an independent
  // strategy has already authenticated the composite shape, when the
  // absolute-table detector retained a complete parallel consumer inventory.
  // Relative tables never use occurrence roots: their table-base provenance
  // must remain reachable through the immutable certified-edge overlay above.
  const bool HasExactRootedTargetRoleCertificate =
      Info.CompositeShapeClaimed ||
      (Info.RelocAbsolute && !ExactConsumerGroup.IndexOccurrences.empty() &&
       ExactConsumerGroup.IndexOccurrences.size() ==
           ExactConsumerGroup.BranchAddrs.size());
  if ((!TargetRole || !TargetRoleComplete) &&
      HasExactRootedTargetRoleCertificate) {
    bool RootedTargetRoleComplete = false;
    const bool RootedTargetRole = branchTargetDependsOnTableLoad(
        Rec, Info, &CandidateEvidenceBudget, &RootedTargetRoleComplete,
        /*UseDefinedAlternativesAsRoots=*/true);
    if (RootedTargetRole && RootedTargetRoleComplete) {
      TargetRole = true;
      TargetRoleComplete = true;
      TargetRoleUsesDefinedOccurrenceRoots = true;
      TargetRoleEdgeOverrides = nullptr;
    } else {
      TargetRole = false;
      TargetRoleComplete &= RootedTargetRoleComplete;
    }
  }
  CandidateEvidenceAnalysisIncomplete |= !TargetRoleComplete;
  if (!TargetRole || !TargetRoleComplete)
    return {};
  // An overlay-backed target proof is valid only in this immutable proposal
  // universe.  Keep the published candidate in the complete-CFG replay set so
  // retirement (or growth) of the borrowed edge forces one final proof without
  // the overlay; the edge certificate itself grants no storage authority.
  if (TargetRoleEdgeOverrides && !TargetRoleEdgeOverrides->empty())
    RequestedCompleteJumpTableProof = true;

  // Cache only this complete, positive target-role certificate inside the
  // current immutable resolveJumpTable invocation.  The address-role proof
  // below may transactionally prune TargetLoads, so capture the exact target
  // inputs before calling it.  Candidate storage/domain metadata is not read
  // by the target proof; any storage effect is represented by the effective
  // proof roots retained in this key.
  auto CaptureTargetRoleCertificate = [&]() {
    const std::set<va_t> &Roots = EffectiveProofRoots();
    size_t CertifiedEdgeTargetCount = 0;
    if (TargetRoleEdgeOverrides) {
      if (!consumeCandidateEvidence(TargetRoleEdgeOverrides->size())) {
        CandidateEvidenceAnalysisIncomplete = true;
        return false;
      }
      for (const auto &[Addr, Targets] : *TargetRoleEdgeOverrides) {
        (void)Addr;
        if (Targets.size() >
            std::numeric_limits<size_t>::max() - CertifiedEdgeTargetCount) {
          consumeCandidateEvidence(std::numeric_limits<size_t>::max());
          CandidateEvidenceAnalysisIncomplete = true;
          return false;
        }
        CertifiedEdgeTargetCount += Targets.size();
      }
    }
    if (!consumeCandidateProducts(
            {{1, 20},
             {Info.TargetLoads.size(), 3},
             {Roots.size(), 3},
             {TargetRoleEdgeOverrides ? TargetRoleEdgeOverrides->size() : 0, 4},
             {CertifiedEdgeTargetCount, 3}})) {
      CandidateEvidenceAnalysisIncomplete = true;
      return false;
    }
    TargetRoleCertificate.emplace(TargetRoleProofCertificate{
        &Rec,
        &Img,
        Rec.Addr,
        Img.getPointerSize(),
        {JumpTableProofContextComplete, ActiveJumpTableProofRoots.has_value(),
         ActiveJumpTableConsumerAudit,
         std::vector<va_t>(Roots.begin(), Roots.end())},
        Info.HasBaseAddr,
        Info.BaseAddr,
        Info.EntrySize,
        Info.IsRelative,
        Info.IsSigned,
        Info.EntryScale,
        Info.HasTargetBase,
        Info.TargetBase,
        TargetRoleUsesDefinedOccurrenceRoots,
        TargetRoleEdgeOverrides ? *TargetRoleEdgeOverrides
                                : std::map<va_t, std::vector<va_t>>{},
        Info.TargetLoads});
    return true;
  };
  if (!CaptureTargetRoleCertificate())
    return {};
  auto TargetRoleCertificateMatches = [&]() -> std::optional<bool> {
    const TargetRoleProofCertificate &Certificate = *TargetRoleCertificate;
    const std::set<va_t> &CurrentRoots = EffectiveProofRoots();
    const std::map<va_t, std::vector<va_t>> EmptyEdgeOverrides;
    const auto &CurrentEdgeOverrides =
        TargetRoleEdgeOverrides ? *TargetRoleEdgeOverrides : EmptyEdgeOverrides;
    // Nineteen immutable scalar/context and container-size checks are
    // prepaid before comparison.  Full occurrence comparison covers NdVar's
    // five fields and Addr/Seq/DefinedAtPoint; roots are already ordered.
    if (!consumeCandidateEvidence(
            detail::kTargetRoleCertificateFixedComparisonWork))
      return std::nullopt;
    const bool FixedInputsMatch =
        Certificate.RecordIdentity == &Rec &&
        Certificate.ImageIdentity == CurrentImg &&
        Certificate.BranchAddr == Rec.Addr &&
        Certificate.PointerSize == Img.getPointerSize() &&
        Certificate.HasBaseAddr == Info.HasBaseAddr &&
        Certificate.BaseAddr == Info.BaseAddr &&
        Certificate.EntrySize == Info.EntrySize &&
        Certificate.IsRelative == Info.IsRelative &&
        Certificate.IsSigned == Info.IsSigned &&
        Certificate.EntryScale == Info.EntryScale &&
        Certificate.HasTargetBase == Info.HasTargetBase &&
        Certificate.TargetBase == Info.TargetBase &&
        Certificate.UsesDefinedOccurrenceRoots ==
            TargetRoleUsesDefinedOccurrenceRoots &&
        Certificate.CertifiedEdgeOverrides.size() ==
            CurrentEdgeOverrides.size() &&
        Certificate.TargetLoads.size() == Info.TargetLoads.size();
    if (!FixedInputsMatch)
      return false;
    if (!consumeCandidateProducts(
            {{Certificate.TargetLoads.size(), 8},
             {Certificate.ProofContext.ProofRoots.size(), 1},
             {Certificate.CertifiedEdgeOverrides.size(), 2}}))
      return std::nullopt;
    auto EdgeIt = Certificate.CertifiedEdgeOverrides.begin();
    auto CurrentEdgeIt = CurrentEdgeOverrides.begin();
    while (EdgeIt != Certificate.CertifiedEdgeOverrides.end()) {
      if (!consumeCandidateProducts(
              {{EdgeIt->second.size(), 1}, {CurrentEdgeIt->second.size(), 1}}))
        return std::nullopt;
      ++EdgeIt;
      ++CurrentEdgeIt;
    }
    return Certificate.TargetLoads == Info.TargetLoads &&
           Certificate.CertifiedEdgeOverrides == CurrentEdgeOverrides &&
           detail::targetRoleProofContextMatches(
               Certificate.ProofContext, JumpTableProofContextComplete,
               ActiveJumpTableProofRoots.has_value(),
               ActiveJumpTableConsumerAudit, CurrentRoots);
  };
  auto EnsureTargetRoleProofCurrent = [&]() {
    const std::optional<bool> InputsUnchanged = TargetRoleCertificateMatches();
    if (!InputsUnchanged) {
      CandidateEvidenceAnalysisIncomplete = true;
      return false;
    }
    if (*InputsUnchanged)
      return true;
    bool Complete = false;
    const bool Proven = branchTargetDependsOnTableLoad(
        Rec, Info, &CandidateEvidenceBudget, &Complete,
        TargetRoleUsesDefinedOccurrenceRoots, TargetRoleEdgeOverrides);
    CandidateEvidenceAnalysisIncomplete |= !Complete;
    return Proven && Complete && CaptureTargetRoleCertificate();
  };

  // Preserve the complete target-role occurrence inventory before the
  // address proof narrows it to roles reachable in the current immutable CFG.
  // A strong proposal uses this inventory only transactionally: it may stop a
  // newly proposed table LOAD from being classified as an independent
  // consumer of its own storage, while the next graph round must rediscover
  // and address-authenticate every role before stable publication.
  std::vector<StrongJumpTableLoadRole> PrePruningLoadRoles;
  if (CandidateProposalStageActive) {
    if (!consumeCandidateProducts({{Info.LoadRoles.size(), 3}}) ||
        !consumeCandidateEvidence(4))
      return {};
    PrePruningLoadRoles.reserve(Info.LoadRoles.size());
    for (const JumpTableLoadRole &Role : Info.LoadRoles)
      PrePruningLoadRoles.push_back({Role.Load, Role.LoadWidth});
  }

  bool AddressRoleComplete = false;
  const bool AddressRole = tableLoadAddressesMatchRole(
      Info, &CandidateEvidenceBudget, &AddressRoleComplete,
      TargetRoleUsesDefinedOccurrenceRoots, TargetRoleEdgeOverrides);
  CandidateEvidenceAnalysisIncomplete |= !AddressRoleComplete;
  if (!AddressRole || !AddressRoleComplete)
    return {};
  if (!EnsureTargetRoleProofCurrent())
    return {};

  // Relocation-free inline relative tables have no independently bounded
  // physical object to certify, but the exact LOAD/role bijection plus the
  // completed target/address proofs above can still authenticate whichever
  // finite edges the domain solver subsequently reads and validates.  Retain
  // only an address identity for that edge certificate.  In particular, this
  // template grants no storage ownership, root suppression, selector domain,
  // or singleton-publication authority.
  if (CandidateProposalStageActive && InspectRelativeRun &&
      !ProvisionalRelativeEdgeTemplate && HasExactTargetLoadOccurrences &&
      Info.EntrySize > 0 && PhysicalEntryStride >= Info.EntrySize) {
    if (!consumeCandidateEvidence(8) ||
        !consumeCandidateProducts({{PrePruningLoadRoles.size(), 3}}))
      return {};
    ProvisionalRelativeEdgeProposal Proposal;
    Proposal.Storage = JumpTableStorageRange{Info.BaseAddr, Info.EntrySize,
                                             PhysicalEntryStride, 1};
    Proposal.AuthenticatesPhysicalStorage = false;
    Proposal.TargetAnchor =
        Info.HasTargetBase ? Info.TargetBase : Info.BaseAddr;
    Proposal.EntryScale = Info.EntryScale;
    Proposal.IsSigned = Info.IsSigned;
    Proposal.LoadRoles = PrePruningLoadRoles;
    Proposal.ProofRank = ActiveJumpTableCandidateProofRank;
    ProvisionalRelativeEdgeTemplate = std::move(Proposal);
  }
  // A complete dense runtime range published by a distinct sibling in the
  // preceding immutable stage may close this candidate's exact singleton
  // domain.  It is deliberately separate from physical ownership: matching it
  // grants neither a strong role nor relocation-root suppression.  Scan the
  // whole prior universe under the candidate account so map order cannot pick
  // one of two inconsistent certificates.
  const JumpTableStorageRange *CertifiedSiblingRuntimeStorage = nullptr;
  const std::vector<va_t> *CertifiedSiblingRuntimeTargets = nullptr;
  if (CandidateProposalStageActive && ProvisionalRelativeEdgeTemplate &&
      InspectRelativeRun && !TargetRoleEdgeOverrides) {
    if (!consumeCandidateEvidence(PriorProvisionalRelativeEdges.size()))
      return {};
    const va_t TargetAnchor =
        Info.HasTargetBase ? Info.TargetBase : Info.BaseAddr;
    const detail::ProvisionalRelativeRuntimeCertificateShape CandidateShape{
        Rec.Addr,
        JumpTableStorageRange{Info.BaseAddr, Info.EntrySize,
                              PhysicalEntryStride, 0},
        TargetAnchor, Info.EntryScale, Info.IsSigned};
    for (const auto &[Addr, Proposal] : PriorProvisionalRelativeEdges) {
      // Prepay the worst fixed path through the optional-range presence check,
      // shape/range validation, producer publication validation, and conflict
      // detection against an earlier matching certificate.  The two possible
      // target-vector passes are charged independently below.
      if (!consumeCandidateEvidence(
              detail::kSiblingRelativeRuntimeCertificateFixedWork))
        return {};
      if (!consumeCandidateEvidence(orderedLookupWork(Insns.size())))
        return {};
      const auto Producer = Insns.find(Addr);
      if (!consumeCandidateProducts(
              {{Proposal.Targets.size(),
                detail::kSiblingRelativeRuntimeCertificateTargetPasses}}))
        return {};
      const detail::ProvisionalRelativeRuntimeCertificateShape ProposalShape{
          Addr, Proposal.Storage, Proposal.TargetAnchor, Proposal.EntryScale,
          Proposal.IsSigned};
      const JumpTableStorageRange *RuntimeRange =
          Proposal.CompleteRuntimeStorageRange
              ? &*Proposal.CompleteRuntimeStorageRange
              : nullptr;
      const auto Match = detail::matchProvisionalRelativeRuntimeCertificate(
          CandidateShape, ProposalShape, RuntimeRange, Proposal.Targets);
      if (Match ==
          detail::ProvisionalRelativeRuntimeCertificateMatch::NotApplicable)
        continue;
      if (Match ==
          detail::ProvisionalRelativeRuntimeCertificateMatch::Malformed) {
        CandidateEvidenceAnalysisIncomplete = true;
        return {};
      }
      // Match already validated the complete runtime payload.  Avoid calling
      // the stable helper here because that would repeat the payload checks;
      // only the producer's ordinary published targets remain to validate.
      if (Producer == Insns.end() ||
          Producer->second.JumpTableTargets.empty() ||
          Producer->second.JumpTableTargets != Proposal.Targets)
        continue;
      if (CertifiedSiblingRuntimeStorage &&
          (!CertifiedSiblingRuntimeTargets || !RuntimeRange ||
           !detail::sameProvisionalRelativeRuntimeCertificatePayload(
               *CertifiedSiblingRuntimeStorage, *CertifiedSiblingRuntimeTargets,
               *RuntimeRange, Proposal.Targets))) {
        CandidateEvidenceAnalysisIncomplete = true;
        return {};
      }
      // The matched certificate is borrowed immutable-stage evidence.  Keep
      // that dependency in the proposal so final publication replays it over
      // the complete CFG, even when this consumer also has an independent
      // selector mask.
      RequestedCompleteJumpTableProof |= detail::
          provisionalRelativeRuntimeCertificateRequiresCompleteCFGReplay(Match);
      CertifiedSiblingRuntimeStorage = RuntimeRange;
      CertifiedSiblingRuntimeTargets = &Proposal.Targets;
    }
    CandidateAwaitingSiblingRuntimeCertificate =
        HasDistinctSameStorageSiblingDemand &&
        CertifiedSiblingRuntimeStorage == nullptr;
  }

  // Clang -O0 commonly decouples one absolute table LOAD from a shared
  // indirect branch by spilling the loaded target to a frame slot.  The
  // instruction-local group detector deliberately requires two distinct
  // branches, so it cannot seed this single shared-dispatch shape.  Once both
  // role proofs above have completed, and the whole physical object is an
  // exact set of local basic-block targets, retain the already authenticated
  // index occurrence as a one-branch relay group.  This is not available to a
  // direct callback load (whose target LOAD is the branch instruction) or to a
  // foreign/self-entry table.
  if (!consumeCandidateEvidence(
          detail::decoupledRelayTargetLoadComparisonWork(
              Info.TargetLoads.size())
              .value_or(std::numeric_limits<size_t>::max())))
    return {};
  bool EveryRelayLoadIsDistinct = true;
  for (const JumpTableValueOccurrence &Load : Info.TargetLoads) {
    bool Distinct = true;
    Distinct &= Load.Addr != InvalidVA;
    Distinct &= Load.Addr != Rec.Addr;
    EveryRelayLoadIsDistinct &= Distinct;
  }
  bool DecoupledAbsoluteRelay = ExactConsumerGroup.IndexOccurrences.empty();
  DecoupledAbsoluteRelay &= Info.RelocAbsolute;
  DecoupledAbsoluteRelay &= !Info.IsRelative;
  DecoupledAbsoluteRelay &= !Info.TargetLoads.empty();
  DecoupledAbsoluteRelay &= EveryRelayLoadIsDistinct;
  bool WholePhysicalRelayTargetsAreLocal = false;
  if (DecoupledAbsoluteRelay &&
      Info.PhysicalCapacity >= limits::kMinJumpTableEntries) {
    const uint32_t SavedMaxEntries = Info.MaxEntries;
    Info.MaxEntries = Info.PhysicalCapacity;
    WholePhysicalRelayTargetsAreLocal =
        HasValidatedLocalPhysicalTargetOwnership(
            Info, /*RequireWholePhysicalLocalSet=*/true);
    Info.MaxEntries = SavedMaxEntries;
  }
  if (WholePhysicalRelayTargetsAreLocal) {
    const size_t OccurrenceCount = Info.IndexValueAlternatives.empty()
                                       ? size_t{1}
                                       : Info.IndexValueAlternatives.size();
    const bool HasFallbackOccurrence =
        Info.IndexValueAtUse.Size != 0 && Info.IndexUseAddr != InvalidVA &&
        Info.IndexUseSeq >= 0 && !Info.IndexValueDefinedAtUse;
    if ((Info.IndexValueAlternatives.empty() && !HasFallbackOccurrence) ||
        !consumeCandidateProducts({{OccurrenceCount, 7}, {1, 4}}))
      return {};
    ExactConsumerGroup.IndexOccurrences.reserve(OccurrenceCount);
    ExactConsumerGroup.BranchAddrs.reserve(OccurrenceCount);
    if (Info.IndexValueAlternatives.empty())
      ExactConsumerGroup.IndexOccurrences.push_back(
          {Info.IndexValueAtUse, Info.IndexUseAddr, Info.IndexUseSeq,
           Info.IndexValueDefinedAtUse});
    else
      ExactConsumerGroup.IndexOccurrences = Info.IndexValueAlternatives;
    ExactConsumerGroup.BranchAddrs.assign(OccurrenceCount, Rec.Addr);
    ExactConsumerGroup.MinimumPresentBranches = 1;
  }
  Info.MutatedUnsafe |= ModuleMutationUnsafe;

  // A runtime-selected dispatch over two non-adjacent code-pointer tables
  // carries its complete target set explicitly (the union of both runs), which
  // no single-base contiguous read can reconstruct.  Use it verbatim: the
  // guard / normalization / stride / emulation machinery below all assume one
  // contiguous base and would corrupt the two-run layout.  The set is already
  // exact and validated (every entry a resolved in-function code pointer), so
  // the dispatch lowers directly to the merged two-table switch.
  if (!Info.ExplicitTargets.empty()) {
    if (!Info.IndexDomainAuthenticated)
      return {};
    if (!JumpTableProofContextComplete && RequestedCompleteJumpTableProof)
      return {};
    // The returned vector becomes persistent stage state.  Reserve its
    // rollback destruction before copying any target element.
    if (!reservePublishedTargetCleanup(Info.ExplicitTargets.size()))
      return {};
    std::vector<va_t> Targets = Info.ExplicitTargets;
    CandidateTargetMaterializationStarted = true;
    // Every entry was validated during the run read; a sanity-check truncation
    // would desync the concatenated positional labels, so require it to keep
    // the full set rather than emit a mis-aligned switch.
    if (!sanityCheckTargets(Img, Targets) ||
        Targets.size() != Info.ExplicitTargets.size() ||
        Targets.size() < limits::kMinJumpTableEntries)
      return {};
    Info.RequiresCompleteCFGProof = RequestedCompleteJumpTableProof;
    if (!reserveResolvedInfoMaterialization(Info) ||
        !consumeCandidateEvidence(orderedLookupWork(ResolvedTableInfo.size()) +
                                  1))
      return {};
    ResolvedTableInfo[Rec.Addr] = Info;
    LLVM_DEBUG(llvm::dbgs()
               << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
               << Targets.size() << " entries ("
               << (Info.TwoLevelIndex ? "two-level index-byte"
                                      : "runtime-selected two-table")
               << ", base=0x" << llvm::utohexstr(Info.BaseAddr) << ")\n");
    CandidateEvidencePublished = true;
    return Targets;
  }

  // Detect normalization (INT_SUB base, right-shift) so we can
  // pull back guard bounds and recover case labels later.  A reloc-absolute
  // computed-goto table is indexed directly by `tab[idx]` with idx in [0,N), so
  // the case values are the raw indices 0..N-1 — there is no case-label
  // normalization to invert.  Crucially, the shift in an index expression like
  // `(acc >> k) & 3` is part of *computing* the index, not a table
  // normalization, so running the detectors here would mis-read it as NormShift
  // and emit bogus `i << k` case values that no longer match the runtime index.
  if (!Info.PreScaledIndex) {
    detectNormalization(Rec, Info);

    // Detect stride from AND masks on the switch variable.  When
    // the index has known-zero low bits the effective table size is
    // guard_bound / stride.
    detectStride(Rec, Info);
  }

  // Refine the entry count through one shared proof path.  A bound is accepted
  // only when the compared value is the exact table-index value at that use,
  // the condition has an unambiguous CFG reaching definition, and the branch
  // polarity on the unique edge to the table LOAD is known.  This replaces the
  // old address-ordered scans and same-slot shortcuts, which could disagree on
  // sibling definitions, memory clobbers, or inclusive polarity.  A composite
  // table may already carry an independently authenticated index domain; a
  // relocation run by itself is only physical storage capacity and never
  // skips this search.
  bool GuardFound = Info.IndexDomainAuthenticated;
  bool AuthenticatedGuardUsesDefinedOccurrenceRoots = false;
  if (!GuardFound) {
    GuardFound = provePreciseGuard(
        Info, /*UseDefinedAlternativesAsRoots=*/false, TargetRoleEdgeOverrides);
    if (GuardFound) {
      Info.IndexDomainAuthenticated = true;
      Info.AuthenticatedGuardBound = Info.MaxEntries;
    }
  }
  // Linked x64 PE RVA switches have no per-entry relocation run: the exact
  // unsigned range guard is their only slot-domain certificate.  Keep this
  // encoding separate from generic compact relative tables, reject mixed
  // relocation storage, and never fall through to readable-prefix inference.
  if (Info.IsPEImageRelativeRVA) {
    if (Img.IsRelocatable || !Img.isCOFF() || Img.Arch != Arch::X64 ||
        Img.getPointerSize() != 8 || !Info.IsRelative || Info.IsSigned ||
        Info.EntrySize != sizeof(uint32_t) || !Info.HasTargetBase ||
        Info.TargetBase != Img.Base || Info.EntryScale != 1 ||
        Info.RelocAbsolute || !GuardFound || !Info.IndexDomainAuthenticated ||
        Info.AuthenticatedGuardBound != Info.MaxEntries ||
        Info.MaxEntries < limits::kMinJumpTableEntries ||
        Info.MaxEntries > limits::kMaxJumpTableEntries)
      return {};
    if (!consumeCandidateFactorProduct({Info.MaxEntries, 25}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries, Img.Segments.size(), 12}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries,
             orderedLookupWork(Img.CodePtrRelocSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries,
             orderedLookupWork(Img.DataPtrRelocSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries,
             orderedLookupWork(Img.RelDataPtrRelocSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries,
             orderedLookupWork(Img.RelCodeRelocSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries,
             orderedLookupWork(Img.ImportStorageSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries, orderedLookupWork(Img.ImportPtrSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries, orderedLookupWork(Img.DyldBindSlots.size())}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries, Img.BaseRelocations.size()}) ||
        !consumeCandidateFactorProduct(
            {Info.MaxEntries, Img.Relocations.size()}))
      return {};
    for (uint32_t Slot = 0; Slot < Info.MaxEntries; ++Slot) {
      if (Slot > (InvalidVA - Info.BaseAddr) / Info.EntrySize)
        return {};
      const va_t SlotVA = Info.BaseAddr + uint64_t{Slot} * Info.EntrySize;
      if (Img.hasRelocationProvenanceAt(SlotVA))
        return {};
    }
  }

  // Record a PIC relative relocation run as physical capacity.  It proves that
  // the occupied slots are relocatable code offsets, but it does not prove the
  // runtime selector domain.  An unguarded `switch(x % N)` is accepted only
  // after the modulo expression below independently authenticates [0, N).
  const bool InspectAbsoluteRun = !Info.TwoTableSelect && !Info.HasTargetBase &&
                                  Info.HasBaseAddr && Info.EntrySize > 0 &&
                                  PhysicalEntryStride >= Info.EntrySize;
  if (DecodedTableAnchors.size() >
      std::numeric_limits<size_t>::max() - Img.RelCodeTableAnchors.size()) {
    consumeCandidateEvidence(std::numeric_limits<size_t>::max());
    return {};
  }
  const size_t MergedAnchorCount =
      Img.RelCodeTableAnchors.size() + DecodedTableAnchors.size();
  if (Img.DataAddressRelocOperands.size() >
      std::numeric_limits<size_t>::max() - MergedAnchorCount) {
    consumeCandidateEvidence(std::numeric_limits<size_t>::max());
    return {};
  }
  const size_t MaxAbsoluteAnchorCount =
      MergedAnchorCount + Img.DataAddressRelocOperands.size();
  const std::optional<uint32_t> RelativeRelocationRun =
      InspectBoundedRelativeRelocationRun();
  if (!RelativeRelocationRun)
    return {};
  if (InspectAbsoluteRun &&
      (!consumeCandidateFactorProduct(
           {size_t(limits::kMaxJumpTableEntries) + 1,
            orderedLookupWork(Img.CodePtrRelocSlots.size())}) ||
       !consumeExactBoundaryInventory(Img, DecodedTableAnchors.size()) ||
       !consumeCandidateProducts(
           {{Img.RelCodeTableAnchors.size(), 3},
            {DecodedTableAnchors.size(),
             orderedLookupWork(MergedAnchorCount) + 2},
            {Img.DataAddressRelocOperands.size(), 2},
            {MaxAbsoluteAnchorCount,
             orderedLookupWork(Img.CodePtrRelocSlots.size())}})))
    return {};
  if (*RelativeRelocationRun >= limits::kMinJumpTableEntries)
    Info.PhysicalCapacity =
        std::max(Info.PhysicalCapacity, *RelativeRelocationRun);

  // A run of absolute code-pointer relocations at the table base proves the
  // entries are absolute code pointers, overriding the backward slice's
  // width-based relative guess.  The slice marks any sub-pointer-width load
  // relative, so an i386 4-byte absolute table (`jmpl *tab(,idx,4)` with
  // R_386_32 entries) would otherwise be decoded as PC-relative offsets and
  // dropped.  This classification is independent of how the table is bounded
  // (a `switch(x & mask)` still has an `and`-derived guard), so it must run
  // regardless of the guard search — decode correctness and entry count are
  // separate concerns.
  bool PhysicalRawAbsCodePtrRunComplete = true;
  const uint32_t PhysicalRawAbsCodePtrRun =
      InspectAbsoluteRun
          ? countCodePtrRelocRun(Img, Info.BaseAddr, PhysicalEntryStride,
                                 &PhysicalRawAbsCodePtrRunComplete)
          : 0;
  const uint32_t RawAbsCodePtrRun =
      Info.RelocAbsolute ? 0 : PhysicalRawAbsCodePtrRun;
  const uint32_t AbsCodePtrRun =
      boundCodePtrRunByNextAnchor(Img, Info.BaseAddr, PhysicalEntryStride,
                                  RawAbsCodePtrRun, DecodedTableAnchors);
  if (AbsCodePtrRun >= limits::kMinJumpTableEntries && Info.IsRelative) {
    Info.IsRelative = false;
    Info.IsSigned = false;
  }
  if (AbsCodePtrRun >= limits::kMinJumpTableEntries)
    Info.PhysicalCapacity = std::max(Info.PhysicalCapacity, AbsCodePtrRun);

  // A physical pointer table whose exact LOAD/address roles include a local
  // basic-block destination is still an indirect dispatch when its selector-
  // domain proof is definitively rejected.  Record that semantic identity
  // independently from proof incompleteness so later tail-call conversion
  // cannot change the stack seen by a local arm.  A complete object boundary
  // is scanned in full, making mixed local/foreign tables independent of entry
  // order.  Without one, only the minimum exact prefix is inspected and it
  // must itself expose a local-frame destination.
  auto ClaimRejectedPhysicalTableIdentity = [&]() -> bool {
    if (CandidateValidatedPhysicalTableIdentity)
      return true;
    if (!consumeCandidateEvidence(
            orderedLookupWork(ValidatedPhysicalJumpTableBranches.size())))
      return false;
    if (ValidatedPhysicalJumpTableBranches.count(Rec.Addr)) {
      CandidateValidatedPhysicalTableIdentity = true;
      return true;
    }
    if (Info.TwoLevelIndex || Info.TwoTableSelect ||
        !Info.ExplicitTargets.empty() || !Info.HasBaseAddr ||
        Info.EntrySize == 0)
      return false;
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize)
      return false;
    // dataObjectSizeAt and the owner query below traverse loader inventories.
    // Charge them before inspection so a boundary cannot be established by
    // unmetered per-candidate symbol/section scans.
    if (!consumeCandidateProducts({{Img.Symbols.size(), 3},
                                   {Img.Segments.size(), 4},
                                   {Img.Sections.size(), 8},
                                   {1, 12}}))
      return false;
    uint32_t ObjectSlots = limits::kMinJumpTableEntries;
    bool HasExactObjectBoundary = false;
    const std::optional<va_t> OwnerEnd =
        Img.mappedObjectOwnerEnd(Info.BaseAddr);
    const uint64_t ObjectSize = Img.dataObjectSizeAt(Info.BaseAddr);
    if (ObjectSize != 0) {
      if (!OwnerEnd || ObjectSize < Info.EntrySize ||
          ObjectSize > InvalidVA - Info.BaseAddr ||
          Info.BaseAddr + ObjectSize > *OwnerEnd ||
          (ObjectSize - Info.EntrySize) % PhysicalStride != 0)
        return false;
      const uint64_t ExactSlots =
          1 + (ObjectSize - Info.EntrySize) / PhysicalStride;
      if (ExactSlots < limits::kMinJumpTableEntries)
        return false;
      if (ExactSlots > limits::kMaxJumpTableEntries) {
        // The object is table-shaped but its complete identity lies beyond the
        // bounded classifier.  Treat that as candidate-local analysis
        // incompleteness; accepting a callable prefix could hide a later
        // local-frame arm and change JMP stack semantics.
        CandidateEvidenceAnalysisIncomplete = true;
        return false;
      }
      ObjectSlots = static_cast<uint32_t>(ExactSlots);
      HasExactObjectBoundary = true;
    }
    // Identity classification must see the complete owner-local relocation
    // run for both absolute pointer tables and PIC-relative offset tables.
    // The bounded proposal count above remains unchanged: this raw run grants
    // neither selector-domain nor publication authority.
    uint32_t RawClassificationSlots =
        std::max(PhysicalRawAbsCodePtrRun, PhysicalRawRelCodeRun);
    bool RawClassificationComplete = true;
    if (PhysicalRawAbsCodePtrRun == RawClassificationSlots &&
        PhysicalRawAbsCodePtrRun != 0)
      RawClassificationComplete &= PhysicalRawAbsCodePtrRunComplete;
    if (PhysicalRawRelCodeRun == RawClassificationSlots &&
        PhysicalRawRelCodeRun != 0)
      RawClassificationComplete &= PhysicalRawRelCodeRunComplete;
    if (OwnerEnd && *OwnerEnd >= Info.BaseAddr &&
        *OwnerEnd - Info.BaseAddr >= Info.EntrySize) {
      const uint64_t OwnerSpan = *OwnerEnd - Info.BaseAddr;
      const uint64_t OwnerSlots =
          1 + (OwnerSpan - Info.EntrySize) / PhysicalStride;
      if (OwnerSlots <= RawClassificationSlots)
        RawClassificationComplete = true;
      RawClassificationSlots = static_cast<uint32_t>(std::min<uint64_t>(
          RawClassificationSlots,
          std::min<uint64_t>(OwnerSlots,
                             std::numeric_limits<uint32_t>::max())));
    }
    const bool HasBoundedPhysicalPrefix =
        Info.PhysicalCapacity >= limits::kMinJumpTableEntries &&
        Info.PhysicalCapacity <= limits::kMaxJumpTableEntries;
    const bool HasRawRelocationPrefix =
        RawClassificationSlots >= limits::kMinJumpTableEntries &&
        RawClassificationSlots <= limits::kMaxJumpTableEntries;
    if (!HasExactObjectBoundary && !HasBoundedPhysicalPrefix &&
        !HasRawRelocationPrefix)
      return false;
    const bool UsedRawRelocationClassification =
        !HasExactObjectBoundary && HasRawRelocationPrefix;
    if (UsedRawRelocationClassification) {
      // An interior-address occurrence may cap proposal exploration before a
      // later relocation slot, but it cannot certify that the preceding two
      // slots form the whole object.  Classify the complete contiguous raw
      // run under the same tri-state policy: any authenticated local-frame arm
      // preserves JMP identity, an all-callable run remains a tail call, and
      // an unknown/budget-exhausted run fails closed.  This scan never grants
      // switch publication or storage-suppression authority.
      ObjectSlots = std::max(ObjectSlots, RawClassificationSlots);
    }
    if (!consumeJumpTableInfoTraversal(Info) ||
        !consumeCandidateProducts({{1, 2}, {ObjectSlots, 2}}))
      return false;

    struct RestoreProofRoots {
      std::optional<std::set<va_t>> &Slot;
      std::optional<std::set<va_t>> Saved;
      ~RestoreProofRoots() { Slot = std::move(Saved); }
    } RestoreRoots{ActiveJumpTableProofRoots,
                   std::move(ActiveJumpTableProofRoots)};

    JumpTableInfo PhysicalProbe = Info;
    PhysicalProbe.MaxEntries = ObjectSlots;
    PhysicalProbe.RuntimeSlotIndices.clear();
    PhysicalProbe.RuntimeCaseLabels.clear();
    PhysicalProbe.AuthenticatedMaskCoordinates.clear();
    PhysicalProbe.AuthenticatedMaskKnownOneWitnesses.clear();
    PhysicalProbe.StorageRanges = {JumpTableStorageRange{
        Info.BaseAddr, Info.EntrySize, PhysicalStride, ObjectSlots}};
    // Identity is not publication authority.  Keep every relocation-derived
    // CFG root active while proving the LOAD/address role and never copy any
    // slot into the suppression allowlist.
    PhysicalProbe.SuppressibleRelocationSlots.clear();

    std::optional<std::set<va_t>> PhysicalRoots =
        budgetedJumpTableProofRoots(PhysicalProbe);
    if (!PhysicalRoots)
      return false;
    ActiveJumpTableProofRoots = std::move(*PhysicalRoots);
    bool PhysicalAddressRoleComplete = false;
    const bool PhysicalAddressRole = tableLoadAddressesMatchRole(
        PhysicalProbe, &CandidateEvidenceBudget, &PhysicalAddressRoleComplete,
        TargetRoleUsesDefinedOccurrenceRoots, TargetRoleEdgeOverrides);
    bool PhysicalTargetRoleComplete = true;
    const bool PhysicalTargetRole =
        PhysicalAddressRole && PhysicalAddressRoleComplete
            ? branchTargetDependsOnTableLoad(
                  Rec, PhysicalProbe, &CandidateEvidenceBudget,
                  &PhysicalTargetRoleComplete,
                  TargetRoleUsesDefinedOccurrenceRoots, TargetRoleEdgeOverrides)
            : false;
    CandidateEvidenceAnalysisIncomplete |=
        !PhysicalAddressRoleComplete ||
        (PhysicalAddressRole && !PhysicalTargetRoleComplete);
    const std::optional<bool> PhysicalIdentity =
        PhysicalAddressRole && PhysicalTargetRole &&
                (!UsedRawRelocationClassification || RawClassificationComplete)
            ? ClassifyPhysicalBranchIdentity(PhysicalProbe)
            : std::nullopt;
    // A complete physical object with an unclassifiable destination is not an
    // authenticated callback table.  Its exact LOAD/address role still proves
    // that this is a table-shaped jump, so preserve it opaquely.  For an
    // open-ended two-slot prefix, require an actually observed local-frame arm
    // before claiming identity; otherwise ordinary callback tail calls remain
    // eligible for CALL+RETURN lowering.
    const bool PreserveOpaqueIdentity =
        PhysicalAddressRole && PhysicalTargetRole &&
        ((PhysicalIdentity && *PhysicalIdentity) ||
         (!PhysicalIdentity &&
          (HasExactObjectBoundary || UsedRawRelocationClassification) &&
          !CandidateEvidenceChargeFailed));
    if (!PreserveOpaqueIdentity ||
        !consumeCandidateEvidence(
            orderedLookupWork(ValidatedPhysicalJumpTableBranches.size()) + 2))
      return false;
    ValidatedPhysicalJumpTableBranches.insert(Rec.Addr);
    CandidateValidatedPhysicalTableIdentity = true;
    return true;
  };
  auto PreservePhysicalDomainMismatch = [&]() -> bool {
    if (CandidateValidatedPhysicalTableIdentity)
      return true;
    if (!consumeCandidateEvidence(
            orderedLookupWork(ValidatedPhysicalJumpTableBranches.size()) + 2))
      return false;
    ValidatedPhysicalJumpTableBranches.insert(Rec.Addr);
    CandidateValidatedPhysicalTableIdentity = true;
    return true;
  };
  // Absolute code-pointer relocations likewise authenticate storage capacity,
  // never a selector domain.  An unguarded `switch(x & mask)` is accepted only
  // when the exact mask occurrence proves its feasible runtime coordinates;
  // sparse masks may own fewer slots than the surrounding relocation run.
  // A `switch(x % N)` table whose entries carry no relocations (AArch64 compact
  // byte/halfword tables, ARM32 inline `.text` word tables) cannot use the
  // relocation run above and has no `cmp` range guard.  Read the modulus N out
  // of the magic-division remainder that computes the index, which bounds the
  // table exactly and keeps the single-target readonly fallback below (which
  // only fires at MaxEntries == 0) from collapsing it to one entry.
  std::optional<std::set<va_t>> CurrentCandidateSelfReplayReachable;
  if (CurrentCandidateSelfReplayTargets) {
    const std::set<va_t> &Roots = ActiveJumpTableProofRoots
                                      ? *ActiveJumpTableProofRoots
                                      : PersistentCFGRoots;
    bool ReplayReachabilityComplete = false;
    CurrentCandidateSelfReplayReachable = candidateReachableInstructions(
        Rec, *CurrentCandidateSelfReplayTargets, Roots, Info.StorageRanges,
        &CandidateEvidenceBudget, &ReplayReachabilityComplete,
        TargetRoleEdgeOverrides);
    if (!ReplayReachabilityComplete) {
      CandidateEvidenceAnalysisIncomplete = true;
      return {};
    }
  }
  bool InitialModuloEvidenceIncomplete = false;
  if (!GuardFound && !Info.IndexDomainAuthenticated && Info.HasBaseAddr &&
      Info.EntrySize > 0) {
    inferBoundsFromModulo(Img, Rec, Info, &CandidateEvidenceBudget,
                          &InitialModuloEvidenceIncomplete,
                          /*RequireProducerReachability=*/
                          CurrentCandidateSelfReplayTargets != nullptr,
                          CurrentCandidateSelfReplayTargets,
                          CurrentCandidateSelfReplayReachable
                              ? &*CurrentCandidateSelfReplayReachable
                              : nullptr,
                          /*AllowFixedPointBootstrap=*/true,
                          /*AuthenticatedProducers=*/nullptr,
                          /*RequiredProducerBound=*/0,
                          /*RequiredProducers=*/nullptr,
                          /*RestrictProducerDiscovery=*/false,
                          TargetRoleEdgeOverrides,
                          /*RetainProvisionalRelativeEdges=*/
                          CandidateProposalStageActive &&
                              ProvisionalRelativeEdgeTemplate.has_value());
  }
  // A candidate-local least-fixed-point round can prove a larger numeric
  // domain before the corresponding destinations exist in this immutable
  // graph.  The resolver records exactly those targets for the outer bounded
  // decoder.  Stop this candidate here: running unrelated mask/domain fallbacks
  // on the deliberately incomplete snapshot would misclassify normal graph
  // growth as shared evidence failure and transactionally discard the queued
  // targets before the outer loop can decode them.
  auto RecordProvisionalRelativeEdges =
      [&](const std::vector<va_t> &Targets,
          const JumpTableStorageRange *CompleteRuntimeStorage = nullptr,
          bool CompleteDenseRuntimeCoordinates = false,
          bool StablePublishedTargetReplay = false) -> bool {
    if (!CandidateProposalStageActive || !ProvisionalRelativeEdgeTemplate ||
        Targets.empty())
      return false;
    // A producer may consume its own complete immutable edge exactly once to
    // replay the now-decoded CFG.  If that replay still cannot publish a table
    // (or a final complete-runtime certificate), do not manufacture a fresh
    // raw token from the token being consumed.  The completed outcome is
    // quarantined transactionally below; resource exhaustion rolls back and
    // retains the prior token for a genuine retry.
    if (CurrentCandidateReplayedOwnProvisionalRelativeEdge &&
        !CompleteRuntimeStorage && !StablePublishedTargetReplay)
      return true;
    if (!consumeCandidateProducts(
            {{ProvisionalRelativeEdgeTemplate->LoadRoles.size(), 3},
             {Targets.size(), 3}}) ||
        !consumeCandidateEvidence(
            orderedLookupWork(NextProvisionalRelativeEdges.size()) + 4))
      return false;
    ProvisionalRelativeEdgeProposal Proposal = *ProvisionalRelativeEdgeTemplate;
    Proposal.Targets = Targets;
    Proposal.CompleteDenseRuntimeCoordinates = CompleteDenseRuntimeCoordinates;
    Proposal.StablePublishedTargetReplay = StablePublishedTargetReplay;
    if (StablePublishedTargetReplay) {
      // The durable sparse replay is only an exact published-edge identity.
      // It must never re-enter the raw physical-coordinate or sibling lanes.
      Proposal.AuthenticatesPhysicalStorage = false;
      Proposal.CompleteDenseRuntimeCoordinates = false;
    }
    if (CompleteRuntimeStorage) {
      // A complete runtime payload is likewise producer/sibling edge evidence,
      // never whole-object ownership.  Retire the one-shot physical token when
      // promoting it to its durable exact runtime form.
      Proposal.AuthenticatesPhysicalStorage = false;
      Proposal.CompleteDenseRuntimeCoordinates = false;
      Proposal.StablePublishedTargetReplay = false;
      Proposal.CompleteRuntimeStorageRange = *CompleteRuntimeStorage;
    }
    NextProvisionalRelativeEdges.insert_or_assign(Rec.Addr,
                                                  std::move(Proposal));
    if (CompleteRuntimeStorage)
      RecordedCompleteRuntimeStorageCertificateForTesting = true;
    if (CompleteRuntimeStorage)
      CandidateCompleteRuntimeCertificateRecorded = true;
    return true;
  };
  auto SuspendForPendingGraphGrowth = [&](bool RetainNoGrowthProposal =
                                              true) -> std::optional<bool> {
    if (!consumeCandidateEvidence(
            orderedLookupWork(CandidateFixedPointExplorationTargets.size())))
      return std::nullopt;
    const auto Exploration =
        CandidateFixedPointExplorationTargets.find(Rec.Addr);
    if (Exploration == CandidateFixedPointExplorationTargets.end())
      return false;
    // Publish only into the next immutable universe.  The producer has already
    // passed ordinary (or lower-rank-overlay) target and address role proofs,
    // the exact LOAD/role bijection, and the domain solver's bounded target
    // read.  Only AuthenticatesPhysicalStorage may later upgrade a certificate
    // into strong ownership; every edge-only proposal remains bootstrap-only
    // and is retired before final stability.
    if (CandidateProposalStageActive && Rec.JumpTableTargets.empty() &&
        ProvisionalRelativeEdgeTemplate &&
        !Exploration->second.Targets.empty()) {
      // A real graph-growth request always retains its authenticated edge.
      // Callers may defer a no-growth proposal while a sibling runtime-range
      // certificate is about to decide an exact singleton; unsuccessful or
      // uncertain queries call again with retention enabled.
      if ((Exploration->second.RequiresGraphGrowth || RetainNoGrowthProposal) &&
          !RecordProvisionalRelativeEdges(
              Exploration->second.Targets,
              /*CompleteRuntimeStorage=*/nullptr,
              Exploration->second.CompleteDenseRuntimeCoordinates))
        return std::nullopt;
    }
    if (!Exploration->second.RequiresGraphGrowth)
      return false;
    // Graph growth is neither a proof loss nor new authority.  If this branch
    // already supplied a strong role proposal in the preceding immutable
    // stage, carry that exact proposal forward while the outer loop decodes
    // the newly authorized targets.  Otherwise reconciliation would quarantine
    // the branch as a definitive loss before the next graph can replay it.
    if (CandidateProposalStageActive) {
      if (!consumeCandidateEvidence(
              orderedLookupWork(PriorStrongJumpTableProposals.size())))
        return {};
      const auto Prior = PriorStrongJumpTableProposals.find(Rec.Addr);
      if (Prior != PriorStrongJumpTableProposals.end()) {
        const StrongJumpTableRoleProposal &Proposal = Prior->second;
        if (!consumeCandidateProducts(
                {{Proposal.StorageRanges.size(), 3},
                 {Proposal.LoadRoles.size(), 3},
                 {Proposal.SuppressibleRelocationSlots.size(), 3}}) ||
            !consumeCandidateEvidence(
                orderedLookupWork(NextStrongJumpTableProposals.size()) + 1) ||
            !consumeCandidateEvidence(
                orderedLookupWork(EverStrongJumpTableProposalBranches.size()) +
                1))
          return {};
        NextStrongJumpTableProposals.insert_or_assign(Rec.Addr, Proposal);
        EverStrongJumpTableProposalBranches.insert(Rec.Addr);
        CandidateStrongProposalRecorded = true;
      }
    }
    return true;
  };
  const std::optional<bool> InitialGraphGrowth = SuspendForPendingGraphGrowth(
      /*RetainNoGrowthProposal=*/!CertifiedSiblingRuntimeStorage);
  if (!InitialGraphGrowth || *InitialGraphGrowth)
    return {};

  // If a normalization offset is present and the guard bound looks
  // like it was applied to the original (pre-normalization) variable,
  // adjust it down to reflect the actual table size.
  if (Info.MaxEntries > 0 && Info.NormBase > 0) {
    uint32_t Adj = pullBackBound(Info.MaxEntries, Info);
    if (Adj != Info.MaxEntries && Adj >= limits::kMinJumpTableEntries) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  pullback: adjusted bound " << Info.MaxEntries << " -> "
                 << Adj << " (normBase=" << Info.NormBase << ")\n");
      Info.MaxEntries = Adj;
    }
  }

  // A power-of-two-modulo / masked index (`and $(2^k-1)`, with an optional
  // following `dec` from a peeled iteration) is hard-bounded by the mask, for
  // every table kind (PIC-relative, GOTOFF, absolute).  Two such tables placed
  // back-to-back in rodata form one continuous relocation run / pointer run, so
  // an over-long read runs past the first table into the second — fabricating
  // bogus successor edges (and, with x87 residents, an unbalanced stack the TOP
  // recovery cannot reconcile).  The mask is a hard upper bound on the index,
  // so clamp to it even when a range guard was found: a guard derived from the
  // pre-`dec` mask (`and $7; dec` => index in [-1,6], 7 entries) over-counts by
  // the offset, and min(guard, mask) is always the safe table size.
  // A two-table merge holds 2N entries while the per-table index mask bounds
  // the index to N; the runtime base select supplies the doubling, so the mask
  // must not clamp the merged count.
  bool IncompleteMaskDomain = false;
  bool SemanticMaskDomainAmbiguous = false;
  bool UsedNonContiguousMask = false;
  std::optional<va_t> ExactFiniteRelativeSingletonTargetValue;
  bool ExactFiniteRelativeClosureUnknownValue = false;
  std::vector<uint32_t> MaskCoordinates;
  std::vector<JumpTableMaskKnownOneWitness> MaskKnownOneWitnesses;
  const bool HasExactFiniteSingletonStorage =
      ProvisionalRelativeEdgeTemplate &&
      ((ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage &&
        Info.ExactBoundedRelativeRelocationSlots >=
            limits::kMinJumpTableEntries) ||
       (CertifiedSiblingRuntimeStorage &&
        CertifiedSiblingRuntimeStorage->PhysicalSlotCount >=
            limits::kMinJumpTableEntries));
  std::optional<va_t> *ExactFiniteRelativeSingletonTarget =
      CandidateProposalStageActive && HasExactFiniteSingletonStorage &&
              !TargetRoleEdgeOverrides
          ? &ExactFiniteRelativeSingletonTargetValue
          : nullptr;
  bool *ExactFiniteRelativeClosureUnknown =
      CandidateProposalStageActive && ProvisionalRelativeEdgeTemplate
          ? &ExactFiniteRelativeClosureUnknownValue
          : nullptr;
  const uint32_t MaskBound = inferBoundsFromMask(
      Rec, Info, /*AllowNonContiguous=*/true, &IncompleteMaskDomain,
      &UsedNonContiguousMask, &MaskCoordinates, &MaskKnownOneWitnesses,
      /*RequireProducerReachability=*/
      CurrentCandidateSelfReplayTargets != nullptr,
      CurrentCandidateSelfReplayTargets,
      CurrentCandidateSelfReplayReachable
          ? &*CurrentCandidateSelfReplayReachable
          : nullptr,
      /*AllowFixedPointBootstrap=*/true,
      /*AllowRawDenseShortcut=*/true, &CandidateEvidenceBudget,
      &SemanticMaskDomainAmbiguous, &ExactConsumerGroup,
      TargetRoleEdgeOverrides, CertifiedSiblingRuntimeStorage,
      ExactFiniteRelativeSingletonTarget, ExactFiniteRelativeClosureUnknown,
      /*RetainProvisionalRelativeEdges=*/
      CandidateProposalStageActive &&
          ProvisionalRelativeEdgeTemplate.has_value());
  const std::optional<bool> MaskGraphGrowth = SuspendForPendingGraphGrowth(
      /*RetainNoGrowthProposal=*/
      !ExactFiniteRelativeSingletonTargetValue.has_value());
  if (!MaskGraphGrowth || *MaskGraphGrowth)
    return {};
  auto RecordAuthenticatedRelativeClosureProposal = [&]() {
    if (!CandidateProposalStageActive)
      return true;
    if (!ProvisionalRelativeEdgeTemplate)
      return false;
    // Edge-only certificates deliberately have no strong ownership to carry.
    // Treat that as a successful no-op, not local proof loss.  Conversely, an
    // exact physical certificate may carry its independently authenticated
    // storage role across a lower-rank edge overlay; the overlay still grants
    // no singleton or stable-publication authority.
    if (!ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage)
      return true;
    if (!consumeCandidateProducts({{1, 3}, {PrePruningLoadRoles.size(), 3}}) ||
        !consumeCandidateEvidence(
            orderedLookupWork(NextStrongJumpTableProposals.size()) + 1) ||
        !consumeCandidateEvidence(
            orderedLookupWork(EverStrongJumpTableProposalBranches.size()) + 1))
      return false;
    StrongJumpTableRoleProposal Proposal;
    Proposal.StorageRanges = {ProvisionalRelativeEdgeTemplate->Storage};
    Proposal.ExactPhysicalStorageRange =
        ProvisionalRelativeEdgeTemplate->Storage;
    Proposal.LoadRoles = PrePruningLoadRoles;
    Proposal.ProofRank = ActiveJumpTableCandidateProofRank;
    NextStrongJumpTableProposals.insert_or_assign(Rec.Addr,
                                                  std::move(Proposal));
    EverStrongJumpTableProposalBranches.insert(Rec.Addr);
    CandidateStrongProposalRecorded = true;
    return true;
  };
  if (ExactFiniteRelativeClosureUnknownValue) {
    // The exact edge is authenticated and has already been copied into the
    // next immutable provisional universe, but an unmodeled reachable exit
    // makes singleton publication unsound.  Preserve strong occurrence state
    // only when a complete physical object independently authenticated it;
    // edge-only state is a successful no-op.  Never turn this semantic
    // uncertainty into resource incompleteness or a table.
    if (!RecordAuthenticatedRelativeClosureProposal())
      return {};
    return {};
  }
  if (ExactFiniteRelativeSingletonTargetValue) {
    if (!ProvisionalRelativeEdgeTemplate || TargetRoleEdgeOverrides ||
        (!ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage &&
         !CertifiedSiblingRuntimeStorage) ||
        MaskBound == 0 || MaskCoordinates.size() != 1 || Info.EntrySize == 0 ||
        PhysicalEntryStride < Info.EntrySize)
      return {};
    const uint64_t SingletonStorageCapacity =
        ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage
            ? Info.ExactBoundedRelativeRelocationSlots
            : CertifiedSiblingRuntimeStorage->PhysicalSlotCount;
    const uint32_t Coordinate = MaskCoordinates.front();
    if (Coordinate >= SingletonStorageCapacity ||
        (Coordinate != 0 &&
         PhysicalEntryStride > (InvalidVA - Info.BaseAddr) / Coordinate))
      return {};
    const va_t SlotAddress =
        Info.BaseAddr + uint64_t{Coordinate} * PhysicalEntryStride;
    if (Info.EntrySize - 1 > InvalidVA - SlotAddress ||
        !consumeCandidateProducts({{1, 16}}) ||
        !reservePublishedTargetCleanup(1))
      return {};

    // The runtime domain below shrinks to one exact coordinate.  Preserve
    // strong physical identity only for a template that independently owns the
    // whole object; an edge-only sibling runtime certificate remains a no-op
    // here and grants neither ownership nor relocation suppression.
    if (!RecordAuthenticatedRelativeClosureProposal())
      return {};

    // The finite-value solver has replayed this exact selector occurrence in
    // the graph containing its only authorized target.  Retain that singleton
    // runtime coordinate, never the surrounding physical relocation capacity.
    Info.IndexDomainAuthenticated = true;
    Info.MaxEntries = MaskBound;
    Info.RuntimeCaseLabels = {Coordinate};
    Info.RuntimeSlotIndices = {Coordinate};
    Info.AuthenticatedMaskCoordinates.clear();
    Info.AuthenticatedMaskKnownOneWitnesses.clear();
    Info.StorageRanges = {
        JumpTableStorageRange{SlotAddress, Info.EntrySize, Info.EntrySize, 1}};
    Info.SuppressibleRelocationSlots.clear();
    // Keep the exact occurrence-backed selector plan.  This singleton is
    // solver-proven at the indexed LOAD use; it is not the shape-layer literal
    // address form that requires shared/constant-target fallback tracing.
    Info.UseSharedDispatchSelector = false;
    Info.RequiresCompleteCFGProof = true;

    if (!ClaimValidatedPotentialTable())
      return {};

    // inferBoundsFromMask already paid for and validated the whole physical
    // target vector, then replayed this one authorized edge in its reachable
    // graph.  Reuse that exact result instead of rescanning attacker-sized
    // loader inventories or depending on a next-stage provisional map that is
    // intentionally absent during the final stable replay.
    if (!consumeCandidateProducts({{1, 3}}))
      return {};
    std::vector<va_t> SingletonTargets{
        *ExactFiniteRelativeSingletonTargetValue};
    CandidateTargetMaterializationStarted = true;
    if (!reserveResolvedInfoMaterialization(Info) ||
        !consumeCandidateEvidence(orderedLookupWork(ResolvedTableInfo.size()) +
                                  1))
      return {};
    ResolvedTableInfo[Rec.Addr] = Info;
    CandidateEvidencePublished = true;
    return SingletonTargets;
  }
  if (!Info.TwoTableSelect && !Info.TwoLevelIndex && MaskBound > 0) {
    if (!consumeCandidateProducts(
            {{Info.LoadRoles.size(), MaskCoordinates.size() + 2}}))
      return {};
    for (const JumpTableLoadRole &Role : Info.LoadRoles)
      if (Role.IsLiteralCoordinate &&
          std::find(MaskCoordinates.begin(), MaskCoordinates.end(),
                    Role.LiteralCoordinate) == MaskCoordinates.end()) {
        ClaimRejectedPhysicalTableIdentity();
        return {};
      }

    // Reserve pruning, both coordinate copies, vector lifetimes, and every
    // coordinate-to-physical-slot field check before any mask-domain vector
    // is mutated or materialized.
    if (!consumeCandidateProducts({{1, 4}, {MaskCoordinates.size(), 16}}))
      return {};
    Info.AuthenticatedMaskCoordinates = MaskCoordinates;
    Info.AuthenticatedMaskKnownOneWitnesses = std::move(MaskKnownOneWitnesses);
    // The next-anchor cap is a runtime-domain heuristic: another reachable
    // consumer may name an interior relocation without ending the physical
    // table object.  For storage ownership, use the uncapped relocation run
    // only when its end is independently equal to the mapped object/section
    // boundary.  This keeps exact physical capacity separate from both the
    // mask domain and consumer-specific suppression permissions.
    const uint32_t ExactAbsoluteStorageSlots = [&] {
      if (Info.EntrySize == 0 || PhysicalEntryStride < Info.EntrySize ||
          RawAbsCodePtrRun == 0)
        return uint32_t{0};
      if (!consumeCandidateProducts(
              {{Img.Segments.size(), 4}, {Img.Sections.size(), 8}, {1, 10}}))
        return uint32_t{0};
      if (const std::optional<va_t> OwnerEnd =
              Img.mappedObjectOwnerEnd(Info.BaseAddr);
          OwnerEnd && *OwnerEnd >= Info.BaseAddr + Info.EntrySize) {
        const uint64_t Span = *OwnerEnd - Info.BaseAddr;
        if ((Span - Info.EntrySize) % PhysicalEntryStride == 0) {
          const uint64_t Slots =
              (Span - Info.EntrySize) / PhysicalEntryStride + 1;
          if (Slots <= RawAbsCodePtrRun &&
              Slots <= std::numeric_limits<uint32_t>::max())
            return static_cast<uint32_t>(Slots);
        }
      }
      if (!consumeExactBoundaryInventory(Img, DecodedTableAnchors.size()))
        return uint32_t{0};
      return codePtrRelocRunHasExactBoundary(
                 Img, Info.BaseAddr, PhysicalEntryStride, RawAbsCodePtrRun,
                 DecodedTableAnchors)
                 ? RawAbsCodePtrRun
                 : uint32_t{0};
    }();
    const uint32_t AuthenticatedStorageSlots =
        std::max(Info.PhysicalCapacity, ExactAbsoluteStorageSlots);
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize || Info.EntrySize == 0 ||
        MaskCoordinates.empty())
      return {};

    std::optional<uint64_t> AddressScale;
    if (!consumeCandidateProducts({{Info.LoadRoles.size(), 3}}))
      return {};
    for (const JumpTableLoadRole &Role : Info.LoadRoles) {
      if (Role.IsLiteralCoordinate)
        continue;
      if (Role.LoadWidth != Info.EntrySize || Role.AddressScale == 0)
        continue;
      if (!consumeCandidateEvidence(Role.AllowedBases.size()))
        return {};
      if (std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                    Info.BaseAddr) == Role.AllowedBases.end())
        continue;
      if (AddressScale && *AddressScale != Role.AddressScale)
        return {};
      AddressScale = Role.AddressScale;
    }
    if (!AddressScale)
      return {};

    // A guard and a mask constrain the same runtime selector domain.  Keep
    // their intersection before converting coordinates to physical slots;
    // MaxEntries from a relocation run is instead storage capacity and is
    // checked only after the conversion.  Pre-scaled indices carry byte
    // coordinates while a comparison commonly bounds logical entries.
    if (GuardFound && Info.MaxEntries > 0) {
      uint64_t GuardCoordinateLimit = Info.MaxEntries;
      if (Info.PreScaledIndex && Info.Stride > 1) {
        if (GuardCoordinateLimit >
            std::numeric_limits<uint64_t>::max() / Info.Stride)
          return {};
        GuardCoordinateLimit *= Info.Stride;
      }
      MaskCoordinates.erase(
          std::remove_if(MaskCoordinates.begin(), MaskCoordinates.end(),
                         [&](uint32_t Coordinate) {
                           return Coordinate >= GuardCoordinateLimit;
                         }),
          MaskCoordinates.end());
      if (MaskCoordinates.size() < limits::kMinJumpTableEntries)
        return {};
    }

    std::vector<uint32_t> PhysicalSlots;
    std::vector<JumpTableStorageRange> ExactStorage;
    PhysicalSlots.reserve(MaskCoordinates.size());
    ExactStorage.reserve(MaskCoordinates.size());
    for (uint32_t Coordinate : MaskCoordinates) {
      if (Coordinate != 0 &&
          *AddressScale > std::numeric_limits<uint64_t>::max() / Coordinate)
        return {};
      const uint64_t ByteOffset = uint64_t(Coordinate) * *AddressScale;
      if (ByteOffset % PhysicalStride != 0)
        return {};
      const uint64_t Slot = ByteOffset / PhysicalStride;
      if (Slot > std::numeric_limits<uint32_t>::max())
        return {};
      if (!PhysicalSlots.empty() && Slot <= PhysicalSlots.back())
        return {};
      if (ByteOffset > InvalidVA - Info.BaseAddr ||
          Info.EntrySize - 1 > InvalidVA - (Info.BaseAddr + ByteOffset))
        return {};
      PhysicalSlots.push_back(static_cast<uint32_t>(Slot));
      ExactStorage.push_back(JumpTableStorageRange{
          Info.BaseAddr + ByteOffset, Info.EntrySize, Info.EntrySize, 1});
    }

    const uint32_t PhysicalSpan = PhysicalSlots.back() + 1;
    // A relocation run is authenticated storage capacity, not an index-domain
    // proof.  Every feasible coordinate must map inside it; taking min would
    // silently discard live selector values.
    if (Info.PhysicalCapacity > 0 && PhysicalSpan > Info.PhysicalCapacity) {
      PreservePhysicalDomainMismatch();
      return {};
    }
    Info.RuntimeCaseLabels = MaskCoordinates;

    // Runtime-domain slots and physical object ownership are different facts.
    // An exact owner/anchor boundary may prove that compiler filler belongs to
    // the same object, but it does not authorize suppressing a filler
    // relocation that another reachable instruction consumes.  That separate
    // permission is computed after the final target graph is known.
    bool OwnsCompletePhysicalObject = false;
    if (AuthenticatedStorageSlots == PhysicalSpan) {
      if (!consumeExactBoundaryInventory(Img, DecodedTableAnchors.size()))
        return {};
      OwnsCompletePhysicalObject =
          codePtrRelocRunHasExactBoundary(Img, Info.BaseAddr, PhysicalStride,
                                          PhysicalSpan, DecodedTableAnchors);
    }
    Info.RuntimeSlotIndices = std::move(PhysicalSlots);
    if (OwnsCompletePhysicalObject) {
      Info.StorageRanges = {JumpTableStorageRange{
          Info.BaseAddr, Info.EntrySize, PhysicalStride, PhysicalSpan}};
    } else {
      Info.StorageRanges = std::move(ExactStorage);
    }
    const size_t ProtectedLookupWork =
        ProtectedJumpTableRelocationSlots
            ? orderedLookupWork(ProtectedJumpTableRelocationSlots->size())
            : 0;
    const std::optional<size_t> SuppressionWork =
        detail::maskSuppressionSlotMaterializationWork(
            Info.RuntimeSlotIndices.size(),
            orderedLookupWork(Img.CodePtrRelocSlots.size()),
            ProtectedLookupWork);
    if (!SuppressionWork || !consumeCandidateEvidence(*SuppressionWork))
      return {};
    Info.SuppressibleRelocationSlots.clear();
    for (uint32_t Slot : Info.RuntimeSlotIndices) {
      if (Slot != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / Slot)
        return {};
      const va_t SlotVA = Info.BaseAddr + uint64_t(Slot) * PhysicalStride;
      if (Img.CodePtrRelocSlots.count(SlotVA))
        Info.SuppressibleRelocationSlots.push_back(SlotVA);
    }
    if (ProtectedJumpTableRelocationSlots)
      Info.SuppressibleRelocationSlots.erase(
          std::remove_if(Info.SuppressibleRelocationSlots.begin(),
                         Info.SuppressibleRelocationSlots.end(),
                         [&](va_t Slot) {
                           return ProtectedJumpTableRelocationSlots->count(
                               Slot);
                         }),
          Info.SuppressibleRelocationSlots.end());
    Info.MaxEntries = PhysicalSpan;
    Info.IndexDomainAuthenticated = true;
    LLVM_DEBUG(llvm::dbgs()
               << "  mask-domain: authenticated "
               << Info.RuntimeCaseLabels.size() << " coordinates over "
               << PhysicalSpan << " physical slots\n");
  } else if (MaskBound == 0 && Info.MaxEntries > 0 && Info.Stride > 1 &&
             !Info.RelocBounded) {
    // Legacy guard-only recovery has no exact coordinate set.  Apply its
    // historical stride adjustment only after exact mask recovery had a chance
    // to establish the real runtime coordinate; doing it earlier divides an
    // equal guard/mask bound twice.
    uint32_t Adj = Info.MaxEntries / Info.Stride;
    if (Adj >= limits::kMinJumpTableEntries)
      Info.MaxEntries = Adj;
  }
  // The first guard pass resolves complete values so that an incomplete
  // candidate graph still drives the outer fixed point.  Once independent
  // modulo and mask proofs have both failed to authenticate this occurrence,
  // retry the same guard syntax with its already authenticated DefinedAtPoint
  // alternatives as occurrence-local SSA roots.  This bounds cyclic state
  // relays without letting an early semantic rejection suppress sibling table
  // discovery; the retry still shares the candidate aggregate account and the
  // separate address-role certificate remains mandatory.
  bool PriorOccurrenceCertificateMatches = false;
  bool UsedPriorOccurrenceCertificateForGuard = false;
  if (CandidateProposalStageActive && Info.IncompleteGuardDomain) {
    if (!consumeCandidateEvidence(
            orderedLookupWork(PriorStrongJumpTableProposals.size())))
      return {};
    const auto Prior = PriorStrongJumpTableProposals.find(Rec.Addr);
    if (Prior != PriorStrongJumpTableProposals.end()) {
      const StrongJumpTableRoleProposal &Proposal = Prior->second;
      const std::optional<size_t> ComparisonWork =
          detail::priorOccurrenceCertificateComparisonWork(
              Proposal.StorageRanges.size(), Proposal.LoadRoles.size());
      if (!consumeCandidateEvidence(
              ComparisonWork.value_or(std::numeric_limits<size_t>::max())))
        return {};
      const uint64_t PhysicalStride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      bool StorageMatches = Info.HasBaseAddr && Info.EntrySize != 0 &&
                            PhysicalStride >= Info.EntrySize &&
                            Info.PhysicalCapacity != 0;
      if (StorageMatches && Proposal.ExactPhysicalStorageRange) {
        StorageMatches = sameStorageRangeNoShortCircuit(
            *Proposal.ExactPhysicalStorageRange,
            JumpTableStorageRange{Info.BaseAddr, Info.EntrySize, PhysicalStride,
                                  Info.PhysicalCapacity});
      } else if (StorageMatches &&
                 Proposal.StorageRanges.size() == Info.PhysicalCapacity) {
        for (size_t I = 0; I < Proposal.StorageRanges.size(); ++I) {
          if (I != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / I) {
            StorageMatches = false;
            break;
          }
          const JumpTableStorageRange Expected{
              Info.BaseAddr + uint64_t(I) * PhysicalStride, Info.EntrySize,
              Info.EntrySize, 1};
          if (!sameStorageRangeNoShortCircuit(Proposal.StorageRanges[I],
                                              Expected)) {
            StorageMatches = false;
            break;
          }
        }
      } else {
        StorageMatches = false;
      }
      const bool LoadRolesMatch = sameLoadRoleVectorsNoShortCircuit(
          Proposal.LoadRoles, PrePruningLoadRoles);
      PriorOccurrenceCertificateMatches = StorageMatches;
      PriorOccurrenceCertificateMatches &= LoadRolesMatch;
    }
  }
  if ((HasExactRootedTargetRoleCertificate ||
       PriorOccurrenceCertificateMatches) &&
      Info.IncompleteGuardDomain && !Info.IndexDomainAuthenticated &&
      Info.AuthenticatedModuloBound == 0 &&
      Info.AuthenticatedMaskCoordinates.empty()) {
    const bool RootedGuard = provePreciseGuard(
        Info, /*UseDefinedAlternativesAsRoots=*/true, TargetRoleEdgeOverrides);
    if (RootedGuard) {
      Info.IndexDomainAuthenticated = true;
      Info.AuthenticatedGuardBound = Info.MaxEntries;
      AuthenticatedGuardUsesDefinedOccurrenceRoots = true;
      UsedPriorOccurrenceCertificateForGuard =
          !HasExactRootedTargetRoleCertificate &&
          PriorOccurrenceCertificateMatches;
    }
  }
  // An unmodelled mask-dependent transform cannot be rescued by readable
  // relocation capacity.  It also must not erase an independent complete
  // proof over this exact final index occurrence: a full-domain guard or the
  // LLVM constant-division remainder theorem already proves every runtime
  // value is inside the published domain, irrespective of incidental ANDs in
  // the quotient/flag calculation.  Final-root revalidation below replays that
  // independent witness after ownership has been narrowed.
  if ((IncompleteMaskDomain || SemanticMaskDomainAmbiguous) &&
      Info.AuthenticatedGuardBound == 0 && Info.AuthenticatedModuloBound == 0) {
    CandidateEvidenceAnalysisIncomplete |= IncompleteMaskDomain;
    const bool ClaimedPhysicalIdentity = ClaimRejectedPhysicalTableIdentity();
    if (IncompleteMaskDomain && ClaimedPhysicalIdentity)
      insertIncompleteBranchOnce();
    return {};
  }
  if (InitialModuloEvidenceIncomplete && !Info.IndexDomainAuthenticated &&
      Info.AuthenticatedGuardBound == 0 &&
      Info.AuthenticatedMaskCoordinates.empty())
    CandidateEvidenceAnalysisIncomplete = true;
  // A sampled/non-prefix guard must not be rescued by a relocation run: the
  // run proves readable table storage, not which selector values can reach
  // it.  An independently authenticated exact mask domain is sufficient
  // because it bounds the actual address coordinate regardless of the guard;
  // the legacy modulo recognizer is intentionally not a rescue here until it
  // is occurrence/CFG authenticated in the same way.
  if ((Info.IncompleteGuardDomain || Info.SemanticGuardDomainAmbiguous) &&
      !Info.IndexDomainAuthenticated) {
    CandidateEvidenceAnalysisIncomplete |= Info.IncompleteGuardDomain;
    ClaimRejectedPhysicalTableIdentity();
    return {};
  }

  if (Info.MaxEntries == 0 || Info.MaxEntries > limits::kMaxJumpTableEntries)
    Info.MaxEntries = 0;
  if (!JumpTableProofContextComplete && RequestedCompleteJumpTableProof)
    return {};
  if (Info.PreScaledIndex && Info.RuntimeSlotIndices.empty())
    return {};
  // Static bytes and relocation runs establish only physical capacity.  Every
  // multi-target publication must also carry a complete proof of the exact
  // runtime selector domain.  In particular, neither the legacy unbounded
  // reader nor self-bounding emulation may turn a readable prefix into an
  // index bound.  A single immutable pointer is a separate direct-branch
  // problem and is intentionally not published as a jump table here.
  if (!Info.IndexDomainAuthenticated ||
      Info.MaxEntries < limits::kMinJumpTableEntries) {
    ClaimRejectedPhysicalTableIdentity();
    return {};
  }

  // Capacity constrains an authenticated domain; it never supplies one.  A
  // domain that exceeds known storage cannot be repaired by taking min(),
  // because that would silently drop feasible selector values.
  if (Info.PhysicalCapacity != 0 && Info.MaxEntries > Info.PhysicalCapacity) {
    // Exact selector evidence proves feasible reads beyond the authenticated
    // storage span.  Even an all-callable known prefix cannot justify CALL
    // lowering for the unknown/faulting suffix; preserve this branch opaquely.
    PreservePhysicalDomainMismatch();
    return {};
  }

  // Mask recovery already materializes its exact (possibly sparse or
  // pre-scaled) coordinate-to-slot map.  Guard and modulo domains are dense:
  // publish only slots [0,N), not the entire relocation capacity that was used
  // to bootstrap the first occurrence proof.  This keeps adjacent table slots
  // out of final ownership and suppression, then revalidates both certificates
  // on the refined root set below.
  if (Info.RuntimeSlotIndices.empty()) {
    if (!Info.HasBaseAddr || Info.EntrySize == 0)
      return {};
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize)
      return {};

    const size_t ProtectedLookupWork =
        ProtectedJumpTableRelocationSlots
            ? orderedLookupWork(ProtectedJumpTableRelocationSlots->size())
            : 0;
    const std::optional<size_t> SuppressionWork =
        detail::denseSuppressionSlotMaterializationWork(
            Info.MaxEntries, Img.Segments.size(), Img.Sections.size(),
            orderedLookupWork(Img.CodePtrRelocSlots.size()),
            ProtectedLookupWork);
    if (!SuppressionWork || !consumeCandidateEvidence(*SuppressionWork))
      return {};

    std::vector<JumpTableStorageRange> ExactStorage;
    std::vector<va_t> ExactSuppressibleSlots;
    ExactStorage.reserve(Info.MaxEntries);
    ExactSuppressibleSlots.reserve(Info.MaxEntries);
    for (uint32_t Slot = 0; Slot < Info.MaxEntries; ++Slot) {
      if (Slot != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / Slot)
        return {};
      const va_t SlotVA = Info.BaseAddr + uint64_t(Slot) * PhysicalStride;
      if (Info.EntrySize - 1 > InvalidVA - SlotVA ||
          !Img.readVA(SlotVA, Info.EntrySize))
        return {};
      const std::optional<va_t> OwnerEnd = Img.mappedObjectOwnerEnd(SlotVA);
      if (!OwnerEnd || SlotVA >= *OwnerEnd ||
          Info.EntrySize > *OwnerEnd - SlotVA)
        return {};
      ExactStorage.push_back(
          JumpTableStorageRange{SlotVA, Info.EntrySize, Info.EntrySize, 1});
      if (Img.CodePtrRelocSlots.count(SlotVA))
        ExactSuppressibleSlots.push_back(SlotVA);
    }
    Info.StorageRanges = std::move(ExactStorage);
    Info.SuppressibleRelocationSlots = std::move(ExactSuppressibleSlots);
    if (ProtectedJumpTableRelocationSlots)
      Info.SuppressibleRelocationSlots.erase(
          std::remove_if(Info.SuppressibleRelocationSlots.begin(),
                         Info.SuppressibleRelocationSlots.end(),
                         [&](va_t Slot) {
                           return ProtectedJumpTableRelocationSlots->count(
                               Slot);
                         }),
          Info.SuppressibleRelocationSlots.end());
  }

  // The first occurrence proof may need a relocation-backed candidate root
  // set before the exact runtime domain is known.  Require both certificates
  // again after final slot ownership/suppression has been materialized.  The
  // index-domain proof itself must also be replayed: a relocation just beyond
  // the final domain may restore a predecessor that bypasses the guard while
  // leaving both LOAD-role certificates valid.
  {
    std::optional<std::set<va_t>> Roots = budgetedJumpTableProofRoots(Info);
    if (!Roots)
      return {};
    ActiveJumpTableProofRoots = std::move(*Roots);
  }
  auto RevalidateIndexDomain = [&]() -> bool {
    bool Revalidated = false;
    if (Info.AuthenticatedGuardBound != 0) {
      if (!consumeJumpTableInfoTraversal(Info))
        return false;
      JumpTableInfo Check = Info;
      Check.MaxEntries = 0;
      Check.IndexDomainAuthenticated = false;
      Check.IncompleteGuardDomain = false;
      Check.SemanticGuardDomainAmbiguous = false;
      const bool GuardRevalidated =
          provePreciseGuard(Check, AuthenticatedGuardUsesDefinedOccurrenceRoots,
                            TargetRoleEdgeOverrides);
      CandidateEvidenceAnalysisIncomplete |= Check.IncompleteGuardDomain;
      if (!GuardRevalidated || Check.MaxEntries != Info.AuthenticatedGuardBound)
        return false;
      Revalidated = true;
    }
    if (Info.AuthenticatedModuloBound != 0) {
      if (!consumeJumpTableInfoTraversal(Info))
        return false;
      JumpTableInfo Check = Info;
      Check.MaxEntries = 0;
      Check.IndexDomainAuthenticated = false;
      Check.AuthenticatedModuloBound = 0;
      bool ModuloEvidenceIncomplete = false;
      const bool ModuloRevalidated = inferBoundsFromModulo(
          Img, Rec, Check, &CandidateEvidenceBudget, &ModuloEvidenceIncomplete,
          /*RequireProducerReachability=*/
          CurrentCandidateSelfReplayTargets != nullptr,
          CurrentCandidateSelfReplayTargets,
          CurrentCandidateSelfReplayReachable
              ? &*CurrentCandidateSelfReplayReachable
              : nullptr,
          /*AllowFixedPointBootstrap=*/true,
          /*AuthenticatedProducers=*/nullptr,
          /*RequiredProducerBound=*/0,
          /*RequiredProducers=*/nullptr,
          /*RestrictProducerDiscovery=*/false, TargetRoleEdgeOverrides,
          /*RetainProvisionalRelativeEdges=*/false);
      CandidateEvidenceAnalysisIncomplete |= ModuloEvidenceIncomplete;
      if (!ModuloRevalidated ||
          Check.MaxEntries != Info.AuthenticatedModuloBound)
        return false;
      Revalidated = true;
    }
    if (!Info.AuthenticatedMaskCoordinates.empty()) {
      bool Incomplete = false;
      bool SemanticAmbiguous = false;
      bool NonContiguous = false;
      std::vector<uint32_t> Coordinates;
      std::vector<JumpTableMaskKnownOneWitness> KnownOneWitnesses;
      const uint32_t Bound = inferBoundsFromMask(
          Rec, Info, /*AllowNonContiguous=*/true, &Incomplete, &NonContiguous,
          &Coordinates, &KnownOneWitnesses,
          // A cyclic computed-goto first exposes only its
          // constant entry arm.  Once provisional table
          // edges exist, require the replay to reach the
          // exact mask producer before final publication.
          /*RequireProducerReachability=*/
          !Rec.JumpTableTargets.empty() ||
              CurrentCandidateSelfReplayTargets != nullptr,
          CurrentCandidateSelfReplayTargets,
          CurrentCandidateSelfReplayReachable
              ? &*CurrentCandidateSelfReplayReachable
              : nullptr,
          /*AllowFixedPointBootstrap=*/true,
          /*AllowRawDenseShortcut=*/true, &CandidateEvidenceBudget,
          &SemanticAmbiguous, &ExactConsumerGroup, TargetRoleEdgeOverrides,
          CertifiedSiblingRuntimeStorage,
          /*ExactFiniteRelativeSingletonTarget=*/nullptr,
          /*ExactFiniteRelativeClosureUnknown=*/nullptr,
          /*RetainProvisionalRelativeEdges=*/false,
          CurrentCandidateHasPriorProvisionalRelativeEdge);
      const std::optional<size_t> MaskComparisonWork =
          detail::maskDomainComparisonWork(
              Coordinates.size(), Info.AuthenticatedMaskCoordinates.size(),
              KnownOneWitnesses.size(),
              Info.AuthenticatedMaskKnownOneWitnesses.size());
      if (!MaskComparisonWork || !consumeCandidateEvidence(*MaskComparisonWork))
        return false;
      bool SameMaskDomain =
          Coordinates.size() == Info.AuthenticatedMaskCoordinates.size();
      for (size_t I = 0; I < std::min(Coordinates.size(),
                                      Info.AuthenticatedMaskCoordinates.size());
           ++I)
        SameMaskDomain &=
            Coordinates[I] == Info.AuthenticatedMaskCoordinates[I];
      SameMaskDomain &= KnownOneWitnesses.size() ==
                        Info.AuthenticatedMaskKnownOneWitnesses.size();
      for (size_t I = 0;
           I < std::min(KnownOneWitnesses.size(),
                        Info.AuthenticatedMaskKnownOneWitnesses.size());
           ++I)
        SameMaskDomain &= sameMaskWitnessNoShortCircuit(
            KnownOneWitnesses[I], Info.AuthenticatedMaskKnownOneWitnesses[I]);
      if (Incomplete || SemanticAmbiguous || Bound == 0 || !SameMaskDomain) {
        CandidateEvidenceAnalysisIncomplete |= Incomplete;
        return false;
      }
      Revalidated = true;
    }
    // Composite strategies have exact storage from the outset and return via
    // ExplicitTargets above; every ordinary table must retain at least one
    // replayable full-domain witness.
    return Revalidated;
  };
  bool PreReadAddressRoleComplete = false;
  const bool PreReadAddressRole = tableLoadAddressesMatchRole(
      Info, &CandidateEvidenceBudget, &PreReadAddressRoleComplete,
      TargetRoleUsesDefinedOccurrenceRoots, TargetRoleEdgeOverrides);
  CandidateEvidenceAnalysisIncomplete |= !PreReadAddressRoleComplete;
  if (!PreReadAddressRole) {
    ClaimRejectedPhysicalTableIdentity();
    return {};
  }
  // Address-role replay publishes its pruned occurrence/index inventory
  // transactionally.  Revalidate the selector domain and target relation only
  // after that mutation, so neither certificate can be reused for stale inputs.
  const bool DomainRevalidated = RevalidateIndexDomain();
  const std::optional<bool> RevalidationGraphGrowth =
      SuspendForPendingGraphGrowth();
  if (!RevalidationGraphGrowth || *RevalidationGraphGrowth)
    return {};
  if (!DomainRevalidated) {
    ClaimRejectedPhysicalTableIdentity();
    return {};
  }
  const bool PreReadTargetRoleCurrent = EnsureTargetRoleProofCurrent();
  if (!PreReadTargetRoleCurrent) {
    ClaimRejectedPhysicalTableIdentity();
    return {};
  }
  // Runtime storage ranges describe only the coordinates used by this
  // dispatch.  Preserve a separate whole-object identity when a relocation
  // run has an independently exact end, so peeled/sparse and full dispatches
  // over one physical table can authenticate each other's exact LOAD
  // occurrence without equating their selector domains.  A prefix-only run
  // deliberately leaves the certificate absent.  This identity is not a
  // suppression permission; the final reachable-consumer audit below may
  // only derive such a permission after inspecting the complete object.
  Info.ExactPhysicalStorageRange.reset();
  if (Info.RelocAbsolute && !Info.IsRelative && Info.HasBaseAddr &&
      Info.EntrySize != 0 && Info.PhysicalCapacity != 0) {
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride >= Info.EntrySize) {
      // Reserve the owner-inventory traversal for the sized-object check below,
      // together with the symbol scan performed by dataObjectSizeAt.  The
      // broader anchor-helper inventory is charged separately only if the
      // owner/symbol evidence cannot establish an exact boundary directly.
      if (!consumeCandidateProducts(
              {{Img.Segments.size(), 3}, {Img.Sections.size(), 3}}) ||
          !consumeCandidateEvidence(Img.Symbols.size()))
        return {};
      uint32_t ExactPhysicalSlotCount = Info.PhysicalCapacity;
      bool ExactOwnerBoundary = false;
      bool AllowAnchorBoundary = true;
      auto ExactSlotsForExtent =
          [&](uint64_t ByteExtent) -> std::optional<uint32_t> {
        if (ByteExtent < Info.EntrySize ||
            (ByteExtent - Info.EntrySize) % PhysicalStride != 0)
          return std::nullopt;
        const uint64_t Slots =
            (ByteExtent - Info.EntrySize) / PhysicalStride + 1;
        if (Slots == 0 || Slots > std::numeric_limits<uint32_t>::max())
          return std::nullopt;
        return static_cast<uint32_t>(Slots);
      };
      const std::optional<va_t> OwnerEnd =
          Img.mappedObjectOwnerEnd(Info.BaseAddr);
      const uint64_t DataObjectSize = Img.dataObjectSizeAt(Info.BaseAddr);
      if (DataObjectSize == 0 &&
          (!PhysicalRawAbsCodePtrRunComplete ||
           PhysicalRawAbsCodePtrRun > Info.PhysicalCapacity))
        AllowAnchorBoundary = false;
      if (DataObjectSize != 0) {
        // A sized symbol is the narrowest available object identity.  It is
        // exact only when it is wholly contained by its mapped owner and the
        // complete sized object fits the authenticated relocation capacity.
        // Never replace a rejected sized-object boundary with an interior
        // anchor: that would certify only a prefix of the same object.
        AllowAnchorBoundary = false;
        if (!OwnerEnd || DataObjectSize > InvalidVA - Info.BaseAddr ||
            Info.BaseAddr + DataObjectSize > *OwnerEnd) {
          ExactPhysicalSlotCount = 0;
        } else if (const std::optional<uint32_t> SizedSlots =
                       ExactSlotsForExtent(DataObjectSize);
                   SizedSlots && *SizedSlots <= Info.PhysicalCapacity) {
          ExactPhysicalSlotCount = *SizedSlots;
          ExactOwnerBoundary = true;
        } else {
          ExactPhysicalSlotCount = 0;
        }
      } else if (OwnerEnd && *OwnerEnd >= Info.BaseAddr) {
        if (const std::optional<uint32_t> OwnerSlots =
                ExactSlotsForExtent(*OwnerEnd - Info.BaseAddr);
            OwnerSlots && *OwnerSlots <= Info.PhysicalCapacity) {
          ExactPhysicalSlotCount = *OwnerSlots;
          ExactOwnerBoundary = true;
        }
      }
      bool HasExactBoundary = ExactOwnerBoundary;
      if (ExactPhysicalSlotCount != 0 && !HasExactBoundary &&
          AllowAnchorBoundary) {
        if (!consumeExactBoundaryInventory(Img, DecodedTableAnchors.size()))
          return {};
        HasExactBoundary = codePtrRelocRunHasExactBoundary(
            Img, Info.BaseAddr, PhysicalStride, ExactPhysicalSlotCount,
            DecodedTableAnchors);
      }
      if (ExactPhysicalSlotCount != 0 && HasExactBoundary) {
        const JumpTableStorageRange CandidatePhysical{
            Info.BaseAddr, Info.EntrySize, PhysicalStride,
            ExactPhysicalSlotCount};
        // Exact physical identity must contain every runtime storage range.
        // Traverse the full inventory without early exit: a malformed or
        // out-of-envelope alternative anywhere in the candidate invalidates
        // the certificate, even when the first range is well formed.
        if (!consumeCandidateProducts(
                {{Info.StorageRanges.size(), 16}, {1, 8}}))
          return {};
        const std::optional<va_t> CandidateEnd = CandidatePhysical.storageEnd();
        bool CoversAllStorage = CandidateEnd.has_value();
        bool HasStorage = false;
        va_t StorageEnvelopeBase = InvalidVA;
        va_t StorageEnvelopeEnd = 0;
        for (const JumpTableStorageRange &Range : Info.StorageRanges) {
          const std::optional<va_t> RangeEnd = Range.storageEnd();
          HasStorage = true;
          StorageEnvelopeBase = std::min(StorageEnvelopeBase, Range.BaseAddr);
          if (RangeEnd)
            StorageEnvelopeEnd = std::max(StorageEnvelopeEnd, *RangeEnd);

          bool RangeCovered = RangeEnd.has_value();
          RangeCovered &= Range.BaseAddr >= CandidatePhysical.BaseAddr;
          RangeCovered &= CandidateEnd.has_value() && RangeEnd.has_value();
          if (CandidateEnd && RangeEnd)
            RangeCovered &= *RangeEnd <= *CandidateEnd;
          RangeCovered &= Range.EntrySize == CandidatePhysical.EntrySize;
          if (Range.BaseAddr >= CandidatePhysical.BaseAddr) {
            const uint64_t Offset = Range.BaseAddr - CandidatePhysical.BaseAddr;
            RangeCovered &= Offset % CandidatePhysical.EntryStride == 0;
          } else {
            RangeCovered = false;
          }
          if (Range.PhysicalSlotCount > 1)
            RangeCovered &=
                Range.EntryStride % CandidatePhysical.EntryStride == 0;
          CoversAllStorage &= RangeCovered;
        }
        if (HasStorage) {
          CoversAllStorage &= StorageEnvelopeBase >= CandidatePhysical.BaseAddr;
          CoversAllStorage &= CandidateEnd.has_value();
          if (CandidateEnd)
            CoversAllStorage &= StorageEnvelopeEnd <= *CandidateEnd;
        }
        if (CoversAllStorage)
          Info.ExactPhysicalStorageRange = CandidatePhysical;
      }
    }
  }
  bool ImportedPriorRelativePhysicalIdentity = false;
  const size_t PreReadRootCount = ActiveJumpTableProofRoots
                                      ? ActiveJumpTableProofRoots->size()
                                      : PersistentCFGRoots.size();
  if (!consumeCandidateProducts({{PreReadRootCount, 2}}))
    return {};
  const std::set<va_t> PreReadValidatedRoots = ActiveJumpTableProofRoots
                                                   ? *ActiveJumpTableProofRoots
                                                   : PersistentCFGRoots;
  const bool PotentialClaimed = ClaimValidatedPotentialTable();
  if (!PotentialClaimed)
    return {};
  if (!Info.ExactPhysicalStorageRange && CurrentCandidateIsStrongProposal &&
      ProvisionalRelativeEdgeTemplate &&
      ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage) {
    const JumpTableStorageRange &Physical =
        ProvisionalRelativeEdgeTemplate->Storage;
    if (!consumeCandidateProducts({{Info.StorageRanges.size(), 6}}))
      return {};
    bool RuntimeAlreadyNamesWholePhysicalObject =
        Info.StorageRanges.size() == 1 &&
        sameStorageRangeNoShortCircuit(Info.StorageRanges.front(), Physical);
    if (!RuntimeAlreadyNamesWholePhysicalObject &&
        Info.StorageRanges.size() == Physical.PhysicalSlotCount) {
      RuntimeAlreadyNamesWholePhysicalObject = true;
      for (size_t I = 0; I < Info.StorageRanges.size(); ++I) {
        if (I != 0 &&
            Physical.EntryStride > (InvalidVA - Physical.BaseAddr) / I) {
          RuntimeAlreadyNamesWholePhysicalObject = false;
          break;
        }
        const va_t Slot =
            Physical.BaseAddr + uint64_t(I) * Physical.EntryStride;
        RuntimeAlreadyNamesWholePhysicalObject &=
            sameStorageRangeNoShortCircuit(
                Info.StorageRanges[I],
                JumpTableStorageRange{Slot, Physical.EntrySize,
                                      Physical.EntrySize, 1});
      }
    }
    // ExactPhysicalStorageRange is a separate identity only when the physical
    // object contains coordinates outside this dispatch's runtime storage.
    // Duplicating a fully covered dense object makes sibling dispatches audit
    // and republish identical roots at increasing ranks without adding proof.
    if (!RuntimeAlreadyNamesWholePhysicalObject)
      Info.ExactPhysicalStorageRange = Physical;
  }
  if (!Info.ExactPhysicalStorageRange && CandidateProposalStageActive &&
      CurrentCandidateIsStrongProposal && Info.IsRelative && Info.HasBaseAddr &&
      Info.EntrySize != 0 && ProvisionalRelativeEdgeTemplate) {
    const auto Prior = PriorStrongJumpTableProposals.find(Rec.Addr);
    if (Prior != PriorStrongJumpTableProposals.end() &&
        Prior->second.ExactPhysicalStorageRange) {
      const uint64_t PhysicalStride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      const JumpTableStorageRange &PriorPhysical =
          *Prior->second.ExactPhysicalStorageRange;
      const bool SamePhysicalShape =
          PriorPhysical.BaseAddr == Info.BaseAddr &&
          PriorPhysical.EntrySize == Info.EntrySize &&
          PriorPhysical.EntryStride == PhysicalStride &&
          PriorPhysical.PhysicalSlotCount >= limits::kMinJumpTableEntries &&
          PriorPhysical.PhysicalSlotCount <= limits::kMaxJumpTableEntries &&
          PriorPhysical.storageEnd().has_value();
      if (SamePhysicalShape) {
        Info.ExactPhysicalStorageRange = PriorPhysical;
        ImportedPriorRelativePhysicalIdentity = true;
      }
    }
  }
  const bool CanRecordStrongProposal =
      !ProvisionalRelativeEdgeTemplate ||
      ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage;
  bool CarryPriorStrongProposal = false;
  uint32_t CarriedPriorStrongProofRank = 0;
  if (CandidateProposalStageActive && CurrentCandidateIsStrongProposal &&
      !CanRecordStrongProposal && ProvisionalRelativeEdgeTemplate &&
      !ProvisionalRelativeEdgeTemplate->AuthenticatesPhysicalStorage &&
      !TargetRoleEdgeOverrides &&
      !CurrentCandidateReplayedOwnProvisionalRelativeEdge &&
      !UsedPriorOccurrenceCertificateForGuard) {
    if (!consumeCandidateEvidence(
            orderedLookupWork(PriorStrongJumpTableProposals.size())))
      return {};
    const auto Prior = PriorStrongJumpTableProposals.find(Rec.Addr);
    if (Prior != PriorStrongJumpTableProposals.end()) {
      const std::optional<size_t> StorageWork =
          detail::siblingStorageIdentityComparisonWork(
              Prior->second.StorageRanges.size(), Info.StorageRanges.size());
      const std::optional<size_t> RoleWork =
          detail::strongJumpTableLoadRoleVectorComparisonWork(
              Prior->second.LoadRoles.size(), PrePruningLoadRoles.size());
      if (!StorageWork || !RoleWork ||
          *RoleWork > std::numeric_limits<size_t>::max() - 5 ||
          *StorageWork > std::numeric_limits<size_t>::max() - *RoleWork - 5 ||
          !consumeCandidateEvidence(*StorageWork + *RoleWork + 5))
        return {};
      bool SameRuntimeIdentity = sameStorageRangeVectorsNoShortCircuit(
          Prior->second.StorageRanges, Info.StorageRanges);
      SameRuntimeIdentity &=
          Prior->second.ExactPhysicalStorageRange.has_value() ==
          Info.ExactPhysicalStorageRange.has_value();
      if (Prior->second.ExactPhysicalStorageRange &&
          Info.ExactPhysicalStorageRange)
        SameRuntimeIdentity &= sameStorageRangeNoShortCircuit(
            *Prior->second.ExactPhysicalStorageRange,
            *Info.ExactPhysicalStorageRange);
      SameRuntimeIdentity &= sameLoadRoleVectorsNoShortCircuit(
          Prior->second.LoadRoles, PrePruningLoadRoles);
      const bool SameIndependentRank =
          ActiveJumpTableCandidateDependencyRank == Prior->second.ProofRank &&
          (ActiveJumpTableCandidateProofRank == Prior->second.ProofRank ||
           (CurrentCandidateMayReplayOwnProvisionalRelativeEdge &&
            Prior->second.ProofRank != std::numeric_limits<uint32_t>::max() &&
            ActiveJumpTableCandidateProofRank == Prior->second.ProofRank + 1));
      if (SameRuntimeIdentity && SameIndependentRank) {
        CarryPriorStrongProposal = true;
        CarriedPriorStrongProofRank = Prior->second.ProofRank;
      }
    }
  }
  if (CandidateProposalStageActive && CurrentCandidateIsStrongProposal &&
      (CanRecordStrongProposal || CarryPriorStrongProposal)) {
    // Retain only the exact occurrence roles consumed by siblings.  Selector,
    // domain, ownership, and target metadata remain candidate-local and must
    // be reconstructed on every stage.  Prepay both scans, retained vector
    // slots, and ordered-container nodes before allocating any proposal state.
    // Scan/copy each occurrence role now and reserve one additional unit for
    // transactional destruction if this stage later rolls back.  That cleanup
    // reserve is consumed before proposal allocation and is never refreshed.
    if (!consumeCandidateProducts({{Info.StorageRanges.size(), 3}}) ||
        !consumeCandidateEvidence(
            orderedLookupWork(NextStrongJumpTableProposals.size()) + 1) ||
        !consumeCandidateEvidence(
            orderedLookupWork(EverStrongJumpTableProposalBranches.size()) + 1))
      return {};
    StrongJumpTableRoleProposal Proposal;
    Proposal.StorageRanges = Info.StorageRanges;
    Proposal.ExactPhysicalStorageRange = Info.ExactPhysicalStorageRange;
    Proposal.LoadRoles = std::move(PrePruningLoadRoles);
    Proposal.ProofRank = CarryPriorStrongProposal
                             ? CarriedPriorStrongProofRank
                             : ActiveJumpTableCandidateProofRank;
    NextStrongJumpTableProposals.insert_or_assign(Rec.Addr,
                                                  std::move(Proposal));
    EverStrongJumpTableProposalBranches.insert(Rec.Addr);
    CandidateStrongProposalRecorded = true;
  }

  // Importing a prior physical identity is only a comparison aid for the
  // complete runtime/load/rank carry gate above.  A failed carry must revoke
  // it before target materialization or consumer auditing can observe it.
  if (ImportedPriorRelativePhysicalIdentity && !CarryPriorStrongProposal)
    Info.ExactPhysicalStorageRange.reset();

  // readTableEntries and every grounded/emulated replacement are bounded by
  // the authenticated runtime domain.  Reserve the largest possible returned
  // vector before the first table entry is materialized; the outer stage may
  // then destroy it transactionally on any later failure without fresh work.
  const size_t TargetCleanupCapacity =
      Info.RuntimeSlotIndices.empty() ? static_cast<size_t>(Info.MaxEntries)
                                      : Info.RuntimeSlotIndices.size();
  if (!reservePublishedTargetCleanup(TargetCleanupCapacity))
    return {};
  std::vector<uint32_t> KeptIdx;
  auto Targets = readTableEntries(Img, Info, &KeptIdx);
  CandidateTargetMaterializationStarted = true;

  // A proven runtime domain is complete: every feasible coordinate must map to
  // one valid target.  Truncation is only meaningful for the legacy unbounded
  // scanner; truncating a bounded domain silently changes guest control flow.
  const size_t BeforeSanity = Targets.size();
  const bool Sane = sanityCheckTargets(Img, Targets);
  if (!Sane || ((Info.MaxEntries > 0 || !Info.RuntimeSlotIndices.empty()) &&
                Targets.size() != BeforeSanity))
    Targets.clear();
  if (KeptIdx.size() > Targets.size())
    KeptIdx.resize(Targets.size()); // sanity-check truncates trailing entries

  // Emulation-based fallback: when all static strategies fail, try
  // running the ops through the NdOp emulator for each candidate index.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries > 0 &&
      Info.RuntimeSlotIndices.empty() && CurrentImg) {
    auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
    std::vector<va_t> Checked = EmuTargets;
    if (EmuTargets.size() == Info.MaxEntries &&
        sanityCheckTargets(Img, Checked) &&
        Checked.size() == EmuTargets.size()) {
      Targets = std::move(EmuTargets);
      KeptIdx.clear(); // dense fallback: positional labels apply
      LLVM_DEBUG(llvm::dbgs() << "  emulated: recovered " << Targets.size()
                              << " entries via NdOp emulation\n");
    }
  }

  // Emulation cross-check for a bounded table that decoded *fewer* targets than
  // its known entry count.  The static reader classifies the entry layout
  // (relative/absolute, sign, scale, target-base) before decoding, and a
  // misclassification can truncate an otherwise-valid table part-way — leaving
  // a plausible-but-incomplete target list that the `< kMin` fallback above
  // (which fires only on near-total failure) never revisits.  Re-run the
  // *actual* dispatch arithmetic through the emulator, which reads the real
  // base+index+load and so cannot mis-guess the layout, and adopt its result
  // only when it is strictly more complete AND reproduces the static decode on
  // every shared index.  That prefix-agreement gate makes this monotonic: it
  // can only append cases the static read dropped, never rewrite one it already
  // decoded, so a correctly recovered table is left untouched.  Restricted to a
  // dense static result (no sparse skips) so the two index coordinates align,
  // and bounded by the same MaxEntries so it can never over-read past the
  // guard/reloc bound.
  if (CurrentImg && Info.RuntimeSlotIndices.empty() && Info.MaxEntries > 0 &&
      Targets.size() >= limits::kMinJumpTableEntries &&
      Targets.size() < Info.MaxEntries) {
    const std::optional<size_t> DenseStaticWork =
        detail::denseSlotIndexComparisonWork(KeptIdx.size());
    if (!DenseStaticWork || !consumeCandidateEvidence(*DenseStaticWork))
      return {};
    bool DenseStatic = KeptIdx.size() == Targets.size();
    for (size_t I = 0; I < KeptIdx.size(); ++I)
      DenseStatic &= KeptIdx[I] == I;
    if (DenseStatic) {
      auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
      const std::optional<size_t> ExtensionWork =
          detail::denseSlotIndexComparisonWork(Targets.size(),
                                               /*FixedWork=*/2);
      if (!ExtensionWork || !consumeCandidateEvidence(*ExtensionWork))
        return {};
      bool ExtendsStatic = EmuTargets.size() == Info.MaxEntries &&
                           EmuTargets.size() > Targets.size();
      const size_t SharedTargetCount =
          std::min(EmuTargets.size(), Targets.size());
      for (size_t I = 0; I < SharedTargetCount; ++I)
        ExtendsStatic &= EmuTargets[I] == Targets[I];
      if (ExtendsStatic) {
        LLVM_DEBUG(llvm::dbgs() << "  emu-verify: extended bounded table from "
                                << Targets.size() << " to " << EmuTargets.size()
                                << " entries via dispatch emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  // Ground-truth cross-check for a plain relative/absolute table: rebuild the
  // targets by executing the *actual* dispatch arithmetic per index instead of
  // trusting the statically classified entry layout (relative-vs-absolute,
  // signedness).  A misclassified layout decodes a full-length but *wrong*
  // target set that still passes the sanity check (every entry lands in the
  // function), a silent miscompile the extend/fallback strategies above never
  // revisit because the count already looks complete.  The emulation reads the
  // same table bytes and applies the same transform the processor would, so
  // when it is fully grounded — every index read the recovered table slot
  // (BaseAddr + i*EntryStride) and produced a valid target — its result is
  // authoritative and supersedes a disagreeing static decode.
  //
  // Guarded to be a no-op wherever the static decode is already trustworthy, so
  // currently-recovered tables keep byte-identical targets: skipped for
  // reloc-bounded / two-table / compact (TargetBase) / pre-scaled tables (whose
  // layout is confirmed by relocations or a dedicated detector), for sparse
  // (gapped) decodes whose positional index would not line up with the emulated
  // slot, and adopted only when the emulation agrees on entry count yet differs
  // on some value.
  const std::optional<size_t> DenseStaticWork =
      detail::denseSlotIndexComparisonWork(KeptIdx.size(), /*FixedWork=*/2);
  if (!DenseStaticWork || !consumeCandidateEvidence(*DenseStaticWork))
    return {};
  bool DenseStatic = KeptIdx.empty() || KeptIdx.size() == Targets.size();
  for (size_t I = 0; I < KeptIdx.size(); ++I)
    DenseStatic &= KeptIdx[I] == I;
  if (CurrentImg && Info.RuntimeSlotIndices.empty() && DenseStatic &&
      !Targets.empty() && Info.IndexReg != InvalidVA && Info.HasBaseAddr &&
      !Info.HasTargetBase && Info.EntryScale == 1 && !Info.PreScaledIndex &&
      !Info.TwoTableSelect && !Info.RelocAbsolute && !Info.RelocBounded &&
      (Info.EntrySize == 1 || Info.EntrySize == 2 || Info.EntrySize == 4 ||
       Info.EntrySize == 8)) {
    bool Grounded = false;
    auto EmuTargets = emulateGroundedTargets(
        Img, Rec, Info, static_cast<uint32_t>(Targets.size()), Grounded);
    const std::optional<size_t> GroundedTargetComparisonWork =
        detail::scalarVectorComparisonWork(EmuTargets.size(), Targets.size());
    if (!GroundedTargetComparisonWork ||
        !consumeCandidateEvidence(*GroundedTargetComparisonWork))
      return {};
    bool SameGroundedTargets = EmuTargets.size() == Targets.size();
    for (size_t I = 0; I < std::min(EmuTargets.size(), Targets.size()); ++I)
      SameGroundedTargets &= EmuTargets[I] == Targets[I];
    if (Grounded && EmuTargets.size() == Targets.size() &&
        !SameGroundedTargets) {
      std::vector<va_t> Check = EmuTargets;
      if (sanityCheckTargets(Img, Check) && Check.size() == EmuTargets.size()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "  emu-ground: corrected " << Targets.size()
                   << " statically-misclassified targets via grounded dispatch "
                      "emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  // A bounded domain is publishable only when every authenticated runtime
  // coordinate produced a valid target.  Relocation-free inline tables may
  // expose only a strict, already validated prefix in the current immutable
  // CFG.  Sign that prefix as edge-only proposal evidence and defer instead of
  // publishing it as a complete switch; the next lower-rank-overlay round can
  // use those exact edges to reach the sibling that closes the domain.
  const size_t AuthenticatedRuntimeTargetCount =
      Info.RuntimeSlotIndices.empty() ? static_cast<size_t>(Info.MaxEntries)
                                      : Info.RuntimeSlotIndices.size();
  if (ProvisionalRelativeEdgeTemplate && AuthenticatedRuntimeTargetCount != 0 &&
      Targets.size() != AuthenticatedRuntimeTargetCount) {
    if (CandidateProposalStageActive && Rec.JumpTableTargets.empty() &&
        !Targets.empty()) {
      if (!consumeCandidateProducts(
              {{Targets.size(), orderedLookupWork(ExploredAddrs.size())},
               {Targets.size(), orderedLookupWork(BlockStarts.size())},
               {Targets.size(), orderedLookupWork(Insns.size())}}))
        return {};
      const bool EveryTargetAlreadyDecoded =
          std::all_of(Targets.begin(), Targets.end(), [&](va_t Target) {
            return ExploredAddrs.count(Target) && BlockStarts.count(Target) &&
                   Insns.count(Target);
          });
      if (EveryTargetAlreadyDecoded && !RecordProvisionalRelativeEdges(Targets))
        return {};
    }
    return {};
  }

  if (Targets.size() < limits::kMinJumpTableEntries) {
    ClaimRejectedPhysicalTableIdentity();
    return {};
  }

  // A relocation-backed absolute pointer array is a computed-goto table only
  // when every entry is an interior basic-block target of this function.
  // CurrentFuncEntry is a self callback, and known/typed entries are ordinary
  // function pointers even when a containing symbol's size covers them.  Keep
  // those branches as normal indirect tail calls; no strong/potential marker
  // is published because candidate-local ownership already rejected them.
  if (Info.RelocAbsolute) {
    if (!consumeCandidateEvidence(Targets.size()))
      return {};
    if (std::any_of(Targets.begin(), Targets.end(), [&](va_t Target) {
          return Target == CurrentFuncEntry ||
                 (KnownFuncEntries && KnownFuncEntries->count(Target)) ||
                 Img.hasFunctionSymbolAt(Target);
        })) {
      ClaimRejectedPhysicalTableIdentity();
      return {};
    }
  }

  if ((!Info.RuntimeCaseLabels.empty() || !Info.RuntimeSlotIndices.empty()) &&
      (Info.RuntimeCaseLabels.size() != Targets.size() ||
       Info.RuntimeSlotIndices.size() != Targets.size()))
    return {};

  // Decide which physical code relocations this dispatch may suppress
  // only after both the final runtime domain and final target edges are known.
  // Start with every pointer relocation in the authenticated physical object,
  // build a candidate-local CFG, and monotonically remove any gap/filler slot
  // that has an independent consumer in that reachable graph.  Repeating to a
  // fixed point handles a target root that becomes reachable only after another
  // slot loses suppression.  A lexical consumer in a pruned block is not
  // evidence; an entry-reachable LEA/LOAD of the slot is.
  const bool AuditExactRelativePhysicalObject =
      Info.IsRelative && Info.EntrySize == sizeof(uint32_t) &&
      Info.ExactPhysicalStorageRange &&
      Info.ExactPhysicalStorageRange->BaseAddr == Info.BaseAddr &&
      Info.ExactPhysicalStorageRange->EntrySize == Info.EntrySize &&
      Info.ExactPhysicalStorageRange->EntryStride >= Info.EntrySize &&
      Info.ExactPhysicalStorageRange->PhysicalSlotCount >=
          limits::kMinJumpTableEntries;
  std::vector<JumpTableStorageRange> ConsumerAuditStorageRanges =
      AuditExactRelativePhysicalObject
          ? std::vector<JumpTableStorageRange>{*Info.ExactPhysicalStorageRange}
          : Info.StorageRanges;
  if (!ConsumerAuditStorageRanges.empty() && Img.getPointerSize() != 0 &&
      (!Info.SuppressibleRelocationSlots.empty() ||
       AuditExactRelativePhysicalObject)) {
    ActiveJumpTableConsumerAudit = true;
    const std::set<uint64_t> &CodeRelocationSlots =
        AuditExactRelativePhysicalObject ? Img.RelCodeRelocSlots
                                         : Img.CodePtrRelocSlots;
    std::vector<va_t> PhysicalRelocationSlots;
    size_t SlotBudget = limits::kMaxJumpTableEntries;
    const std::optional<size_t> StorageRangeInventoryWork =
        detail::physicalCodePointerSlotScanWork(
            ConsumerAuditStorageRanges.size(), /*PhysicalSlotCount=*/0,
            /*RelocationLookupWork=*/0);
    if (!StorageRangeInventoryWork ||
        !consumeCandidateEvidence(*StorageRangeInventoryWork))
      return {};
    size_t PhysicalSlotUpperBound = 0;
    for (const JumpTableStorageRange &Range : ConsumerAuditStorageRanges) {
      if (Range.PhysicalSlotCount >
          std::numeric_limits<size_t>::max() - PhysicalSlotUpperBound) {
        consumeCandidateEvidence(std::numeric_limits<size_t>::max());
        return {};
      }
      PhysicalSlotUpperBound += static_cast<size_t>(Range.PhysicalSlotCount);
    }
    const std::optional<size_t> PhysicalSlotScanWork =
        detail::physicalCodePointerSlotScanWork(
            /*StorageRangeCount=*/0, PhysicalSlotUpperBound,
            orderedLookupWork(CodeRelocationSlots.size()));
    if (!consumeCandidateEvidence(
            PhysicalSlotScanWork.value_or(std::numeric_limits<size_t>::max())))
      return {};
    for (const JumpTableStorageRange &Range : ConsumerAuditStorageRanges) {
      if ((!AuditExactRelativePhysicalObject &&
           Range.EntrySize < Img.getPointerSize()) ||
          (AuditExactRelativePhysicalObject &&
           Range.EntrySize != sizeof(uint32_t)) ||
          Range.EntryStride < Range.EntrySize || Range.PhysicalSlotCount == 0)
        continue;
      if (Range.PhysicalSlotCount > SlotBudget)
        return {};
      SlotBudget -= Range.PhysicalSlotCount;
      for (uint32_t I = 0; I < Range.PhysicalSlotCount; ++I) {
        if (I != 0 && Range.EntryStride > (InvalidVA - Range.BaseAddr) / I)
          return {};
        const va_t Slot = Range.BaseAddr + uint64_t(I) * Range.EntryStride;
        if (CodeRelocationSlots.count(Slot))
          PhysicalRelocationSlots.push_back(Slot);
      }
    }
    size_t SortPasses = 0;
    for (size_t N = PhysicalRelocationSlots.size(); N > 1; N = (N + 1) / 2)
      ++SortPasses;
    if (!consumeCandidateProducts(
            {{PhysicalRelocationSlots.size(), SortPasses + 2}}))
      return {};
    std::sort(PhysicalRelocationSlots.begin(), PhysicalRelocationSlots.end());
    PhysicalRelocationSlots.erase(std::unique(PhysicalRelocationSlots.begin(),
                                              PhysicalRelocationSlots.end()),
                                  PhysicalRelocationSlots.end());

    auto storageOwns = [&](va_t Address, bool &AnalysisComplete) {
      if (!consumeCandidateEvidence(
              detail::storageOwnershipComparisonWork(
                  ConsumerAuditStorageRanges.size())
                  .value_or(std::numeric_limits<size_t>::max()))) {
        AnalysisComplete = false;
        return false;
      }
      bool Owns = false;
      for (const JumpTableStorageRange &Range : ConsumerAuditStorageRanges)
        Owns |= Range.ownsStorageAddress(Address);
      return Owns;
    };
    auto containingInsnIsReachable = [&](va_t Field,
                                         const std::set<va_t> &Reachable,
                                         bool &AnalysisComplete) {
      if (!consumeCandidateProducts({{1, orderedLookupWork(Insns.size())},
                                     {1, orderedLookupWork(Reachable.size())},
                                     {1, 3}})) {
        AnalysisComplete = false;
        return false;
      }
      auto It = Insns.upper_bound(Field);
      if (It == Insns.begin())
        return false;
      --It;
      const va_t Begin = It->first;
      const uint64_t Size = It->second.Size;
      return Reachable.count(Begin) && Field >= Begin &&
             Size <= InvalidVA - Begin && Field < Begin + Size;
    };
    auto storageBase = [&](va_t Address, bool &AnalysisComplete) {
      if (!consumeCandidateEvidence(ConsumerAuditStorageRanges.size())) {
        AnalysisComplete = false;
        return false;
      }
      return std::any_of(ConsumerAuditStorageRanges.begin(),
                         ConsumerAuditStorageRanges.end(),
                         [&](const JumpTableStorageRange &Range) {
                           return Range.BaseAddr == Address;
                         });
    };
    auto isAuthenticatedStaticSourceRelocation =
        [&](va_t StaticAddress, va_t FieldVA, va_t OwnerVA, va_t InsnAddr,
            bool &AnalysisComplete) {
          const size_t InitializerCount =
              Info.AuthenticatedFrameStorage.Initializers.size();
          const std::optional<size_t> InitializerWork =
              detail::staticSourceRelocationComparisonWork(InitializerCount, 0);
          if (!consumeCandidateEvidence(InitializerWork.value_or(
                  std::numeric_limits<size_t>::max()))) {
            AnalysisComplete = false;
            return false;
          }
          size_t StaticSourceCount = 0;
          for (const JumpTableFrameInitializerChunk &Initializer :
               Info.AuthenticatedFrameStorage.Initializers) {
            if (Initializer.StaticSources.size() >
                std::numeric_limits<size_t>::max() - StaticSourceCount) {
              consumeCandidateEvidence(std::numeric_limits<size_t>::max());
              AnalysisComplete = false;
              return false;
            }
            StaticSourceCount += Initializer.StaticSources.size();
          }
          const std::optional<size_t> StaticSourceWork =
              detail::staticSourceRelocationComparisonWork(
                  /*InitializerCount=*/0, StaticSourceCount);
          if (!consumeCandidateEvidence(StaticSourceWork.value_or(
                  std::numeric_limits<size_t>::max()))) {
            AnalysisComplete = false;
            return false;
          }
          for (const JumpTableFrameInitializerChunk &Initializer :
               Info.AuthenticatedFrameStorage.Initializers) {
            if (Initializer.IsMemcpy &&
                Initializer.StaticSourceProvenance ==
                    ConstantAddressProvenance::DataAddress &&
                Initializer.StaticSourceOwnerVA != InvalidVA &&
                Initializer.StaticSourceProducerTargetVA == StaticAddress &&
                Initializer.StaticSourceFieldVA == FieldVA &&
                Initializer.StaticSourceOwnerVA == OwnerVA &&
                Initializer.StaticSourceProducer.Addr == InsnAddr &&
                Initializer.StaticSourceProducer.Seq >= 0)
              return true;
            for (const auto &Source : Initializer.StaticSources) {
              const JumpTableValueOccurrence &Producer =
                  Source.StaticAddressProducer;
              if (Source.StaticAddressProvenance ==
                      ConstantAddressProvenance::DataAddress &&
                  Source.StaticAddressOwnerVA != InvalidVA &&
                  Source.StaticAddressProducerTargetVA == StaticAddress &&
                  Source.StaticAddressFieldVA == FieldVA &&
                  Source.StaticAddressOwnerVA == OwnerVA &&
                  Producer.Addr == InsnAddr && Producer.Seq >= 0)
                return true;
            }
          }
          return false;
        };
    auto isAuthenticatedStaticSourceLowOccurrence =
        [&](va_t StaticAddress, const LowOp &Op, const NdVar &Value,
            bool &AnalysisComplete) {
          const size_t InitializerCount =
              Info.AuthenticatedFrameStorage.Initializers.size();
          const std::optional<size_t> InitializerWork =
              detail::staticSourceLowOccurrenceComparisonWork(
                  InitializerCount, /*StaticSourceCount=*/0, Op.NumInputs);
          if (!consumeCandidateEvidence(InitializerWork.value_or(
                  std::numeric_limits<size_t>::max()))) {
            AnalysisComplete = false;
            return false;
          }
          size_t StaticSourceCount = 0;
          for (const JumpTableFrameInitializerChunk &Initializer :
               Info.AuthenticatedFrameStorage.Initializers) {
            if (Initializer.StaticSources.size() >
                std::numeric_limits<size_t>::max() - StaticSourceCount) {
              consumeCandidateEvidence(std::numeric_limits<size_t>::max());
              AnalysisComplete = false;
              return false;
            }
            StaticSourceCount += Initializer.StaticSources.size();
          }
          const std::optional<size_t> StaticSourceWork =
              detail::staticSourceLowOccurrenceComparisonWork(
                  /*InitializerCount=*/0, StaticSourceCount, Op.NumInputs);
          if (!consumeCandidateEvidence(StaticSourceWork.value_or(
                  std::numeric_limits<size_t>::max()))) {
            AnalysisComplete = false;
            return false;
          }
          for (const JumpTableFrameInitializerChunk &Initializer :
               Info.AuthenticatedFrameStorage.Initializers) {
            if (Initializer.IsMemcpy &&
                Initializer.StaticSourceProvenance ==
                    ConstantAddressProvenance::DataAddress &&
                Initializer.StaticSourceOwnerVA != InvalidVA &&
                Initializer.StaticSourceProducerTargetVA == StaticAddress) {
              const JumpTableValueOccurrence &Producer =
                  Initializer.StaticSourceProducer;
              if (Op.Addr == Producer.Addr && Op.Seq == Producer.Seq &&
                  Value == Producer.Value &&
                  ((Producer.DefinedAtPoint && Op.Output == Value) ||
                   (!Producer.DefinedAtPoint &&
                    llvm::is_contained(
                        llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                        Value))))
                return true;
            }
            for (const auto &Source : Initializer.StaticSources) {
              const JumpTableValueOccurrence &Producer =
                  Source.StaticAddressProducer;
              if (Source.StaticAddressProvenance !=
                      ConstantAddressProvenance::DataAddress ||
                  Source.StaticAddressOwnerVA == InvalidVA ||
                  Source.StaticAddressProducerTargetVA != StaticAddress ||
                  Op.Addr != Producer.Addr || Op.Seq != Producer.Seq ||
                  Value != Producer.Value)
                continue;
              if (Producer.DefinedAtPoint && Op.Output == Value)
                return true;
              if (!Producer.DefinedAtPoint)
                for (uint8_t I = 0; I < Op.NumInputs; ++I)
                  if (Op.Inputs[I] == Value)
                    return true;
            }
          }
          return false;
        };
    auto isAuthenticatedTargetLoad = [&](const LowOp &Op,
                                         bool &AnalysisComplete) {
      if (Op.Opcode != NdOp::LOAD)
        return false;
      auto Authenticates = [&](const auto &Candidate) {
        const std::optional<size_t> ComparisonWork =
            detail::strongJumpTableLoadRoleScanComparisonWork(
                Candidate.LoadRoles.size());
        if (!consumeCandidateEvidence(
                ComparisonWork.value_or(std::numeric_limits<size_t>::max()))) {
          AnalysisComplete = false;
          return false;
        }
        bool Authenticated = false;
        for (const auto &Role : Candidate.LoadRoles) {
          bool Matches = true;
          Matches &= Role.Load.DefinedAtPoint;
          Matches &= Role.Load.Value.Space == Op.Output.Space;
          Matches &= Role.Load.Value.Offset == Op.Output.Offset;
          Matches &= Role.Load.Value.Size == Op.Output.Size;
          Matches &= Role.Load.Value.Provenance == Op.Output.Provenance;
          Matches &= Role.Load.Value.AddressOwnerVA == Op.Output.AddressOwnerVA;
          Matches &= Role.Load.Addr == Op.Addr;
          Matches &= Role.Load.Seq == Op.Seq;
          Matches &= Role.LoadWidth == Op.Output.Size;
          Authenticated |= Matches;
        }
        return Authenticated;
      };
      if (Authenticates(Info))
        return true;
      if (!AnalysisComplete)
        return false;
      if (CandidateProposalStageActive && CandidateStrongProposalRecorded) {
        if (!consumeCandidateEvidence(
                orderedLookupWork(NextStrongJumpTableProposals.size()))) {
          AnalysisComplete = false;
          return false;
        }
        const auto Current = NextStrongJumpTableProposals.find(Rec.Addr);
        if (Current == NextStrongJumpTableProposals.end()) {
          AnalysisComplete = false;
          return false;
        }
        // The current stage's proposal was captured from the complete
        // pre-pruning address-role inventory above.  Use it transactionally
        // while deciding whether one of those exact table LOADs is an
        // independent consumer of its own storage.  Publication still
        // requires the final role/domain replay, and rollback discards this
        // proposal together with every suppression decision derived from it.
        if (Authenticates(Current->second))
          return true;
        if (!AnalysisComplete)
          return false;
      }
      const size_t RoleUniverseSize = CandidateProposalStageActive
                                          ? PriorStrongJumpTableProposals.size()
                                          : ResolvedTableInfo.size();
      if (!consumeCandidateEvidence(RoleUniverseSize)) {
        AnalysisComplete = false;
        return false;
      }
      // A peeled/loop-body pair can dispatch through the same exact physical
      // table.  Once one branch has a published occurrence certificate, its
      // target LOAD is not an independent data consumer of the sibling table:
      // both loads are candidates for the same post-SSA terminal-use check.
      // Compare precise storage runs, not a numeric base, so an adjacent or
      // overlapping foreign table cannot borrow this exemption.  The module
      // Requested/Vetoed suppression arbitration remains authoritative when a
      // certified LOAD has any observable side use or a later rebuild loses
      // its jump-table plan.
      auto StorageIsProvablyDisjoint = [&](const auto &Other) {
        const std::optional<size_t> ComparisonWork =
            detail::disjointStorageRangeComparisonWork(
                Info.StorageRanges.size(), Other.StorageRanges.size());
        if (!consumeCandidateEvidence(
                ComparisonWork.value_or(std::numeric_limits<size_t>::max()))) {
          AnalysisComplete = false;
          return false;
        }
        if (Info.StorageRanges.empty() || Other.StorageRanges.empty())
          return false;
        auto InclusiveEnd = [](const JumpTableStorageRange &Range, va_t &End) {
          if (Range.EntrySize == 0 || Range.EntryStride < Range.EntrySize ||
              Range.PhysicalSlotCount == 0)
            return false;
          const uint64_t LastIndex = Range.PhysicalSlotCount - 1;
          if (LastIndex != 0 &&
              Range.EntryStride > (InvalidVA - Range.BaseAddr) / LastIndex)
            return false;
          const va_t LastStart = Range.BaseAddr + LastIndex * Range.EntryStride;
          if (Range.EntrySize - 1 > InvalidVA - LastStart)
            return false;
          End = LastStart + Range.EntrySize - 1;
          return true;
        };
        for (const JumpTableStorageRange &A : Info.StorageRanges)
          for (const JumpTableStorageRange &B : Other.StorageRanges) {
            va_t AEnd = InvalidVA;
            va_t BEnd = InvalidVA;
            if (!InclusiveEnd(A, AEnd) || !InclusiveEnd(B, BEnd)) {
              AnalysisComplete = false;
              return false;
            }
            if (!(AEnd < B.BaseAddr || BEnd < A.BaseAddr))
              return false;
          }
        return true;
      };
      auto AuthenticatesSibling = [&](const auto &RoleUniverse) {
        for (const auto &Entry : RoleUniverse) {
          const auto &Other = Entry.second;
          const std::optional<size_t> ComparisonWork =
              detail::siblingStorageIdentityComparisonWork(
                  Info.StorageRanges.size(), Other.StorageRanges.size());
          if (!consumeCandidateEvidence(ComparisonWork.value_or(
                  std::numeric_limits<size_t>::max()))) {
            AnalysisComplete = false;
            return false;
          }
          bool SameRuntimeStorage = !Info.StorageRanges.empty();
          SameRuntimeStorage &= sameStorageRangeVectorsNoShortCircuit(
              Info.StorageRanges, Other.StorageRanges);
          const bool InfoHasExact = Info.ExactPhysicalStorageRange.has_value();
          const bool OtherHasExact =
              Other.ExactPhysicalStorageRange.has_value();
          bool SameExactPhysicalStorage = InfoHasExact;
          SameExactPhysicalStorage &= OtherHasExact;
          if (InfoHasExact && OtherHasExact)
            SameExactPhysicalStorage &= sameStorageRangeNoShortCircuit(
                *Info.ExactPhysicalStorageRange,
                *Other.ExactPhysicalStorageRange);
          if ((SameRuntimeStorage || SameExactPhysicalStorage) &&
              Authenticates(Other))
            return true;
          // A frozen exact LOAD over disjoint storage is not an independent
          // consumer of this object.  Every borrowed proposal is revalidated
          // in the same transaction; if it disappears, the changed universe
          // forces this candidate to replay before fixed-point publication.
          if (StorageIsProvablyDisjoint(Other) && Authenticates(Other))
            return true;
          if (!AnalysisComplete)
            return false;
        }
        return false;
      };
      if (CandidateProposalStageActive)
        return AuthenticatesSibling(PriorStrongJumpTableProposals);
      return AuthenticatesSibling(ResolvedTableInfo);
    };
    auto instructionHasAuthenticatedTargetLoad =
        [&](va_t Addr, bool &AnalysisComplete) {
          if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
            AnalysisComplete = false;
            return false;
          }
          const auto It = Insns.find(Addr);
          if (It == Insns.end())
            return false;
          if (!consumeCandidateEvidence(It->second.Ops.size())) {
            AnalysisComplete = false;
            return false;
          }
          for (const LowOp &Op : It->second.Ops)
            if (Op.Opcode == NdOp::LOAD &&
                isAuthenticatedTargetLoad(Op, AnalysisComplete))
              return true;
          return false;
        };
    auto isAuthenticatedStorageConsumer =
        [&](const LowOp &Op, const NdVar &Value, bool &AnalysisComplete) {
          if (!consumeCandidateEvidence(
                  detail::authenticatedStorageConsumerComparisonWork(
                      Info.AuthenticatedStorageConsumers.size())
                      .value_or(std::numeric_limits<size_t>::max()))) {
            AnalysisComplete = false;
            return false;
          }
          bool Authenticated = false;
          for (const JumpTableValueOccurrence &Consumer :
               Info.AuthenticatedStorageConsumers) {
            bool Matches = true;
            Matches &= !Consumer.DefinedAtPoint;
            Matches &= Consumer.Addr == Op.Addr;
            Matches &= Consumer.Seq == Op.Seq;
            Matches &= Consumer.Value.Space == Value.Space;
            Matches &= Consumer.Value.Offset == Value.Offset;
            Matches &= Consumer.Value.Size == Value.Size;
            Matches &= Consumer.Value.Provenance == Value.Provenance;
            Matches &= Consumer.Value.AddressOwnerVA == Value.AddressOwnerVA;
            Authenticated |= Matches;
          }
          return Authenticated;
        };
    std::vector<JumpTableValueOccurrence> SlotAddressAlternatives;
    if (!consumeCandidateProducts({{PhysicalRelocationSlots.size(), 2}}))
      return {};
    SlotAddressAlternatives.reserve(PhysicalRelocationSlots.size());
    for (va_t Slot : PhysicalRelocationSlots)
      SlotAddressAlternatives.push_back(
          {NdVar::address(Slot, Img.getPointerSize()), InvalidVA, -1,
           /*DefinedAtPoint=*/false});

    // Object escape is wider than code-pointer-slot identity.  A consumer can
    // expose `base + 4`, an entry's interior byte, or a dynamically formed
    // pointer rooted anywhere in the authenticated storage span and then walk
    // to another relocation slot.  Enumerate every byte of the (bounded)
    // physical ranges for this revocation-only proof; if the evidence budget
    // cannot cover the complete object, fail closed by retaining every mirror
    // relocation.  Direct LOAD classification below deliberately keeps the
    // narrower slot-start alternatives so an exact read preserves only the
    // code pointer it actually consumes.
    std::vector<JumpTableValueOccurrence> StorageAddressAlternatives;
    bool StorageAddressAlternativesComplete = true;
    // This local ceiling bounds byte-wise object alternatives independently of
    // the candidate aggregate.  A legitimate maximum-width table may occupy
    // kMaxJumpTableEntries * 8 bytes, so the old generic 4096-work cap rejected
    // valid large tables before the shared account could meter them.
    size_t StorageAddressBudget =
        size_t{limits::kMaxJumpTableEntries} * sizeof(uint64_t);
    std::set<va_t> StorageAddresses;
    if (!consumeCandidateEvidence(
            detail::storageEndComparisonWork(ConsumerAuditStorageRanges.size())
                .value_or(std::numeric_limits<size_t>::max())))
      return {};
    for (const JumpTableStorageRange &Range : ConsumerAuditStorageRanges) {
      const std::optional<va_t> End = Range.storageEnd();
      if (!End || *End < Range.BaseAddr ||
          *End - Range.BaseAddr > StorageAddressBudget) {
        StorageAddressAlternativesComplete = false;
        break;
      }
      const size_t ByteCount = static_cast<size_t>(*End - Range.BaseAddr);
      if (!consumeCandidateProducts({{ByteCount, 2}})) {
        StorageAddressAlternativesComplete = false;
        break;
      }
      StorageAddressBudget -= ByteCount;
      for (va_t Address = Range.BaseAddr; Address < *End; ++Address) {
        if (!consumeCandidateEvidence(
                orderedLookupWork(StorageAddresses.size()))) {
          StorageAddressAlternativesComplete = false;
          break;
        }
        StorageAddresses.insert(Address);
      }
      if (!StorageAddressAlternativesComplete)
        break;
    }
    if (StorageAddressAlternativesComplete) {
      if (!consumeCandidateProducts({{StorageAddresses.size(), 2}}))
        return {};
      StorageAddressAlternatives.reserve(StorageAddresses.size());
      for (va_t Address : StorageAddresses)
        StorageAddressAlternatives.push_back(
            {NdVar::address(Address, Img.getPointerSize()), InvalidVA, -1,
             /*DefinedAtPoint=*/false});
    }
    auto objectAddressEscapes = [&](const std::set<va_t> &Reachable,
                                    bool &AnalysisComplete) {
      if (!StorageAddressAlternativesComplete) {
        AnalysisComplete = false;
        return false;
      }
      if (StorageAddressAlternatives.empty())
        return false;

      std::vector<JumpTableValueQuery> EscapeQueries;
      bool QueryConstructionComplete = true;
      auto AddQuery = [&](const NdVar &Value, const LowOp &Use) {
        if ((!Value.isReg() && !Value.isTemp() && !Value.isConst()) ||
            Value.Size == 0)
          return;
        if (isAuthenticatedStorageConsumer(Use, Value, AnalysisComplete))
          return;
        if (!AnalysisComplete) {
          QueryConstructionComplete = false;
          return;
        }
        if (EscapeQueries.size() >=
            limits::kMaxJumpTableValueMatchEvidenceWork) {
          QueryConstructionComplete = false;
          AnalysisComplete = false;
          return;
        }
        if (!consumeCandidateProducts(
                {{1, 2}, {StorageAddressAlternatives.size(), 1}})) {
          QueryConstructionComplete = false;
          return;
        }
        JumpTableValueQuery Q;
        Q.Candidate = Value;
        Q.UseAddr = Use.Addr;
        Q.UseSeq = Use.Seq;
        // This query can only revoke relocation suppression.  A generic exact
        // address alternative is therefore deliberately conservative: a
        // coincident scalar may retain a mirror field, but can never grant a
        // stale-address optimization.  Loader/role owners remain mandatory in
        // the positive table certificates above.
        Q.Alternatives = StorageAddressAlternatives;
        Q.AllowZeroExtension = true;
        Q.Relation = JumpTableValueRelation::MayDepend;
        EscapeQueries.push_back(std::move(Q));
      };
      if (!consumeCandidateEvidence(Reachable.size())) {
        AnalysisComplete = false;
        return false;
      }
      for (va_t Addr : Reachable) {
        if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
          AnalysisComplete = false;
          return false;
        }
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        if (!consumeCandidateProducts({{It->second.Ops.size(), 9}})) {
          AnalysisComplete = false;
          return false;
        }
        for (const LowOp &Op : It->second.Ops) {
          if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
            AddQuery(Op.Inputs[1], Op);
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
              Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::RETURN) {
            if (!consumeCandidateEvidence(Op.NumInputs)) {
              QueryConstructionComplete = false;
              AnalysisComplete = false;
              return false;
            }
            for (uint8_t I = 0; I < Op.NumInputs; ++I)
              AddQuery(Op.Inputs[I], Op);
          }
        }
      }
      if (!QueryConstructionComplete) {
        AnalysisComplete = false;
        return false;
      }
      if (EscapeQueries.empty())
        return false;
      bool Complete = true;
      const std::vector<bool> Results =
          tableValuesMatchAtUses(EscapeQueries, &Complete, nullptr, InvalidVA,
                                 nullptr, &CandidateEvidenceBudget,
                                 /*LocalMatchEvidenceLimit=*/0,
                                 /*CandidateBranchesSharingTargets=*/nullptr,
                                 /*QueryUnsignedFeasibleMasks=*/nullptr,
                                 limits::kMaxJumpTableExpandedResolverDepth);
      AnalysisComplete &= Complete;
      if (!consumeCandidateEvidence(Results.size())) {
        AnalysisComplete = false;
        return false;
      }
      return std::any_of(Results.begin(), Results.end(),
                         [](bool Result) { return Result; });
    };
    auto reachableIndependentLoads = [&](const std::set<va_t> &Reachable,
                                         bool &AnalysisComplete,
                                         std::set<va_t> &ExactSlots,
                                         bool &MayAliasObject) {
      struct LoadUse {
        NdVar Address;
        va_t Addr = InvalidVA;
        int Seq = -1;
      };
      std::vector<LoadUse> Loads;
      if (!consumeCandidateEvidence(Reachable.size())) {
        AnalysisComplete = false;
        return;
      }
      for (va_t Addr : Reachable) {
        if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
          AnalysisComplete = false;
          return;
        }
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        if (!consumeCandidateProducts({{It->second.Ops.size(), 7}})) {
          AnalysisComplete = false;
          return;
        }
        for (const LowOp &Op : It->second.Ops) {
          if (Op.Opcode != NdOp::LOAD || Op.NumInputs == 0)
            continue;
          if (isAuthenticatedTargetLoad(Op, AnalysisComplete))
            continue;
          if (!AnalysisComplete)
            return;
          const NdVar &Address =
              Op.NumInputs >= 2 ? Op.Inputs[1] : Op.Inputs[0];
          if ((!Address.isReg() && !Address.isTemp() && !Address.isConst()) ||
              Address.Size == 0)
            continue;
          if (isAuthenticatedStorageConsumer(Op, Address, AnalysisComplete))
            continue;
          if (!AnalysisComplete)
            return;
          if (!consumeCandidateEvidence(2)) {
            AnalysisComplete = false;
            return;
          }
          Loads.push_back({Address, Op.Addr, Op.Seq});
        }
      }
      if (!StorageAddressAlternativesComplete) {
        AnalysisComplete = false;
        return;
      }
      if (Loads.empty() || StorageAddressAlternatives.empty())
        return;
      const size_t Max = std::numeric_limits<size_t>::max();
      if (PhysicalRelocationSlots.size() == Max) {
        AnalysisComplete = false;
        return;
      }
      const size_t QueriesPerLoad = PhysicalRelocationSlots.size() + 1;
      if (Loads.size() > Max / QueriesPerLoad ||
          (!StorageAddressAlternatives.empty() &&
           Loads.size() > Max / StorageAddressAlternatives.size())) {
        consumeCandidateEvidence(Max);
        AnalysisComplete = false;
        return;
      }
      const size_t QueryCount = Loads.size() * QueriesPerLoad;
      const size_t DependencyAlternativeCount =
          Loads.size() * StorageAddressAlternatives.size();
      if (QueryCount > limits::kMaxJumpTableValueMatchEvidenceWork) {
        AnalysisComplete = false;
        return;
      }
      if (!consumeCandidateProducts(
              {{QueryCount, 2},
               {Loads.size(), PhysicalRelocationSlots.size()},
               {DependencyAlternativeCount, 1}})) {
        AnalysisComplete = false;
        return;
      }

      std::vector<JumpTableValueQuery> Queries;
      Queries.reserve(QueryCount);
      for (size_t LoadIndex = 0; LoadIndex < Loads.size(); ++LoadIndex) {
        const LoadUse &Load = Loads[LoadIndex];
        for (size_t SlotIndex = 0; SlotIndex < PhysicalRelocationSlots.size();
             ++SlotIndex) {
          JumpTableValueQuery Exact;
          Exact.Candidate = Load.Address;
          Exact.UseAddr = Load.Addr;
          Exact.UseSeq = Load.Seq;
          Exact.Alternatives = {SlotAddressAlternatives[SlotIndex]};
          Exact.AllowZeroExtension = true;
          Queries.push_back(std::move(Exact));
        }
        JumpTableValueQuery Dependency;
        Dependency.Candidate = Load.Address;
        Dependency.UseAddr = Load.Addr;
        Dependency.UseSeq = Load.Seq;
        Dependency.Alternatives = StorageAddressAlternatives;
        Dependency.AllowZeroExtension = true;
        Dependency.Relation = JumpTableValueRelation::MayDepend;
        Queries.push_back(std::move(Dependency));
      }

      bool Complete = true;
      const std::vector<bool> Results =
          tableValuesMatchAtUses(Queries, &Complete, nullptr, InvalidVA,
                                 nullptr, &CandidateEvidenceBudget,
                                 /*LocalMatchEvidenceLimit=*/0,
                                 /*CandidateBranchesSharingTargets=*/nullptr,
                                 /*QueryUnsignedFeasibleMasks=*/nullptr,
                                 limits::kMaxJumpTableExpandedResolverDepth);
      AnalysisComplete &= Complete;
      if (!AnalysisComplete)
        return;
      if (!consumeCandidateProducts(
              {{Results.size(), 1}, {PhysicalRelocationSlots.size(), 1}})) {
        AnalysisComplete = false;
        return;
      }
      const size_t ExactSlotInsertionWork =
          orderedLookupWork(PhysicalRelocationSlots.size()) + 2;
      if (!consumeCandidateFactorProduct({Loads.size(),
                                          PhysicalRelocationSlots.size(),
                                          ExactSlotInsertionWork})) {
        AnalysisComplete = false;
        return;
      }
      size_t ResultIndex = 0;
      for (size_t LoadIndex = 0; LoadIndex < Loads.size(); ++LoadIndex) {
        bool HasExactSlot = false;
        for (size_t SlotIndex = 0; SlotIndex < PhysicalRelocationSlots.size();
             ++SlotIndex) {
          if (ResultIndex < Results.size() && Results[ResultIndex]) {
            ExactSlots.insert(PhysicalRelocationSlots[SlotIndex]);
            HasExactSlot = true;
          }
          ++ResultIndex;
        }
        const bool DependsOnObject =
            ResultIndex < Results.size() && Results[ResultIndex];
        ++ResultIndex;
        if (DependsOnObject && !HasExactSlot)
          MayAliasObject = true;
      }
    };
    auto hasReachableIndependentConsumer = [&](va_t Slot,
                                               const std::set<va_t> &Reachable,
                                               bool WholeObjectEscapes,
                                               bool &AnalysisComplete) {
      if (WholeObjectEscapes)
        return true;
      if (!consumeCandidateProducts({{Img.DataAddressRelocOperands.size(), 3},
                                     {Img.CodeAddressRelocOperands.size(), 3},
                                     {Reachable.size(), 1},
                                     {Img.DataPtrRelocSlots.size(), 1}})) {
        AnalysisComplete = false;
        return true;
      }
      const bool SlotIsStorageBase = storageBase(Slot, AnalysisComplete);
      if (!AnalysisComplete)
        return true;
      for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands)
        if (!Field.PCRelativeFromInstructionEnd && Field.TargetVA == Slot &&
            !SlotIsStorageBase &&
            containingInsnIsReachable(FieldVA, Reachable, AnalysisComplete)) {
          if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
            AnalysisComplete = false;
            return true;
          }
          auto It = Insns.upper_bound(FieldVA);
          if (It != Insns.begin()) {
            --It;
            if (instructionHasAuthenticatedTargetLoad(It->first,
                                                      AnalysisComplete))
              continue;
            if (!AnalysisComplete)
              return true;
            if (isAuthenticatedStaticSourceRelocation(
                    Slot, FieldVA, Field.TargetOwnerVA, It->first,
                    AnalysisComplete))
              continue;
            if (!AnalysisComplete)
              return true;
          }
          return true;
        }
      if (!AnalysisComplete)
        return true;
      for (const auto &[FieldVA, Field] : Img.CodeAddressRelocOperands)
        if (!Field.PCRelativeFromInstructionEnd && Field.TargetVA == Slot &&
            !SlotIsStorageBase &&
            containingInsnIsReachable(FieldVA, Reachable, AnalysisComplete)) {
          if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
            AnalysisComplete = false;
            return true;
          }
          auto It = Insns.upper_bound(FieldVA);
          if (It != Insns.begin()) {
            --It;
            if (instructionHasAuthenticatedTargetLoad(It->first,
                                                      AnalysisComplete))
              continue;
            if (!AnalysisComplete)
              return true;
            if (isAuthenticatedStaticSourceRelocation(
                    Slot, FieldVA, Field.TargetOwnerVA, It->first,
                    AnalysisComplete))
              continue;
            if (!AnalysisComplete)
              return true;
          }
          return true;
        }
      if (!AnalysisComplete)
        return true;

      // Relocation-free same-section materializations still carry exact
      // address provenance in LowIR.  Restrict the scan to the final
      // candidate graph so an unreachable textual LEA cannot self-bootstrap
      for (va_t Addr : Reachable) {
        if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
          AnalysisComplete = false;
          return true;
        }
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        if (!consumeCandidateProducts({{It->second.Ops.size(), 2}})) {
          AnalysisComplete = false;
          return true;
        }
        for (const LowOp &Op : It->second.Ops) {
          if (!consumeCandidateProducts(
                  {{static_cast<size_t>(Op.NumInputs) + 1, 12}})) {
            AnalysisComplete = false;
            return true;
          }
          auto IsExactSlotValue = [&](const NdVar &V) {
            return V.isConst() && V.Offset == Slot && !SlotIsStorageBase &&
                   isExactAddressProvenance(V.Provenance);
          };
          bool CarriesExactSlot = IsExactSlotValue(Op.Output);
          for (uint8_t I = 0; !CarriesExactSlot && I < Op.NumInputs; ++I)
            CarriesExactSlot = IsExactSlotValue(Op.Inputs[I]);
          if (!CarriesExactSlot)
            continue;
          if (instructionHasAuthenticatedTargetLoad(Op.Addr, AnalysisComplete))
            continue;
          if (!AnalysisComplete)
            return true;

          // Authentication is relevant only to an occurrence that actually
          // materializes this exact slot.  Querying every LOAD and every
          // operand once per physical table entry turns the audit into
          // slots-times-graph proof work and can exhaust a complete large
          // switch before reaching the only values that could affect this
          // slot's suppression decision.
          if (isAuthenticatedTargetLoad(Op, AnalysisComplete))
            continue;
          if (!AnalysisComplete)
            return true;
          auto IsExactSlot = [&](const NdVar &V) {
            if (!IsExactSlotValue(V))
              return false;
            if (isAuthenticatedStorageConsumer(Op, V, AnalysisComplete))
              return false;
            if (!AnalysisComplete)
              return false;
            if (isAuthenticatedStaticSourceLowOccurrence(Slot, Op, V,
                                                         AnalysisComplete))
              return false;
            if (!AnalysisComplete)
              return false;
            return true;
          };
          if (IsExactSlot(Op.Output))
            return true;
          if (!AnalysisComplete)
            return true;
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (IsExactSlot(Op.Inputs[I]))
              return true;
          if (!AnalysisComplete)
            return true;
        }
      }

      // An occurrence-backed pointer elsewhere in the image is
      // conservatively independent when its source slot is outside this
      // candidate object.  This can only retain evidence; it never grants
      // suppression in the absence of a reachable consumer proof.
      if (!consumeCandidateProducts({{Img.DataPtrRelocSlots.size(), 5}}) ||
          !consumeCandidateFactorProduct(
              {Img.DataPtrRelocSlots.size(), Img.Segments.size(), 2})) {
        AnalysisComplete = false;
        return true;
      }
      for (va_t PointerSlot : Img.DataPtrRelocSlots) {
        if (storageOwns(PointerSlot, AnalysisComplete))
          continue;
        if (!AnalysisComplete)
          return true;
        const uint8_t *P = Img.readVA(PointerSlot, Img.getPointerSize());
        if (P && readPtr(P, Img.is64Bit()) == Slot)
          return true;
      }
      // Value-keyed loader summaries do not identify a consuming
      // instruction and therefore cannot distinguish a reachable use from
      // a relocation in a pruned lexical block.  All occurrence-backed
      // uses in this function were checked above; cross-function vetoes
      // are resolved once every LowFunc is available, where reachability
      // and exact LOAD/escape occurrences can be considered together.
      return false;
    };
    auto collectPotentialIndependentConsumerSlots =
        [&](const std::set<va_t> &Reachable, std::set<va_t> &Potential,
            bool &AnalysisComplete) {
          // Build a conservative value inventory once per refinement round.
          // The detailed predicate below remains authoritative; this set only
          // avoids rescanning the complete reachable graph for table slots
          // whose value never occurs in any independent-consumer source.
          if (!consumeCandidateProducts(
                  {{Img.DataAddressRelocOperands.size(), 3},
                   {Img.CodeAddressRelocOperands.size(), 3},
                   {Reachable.size(), 1},
                   {Img.DataPtrRelocSlots.size(), 1}})) {
            AnalysisComplete = false;
            return;
          }

          auto AddPotential = [&](va_t Address) {
            // Charge the ordered lookup, possible node allocation, and
            // eventual cleanup before changing the candidate-local set.
            constexpr size_t NodeAndCleanupWork = 2;
            if (!consumeCandidateEvidence(orderedLookupWork(Potential.size()) +
                                          NodeAndCleanupWork)) {
              AnalysisComplete = false;
              return false;
            }
            Potential.insert(Address);
            return true;
          };

          for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands) {
            (void)FieldVA;
            if (!AddPotential(Field.TargetVA))
              return;
          }
          for (const auto &[FieldVA, Field] : Img.CodeAddressRelocOperands) {
            (void)FieldVA;
            if (!AddPotential(Field.TargetVA))
              return;
          }

          for (va_t Addr : Reachable) {
            if (!consumeCandidateEvidence(orderedLookupWork(Insns.size()))) {
              AnalysisComplete = false;
              return;
            }
            const auto It = Insns.find(Addr);
            if (It == Insns.end())
              continue;
            if (!consumeCandidateProducts({{It->second.Ops.size(), 2}})) {
              AnalysisComplete = false;
              return;
            }
            for (const LowOp &Op : It->second.Ops) {
              if (!consumeCandidateProducts(
                      {{static_cast<size_t>(Op.NumInputs) + 1, 12}})) {
                AnalysisComplete = false;
                return;
              }
              auto AddExactAddress = [&](const NdVar &Value) {
                if (!Value.isConst() ||
                    !isExactAddressProvenance(Value.Provenance))
                  return true;
                return AddPotential(Value.Offset);
              };
              if (!AddExactAddress(Op.Output))
                return;
              for (uint8_t I = 0; I < Op.NumInputs; ++I)
                if (!AddExactAddress(Op.Inputs[I]))
                  return;
            }
          }

          if (!consumeCandidateProducts({{Img.DataPtrRelocSlots.size(), 5}}) ||
              !consumeCandidateFactorProduct(
                  {Img.DataPtrRelocSlots.size(), Img.Segments.size(), 2})) {
            AnalysisComplete = false;
            return;
          }
          for (va_t PointerSlot : Img.DataPtrRelocSlots) {
            const uint8_t *P = Img.readVA(PointerSlot, Img.getPointerSize());
            if (P && !AddPotential(readPtr(P, Img.is64Bit())))
              return;
          }
        };

    if (!consumeCandidateProducts({{PhysicalRelocationSlots.size(), 2}}))
      return {};
    std::vector<va_t> Allowlist = PhysicalRelocationSlots;
    if (ProtectedJumpTableRelocationSlots) {
      const size_t ProtectedLookupWork =
          orderedLookupWork(ProtectedJumpTableRelocationSlots->size());
      if (!consumeCandidateProducts(
              {{Allowlist.size(), ProtectedLookupWork + 1}}))
        return {};
      Allowlist.erase(
          std::remove_if(Allowlist.begin(), Allowlist.end(),
                         [&](va_t Slot) {
                           return ProtectedJumpTableRelocationSlots->count(
                               Slot);
                         }),
          Allowlist.end());
    }
    bool Stable = false;
    for (size_t Iteration = 0; Iteration <= PhysicalRelocationSlots.size();
         ++Iteration) {
      if (!consumeCandidateProducts({{Allowlist.size(), 2}}))
        return {};
      Info.SuppressibleRelocationSlots = Allowlist;
      std::optional<std::set<va_t>> Roots = budgetedJumpTableProofRoots(Info);
      if (!Roots)
        return {};
      ActiveJumpTableProofRoots = std::move(*Roots);
      bool ReachabilityComplete = false;
      const std::set<va_t> Reachable = candidateReachableInstructions(
          Rec, Targets, *ActiveJumpTableProofRoots, ConsumerAuditStorageRanges,
          &CandidateEvidenceBudget, &ReachabilityComplete);
      if (!ReachabilityComplete) {
        CandidateEvidenceAnalysisIncomplete = true;
        return {};
      }
      if (!Reachable.count(CurrentFuncEntry))
        return {};

      bool ConsumerAnalysisComplete = true;
      const bool WholeObjectEscapes =
          objectAddressEscapes(Reachable, ConsumerAnalysisComplete);
      std::set<va_t> DirectReadSlots;
      bool AmbiguousObjectLoad = false;
      reachableIndependentLoads(Reachable, ConsumerAnalysisComplete,
                                DirectReadSlots, AmbiguousObjectLoad);
      if (!ConsumerAnalysisComplete) {
        CandidateEvidenceAnalysisIncomplete = true;
        return {};
      }

      const std::optional<size_t> RefinementWork =
          detail::relocationAllowlistRefinementWork(
              Allowlist.size(), orderedLookupWork(DirectReadSlots.size()));
      if (!consumeCandidateEvidence(
              RefinementWork.value_or(std::numeric_limits<size_t>::max())))
        return {};
      std::vector<va_t> Refined;
      Refined.reserve(Allowlist.size());
      std::set<va_t> PotentialIndependentConsumerSlots;
      bool PotentialSlotsCollected = false;
      // The motivating regression is the i386 code-relative big table.  Keep
      // the established proof trajectory on other architectures until their
      // distinct table encodings have dedicated structural coverage for this
      // optimization.
      const bool UsePotentialConsumerPrefilter = Img.Arch == Arch::X86;
      for (va_t Slot : Allowlist) {
        const bool NeedsDetailedAudit = !WholeObjectEscapes &&
                                        !AmbiguousObjectLoad &&
                                        !DirectReadSlots.count(Slot);
        bool Independent = false;
        if (NeedsDetailedAudit) {
          if (UsePotentialConsumerPrefilter && !PotentialSlotsCollected) {
            PotentialSlotsCollected = true;
            collectPotentialIndependentConsumerSlots(
                Reachable, PotentialIndependentConsumerSlots,
                ConsumerAnalysisComplete);
            if (!ConsumerAnalysisComplete) {
              CandidateEvidenceAnalysisIncomplete = true;
              return {};
            }
          }
          bool MayHaveIndependentConsumer = true;
          if (UsePotentialConsumerPrefilter) {
            if (!consumeCandidateEvidence(orderedLookupWork(
                    PotentialIndependentConsumerSlots.size()))) {
              CandidateEvidenceAnalysisIncomplete = true;
              return {};
            }
            MayHaveIndependentConsumer =
                PotentialIndependentConsumerSlots.count(Slot) != 0;
          }
          if (MayHaveIndependentConsumer)
            Independent = hasReachableIndependentConsumer(
                Slot, Reachable, /*WholeObjectEscapes=*/false,
                ConsumerAnalysisComplete);
        }
        if (NeedsDetailedAudit && !Independent)
          Refined.push_back(Slot);
        if (!ConsumerAnalysisComplete) {
          CandidateEvidenceAnalysisIncomplete = true;
          return {};
        }
      }
      if (Refined == Allowlist) {
        Stable = true;
        break;
      }
      Allowlist = std::move(Refined);
    }
    if (!Stable)
      return {};
    Info.SuppressibleRelocationSlots = std::move(Allowlist);
    // Consumer-audit proof context is needed only while the final candidate
    // actually suppresses relocation roots.  If the fixed point retains every
    // physical slot, publish against the ordinary full-root context; forcing
    // the audit-only i386 proposal context here can reject an otherwise exact
    // table even though this candidate grants no suppression permission.
    ActiveJumpTableConsumerAudit = !Info.SuppressibleRelocationSlots.empty();

    // A newly preserved relocation target is a real proof root.  Re-run both
    // occurrence certificates with the final allowlist before publishing.
    std::optional<std::set<va_t>> FinalRoots =
        budgetedJumpTableProofRoots(Info);
    if (!FinalRoots)
      return {};
    ActiveJumpTableProofRoots = std::move(*FinalRoots);
    const std::set<va_t> &FinalProofRoots = *ActiveJumpTableProofRoots;
    // Address-role replay may prune TargetLoads when the final proof graph
    // exposes a different reachable-role subset.  Run it before comparing or
    // revalidating any certificate, because it also publishes the corresponding
    // index occurrence metadata transactionally.
    bool FinalAddressRoleComplete = false;
    bool FinalAddressRoleDomainInputsChanged = false;
    const bool FinalAddressRole = tableLoadAddressesMatchRole(
        Info, &CandidateEvidenceBudget, &FinalAddressRoleComplete,
        TargetRoleUsesDefinedOccurrenceRoots, TargetRoleEdgeOverrides,
        &FinalAddressRoleDomainInputsChanged);
    CandidateEvidenceAnalysisIncomplete |= !FinalAddressRoleComplete;
    if (!FinalAddressRole || !FinalAddressRoleComplete)
      return {};
    const std::optional<size_t> RootComparisonWork =
        detail::proofRootSetComparisonWork(FinalProofRoots.size(),
                                           PreReadValidatedRoots.size());
    if (!consumeCandidateEvidence(
            RootComparisonWork.value_or(std::numeric_limits<size_t>::max())))
      return {};
    bool ProofRootsUnchanged =
        FinalProofRoots.size() == PreReadValidatedRoots.size();
    auto FinalRoot = FinalProofRoots.begin();
    auto PreReadRoot = PreReadValidatedRoots.begin();
    while (FinalRoot != FinalProofRoots.end() &&
           PreReadRoot != PreReadValidatedRoots.end()) {
      ProofRootsUnchanged &= *FinalRoot == *PreReadRoot;
      ++FinalRoot;
      ++PreReadRoot;
    }
    const bool DomainInputsUnchanged =
        !FinalAddressRoleDomainInputsChanged && ProofRootsUnchanged;
    const bool FinalIndexDomain =
        DomainInputsUnchanged || RevalidateIndexDomain();
    const std::optional<bool> FinalRevalidationGraphGrowth =
        SuspendForPendingGraphGrowth();
    if (!FinalRevalidationGraphGrowth || *FinalRevalidationGraphGrowth)
      return {};
    if (!FinalIndexDomain)
      return {};
    const bool FinalTargetRoleCurrent = EnsureTargetRoleProofCurrent();
    if (!FinalTargetRoleCurrent)
      return {};
  } else {
    // This fixed point grants permission to suppress independent code-pointer
    // relocation roots.  With no such permission there is nothing to refine:
    // auditing unrelated data-address consumers can only reject an already
    // authenticated relative/compact table and cannot change any output.
    Info.SuppressibleRelocationSlots.clear();
  }

  // A durable relative certificate exists to close a distinct same-table
  // consumer, not to let an ordinary table prove itself forever.  Mint it only
  // when the preceding immutable universe contains a raw sibling with the
  // exact same physical storage.  Once minted, allow that branch to retain its
  // own durable certificate: the raw sibling may have retired in the meantime,
  // while the consumer still needs one more immutable round.  The final block
  // below still requires either exact runtime storage or a shape-checked self
  // replay, plus published targets and final CFG nodes, so this sticky path
  // grants no new edge or storage fact.
  // Retain a separate edge-only certificate only after the final selector,
  // target, address and CFG replays have all succeeded.  The runtime slots
  // must describe exactly one dense 0..N-1 range and every slot in that runtime
  // prefix must be covered by the materialized storage.  An enclosing physical
  // run may contain a following table; the certificate does not claim those
  // extra slots.  Sparse or partial domains publish no such certificate.  This
  // never upgrades the proposal to strong storage ownership and never
  // authorizes relocation-root suppression.
  if (CandidateProposalStageActive && ProvisionalRelativeEdgeTemplate &&
      (HasDistinctSameStorageSiblingDemand ||
       RetainPriorDurableSelfCertificate) &&
      JumpTableProofContextComplete && Info.IndexDomainAuthenticated &&
      Targets.size() >= limits::kMinJumpTableEntries &&
      Targets.size() <= limits::kMaxJumpTableEntries &&
      Info.RuntimeSlotIndices.size() == Targets.size()) {
    bool PublishedTargetsMatch = true;
    if (!Rec.JumpTableTargets.empty()) {
      const std::optional<size_t> PublishedTargetComparisonWork =
          detail::scalarVectorComparisonWork(Rec.JumpTableTargets.size(),
                                             Targets.size());
      if (!PublishedTargetComparisonWork ||
          !consumeCandidateEvidence(*PublishedTargetComparisonWork))
        return {};
      PublishedTargetsMatch = Rec.JumpTableTargets.size() == Targets.size();
      for (size_t I = 0;
           I < std::min(Rec.JumpTableTargets.size(), Targets.size()); ++I)
        PublishedTargetsMatch &= Rec.JumpTableTargets[I] == Targets[I];
    }
    if ((!Rec.JumpTableTargets.empty() && !PublishedTargetsMatch) ||
        !consumeCandidateProducts({{Info.RuntimeSlotIndices.size(), 1},
                                   {Info.StorageRanges.size(), 4},
                                   {Targets.size(), 3}}) ||
        !consumeCandidateEvidence(12))
      return {};
    const JumpTableStorageRange CompleteRuntimeStorage{
        Info.BaseAddr, Info.EntrySize, PhysicalEntryStride, Targets.size()};
    bool ExactRuntimeStorage =
        Info.HasBaseAddr &&
        detail::isDenseRelativeRuntimeCertificatePayload(
            CompleteRuntimeStorage, Info.RuntimeSlotIndices, Targets);
    if (ExactRuntimeStorage && Info.StorageRanges.size() == 1) {
      const JumpTableStorageRange &Materialized = Info.StorageRanges.front();
      ExactRuntimeStorage &=
          Materialized.BaseAddr == CompleteRuntimeStorage.BaseAddr;
      ExactRuntimeStorage &=
          Materialized.EntrySize == CompleteRuntimeStorage.EntrySize;
      ExactRuntimeStorage &=
          Materialized.EntryStride == CompleteRuntimeStorage.EntryStride;
      ExactRuntimeStorage &= Materialized.PhysicalSlotCount >=
                             CompleteRuntimeStorage.PhysicalSlotCount;
    } else if (ExactRuntimeStorage &&
               Info.StorageRanges.size() == Targets.size()) {
      for (size_t I = 0; ExactRuntimeStorage && I < Targets.size(); ++I) {
        if (I != 0 && PhysicalEntryStride > (InvalidVA - Info.BaseAddr) / I) {
          ExactRuntimeStorage = false;
          break;
        }
        const va_t Slot = Info.BaseAddr + uint64_t(I) * PhysicalEntryStride;
        ExactRuntimeStorage =
            Info.StorageRanges[I] ==
            JumpTableStorageRange{Slot, Info.EntrySize, Info.EntrySize, 1};
      }
    } else {
      ExactRuntimeStorage = false;
    }
    if (ExactRuntimeStorage ||
        CurrentCandidateReplayedOwnProvisionalRelativeEdge) {
      if (!consumeCandidateProducts(
              {{Targets.size(), 1},
               {Targets.size(), orderedLookupWork(ExploredAddrs.size())},
               {Targets.size(), orderedLookupWork(BlockStarts.size())},
               {Targets.size(), orderedLookupWork(Insns.size())}}))
        return {};
      // Every materialized replacement above has already passed the prepaid
      // target-ownership/sanity inventory.  This final replay only verifies
      // that each target is now a concrete node in all three resolver maps.
      const bool TargetsAreFinalCFGNodes =
          std::all_of(Targets.begin(), Targets.end(), [&](va_t Target) {
            return ExploredAddrs.count(Target) && BlockStarts.count(Target) &&
                   Insns.count(Target);
          });
      if (TargetsAreFinalCFGNodes) {
        if (ExactRuntimeStorage) {
          if (!RecordProvisionalRelativeEdges(
                  Targets, &CompleteRuntimeStorage,
                  /*CompleteDenseRuntimeCoordinates=*/true))
            return {};
        } else if (!RecordProvisionalRelativeEdges(
                       Targets, /*CompleteRuntimeStorage=*/nullptr,
                       /*CompleteDenseRuntimeCoordinates=*/false,
                       /*StablePublishedTargetReplay=*/true)) {
          return {};
        }
      }
    }
  }

  // Carry the kept slot indices so recoverCaseLabels assigns case values by the
  // real table index (a bounded sparse table skips don't-care slots).  Only
  // useful when the kept indices are *not* the trivial 0..N-1 (a gap exists);
  // an empty vector leaves the positional labelling unchanged.
  {
    const std::optional<size_t> GapComparisonWork =
        detail::denseSlotIndexComparisonWork(KeptIdx.size());
    if (!GapComparisonWork || !consumeCandidateEvidence(*GapComparisonWork))
      return {};
    bool HasGap = KeptIdx.size() != Targets.size();
    for (size_t I = 0; I < KeptIdx.size(); ++I)
      HasGap |= KeptIdx[I] != I;
    Info.EntryIndices = HasGap ? std::move(KeptIdx) : std::vector<uint32_t>{};
  }
  if (CandidateProposalStageActive && CandidateStrongProposalRecorded) {
    if (!consumeCandidateProducts(
            {{Info.SuppressibleRelocationSlots.size(), 2}}) ||
        !consumeCandidateEvidence(
            orderedLookupWork(NextStrongJumpTableProposals.size())))
      return {};
    auto Proposal = NextStrongJumpTableProposals.find(Rec.Addr);
    if (Proposal == NextStrongJumpTableProposals.end()) {
      CandidateEvidenceAnalysisIncomplete = true;
      return {};
    }
    Proposal->second.SuppressibleRelocationSlots =
        Info.SuppressibleRelocationSlots;
  }
  if (!consumeCandidateEvidence(Info.LoadRoles.size()))
    return {};
  const bool HasLiteralDispatchRole = std::any_of(
      Info.LoadRoles.begin(), Info.LoadRoles.end(),
      [](const JumpTableLoadRole &Role) { return Role.IsLiteralCoordinate; });
  Info.UseSharedDispatchSelector = HasLiteralDispatchRole;
  Info.RequiresCompleteCFGProof = RequestedCompleteJumpTableProof;
  if (!consumeCandidateEvidence(
          orderedLookupWork(StackTableEvidenceIncompleteBranches.size())) ||
      !reserveResolvedInfoMaterialization(Info) ||
      !consumeCandidateEvidence(orderedLookupWork(ResolvedTableInfo.size()) +
                                1))
    return {};
  // An alternate stack-materialized-table probe can exhaust while this same
  // branch is independently authenticated and published by an exact
  // relocation/role/domain proof.  The marker is stage-local safety state:
  // retire it only at the final mutation boundary, after every later failure
  // path has been prepaid.  Proposal rollback still restores any previously
  // committed marker from the saved transaction snapshot.
  StackTableEvidenceIncompleteBranches.erase(Rec.Addr);
  ResolvedTableInfo[Rec.Addr] = Info;

  LLVM_DEBUG({
    llvm::dbgs() << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
                 << Targets.size() << " entries, base=0x"
                 << llvm::utohexstr(Info.BaseAddr)
                 << ", entrySize=" << Info.EntrySize
                 << (Info.IsRelative ? " (relative" : " (absolute")
                 << (Info.IsSigned ? ", signed)" : ")") << "\n";
  });

  CandidateEvidencePublished = true;
  return Targets;
}

} // namespace neverd
