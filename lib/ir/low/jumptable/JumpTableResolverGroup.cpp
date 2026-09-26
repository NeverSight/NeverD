//===- JumpTableResolverGroup.cpp - Joint dense-guard table proofs
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace neverd {
namespace {

// All work in the joint proof, including preparation and retirement of its
// private copies, belongs to the current immutable proposal stage. Overflow
// has exactly the same incomplete outcome as an exhausted allowance.
class GroupEvidence {
  size_t &Remaining;
  bool &Incomplete;

public:
  GroupEvidence(size_t &Remaining, bool &Incomplete)
      : Remaining(Remaining), Incomplete(Incomplete) {}
  bool take(size_t Work) {
    if (Work > Remaining) {
      Remaining = 0;
      Incomplete = true;
      return false;
    }
    Remaining -= Work;
    return true;
  }
  bool fail() {
    Remaining = 0;
    Incomplete = true;
    return false;
  }
  bool products(std::initializer_list<std::pair<size_t, size_t>> Terms) {
    size_t Work = 0;
    for (auto [Count, Cost] : Terms)
      if (!detail::addLinearComparisonWork(Work, Count, Cost))
        return fail();
    return take(Work);
  }
};

size_t lookupWork(size_t Count) {
  size_t Work = 1;
  for (; Count > 1; Count = Count / 2 + Count % 2)
    ++Work;
  return Work;
}

struct FiniteGOTOFFClaimShape {
  va_t LoadInsn = InvalidVA;
  va_t Base = InvalidVA;
};

// This only identifies an opaque branch for a failed *group* proof. It does
// not authenticate any table target. Follow the branch's actual value through
// the indexed LOAD and require the same unmodified GOT register at both adds;
// a nearby GOTOFF operand cannot turn a callback into a table consumer.
std::optional<FiniteGOTOFFClaimShape>
finiteGOTOFFClaimShape(const std::vector<LowOp> &Ops, va_t Branch,
                       va_t OwnerBegin, va_t OwnerEnd) {
  auto overlaps = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Size && B.Size &&
           A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
  };
  auto def = [&](int Before, const NdVar &Value) {
    if ((!Value.isReg() && !Value.isTemp()) || Value.Size == 0)
      return -2;
    for (int I = Before; I >= 0; --I) {
      const NdVar &Output = Ops[I].Output;
      if (overlaps(Output, Value))
        return Output.Space == Value.Space && Output.Offset == Value.Offset &&
                       Output.Size == Value.Size
                   ? I
                   : -2;
    }
    return -1;
  };
  auto peel = [&](NdVar Value, int Before, bool AddressTransport) {
    for (unsigned Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      const int D = def(Before, Value);
      if (D < 0)
        return D;
      const LowOp &Op = Ops[D];
      const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                        Op.Inputs[0].Size == Value.Size;
      const bool Zext = AddressTransport && Op.Opcode == NdOp::INT_ZEXT &&
                        Op.NumInputs == 1 && Op.Inputs[0].Size == 4 &&
                        Value.Size == 8;
      if (!Copy && !Zext)
        return D;
      Value = Op.Inputs[0];
      Before = D - 1;
    }
    return -2;
  };
  auto sourceRegister = [&](NdVar Value, int Before,
                            int *CaptureIdx = nullptr) -> std::optional<NdVar> {
    for (unsigned Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (Value.isReg() && Value.Size == 4)
        return Value;
      if (!Value.isTemp() || Value.Size != 4)
        return std::nullopt;
      const int D = def(Before, Value);
      if (D < 0 || Ops[D].Opcode != NdOp::COPY || Ops[D].NumInputs != 1 ||
          Ops[D].Inputs[0].Size != 4)
        return std::nullopt;
      if (CaptureIdx)
        *CaptureIdx = D;
      Value = Ops[D].Inputs[0];
      Before = D - 1;
    }
    return std::nullopt;
  };
  int BranchIdx = -1;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I)
    if (Ops[I].Addr == Branch && Ops[I].Opcode == NdOp::INDIR_BR &&
        Ops[I].NumInputs == 1 && Ops[I].Inputs[0].Size == 4) {
      if (BranchIdx >= 0)
        return std::nullopt;
      BranchIdx = I;
    }
  if (BranchIdx < 0)
    return std::nullopt;
  const int TargetIdx = peel(Ops[BranchIdx].Inputs[0], BranchIdx - 1, false);
  if (TargetIdx < 0 || Ops[TargetIdx].Opcode != NdOp::INT_ADD ||
      Ops[TargetIdx].NumInputs != 2 || Ops[TargetIdx].Output.Size != 4)
    return std::nullopt;
  const LowOp &TargetAdd = Ops[TargetIdx];
  std::optional<FiniteGOTOFFClaimShape> Found;
  for (int LoadSide = 0; LoadSide < 2; ++LoadSide) {
    const int LoadIdx = peel(TargetAdd.Inputs[LoadSide], TargetIdx - 1, false);
    if (LoadIdx < 0 || Ops[LoadIdx].Opcode != NdOp::LOAD ||
        Ops[LoadIdx].Output.Size != 4 || Ops[LoadIdx].NumInputs < 1 ||
        Ops[LoadIdx].NumInputs > 2)
      continue;
    const auto GOT =
        sourceRegister(TargetAdd.Inputs[1 - LoadSide], TargetIdx - 1);
    if (!GOT)
      continue;
    const LowOp &Load = Ops[LoadIdx];
    const int OuterIdx =
        peel(Load.Inputs[Load.NumInputs - 1], LoadIdx - 1, true);
    if (OuterIdx < 0 || Ops[OuterIdx].Opcode != NdOp::INT_ADD ||
        Ops[OuterIdx].NumInputs != 2 || Ops[OuterIdx].Output.Size != 4 ||
        Ops[OuterIdx].Addr != Load.Addr)
      continue;
    const LowOp &Outer = Ops[OuterIdx];
    for (int DispSide = 0; DispSide < 2; ++DispSide) {
      const NdVar &Disp = Outer.Inputs[DispSide];
      if (!Disp.isConst() || Disp.Size != 4 ||
          (Disp.Offset < OwnerBegin || Disp.Offset >= OwnerEnd ||
           (Disp.Offset - OwnerBegin) % 4 != 0))
        continue;
      const int InnerIdx =
          peel(Outer.Inputs[1 - DispSide], OuterIdx - 1, false);
      if (InnerIdx < 0 || Ops[InnerIdx].Opcode != NdOp::INT_ADD ||
          Ops[InnerIdx].NumInputs != 2 || Ops[InnerIdx].Output.Size != 4 ||
          Ops[InnerIdx].Addr != Load.Addr)
        continue;
      const LowOp &Inner = Ops[InnerIdx];
      for (int IndexSide = 0; IndexSide < 2; ++IndexSide) {
        int GOTCapture = -1;
        const auto AddressGOT = sourceRegister(Inner.Inputs[1 - IndexSide],
                                               InnerIdx - 1, &GOTCapture);
        if (!AddressGOT || *AddressGOT != *GOT)
          continue;
        const int ScaleIdx = peel(Inner.Inputs[IndexSide], InnerIdx - 1, false);
        if (ScaleIdx < 0 || Ops[ScaleIdx].Opcode != NdOp::INT_MULT ||
            Ops[ScaleIdx].NumInputs != 2 || Ops[ScaleIdx].Output.Size != 4 ||
            Ops[ScaleIdx].Addr != Load.Addr)
          continue;
        const LowOp &Scale = Ops[ScaleIdx];
        const bool ScaleFour =
            (Scale.Inputs[0].isConst() && Scale.Inputs[0].Size == 4 &&
             Scale.Inputs[0].Offset == 4 && Scale.Inputs[1].Size == 4 &&
             (Scale.Inputs[1].isReg() || Scale.Inputs[1].isTemp())) ||
            (Scale.Inputs[1].isConst() && Scale.Inputs[1].Size == 4 &&
             Scale.Inputs[1].Offset == 4 && Scale.Inputs[0].Size == 4 &&
             (Scale.Inputs[0].isReg() || Scale.Inputs[0].isTemp()));
        if (!ScaleFour)
          continue;
        // A write to the GOT lane after the indexed address was formed would
        // make the target add use a different value despite matching registers.
        bool GOTOverwritten = false;
        for (int I = (GOTCapture >= 0 ? GOTCapture : InnerIdx) + 1;
             I < TargetIdx; ++I)
          GOTOverwritten |= overlaps(Ops[I].Output, *GOT);
        if (GOTOverwritten || Found)
          return std::nullopt;
        Found = FiniteGOTOFFClaimShape{Load.Addr, Disp.Offset};
      }
    }
  }
  return Found;
}

} // namespace

