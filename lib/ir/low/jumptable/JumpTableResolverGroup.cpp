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
  // clearing results. No proposed/published sibling certificate, mutable cache,
  // callback classification or proof history enters the fresh member query.
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
  if (!GuardedGroupState || !GuardedGroupState->Published)
    return false;
  const auto &Key = GuardedGroupState->Key;
  return std::find(Key.Members.begin(), Key.Members.begin() + Key.MemberCount,
                   Branch) != Key.Members.begin() + Key.MemberCount;
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
      Img.I386GOTPCFields.empty() || Img.DataAddressRelocOperands.size() < 2 ||
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
    // A maximum of 64 slots and eight members bounds context storage. All
    // ordered inserts, lookups, copies and retirement are prepaid here.
    if (!Budget.products(
            {{SlotCount,
              64 + 8 * lookupWork(Insns.size()) +
                  2 * lookupWork(Img.CodePtrRelocSlots.size()) +
                  2 * lookupWork(KnownFuncEntries ? KnownFuncEntries->size()
                                                  : 0)},
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
          Img.hasFunctionSymbolAt(Target) ||
          (KnownFuncEntries && KnownFuncEntries->count(Target)) ||
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
  const auto &Key = Owner->Key;
  if (GuardedGroupIdentity && Key != *GuardedGroupIdentity)
    return false;
  const va_t End = Key.OwnerEnd;
  const size_t SlotCount = (End - Section.VA) / 4;
  const auto &Slots = Owner->Slots;
  const auto &Targets = Owner->Targets;
  const auto &Bases = Owner->Bases;
  GuardedGroupIdentity = Key;
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
    Context.Edges.emplace(Branch, Targets);
    Context.EmptyEdges.emplace(Branch, std::vector<va_t>{});
  }
  std::map<va_t, JumpTableInfo> PreviousInfos;
  for (unsigned Round = 0; Round < 2; ++Round) {
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
      bool Passed =
          Capacity != Capacities.end() && Capacity->second == Result.size() &&
          DenseDomain && Info.IndexDomainAuthenticated &&
          !Info.IncompleteGuardDomain && !Info.SemanticGuardDomainAmbiguous &&
          Info.RelocAbsolute && !Info.IsRelative && !Info.MutatedUnsafe &&
          Info.EntrySize == 4 &&
          (Info.EntryStride == 0 || Info.EntryStride == 4) &&
          Info.MaxEntries == Result.size() && Info.Stride == 1 &&
          !Info.PreScaledIndex &&
          (Info.RuntimeSlotIndices.empty() ||
           Info.RuntimeSlotIndices.size() == Result.size()) &&
          (Info.RuntimeCaseLabels.empty() ||
           Info.RuntimeCaseLabels.size() == Result.size()) &&
          (Info.EntryIndices.empty() ||
           Info.EntryIndices.size() == Result.size());
      if (!Budget.products({{Result.size(), 32},
                            {Info.SuppressibleRelocationSlots.size(), 24}}))
        return Incomplete();
      for (size_t I = 0; Passed && I < Result.size(); ++I) {
        const size_t Physical = (Info.BaseAddr - Section.VA) / 4 + I;
        // Empty coordinate vectors are the existing canonical dense form;
        // explicit vectors must describe that same complete [0,N) prefix.
        Passed &= (Info.RuntimeSlotIndices.empty() ||
                   Info.RuntimeSlotIndices[I] == I) &&
                  (Info.RuntimeCaseLabels.empty() ||
                   Info.RuntimeCaseLabels[I] == I) &&
                  (Info.EntryIndices.empty() || Info.EntryIndices[I] == I) &&
                  Physical < Targets.size() && Result[I] == Targets[Physical];
      }
      for (va_t Slot : Info.SuppressibleRelocationSlots)
        Passed &= Slots.count(Slot) != 0;
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
    // Suppression permission must cover the complete physical owner. There
    // is no implicit filler, unused tail, or extra same-section pointer.
    if (RuntimeSlots != Slots || UsedBases != Bases) {
      JumpTableGroupLifecycleForTesting.LastProof.Stage = "owner-coverage";
      GuardedGroupRejected = true;
      return false;
    }
    bool Stable = RuntimeEdges == Context.Edges &&
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
      if (!guardedGroupIsComplete())
        return false;
      JumpTableGroupLifecycleForTesting.PublishedMemberCount = Key.MemberCount;
      return true;
    }
    Context.Edges = std::move(RuntimeEdges);
    PreviousInfos = std::move(RuntimeInfos);
  }
  // A complete but unstable two-round proof is not a resource failure and
  // cannot refresh itself forever using the same owner/member identity.
  GuardedGroupRejected = true;
  return false;
}

} // namespace neverd