uint32_t CFGBuilder::proveGroupDenseMaskBound(const InsnRecord &Rec,
                                              const JumpTableInfo &Info,
                                              size_t *AggregateEvidenceBudget,
                                              bool &Incomplete) {
  auto &Observation = JumpTableGroupLifecycleForTesting.LastProof;
  Observation.MaskBound = 0;
  Observation.MaskQueryIssued = false;
  Observation.MaskQueryComplete = false;
  Observation.MaskQueryMatched = false;
  if (!AggregateEvidenceBudget) {
    Incomplete = true;
    return 0;
  }
  GroupEvidence Budget(*AggregateEvidenceBudget, Incomplete);
  if (!GuardedGroupProofContext || Info.IndexValueDefinedAtUse ||
      Info.IndexValueAtUse.Size != 4 || Info.IndexUseAddr == InvalidVA ||
      Info.IndexUseSeq < 0 || Info.PreScaledIndex || Info.Stride != 1 ||
      Info.NormBase != 0 || Info.NormShift != 0)
    return 0;

  // The mask itself proves an unsigned envelope independently of every CFG
  // hypothesis. Never derive this bound from the table's physical capacity,
  // enumerate feasible coordinates, or accept a merely dependent selector.
  std::map<uint32_t, std::vector<JumpTableValueOccurrence>> Producers;
  if (!Budget.products({{Insns.size(), 1}}))
    return 0;
  for (const auto &[Addr, Insn] : Insns) {
    if (!Budget.products({{Insn.Ops.size(), 16}}))
      return 0;
    if (Insn.IsInstructionGuard)
      continue;
    for (const LowOp &Op : Insn.Ops) {
      if (Op.Opcode != NdOp::INT_AND || Op.NumInputs != 2 ||
          Op.Output.Size != 4 || (!Op.Output.isReg() && !Op.Output.isTemp()) ||
          Op.Addr != Addr || Op.Seq < 0 || Op.Inputs[0].Size != 4 ||
          Op.Inputs[1].Size != 4 ||
          Op.Inputs[0].isConst() == Op.Inputs[1].isConst())
        continue;
      const NdVar &Mask = Op.Inputs[Op.Inputs[0].isConst() ? 0 : 1];
      if ((Mask.Provenance != ConstantAddressProvenance::Unknown &&
           Mask.Provenance != ConstantAddressProvenance::Scalar) ||
          Mask.Offset == 0 || Mask.Offset >= limits::kMaxJumpTableEntries ||
          (Mask.Offset & (Mask.Offset + 1)) != 0)
        continue;
      const auto Bound = static_cast<uint32_t>(Mask.Offset + 1);
      // Bound groups, exact occurrence payloads, vector growth/copies, and
      // eventual destruction all debit the caller's unchanged aggregate.
      if (!Budget.take(32 + lookupWork(Producers.size())))
        return 0;
      Producers[Bound].push_back(
          {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true});
    }
  }
  if (!Budget.products({{Producers.size(), 1}}))
    return 0;
  for (const auto &[Bound, Alternatives] : Producers) {
    if (!Budget.products({{1, 16}, {Alternatives.size(), 32}}))
      return 0;
    JumpTableValueQuery Query;
    Query.Candidate = Info.IndexValueAtUse;
    Query.UseAddr = Info.IndexUseAddr;
    Query.UseSeq = Info.IndexUseSeq;
    Query.Alternatives = Alternatives;
    Query.Relation = JumpTableValueRelation::MustEqual;
    Query.UseDefinedAlternativesAsOccurrenceRoots = true;
    // Equal width is intentional: a partial overwrite or a wider selector
    // cannot borrow an AND certificate for another architectural lane.
    bool Complete = false;
    std::vector<bool> QueryComplete;
    Observation.MaskBound = Bound;
    Observation.MaskQueryIssued = true;
    const auto Matches = tableValuesMatchAtUses(
        {Query}, &Complete, &QueryComplete, Rec.Addr,
        /*CandidateTargetsOverride=*/nullptr, AggregateEvidenceBudget,
        limits::kMaxJumpTableMaskMatchEvidenceWork,
        /*CandidateBranchesSharingTargets=*/nullptr,
        /*QueryUnsignedFeasibleMasks=*/nullptr,
        limits::kMaxJumpTableLargeExpressionRoleResolverDepth,
        &GuardedGroupProofContext->Edges);
    Observation.MaskQueryComplete = Complete && Matches.size() == 1 &&
                                    QueryComplete.size() == 1 &&
                                    QueryComplete.front();
    Observation.MaskQueryMatched = Matches.size() == 1 && Matches.front();
    if (!Complete || Matches.size() != 1 || QueryComplete.size() != 1 ||
        !QueryComplete.front()) {
      Incomplete = true;
      return 0;
    }
    if (Matches.front())
      return Bound;
  }
  return 0;
}

bool CFGBuilder::prepayJumpTableInfoCopy(const JumpTableInfo &Info,
                                         size_t Copies) {
  GroupEvidence Budget(CandidateProposalStageEvidenceRemaining,
                       CandidateProposalStageEvidenceIncomplete);
  // Prepay the shape scan before visiting any nested initializer. The fixed
  // bounds include NdVar/occurrence payloads, not merely vector headers.
  if (!Budget.products(
          {{1, 512},
           {Info.LoadRoles.size(), 128},
           {Info.AuthenticatedFrameStorage.Initializers.size(), 32}}))
    return false;
  size_t Work = 512;
  auto Add = [&](size_t Count, size_t Cost = 64) {
    return detail::addLinearComparisonWork(Work, Count, Cost);
  };
  auto Frame = [&](const JumpTableFrameStorageRole &Storage) {
    if (!Add(Storage.Initializers.size(), 128))
      return false;
    for (const auto &Initializer : Storage.Initializers)
      if (!Add(Initializer.StaticSources.size(), 64))
        return false;
    return true;
  };
  if (!Add(Info.AuthenticatedMaskCoordinates.size()) ||
      !Add(Info.AuthenticatedMaskKnownOneWitnesses.size()) ||
      !Add(Info.StorageRanges.size()) ||
      !Add(Info.SuppressibleRelocationSlots.size()) ||
      !Add(Info.IndexValueAlternatives.size()) ||
      !Add(Info.TargetLoads.size()) ||
      !Add(Info.AuthenticatedStorageConsumers.size()) ||
      !Add(Info.LoadRoles.size(), 256) || !Add(Info.EntryIndices.size()) ||
      !Add(Info.RuntimeCaseLabels.size()) ||
      !Add(Info.RuntimeSlotIndices.size()) ||
      !Add(Info.ExplicitTargets.size()) ||
      !Frame(Info.AuthenticatedFrameStorage))
    return Budget.fail();
  for (const auto &Role : Info.LoadRoles) {
    if (!Budget.products({{Role.FrameStorage.Initializers.size(), 32}}))
      return false;
    if (!Add(Role.AllowedBases.size()) || !Add(Role.Indices.size()) ||
        !Frame(Role.FrameStorage))
      return Budget.fail();
  }
  // One traversal to construct and another to destroy every owned copy.
  return Budget.products({{Copies, Work}, {Copies, Work}});
}

bool CFGBuilder::copyGuardedGroupProofSnapshot(
    CFGBuilder &Scratch, const GuardedJumpTableGroupProofContext &Context) {
  GroupEvidence Budget(CandidateProposalStageEvidenceRemaining,
                       CandidateProposalStageEvidenceIncomplete);
  if (!Budget.products({{1, 128},
                        {Insns.size(), 16},
                        {DiscoveredCodeRefSources.size(), 4},
                        {RelocationCFGRootSources.size(), 4},
                        {RelocatedInstructionAddressOccurrences.size(), 8},
                        {Context.Edges.size(), 4}}))
    return false;
  size_t Work = 256;
  auto Add = [&](size_t Count, size_t Cost) {
    return detail::addLinearComparisonWork(Work, Count, Cost);
  };
  // std::map/set copies visit every node; vectors own their elements; DenseSet
  // copies allocated buckets (including tombstones), not only live entries.
  // Each charge includes construction, eventual destruction and a spare visit.
  if (!Add(Insns.size(), 48) || !Add(BlockStarts.size(), 6) ||
      !Add(PublishedBlockStarts.size(), 3) ||
      !Add(PublishedReachableInsns.size(), 6) ||
      !Add(PersistentCFGRoots.size(), 6) || !Add(OrdinaryCFGRoots.size(), 6) ||
      !Add(DurableCFGRoots.size(), 6) ||
      !Add(ExploredAddrs.getMemorySize() / sizeof(va_t), 3) ||
      !Add(DiscoveredCodeRefSources.size(), 8) ||
      !Add(RelocationCFGRootSources.size(), 8) ||
      !Add(RelocatedInstructionAddressOccurrences.size(), 192) ||
      !Add(RelocatedInstructionScalarOperandOccurrences.size(), 96) ||
      !Add(RelocatedInstructionScalarModelOccurrences.size(), 192) ||
      !Add(Context.Roots.size(), 6) || !Add(Context.Edges.size(), 16) ||
      !Add(Context.EmptyEdges.size(), 16))
    return Budget.fail();
  for (const auto &[Addr, Rec] : Insns)
    if (!Add(Rec.Ops.size(), 192) || !Add(Rec.JumpTableTargets.size(), 3))
      return Budget.fail();
  for (const auto &[Target, Sources] : DiscoveredCodeRefSources)
    if (!Add(Sources.size(), 6))
      return Budget.fail();
  for (const auto &[Target, Sources] : RelocationCFGRootSources)
    if (!Add(Sources.size(), 6))
      return Budget.fail();
  for (const auto &Occurrence : RelocatedInstructionAddressOccurrences)
    if (!Add(Occurrence.ArithmeticProof.size(), 96))
      return Budget.fail();
  for (const auto &[Branch, Targets] : Context.Edges)
    if (!Add(Targets.size(), 3))
      return Budget.fail();
  if (!Budget.take(Work))
    return false;

  // This is an input whitelist, deliberately not a builder copy followed by
  // clearing results. No proposed/published sibling certificate, callback
  // classification or proof history enters the fresh member query. Context
  // may borrow complete prior-phase roles for the consumer audit and one
  // complete finite certificate from this same immutable round. Neither can
  // import a result from a later round or another proposal stage.
  Scratch.Insns = Insns;
  Scratch.BlockStarts = BlockStarts;
  Scratch.PublishedBlockStarts = PublishedBlockStarts;
  Scratch.PublishedReachableInsns = PublishedReachableInsns;
  Scratch.PersistentCFGRoots = PersistentCFGRoots;
  Scratch.OrdinaryCFGRoots = OrdinaryCFGRoots;
  Scratch.DurableCFGRoots = DurableCFGRoots;
  Scratch.RelocationCFGRootSources = RelocationCFGRootSources;
  Scratch.DiscoveredCodeRefSources = DiscoveredCodeRefSources;
  Scratch.ExploredAddrs = ExploredAddrs;
  Scratch.RelocatedInstructionAddressOccurrences =
      RelocatedInstructionAddressOccurrences;
  Scratch.RelocatedInstructionScalarOperandOccurrences =
      RelocatedInstructionScalarOperandOccurrences;
  Scratch.RelocatedInstructionScalarModelOccurrences =
      RelocatedInstructionScalarModelOccurrences;
  Scratch.CurrentImg = CurrentImg;
  // This immutable image index carries no candidate or sibling proof state.
  Scratch.ExecutableCodeOwners = ExecutableCodeOwners;
  Scratch.CurrentFuncEntry = CurrentFuncEntry;
  Scratch.CurrentFuncRange = CurrentFuncRange;
  Scratch.AuthoritativeCurrentFuncRange = AuthoritativeCurrentFuncRange;
  Scratch.KnownFuncEntries = KnownFuncEntries;
  Scratch.CrossFunctionContinuationRoots = CrossFunctionContinuationRoots;
  Scratch.ProtectedJumpTableRelocationSlots = ProtectedJumpTableRelocationSlots;
  Scratch.UnsafeJumpTableBranches = UnsafeJumpTableBranches;
  Scratch.I386GOTModelEvidenceIncomplete = I386GOTModelEvidenceIncomplete;
  Scratch.I386GOTModelEvidenceBudgetForTesting =
      I386GOTModelEvidenceBudgetForTesting;
  Scratch.I386GOTOFFProposalEvidenceBudgetForTesting =
      I386GOTOFFProposalEvidenceBudgetForTesting;
  Scratch.StackTableEvidenceBudgetForTesting =
      StackTableEvidenceBudgetForTesting;
  Scratch.MaskFixedPointEvidenceBudgetForTesting =
      MaskFixedPointEvidenceBudgetForTesting;
  Scratch.FiniteSetSymbolEvidenceBudgetForTesting =
      FiniteSetSymbolEvidenceBudgetForTesting;
  if (finiteGOTOFFGroupClaimed()) {
    if (!Budget.take(32))
      return false;
    Scratch.GuardedGroupIdentity = GuardedGroupIdentity;
  }
  Scratch.GuardedGroupProofContext = Context;
  Scratch.CandidateProposalStageActive = true;
  Scratch.CandidateProposalOutcomeTracked = true;
  Scratch.JumpTableProofContextComplete = true;
  Scratch.CandidateProposalStageEvidenceRemaining =
      CandidateProposalStageEvidenceRemaining;
  Scratch.StackTableEvidenceRemaining = StackTableEvidenceRemaining;
  Scratch.IncompleteBranchMarkerEvidenceRemaining =
      IncompleteBranchMarkerEvidenceRemaining;
  return true;
}

bool CFGBuilder::guardedGroupContains(va_t Branch) const {
  const GuardedJumpTableGroupKey *Key = nullptr;
  if (GuardedGroupState && GuardedGroupState->Published)
    Key = &GuardedGroupState->Key;
  else if (finiteGOTOFFGroupClaimed())
    Key = &*GuardedGroupIdentity;
  if (!Key)
    return false;
  return std::find(Key->Members.begin(),
                   Key->Members.begin() + Key->MemberCount,
                   Branch) != Key->Members.begin() + Key->MemberCount;
}

bool CFGBuilder::finiteGOTOFFGroupClaimed() const {
  return GuardedGroupIdentity &&
         GuardedGroupIdentity->Kind ==
             GuardedJumpTableGroupKind::FiniteAdjacentGOTOFF &&
         GuardedGroupIdentity->MemberCount >= 4 &&
         GuardedGroupIdentity->MemberCount <= 8 &&
         GuardedGroupIdentity->MemberCount % 2 == 0;
}

bool CFGBuilder::guardedGroupHasNoLiveMembers() const {
  if (!GuardedGroupState)
    return true;
  const auto &Key = GuardedGroupState->Key;
  for (size_t I = 0; I < Key.MemberCount; ++I) {
    const auto Rec = Insns.find(Key.Members[I]);
    if ((Rec != Insns.end() && !Rec->second.JumpTableTargets.empty()) ||
        ResolvedTableInfo.count(Key.Members[I]))
      return false;
  }
  return true;
}

bool CFGBuilder::guardedGroupIsComplete() const {
  if (!GuardedGroupState || !GuardedGroupState->Published)
    return false;
  const auto &Key = GuardedGroupState->Key;
  for (size_t I = 0; I < Key.MemberCount; ++I) {
    const va_t Branch = Key.Members[I];
    const auto Rec = Insns.find(Branch);
    if (Rec == Insns.end() || Rec->second.JumpTableTargets.empty() ||
        !ResolvedTableInfo.count(Branch))
      return false;
  }
  return true;
}

void CFGBuilder::closeGuardedGroupOwners(std::set<va_t> &Owners) const {
  if (!GuardedGroupState || !GuardedGroupState->Published)
    return;
  const auto &Key = GuardedGroupState->Key;
  bool Complete = guardedGroupIsComplete();
  for (size_t I = 0; I < Key.MemberCount; ++I)
    Complete &= Owners.count(Key.Members[I]) != 0;
  if (!Complete)
    for (size_t I = 0; I < Key.MemberCount; ++I)
      Owners.erase(Key.Members[I]);
}

void CFGBuilder::withdrawGuardedJumpTableGroup(bool Reject) {
  if (!GuardedGroupState)
    return;
  // Publication prepaid all member lookups, live-value destruction and later
  // cached-value destruction. Withdrawal remains possible with zero budget.
  const auto &Key = GuardedGroupState->Key;
  for (size_t I = 0; I < Key.MemberCount; ++I) {
    const va_t Branch = Key.Members[I];
    if (auto Rec = Insns.find(Branch); Rec != Insns.end())
      Rec->second.JumpTableTargets.clear();
    ResolvedTableInfo.erase(Branch);
  }
  GuardedGroupState->Published = false;
  GuardedGroupRejected |= Reject;
}

void CFGBuilder::markGuardedJumpTableGroupIncomplete(
    llvm::ArrayRef<va_t> Members) {
  CandidateProposalStageEvidenceIncomplete = true;
  if (Members.empty() && GuardedGroupIdentity) {
    const auto &Key = *GuardedGroupIdentity;
    Members = llvm::ArrayRef<va_t>(Key.Members.data(), Key.MemberCount);
  }
  if (Members.empty() || Members.size() > 8) {
    IncompleteBranchMarkerEvidenceIncomplete = true;
    return;
  }
  // The function-wide marker account is separate from proof work. Reserve the
  // entire group before the first insertion, so even a failed first member
  // preserves every unresolved member as INDIR_BR after bounded retries.
  const size_t Count = Members.size();
  const size_t Max = std::numeric_limits<size_t>::max();
  if (IndexDomainEvidenceIncompleteBranches.size() > Max - Count) {
    IncompleteBranchMarkerEvidenceIncomplete = true;
    return;
  }
  if (!consumeIncompleteBranchMarkerEvidence(
          Count * (3 + lookupWork(IndexDomainEvidenceIncompleteBranches.size() +
                                  Count))))
    return;
  for (va_t Branch : Members)
    IndexDomainEvidenceIncompleteBranches.insert(Branch);
}

bool CFGBuilder::recoverGuardedJumpTableGroup(const BinaryImage &Img,
                                              LowFunc &Func,
                                              llvm::ArrayRef<va_t> Candidates,
                                              bool &MadeProgress,
                                              bool &MetadataRefreshed) {
  if (!GuardedGroupRejected) {
    JumpTableGroupLifecycleForTesting.LastProof = {};
    JumpTableGroupLifecycleForTesting.LastProof.Stage = "eligibility";
    JumpTableGroupLifecycleForTesting.LastProof.MemberCount = Candidates.size();
  }
  // Cheap, narrow gates precede every image or instruction inventory walk.
  if (GuardedGroupRejected || GuardedGroupProofContext || !Img.IsRelocatable ||
      Img.Format != BinaryFormat::ELF || Img.Arch != Arch::X86 ||
      Img.getPointerSize() != 4 || !AuthoritativeCurrentFuncRange ||
      Img.I386GOTPCFields.empty() ||
      (Img.DataAddressRelocOperands.size() < 2 &&
       (Candidates.size() < 4 || Candidates.size() % 2 != 0)) ||
      Candidates.size() < 2 || Candidates.size() > 8 ||
      !ResolvedTableInfo.empty() || !PriorStrongJumpTableProposals.empty() ||
      !NextStrongJumpTableProposals.empty() ||
      !PriorProvisionalRelativeEdges.empty() ||
      !NextProvisionalRelativeEdges.empty() ||
      !QuarantinedJumpTableProposals.empty() ||
      !EverStrongJumpTableProposalBranches.empty())
    return false;
  JumpTableGroupLifecycleForTesting.LastProof.Stage = "owner-inventory";
  GroupEvidence Budget(CandidateProposalStageEvidenceRemaining,
                       CandidateProposalStageEvidenceIncomplete);
  auto Incomplete = [&]() {
    markGuardedJumpTableGroupIncomplete(Candidates);
    return false;
  };
  if (!Budget.products({{Insns.size(), 1}, {1, 16}}))
    return Incomplete();
  for (const auto &[Addr, Rec] : Insns)
    if (!Rec.JumpTableTargets.empty())
      return false;

  struct OwnerInventory {
    const Section *Storage = nullptr;
    GuardedJumpTableGroupKey Key;
    std::set<va_t> Slots;
    std::vector<va_t> Targets;
    std::set<va_t> Bases;
  };
  std::optional<OwnerInventory> Owner;
  for (const auto &Section : Img.Sections) {
    if (!Budget.take(16))
      return Incomplete();
    if (!Section.isReadable() || Section.isWritable() ||
        Section.isExecutable() || Section.Size < 8 || Section.Size > 256 ||
        Section.Size % 4 != 0 || Section.Size > InvalidVA - Section.VA)
      continue;
    const va_t End = Section.VA + Section.Size;
    const size_t SlotCount = Section.Size / 4;
    GuardedJumpTableGroupKey Key;
    Key.OwnerBegin = Section.VA;
    Key.OwnerEnd = End;
    Key.MemberCount = Candidates.size();
    std::copy(Candidates.begin(), Candidates.end(), Key.Members.begin());
    if (!Budget.products({{Img.Sections.size(), 8},
                          {Img.Segments.size(), 8},
                          {Img.Symbols.size(), 8}}))
      return Incomplete();
    if (Img.mappedObjectOwnerEnd(Section.VA) != std::optional<va_t>(End) ||
        Img.getSectionFor(Section.VA) != &Section ||
        Img.getSectionFor(End - 1) != &Section)
      continue;
    bool Valid = true;
    // Bare labels may partition a section through exact decoded GOTOFF fields.
    // An explicit sized object is stronger: never join across its boundary or
    // accept a group that uses a prefix/suffix of a declared object.
    for (const auto &Symbol : Img.Symbols) {
      if (Symbol.IsFunc || Symbol.Size == 0)
        continue;
      if (Symbol.Size > InvalidVA - Symbol.Addr) {
        if (Symbol.Addr < End)
          Valid = false;
        continue;
      }
      if (Symbol.Addr < End && Symbol.Addr + Symbol.Size > Section.VA &&
          (Symbol.Addr != Section.VA || Symbol.Size != Section.Size))
        Valid = false;
    }
    if (!Valid)
      continue;
    if (Candidates.size() >= 4 && Candidates.size() % 2 == 0) {
      // ELF ET_REL keeps RelocationEntry::Address as the encoded r_offset,
      // local to the relocation section's target. Bind that raw field to one
      // uniquely named executable section before comparing it with a decoded
      // LOAD instruction. This mapping supplies only a negative witness.
      if (!Budget.products({{Img.Sections.size(), 2}}))
        return Incomplete();
      const auto *CodeSection = Img.getSectionFor(CurrentFuncEntry);
      if (!CodeSection || !CodeSection->isExecutable())
        continue;
      size_t NameComparisonWork = 4;
      if (!detail::addLinearComparisonWork(NameComparisonWork,
                                           CodeSection->Name.size(), 2) ||
          !Budget.products({{Img.Sections.size(), NameComparisonWork}}))
        return Incomplete();
      size_t SameNamedCodeSections = 0;
      for (const auto &Other : Img.Sections)
        SameNamedCodeSections += Other.Name == CodeSection->Name;
      if (SameNamedCodeSections != 1)
        continue;
      const std::string RelName = ".rel" + CodeSection->Name;
      const std::string RelaName = ".rela" + CodeSection->Name;
      auto MappedRawField =
          [&](const RelocationEntry &Reloc) -> std::optional<va_t> {
        if (Reloc.Type != llvm::ELF::R_386_GOTOFF ||
            (Reloc.SectionName != RelName && Reloc.SectionName != RelaName) ||
            Reloc.Address >= CodeSection->Size ||
            CodeSection->Size - Reloc.Address < 4 ||
            Reloc.Address > InvalidVA - CodeSection->VA)
          return std::nullopt;
        return CodeSection->VA + Reloc.Address;
      };
      // Recognize the finite joint owner before validating every physical
      // pointer. A missing run relocation vetoes the group; no intact
      // run may recover alone after this inventory rejects the owner.
      size_t PerRawWork = 32 + lookupWork(Img.Relocations.size());
      if (!detail::addLinearComparisonWork(PerRawWork, Img.Segments.size(),
                                           4) ||
          !detail::addLinearComparisonWork(PerRawWork, Img.Sections.size(),
                                           4) ||
          !detail::addLinearComparisonWork(PerRawWork, CodeSection->Name.size(),
                                           4) ||
          !Budget.products({{Img.Relocations.size(), PerRawWork},
                            {Img.Symbols.size(), 4},
                            {Img.Sections.size(), 4},
                            {1, 32}}))
        return Incomplete();
      // Only the actual indexed LOAD of each branch nominates a base.
      // Unrelated raw references cannot add a partition or erase the negative
      // identity when normalized anchors are damaged or missing.
      // An adjacent array of function pointers has the same indexed
      // GOTOFF dataflow but is a legitimate tail-call source. Require
      // every readable slot to identify an interior instruction of this
      // function before claiming the negative group identity. A missing
      // relocation *slot* still has bytes and remains covered here.
      size_t OwnerLookupWork = 128;
      if (!detail::addLinearComparisonWork(OwnerLookupWork, Img.Segments.size(),
                                           16) ||
          !detail::addLinearComparisonWork(OwnerLookupWork, Img.Sections.size(),
                                           16) ||
          !detail::addLinearComparisonWork(OwnerLookupWork, Img.Symbols.size(),
                                           16) ||
          !detail::addLinearComparisonWork(OwnerLookupWork,
                                           Img.KnownCodeRanges.size(), 8) ||
          !detail::addLinearComparisonWork(OwnerLookupWork,
                                           Img.ImportStubRanges.size(), 8) ||
          !detail::addLinearComparisonWork(OwnerLookupWork, Img.Imports.size(),
                                           8) ||
          !detail::addLinearComparisonWork(OwnerLookupWork, Img.Exports.size(),
                                           8) ||
          !detail::addLinearComparisonWork(OwnerLookupWork,
                                           knownFunctionEntryCount(), 8) ||
          !detail::addLinearComparisonWork(OwnerLookupWork, Insns.size(), 1) ||
          !Budget.products({{SlotCount, OwnerLookupWork}}))
        return Incomplete();
      bool InteriorTargets = true;
      for (va_t Slot = Section.VA; Slot < End; Slot += 4) {
        const uint8_t *Bytes = Img.readVA(Slot, 4);
        if (!Bytes) {
          InteriorTargets = false;
          break;
        }
        const va_t Target = uint32_t{Bytes[0]} | uint32_t{Bytes[1]} << 8 |
                            uint32_t{Bytes[2]} << 16 | uint32_t{Bytes[3]} << 24;
        const bool HasFunctionSymbol =
            Img.hasFunctionSymbolAt(Target, ExecutableCodeOwners);
        const bool KnownEntry = isKnownFunctionEntry(Target);
        const bool OwnedInterior = isOwnedInteriorTarget(Img, Target);
        if (Target <= AuthoritativeCurrentFuncRange->first ||
            Target >= AuthoritativeCurrentFuncRange->second ||
            HasFunctionSymbol || KnownEntry || !OwnedInterior) {
          InteriorTargets = false;
          break;
        }
      }
      std::map<va_t, size_t> ShapedBases;
      bool ExactClaim = InteriorTargets;
      for (va_t Branch : Candidates) {
        if (!ExactClaim)
          break;
        const auto BranchInsn = Insns.find(Branch);
        if (BranchInsn == Insns.end() || !BranchInsn->second.IsBranch ||
            !BranchInsn->second.IsIndirect || BranchInsn->second.IsCall ||
            BranchInsn->second.IsRet || BranchInsn->second.IsCond) {
          ExactClaim = false;
          break;
        }
        va_t BlockStart = CurrentFuncEntry;
        if (!PublishedBlockStarts.empty()) {
          const auto It = std::upper_bound(PublishedBlockStarts.begin(),
                                           PublishedBlockStarts.end(), Branch);
          if (It != PublishedBlockStarts.begin())
            BlockStart = *std::prev(It);
        } else {
          const auto It = BlockStarts.upper_bound(Branch);
          if (It != BlockStarts.begin())
            BlockStart = *std::prev(It);
        }
        std::vector<LowOp> Ops;
        for (auto It = Insns.lower_bound(BlockStart);
             It != Insns.end() && It->first <= Branch; ++It) {
          // At most 432 bounded def() chains inspect the flattened Ops;
          // include both candidate LOAD sides and near-matching paths.
          if (!Budget.products({{It->second.Ops.size(), 512}}))
            return Incomplete();
          Ops.insert(Ops.end(), It->second.Ops.begin(), It->second.Ops.end());
        }
        const auto Shape = finiteGOTOFFClaimShape(Ops, Branch, Section.VA, End);
        if (!Shape) {
          ExactClaim = false;
          break;
        }
        if (!Budget.products(
                {{Img.Relocations.size(), PerRawWork},
                 {RelocatedInstructionAddressOccurrences.size(), 24},
                 {1, lookupWork(Img.DataAddressRelocOperands.size())}}))
          return Incomplete();
        const auto Source = Insns.find(Shape->LoadInsn);
        if (Source == Insns.end() || Source->second.IsInstructionGuard ||
            Source->second.Size < 4 ||
            Source->second.Size > InvalidVA - Source->first) {
          ExactClaim = false;
          break;
        }
        const va_t SourceEnd = Source->first + Source->second.Size;
        va_t FieldVA = InvalidVA;
        for (const RelocationEntry &Reloc : Img.Relocations) {
          const auto Mapped = MappedRawField(Reloc);
          if (!Mapped || *Mapped < Source->first || *Mapped >= SourceEnd)
            continue;
          if (*Mapped > SourceEnd - 4 || FieldVA != InvalidVA) {
            ExactClaim = false;
            break;
          }
          FieldVA = *Mapped;
        }
        if (!ExactClaim || FieldVA == InvalidVA) {
          ExactClaim = false;
          break;
        }
        if (!Budget.products(
                {{Img.Sections.size(), 4}, {Img.Segments.size(), 4}}))
          return Incomplete();
        const uint8_t *Bytes = Img.readVA(FieldVA, 4);
        if (!Bytes || (uint32_t{Bytes[0]} | uint32_t{Bytes[1]} << 8 |
                       uint32_t{Bytes[2]} << 16 | uint32_t{Bytes[3]} << 24) !=
                          Shape->Base) {
          ExactClaim = false;
          break;
        }
        // A present but malformed normalized anchor also fails the later
        // positive group audit. It cannot erase the raw+dataflow witness
        // that keeps this branch opaque during that failure.
        ++ShapedBases[Shape->Base];
      }
      ExactClaim &= ShapedBases.size() * 2 == Candidates.size() &&
                    ShapedBases.count(Section.VA) != 0;
      for (auto Base = ShapedBases.begin();
           ExactClaim && Base != ShapedBases.end(); ++Base) {
        const auto Next = std::next(Base);
        const va_t Bound = Next == ShapedBases.end() ? End : Next->first;
        const size_t Capacity = (Bound - Base->first) / 4;
        if (!Budget.products({{Img.Symbols.size(), 2}}))
          return Incomplete();
        ExactClaim = Base->second == 2 &&
                     Capacity >= limits::kMinJumpTableEntries &&
                     Capacity <= 64 && Img.dataObjectSizeAt(Base->first) == 0;
      }
      if (ExactClaim) {
        Key.Kind = GuardedJumpTableGroupKind::FiniteAdjacentGOTOFF;
        const bool ChangedIdentity =
            GuardedGroupIdentity && Key != *GuardedGroupIdentity;
        GuardedGroupIdentity = Key;
        if (ChangedIdentity) {
          GuardedGroupRejected = true;
          return false;
        }
      }
    }
    // A maximum of 64 slots and eight members bounds context storage. All
    // ordered inserts, lookups, copies and retirement are prepaid here.
    if (!Budget.products(
            {{SlotCount,
              64 + 8 * lookupWork(Insns.size()) +
                  2 * lookupWork(Img.CodePtrRelocSlots.size()) +
                  2 * lookupWork(knownFunctionEntryCount())},
             {Img.Symbols.size(), 2 * SlotCount},
             {Img.Segments.size(), 8 * SlotCount},
             {Img.DataAddressRelocOperands.size(), 24}}))
      return Incomplete();
    std::set<va_t> Slots;
    std::vector<va_t> Targets;
    Targets.reserve(SlotCount);
    for (va_t Slot = Section.VA; Slot < End; Slot += 4) {
      const uint8_t *Bytes = Img.readVA(Slot, 4);
      if (!Bytes || !Img.CodePtrRelocSlots.count(Slot)) {
        Valid = false;
        break;
      }
      const va_t Target = uint32_t{Bytes[0]} | uint32_t{Bytes[1]} << 8 |
                          uint32_t{Bytes[2]} << 16 | uint32_t{Bytes[3]} << 24;
      if (Target <= AuthoritativeCurrentFuncRange->first ||
          Target >= AuthoritativeCurrentFuncRange->second ||
          Img.hasFunctionSymbolAt(Target, ExecutableCodeOwners) ||
          isKnownFunctionEntry(Target) ||
          !Insns.count(Target)) {
        Valid = false;
        break;
      }
      Slots.insert(Slot);
      Targets.push_back(Target);
    }
    if (!Valid)
      continue;
    // A relocation starting inside a pointer field is an additional physical
    // writer, not another table coordinate.
    if (!Budget.products(
            {{1, lookupWork(Img.CodePtrRelocSlots.size())}, {Section.Size, 4}}))
      return Incomplete();
    for (auto It = Img.CodePtrRelocSlots.lower_bound(Section.VA);
         It != Img.CodePtrRelocSlots.end() && *It < End; ++It)
      if ((*It - Section.VA) % 4 != 0)
        Valid = false;
    std::set<va_t> Bases;
    for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands) {
      if (Field.TargetVA < Section.VA || Field.TargetVA >= End)
        continue;
      if (Field.Kind != RelocatedAddressFieldKind::I386ELFGOTOFF ||
          Field.Width != 4 || FieldVA < AuthoritativeCurrentFuncRange->first ||
          FieldVA >= AuthoritativeCurrentFuncRange->second ||
          (Field.TargetVA - Section.VA) % 4 != 0) {
        Valid = false;
        break;
      }
      Bases.insert(Field.TargetVA);
    }
    if (!Valid || Bases.size() < 2 || !Bases.count(Section.VA))
      continue;
    // A second complete owner makes this narrow recovery inapplicable.
    // Never choose a winner from section order or join distinct objects.
    if (Owner)
      return false;
    Owner.emplace(OwnerInventory{&Section, Key, std::move(Slots),
                                 std::move(Targets), std::move(Bases)});
  }
  if (!Owner)
    return false;
  JumpTableGroupLifecycleForTesting.LastProof.Stage = "owner-context";
  const auto &Section = *Owner->Storage;
  auto &Key = Owner->Key;
  const va_t End = Key.OwnerEnd;
  const size_t SlotCount = (End - Section.VA) / 4;
  const auto &Slots = Owner->Slots;
  const auto &Targets = Owner->Targets;
  const auto &Bases = Owner->Bases;
  std::map<va_t, size_t> Capacities;
  GuardedJumpTableGroupProofContext Context;
  if (!Budget.products({{Bases.size(), 32},
                        {PersistentCFGRoots.size(), 8},
                        {RelocationCFGRootSources.size(), 16},
                        {Candidates.size(), 32 + 3 * SlotCount}}))
    return Incomplete();
  for (auto Base = Bases.begin(); Base != Bases.end(); ++Base) {
    const auto Next = std::next(Base);
    const va_t Bound = Next == Bases.end() ? End : *Next;
    Capacities.emplace(*Base, (Bound - *Base) / 4);
  }
  bool FiniteJointGOTOFF = false;
  std::map<va_t, va_t> FiniteBranchBases;
  if (Key.Kind == GuardedJumpTableGroupKind::FiniteAdjacentGOTOFF) {
    bool CompletePartition =
        Bases.size() * 2 == Candidates.size() && *Bases.begin() == Section.VA;
    for (const auto &[Base, Capacity] : Capacities) {
      if (!Budget.products({{Img.Symbols.size(), 2}}))
        return Incomplete();
      CompletePartition &= Capacity >= limits::kMinJumpTableEntries &&
                           Capacity <= 64 && Img.dataObjectSizeAt(Base) == 0;
    }
    if (!CompletePartition) {
      GuardedGroupRejected = true;
      return false;
    }
    // Claim the complete multi-run owner before inspecting its anchors.
    // A damaged consumer vetoes every sibling instead of falling through
    // to an ordinary per-branch proposal after the group rejects.
    Key.Kind = GuardedJumpTableGroupKind::FiniteAdjacentGOTOFF;
    if (GuardedGroupIdentity && Key != *GuardedGroupIdentity)
      return false;
    GuardedGroupIdentity = Key;
    FiniteJointGOTOFF = true;
    size_t PerOccurrenceWork = 64;
    if (!detail::addLinearComparisonWork(PerOccurrenceWork, Insns.size(), 12) ||
        !detail::addLinearComparisonWork(PerOccurrenceWork, Img.Symbols.size(),
                                         8) ||
        !detail::addLinearComparisonWork(PerOccurrenceWork, Img.Segments.size(),
                                         8) ||
        !detail::addLinearComparisonWork(PerOccurrenceWork, Img.Sections.size(),
                                         8) ||
        !detail::addLinearComparisonWork(
            PerOccurrenceWork,
            lookupWork(Img.DataAddressRelocOperands.size()) +
                lookupWork(Insns.size()),
            2) ||
        !Budget.products(
            {{RelocatedInstructionAddressOccurrences.size(), PerOccurrenceWork},
             {Candidates.size(), 32},
             {Img.DataAddressRelocOperands.size(), 8}}))
      return Incomplete();
    const std::set<va_t> CandidateSet(Candidates.begin(), Candidates.end());
    bool Exact = true;
    for (const RelocatedInstructionAddressOccurrence &Occurrence :
         RelocatedInstructionAddressOccurrences) {
      if (!Bases.count(Occurrence.TargetVA))
        continue;
      const auto Field = Img.DataAddressRelocOperands.find(Occurrence.FieldVA);
      if (Occurrence.Width != 4 ||
          Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
          Occurrence.PCRelativeFromInstructionEnd ||
          Occurrence.OutputMayDepend || Occurrence.InputIndex < 0 ||
          Field == Img.DataAddressRelocOperands.end() ||
          Field->second.Kind != RelocatedAddressFieldKind::I386ELFGOTOFF ||
          Field->second.Width != 4 ||
          Field->second.TargetVA != Occurrence.TargetVA ||
          Field->second.TargetOwnerVA != Occurrence.TargetOwnerVA ||
          !Img.relocatedI386GOTOFFTargetBelongsToOwner(
              Field->second.TargetVA, Field->second.TargetOwnerVA)) {
        Exact = false;
        break;
      }
      const auto Source = Insns.find(Occurrence.InstructionAddr);
      if (Source == Insns.end() || Source->second.IsInstructionGuard ||
          Source->second.Size == 0 ||
          Source->second.Size > InvalidVA - Source->first ||
          Occurrence.FieldVA < Source->first ||
          Occurrence.FieldVA >= Source->first + Source->second.Size) {
        Exact = false;
        break;
      }
      va_t BlockEnd = CurrentFuncRange ? CurrentFuncRange->second : InvalidVA;
      if (!PublishedBlockStarts.empty()) {
        const auto Next =
            std::upper_bound(PublishedBlockStarts.begin(),
                             PublishedBlockStarts.end(), Source->first);
        if (Next != PublishedBlockStarts.end())
          BlockEnd = std::min(BlockEnd, *Next);
      } else {
        const auto Next = BlockStarts.upper_bound(Source->first);
        if (Next != BlockStarts.end())
          BlockEnd = std::min(BlockEnd, *Next);
      }
      va_t Branch = InvalidVA;
      for (auto It = Insns.upper_bound(Source->first);
           It != Insns.end() && It->first < BlockEnd; ++It) {
        const InsnRecord &Candidate = It->second;
        if (Candidate.IsCall || Candidate.IsBranch || Candidate.IsRet) {
          if (Candidate.IsBranch && Candidate.IsIndirect && !Candidate.IsCall &&
              !Candidate.IsRet && !Candidate.IsCond)
            Branch = Candidate.Addr;
          break;
        }
      }
      if (!CandidateSet.count(Branch) ||
          !FiniteBranchBases.emplace(Branch, Occurrence.TargetVA).second) {
        Exact = false;
        break;
      }
    }
    std::map<va_t, size_t> ConsumersPerBase;
    for (const auto &[Branch, Base] : FiniteBranchBases) {
      (void)Branch;
      ++ConsumersPerBase[Base];
    }
    const bool CompleteConsumers =
        ConsumersPerBase.size() == Bases.size() &&
        std::all_of(ConsumersPerBase.begin(), ConsumersPerBase.end(),
                    [](const auto &Entry) { return Entry.second == 2; });
    if (!Exact || FiniteBranchBases.size() != Candidates.size() ||
        !CompleteConsumers) {
      GuardedGroupRejected = true;
      return false;
    }
  }
  if (!FiniteJointGOTOFF) {
    if (GuardedGroupIdentity && Key != *GuardedGroupIdentity)
      return false;
    GuardedGroupIdentity = Key;
  }
  Context.Roots = PersistentCFGRoots;
  for (const auto &[Target, Sources] : RelocationCFGRootSources) {
    if (!Budget.products(
            {{Sources.size(),
              8 + lookupWork(ProtectedJumpTableRelocationSlots
                                 ? ProtectedJumpTableRelocationSlots->size()
                                 : 0)},
             {1, 2 * lookupWork(PersistentCFGRoots.size())}}))
      return Incomplete();
    if (DurableCFGRoots.count(Target) || Sources.empty())
      continue;
    const bool Covered =
        std::all_of(Sources.begin(), Sources.end(), [&](va_t Slot) {
          return Slots.count(Slot) &&
                 (!ProtectedJumpTableRelocationSlots ||
                  !ProtectedJumpTableRelocationSlots->count(Slot));
        });
    if (Covered)
      Context.Roots.erase(Target);
  }
  for (va_t Branch : Candidates) {
    if (FiniteJointGOTOFF) {
      const va_t Base = FiniteBranchBases.at(Branch);
      const size_t Start = static_cast<size_t>((Base - Section.VA) / 4);
      const size_t Capacity = Capacities.at(Base);
      Context.Edges.emplace(
          Branch, std::vector<va_t>(Targets.begin() + Start,
                                    Targets.begin() + Start + Capacity));
    } else {
      Context.Edges.emplace(Branch, Targets);
    }
    Context.EmptyEdges.emplace(Branch, std::vector<va_t>{});
  }
  std::map<va_t, JumpTableInfo> PreviousInfos;
  // Bootstrap every member without sibling exemptions, then prove the entire
  // consumer set twice with an immutable preceding-phase role universe. A
  // phase never observes an already processed prefix of its own members.
  for (unsigned Round = 0; Round < 3; ++Round) {
    FiniteGOTOFFRoundCertificate RoundCertificate;
    if (FiniteJointGOTOFF) {
      if (!Budget.products({{Candidates.size(), 64}, {SlotCount, 16}}))
        return Incomplete();
      for (const auto &[Branch, Base] : FiniteBranchBases) {
        const size_t Start = (Base - Section.VA) / 4;
        const size_t Capacity = Capacities.at(Base);
        RoundCertificate.PhysicalTargets.emplace(
            Branch, std::vector<va_t>(Targets.begin() + Start,
                                      Targets.begin() + Start + Capacity));
      }
    }
    Context.FiniteRoundCertificate =
        FiniteJointGOTOFF ? &RoundCertificate : nullptr;
    std::map<va_t, std::vector<va_t>> RuntimeEdges;
    std::map<va_t, JumpTableInfo> RuntimeInfos;
    std::set<va_t> RuntimeSlots;
    std::set<va_t> UsedBases;
    bool AllPassed = true;
    for (va_t Branch : Candidates) {
      CFGBuilder Scratch;
      if (!copyGuardedGroupProofSnapshot(Scratch, Context))
        return Incomplete();
      // The single outcome node and result holders have bounded cardinality;
      // reserve both insertion and destruction before entering the resolver.
      if (!Budget.take(64))
        return Incomplete();
      Scratch.CandidateProposalStageEvidenceRemaining =
          CandidateProposalStageEvidenceRemaining;
      Scratch.StrongJumpTableProposalOutcomes.emplace(
          Branch, StrongJumpTableProposalOutcome::DefinitiveLocalProofLoss);
      Scratch.JumpTableGroupLifecycleForTesting.LastProof.MemberCount =
          Candidates.size();
      auto Result = Scratch.resolveJumpTable(Img, Scratch.Insns.at(Branch));
      JumpTableGroupLifecycleForTesting.LastProof =
          Scratch.JumpTableGroupLifecycleForTesting.LastProof;
      // Transfer balances, never re-debit the resolver or restore a previous
      // allowance. Its RAII candidate outcome has already settled its work.
      CandidateProposalStageEvidenceRemaining =
          Scratch.CandidateProposalStageEvidenceRemaining;
      StackTableEvidenceRemaining = Scratch.StackTableEvidenceRemaining;
      IncompleteBranchMarkerEvidenceRemaining =
          Scratch.IncompleteBranchMarkerEvidenceRemaining;
      I386GOTOFFGraphQueryIssuedForTesting |=
          Scratch.I386GOTOFFGraphQueryIssuedForTesting;
      I386GOTOFFGraphQueryBudgetExhaustedForTesting |=
          Scratch.I386GOTOFFGraphQueryBudgetExhaustedForTesting;
      const auto Outcome = Scratch.StrongJumpTableProposalOutcomes.find(Branch);
      const bool MemberIncomplete =
          Scratch.CandidateProposalStageEvidenceIncomplete ||
          Scratch.IncompleteBranchMarkerEvidenceIncomplete ||
          Scratch.StackTableEvidenceIncompleteBranches.count(Branch) ||
          Scratch.IndexDomainEvidenceIncompleteBranches.count(Branch) ||
          (Outcome != Scratch.StrongJumpTableProposalOutcomes.end() &&
           Outcome->second ==
               StrongJumpTableProposalOutcome::EvidenceIncomplete);
      IncompleteBranchMarkerEvidenceIncomplete |=
          Scratch.IncompleteBranchMarkerEvidenceIncomplete;
      if (MemberIncomplete)
        return Incomplete();
      auto InfoIt = Scratch.ResolvedTableInfo.find(Branch);
      if (Result.empty() || InfoIt == Scratch.ResolvedTableInfo.end()) {
        AllPassed = false;
        break;
      }
      const JumpTableInfo &Info = InfoIt->second;
      JumpTableGroupLifecycleForTesting.LastProof.Stage = "member-contract";
      const auto Capacity = Capacities.find(Info.BaseAddr);
      const bool DenseDomain =
          (Info.AuthenticatedGuardBound == Result.size() &&
           Info.HasControllingGuard) ||
          Info.AuthenticatedDenseMaskBound == Result.size();
      const bool ExactFiniteDomain =
          FiniteJointGOTOFF && Capacity != Capacities.end() &&
          Info.HasBaseAddr && Info.PhysicalCapacity == Capacity->second &&
          Info.MaxEntries >= Result.size() &&
          Info.MaxEntries <= Capacity->second &&
          (Info.RuntimeSlotIndices.size() == Result.size() ||
           (Info.RuntimeSlotIndices.empty() &&
            Result.size() == Capacity->second));
      // A completed all-consumer finite-domain proof stands independently of
      // a syntactically ambiguous guard. Dense groups still require their
      // guard or mask proof to be unambiguous.
      bool Passed =
          Capacity != Capacities.end() &&
          (FiniteJointGOTOFF
               ? ExactFiniteDomain
               : (Capacity->second == Result.size() && DenseDomain)) &&
          Info.IndexDomainAuthenticated && !Info.IncompleteGuardDomain &&
          (FiniteJointGOTOFF || !Info.SemanticGuardDomainAmbiguous) &&
          Info.RelocAbsolute && !Info.IsRelative && !Info.MutatedUnsafe &&
          Info.EntrySize == 4 &&
          (Info.EntryStride == 0 || Info.EntryStride == 4) &&
          (FiniteJointGOTOFF || Info.MaxEntries == Result.size()) &&
          Info.Stride == 1 && !Info.PreScaledIndex &&
          (Info.RuntimeSlotIndices.empty() ||
           Info.RuntimeSlotIndices.size() == Result.size()) &&
          (Info.RuntimeCaseLabels.empty() ||
           Info.RuntimeCaseLabels.size() == Result.size()) &&
          (Info.EntryIndices.empty() ||
           Info.EntryIndices.size() == Result.size());
      if (!Budget.products({{Result.size(), 32},
                            {Info.SuppressibleRelocationSlots.size(),
                             FiniteJointGOTOFF ? 32u : 24u}}))
        return Incomplete();
      for (size_t I = 0; Passed && I < Result.size(); ++I) {
        const size_t Slot =
            Info.RuntimeSlotIndices.empty() ? I : Info.RuntimeSlotIndices[I];
        const size_t Physical = (Info.BaseAddr - Section.VA) / 4 + Slot;
        if (FiniteJointGOTOFF) {
          Passed &= Slot < Capacity->second &&
                    (I == 0 || Info.RuntimeSlotIndices.empty() ||
                     Info.RuntimeSlotIndices[I - 1] < Slot) &&
                    Physical < Targets.size() && Result[I] == Targets[Physical];
        } else {
          // Empty coordinate vectors are the established canonical dense
          // form; explicit vectors describe the same [0,N) prefix.
          Passed &= (Info.RuntimeSlotIndices.empty() || Slot == I) &&
                    (Info.RuntimeCaseLabels.empty() ||
                     Info.RuntimeCaseLabels[I] == I) &&
                    (Info.EntryIndices.empty() || Info.EntryIndices[I] == I) &&
                    Physical < Targets.size() && Result[I] == Targets[Physical];
        }
      }
      for (va_t Slot : Info.SuppressibleRelocationSlots) {
        Passed &= Slots.count(Slot) != 0;
        if (FiniteJointGOTOFF && Capacity != Capacities.end())
          Passed &= Slot >= Info.BaseAddr &&
                    Slot < Info.BaseAddr + uint64_t{4} * Capacity->second &&
                    (Slot - Info.BaseAddr) % 4 == 0;
      }
      if (!Passed) {
        AllPassed = false;
        break;
      }
      if (!prepayJumpTableInfoCopy(Info, 1))
        return Incomplete();
      UsedBases.insert(Info.BaseAddr);
      RuntimeSlots.insert(Info.SuppressibleRelocationSlots.begin(),
                          Info.SuppressibleRelocationSlots.end());
      RuntimeEdges.emplace(Branch, std::move(Result));
      RuntimeInfos.emplace(Branch, std::move(InfoIt->second));
    }
    if (!AllPassed) {
      GuardedGroupRejected = true;
      return false;
    }
    if (!Budget.products(
            {{SlotCount, 8}, {Candidates.size(), 16 + 2 * SlotCount}}))
      return Incomplete();
    auto &Coverage = JumpTableGroupLifecycleForTesting.LastProof;
    Coverage.OwnerSlotCount = Slots.size();
    Coverage.PermittedSlotCount = RuntimeSlots.size();
    Coverage.OwnerBaseCount = Bases.size();
    Coverage.UsedBaseCount = UsedBases.size();
    // Suppression permission must cover the complete physical owner in both
    // consumer-proof rounds. Bootstrap establishes only complete member
    // contracts and base coverage; it cannot publish suppression permission.
    if (UsedBases != Bases || (Round != 0 && RuntimeSlots != Slots)) {
      JumpTableGroupLifecycleForTesting.LastProof.Stage = "owner-coverage";
      GuardedGroupRejected = true;
      return false;
    }
    if (Round == 0) {
      Context.FiniteRoundCertificate = nullptr;
      Context.Edges = std::move(RuntimeEdges);
      PreviousInfos = std::move(RuntimeInfos);
      Context.ConsumerRoleInfos = &PreviousInfos;
      continue;
    }
    bool Stable = Round == 2 && RuntimeEdges == Context.Edges &&
                  RuntimeInfos.size() == PreviousInfos.size();
    for (const auto &[Branch, Info] : RuntimeInfos) {
      const auto Prev = PreviousInfos.find(Branch);
      if (Prev == PreviousInfos.end()) {
        Stable = false;
        continue;
      }
      const auto Same = compareJumpTableInfo(
          Info, Prev->second, CandidateProposalStageEvidenceRemaining);
      if (!Same)
        return Incomplete();
      Stable &= *Same;
    }
    if (Stable) {
      bool SameOld = GuardedGroupState && GuardedGroupState->Key == Key &&
                     GuardedGroupState->Targets == RuntimeEdges &&
                     GuardedGroupState->Infos.size() == RuntimeInfos.size();
      for (auto &[Branch, Info] : RuntimeInfos) {
        Info.RequiresCompleteCFGProof = true;
        if (!GuardedGroupState || !GuardedGroupState->Infos.count(Branch)) {
          SameOld = false;
        } else {
          const auto Same =
              compareJumpTableInfo(Info, GuardedGroupState->Infos.at(Branch),
                                   CandidateProposalStageEvidenceRemaining);
          if (!Same)
            return Incomplete();
          SameOld &= *Same;
        }
        if (!prepayJumpTableInfoCopy(Info, 1))
          return Incomplete();
      }
      // Includes the live copy, immutable cached comparison state, all
      // future member withdrawal/owner-closure lookups and result cleanup.
      const size_t Max = std::numeric_limits<size_t>::max();
      if (BlockStarts.size() > Max - SlotCount ||
          EverPublishedJumpTableBranches.size() > Max - Candidates.size())
        return Incomplete();
      const size_t MemberLookup =
          lookupWork(Insns.size()) +
          lookupWork(BlockStarts.size() + SlotCount) +
          lookupWork(EverPublishedJumpTableBranches.size() + Candidates.size());
      if (!Budget.products({{Candidates.size(), 128 + 16 * MemberLookup},
                            {SlotCount, 32 + 4 * MemberLookup}}))
        return Incomplete();
      GuardedJumpTableGroupState Pending;
      Pending.Key = Key;
      Pending.Targets = std::move(RuntimeEdges);
      Pending.Infos = std::move(RuntimeInfos);
      // No query or rebuild can observe the prefix: all live records and
      // the registry are installed before the first public CFG rebuild.
      for (const auto &[Branch, Edges] : Pending.Targets) {
        Insns.at(Branch).JumpTableTargets = Edges;
        ResolvedTableInfo.emplace(Branch, Pending.Infos.at(Branch));
        BlockStarts.insert(Edges.begin(), Edges.end());
        EverPublishedJumpTableBranches.insert(Branch);
      }
      Pending.Published = true;
      GuardedGroupState = std::move(Pending);
      MadeProgress |= !SameOld;
      MetadataRefreshed |= !SameOld;
      splitBlocks();
      rebuildBlocks(Func);
      if (!guardedGroupIsComplete()) {
        if (FiniteJointGOTOFF)
          withdrawGuardedJumpTableGroup(/*Reject=*/true);
        return false;
      }
      JumpTableGroupLifecycleForTesting.PublishedMemberCount = Key.MemberCount;
      return true;
    }
    Context.FiniteRoundCertificate = nullptr;
    Context.Edges = std::move(RuntimeEdges);
    PreviousInfos = std::move(RuntimeInfos);
  }
  // A complete but unstable two-round proof is not a resource failure and
  // cannot refresh itself forever using the same owner/member identity.
  GuardedGroupRejected = true;
  return false;
}

} // namespace neverd
