//===- CFGBuilder.cpp - Control-flow graph construction ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements recursive-descent disassembly for single-function CFG
/// construction: the build entry point, the exploration worklist, and the
/// multi-stage retry of indirect branches.  Basic-block formation and ordinary
/// successor linking live in CFGBuilderBlocks.cpp, instruction classification
/// and call/tail-call rewriting in CFGBuilderInsn.cpp, exceptional successor
/// linking in CFGBuilderException.cpp, function entry-point detection in
/// FuncDetector.cpp, and jump-table resolution in jumptable/.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"

#include "neverd/Limits.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <queue>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

void detail::retireReplayedI386GOTPCAmbiguities(
    std::set<I386GOTOFFAmbiguityReplayKey> &Pending,
    const std::set<I386GOTOFFAmbiguityReplayKey> &Replayed,
    const std::set<va_t> *SafelyPublishedBranches) {
  for (auto It = Pending.begin(); It != Pending.end();) {
    const bool SafelyPublished =
        SafelyPublishedBranches &&
        SafelyPublishedBranches->count(std::get<0>(*It));
    if (Replayed.count(*It) || SafelyPublished)
      It = Pending.erase(It);
    else
      ++It;
  }
}

namespace {

/// True when \p Next begins an instruction that belongs to the same function as
/// the resumable trap in front of it.
///
/// Padding is what this tells apart from a body.  A linker pads between
/// functions with a run of `int3`, and a compiler plants a trap in front of an
/// embedded jump table or string; in both cases the bytes after the trap are
/// either another trap or not an instruction at all.  A `__debugbreak()` in a
/// live function is followed by the rest of that function.
bool codeFollowsTrap(const BinaryImage &Img, Decoder &Dec, va_t Next) {
  const Segment *Seg = Img.getSegmentFor(Next);
  if (!Seg || !Img.hasExecutableCodeOwnerAt(Next) || Next < Seg->VA)
    return false;
  const size_t Offset = static_cast<size_t>(Next - Seg->VA);
  if (Offset >= Seg->Data.size())
    return false;

  // Classification only: this must not disturb the operand detail or the x87
  // stack state the surrounding lift walk depends on.
  const bool PreviousDetail = Dec.detailEnabled();
  Dec.setDetail(false);
  DecodedInsn Peek;
  const int Size = Dec.decodeOneLight(Seg->Data.data() + Offset,
                                      Seg->Data.size() - Offset, Next, Peek);
  const bool HasCodeOwner = Size > 0 && Img.hasExecutableCodeOwnerRange(
                                            Next, static_cast<uint64_t>(Size));
  const bool IsTrap = HasCodeOwner && Dec.isResumableTrap(Peek);
  Dec.setDetail(PreviousDetail);
  return HasCodeOwner && !IsTrap;
}

InstructionMode effectiveInstructionMode(Arch Architecture,
                                         InstructionMode Mode) {
  if (Architecture == Arch::ARM && Mode == InstructionMode::Default)
    return InstructionMode::ARM;
  return Mode;
}

} // namespace

//===----------------------------------------------------------------------===//
// CFGBuilder
//===----------------------------------------------------------------------===//

LowFunc CFGBuilder::build(const BinaryImage &Img, Decoder &Dec, va_t EntryAddr,
                          const std::string &FuncName) {
  RelativeRelocationRootSourceCachePrepared = false;
  RelativeRelocationRootSourceCacheImage = nullptr;
  RelativeRelocationRootSourceCacheArch = Arch::Unknown;
  RelativeRelocationRootSourceCacheMode = InstructionMode::Default;
  RelativeRelocationRootSourceCacheSlotCount = 0;
  RelativeRelocationRootSourceCache.clear();
  RelativeRelocationRootSourceCacheLookupCountForTesting = 0;
  Insns.clear();
  BlockStarts.clear();
  ExploredAddrs.clear();
  CallTargets.clear();
  DiscoveredCodeRefs.clear();
  DiscoveredCodeRefSources.clear();
  ResolvedTableInfo.clear();
  PriorStrongJumpTableProposals.clear();
  NextStrongJumpTableProposals.clear();
  PriorProvisionalRelativeEdges.clear();
  NextProvisionalRelativeEdges.clear();
  StrongJumpTableProposalOutcomes.clear();
  CandidateProposalStageMutationAddrs.clear();
  QuarantinedJumpTableProposals.clear();
  EverStrongJumpTableProposalBranches.clear();
  CandidateProposalStageActive = false;
  CandidateProposalStageEvidenceRemaining = 0;
  CandidateProposalStageEvidenceIncomplete = false;
  IncompleteBranchMarkerEvidenceRemaining =
      std::min<size_t>(limits::kMaxJumpTableProposalStageEvidenceWork,
                       IncompleteBranchMarkerEvidenceBudgetForTesting.value_or(
                           limits::kMaxJumpTableProposalStageEvidenceWork));
  IncompleteBranchMarkerEvidenceIncomplete = false;
  I386GOTOFFTombstoneLookupCountForTesting = 0;
  ProposalStageCommitTailEvidenceExhaustedForTesting = false;
  CommitTailRollbackRetainedPendingI386AmbiguityForTesting = false;
  ExhaustedStableI386AmbiguityCommitTailForTesting = false;
  ExhaustedProposalStageCommitTailForTesting = false;
  ProposalStageForcedCommitTailRollbackPreservedStateForTesting = false;
  ProposalStageRollbackMutatedQuarantineForTesting = false;
  UntrackedJumpTableCandidateExhaustedThisStageForTesting = false;
  UntrackedJumpTableCandidateRollbackObservedForTesting = false;
  ForcedUntrackedJumpTableCandidateAddrForTesting = InvalidVA;
  UntrackedJumpTableCandidateProvisionalStateObservedForTesting = false;
  UntrackedJumpTableCandidateStateClearedOnRollbackForTesting = false;
  RecordedCompleteRuntimeStorageCertificateForTesting = false;
  NestedMutationTrackingEvidenceExhaustedForTesting = false;
  NestedMutationTrackingEvidenceExhaustedAddrForTesting = InvalidVA;
  ConstBaseLocalShapeClaimedForTesting = false;
  ConstBasePostShapeAnalysisIncompleteForTesting = false;
  ConstBaseFirstLocalShapeClaimedAddrForTesting = InvalidVA;
  ConstBaseSecondLocalShapeClaimedAddrForTesting = InvalidVA;
  ConstBaseLocalShapeClaimedAddrOverflowForTesting = false;
  ProposalCleanupEvidenceForTesting = {};
  JumpTableProofContextComplete = false;
  RequestedCompleteJumpTableProof = false;
  PersistentCFGRoots.clear();
  OrdinaryCFGRoots.clear();
  DurableCFGRoots.clear();
  RelocationCFGRootSources.clear();
  ActiveJumpTableProofRoots.reset();
  ActiveJumpTableCandidateAddr = InvalidVA;
  ActiveJumpTableCandidateProofRank = 0;
  ActiveJumpTableCandidateDependencyRank = 0;
  ActiveJumpTableConsumerAudit = false;
  PotentialJumpTableBranches.clear();
  EverPublishedJumpTableBranches.clear();
  LostValidatedJumpTableBranches.clear();
  StackTableEvidenceIncompleteBranches.clear();
  IndexDomainEvidenceIncompleteBranches.clear();
  ValidatedPhysicalJumpTableBranches.clear();
  CandidateFixedPointExplorationTargets.clear();
  MaxCandidateFixedPointExplorationTargetCountForTesting = 0;
  AmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCKeys.clear();
  StageAmbiguousI386GOTPCBranches.clear();
  StageReplayedI386GOTPCKeys.clear();
  CurrentI386GOTOFFAmbiguityKeys.clear();
  PublishedReachableInsns.clear();
  PublishedBlockStarts.clear();
  DecodedInstructionCount = 0;
  LiftedInstructionCount = 0;
  DecodedInstructionAddresses.clear();
  LiftedInstructionAddresses.clear();
  DecodeFailureAddresses.clear();
  UnsupportedInstructionAddresses.clear();
  TruncatedPathAddresses.clear();
  RelocatedInstructionAddressOccurrences.clear();
  I386GetPcOccurrences.clear();
  RelocatedInstructionScalarOperandOccurrences.clear();
  RelocatedInstructionScalarModelOccurrences.clear();
  I386GOTOFFProposalRootCache.clear();
  I386GOTOFFModelReachCache.clear();
  I386GOTOFFProposalEvidenceRemaining = 0;
  I386GOTOFFProposalShapeClaimed = false;
  I386GOTOFFProposalEvidenceIncomplete = false;
  I386GOTModelEvidenceIncomplete = false;
  I386GOTOFFAmbiguousModelReach = false;
  I386GOTOFFGraphQueryIssuedForTesting = false;
  I386GOTOFFGraphQueryBudgetExhaustedForTesting = false;
  StackTableEvidenceRemaining =
      std::min<size_t>(limits::kMaxJumpTableStackEvidenceWork,
                       StackTableEvidenceBudgetForTesting.value_or(
                           limits::kMaxJumpTableStackEvidenceWork));
  PreviouslyPublishedJumpTableBranches =
      std::move(PendingPreviouslyPublishedJumpTableBranches);
  PendingPreviouslyPublishedJumpTableBranches.clear();

  CurrentFuncEntry = EntryAddr;
  CurrentFuncRange.reset();
  AuthoritativeCurrentFuncRange.reset();
  CurrentImg = &Img;
  // The x87 TOP counter persists across functions in the shared lifter; start
  // each function with an empty stack so the entry block's lift TOP is 0.
  Dec.resetX86FpuState();
  BlockStarts.insert(EntryAddr);
  PersistentCFGRoots.insert(EntryAddr);
  OrdinaryCFGRoots.insert(EntryAddr);
  DurableCFGRoots.insert(EntryAddr);

  const ExceptionFunction *Exception = nullptr;
  for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
    if (Candidate.CodeRange.Begin == EntryAddr &&
        Candidate.Kind == RuntimeFunctionKind::Primary) {
      Exception = &Candidate;
      break;
    }
  }
  if (!Exception)
    Exception = Img.ExceptionMetadata.findFunction(EntryAddr);
  establishCurrentFuncRange(Img, Exception);
  std::vector<va_t> ExceptionalRoots;
  std::vector<va_t> ContinuationRoots;
  if (Exception) {
    // Every one of these tables spells "this field names no address" as zero,
    // so a zero has to be dropped before the range test rather than left to it.
    auto AddBoundary = [&](va_t Address) {
      if (Address != 0 && Exception->CodeRange.contains(Address) &&
          Address != EntryAddr)
        BlockStarts.insert(Address);
    };
    auto AddExceptionalRoot = [&](va_t Address) {
      if (Address == 0 || !Exception->CodeRange.contains(Address))
        return;
      AddBoundary(Address);
      PersistentCFGRoots.insert(Address);
      DurableCFGRoots.insert(Address);
      if (Address != EntryAddr)
        ExceptionalRoots.push_back(Address);
    };
    auto AddContinuationRoot = [&](va_t Address) {
      if (Address == 0 || !Exception->CodeRange.contains(Address))
        return;
      if (Address != EntryAddr)
        ContinuationRoots.push_back(Address);
    };
    if (Exception->SEH)
      for (const SEHScopeRecord &Scope : Exception->SEH->Scopes) {
        if (const auto Range = getSemanticSEHGuardedRange(
                Scope, Img.Arch, Exception->CodeRange)) {
          AddBoundary(Range->Begin);
          if (Range->End != Exception->CodeRange.End)
            AddBoundary(Range->End);
        }
        AddExceptionalRoot(Scope.FilterOrFinallyVA);
        AddExceptionalRoot(Scope.HandlerVA);
        AddBoundary(Scope.ContinuationVA);
      }
    if (Exception->Cxx) {
      for (const CxxIPState &State : Exception->Cxx->IPMap)
        AddBoundary(State.IP);
      for (const CxxUnwindAction &Action : Exception->Cxx->UnwindMap)
        AddExceptionalRoot(Action.ActionVA);
      for (const CxxTryBlock &Try : Exception->Cxx->TryBlocks)
        for (const CxxCatchHandler &Catch : Try.Handlers) {
          AddExceptionalRoot(Catch.HandlerVA);
          for (va_t Continuation : Catch.ContinuationVAs)
            AddContinuationRoot(Continuation);
        }
    }
    if (Exception->Itanium)
      for (const ItaniumCallSite &Site : Exception->Itanium->CallSites) {
        // An SJLJ table's "ranges" are call-site indices, not addresses, so
        // nothing in it may be read as one.
        if (!Exception->Itanium->IsCallSiteAddressForm)
          break;
        AddBoundary(Site.GuardedRange.Begin);
        if (Site.GuardedRange.End != Exception->CodeRange.End)
          AddBoundary(Site.GuardedRange.End);
        AddExceptionalRoot(Site.LandingPadVA);
      }
    if (Exception->Rust)
      for (const RustLandingPad &Pad : Exception->Rust->LandingPads)
        AddExceptionalRoot(Pad.PadVA);
    if (Exception->Registration) {
      for (const RegistrationScopeRecord &Scope :
           Exception->Registration->Scopes) {
        AddExceptionalRoot(Scope.FilterVA);
        AddExceptionalRoot(Scope.HandlerVA);
      }
      // A store of the current try level ends the region the previous level
      // guarded and begins the next one.  Cutting the block after it keeps one
      // level current throughout every block, which is what lets a block be
      // given its scope's edges without splitting hairs over where in the block
      // the change took effect.
      for (const RegistrationTryLevelStore &Store :
           Exception->Registration->TryLevelStores)
        AddBoundary(Store.EndVA);
    }
    if (Exception->Delphi) {
      AddExceptionalRoot(Exception->Delphi->FinallyBodyVA);
      AddExceptionalRoot(Exception->Delphi->ExceptBodyVA);
      for (const DelphiOnExceptionEntry &Arm : Exception->Delphi->OnExceptions)
        AddExceptionalRoot(Arm.HandlerVA);
    }
    if (Exception->DelphiScopes)
      for (const DelphiScopeRecord &Scope : Exception->DelphiScopes->Scopes) {
        AddBoundary(Scope.GuardedRange.Begin);
        if (Scope.GuardedRange.End != Exception->CodeRange.End)
          AddBoundary(Scope.GuardedRange.End);
        AddExceptionalRoot(Scope.TargetVA);
        for (const DelphiOnExceptionEntry &Arm : Scope.OnExceptions)
          AddExceptionalRoot(Arm.HandlerVA);
      }
    if (Exception->Go && Exception->Go->DeferReturnOffset) {
      // The runtime resumes a panicking frame at this offset to run what the
      // frame deferred, so it is entered without any branch reaching it.
      AddExceptionalRoot(Exception->CodeRange.Begin +
                         *Exception->Go->DeferReturnOffset);
    }
  }
  explore(Img, Dec, EntryAddr);
  // A relocation can take the address of a basic block that no ordinary edge
  // reaches (GNU computed-goto labels are the canonical case).  Decode those
  // roots only after the normal entry walk, so an invalid target cannot split a
  // real instruction that the entry traversal already established.
  exploreAddressTakenRoots(Img, Dec);
  // FH4 handler maps name ordinary resume points explicitly.  They are
  // disconnected control roots in the declaring runtime-function body, but
  // they are not exceptional entries and therefore stay separate from the
  // landing-pad inventory below.
  std::sort(ContinuationRoots.begin(), ContinuationRoots.end());
  ContinuationRoots.erase(
      std::unique(ContinuationRoots.begin(), ContinuationRoots.end()),
      ContinuationRoots.end());
  for (va_t Root : ContinuationRoots) {
    if (!AuthoritativeCurrentFuncRange ||
        Root <= AuthoritativeCurrentFuncRange->first ||
        Root >= AuthoritativeCurrentFuncRange->second ||
        !isOwnedInteriorTarget(Img, Root))
      continue;
    BlockStarts.insert(Root);
    PersistentCFGRoots.insert(Root);
    OrdinaryCFGRoots.insert(Root);
    DurableCFGRoots.insert(Root);
    if (!ExploredAddrs.count(Root))
      explore(Img, Dec, Root);
  }
  // A handler or landing pad inside the owning range is a legal CFG root even
  // though no ordinary branch reaches it — only the unwinder or the dispatcher
  // enters one, so recursive descent alone would leave its body undecoded.
  // Decode them explicitly; exceptional edges remain separate and therefore do
  // not perturb dominator or ordinary structuring semantics.
  std::sort(ExceptionalRoots.begin(), ExceptionalRoots.end());
  ExceptionalRoots.erase(
      std::unique(ExceptionalRoots.begin(), ExceptionalRoots.end()),
      ExceptionalRoots.end());
  for (va_t Root : ExceptionalRoots)
    if (!ExploredAddrs.count(Root))
      explore(Img, Dec, Root);
  // An exceptional or continuation root can itself materialize an exact
  // address-taken continuation (for example, a PC-relative epilogue address
  // passed to a local-unwind helper).  The first address-root walk ran before
  // those disconnected roots were decoded, so close the local discovery fixed
  // point once more.  exploreAddressTakenRoots loops over any further roots it
  // discovers and still applies the authoritative owner/instruction-boundary
  // checks to every candidate.
  exploreAddressTakenRoots(Img, Dec);
  completeExactAArch64PageBases(Img);
  splitBlocks();

  LowFunc Func;
  Func.Entry = EntryAddr;
  if (Exception)
    Func.ExceptionMetadata = *Exception;
  Func.Name = FuncName.empty()
                  ? (kAutoFuncPrefix + llvm::utohexstr(EntryAddr)).str()
                  : FuncName;
  // The callee-cleanup pop (x86 `ret imm`) seen while lifting this function's
  // instructions just above; a caller adds it to its post-call stack pointer.
  Func.CalleePopBytes = Dec.getX86RetPopBytes();

  rebuildBlocks(Func);

  multiStageResolve(Img, Dec, Func);

  // PAGEOFF output provenance is consumed only after lifting, so authenticate
  // it on the final resolver CFG rather than during linear instruction
  // discovery.  In particular, an ADD that is also an address-taken/exception
  // root has an independent live-in and must not borrow a preceding ADRP.
  ActiveJumpTableProofRoots.reset();
  JumpTableProofContextComplete = true;
  completeExactAArch64PageBases(Img);
  completeExactARMRelativeLiteralAddresses(Img);
  completeExactI386GOTBaseModels(Img);
  JumpTableProofContextComplete = false;

  convertIndirectTailCalls(Func);

  std::set<va_t> ReachableInsnAddrs;
  for (const LowBlock &Block : Func.Blocks)
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
      ReachableInsnAddrs.insert(Boundary.Address);

  // A relocation-free code reference discovered only while decoding a
  // provisional table case is not function-global evidence.  Once fixed-point
  // recovery removes that case, drop both the reference and its conditional
  // root instead of leaking an unreachable LEA into later functions.
  for (auto It = DiscoveredCodeRefs.begin(); It != DiscoveredCodeRefs.end();) {
    auto Sources = DiscoveredCodeRefSources.find(*It);
    const bool HasReachableSource =
        Sources != DiscoveredCodeRefSources.end() &&
        std::any_of(
            Sources->second.begin(), Sources->second.end(),
            [&](va_t Source) { return ReachableInsnAddrs.count(Source) != 0; });
    if (!HasReachableSource) {
      auto Erase = It++;
      DiscoveredCodeRefs.erase(Erase);
    } else {
      ++It;
    }
  }
  auto exactProofOp = [&](va_t Addr, int Seq) -> const LowOp * {
    const auto Insn = Insns.find(Addr);
    if (Insn == Insns.end() || Insn->second.IsInstructionGuard)
      return nullptr;
    const LowOp *Found = nullptr;
    for (const LowOp &Op : Insn->second.Ops) {
      if (Op.Addr != Addr || Op.Seq != Seq)
        continue;
      if (Found)
        return nullptr;
      Found = &Op;
    }
    return Found;
  };
  auto proofRegistersOverlap = [](const NdVar &Left, const NdVar &Right) {
    if (!Left.isReg() || !Right.isReg() || Left.Size == 0 || Right.Size == 0)
      return false;
    const uint64_t LeftEnd = Left.Offset + Left.Size;
    const uint64_t RightEnd = Right.Offset + Right.Size;
    return Left.Offset < RightEnd && Right.Offset < LeftEnd;
  };
  auto proofValueClobbers = [&](const NdVar &Output, const NdVar &Value) {
    return Output.Size != 0 &&
           (Output == Value || proofRegistersOverlap(Output, Value));
  };
  auto relocationFreeProofStillMatches =
      [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
    if (Occurrence.Authority ==
        RelocatedInstructionAddressProofKind::X86PCRelativeCodeAddress) {
      const uint16_t PointerSize = Img.getPointerSize();
      const auto Rec = Insns.find(Occurrence.InstructionAddr);
      const LowOp *Output =
          exactProofOp(Occurrence.InstructionAddr, Occurrence.OpSeq);
      const auto Sources = DiscoveredCodeRefSources.find(Occurrence.TargetVA);
      if ((Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
          PointerSize == 0 || Occurrence.FieldVA != InvalidVA ||
          Occurrence.Width != PointerSize || !Occurrence.DefinesOutput ||
          Occurrence.OutputMayDepend ||
          Occurrence.Provenance != ConstantAddressProvenance::CodeAddress ||
          !Occurrence.PCRelativeFromInstructionEnd ||
          Occurrence.TargetVA == InvalidVA ||
          Occurrence.TargetOwnerVA == InvalidVA ||
          !Img.hasExecutableCodeOwnerAt(Occurrence.TargetVA) ||
          !Img.relocatedTargetBelongsToOwner(Occurrence.TargetVA,
                                             Occurrence.TargetOwnerVA) ||
          Rec == Insns.end() || Rec->second.IsInstructionGuard ||
          Rec->second.IsBranch || Rec->second.IsCall || Rec->second.IsRet ||
          Rec->second.IsOpaqueTerminator ||
          Rec->second.IsResumableTerminator || !Output ||
          Output->Opcode != NdOp::COPY ||
          Output->Opcode != Occurrence.OutputOpcode ||
          Output->Output != Occurrence.OutputWitness ||
          !Output->Output.isReg() || Output->Output.Size != PointerSize ||
          Output->NumInputs != 1 || !Output->Inputs[0].isTemp() ||
          Output->Inputs[0].Size != PointerSize ||
          Occurrence.SeedInstructionAddr != Occurrence.InstructionAddr ||
          Occurrence.SeedOpSeq != Occurrence.OpSeq ||
          Occurrence.SeedOpcode != Occurrence.OutputOpcode ||
          Occurrence.SeedInputWitness != Output->Inputs[0] ||
          Occurrence.SeedOutputWitness != Occurrence.OutputWitness ||
          Sources == DiscoveredCodeRefSources.end() ||
          Sources->second.count(Occurrence.InstructionAddr) == 0)
        return false;

      unsigned MatchingOutputs = 0;
      for (const LowOp &Op : Rec->second.Ops)
        MatchingOutputs += Op.Addr == Occurrence.InstructionAddr &&
                           Op.Opcode == NdOp::COPY && Op.Output.isReg() &&
                           Op.Output.Size == PointerSize && Op.NumInputs == 1 &&
                           Op.Inputs[0].isTemp() &&
                           Op.Inputs[0].Size == PointerSize;
      return MatchingOutputs == 1;
    }
    if (Occurrence.Authority != RelocatedInstructionAddressProofKind::
                                    AArch64RelocationFreeDataDereference)
      return true;
    const uint16_t PointerSize = Img.getPointerSize();
    if (Img.Arch != Arch::AArch64 || Img.IsRelocatable || PointerSize == 0 ||
        PointerSize > sizeof(va_t) || Occurrence.FieldVA != InvalidVA ||
        Occurrence.Width != PointerSize || !Occurrence.DefinesOutput ||
        Occurrence.OutputMayDepend ||
        Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
        Occurrence.TargetVA == InvalidVA ||
        Occurrence.TargetOwnerVA == InvalidVA ||
        Occurrence.ArithmeticProof.empty() ||
        Occurrence.ArithmeticProof.size() > 32)
      return false;
    const auto RootIt = Insns.find(Occurrence.SeedInstructionAddr);
    const LowOp *Root =
        exactProofOp(Occurrence.SeedInstructionAddr, Occurrence.SeedOpSeq);
    if (RootIt == Insns.end() || !Root || RootIt->second.Ops.size() != 1 ||
        RootIt->second.Size == 0 ||
        RootIt->second.Size > InvalidVA - RootIt->second.Addr ||
        RootIt->second.IsBranch || RootIt->second.IsCall ||
        RootIt->second.IsRet || RootIt->second.IsOpaqueTerminator ||
        RootIt->second.IsResumableTerminator ||
        Root->Opcode != Occurrence.SeedOpcode || Root->Opcode != NdOp::COPY ||
        Root->NumInputs != 1 ||
        Root->Inputs[0] != Occurrence.SeedInputWitness ||
        Root->Output != Occurrence.SeedOutputWitness || !Root->Output.isReg() ||
        Root->Output.Size != PointerSize || !Root->Inputs[0].isConst() ||
        Root->Inputs[0].Size != PointerSize ||
        Root->Inputs[0].Provenance !=
            ConstantAddressProvenance::AddressFragment)
      return false;

    const unsigned PointerBits = static_cast<unsigned>(PointerSize) * 8;
    const uint64_t PointerMask = PointerBits == 64
                                     ? std::numeric_limits<uint64_t>::max()
                                     : (uint64_t{1} << PointerBits) - 1;
    auto canonicalScalar =
        [&](const LowOp &Op,
            const RelocatedInstructionAddressArithmeticStep &Step)
        -> std::optional<uint64_t> {
      if (!Step.ScalarInputWitness.isConst() ||
          Step.ScalarInputWitness.Provenance !=
              ConstantAddressProvenance::Scalar)
        return std::nullopt;
      if (Step.ScalarInputWitness.Size == PointerSize)
        return Step.ScalarInputWitness.Offset & PointerMask;
      if (PointerSize != 8 || Step.ScalarInputWitness.Size != 4 ||
          Step.BaseInputIndex != 0)
        return std::nullopt;
      const uint8_t *Bytes = Img.readVA(Op.Addr, sizeof(uint32_t));
      if (!Bytes)
        return std::nullopt;
      const uint32_t Word = readLE<uint32_t>(Bytes);
      if ((Word & 0x1f000000u) != 0x11000000u || (Word & 0x80000000u) == 0 ||
          (Word & 0x20000000u) != 0 ||
          (((Word & 0x40000000u) != 0) != (Op.Opcode == NdOp::INT_SUB)))
        return std::nullopt;
      const uint64_t Encoded = uint64_t((Word >> 10) & 0xfffu)
                               << (((Word >> 22) & 1u) ? 12 : 0);
      return Encoded == Step.ScalarInputWitness.Offset
                 ? std::optional<uint64_t>(Encoded)
                 : std::nullopt;
    };
    NdVar Current = Root->Output;
    va_t Address = Root->Inputs[0].Offset;
    va_t ExpectedAddress = RootIt->second.Addr + RootIt->second.Size;
    for (const RelocatedInstructionAddressArithmeticStep &Step :
         Occurrence.ArithmeticProof) {
      const auto Rec = Insns.find(Step.InstructionAddr);
      const LowOp *Op = exactProofOp(Step.InstructionAddr, Step.OpSeq);
      if (Rec == Insns.end() || Rec->second.Addr != ExpectedAddress || !Op ||
          Rec->second.Size == 0 ||
          Rec->second.Size > InvalidVA - Rec->second.Addr ||
          Rec->second.IsBranch || Rec->second.IsCall || Rec->second.IsRet ||
          Rec->second.IsOpaqueTerminator || Rec->second.IsResumableTerminator ||
          (Step.Opcode != NdOp::INT_ADD && Step.Opcode != NdOp::INT_SUB) ||
          Op->Opcode != Step.Opcode || Op->NumInputs != 2 ||
          Step.BaseInputIndex > 1 ||
          (Op->Opcode == NdOp::INT_SUB && Step.BaseInputIndex != 0) ||
          Op->Inputs[Step.BaseInputIndex] != Step.BaseInputWitness ||
          Op->Inputs[1 - Step.BaseInputIndex] != Step.ScalarInputWitness ||
          Op->Output != Step.OutputWitness ||
          (!Op->Output.isReg() && !Op->Output.isTemp()) ||
          Op->Output.Size != PointerSize ||
          Step.BaseInputWitness.Size != PointerSize ||
          Img.InstructionAddressMaterializations.count(Op->Addr))
        return false;

      // Some AArch64 memory encodings lower their unsigned offset into the
      // same instruction record as an instruction-local COPY/ADD/LOAD
      // chain. Reconstruct only exact, full-width COPY aliases from the
      // previous address value to this arithmetic input. P-code may retain
      // both the architectural register and a bookkeeping temporary in one
      // record; a COPY to the temporary does not invalidate the still-live
      // register.
      std::vector<NdVar> BaseAliases{Current};
      bool SawArithmetic = false;
      for (const LowOp &Other : Rec->second.Ops) {
        if (&Other == Op) {
          if (std::find(BaseAliases.begin(), BaseAliases.end(),
                        Step.BaseInputWitness) == BaseAliases.end())
            return false;
          SawArithmetic = true;
          continue;
        }
        const LowMemoryOperandView Memory = lowMemoryOperands(Other);
        if (!SawArithmetic) {
          if (Memory.Complete)
            return false;
          const bool CopiesAlias =
              Other.Opcode == NdOp::COPY && Other.NumInputs == 1 &&
              (Other.Output.isReg() || Other.Output.isTemp()) &&
              Other.Output.Size == PointerSize &&
              std::find(BaseAliases.begin(), BaseAliases.end(),
                        Other.Inputs[0]) != BaseAliases.end();
          if (CopiesAlias) {
            if (std::find(BaseAliases.begin(), BaseAliases.end(),
                          Other.Output) == BaseAliases.end())
              BaseAliases.push_back(Other.Output);
            continue;
          }
          if (std::any_of(BaseAliases.begin(), BaseAliases.end(),
                          [&](const NdVar &Alias) {
                            return proofValueClobbers(Other.Output, Alias);
                          }) ||
              proofValueClobbers(Other.Output, Op->Output))
            return false;
          continue;
        }

        const bool SameRecordFinalDereference =
            &Step == &Occurrence.ArithmeticProof.back() &&
            Occurrence.DereferenceInstructionAddr == Step.InstructionAddr;
        if (!SameRecordFinalDereference &&
            (Memory.Complete || proofValueClobbers(Other.Output, Current) ||
             proofValueClobbers(Other.Output, Op->Output)))
          return false;
      }
      if (!SawArithmetic)
        return false;
      const std::optional<uint64_t> Delta = canonicalScalar(*Op, Step);
      if (!Delta || Address > PointerMask ||
          (Op->Opcode == NdOp::INT_ADD && *Delta > PointerMask - Address) ||
          (Op->Opcode == NdOp::INT_SUB && *Delta > Address))
        return false;
      Address =
          Op->Opcode == NdOp::INT_ADD ? Address + *Delta : Address - *Delta;
      Current = Op->Output;
      ExpectedAddress =
          (&Step == &Occurrence.ArithmeticProof.back() &&
           Occurrence.DereferenceInstructionAddr == Step.InstructionAddr)
              ? Rec->second.Addr
              : Rec->second.Addr + Rec->second.Size;
    }
    const RelocatedInstructionAddressArithmeticStep &Final =
        Occurrence.ArithmeticProof.back();
    if (Occurrence.InstructionAddr != Final.InstructionAddr ||
        Occurrence.OpSeq != Final.OpSeq ||
        Occurrence.OutputOpcode != Final.Opcode ||
        Occurrence.OutputWitness != Final.OutputWitness ||
        Occurrence.TargetVA != Address)
      return false;

    const auto DereferenceRec =
        Insns.find(Occurrence.DereferenceInstructionAddr);
    const LowOp *Dereference = exactProofOp(
        Occurrence.DereferenceInstructionAddr, Occurrence.DereferenceOpSeq);
    if (DereferenceRec == Insns.end() || !Dereference ||
        DereferenceRec->second.Addr != ExpectedAddress ||
        DereferenceRec->second.IsBranch || DereferenceRec->second.IsCall ||
        DereferenceRec->second.IsRet ||
        DereferenceRec->second.IsOpaqueTerminator ||
        DereferenceRec->second.IsResumableTerminator ||
        Dereference->Opcode != Occurrence.DereferenceOpcode ||
        (Dereference->Opcode != NdOp::LOAD &&
         Dereference->Opcode != NdOp::STORE))
      return false;
    NdVar DereferenceAddress = Current;
    bool SawDereference = false;
    bool SawFinalArithmetic =
        DereferenceRec->second.Addr != Final.InstructionAddr;
    for (const LowOp &Op : DereferenceRec->second.Ops) {
      if (!SawFinalArithmetic) {
        if (Op.Addr == Final.InstructionAddr && Op.Seq == Final.OpSeq) {
          SawFinalArithmetic = true;
          continue;
        }
        continue;
      }
      const LowMemoryOperandView CandidateMemory = lowMemoryOperands(Op);
      if (&Op == Dereference) {
        if (!CandidateMemory.Complete || !CandidateMemory.Address ||
            *CandidateMemory.Address != DereferenceAddress || SawDereference ||
            proofValueClobbers(Op.Output, Current))
          return false;
        SawDereference = true;
        continue;
      }
      if (CandidateMemory.Complete || proofValueClobbers(Op.Output, Current))
        return false;
      if (!SawDereference) {
        if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
            Op.Inputs[0] != DereferenceAddress ||
            (!Op.Output.isReg() && !Op.Output.isTemp()) ||
            Op.Output.Size != PointerSize ||
            (Op.Output.isReg() &&
             proofRegistersOverlap(Op.Output, Current)))
          return false;
        DereferenceAddress = Op.Output;
      }
    }
    const LowMemoryOperandView Memory = lowMemoryOperands(*Dereference);
    if (!SawFinalArithmetic || !SawDereference || !Memory.Complete ||
        !Memory.Address ||
        DereferenceAddress != Occurrence.DereferenceAddressWitness ||
        *Memory.Address != DereferenceAddress ||
        Memory.AccessSize != Occurrence.DereferenceAccessSize ||
        Memory.AccessSize == 0 ||
        Memory.AccessSize - 1 > InvalidVA - Address)
      return false;
    const va_t Last = Address + Memory.AccessSize - 1;
    if (!Img.hasObjectDataProvenance(Address) ||
        !Img.hasObjectDataProvenance(Last) ||
        Img.hasExecutableCodeOwnerAt(Address) ||
        Img.hasExecutableCodeOwnerAt(Last) ||
        isRuntimeWritableAddress(Img, Address) ||
        isRuntimeWritableAddress(Img, Last) ||
        !Img.relocatedTargetBelongsToOwner(Address,
                                           Occurrence.TargetOwnerVA))
      return false;
    const Section *StartSection = Img.getSectionFor(Address);
    const Section *LastSection = Img.getSectionFor(Last);
    if (StartSection || LastSection)
      return StartSection && StartSection == LastSection &&
             StartSection->VA == Occurrence.TargetOwnerVA;
    const Segment *StartSegment = Img.getSegmentFor(Address);
    const Segment *LastSegment = Img.getSegmentFor(Last);
    return StartSegment && StartSegment == LastSegment &&
           StartSegment->VA == Occurrence.TargetOwnerVA;
  };

  Func.RelocatedInstructionAddressOccurrences.clear();
  for (const RelocatedInstructionAddressOccurrence &Occurrence :
       RelocatedInstructionAddressOccurrences) {
    bool Published = false;
    for (const LowBlock &Block : Func.Blocks) {
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.Address != Occurrence.InstructionAddr ||
            (!Occurrence.DefinesOutput &&
             (Boundary.Size > InvalidVA - Boundary.Address ||
              Occurrence.FieldVA < Boundary.Address ||
              Occurrence.FieldVA >= Boundary.Address + Boundary.Size)))
          continue;
        const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
        if (End > Block.Ops.size())
          continue;
        for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
          const LowOp &Op = Block.Ops[I];
          if (Op.Addr != Occurrence.InstructionAddr ||
              Op.Seq != Occurrence.OpSeq)
            continue;
          auto Matches = [&](const NdVar &Value) {
            return Value.isConst() &&
                   isExactAddressProvenance(Value.Provenance) &&
                   Value.Provenance == Occurrence.Provenance &&
                   Value.Offset == Occurrence.TargetVA &&
                   (Occurrence.TargetOwnerVA == InvalidVA ||
                    Value.AddressOwnerVA == Occurrence.TargetOwnerVA);
          };
          if (Occurrence.DefinesOutput) {
            Published = Op.Opcode == Occurrence.OutputOpcode &&
                        Op.Output == Occurrence.OutputWitness &&
                        (Op.Output.isReg() || Op.Output.isTemp()) &&
                        Op.Output.Size != 0;
          } else {
            Published = Matches(Op.Output);
            for (uint8_t InputIndex = 0;
                 !Published && InputIndex < Op.NumInputs; ++InputIndex)
              Published = Matches(Op.Inputs[InputIndex]);
          }
          if (Published)
            break;
        }
        if (Published)
          break;
      }
      if (Published)
        break;
    }
    if (Published && relocationFreeProofStillMatches(Occurrence)) {
      Func.RelocatedInstructionAddressOccurrences.push_back(Occurrence);
      if (!Occurrence.OutputMayDepend &&
          Occurrence.Provenance == ConstantAddressProvenance::CodeAddress)
        DiscoveredCodeRefs.insert(
            normalizeCodeAddress(Occurrence.TargetVA, Img.Arch, Img.Mode));
    }
  }

  Func.I386GetPcOccurrences.clear();
  for (const I386GetPcOccurrence &Occurrence : I386GetPcOccurrences) {
    bool Published = false;
    for (const LowBlock &Block : Func.Blocks) {
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.Address != Occurrence.InstructionAddr)
          continue;
        const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
        if (End > Block.Ops.size())
          continue;
        for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
          const LowOp &Op = Block.Ops[I];
          if (Op.Addr == Occurrence.InstructionAddr &&
              Op.Seq == Occurrence.OpSeq &&
              Op.Opcode == Occurrence.OutputOpcode &&
              Op.Output == Occurrence.OutputWitness && Op.NumInputs == 1 &&
              Op.Inputs[0] == Occurrence.InputWitness) {
            Published = true;
            break;
          }
        }
        if (Published)
          break;
      }
      if (Published)
        break;
    }
    if (Published)
      Func.I386GetPcOccurrences.push_back(Occurrence);
  }

  Func.RelocatedInstructionScalarModelOccurrences.clear();
  for (const RelocatedInstructionScalarModelOccurrence &Occurrence :
       RelocatedInstructionScalarModelOccurrences) {
    bool Published = false;
    for (const LowBlock &Block : Func.Blocks) {
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (Boundary.Address != Occurrence.InstructionAddr ||
            Boundary.Size > InvalidVA - Boundary.Address ||
            Occurrence.FieldVA < Boundary.Address ||
            Occurrence.FieldVA >= Boundary.Address + Boundary.Size)
          continue;
        const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
        if (End > Block.Ops.size())
          continue;
        for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
          const LowOp &Op = Block.Ops[I];
          if (Op.Addr == Occurrence.InstructionAddr &&
              Op.Seq == Occurrence.OpSeq &&
              Op.Opcode == Occurrence.OutputOpcode &&
              Op.Output == Occurrence.OutputWitness &&
              (Op.Output.isReg() || Op.Output.isTemp()) &&
              Op.Output.Size == Occurrence.Width) {
            Published = true;
            break;
          }
        }
        if (Published)
          break;
      }
      if (Published)
        break;
    }
    if (Published)
      Func.RelocatedInstructionScalarModelOccurrences.push_back(Occurrence);
  }

  // A relocation-free RIP-relative LEA is initially only an address-of
  // candidate.  Once jump-table recovery proves that the same address owns
  // inline table storage (which Mach-O commonly places in __text), the data
  // owner wins: publishing it as a global CodeRefTarget would make every
  // numerically equal occurrence look like a function/label identity.
  // Apply the same arbitration to exact code-address relocation occurrences
  // before publishing the function-local inventory.
  for (auto It = DiscoveredCodeRefs.begin(); It != DiscoveredCodeRefs.end();) {
    bool IsJumpTableStorage = false;
    for (const JumpTable &JT : Func.JumpTables)
      if (JT.ownsStorageAddress(*It)) {
        IsJumpTableStorage = true;
        break;
      }
    if (IsJumpTableStorage) {
      auto Erase = It++;
      DiscoveredCodeRefs.erase(Erase);
    } else {
      ++It;
    }
  }
  Func.RelocatedInstructionAddressOccurrences.erase(
      std::remove_if(
          Func.RelocatedInstructionAddressOccurrences.begin(),
          Func.RelocatedInstructionAddressOccurrences.end(),
          [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
            return Occurrence.Authority ==
                       RelocatedInstructionAddressProofKind::
                           X86PCRelativeCodeAddress &&
                   DiscoveredCodeRefs.count(Occurrence.TargetVA) == 0;
          }),
      Func.RelocatedInstructionAddressOccurrences.end());
  Func.CodeRefTargets.assign(DiscoveredCodeRefs.begin(),
                             DiscoveredCodeRefs.end());
  // Recursive descent intentionally retains decoded provisional jump-table
  // cases across fixed-point rounds.  Coverage is a property of the final
  // public function, however, so derive its inventory from the final roots and
  // edges instead of publishing the exploration cache wholesale.  Terminal
  // decode/lift failures have no LowInstructionBoundary of their own; retain
  // them when a final edge (or an unsuppressed durable/address-taken root)
  // attempted that address.
  std::set<va_t> FinalAttemptedAddresses = PublishedReachableInsns;
  auto AddAttempted = [&](va_t Address) {
    if (Address != InvalidVA && ExploredAddrs.count(Address))
      FinalAttemptedAddresses.insert(Address);
  };
  auto FinalTableSuppressesRelocationRoot = [&](va_t Root) {
    if (DurableCFGRoots.count(Root))
      return false;
    const auto Sources = RelocationCFGRootSources.find(Root);
    if (Sources == RelocationCFGRootSources.end() || Sources->second.empty())
      return false;
    return std::all_of(
        Sources->second.begin(), Sources->second.end(), [&](va_t Slot) {
          for (const auto &[BranchAddr, Info] : ResolvedTableInfo) {
            const auto Branch = Insns.find(BranchAddr);
            if (Branch == Insns.end() ||
                Branch->second.JumpTableTargets.empty() ||
                !PublishedReachableInsns.count(BranchAddr))
              continue;
            if (std::find(Info.SuppressibleRelocationSlots.begin(),
                          Info.SuppressibleRelocationSlots.end(),
                          Slot) != Info.SuppressibleRelocationSlots.end())
              return true;
          }
          return false;
        });
  };
  for (va_t Root : PersistentCFGRoots)
    if (!FinalTableSuppressesRelocationRoot(Root))
      AddAttempted(Root);
  for (va_t Root : DiscoveredCodeRefs)
    AddAttempted(Root);

  for (va_t Address : PublishedReachableInsns) {
    const auto It = Insns.find(Address);
    if (It == Insns.end())
      continue;
    const InsnRecord &Rec = It->second;
    if (Rec.IsRet) {
      if (Rec.IsCond && Rec.IsBranch)
        AddAttempted(Rec.BranchTarget);
      continue;
    }
    if (Rec.IsBranch && !Rec.IsCall) {
      if (Rec.IsIndirect)
        for (va_t Target : Rec.JumpTableTargets)
          AddAttempted(Target);
      else
        AddAttempted(Rec.BranchTarget);
      if (Rec.IsCond && Rec.Size <= InvalidVA - Rec.Addr)
        AddAttempted(Rec.Addr + Rec.Size);
      continue;
    }
    if (Rec.IsNoReturnCall && !Rec.IsCond)
      continue;
    if (Rec.Size <= InvalidVA - Rec.Addr)
      AddAttempted(Rec.Addr + Rec.Size);
  }

  Func.DecodedInstructionCount = static_cast<uint64_t>(std::count_if(
      DecodedInstructionAddresses.begin(), DecodedInstructionAddresses.end(),
      [&](va_t Address) { return FinalAttemptedAddresses.count(Address); }));
  Func.LiftedInstructionCount = static_cast<uint64_t>(std::count_if(
      LiftedInstructionAddresses.begin(), LiftedInstructionAddresses.end(),
      [&](va_t Address) { return FinalAttemptedAddresses.count(Address); }));
  auto PublishFailures = [&](const std::set<va_t> &Failures,
                             std::vector<va_t> &Published) {
    for (va_t Address : Failures)
      if (FinalAttemptedAddresses.count(Address))
        Published.push_back(Address);
  };
  PublishFailures(DecodeFailureAddresses, Func.DecodeFailureAddresses);
  PublishFailures(UnsupportedInstructionAddresses,
                  Func.UnsupportedInstructionAddresses);
  PublishFailures(TruncatedPathAddresses, Func.TruncatedPathAddresses);
  if (UnsafeJumpTableBranches)
    for (va_t Addr : *UnsafeJumpTableBranches)
      if (Insns.count(Addr))
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
  if (PreservePotentialJumpTableBranches)
    for (va_t Addr : PotentialJumpTableBranches) {
      auto It = Insns.find(Addr);
      if (It != Insns.end() && It->second.JumpTableTargets.empty())
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
    }
  Func.EverPublishedJumpTableBranchAddresses.insert(
      EverPublishedJumpTableBranches.begin(),
      EverPublishedJumpTableBranches.end());
  for (va_t Addr : LostValidatedJumpTableBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : StackTableEvidenceIncompleteBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : IndexDomainEvidenceIncompleteBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  if (IncompleteBranchMarkerEvidenceIncomplete)
    for (const auto &[Addr, Rec] : Insns)
      if (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall && !Rec.IsRet &&
          !Rec.IsCond && Rec.JumpTableTargets.empty())
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : ValidatedPhysicalJumpTableBranches)
    if (auto It = Insns.find(Addr);
        It != Insns.end() && It->second.JumpTableTargets.empty())
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : AmbiguousI386GOTPCBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : PendingAmbiguousI386GOTPCBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);

  LLVM_DEBUG(llvm::dbgs() << "CFG built: " << Func.Blocks.size()
                          << " blocks for " << Func.Name << " @ 0x"
                          << llvm::utohexstr(Func.Entry) << "\n");
  return Func;
}

void CFGBuilder::explore(const BinaryImage &Img, Decoder &Dec, va_t Addr) {
  // Resolver-stage proposal caches are valid only for one immutable decoded
  // graph.  Exploring even one new target can add an alternate predecessor,
  // persistent relocation root, or source occurrence that invalidates a
  // previously successful exact reaching-value proof.  Centralize the
  // invalidation here so target publication, relocated-interior recovery, and
  // shared-table reconciliation cannot accidentally reuse a stale result.
  I386GOTOFFProposalRootCache.clear();
  I386GOTOFFModelReachCache.clear();
  std::queue<va_t> Worklist;
  Worklist.push(Addr);

  while (!Worklist.empty()) {
    va_t Cur = Worklist.front();
    Worklist.pop();

    while (true) {
      if (ExploredAddrs.count(Cur))
        break;
      // An actual graph extension invalidates every generation-local replay
      // and positive ambiguity shadow.  Pending exact query identities remain
      // fail-closed carry, but only a fresh query on this immutable graph may
      // retire them or commit a semantic certificate.
      StageReplayedI386GOTPCKeys.clear();
      StageAmbiguousI386GOTPCBranches.clear();
      ExploredAddrs.insert(Cur);

      const auto *Seg = Img.getSegmentFor(Cur);
      if (!Seg || !Img.hasExecutableCodeOwnerAt(Cur)) {
        TruncatedPathAddresses.insert(Cur);
        break;
      }

      // A segment's VA range (Seg->Size) can exceed its materialized bytes
      // (e.g. .bss, or bytes the loader refused to map from a crafted header),
      // so guard before subtracting or Remain underflows into a huge length.
      size_t Off = static_cast<size_t>(Cur - Seg->VA);
      if (Off >= Seg->Data.size()) {
        TruncatedPathAddresses.insert(Cur);
        break;
      }
      size_t Remain = Seg->Data.size() - Off;

      DecodedInsn DI;
      int Sz = Dec.decodeOneForLift(Seg->Data.data() + Off, Remain, Cur, DI);
      if (Sz <= 0) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz))) {
        TruncatedPathAddresses.insert(Cur);
        break;
      }
      ++DecodedInstructionCount;
      DecodedInstructionAddresses.insert(Cur);

      const va_t InsnSize = static_cast<va_t>(Sz);
      if (InsnSize > std::numeric_limits<va_t>::max() - Cur) {
        DecodeFailureAddresses.insert(Cur);
        break;
      }
      const va_t Next = Cur + InsnSize;

      InsnRecord Rec;
      Rec.Addr = Cur;
      Rec.Size = static_cast<uint16_t>(Sz);
      Rec.Mode = effectiveInstructionMode(Img.Arch, Img.Mode);
      Rec.Immediate = Dec.returnImmediate(DI);
      Rec.TargetMode = Dec.controlTargetMode(DI, Rec.Mode);
      Rec.FpuTopIn = Dec.getX86FpuTop();
      std::vector<RelocatedAddressOperand> RelocatedOperands;
      auto selectRelocatedOperand =
          [&](const std::map<va_t, RelocatedAddressField> &Occurrences,
              ConstantAddressProvenance Provenance) {
            auto It = Occurrences.lower_bound(Cur);
            for (; It != Occurrences.end() && It->first < Next; ++It) {
              va_t TargetVA = It->second.TargetVA;
              if (It->second.PCRelativeFromInstructionEnd) {
                if (It->second.Width == 0 || It->second.Width > 8)
                  continue;
                const unsigned Bits = It->second.Width * 8;
                uint64_t Disp = It->second.EncodedValue;
                if (Bits < 64) {
                  const uint64_t Mask = (uint64_t(1) << Bits) - 1;
                  Disp &= Mask;
                  if (Disp & (uint64_t(1) << (Bits - 1)))
                    Disp |= ~Mask;
                }
                TargetVA = Next + Disp;
                if (Img.getPointerSize() == 4)
                  TargetVA = static_cast<uint32_t>(TargetVA);
              }
              const bool OwnerMatches =
                  It->second.Kind == RelocatedAddressFieldKind::I386ELFGOTOFF
                      ? Img.relocatedI386GOTOFFTargetBelongsToOwner(
                            TargetVA, It->second.TargetOwnerVA)
                      : Img.relocatedTargetBelongsToOwner(
                            TargetVA, It->second.TargetOwnerVA);
              if (!OwnerMatches)
                continue;
              RelocatedOperands.push_back(RelocatedAddressOperand{
                  It->first, It->second.EncodedValue, TargetVA,
                  It->second.Width, Provenance, It->second.TargetOwnerVA,
                  It->second.PCRelativeFromInstructionEnd});
            }
          };
      selectRelocatedOperand(Img.DataAddressRelocOperands,
                             ConstantAddressProvenance::DataAddress);
      selectRelocatedOperand(Img.CodeAddressRelocOperands,
                             ConstantAddressProvenance::CodeAddress);
      std::vector<RelocatedScalarOperand> RelocatedScalarOperands;
      if (Img.Arch == Arch::X86 && Img.isELF() && Img.getPointerSize() == 4) {
        auto Field = Img.I386GOTPCFields.lower_bound(Cur);
        for (; Field != Img.I386GOTPCFields.end() && Field->first < Next;
             ++Field)
          RelocatedScalarOperands.push_back(
              {Field->first, Field->second.EncodedValue, 4,
               RelocatedScalarOperand::Kind::I386ELFGOTPC});
        auto Ambiguous = Img.AmbiguousI386GOTPCFields.lower_bound(Cur);
        for (; Ambiguous != Img.AmbiguousI386GOTPCFields.end() &&
               *Ambiguous < Next;
             ++Ambiguous) {
          const uint8_t *EncodedBytes = Img.readVA(*Ambiguous, 4);
          if (!EncodedBytes)
            continue;
          uint32_t Encoded = 0;
          std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
          RelocatedScalarOperands.push_back(
              {*Ambiguous, Encoded, 4,
               RelocatedScalarOperand::Kind::I386ELFGOTPC});
        }
        auto AmbiguousGOTOFF = Img.AmbiguousI386GOTOFFFields.lower_bound(Cur);
        for (; AmbiguousGOTOFF != Img.AmbiguousI386GOTOFFFields.end() &&
               *AmbiguousGOTOFF < Next;
             ++AmbiguousGOTOFF) {
          const uint8_t *EncodedBytes = Img.readVA(*AmbiguousGOTOFF, 4);
          if (!EncodedBytes)
            continue;
          uint32_t Encoded = 0;
          std::memcpy(&Encoded, EncodedBytes, sizeof(Encoded));
          RelocatedScalarOperands.push_back(
              {*AmbiguousGOTOFF, Encoded, 4,
               RelocatedScalarOperand::Kind::I386ELFAmbiguousGOTOFF});
        }
      }
      try {
        Dec.liftToLow(DI, Rec.Ops, RelocatedOperands, RelocatedScalarOperands);
      } catch (const UnliftedInstruction &Failure) {
        UnsupportedInstructionAddresses.insert(Failure.getAddr());
        break;
      }
      if (std::optional<I386GetPcOccurrence> GetPc =
              Dec.getX86GetPcOccurrence())
        I386GetPcOccurrences.push_back(*GetPc);
      if (std::optional<RelocatedInstructionScalarOperandOccurrence> Scalar =
              Dec.getX86ScalarOperandOccurrence())
        RelocatedInstructionScalarOperandOccurrences.push_back(*Scalar);
      for (const RelocatedAddressOperand &Reloc : RelocatedOperands) {
        for (const LowOp &Op : Rec.Ops) {
          auto Matches = [&](const NdVar &Value) {
            return Value.isConst() &&
                   isExactAddressProvenance(Value.Provenance) &&
                   Value.Provenance == Reloc.Provenance &&
                   Value.Offset == Reloc.TargetVA &&
                   (Reloc.TargetOwnerVA == InvalidVA ||
                    Value.AddressOwnerVA == Reloc.TargetOwnerVA);
          };
          const bool ConsumedOutput = Matches(Op.Output);
          unsigned MatchingInputs = 0;
          int MatchingInput = -1;
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (Matches(Op.Inputs[I])) {
              ++MatchingInputs;
              MatchingInput = I;
            }
          if (!ConsumedOutput && MatchingInputs == 0)
            continue;
          RelocatedInstructionAddressOccurrence Occurrence{
              Reloc.FieldVA,
              Rec.Addr,
              Op.Seq,
              Reloc.TargetVA,
              Reloc.TargetOwnerVA,
              Reloc.Width,
              Reloc.Provenance,
              Reloc.PCRelativeFromInstructionEnd};
          if (!ConsumedOutput && MatchingInputs == 1)
            Occurrence.InputIndex = MatchingInput;
          RelocatedInstructionAddressOccurrences.push_back(
              std::move(Occurrence));
          break;
        }
      }
      ++LiftedInstructionCount;
      LiftedInstructionAddresses.insert(Cur);
      Rec.FpuTopOut = Dec.getX86FpuTop();
      Rec.FpuReset = Dec.x86FpuDidReset();

      // A relocation-free PC-relative `lea` taking the address of executable
      // code is a same-section function pointer the assembler resolved (so the
      // loader saw no relocation).  Record the target so the emitter symbolizes
      // the folded constant to `ptrtoint @func` rather than the stale VA.
      if (va_t Ref = Dec.pcRelCodeRefTarget(DI); Ref != InvalidVA) {
        if (Img.hasExecutableCodeOwnerAt(Ref)) {
          DiscoveredCodeRefs.insert(Ref);
          DiscoveredCodeRefSources[Ref].insert(Cur);

          // Preserve the decoded address-of as an occurrence certificate, not
          // merely as a module-wide target number.  A pure RIP/EIP-relative
          // LEA has exactly one architectural pointer-width COPY from its
          // effective-address temporary.  Binding that exact output prevents a
          // later scalar with the same numeric value (or a LEA into a different
          // register) from acquiring code-address provenance.
          const LowOp *Output = nullptr;
          bool AmbiguousOutput = false;
          const uint16_t PointerSize = Img.getPointerSize();
          for (const LowOp &Op : Rec.Ops) {
            if (Op.Addr != Cur || Op.Opcode != NdOp::COPY ||
                !Op.Output.isReg() || Op.Output.Size != PointerSize ||
                Op.NumInputs != 1 || !Op.Inputs[0].isTemp() ||
                Op.Inputs[0].Size != PointerSize)
              continue;
            if (Output) {
              AmbiguousOutput = true;
              break;
            }
            Output = &Op;
          }

          va_t TargetOwnerVA = InvalidVA;
          if (const Section *Sec = Img.getSectionFor(Ref);
              Sec && Sec->isExecutable())
            TargetOwnerVA = Sec->VA;
          else if (const Segment *TargetSeg = Img.getSegmentFor(Ref);
                   TargetSeg && TargetSeg->isExecutable())
            TargetOwnerVA = TargetSeg->VA;

          if (!AmbiguousOutput && Output && RelocatedOperands.empty() &&
              PointerSize != 0 && TargetOwnerVA != InvalidVA &&
              Img.relocatedTargetBelongsToOwner(Ref, TargetOwnerVA)) {
            RelocatedInstructionAddressOccurrence Occurrence;
            Occurrence.InstructionAddr = Cur;
            Occurrence.OpSeq = Output->Seq;
            Occurrence.TargetVA = Ref;
            Occurrence.TargetOwnerVA = TargetOwnerVA;
            Occurrence.Width = PointerSize;
            Occurrence.Provenance = ConstantAddressProvenance::CodeAddress;
            Occurrence.PCRelativeFromInstructionEnd = true;
            Occurrence.DefinesOutput = true;
            Occurrence.OutputOpcode = Output->Opcode;
            Occurrence.OutputWitness = Output->Output;
            Occurrence.Authority =
                RelocatedInstructionAddressProofKind::X86PCRelativeCodeAddress;
            Occurrence.SeedInstructionAddr = Cur;
            Occurrence.SeedOpSeq = Output->Seq;
            Occurrence.SeedOpcode = Output->Opcode;
            Occurrence.SeedInputWitness = Output->Inputs[0];
            Occurrence.SeedOutputWitness = Output->Output;
            RelocatedInstructionAddressOccurrences.push_back(
                std::move(Occurrence));
          }
        }
      }

      classifyInsn(Rec);

      // Keep recursive CFG exploration consistent with the decoder's
      // architecture-specific terminator classification.  Trap instructions
      // such as x86 INT3/UD2 lift to an intrinsic rather than a RETURN op, but
      // control does not fall through into the inline data or padding that
      // commonly follows them.  Real branches/returns were already classified
      // from their LowIR ops and retain their more precise handling below.
      //
      // `int3` is the exception: it is resumable, and `__debugbreak()` puts one
      // in the middle of an ordinary function with the epilogue behind it.
      // Whether it ends the block is therefore decided by what follows, which
      // separates that case from the run of `int3` a linker pads with and from
      // the embedded data a trap is often planted in front of.
      if (!Rec.IsBranch && !Rec.IsRet && Dec.isFunctionTerminator(DI)) {
        Rec.IsOpaqueTerminator = true;
        Rec.IsResumableTerminator = Dec.isResumableTrap(DI);
        Rec.IsRet =
            !Rec.IsResumableTerminator || !codeFollowsTrap(Img, Dec, Next);
      }

      // operator[]= returns the stored element, so bind to it directly rather
      // than doing a second tree lookup for the same key on the decode hot
      // path.
      auto &Saved = (Insns[Cur] = std::move(Rec));

      // AArch64 can spell a direct local transfer through an explicit RET
      // register (`adr x16, label; ret x16`).  Fold that dominating constant
      // before treating the unresolved branch as a function-pointer tail call,
      // so recursive descent reaches the local target and keeps it in this
      // function's CFG.
      resolveConstantIndirectBranch(Img, DI.Id, Saved);
      // Relocation-backed interior branches are deliberately resolved later,
      // after every address-taken root has been decoded.  Until then the CFG's
      // predecessor set is incomplete and cannot prove a unique relay path.

      if (Saved.IsRet && !(Saved.IsCond && Saved.IsBranch))
        break;
      if (Saved.IsRet && Saved.IsCond && Saved.IsBranch) {
        if (Saved.BranchTarget != InvalidVA) {
          BlockStarts.insert(Saved.BranchTarget);
          if (!ExploredAddrs.count(Saved.BranchTarget))
            Worklist.push(Saved.BranchTarget);
        }
        break;
      }

      // Tail call: an unconditional direct branch to *another* known function's
      // entry is `call target; ret`, not intra-function control flow. Following
      // the target inlines the callee into this CFG (harmless for an acyclic
      // tail chain, but it fuses mutually-recursive functions into one bogus
      // cyclic CFG).  Rewrite it to an explicit CALL + RETURN and stop
      // exploring here.
      if (Saved.IsBranch && !Saved.IsCall && !Saved.IsCond &&
          !Saved.IsIndirect && isTailCallTarget(Saved.BranchTarget)) {
        rewriteAsTailCall(Saved);
        break;
      }

      if (Saved.IsBranch && !Saved.IsCall) {
        if (Saved.IsIndirect && !Saved.IsCond) {
          auto Targets = resolveJumpTable(Img, Saved);
          if (!Targets.empty()) {
            Saved.JumpTableTargets = std::move(Targets);
            for (va_t T : Saved.JumpTableTargets) {
              BlockStarts.insert(T);
              if (!ExploredAddrs.count(T))
                Worklist.push(T);
            }
          }
        }
        if (Saved.BranchTarget != InvalidVA) {
          BlockStarts.insert(Saved.BranchTarget);
          if (!ExploredAddrs.count(Saved.BranchTarget))
            Worklist.push(Saved.BranchTarget);
        }
        if (Saved.IsCond) {
          va_t Fall = Next;
          BlockStarts.insert(Fall);
          if (!ExploredAddrs.count(Fall))
            Worklist.push(Fall);
        }
        break;
      }

      // An unconditional direct call to a no-return libc function (longjmp /
      // abort / exit / ...) is a control-flow terminator.  At -O2 the compiler
      // emits nothing after it, so continuing would absorb the next function
      // into this CFG.  A predicated ARM call is different: its false path must
      // continue at the following instruction even though its taken path never
      // returns.
      if (Saved.IsCall && !Saved.IsIndirect && isNoReturnCall(Saved)) {
        Saved.IsNoReturnCall = true;
        if (!Saved.IsCond)
          break;
      }

      Cur = Next;
    }
  }
}

std::set<va_t> CFGBuilder::currentRelocatedInstructionTableAnchors(
    const BinaryImage &Img) const {
  std::set<va_t> Anchors;
  for (const RelocatedInstructionAddressOccurrence &Occurrence :
       RelocatedInstructionAddressOccurrences) {
    if (!PublishedReachableInsns.count(Occurrence.InstructionAddr) ||
        Occurrence.OutputMayDepend ||
        Occurrence.Provenance != ConstantAddressProvenance::DataAddress)
      continue;
    if (Img.RelCodeRelocSlots.count(Occurrence.TargetVA) ||
        Img.CodePtrRelocSlots.count(Occurrence.TargetVA))
      Anchors.insert(Occurrence.TargetVA);
  }
  return Anchors;
}

void CFGBuilder::completeExactI386GOTBaseModels(const BinaryImage &Img) {
  // Destruction here was prepaid when the preceding generation published
  // each occurrence.  I386GetPcOccurrence initializes RawPCAuthenticated to
  // false; ELF stage refreshes never set it, while the non-ELF path runs only
  // once after multi-stage resolution, so no per-stage reset scan is needed.
  RelocatedInstructionScalarModelOccurrences.clear();
  I386GOTModelEvidenceIncomplete = false;
  if (Img.Arch != Arch::X86 || Img.getPointerSize() != 4 ||
      !JumpTableProofContextComplete)
    return;

  size_t EvidenceWork =
      std::min<size_t>(limits::kMaxI386GOTModelEvidenceWork,
                       I386GOTModelEvidenceBudgetForTesting.value_or(
                           limits::kMaxI386GOTModelEvidenceWork));
  auto Consume = [&](size_t Amount = 1) {
    if (Amount > EvidenceWork) {
      EvidenceWork = 0;
      I386GOTModelEvidenceIncomplete = true;
      return false;
    }
    EvidenceWork -= Amount;
    return true;
  };
  auto OrderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = (N + 1) / 2)
      ++Work;
    return Work;
  };
  auto ConsumeProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count) {
            Consume(Max);
            return false;
          }
          const size_t Product = Count * Cost;
          if (Product > Max - Total) {
            Consume(Max);
            return false;
          }
          Total += Product;
        }
        return Consume(Total);
      };
  if (!Consume(RelocatedInstructionScalarOperandOccurrences.size()) ||
      !Consume(I386GetPcOccurrences.size()))
    return;

  struct PendingModel {
    va_t FieldVA = InvalidVA;
    const LowOp *Op = nullptr;
    const I386GetPcOccurrence *Seed = nullptr;
    size_t QueryIndex = 0;
  };
  std::vector<JumpTableValueQuery> Queries;
  std::vector<PendingModel> Pending;

  std::map<uint32_t, std::vector<const I386GetPcOccurrence *>> SeedsByPC;
  std::vector<I386GetPcOccurrence *> AuthenticatedGetPcSeeds;
  using SeedIdentity =
      std::tuple<va_t, int, uint8_t, uint64_t, uint16_t, uint8_t, uint64_t>;
  constexpr size_t SeedIdentityWork = 7;
  std::set<SeedIdentity> SeenSeedPoints;
  const size_t GetPcCount = I386GetPcOccurrences.size();
  const size_t OperandCount =
      RelocatedInstructionScalarOperandOccurrences.size();
  // Reserve every outer vector before scanning the graph.  Per-element copy,
  // nested alternative allocation and eventual destruction are charged here;
  // ordered map/set nodes remain charged immediately before insertion.
  if (!ConsumeProducts({{GetPcCount, 4}, {OperandCount, 12}}))
    return;
  AuthenticatedGetPcSeeds.reserve(GetPcCount);
  Queries.reserve(OperandCount);
  Pending.reserve(OperandCount);
  RelocatedInstructionScalarModelOccurrences.reserve(OperandCount);
  for (I386GetPcOccurrence &GetPc : I386GetPcOccurrences) {
    if (!Consume())
      return;
    if (!ConsumeProducts(
            {{1, OrderedLookupWork(PublishedReachableInsns.size())},
             {1, OrderedLookupWork(PersistentCFGRoots.size())},
             {2, OrderedLookupWork(Insns.size())}}))
      return;
    if (GetPc.OutputOpcode != NdOp::COPY || GetPc.OutputWitness.Size != 4 ||
        (!GetPc.OutputWitness.isReg() && !GetPc.OutputWitness.isTemp()) ||
        !GetPc.InputWitness.isTemp() || GetPc.InputWitness.Size != 4 ||
        GetPc.PCValue != GetPc.InstructionAddr ||
        GetPc.CallInstructionAddr == InvalidVA ||
        GetPc.CallInstructionAddr >= GetPc.InstructionAddr ||
        !PublishedReachableInsns.count(GetPc.InstructionAddr) ||
        PersistentCFGRoots.count(GetPc.InstructionAddr))
      continue;

    // The pop is a valid get-PC seed only when the adjacent call is its sole
    // machine-code predecessor.  An address-taken/exception root, another
    // fall-through decode, or a direct/indirect branch into the pop can enter
    // with an arbitrary stack value and must not inherit the call's pushed PC.
    const auto CallIt = Insns.find(GetPc.CallInstructionAddr);
    if (CallIt == Insns.end() || CallIt->second.Size == 0 ||
        CallIt->second.Size > InvalidVA - GetPc.CallInstructionAddr ||
        GetPc.CallInstructionAddr + CallIt->second.Size !=
            GetPc.InstructionAddr)
      continue;
    const LowOp *PushAdjust = nullptr;
    const LowOp *PushStore = nullptr;
    if (!Consume(CallIt->second.Ops.size()))
      return;
    for (const LowOp &Op : CallIt->second.Ops) {
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2 &&
          Op.Output.isReg() && Op.Output.Size == 4 &&
          Op.Inputs[0] == Op.Output && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Size == 4 && Op.Inputs[1].Offset == 4) {
        if (PushAdjust)
          return;
        PushAdjust = &Op;
      }
      if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          Op.Inputs[0].isReg() && Op.Inputs[0].Size == 4 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Size == 4 &&
          static_cast<uint32_t>(Op.Inputs[1].Offset) == GetPc.PCValue) {
        if (PushStore)
          return;
        PushStore = &Op;
      }
    }
    if (!PushAdjust || !PushStore ||
        PushStore->Inputs[0] != PushAdjust->Output ||
        PushAdjust->Seq >= PushStore->Seq)
      continue;
    bool HasAlternateEntry = false;
    for (const auto &[Addr, Rec] : Insns) {
      if (!Consume() || !Consume(Rec.JumpTableTargets.size()))
        return;
      if (Addr != GetPc.CallInstructionAddr && Rec.Size != 0 &&
          Rec.Size <= InvalidVA - Addr &&
          Addr + Rec.Size == GetPc.InstructionAddr)
        HasAlternateEntry = true;
      if ((Rec.BranchTarget == GetPc.InstructionAddr) ||
          (Rec.IsCall && Rec.Immediate &&
           *Rec.Immediate == GetPc.InstructionAddr) ||
          llvm::is_contained(Rec.JumpTableTargets, GetPc.InstructionAddr))
        HasAlternateEntry = true;
    }
    if (HasAlternateEntry)
      continue;
    const SeedIdentity Identity =
        std::make_tuple(GetPc.InstructionAddr, GetPc.OpSeq,
                        static_cast<uint8_t>(GetPc.OutputWitness.Space),
                        GetPc.OutputWitness.Offset, GetPc.OutputWitness.Size,
                        static_cast<uint8_t>(GetPc.OutputWitness.Provenance),
                        GetPc.OutputWitness.AddressOwnerVA);
    if (!ConsumeProducts(
            {{SeedIdentityWork, OrderedLookupWork(SeenSeedPoints.size())},
             {SeedIdentityWork, 2},
             // Set-node allocation and its eventual destruction.
             {1, 2}}))
      return;
    if (!SeenSeedPoints.insert(Identity).second)
      return;
    const auto RecIt = Insns.find(GetPc.InstructionAddr);
    if (RecIt == Insns.end() || RecIt->second.IsInstructionGuard)
      continue;
    const LowOp *Producer = nullptr;
    for (const LowOp &Op : RecIt->second.Ops) {
      if (!Consume())
        return;
      if (Op.Addr == GetPc.InstructionAddr && Op.Seq == GetPc.OpSeq &&
          Op.Opcode == GetPc.OutputOpcode && Op.Output == GetPc.OutputWitness) {
        if (Producer)
          return;
        Producer = &Op;
      }
    }
    if (!Producer || Producer->NumInputs != 1 ||
        Producer->Inputs[0] != GetPc.InputWitness)
      continue;
    const LowOp *PopLoad = nullptr;
    const LowOp *LegacyPopAdjust = nullptr;
    const LowOp *StagedPopAdjust = nullptr;
    const LowOp *StagedStackCommit = nullptr;
    for (const LowOp &Op : RecIt->second.Ops) {
      if (!Consume())
        return;
      if (Op.Addr == GetPc.InstructionAddr && Op.Opcode == NdOp::LOAD &&
          Op.NumInputs == 1 && Op.Output == Producer->Inputs[0] &&
          Op.Inputs[0].isReg() && Op.Output.isTemp() && Op.Output.Size == 4 &&
          Op.Inputs[0].Size == 4) {
        if (PopLoad)
          return;
        PopLoad = &Op;
      }
      const bool IsStackAdjust =
          Op.Addr == GetPc.InstructionAddr && Op.Opcode == NdOp::INT_ADD &&
          Op.NumInputs == 2 && Op.Output.Size == 4 && Op.Inputs[0].isReg() &&
          Op.Inputs[0].Size == 4 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Size == 4 && Op.Inputs[1].Offset == 4;
      if (IsStackAdjust) {
        if (Op.Output.isReg() && Op.Inputs[0] == Op.Output) {
          if (LegacyPopAdjust)
            return;
          LegacyPopAdjust = &Op;
        } else if (Op.Output.isTemp()) {
          if (StagedPopAdjust)
            return;
          StagedPopAdjust = &Op;
        }
      }
      if (&Op != Producer && Op.Addr == GetPc.InstructionAddr &&
          Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && Op.Output.isReg() &&
          Op.Output.Size == 4 && Op.Inputs[0].isTemp() &&
          Op.Inputs[0].Size == 4) {
        if (StagedStackCommit)
          return;
        StagedStackCommit = &Op;
      }
    }
    if (!PopLoad || PopLoad->Inputs[0] != PushStore->Inputs[0])
      continue;
    const bool LegacyShape =
        LegacyPopAdjust && LegacyPopAdjust->Output == PopLoad->Inputs[0] &&
        PopLoad->Seq < Producer->Seq && Producer->Seq < LegacyPopAdjust->Seq;
    const bool StagedShape =
        StagedPopAdjust && StagedStackCommit &&
        StagedPopAdjust->Inputs[0] == PopLoad->Inputs[0] &&
        StagedStackCommit->Output == PopLoad->Inputs[0] &&
        StagedStackCommit->Inputs[0] == StagedPopAdjust->Output &&
        PopLoad->Seq < StagedPopAdjust->Seq &&
        StagedPopAdjust->Seq < StagedStackCommit->Seq &&
        StagedStackCommit->Seq < Producer->Seq;
    if (LegacyShape == StagedShape)
      continue;
    if (!ConsumeProducts(
            {{1, OrderedLookupWork(SeedsByPC.size())},
             // Possible map node, nested vector element/allocation/destruction,
             // and the authenticated-seed vector element/destruction.
             {1, 7}}))
      return;
    SeedsByPC[GetPc.PCValue].push_back(&GetPc);
    AuthenticatedGetPcSeeds.push_back(&GetPc);
  }

  // Mach-O/COFF PIC arithmetic has no ELF R_386_GOTPC scalar field to bind,
  // but the same call-next/POP seed is still an exact architectural PC once
  // the CFG proof above succeeds.  Publish that fact without rewriting the
  // ordinary LOAD/COPY: Low-to-Med will bind the exact surviving occurrence,
  // and the LLVM data resolver may fold only arithmetic rooted at that bound
  // value.  ELF keeps its stricter combined call/POP + exact GOTPC contract
  // below, so a raw encoded displacement cannot borrow this permission.
  if (!Img.isELF()) {
    if (!ConsumeProducts({{AuthenticatedGetPcSeeds.size(), 2}}))
      return;
    for (I386GetPcOccurrence *Seed : AuthenticatedGetPcSeeds)
      Seed->RawPCAuthenticated = true;
    return;
  }
  if (Img.I386GOTPCFields.empty())
    return;

  std::set<std::tuple<va_t, va_t, int>> SeenOperandPoints;
  for (const RelocatedInstructionScalarOperandOccurrence &Operand :
       RelocatedInstructionScalarOperandOccurrences) {
    constexpr size_t OperandPointKeyWork = 3;
    if (!Consume(3) ||
        !ConsumeProducts(
            {{1, OrderedLookupWork(PublishedReachableInsns.size())},
             {OperandPointKeyWork, OrderedLookupWork(SeenOperandPoints.size())},
             {OperandPointKeyWork, 2},
             // Set-node allocation and its eventual destruction.
             {1, 2},
             {2, OrderedLookupWork(Img.I386GOTPCFields.size())},
             {1, OrderedLookupWork(Insns.size())},
             {1, OrderedLookupWork(SeedsByPC.size())}}))
      return;
    if (Operand.Kind != RelocatedInstructionScalarOperandOccurrence::
                            OperandKind::I386ELFGOTPC ||
        Operand.Width != 4 || Operand.InputIndex >= 2 ||
        Operand.Opcode != NdOp::INT_ADD || Operand.OutputWitness.Size != 4 ||
        (!Operand.OutputWitness.isReg() && !Operand.OutputWitness.isTemp()) ||
        !PublishedReachableInsns.count(Operand.InstructionAddr) ||
        !SeenOperandPoints
             .insert({Operand.FieldVA, Operand.InstructionAddr, Operand.OpSeq})
             .second)
      return;

    const auto FieldIt = Img.I386GOTPCFields.find(Operand.FieldVA);
    const auto InsnIt = Insns.find(Operand.InstructionAddr);
    if (FieldIt == Img.I386GOTPCFields.end() || InsnIt == Insns.end() ||
        InsnIt->second.IsInstructionGuard || InsnIt->second.Size == 0 ||
        InsnIt->second.Size > InvalidVA - Operand.InstructionAddr ||
        Operand.FieldVA < Operand.InstructionAddr ||
        Operand.FieldVA >= Operand.InstructionAddr + InsnIt->second.Size ||
        FieldIt->second.EncodedValue !=
            static_cast<uint32_t>(Operand.EncodedValue) ||
        static_cast<uint32_t>(FieldIt->second.EncodedValue +
                              FieldIt->second.ExpectedPCValue) != 0)
      continue;

    unsigned InstructionFields = 0;
    auto Field = Img.I386GOTPCFields.lower_bound(Operand.InstructionAddr);
    for (; Field != Img.I386GOTPCFields.end() &&
           Field->first < Operand.InstructionAddr + InsnIt->second.Size;
         ++Field) {
      if (!Consume())
        return;
      ++InstructionFields;
    }
    if (InstructionFields != 1)
      continue;

    const LowOp *Candidate = nullptr;
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (!Consume())
        return;
      if (Op.Addr == Operand.InstructionAddr && Op.Seq == Operand.OpSeq &&
          Op.Opcode == Operand.Opcode && Op.Output == Operand.OutputWitness) {
        if (Candidate)
          return;
        Candidate = &Op;
      }
    }
    if (!Candidate || Candidate->NumInputs != 2 ||
        Operand.InputIndex >= Candidate->NumInputs)
      continue;
    const NdVar &Immediate = Candidate->Inputs[Operand.InputIndex];
    const NdVar &Base = Candidate->Inputs[1 - Operand.InputIndex];
    if (!Immediate.isConst() || Immediate.Size != 4 ||
        Immediate.Provenance != ConstantAddressProvenance::Scalar ||
        static_cast<uint32_t>(Immediate.Offset) !=
            FieldIt->second.EncodedValue ||
        (!Base.isReg() && !Base.isTemp()) || Base.Size != 4)
      continue;

    const auto Seeds = SeedsByPC.find(FieldIt->second.ExpectedPCValue);
    if (Seeds == SeedsByPC.end() || Seeds->second.size() != 1)
      continue;
    const I386GetPcOccurrence &Seed = *Seeds->second.front();
    if (!Consume(2))
      return;
    JumpTableValueQuery Query;
    Query.Candidate = Base;
    Query.UseAddr = Candidate->Addr;
    Query.UseSeq = Candidate->Seq;
    Query.Alternatives.push_back({Seed.OutputWitness, Seed.InstructionAddr,
                                  Seed.OpSeq, /*DefinedAtPoint=*/true});
    Query.RequireExactAddressOwner = true;
    Pending.push_back({Operand.FieldVA, Candidate, &Seed, Queries.size()});
    Queries.push_back(std::move(Query));
  }

  if (Queries.empty())
    return;
  // Reserve the transactional commit tail before the graph resolver sees the
  // remaining balance.  A complete query must still scan every pending model
  // and release its result buffer; those operations cannot occur after graph
  // propagation has consumed the final evidence unit.
  if (!ConsumeProducts({{Pending.size(), 1}, {1, 1}}))
    return;
  bool AnalysisComplete = false;
  const std::vector<bool> Matches = tableValuesMatchAtUses(
      Queries, &AnalysisComplete, /*QueryAnalysisComplete=*/nullptr,
      /*CandidateBranchOverride=*/InvalidVA,
      /*CandidateTargetsOverride=*/nullptr, &EvidenceWork);
  if (!AnalysisComplete || Matches.size() != Queries.size()) {
    I386GOTModelEvidenceIncomplete = true;
    return;
  }

  for (const PendingModel &Model : Pending) {
    if (!Model.Op || !Model.Seed || Model.QueryIndex >= Matches.size() ||
        !Matches[Model.QueryIndex])
      continue;
    RelocatedInstructionScalarModelOccurrence Occurrence;
    Occurrence.FieldVA = Model.FieldVA;
    Occurrence.InstructionAddr = Model.Op->Addr;
    Occurrence.OpSeq = Model.Op->Seq;
    Occurrence.Width = 4;
    Occurrence.Model = RelocatedInstructionScalarModelOccurrence::ModelKind::
        I386ELFGOTBaseZero;
    Occurrence.OutputOpcode = Model.Op->Opcode;
    Occurrence.OutputWitness = Model.Op->Output;
    Occurrence.SeedInstructionAddr = Model.Seed->InstructionAddr;
    Occurrence.SeedOpSeq = Model.Seed->OpSeq;
    Occurrence.SeedOpcode = Model.Seed->OutputOpcode;
    Occurrence.SeedOutputWitness = Model.Seed->OutputWitness;
    RelocatedInstructionScalarModelOccurrences.push_back(std::move(Occurrence));
  }
}

void CFGBuilder::completeExactAArch64PageBases(const BinaryImage &Img) {
  if (Img.Arch != Arch::AArch64)
    return;

  auto sameRegister = [](const NdVar &Left, const NdVar &Right) {
    return Left.isReg() && Right.isReg() && Left.Offset == Right.Offset &&
           Left.Size == Right.Size;
  };
  auto overlapsRegister = [](const NdVar &Left, const NdVar &Right) {
    if (!Left.isReg() || !Right.isReg() || Left.Size == 0 || Right.Size == 0)
      return false;
    const uint64_t LeftEnd = Left.Offset + Left.Size;
    const uint64_t RightEnd = Right.Offset + Right.Size;
    return Left.Offset < RightEnd && Right.Offset < LeftEnd;
  };

  // ADRP always clears the architectural low 12 bits, independent of the
  // operating system's VM page size.
  constexpr uint64_t AArch64PageMask = 0xfff;
  constexpr unsigned MaxLookaheadInstructions = 32;

  for (auto RootIt = Insns.begin(); RootIt != Insns.end(); ++RootIt) {
    InsnRecord &RootRec = RootIt->second;
    if (RootRec.Ops.size() != 1)
      continue;
    LowOp &Root = RootRec.Ops.front();
    if (Root.Opcode != NdOp::COPY || Root.NumInputs != 1 ||
        !Root.Output.isReg() || Root.Output.Size != Img.getPointerSize() ||
        !Root.Inputs[0].isConst() ||
        Root.Inputs[0].Provenance != ConstantAddressProvenance::AddressFragment)
      continue;

    const va_t PageBase = Root.Inputs[0].Offset;
    const auto PageMaterialization =
        Img.InstructionPageAddressFragments.find(RootRec.Addr);
    const bool HasAuthenticatedPage =
        PageMaterialization != Img.InstructionPageAddressFragments.end() &&
        PageMaterialization->second.TargetOwnerVA != InvalidVA &&
        Img.relocatedTargetBelongsToOwner(
            PageMaterialization->second.TargetVA,
            PageMaterialization->second.TargetOwnerVA) &&
        (PageMaterialization->second.TargetVA & ~AArch64PageMask) == PageBase;
    const bool CanAuthenticatePageByDereference =
        Img.hasObjectDataProvenance(PageBase) &&
        !Img.hasExecutableCodeOwnerAt(PageBase);
    if ((PageBase & AArch64PageMask) != 0 ||
        (!HasAuthenticatedPage && !CanAuthenticatePageByDereference))
      continue;
    if (HasAuthenticatedPage)
      Root.Inputs[0].AddressOwnerVA = PageMaterialization->second.TargetOwnerVA;

    va_t OwnerVA = InvalidVA;
    if (CanAuthenticatePageByDereference) {
      const Segment *OwnerSegment = Img.getSegmentFor(PageBase);
      if (!OwnerSegment)
        continue;
      const Section *OwnerSection = Img.getSectionFor(PageBase);
      OwnerVA = OwnerSection ? OwnerSection->VA : OwnerSegment->VA;
    }
    const NdVar PageRegister = Root.Output;

    bool ProvedExactDereference = false;
    va_t ExpectedAddress = RootRec.Addr + RootRec.Size;
    unsigned Lookahead = 0;
    for (auto UseIt = std::next(RootIt);
         UseIt != Insns.end() && Lookahead++ < MaxLookaheadInstructions;
         ++UseIt) {
      InsnRecord &UseRec = UseIt->second;
      if (UseRec.Addr != ExpectedAddress)
        break;

      // Temporary ids are instruction-local.  Track only expressions that are
      // an exact byte offset from the still-live ADRP destination; this is
      // enough to distinguish [xN] from [xN,#off] without treating unrelated
      // constants in the instruction as addresses.
      std::vector<std::pair<NdVar, uint64_t>> RelativeValues;
      auto relativeOffset = [&](const NdVar &Value) -> std::optional<uint64_t> {
        if (sameRegister(Value, PageRegister))
          return 0;
        for (auto It = RelativeValues.rbegin(); It != RelativeValues.rend();
             ++It)
          if (It->first.Space == Value.Space &&
              It->first.Offset == Value.Offset && It->first.Size == Value.Size)
            return It->second;
        return std::nullopt;
      };
      auto scalarValue = [](const NdVar &Value) -> std::optional<uint64_t> {
        if (!Value.isConst() ||
            Value.Provenance != ConstantAddressProvenance::Scalar)
          return std::nullopt;
        return Value.Offset;
      };

      bool RedefinedPageRegister = false;
      for (const LowOp &Op : UseRec.Ops) {
        if (CanAuthenticatePageByDereference &&
            (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
            Op.NumInputs >= 1) {
          if (auto Offset = relativeOffset(Op.Inputs[0]);
              Offset && *Offset == 0) {
            ProvedExactDereference = true;
            break;
          }
        }

        std::optional<uint64_t> Derived;
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
          Derived = relativeOffset(Op.Inputs[0]);
        } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
          if (auto Base = relativeOffset(Op.Inputs[0])) {
            if (auto Delta = scalarValue(Op.Inputs[1]))
              Derived = *Base + *Delta;
          } else if (auto Base = relativeOffset(Op.Inputs[1])) {
            if (auto Delta = scalarValue(Op.Inputs[0]))
              Derived = *Base + *Delta;
          }
        } else if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2) {
          if (auto Base = relativeOffset(Op.Inputs[0]))
            if (auto Delta = scalarValue(Op.Inputs[1]))
              Derived = *Base - *Delta;
        }

        if (Op.Output.Size != 0 && Derived)
          RelativeValues.emplace_back(Op.Output, *Derived);
        if (overlapsRegister(Op.Output, PageRegister)) {
          RedefinedPageRegister = true;
          break;
        }
      }

      if (ProvedExactDereference || RedefinedPageRegister)
        break;
      if (UseRec.IsBranch || UseRec.IsCall || UseRec.IsRet ||
          UseRec.IsOpaqueTerminator)
        break;
      ExpectedAddress = UseRec.Addr + UseRec.Size;
    }

    if (!ProvedExactDereference)
      continue;
    Root.Inputs[0].Provenance = ConstantAddressProvenance::DataAddress;
    Root.Inputs[0].AddressOwnerVA = OwnerVA;
  }

  if (!JumpTableProofContextComplete)
    return;

  // Output certificates are rebuilt from the current final proof graph.  A
  // certificate from an earlier provisional CFG must not survive after a new
  // root/predecessor makes the PAGEOFF base ambiguous.
  RelocatedInstructionAddressOccurrences.erase(
      std::remove_if(RelocatedInstructionAddressOccurrences.begin(),
                     RelocatedInstructionAddressOccurrences.end(),
                     [](const RelocatedInstructionAddressOccurrence &Item) {
                       return Item.DefinesOutput;
                     }),
      RelocatedInstructionAddressOccurrences.end());

  struct AuthenticatedPageDefinition {
    va_t TargetVA = InvalidVA;
    va_t TargetOwnerVA = InvalidVA;
    JumpTableValueOccurrence Occurrence;
  };
  std::vector<AuthenticatedPageDefinition> PageDefinitions;
  for (const auto &[Addr, Rec] : Insns) {
    const auto Materialized = Img.InstructionPageAddressFragments.find(Addr);
    if (Materialized == Img.InstructionPageAddressFragments.end() ||
        Materialized->second.TargetOwnerVA == InvalidVA ||
        !Img.relocatedTargetBelongsToOwner(
            Materialized->second.TargetVA,
            Materialized->second.TargetOwnerVA) ||
        Rec.Ops.size() != 1)
      continue;
    const LowOp &Op = Rec.Ops.front();
    const va_t PageBase = Materialized->second.TargetVA & ~AArch64PageMask;
    if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 || !Op.Output.isReg() ||
        Op.Output.Size != Img.getPointerSize() || !Op.Inputs[0].isConst() ||
        (Op.Inputs[0].Provenance !=
             ConstantAddressProvenance::AddressFragment &&
         Op.Inputs[0].Provenance != ConstantAddressProvenance::DataAddress) ||
        Op.Inputs[0].Offset != PageBase)
      continue;
    PageDefinitions.push_back(
        {Materialized->second.TargetVA,
         Materialized->second.TargetOwnerVA,
         {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true}});
  }

  struct PendingMaterialization {
    const LowOp *Op = nullptr;
    RelocatedInstructionAddressMaterialization Materialized;
    size_t ExactQuery = std::numeric_limits<size_t>::max();
    size_t MayQuery = 0;
  };
  std::vector<JumpTableValueQuery> Queries;
  std::vector<PendingMaterialization> Pending;
  for (const auto &[Addr, Rec] : Insns) {
    const auto Materialized = Img.InstructionAddressMaterializations.find(Addr);
    if (Materialized == Img.InstructionAddressMaterializations.end() ||
        Materialized->second.TargetOwnerVA == InvalidVA ||
        !Img.relocatedTargetBelongsToOwner(
            Materialized->second.TargetVA,
            Materialized->second.TargetOwnerVA) ||
        Rec.IsInstructionGuard)
      continue;
    const va_t PageBase = Materialized->second.TargetVA & ~AArch64PageMask;
    const uint64_t PageOffset = Materialized->second.TargetVA & AArch64PageMask;
    for (const LowOp &Op : Rec.Ops) {
      if (Op.Opcode != NdOp::INT_ADD || Op.NumInputs != 2 ||
          !Op.Output.isReg() || Op.Output.Size != Img.getPointerSize())
        continue;
      int BaseSide = -1;
      for (int Side = 0; Side < 2; ++Side) {
        const NdVar &Immediate = Op.Inputs[1 - Side];
        if ((!Op.Inputs[Side].isReg() && !Op.Inputs[Side].isTemp()) ||
            Op.Inputs[Side].Size != Img.getPointerSize() ||
            !Immediate.isConst() ||
            Immediate.Provenance != ConstantAddressProvenance::Scalar ||
            Immediate.Offset != PageOffset)
          continue;
        if (BaseSide != -1) {
          BaseSide = -2;
          break;
        }
        BaseSide = Side;
      }
      if (BaseSide < 0)
        continue;

      std::vector<JumpTableValueOccurrence> ExactAlternatives;
      std::vector<JumpTableValueOccurrence> AllPageAlternatives;
      for (const AuthenticatedPageDefinition &Page : PageDefinitions)
        if (Page.Occurrence.Value.Size == Op.Inputs[BaseSide].Size) {
          AllPageAlternatives.push_back(Page.Occurrence);
          if ((Page.TargetVA & ~AArch64PageMask) == PageBase)
            ExactAlternatives.push_back(Page.Occurrence);
        }
      if (AllPageAlternatives.empty())
        continue;
      size_t ExactQuery = std::numeric_limits<size_t>::max();
      if (!ExactAlternatives.empty()) {
        ExactQuery = Queries.size();
        Queries.push_back({Op.Inputs[BaseSide], Op.Addr, Op.Seq,
                           std::move(ExactAlternatives), false, false});
      }
      const size_t MayQuery = Queries.size();
      JumpTableValueQuery MayDepend{Op.Inputs[BaseSide],
                                    Op.Addr,
                                    Op.Seq,
                                    std::move(AllPageAlternatives),
                                    false,
                                    false};
      MayDepend.Relation = JumpTableValueRelation::MayDepend;
      Queries.push_back(std::move(MayDepend));
      Pending.push_back({&Op, Materialized->second, ExactQuery, MayQuery});
    }
  }

  if (!Queries.empty()) {
    bool AnalysisComplete = false;
    const std::vector<bool> Matches =
        tableValuesMatchAtUses(Queries, &AnalysisComplete);
    const bool Complete = AnalysisComplete && Matches.size() == Queries.size();
    for (const PendingMaterialization &Candidate : Pending) {
      if (!Candidate.Op)
        continue;
      const bool HasExactQuery =
          Candidate.ExactQuery != std::numeric_limits<size_t>::max();
      const bool IsExact = Complete && HasExactQuery &&
                           Candidate.ExactQuery < Matches.size() &&
                           Matches[Candidate.ExactQuery];
      const bool MayDepend = Complete && Candidate.MayQuery < Matches.size() &&
                             Matches[Candidate.MayQuery];
      // A complete query that proves no authenticated page reaches this ADD is
      // not address evidence: an unrelated/sibling ADRP must not poison module
      // arbitration.  An incomplete proof is different — the PAGEOFF output
      // may still carry a reachable page definition, so publish an explicit
      // partial certificate and let module arbitration fail closed.
      if (Complete && !IsExact && !MayDepend)
        continue;
      const LowOp &Op = *Candidate.Op;
      const RelocatedInstructionAddressMaterialization &Materialized =
          Candidate.Materialized;
      RelocatedInstructionAddressOccurrence Occurrence;
      Occurrence.FieldVA = Op.Addr;
      Occurrence.InstructionAddr = Op.Addr;
      Occurrence.OpSeq = Op.Seq;
      Occurrence.TargetVA = Materialized.TargetVA;
      Occurrence.TargetOwnerVA = Materialized.TargetOwnerVA;
      Occurrence.Width = 4;
      Occurrence.Provenance =
          Img.hasExecutableCodeOwnerAt(Materialized.TargetVA)
              ? ConstantAddressProvenance::CodeAddress
              : ConstantAddressProvenance::DataAddress;
      Occurrence.DefinesOutput = true;
      Occurrence.OutputMayDepend = !IsExact;
      Occurrence.OutputOpcode = Op.Opcode;
      Occurrence.OutputWitness = Op.Output;
      if (!llvm::is_contained(RelocatedInstructionAddressOccurrences,
                              Occurrence))
        RelocatedInstructionAddressOccurrences.push_back(std::move(Occurrence));
    }
  }

  // A linked AArch64 image may no longer carry PAGE/PAGEOFF relocation
  // records even though the decoded ADRP/add pair still computes an exact
  // image address.  Do not turn numeric foldability into provenance.  The
  // fallback below publishes only a fully occurrence-bound chain whose final
  // full-width scalar result is immediately dereferenced in one immutable
  // object-data owner.  Relocatable objects and any instruction with loader
  // materialization authority remain exclusively on the path above.
  if (Img.IsRelocatable || Img.getPointerSize() == 0 ||
      Img.getPointerSize() > sizeof(va_t))
    return;

  const uint16_t PointerSize = Img.getPointerSize();
  const unsigned PointerBits = static_cast<unsigned>(PointerSize) * 8;
  const uint64_t PointerMask = PointerBits == 64
                                   ? std::numeric_limits<uint64_t>::max()
                                   : (uint64_t{1} << PointerBits) - 1;
  auto canonicalArithmeticScalar =
      [&](const LowOp &Op, int BaseSide) -> std::optional<uint64_t> {
    if (BaseSide < 0 || BaseSide > 1 || !Op.Inputs[1 - BaseSide].isConst() ||
        Op.Inputs[1 - BaseSide].Provenance != ConstantAddressProvenance::Scalar)
      return std::nullopt;
    const NdVar &Scalar = Op.Inputs[1 - BaseSide];
    if (Scalar.Size == PointerSize)
      return Scalar.Offset & PointerMask;

    // AArch64Lifter intentionally retains a compact 32-bit NdVar for an
    // encoded ADD/SUB immediate whose value fits in uint32_t; INT_ADD/SUB then
    // applies that non-negative displacement in the destination width.  Admit
    // this mixed-width form only when the exact machine word independently
    // proves a 64-bit, non-flag-setting immediate instruction and its imm12 +
    // optional lsl #12 value is exactly the scalar carried by this LowOp.
    if (PointerSize != 8 || Scalar.Size != 4 || BaseSide != 0)
      return std::nullopt;
    const uint8_t *Bytes = Img.readVA(Op.Addr, sizeof(uint32_t));
    if (!Bytes)
      return std::nullopt;
    const uint32_t Word = readLE<uint32_t>(Bytes);
    if ((Word & 0x1f000000u) != 0x11000000u || (Word & 0x80000000u) == 0 ||
        (Word & 0x20000000u) != 0 ||
        (((Word & 0x40000000u) != 0) != (Op.Opcode == NdOp::INT_SUB)))
      return std::nullopt;
    const uint64_t Encoded = uint64_t((Word >> 10) & 0xfffu)
                             << (((Word >> 22) & 1u) ? 12 : 0);
    if (Encoded != Scalar.Offset)
      return std::nullopt;
    return Encoded;
  };
  auto checkedArithmetic = [&](va_t Base, const LowOp &Op,
                               int BaseSide) -> std::optional<va_t> {
    if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
        Op.NumInputs != 2 || BaseSide < 0 || BaseSide > 1 ||
        (Op.Opcode == NdOp::INT_SUB && BaseSide != 0) ||
        Op.Output.Size != PointerSize ||
        Op.Inputs[BaseSide].Size != PointerSize || Base > PointerMask)
      return std::nullopt;
    const std::optional<uint64_t> Delta =
        canonicalArithmeticScalar(Op, BaseSide);
    if (!Delta)
      return std::nullopt;
    if (Op.Opcode == NdOp::INT_ADD) {
      if (*Delta > PointerMask - Base)
        return std::nullopt;
      return Base + *Delta;
    }
    if (*Delta > Base)
      return std::nullopt;
    return Base - *Delta;
  };

  auto uniqueImmutableDataOwner =
      [&](va_t Address, uint16_t AccessSize) -> std::optional<va_t> {
    if (AccessSize == 0 || AccessSize - 1 > InvalidVA - Address)
      return std::nullopt;
    const va_t Last = Address + AccessSize - 1;
    if (!Img.hasObjectDataProvenance(Address) ||
        !Img.hasObjectDataProvenance(Last) ||
        Img.hasExecutableCodeOwnerAt(Address) ||
        Img.hasExecutableCodeOwnerAt(Last) ||
        isRuntimeWritableAddress(Img, Address) ||
        isRuntimeWritableAddress(Img, Last))
      return std::nullopt;

    const Section *OnlySection = nullptr;
    for (const Section &Section : Img.Sections) {
      if (!Section.isReadable() || Section.isExecutable() ||
          !Section.contains(Address) || !Section.contains(Last))
        continue;
      if (OnlySection)
        return std::nullopt;
      OnlySection = &Section;
    }
    if (OnlySection)
      return OnlySection->VA;

    const Segment *OnlySegment = nullptr;
    for (const Segment &Segment : Img.Segments) {
      if (!Segment.isReadable() || Segment.isExecutable() ||
          !Segment.contains(Address) || !Segment.contains(Last) ||
          Address < Segment.VA || Last < Segment.VA ||
          Last - Segment.VA >= Segment.Data.size())
        continue;
      if (OnlySegment)
        return std::nullopt;
      OnlySegment = &Segment;
    }
    return OnlySegment ? std::optional<va_t>(OnlySegment->VA) : std::nullopt;
  };

  struct RelocationFreeCandidate {
    RelocatedInstructionAddressOccurrence Occurrence;
    std::vector<size_t> QueryIndices;
  };
  std::vector<JumpTableValueQuery> RelocationFreeQueries;
  std::vector<RelocationFreeCandidate> RelocationFreeCandidates;
  for (auto RootIt = Insns.begin(); RootIt != Insns.end(); ++RootIt) {
    const InsnRecord &RootRec = RootIt->second;
    if (RootRec.IsInstructionGuard || RootRec.IsBranch || RootRec.IsCall ||
        RootRec.IsRet || RootRec.IsOpaqueTerminator ||
        RootRec.IsResumableTerminator || RootRec.Size == 0 ||
        RootRec.Size > InvalidVA - RootRec.Addr || RootRec.Ops.size() != 1)
      continue;
    const LowOp &Root = RootRec.Ops.front();
    if (Root.Opcode != NdOp::COPY || Root.NumInputs != 1 ||
        !Root.Output.isReg() || Root.Output.Size != PointerSize ||
        !Root.Inputs[0].isConst() || Root.Inputs[0].Size != PointerSize ||
        Root.Inputs[0].Provenance !=
            ConstantAddressProvenance::AddressFragment ||
        Root.Inputs[0].Offset > PointerMask ||
        (Root.Inputs[0].Offset & AArch64PageMask) != 0)
      continue;

    NdVar CurrentValue = Root.Output;
    va_t CurrentAddress = Root.Inputs[0].Offset;
    JumpTableValueOccurrence CurrentDefinition{Root.Output, Root.Addr, Root.Seq,
                                               /*DefinedAtPoint=*/true};
    std::vector<RelocatedInstructionAddressArithmeticStep> Steps;
    std::vector<JumpTableValueQuery> CandidateQueries;
    va_t ExpectedAddress = RootRec.Addr + RootRec.Size;

    for (auto UseIt = std::next(RootIt);
         UseIt != Insns.end() && Steps.size() < MaxLookaheadInstructions;
         ++UseIt) {
      const InsnRecord &UseRec = UseIt->second;
      if (UseRec.Addr != ExpectedAddress || UseRec.Size == 0 ||
          UseRec.Size > InvalidVA - UseRec.Addr || UseRec.IsInstructionGuard ||
          UseRec.IsBranch || UseRec.IsCall || UseRec.IsRet ||
          UseRec.IsOpaqueTerminator || UseRec.IsResumableTerminator)
        break;

      // AArch64 unsigned-offset LDR/STR folds the address calculation into the
      // memory instruction.  Its LowIR is instruction-local rather than three
      // records:
      //
      //   COPY    tmp, page-reg
      //   INT_ADD tmp, tmp, #offset
      //   LOAD    value, tmp
      //
      // The relocation-free proof still owns the exact ADD output occurrence;
      // the immediately following memory op is its dereference witness.  Keep
      // this narrow shape separate from the ordinary cross-instruction walk:
      // every transport and the sole memory effect must occur in this exact
      // InsnRecord, and the usual all-path value queries remain authoritative.
      const LowOp *LocalArithmetic = nullptr;
      const LowOp *LocalDereference = nullptr;
      LowMemoryOperandView LocalDereferenceMemory;
      NdVar LocalDereferenceAddress;
      int LocalBaseSide = -1;
      NdVar LocalAddressValue = CurrentValue;
      bool InvalidLocalShape = false;
      for (const LowOp &Op : UseRec.Ops) {
        const LowMemoryOperandView Memory = lowMemoryOperands(Op);
        if (!LocalArithmetic) {
          if (Memory.Complete) {
            InvalidLocalShape = true;
            break;
          }
          int CandidateBase = -1;
          if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
              Op.NumInputs == 2 && (Op.Output.isReg() || Op.Output.isTemp()) &&
              Op.Output.Size == PointerSize) {
            const bool LeftAlias = Op.Inputs[0] == LocalAddressValue;
            const bool RightAlias = Op.Inputs[1] == LocalAddressValue;
            if (LeftAlias && Op.Inputs[1].isConst())
              CandidateBase = 0;
            if (Op.Opcode == NdOp::INT_ADD && RightAlias &&
                Op.Inputs[0].isConst()) {
              if (CandidateBase >= 0)
                CandidateBase = -2;
              else
                CandidateBase = 1;
            }
          }
          if (CandidateBase >= 0 &&
              checkedArithmetic(CurrentAddress, Op, CandidateBase)) {
            LocalArithmetic = &Op;
            LocalBaseSide = CandidateBase;
            LocalAddressValue = Op.Output;
            continue;
          }
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0] == LocalAddressValue &&
              (Op.Output.isReg() || Op.Output.isTemp()) &&
              Op.Output.Size == PointerSize) {
            LocalAddressValue = Op.Output;
            continue;
          }
          if (Op.Output == LocalAddressValue ||
              overlapsRegister(Op.Output, LocalAddressValue)) {
            InvalidLocalShape = true;
            break;
          }
          continue;
        }

        if (Memory.Complete) {
          if (LocalDereference ||
              (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
              !Memory.Address || *Memory.Address != LocalAddressValue ||
              Op.Output == LocalAddressValue ||
              overlapsRegister(Op.Output, LocalAddressValue)) {
            InvalidLocalShape = true;
            break;
          }
          LocalDereference = &Op;
          LocalDereferenceMemory = Memory;
          LocalDereferenceAddress = *Memory.Address;
          continue;
        }
        if (!LocalDereference && Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0] == LocalAddressValue &&
            (Op.Output.isReg() || Op.Output.isTemp()) &&
            Op.Output.Size == PointerSize) {
          LocalAddressValue = Op.Output;
          continue;
        }
        if (!LocalDereference || Op.Output == LocalAddressValue ||
            overlapsRegister(Op.Output, LocalAddressValue)) {
          InvalidLocalShape = true;
          break;
        }
      }

      if (!InvalidLocalShape && LocalArithmetic && LocalBaseSide >= 0 &&
          LocalDereference && LocalDereferenceMemory.AccessSize != 0 &&
          !Img.InstructionAddressMaterializations.count(
              LocalArithmetic->Addr)) {
        const std::optional<va_t> NextAddress =
            checkedArithmetic(CurrentAddress, *LocalArithmetic, LocalBaseSide);
        const std::optional<va_t> Owner =
            NextAddress ? uniqueImmutableDataOwner(
                              *NextAddress, LocalDereferenceMemory.AccessSize)
                        : std::nullopt;
        if (!NextAddress || !Owner ||
            !Img.relocatedTargetBelongsToOwner(*NextAddress, *Owner))
          break;

        CandidateQueries.push_back({LocalArithmetic->Inputs[LocalBaseSide],
                                    LocalArithmetic->Addr,
                                    LocalArithmetic->Seq,
                                    {CurrentDefinition},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});
        Steps.push_back({LocalArithmetic->Addr, LocalArithmetic->Seq,
                         LocalArithmetic->Opcode,
                         static_cast<uint8_t>(LocalBaseSide),
                         LocalArithmetic->Inputs[LocalBaseSide],
                         LocalArithmetic->Inputs[1 - LocalBaseSide],
                         LocalArithmetic->Output});
        CurrentValue = LocalArithmetic->Output;
        CurrentAddress = *NextAddress;
        CurrentDefinition = {LocalArithmetic->Output, LocalArithmetic->Addr,
                             LocalArithmetic->Seq,
                             /*DefinedAtPoint=*/true};
        CandidateQueries.push_back({LocalDereferenceAddress,
                                    LocalDereference->Addr,
                                    LocalDereference->Seq,
                                    {CurrentDefinition},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});

        RelocationFreeCandidate Candidate;
        for (JumpTableValueQuery &Query : CandidateQueries) {
          Candidate.QueryIndices.push_back(RelocationFreeQueries.size());
          RelocationFreeQueries.push_back(std::move(Query));
        }
        RelocatedInstructionAddressOccurrence &Occurrence =
            Candidate.Occurrence;
        Occurrence.FieldVA = InvalidVA;
        Occurrence.InstructionAddr = Steps.back().InstructionAddr;
        Occurrence.OpSeq = Steps.back().OpSeq;
        Occurrence.TargetVA = CurrentAddress;
        Occurrence.TargetOwnerVA = *Owner;
        Occurrence.Width = static_cast<uint8_t>(PointerSize);
        Occurrence.Provenance = ConstantAddressProvenance::DataAddress;
        Occurrence.DefinesOutput = true;
        Occurrence.OutputOpcode = Steps.back().Opcode;
        Occurrence.OutputWitness = Steps.back().OutputWitness;
        Occurrence.Authority = RelocatedInstructionAddressProofKind::
            AArch64RelocationFreeDataDereference;
        Occurrence.SeedInstructionAddr = Root.Addr;
        Occurrence.SeedOpSeq = Root.Seq;
        Occurrence.SeedOpcode = Root.Opcode;
        Occurrence.SeedInputWitness = Root.Inputs[0];
        Occurrence.SeedOutputWitness = Root.Output;
        Occurrence.ArithmeticProof = Steps;
        Occurrence.DereferenceInstructionAddr = LocalDereference->Addr;
        Occurrence.DereferenceOpSeq = LocalDereference->Seq;
        Occurrence.DereferenceOpcode = LocalDereference->Opcode;
        Occurrence.DereferenceAddressWitness = LocalDereferenceAddress;
        Occurrence.DereferenceAccessSize = LocalDereferenceMemory.AccessSize;
        RelocationFreeCandidates.push_back(std::move(Candidate));
        break;
      }

      const LowOp *Arithmetic = nullptr;
      int BaseSide = -1;
      for (const LowOp &Op : UseRec.Ops) {
        if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
            Op.NumInputs != 2 || !Op.Output.isReg() ||
            Op.Output.Size != PointerSize)
          continue;
        int CandidateBase = -1;
        if (Op.Inputs[0] == CurrentValue && Op.Inputs[1].isConst())
          CandidateBase = 0;
        if (Op.Opcode == NdOp::INT_ADD && Op.Inputs[1] == CurrentValue &&
            Op.Inputs[0].isConst()) {
          if (CandidateBase >= 0) {
            CandidateBase = -2;
          } else {
            CandidateBase = 1;
          }
        }
        if (CandidateBase < 0 ||
            !checkedArithmetic(CurrentAddress, Op, CandidateBase))
          continue;
        if (Arithmetic) {
          Arithmetic = nullptr;
          BaseSide = -2;
          break;
        }
        Arithmetic = &Op;
        BaseSide = CandidateBase;
      }

      if (Arithmetic && BaseSide >= 0) {
        bool InvalidStep = false;
        for (const LowOp &Op : UseRec.Ops) {
          const LowMemoryOperandView Memory = lowMemoryOperands(Op);
          if (Memory.Complete ||
              (&Op != Arithmetic &&
               (overlapsRegister(Op.Output, CurrentValue) ||
                overlapsRegister(Op.Output, Arithmetic->Output)))) {
            InvalidStep = true;
            break;
          }
        }
        if (InvalidStep ||
            Img.InstructionAddressMaterializations.count(Arithmetic->Addr))
          break;
        const std::optional<va_t> NextAddress =
            checkedArithmetic(CurrentAddress, *Arithmetic, BaseSide);
        if (!NextAddress)
          break;
        CandidateQueries.push_back({Arithmetic->Inputs[BaseSide],
                                    Arithmetic->Addr,
                                    Arithmetic->Seq,
                                    {CurrentDefinition},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});
        Steps.push_back({Arithmetic->Addr, Arithmetic->Seq, Arithmetic->Opcode,
                         static_cast<uint8_t>(BaseSide),
                         Arithmetic->Inputs[BaseSide],
                         Arithmetic->Inputs[1 - BaseSide], Arithmetic->Output});
        CurrentValue = Arithmetic->Output;
        CurrentAddress = *NextAddress;
        CurrentDefinition = {Arithmetic->Output, Arithmetic->Addr,
                             Arithmetic->Seq, /*DefinedAtPoint=*/true};
        ExpectedAddress = UseRec.Addr + UseRec.Size;
        continue;
      }

      if (Steps.empty())
        break;
      const LowOp *Dereference = nullptr;
      LowMemoryOperandView DereferenceMemory;
      NdVar DereferenceAddress = CurrentValue;
      bool InvalidDereference = false;
      for (const LowOp &Op : UseRec.Ops) {
        const LowMemoryOperandView Memory = lowMemoryOperands(Op);
        if (Memory.Complete) {
          if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
              !Memory.Address || *Memory.Address != DereferenceAddress ||
              Dereference) {
            InvalidDereference = true;
            break;
          }
          Dereference = &Op;
          DereferenceMemory = Memory;
          if (overlapsRegister(Op.Output, CurrentValue)) {
            InvalidDereference = true;
            break;
          }
          continue;
        }
        if (!Dereference) {
          if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 ||
              Op.Inputs[0] != DereferenceAddress ||
              (!Op.Output.isReg() && !Op.Output.isTemp()) ||
              Op.Output.Size != PointerSize ||
              (Op.Output.isReg() &&
               overlapsRegister(Op.Output, CurrentValue))) {
            InvalidDereference = true;
            break;
          }
          DereferenceAddress = Op.Output;
        }
        if (overlapsRegister(Op.Output, CurrentValue)) {
          InvalidDereference = true;
          break;
        }
      }
      if (InvalidDereference || !Dereference ||
          DereferenceMemory.AccessSize == 0)
        break;
      const std::optional<va_t> Owner = uniqueImmutableDataOwner(
          CurrentAddress, DereferenceMemory.AccessSize);
      if (!Owner || !Img.relocatedTargetBelongsToOwner(CurrentAddress, *Owner))
        break;

      CandidateQueries.push_back({DereferenceAddress,
                                  Dereference->Addr,
                                  Dereference->Seq,
                                  {CurrentDefinition},
                                  /*AllowZeroExtension=*/false,
                                  /*AllowSignExtension=*/false});
      RelocationFreeCandidate Candidate;
      for (JumpTableValueQuery &Query : CandidateQueries) {
        Candidate.QueryIndices.push_back(RelocationFreeQueries.size());
        RelocationFreeQueries.push_back(std::move(Query));
      }
      RelocatedInstructionAddressOccurrence &Occurrence = Candidate.Occurrence;
      Occurrence.FieldVA = InvalidVA;
      Occurrence.InstructionAddr = Steps.back().InstructionAddr;
      Occurrence.OpSeq = Steps.back().OpSeq;
      Occurrence.TargetVA = CurrentAddress;
      Occurrence.TargetOwnerVA = *Owner;
      Occurrence.Width = static_cast<uint8_t>(PointerSize);
      Occurrence.Provenance = ConstantAddressProvenance::DataAddress;
      Occurrence.DefinesOutput = true;
      Occurrence.OutputOpcode = Steps.back().Opcode;
      Occurrence.OutputWitness = Steps.back().OutputWitness;
      Occurrence.Authority = RelocatedInstructionAddressProofKind::
          AArch64RelocationFreeDataDereference;
      Occurrence.SeedInstructionAddr = Root.Addr;
      Occurrence.SeedOpSeq = Root.Seq;
      Occurrence.SeedOpcode = Root.Opcode;
      Occurrence.SeedInputWitness = Root.Inputs[0];
      Occurrence.SeedOutputWitness = Root.Output;
      Occurrence.ArithmeticProof = Steps;
      Occurrence.DereferenceInstructionAddr = Dereference->Addr;
      Occurrence.DereferenceOpSeq = Dereference->Seq;
      Occurrence.DereferenceOpcode = Dereference->Opcode;
      Occurrence.DereferenceAddressWitness = DereferenceAddress;
      Occurrence.DereferenceAccessSize = DereferenceMemory.AccessSize;
      RelocationFreeCandidates.push_back(std::move(Candidate));
      break;
    }
  }

  if (RelocationFreeQueries.empty())
    return;
  bool RelocationFreeAnalysisComplete = false;
  const std::vector<bool> RelocationFreeMatches = tableValuesMatchAtUses(
      RelocationFreeQueries, &RelocationFreeAnalysisComplete);
  if (!RelocationFreeAnalysisComplete ||
      RelocationFreeMatches.size() != RelocationFreeQueries.size())
    return;
  for (RelocationFreeCandidate &Candidate : RelocationFreeCandidates) {
    if (!std::all_of(Candidate.QueryIndices.begin(),
                     Candidate.QueryIndices.end(), [&](size_t Index) {
                       return Index < RelocationFreeMatches.size() &&
                              RelocationFreeMatches[Index];
                     }))
      continue;
    if (!llvm::is_contained(RelocatedInstructionAddressOccurrences,
                            Candidate.Occurrence))
      RelocatedInstructionAddressOccurrences.push_back(
          std::move(Candidate.Occurrence));
  }
}

void CFGBuilder::completeExactARMRelativeLiteralAddresses(
    const BinaryImage &Img) {
  if (Img.Arch != Arch::ARM || !Img.isELF() || !JumpTableProofContextComplete ||
      Img.getPointerSize() != 4)
    return;

  struct LiteralDefinition {
    va_t SlotVA = InvalidVA;
    va_t TargetVA = InvalidVA;
    va_t TargetOwnerVA = InvalidVA;
    uint32_t Encoded = 0;
    JumpTableValueOccurrence Occurrence;
  };
  std::vector<LiteralDefinition> Literals;

  // Resolve only instruction-local literal effective addresses.  This is not
  // a CFG proof: it authenticates which exact LOAD occurrence read the
  // relocation field.  The later batch query proves that LOAD's output reaches
  // the ADD base on every feasible incoming path.
  for (const auto &[Addr, Rec] : Insns) {
    (void)Addr;
    std::vector<std::pair<NdVar, uint32_t>> Known;
    auto knownValue = [&](const NdVar &Value) -> std::optional<uint32_t> {
      if (Value.isConst())
        return static_cast<uint32_t>(Value.Offset);
      for (auto It = Known.rbegin(); It != Known.rend(); ++It)
        if (It->first == Value)
          return It->second;
      return std::nullopt;
    };
    for (const LowOp &Op : Rec.Ops) {
      if (Op.Opcode == NdOp::LOAD) {
        const LowMemoryOperandView Memory = lowMemoryOperands(Op);
        if (Memory.Complete && Memory.Address) {
          const std::optional<uint32_t> Slot = knownValue(*Memory.Address);
          const auto Applied = Slot ? Img.ARMRelativeLiteralFields.find(*Slot)
                                    : Img.ARMRelativeLiteralFields.end();
          const Segment *SlotSegment =
              Slot ? Img.getSegmentFor(*Slot) : nullptr;
          const uint8_t *Bytes =
              Slot ? Img.readVA(*Slot, sizeof(uint32_t)) : nullptr;
          if (Applied != Img.ARMRelativeLiteralFields.end() &&
              Applied->second.TargetVA != InvalidVA &&
              Applied->second.TargetOwnerVA != InvalidVA && SlotSegment &&
              SlotSegment->isReadable() && !SlotSegment->isWritable() &&
              Bytes && Op.Output.Size == 4) {
            const AppliedARMRelativeLiteral &Field = Applied->second;
            if (readLE<uint32_t>(Bytes) == Field.EncodedValue &&
                Img.relocatedTargetBelongsToOwner(Field.TargetVA,
                                                  Field.TargetOwnerVA))
              Literals.push_back(
                  {*Slot,
                   Field.TargetVA,
                   Field.TargetOwnerVA,
                   Field.EncodedValue,
                   {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true}});
          }
        }
      }

      std::optional<uint32_t> Result;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
        Result = knownValue(Op.Inputs[0]);
      } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
        const auto Left = knownValue(Op.Inputs[0]);
        const auto Right = knownValue(Op.Inputs[1]);
        if (Left && Right)
          Result = static_cast<uint32_t>(*Left + *Right);
      } else if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2) {
        const auto Left = knownValue(Op.Inputs[0]);
        const auto Right = knownValue(Op.Inputs[1]);
        if (Left && Right)
          Result = static_cast<uint32_t>(*Left - *Right);
      }
      if (Result && Op.Output.Size == 4)
        Known.emplace_back(Op.Output, *Result);
    }
  }
  if (Literals.empty())
    return;

  struct PendingMaterialization {
    const LowOp *Op = nullptr;
    va_t TargetVA = InvalidVA;
    va_t TargetOwnerVA = InvalidVA;
    size_t ExactQuery = 0;
    std::vector<std::pair<const LiteralDefinition *, size_t>> Sources;
  };
  std::vector<JumpTableValueQuery> Queries;
  std::vector<PendingMaterialization> Pending;
  for (const auto &[Addr, Rec] : Insns) {
    (void)Addr;
    if (Rec.IsInstructionGuard)
      continue;
    std::vector<std::pair<NdVar, uint32_t>> Known;
    auto knownValue = [&](const NdVar &Value) -> std::optional<uint32_t> {
      if (Value.isConst())
        return static_cast<uint32_t>(Value.Offset);
      for (auto It = Known.rbegin(); It != Known.rend(); ++It)
        if (It->first == Value)
          return It->second;
      return std::nullopt;
    };
    for (const LowOp &Op : Rec.Ops) {
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
          Op.Output.isReg() && Op.Output.Size == 4) {
        int PCSide = -1;
        for (int Side = 0; Side < 2; ++Side) {
          const std::optional<uint32_t> PC = knownValue(Op.Inputs[Side]);
          if (!PC || *PC != static_cast<uint32_t>(Op.Addr + 8))
            continue;
          if (PCSide != -1) {
            PCSide = -2;
            break;
          }
          PCSide = Side;
        }
        if (PCSide >= 0) {
          using TargetKey = std::pair<va_t, va_t>;
          std::map<TargetKey, std::vector<const LiteralDefinition *>> Groups;
          const uint32_t PC = static_cast<uint32_t>(Op.Addr + 8);
          for (const LiteralDefinition &Literal : Literals) {
            if (static_cast<uint32_t>(PC + Literal.Encoded) !=
                    static_cast<uint32_t>(Literal.TargetVA) ||
                Literal.Occurrence.Value.Size != Op.Inputs[1 - PCSide].Size)
              continue;
            const TargetKey Key{Literal.TargetVA, Literal.TargetOwnerVA};
            Groups[Key].push_back(&Literal);
          }
          for (auto &[Key, Sources] : Groups) {
            std::vector<JumpTableValueOccurrence> Alternatives;
            Alternatives.reserve(Sources.size());
            for (const LiteralDefinition *Source : Sources)
              Alternatives.push_back(Source->Occurrence);
            const size_t ExactQuery = Queries.size();
            Queries.push_back({Op.Inputs[1 - PCSide], Op.Addr, Op.Seq,
                               Alternatives, false, false});
            PendingMaterialization Candidate{
                &Op, Key.first, Key.second, ExactQuery, {}};
            Candidate.Sources.reserve(Sources.size());
            for (const LiteralDefinition *Source : Sources) {
              const size_t MayQuery = Queries.size();
              JumpTableValueQuery MayDepend{
                  Op.Inputs[1 - PCSide], Op.Addr, Op.Seq,
                  {Source->Occurrence},  false,   false};
              MayDepend.Relation = JumpTableValueRelation::MayDepend;
              Queries.push_back(std::move(MayDepend));
              Candidate.Sources.emplace_back(Source, MayQuery);
            }
            Pending.push_back(std::move(Candidate));
          }
        }
      }

      std::optional<uint32_t> Result;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
        Result = knownValue(Op.Inputs[0]);
      } else if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2) {
        const auto Left = knownValue(Op.Inputs[0]);
        const auto Right = knownValue(Op.Inputs[1]);
        if (Left && Right)
          Result = static_cast<uint32_t>(*Left + *Right);
      } else if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2) {
        const auto Left = knownValue(Op.Inputs[0]);
        const auto Right = knownValue(Op.Inputs[1]);
        if (Left && Right)
          Result = static_cast<uint32_t>(*Left - *Right);
      }
      if (Result && Op.Output.Size == 4)
        Known.emplace_back(Op.Output, *Result);
    }
  }
  if (Queries.empty())
    return;

  bool AnalysisComplete = false;
  const std::vector<bool> Matches =
      tableValuesMatchAtUses(Queries, &AnalysisComplete);
  if (!AnalysisComplete || Matches.size() != Queries.size())
    return;
  for (const PendingMaterialization &Candidate : Pending) {
    if (!Candidate.Op || Candidate.ExactQuery >= Matches.size())
      continue;
    const bool IsExact = Matches[Candidate.ExactQuery];
    for (const auto &[Source, MayQuery] : Candidate.Sources) {
      if (!Source || MayQuery >= Matches.size() || !Matches[MayQuery])
        continue;
      RelocatedInstructionAddressOccurrence Occurrence;
      Occurrence.FieldVA = Source->SlotVA;
      Occurrence.InstructionAddr = Candidate.Op->Addr;
      Occurrence.OpSeq = Candidate.Op->Seq;
      Occurrence.TargetVA = Candidate.TargetVA;
      Occurrence.TargetOwnerVA = Candidate.TargetOwnerVA;
      Occurrence.Width = 4;
      Occurrence.Provenance = Img.hasExecutableCodeOwnerAt(Candidate.TargetVA)
                                  ? ConstantAddressProvenance::CodeAddress
                                  : ConstantAddressProvenance::DataAddress;
      Occurrence.DefinesOutput = true;
      Occurrence.OutputMayDepend = !IsExact;
      Occurrence.OutputOpcode = Candidate.Op->Opcode;
      Occurrence.OutputWitness = Candidate.Op->Output;
      if (!llvm::is_contained(RelocatedInstructionAddressOccurrences,
                              Occurrence))
        RelocatedInstructionAddressOccurrences.push_back(std::move(Occurrence));
    }
  }
}

//===----------------------------------------------------------------------===//
// multiStageResolve — retry unresolved INDIR_BR with more context
//===----------------------------------------------------------------------===//

void CFGBuilder::multiStageResolve(const BinaryImage &Img, Decoder &Dec,
                                   LowFunc &Func) {
  AmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCBranches.clear();
  PendingAmbiguousI386GOTPCKeys.clear();
  StageAmbiguousI386GOTPCBranches.clear();
  StageReplayedI386GOTPCKeys.clear();
  CurrentI386GOTOFFAmbiguityKeys.clear();
  for (va_t Addr : PreviouslyPublishedJumpTableBranches) {
    auto It = Insns.find(Addr);
    if (It != Insns.end() && It->second.IsBranch && It->second.IsIndirect &&
        !It->second.IsCall)
      EverPublishedJumpTableBranches.insert(Addr);
  }
  auto RememberPublishedJumpTables = [&]() {
    for (const auto &[Addr, Rec] : Insns)
      if (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall &&
          !Rec.JumpTableTargets.empty())
        EverPublishedJumpTableBranches.insert(Addr);
  };
  RememberPublishedJumpTables();

  bool ReachedFixedPoint = false;
  for (int Stage = 0; Stage < limits::kMaxMultiStageRetries; ++Stage) {
    // Incomplete-evidence branch identity is persistent safety state.  Rebuild
    // it transactionally with the proposal graph: a stage may clear an old
    // marker only after every candidate and the proposal commit tail have
    // completed.  Moving the committed sets aside also avoids an unmetered
    // deep snapshot; the empty member sets are the stage-local shadows.
    std::set<va_t> SavedStackTableEvidenceIncompleteBranches;
    std::set<va_t> SavedIndexDomainEvidenceIncompleteBranches;
    SavedStackTableEvidenceIncompleteBranches.swap(
        StackTableEvidenceIncompleteBranches);
    SavedIndexDomainEvidenceIncompleteBranches.swap(
        IndexDomainEvidenceIncompleteBranches);
    bool IncompleteBranchMarkerStageActive = true;
    bool IncompleteBranchMarkerCleanupReserved = false;
    auto RestoreIncompleteBranchMarkers = [&]() {
      if (!IncompleteBranchMarkerStageActive)
        return;
      // The initial inventory reserve can fail before any candidate has had a
      // chance to populate the stage-local shadows.  Restore that boundary
      // with two constant-time swaps; merge/clear below is reserved only after
      // candidate mutation becomes possible.
      if (StackTableEvidenceIncompleteBranches.empty() &&
          IndexDomainEvidenceIncompleteBranches.empty()) {
        StackTableEvidenceIncompleteBranches.swap(
            SavedStackTableEvidenceIncompleteBranches);
        IndexDomainEvidenceIncompleteBranches.swap(
            SavedIndexDomainEvidenceIncompleteBranches);
        IncompleteBranchMarkerStageActive = false;
        return;
      }
      assert(IncompleteBranchMarkerCleanupReserved &&
             "marker rollback requires prepaid cleanup");
      // A failed stage cannot commit deletion of an entry marker, but newly
      // observed resource-incomplete candidates must remain fail-closed when
      // bounded retries end.  Node-merge preserves both sets without a second
      // allocation; duplicate working nodes are destroyed by the cleanup
      // reserve charged below.
      SavedStackTableEvidenceIncompleteBranches.merge(
          StackTableEvidenceIncompleteBranches);
      SavedIndexDomainEvidenceIncompleteBranches.merge(
          IndexDomainEvidenceIncompleteBranches);
      StackTableEvidenceIncompleteBranches.clear();
      IndexDomainEvidenceIncompleteBranches.clear();
      StackTableEvidenceIncompleteBranches.swap(
          SavedStackTableEvidenceIncompleteBranches);
      IndexDomainEvidenceIncompleteBranches.swap(
          SavedIndexDomainEvidenceIncompleteBranches);
      IncompleteBranchMarkerStageActive = false;
    };
    auto CommitIncompleteBranchMarkers = [&]() {
      assert(IncompleteBranchMarkerCleanupReserved &&
             "marker commit requires prepaid cleanup");
      SavedStackTableEvidenceIncompleteBranches.clear();
      SavedIndexDomainEvidenceIncompleteBranches.clear();
      IncompleteBranchMarkerStageActive = false;
    };
    bool ForcedStableAmbiguityCommitTailThisStage = false;
    bool ForcedProposalCommitTailThisStage = false;
    size_t ForcedPendingKeyCount = 0;
    // Reachability and relocation-root suppression can change after every
    // published candidate.  Reuse proposal roots within this stage only; the
    // fixed retry bound caps total work, while each newly published resolver
    // graph receives one transactional stack-evidence allowance shared by all
    // candidates in that stage.  Do not let a provisional graph consume the
    // final revalidation graph's proof budget.
    StackTableEvidenceRemaining =
        std::min<size_t>(limits::kMaxJumpTableStackEvidenceWork,
                         StackTableEvidenceBudgetForTesting.value_or(
                             limits::kMaxJumpTableStackEvidenceWork));
    CandidateFixedPointExplorationTargets.clear();
    StageAmbiguousI386GOTPCBranches.clear();
    StageReplayedI386GOTPCKeys.clear();
    I386GOTOFFProposalRootCache.clear();
    I386GOTOFFModelReachCache.clear();
    NextStrongJumpTableProposals.clear();
    NextProvisionalRelativeEdges.clear();
    StrongJumpTableProposalOutcomes.clear();
    CandidateProposalStageEvidenceRemaining =
        std::min<size_t>(limits::kMaxJumpTableProposalStageEvidenceWork,
                         MaskFixedPointEvidenceBudgetForTesting.value_or(
                             limits::kMaxJumpTableProposalStageEvidenceWork));
    // The nested-tracking boundary hook models a persistent attacker-sized
    // stage after the first recursively discovered candidate reaches that
    // boundary.  Do not clamp the initial parent stage: it must first decode
    // the exact nested branch whose identity the regression observes.
    if (NestedMutationTrackingEvidenceExhaustedForTesting &&
        NestedMutationTrackingStageAllowanceForTesting)
      CandidateProposalStageEvidenceRemaining =
          std::min(CandidateProposalStageEvidenceRemaining,
                   *NestedMutationTrackingStageAllowanceForTesting);
    CandidateProposalStageEvidenceIncomplete = false;
    CandidateProposalStageActive = true;
    UntrackedJumpTableCandidateExhaustedThisStageForTesting = false;
    assert(CandidateProposalStageMutationAddrs.empty() &&
           "proposal mutation inventory must be retired with its stage");
    const size_t QuarantineSizeAtStageStart =
        QuarantinedJumpTableProposals.size();
    auto ConsumeProposalStageEvidence = [&](size_t Amount = 1) {
      if (Amount > CandidateProposalStageEvidenceRemaining) {
        CandidateProposalStageEvidenceRemaining = 0;
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      CandidateProposalStageEvidenceRemaining -= Amount;
      return true;
    };
    auto ConsumeProposalStageProducts =
        [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
          const size_t Max = std::numeric_limits<size_t>::max();
          size_t Total = 0;
          for (const auto &[Count, Cost] : Products) {
            if (Count != 0 && Cost > Max / Count)
              return ConsumeProposalStageEvidence(Max);
            const size_t Product = Count * Cost;
            if (Product > Max - Total)
              return ConsumeProposalStageEvidence(Max);
            Total += Product;
          }
          return ConsumeProposalStageEvidence(Total);
        };
    auto OrderedLookupWork = [](size_t Count) {
      size_t Work = 1;
      for (size_t N = Count; N > 1; N = N / 2 + N % 2)
        ++Work;
      return Work;
    };
    auto PrepayTargetSetOperations = [&](size_t TargetCount) {
      const size_t Max = std::numeric_limits<size_t>::max();
      if (Insns.size() > Max - BlockStarts.size() ||
          Insns.size() + BlockStarts.size() > Max - TargetCount ||
          Insns.size() > Max - ExploredAddrs.size() ||
          Insns.size() + ExploredAddrs.size() > Max - TargetCount)
        return ConsumeProposalStageEvidence(Max);
      const size_t FutureBlockStarts =
          Insns.size() + BlockStarts.size() + TargetCount;
      const size_t FutureExplored =
          Insns.size() + ExploredAddrs.size() + TargetCount;
      const size_t BlockWork = OrderedLookupWork(FutureBlockStarts);
      const size_t ExploreWork = OrderedLookupWork(FutureExplored);
      if (BlockWork > Max - 1 || ExploreWork > Max - (BlockWork + 1))
        return ConsumeProposalStageEvidence(Max);
      const size_t PerTarget = BlockWork + 1 + ExploreWork;
      return ConsumeProposalStageProducts({{TargetCount, PerTarget}});
    };
    auto ConsumeJumpTableInfoLifecycle = [&](const JumpTableInfo &Candidate) {
      if (!ConsumeProposalStageProducts(
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
        if (!ConsumeProposalStageEvidence(Initializer.StaticSources.size()))
          return false;
      for (const JumpTableLoadRole &Role : Candidate.LoadRoles) {
        if (!ConsumeProposalStageEvidence(Role.AllowedBases.size()) ||
            !ConsumeProposalStageEvidence(Role.Indices.size()) ||
            !ConsumeProposalStageEvidence(
                Role.FrameStorage.Initializers.size()))
          return false;
        for (const JumpTableFrameInitializerChunk &Initializer :
             Role.FrameStorage.Initializers)
          if (!ConsumeProposalStageEvidence(Initializer.StaticSources.size()))
            return false;
      }
      return ConsumeProposalStageEvidence(1);
    };
    auto SameNdVarNoShortCircuit =
        [&](const NdVar *Left, const NdVar *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(5))
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = true;
      Same &= Left->Space == Right->Space;
      Same &= Left->Offset == Right->Offset;
      Same &= Left->Size == Right->Size;
      Same &= Left->Provenance == Right->Provenance;
      Same &= Left->AddressOwnerVA == Right->AddressOwnerVA;
      return Same;
    };
    auto SameOccurrenceNoShortCircuit =
        [&](const JumpTableValueOccurrence *Left,
            const JumpTableValueOccurrence *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(3))
        return std::nullopt;
      const std::optional<bool> SameValue = SameNdVarNoShortCircuit(
          Left ? &Left->Value : nullptr, Right ? &Right->Value : nullptr);
      if (!SameValue)
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = *SameValue;
      Same &= Left->Addr == Right->Addr;
      Same &= Left->Seq == Right->Seq;
      Same &= Left->DefinedAtPoint == Right->DefinedAtPoint;
      return Same;
    };
    auto SameStorageRangeNoShortCircuit =
        [&](const JumpTableStorageRange *Left,
            const JumpTableStorageRange *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(4))
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = true;
      Same &= Left->BaseAddr == Right->BaseAddr;
      Same &= Left->EntrySize == Right->EntrySize;
      Same &= Left->EntryStride == Right->EntryStride;
      Same &= Left->PhysicalSlotCount == Right->PhysicalSlotCount;
      return Same;
    };
    auto SameMaskWitnessNoShortCircuit =
        [&](const JumpTableMaskKnownOneWitness *Left,
            const JumpTableMaskKnownOneWitness *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      const std::optional<bool> SameOr = SameOccurrenceNoShortCircuit(
          Left ? &Left->OrOutput : nullptr, Right ? &Right->OrOutput : nullptr);
      if (!SameOr)
        return std::nullopt;
      const std::optional<bool> SameMask =
          SameOccurrenceNoShortCircuit(Left ? &Left->MaskOutput : nullptr,
                                       Right ? &Right->MaskOutput : nullptr);
      if (!SameMask)
        return std::nullopt;
      const std::optional<bool> SameConstant =
          SameNdVarNoShortCircuit(Left ? &Left->ConstantOperand : nullptr,
                                  Right ? &Right->ConstantOperand : nullptr);
      if (!SameConstant)
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = *SameOr & *SameMask & *SameConstant;
      Same &= Left->KnownOneBits == Right->KnownOneBits;
      return Same;
    };
    auto SameFrameAddressUseNoShortCircuit =
        [&](const JumpTableFrameAddressUse *Left,
            const JumpTableFrameAddressUse *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      const std::optional<bool> SameUse = SameOccurrenceNoShortCircuit(
          Left ? &Left->Use : nullptr, Right ? &Right->Use : nullptr);
      if (!SameUse)
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = *SameUse;
      Same &= Left->ByteAddend == Right->ByteAddend;
      return Same;
    };
    auto SameStaticSourcePieceNoShortCircuit =
        [&](const JumpTableFrameInitializerChunk::StaticSourcePiece *Left,
            const JumpTableFrameInitializerChunk::StaticSourcePiece *Right)
        -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(6))
        return std::nullopt;
      const std::optional<bool> SameValue = SameOccurrenceNoShortCircuit(
          Left ? &Left->Value : nullptr, Right ? &Right->Value : nullptr);
      if (!SameValue)
        return std::nullopt;
      const std::optional<bool> SameAddress = SameOccurrenceNoShortCircuit(
          Left ? &Left->Address : nullptr, Right ? &Right->Address : nullptr);
      if (!SameAddress)
        return std::nullopt;
      const std::optional<bool> SameProducer = SameOccurrenceNoShortCircuit(
          Left ? &Left->StaticAddressProducer : nullptr,
          Right ? &Right->StaticAddressProducer : nullptr);
      if (!SameProducer)
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = *SameValue & *SameAddress & *SameProducer;
      Same &= Left->StaticAddressFieldVA == Right->StaticAddressFieldVA;
      Same &= Left->StaticAddressProducerTargetVA ==
              Right->StaticAddressProducerTargetVA;
      Same &= Left->StaticAddress == Right->StaticAddress;
      Same &= Left->ByteCount == Right->ByteCount;
      Same &= Left->StaticAddressProvenance == Right->StaticAddressProvenance;
      Same &= Left->StaticAddressOwnerVA == Right->StaticAddressOwnerVA;
      return Same;
    };
    auto SameInitializerNoShortCircuit =
        [&](const JumpTableFrameInitializerChunk *Left,
            const JumpTableFrameInitializerChunk *Right)
        -> std::optional<bool> {
      // Seven direct scalar fields plus the StaticSources vector size.
      if (!ConsumeProposalStageEvidence(8))
        return std::nullopt;
      bool Same = Left && Right;
      if (Left && Right) {
        Same &= Left->ByteCount == Right->ByteCount;
        Same &= Left->StaticSourceAddress == Right->StaticSourceAddress;
        Same &= Left->StaticSourceProvenance == Right->StaticSourceProvenance;
        Same &= Left->StaticSourceOwnerVA == Right->StaticSourceOwnerVA;
        Same &= Left->StaticSourceFieldVA == Right->StaticSourceFieldVA;
        Same &= Left->StaticSourceProducerTargetVA ==
                Right->StaticSourceProducerTargetVA;
        Same &= Left->IsMemcpy == Right->IsMemcpy;
        Same &= Left->StaticSources.size() == Right->StaticSources.size();
      }
      const std::optional<bool> SameWriter = SameOccurrenceNoShortCircuit(
          Left ? &Left->Writer : nullptr, Right ? &Right->Writer : nullptr);
      if (!SameWriter)
        return std::nullopt;
      Same &= *SameWriter;
      const std::optional<bool> SameDestination =
          SameFrameAddressUseNoShortCircuit(Left ? &Left->Destination : nullptr,
                                            Right ? &Right->Destination
                                                  : nullptr);
      if (!SameDestination)
        return std::nullopt;
      Same &= *SameDestination;
      const std::optional<bool> SameStored =
          SameOccurrenceNoShortCircuit(Left ? &Left->StoredValue : nullptr,
                                       Right ? &Right->StoredValue : nullptr);
      if (!SameStored)
        return std::nullopt;
      Same &= *SameStored;
      const std::optional<bool> SameSource =
          SameOccurrenceNoShortCircuit(Left ? &Left->SourceAddress : nullptr,
                                       Right ? &Right->SourceAddress : nullptr);
      if (!SameSource)
        return std::nullopt;
      Same &= *SameSource;
      const std::optional<bool> SameLength = SameOccurrenceNoShortCircuit(
          Left ? &Left->Length : nullptr, Right ? &Right->Length : nullptr);
      if (!SameLength)
        return std::nullopt;
      Same &= *SameLength;
      const std::optional<bool> SameLengthProducer =
          SameOccurrenceNoShortCircuit(Left ? &Left->LengthProducer : nullptr,
                                       Right ? &Right->LengthProducer
                                             : nullptr);
      if (!SameLengthProducer)
        return std::nullopt;
      Same &= *SameLengthProducer;
      const std::optional<bool> SameStaticProducer =
          SameOccurrenceNoShortCircuit(
              Left ? &Left->StaticSourceProducer : nullptr,
              Right ? &Right->StaticSourceProducer : nullptr);
      if (!SameStaticProducer)
        return std::nullopt;
      Same &= *SameStaticProducer;
      const size_t LeftSourceCount = Left ? Left->StaticSources.size() : 0;
      const size_t RightSourceCount = Right ? Right->StaticSources.size() : 0;
      for (size_t I = 0; I < std::max(LeftSourceCount, RightSourceCount); ++I) {
        const auto *LeftSource =
            Left && I < LeftSourceCount ? &Left->StaticSources[I] : nullptr;
        const auto *RightSource =
            Right && I < RightSourceCount ? &Right->StaticSources[I] : nullptr;
        const std::optional<bool> SameSourcePiece =
            SameStaticSourcePieceNoShortCircuit(LeftSource, RightSource);
        if (!SameSourcePiece)
          return std::nullopt;
        Same &= *SameSourcePiece;
      }
      return Same;
    };
    auto SameFrameStorageNoShortCircuit =
        [&](const JumpTableFrameStorageRole *Left,
            const JumpTableFrameStorageRole *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      bool Same = Left && Right;
      if (Left && Right)
        Same &= Left->Initializers.size() == Right->Initializers.size();
      const std::optional<bool> SameRuntimeBase =
          SameFrameAddressUseNoShortCircuit(Left ? &Left->RuntimeBase : nullptr,
                                            Right ? &Right->RuntimeBase
                                                  : nullptr);
      if (!SameRuntimeBase)
        return std::nullopt;
      Same &= *SameRuntimeBase;
      const std::optional<bool> SameCompleteAddress =
          SameOccurrenceNoShortCircuit(Left ? &Left->CompleteAddress : nullptr,
                                       Right ? &Right->CompleteAddress
                                             : nullptr);
      if (!SameCompleteAddress)
        return std::nullopt;
      Same &= *SameCompleteAddress;
      const size_t LeftInitializerCount = Left ? Left->Initializers.size() : 0;
      const size_t RightInitializerCount =
          Right ? Right->Initializers.size() : 0;
      for (size_t I = 0;
           I < std::max(LeftInitializerCount, RightInitializerCount); ++I) {
        const auto *LeftInitializer =
            Left && I < LeftInitializerCount ? &Left->Initializers[I] : nullptr;
        const auto *RightInitializer = Right && I < RightInitializerCount
                                           ? &Right->Initializers[I]
                                           : nullptr;
        const std::optional<bool> SameInitializer =
            SameInitializerNoShortCircuit(LeftInitializer, RightInitializer);
        if (!SameInitializer)
          return std::nullopt;
        Same &= *SameInitializer;
      }
      return Same;
    };
    auto SameDisplacedAddressNoShortCircuit =
        [&](const JumpTableDisplacedAddressRole *Left,
            const JumpTableDisplacedAddressRole *Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(2))
        return std::nullopt;
      const std::optional<bool> SameRuntimeBase = SameOccurrenceNoShortCircuit(
          Left ? &Left->RuntimeBaseUse : nullptr,
          Right ? &Right->RuntimeBaseUse : nullptr);
      if (!SameRuntimeBase)
        return std::nullopt;
      const std::optional<bool> SameCompleteAddress =
          SameOccurrenceNoShortCircuit(Left ? &Left->CompleteAddress : nullptr,
                                       Right ? &Right->CompleteAddress
                                             : nullptr);
      if (!SameCompleteAddress)
        return std::nullopt;
      if (!Left || !Right)
        return false;
      bool Same = *SameRuntimeBase & *SameCompleteAddress;
      Same &= Left->ExpectedRuntimeBase == Right->ExpectedRuntimeBase;
      Same &= Left->ByteAddend == Right->ByteAddend;
      return Same;
    };
    auto SameScalarVectorNoShortCircuit =
        [&](const auto &Left, const auto &Right) -> std::optional<bool> {
      const std::optional<size_t> Work =
          detail::scalarVectorComparisonWork(Left.size(), Right.size());
      if (!Work || !ConsumeProposalStageEvidence(*Work))
        return std::nullopt;
      bool Same = Left.size() == Right.size();
      for (size_t I = 0; I < std::min(Left.size(), Right.size()); ++I)
        Same &= Left[I] == Right[I];
      return Same;
    };
    auto SameOccurrenceVectorNoShortCircuit =
        [&](const auto &Left, const auto &Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      bool Same = Left.size() == Right.size();
      for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
        const auto *LeftOccurrence = I < Left.size() ? &Left[I] : nullptr;
        const auto *RightOccurrence = I < Right.size() ? &Right[I] : nullptr;
        const std::optional<bool> SameOccurrence =
            SameOccurrenceNoShortCircuit(LeftOccurrence, RightOccurrence);
        if (!SameOccurrence)
          return std::nullopt;
        Same &= *SameOccurrence;
      }
      return Same;
    };
    auto SameStorageRangeVectorNoShortCircuit =
        [&](const auto &Left, const auto &Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      bool Same = Left.size() == Right.size();
      for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
        const auto *LeftRange = I < Left.size() ? &Left[I] : nullptr;
        const auto *RightRange = I < Right.size() ? &Right[I] : nullptr;
        const std::optional<bool> SameRange =
            SameStorageRangeNoShortCircuit(LeftRange, RightRange);
        if (!SameRange)
          return std::nullopt;
        Same &= *SameRange;
      }
      return Same;
    };
    auto SameWitnessVectorNoShortCircuit =
        [&](const auto &Left, const auto &Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      bool Same = Left.size() == Right.size();
      for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
        const auto *LeftWitness = I < Left.size() ? &Left[I] : nullptr;
        const auto *RightWitness = I < Right.size() ? &Right[I] : nullptr;
        const std::optional<bool> SameWitness =
            SameMaskWitnessNoShortCircuit(LeftWitness, RightWitness);
        if (!SameWitness)
          return std::nullopt;
        Same &= *SameWitness;
      }
      return Same;
    };
    auto SameLoadRoleNoShortCircuit =
        [&](const JumpTableLoadRole *Left,
            const JumpTableLoadRole *Right) -> std::optional<bool> {
      // Direct scalars excluding the two vector sizes and nested certificates.
      if (!ConsumeProposalStageEvidence(13))
        return std::nullopt;
      bool Same = Left && Right;
      if (Left && Right) {
        Same &= Left->LoadWidth == Right->LoadWidth;
        Same &= Left->AddressScale == Right->AddressScale;
        Same &= Left->IsLiteralCoordinate == Right->IsLiteralCoordinate;
        Same &= Left->LiteralCoordinate == Right->LiteralCoordinate;
        Same &= Left->AllowZeroExtension == Right->AllowZeroExtension;
        Same &= Left->AllowSignExtension == Right->AllowSignExtension;
        Same &= Left->HasBaseSelect == Right->HasBaseSelect;
        Same &= Left->HasBaseMaskBlend == Right->HasBaseMaskBlend;
        Same &= Left->TrueBase == Right->TrueBase;
        Same &= Left->FalseBase == Right->FalseBase;
        Same &= Left->PositiveBlendInputSide == Right->PositiveBlendInputSide;
        Same &= Left->PositiveBaseInputSide == Right->PositiveBaseInputSide;
        Same &= Left->NegativeBaseInputSide == Right->NegativeBaseInputSide;
      }
      const std::optional<bool> SameLoad = SameOccurrenceNoShortCircuit(
          Left ? &Left->Load : nullptr, Right ? &Right->Load : nullptr);
      if (!SameLoad)
        return std::nullopt;
      Same &= *SameLoad;
      static const std::vector<va_t> EmptyBases;
      const std::optional<bool> SameBases = SameScalarVectorNoShortCircuit(
          Left ? Left->AllowedBases : EmptyBases,
          Right ? Right->AllowedBases : EmptyBases);
      if (!SameBases)
        return std::nullopt;
      Same &= *SameBases;
      const std::optional<bool> SameFrame = SameFrameStorageNoShortCircuit(
          Left ? &Left->FrameStorage : nullptr,
          Right ? &Right->FrameStorage : nullptr);
      if (!SameFrame)
        return std::nullopt;
      Same &= *SameFrame;
      const std::optional<bool> SameDisplaced =
          SameDisplacedAddressNoShortCircuit(
              Left ? &Left->DisplacedAddress : nullptr,
              Right ? &Right->DisplacedAddress : nullptr);
      if (!SameDisplaced)
        return std::nullopt;
      Same &= *SameDisplaced;
      static const std::vector<JumpTableValueOccurrence> EmptyOccurrences;
      const std::optional<bool> SameIndices =
          SameOccurrenceVectorNoShortCircuit(
              Left ? Left->Indices : EmptyOccurrences,
              Right ? Right->Indices : EmptyOccurrences);
      if (!SameIndices)
        return std::nullopt;
      Same &= *SameIndices;
      const std::optional<bool> SameAddressIndex =
          SameOccurrenceNoShortCircuit(Left ? &Left->AddressIndex : nullptr,
                                       Right ? &Right->AddressIndex : nullptr);
      if (!SameAddressIndex)
        return std::nullopt;
      Same &= *SameAddressIndex;
      const JumpTableValueOccurrence *LeftOccurrences[] = {
          Left ? &Left->SelectedBase : nullptr,
          Left ? &Left->SelectCondition : nullptr,
          Left ? &Left->PositiveBlendArm : nullptr,
          Left ? &Left->NegativeBlendArm : nullptr,
          Left ? &Left->PositiveMask : nullptr,
          Left ? &Left->NegativeMask : nullptr};
      const JumpTableValueOccurrence *RightOccurrences[] = {
          Right ? &Right->SelectedBase : nullptr,
          Right ? &Right->SelectCondition : nullptr,
          Right ? &Right->PositiveBlendArm : nullptr,
          Right ? &Right->NegativeBlendArm : nullptr,
          Right ? &Right->PositiveMask : nullptr,
          Right ? &Right->NegativeMask : nullptr};
      for (size_t I = 0; I < std::size(LeftOccurrences); ++I) {
        const std::optional<bool> SameOccurrence = SameOccurrenceNoShortCircuit(
            LeftOccurrences[I], RightOccurrences[I]);
        if (!SameOccurrence)
          return std::nullopt;
        Same &= *SameOccurrence;
      }
      return Same;
    };
    auto SameLoadRoleVectorNoShortCircuit =
        [&](const auto &Left, const auto &Right) -> std::optional<bool> {
      if (!ConsumeProposalStageEvidence(1))
        return std::nullopt;
      bool Same = Left.size() == Right.size();
      for (size_t I = 0; I < std::max(Left.size(), Right.size()); ++I) {
        const auto *LeftRole = I < Left.size() ? &Left[I] : nullptr;
        const auto *RightRole = I < Right.size() ? &Right[I] : nullptr;
        const std::optional<bool> SameRole =
            SameLoadRoleNoShortCircuit(LeftRole, RightRole);
        if (!SameRole)
          return std::nullopt;
        Same &= *SameRole;
      }
      return Same;
    };
    auto SameJumpTableInfoNoShortCircuit =
        [&](const JumpTableInfo &Left,
            const JumpTableInfo &Right) -> std::optional<bool> {
      // Thirty-nine direct scalar fields plus optional-storage presence.  Each
      // dynamic container pays its size and maximum element traversal below.
      if (!ConsumeProposalStageEvidence(40))
        return std::nullopt;
      bool Same = true;
      Same &= Left.BaseAddr == Right.BaseAddr;
      Same &= Left.HasBaseAddr == Right.HasBaseAddr;
      Same &= Left.EntrySize == Right.EntrySize;
      Same &= Left.EntryStride == Right.EntryStride;
      Same &= Left.MaxEntries == Right.MaxEntries;
      Same &= Left.PhysicalCapacity == Right.PhysicalCapacity;
      Same &= Left.ExactBoundedRelativeRelocationSlots ==
              Right.ExactBoundedRelativeRelocationSlots;
      Same &= Left.IndexDomainAuthenticated == Right.IndexDomainAuthenticated;
      Same &= Left.AuthenticatedGuardBound == Right.AuthenticatedGuardBound;
      Same &= Left.AuthenticatedModuloBound == Right.AuthenticatedModuloBound;
      Same &= Left.ExactPhysicalStorageRange.has_value() ==
              Right.ExactPhysicalStorageRange.has_value();
      Same &= Left.IsRelative == Right.IsRelative;
      Same &= Left.IsSigned == Right.IsSigned;
      Same &= Left.RelocAbsolute == Right.RelocAbsolute;
      Same &= Left.RelocBounded == Right.RelocBounded;
      Same &= Left.TargetBase == Right.TargetBase;
      Same &= Left.HasTargetBase == Right.HasTargetBase;
      Same &= Left.IsPEImageRelativeRVA == Right.IsPEImageRelativeRVA;
      Same &= Left.EntryScale == Right.EntryScale;
      Same &= Left.NormBase == Right.NormBase;
      Same &= Left.NormShift == Right.NormShift;
      Same &= Left.Stride == Right.Stride;
      Same &= Left.PreScaledIndex == Right.PreScaledIndex;
      Same &= Left.UseSharedDispatchSelector == Right.UseSharedDispatchSelector;
      Same &= Left.TwoTableSelect == Right.TwoTableSelect;
      Same &= Left.CompositeShapeClaimed == Right.CompositeShapeClaimed;
      Same &= Left.TwoLevelIndex == Right.TwoLevelIndex;
      Same &= Left.TwoTableOffset == Right.TwoTableOffset;
      Same &= Left.TwoTableHiPositive == Right.TwoTableHiPositive;
      Same &= Left.MutatedUnsafe == Right.MutatedUnsafe;
      Same &= Left.IndexReg == Right.IndexReg;
      Same &= Left.IndexUseAddr == Right.IndexUseAddr;
      Same &= Left.IndexUseSeq == Right.IndexUseSeq;
      Same &= Left.IndexValueDefinedAtUse == Right.IndexValueDefinedAtUse;
      Same &= Left.TableLoadAddr == Right.TableLoadAddr;
      Same &= Left.TableLoadSeq == Right.TableLoadSeq;
      Same &= Left.RequiresCompleteCFGProof == Right.RequiresCompleteCFGProof;
      Same &= Left.HasControllingGuard == Right.HasControllingGuard;
      Same &= Left.IncompleteGuardDomain == Right.IncompleteGuardDomain;
      Same &= Left.SemanticGuardDomainAmbiguous ==
              Right.SemanticGuardDomainAmbiguous;

      const std::optional<bool> SameIndexValue = SameNdVarNoShortCircuit(
          &Left.IndexValueAtUse, &Right.IndexValueAtUse);
      if (!SameIndexValue)
        return std::nullopt;
      Same &= *SameIndexValue;
      const std::optional<bool> SameCoordinates =
          SameScalarVectorNoShortCircuit(Left.AuthenticatedMaskCoordinates,
                                         Right.AuthenticatedMaskCoordinates);
      if (!SameCoordinates)
        return std::nullopt;
      Same &= *SameCoordinates;
      const std::optional<bool> SameWitnesses = SameWitnessVectorNoShortCircuit(
          Left.AuthenticatedMaskKnownOneWitnesses,
          Right.AuthenticatedMaskKnownOneWitnesses);
      if (!SameWitnesses)
        return std::nullopt;
      Same &= *SameWitnesses;
      const std::optional<bool> SameStorageRanges =
          SameStorageRangeVectorNoShortCircuit(Left.StorageRanges,
                                               Right.StorageRanges);
      if (!SameStorageRanges)
        return std::nullopt;
      Same &= *SameStorageRanges;
      if (Left.ExactPhysicalStorageRange || Right.ExactPhysicalStorageRange) {
        const std::optional<bool> SameExactStorage =
            SameStorageRangeNoShortCircuit(
                Left.ExactPhysicalStorageRange
                    ? &*Left.ExactPhysicalStorageRange
                    : nullptr,
                Right.ExactPhysicalStorageRange
                    ? &*Right.ExactPhysicalStorageRange
                    : nullptr);
        if (!SameExactStorage)
          return std::nullopt;
        Same &= *SameExactStorage;
      }
      const std::optional<bool> SameSuppression =
          SameScalarVectorNoShortCircuit(Left.SuppressibleRelocationSlots,
                                         Right.SuppressibleRelocationSlots);
      if (!SameSuppression)
        return std::nullopt;
      Same &= *SameSuppression;
      const std::optional<bool> SameAlternatives =
          SameOccurrenceVectorNoShortCircuit(Left.IndexValueAlternatives,
                                             Right.IndexValueAlternatives);
      if (!SameAlternatives)
        return std::nullopt;
      Same &= *SameAlternatives;
      const std::optional<bool> SameTargetLoads =
          SameOccurrenceVectorNoShortCircuit(Left.TargetLoads,
                                             Right.TargetLoads);
      if (!SameTargetLoads)
        return std::nullopt;
      Same &= *SameTargetLoads;
      const std::optional<bool> SameFrameStorage =
          SameFrameStorageNoShortCircuit(&Left.AuthenticatedFrameStorage,
                                         &Right.AuthenticatedFrameStorage);
      if (!SameFrameStorage)
        return std::nullopt;
      Same &= *SameFrameStorage;
      const std::optional<bool> SameDisplacedAddress =
          SameDisplacedAddressNoShortCircuit(
              &Left.AuthenticatedDisplacedAddress,
              &Right.AuthenticatedDisplacedAddress);
      if (!SameDisplacedAddress)
        return std::nullopt;
      Same &= *SameDisplacedAddress;
      const std::optional<bool> SameConsumers =
          SameOccurrenceVectorNoShortCircuit(
              Left.AuthenticatedStorageConsumers,
              Right.AuthenticatedStorageConsumers);
      if (!SameConsumers)
        return std::nullopt;
      Same &= *SameConsumers;
      const std::optional<bool> SameLoadRoles =
          SameLoadRoleVectorNoShortCircuit(Left.LoadRoles, Right.LoadRoles);
      if (!SameLoadRoles)
        return std::nullopt;
      Same &= *SameLoadRoles;
      const std::optional<bool> SameEntryIndices =
          SameScalarVectorNoShortCircuit(Left.EntryIndices, Right.EntryIndices);
      if (!SameEntryIndices)
        return std::nullopt;
      Same &= *SameEntryIndices;
      const std::optional<bool> SameRuntimeLabels =
          SameScalarVectorNoShortCircuit(Left.RuntimeCaseLabels,
                                         Right.RuntimeCaseLabels);
      if (!SameRuntimeLabels)
        return std::nullopt;
      Same &= *SameRuntimeLabels;
      const std::optional<bool> SameRuntimeSlots =
          SameScalarVectorNoShortCircuit(Left.RuntimeSlotIndices,
                                         Right.RuntimeSlotIndices);
      if (!SameRuntimeSlots)
        return std::nullopt;
      Same &= *SameRuntimeSlots;
      const std::optional<bool> SameExplicitTargets =
          SameScalarVectorNoShortCircuit(Left.ExplicitTargets,
                                         Right.ExplicitTargets);
      if (!SameExplicitTargets)
        return std::nullopt;
      Same &= *SameExplicitTargets;

      // CircleRange equality treats all empty ranges as equal, independent of
      // their inactive payload.  Evaluate every public field after reserving
      // the full five-comparison upper bound, then preserve that semantics.
      if (!ConsumeProposalStageEvidence(5))
        return std::nullopt;
      const bool LeftEmpty = Left.GuardRange.isEmpty();
      const bool RightEmpty = Right.GuardRange.isEmpty();
      bool SameActiveRange = true;
      SameActiveRange &= Left.GuardRange.getMin() == Right.GuardRange.getMin();
      SameActiveRange &= Left.GuardRange.getEnd() == Right.GuardRange.getEnd();
      SameActiveRange &=
          Left.GuardRange.getMask() == Right.GuardRange.getMask();
      SameActiveRange &=
          Left.GuardRange.getStep() == Right.GuardRange.getStep();
      Same &= LeftEmpty == RightEmpty;
      Same &= (LeftEmpty && RightEmpty) ||
              (!LeftEmpty && !RightEmpty && SameActiveRange);
      return Same;
    };
    // Architecture address models are CFG proofs, not decoder-time numeric
    // facts.  Recompute them from the currently rebuilt graph before every
    // resolver stage so a frame initializer can consume the same exact
    // occurrence certificate that final module arbitration will replay.  The
    // next stage rebuilds these transactionally after any newly published
    // table edges change reachability.
    const bool RefreshAArch64Addresses = Img.Arch == Arch::AArch64;
    const bool RefreshARMAddresses =
        Img.Arch == Arch::ARM && Img.isELF() && Img.getPointerSize() == 4;
    const bool RefreshI386Addresses = Img.Arch == Arch::X86 && Img.isELF() &&
                                      Img.getPointerSize() == 4 &&
                                      !Img.I386GOTPCFields.empty();
    if (RefreshAArch64Addresses || RefreshARMAddresses ||
        RefreshI386Addresses) {
      ActiveJumpTableProofRoots.reset();
      JumpTableProofContextComplete = true;
      if (RefreshAArch64Addresses)
        completeExactAArch64PageBases(Img);
      if (RefreshARMAddresses)
        completeExactARMRelativeLiteralAddresses(Img);
      if (RefreshI386Addresses)
        completeExactI386GOTBaseModels(Img);
      JumpTableProofContextComplete = false;
    }

    std::vector<va_t> CandidateAddrs;
    // First build an immutable candidate-address inventory.  Every instruction
    // can conservatively be retained, so prepay the vector's full ownership
    // and the two read-only ordered lookups before reserving its storage.  The
    // later phases separately reserve old-state destruction and proposal-map
    // rollback before either proposal container can be mutated.
    const size_t ResolvedLookupWork =
        OrderedLookupWork(std::max(Insns.size(), ResolvedTableInfo.size()));
    const size_t PriorLookupWork =
        OrderedLookupWork(PriorStrongJumpTableProposals.size());
    const size_t PriorProvisionalLookupWork =
        OrderedLookupWork(PriorProvisionalRelativeEdges.size());
    const size_t RollbackLookupCeiling =
        OrderedLookupWork(std::numeric_limits<size_t>::max());
    const size_t MarkerRollbackLookupWork = RollbackLookupCeiling + 1;
    if (!ConsumeProposalStageProducts(
            {// CandidateAddrs owns a reserved buffer and its eventual cleanup
             // (2N), one retained value per candidate (N), and the complete
             // source-instruction traversal (N).  Fixed vector lifetime is
             // paid separately below.
             {Insns.size(), 4},
             {Insns.size(), ResolvedLookupWork},
             {Insns.size(), PriorLookupWork},
             {Insns.size(), PriorProvisionalLookupWork}}) ||
        !ConsumeProposalStageEvidence(2)) {
      CandidateProposalStageActive = false;
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }
    CandidateAddrs.reserve(Insns.size());
    for (auto &[Addr, Rec] : Insns) {
      if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall)
        continue;
      auto Info = ResolvedTableInfo.find(Addr);
      const bool NeedsRevalidation = Info != ResolvedTableInfo.end() &&
                                     Info->second.RequiresCompleteCFGProof;
      if (Rec.JumpTableTargets.empty() || NeedsRevalidation ||
          PriorStrongJumpTableProposals.count(Addr) ||
          PriorProvisionalRelativeEdges.count(Addr)) {
        CandidateAddrs.push_back(Addr);
      }
    }

    // This second traversal is real work of its own.  Prepay its source visit
    // and both ordered lookups at the maximum possible final tree height; the
    // stage may decode additional instructions before rollback actually uses
    // the corresponding cleanup reservation.
    if (!ConsumeProposalStageProducts(
            {{CandidateAddrs.size(), size_t{1} + 2 * RollbackLookupCeiling}})) {
      CandidateProposalStageActive = false;
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }

    // CandidateAddrs is now an immutable, read-only stage inventory.  Before
    // inserting any address into the mutation set, reserve destruction of all
    // pre-existing persistent state for the complete batch.  A preceding
    // candidate may exhaust before a later candidate runs, yet rollback still
    // clears that later candidate's old targets/metadata; its per-candidate
    // reservation therefore cannot be deferred to the execution loop.
    bool StageStartRollbackStateReserved = true;
    for (va_t Addr : CandidateAddrs) {
      auto It = Insns.find(Addr);
      auto Info = ResolvedTableInfo.find(Addr);
      const size_t OldTargetCount =
          It == Insns.end() ? 0 : It->second.JumpTableTargets.size();
      const bool HasOldPersistentState =
          OldTargetCount != 0 || Info != ResolvedTableInfo.end();
      if (HasOldPersistentState &&
          ProposalOldStateCleanupEvidenceExhaustionForTesting) {
        ProposalOldStateCleanupEvidenceExhaustionForTesting = false;
        CandidateProposalStageEvidenceRemaining = 0;
        CandidateProposalStageEvidenceIncomplete = true;
        ProposalCleanupEvidenceForTesting.OldStateExhausted = true;
        StageStartRollbackStateReserved = false;
        break;
      }
      if (!ConsumeProposalStageEvidence(OldTargetCount) ||
          (Info != ResolvedTableInfo.end() &&
           !ConsumeJumpTableInfoLifecycle(Info->second))) {
        if (HasOldPersistentState)
          ProposalCleanupEvidenceForTesting.OldStateExhausted = true;
        StageStartRollbackStateReserved = false;
        break;
      }
      if (HasOldPersistentState)
        ProposalCleanupEvidenceForTesting.OldStateReserved = true;
    }
    if (!StageStartRollbackStateReserved) {
      CandidateProposalStageActive = false;
      CandidateProposalStageMutationAddrs.clear();
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      StrongJumpTableProposalOutcomes.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }

    // Only after every old dynamic object is covered may stage bookkeeping
    // itself become persistent.  Pay the third CandidateAddrs traversal, both
    // ordered-container insertions and node/value lifetimes, exact rollback,
    // and transactional incomplete-marker cleanup before either insert.
    const size_t CandidateInventoryLookup =
        OrderedLookupWork(CandidateAddrs.size());
    if (!ConsumeProposalStageProducts(
            {// Third-phase CandidateAddrs source traversal.
             {CandidateAddrs.size(), 1},
             // Mutation set lookup plus node/value/future destruction.
             {CandidateAddrs.size(), CandidateInventoryLookup},
             {CandidateAddrs.size(), 3},
             // Outcome map lookup plus node/value/future destruction.
             {CandidateAddrs.size(), CandidateInventoryLookup},
             {CandidateAddrs.size(), 3},
             // Rollback source/clear/erase constants and final-height lookups.
             {CandidateAddrs.size(), 3},
             {CandidateAddrs.size(), RollbackLookupCeiling},
             {CandidateAddrs.size(), RollbackLookupCeiling},
             {CandidateAddrs.size(), RollbackLookupCeiling},
             // Marker commit/rollback owns saved nodes plus at most one stack
             // and one index marker per exact stage-start candidate.
             {SavedStackTableEvidenceIncompleteBranches.size(), 1},
             {SavedIndexDomainEvidenceIncompleteBranches.size(), 1},
             {CandidateAddrs.size(), 2},
             {CandidateAddrs.size(), MarkerRollbackLookupWork},
             {CandidateAddrs.size(), MarkerRollbackLookupWork}})) {
      CandidateProposalStageActive = false;
      CandidateProposalStageMutationAddrs.clear();
      NextStrongJumpTableProposals.clear();
      NextProvisionalRelativeEdges.clear();
      StrongJumpTableProposalOutcomes.clear();
      RestoreIncompleteBranchMarkers();
      continue;
    }
    IncompleteBranchMarkerCleanupReserved = true;
    for (va_t Addr : CandidateAddrs) {
      CandidateProposalStageMutationAddrs.insert(Addr);
      StrongJumpTableProposalOutcomes.emplace(
          Addr, StrongJumpTableProposalOutcome::DefinitiveLocalProofLoss);
    }

    bool MadeProgress = false;
    bool RefreshedProofMetadata = false;
    for (va_t UA : CandidateAddrs) {
      // Charge the invocation itself before resolveJumpTable snapshots the
      // remaining stage balance into its local account.  Every nested helper
      // then consumes only min(per-candidate-cap, stage-remaining); the exact
      // used delta is debited when the invocation returns.
      const size_t CurrentResolvedLookupWork =
          OrderedLookupWork(std::max(Insns.size(), ResolvedTableInfo.size()));
      const size_t CurrentMaskLookupWork = OrderedLookupWork(
          std::max(Insns.size(), CandidateFixedPointExplorationTargets.size()));
      if (!ConsumeProposalStageProducts({
              // Insns lookup plus the resolver's erase and the three outer
              // before/after comparison lookups.  Resolver writes debit the
              // nested candidate account and flow back as its used delta.
              {1, OrderedLookupWork(Insns.size())},
              {4, CurrentResolvedLookupWork},
              // Outer erase/find of the exploration record.
              {2, CurrentMaskLookupWork},
              // Quarantine and incomplete-domain set lookups.
              {1, OrderedLookupWork(QuarantinedJumpTableProposals.size())},
              {2,
               OrderedLookupWork(IndexDomainEvidenceIncompleteBranches.size())},
              {1, 1},
              // CandidateOutcome lookup.  Proposal/strong-set insertions
              // occur inside the candidate-local account and their exact
              // delta is debited from this same stage balance on return.
              {1, OrderedLookupWork(StrongJumpTableProposalOutcomes.size())},
          }))
        break;
      auto It = Insns.find(UA);
      if (It == Insns.end())
        continue;

      auto PriorInfo = ResolvedTableInfo.find(UA);
      const bool WasProofDependent = PriorInfo != ResolvedTableInfo.end() &&
                                     PriorInfo->second.RequiresCompleteCFGProof;

      if (It->second.JumpTableTargets.empty() && !PrepayTargetSetOperations(1))
        break;
      if (It->second.JumpTableTargets.empty() &&
          resolveRelocatedInteriorBranch(Img, It->second)) {
        const va_t Target = It->second.BranchTarget;
        if (Target != InvalidVA) {
          BlockStarts.insert(Target);
          if (!ExploredAddrs.count(Target))
            explore(Img, Dec, Target);
        }
        MadeProgress = true;
        continue;
      }

      // resolveJumpTable erases the old map entry first.  Move its dynamic
      // metadata into the comparison snapshot instead of copying it before
      // the candidate-local account begins.
      // The immutable stage-inventory preflight reserved destruction of every
      // old persistent object before any candidate mutation.  Move this map
      // value into the comparison snapshot without duplicating its storage;
      // candidate-local accounting separately reserves any replacement.
      PriorInfo = ResolvedTableInfo.find(UA);
      std::optional<JumpTableInfo> OldInfo;
      if (PriorInfo != ResolvedTableInfo.end())
        OldInfo.emplace(std::move(PriorInfo->second));
      CandidateFixedPointExplorationTargets.erase(UA);
      JumpTableProofContextComplete = true;
      std::vector<va_t> Targets;
      {
        struct ProposalOutcomeTrackingScope {
          bool &Tracked;
          const bool Saved;
          explicit ProposalOutcomeTrackingScope(bool &Tracked)
              : Tracked(Tracked), Saved(Tracked) {
            Tracked = true;
          }
          ~ProposalOutcomeTrackingScope() { Tracked = Saved; }
        } TrackProposalOutcome{CandidateProposalOutcomeTracked};
        Targets = resolveJumpTable(Img, It->second);
      }
      JumpTableProofContextComplete = false;
      if (CandidateProposalStageEvidenceIncomplete)
        break;
      auto NewInfo = ResolvedTableInfo.find(UA);
      bool MetadataChanged =
          OldInfo.has_value() != (NewInfo != ResolvedTableInfo.end());
      if (OldInfo && NewInfo != ResolvedTableInfo.end()) {
        const std::optional<bool> SameMetadata =
            SameJumpTableInfoNoShortCircuit(*OldInfo, NewInfo->second);
        if (!SameMetadata)
          break;
        MetadataChanged = !*SameMetadata;
      }
      RefreshedProofMetadata |= WasProofDependent && MetadataChanged;
      // Old/new destruction was reserved before materialization.  Charge only
      // the element-wise equality walk here, still before comparing or moving
      // either target vector.
      const std::optional<size_t> TargetComparisonWork =
          detail::scalarVectorComparisonWork(It->second.JumpTableTargets.size(),
                                             Targets.size());
      if (!TargetComparisonWork ||
          !ConsumeProposalStageEvidence(*TargetComparisonWork))
        break;
      bool SameTargets = Targets.size() == It->second.JumpTableTargets.size();
      for (size_t I = 0;
           I < std::min(Targets.size(), It->second.JumpTableTargets.size());
           ++I)
        SameTargets &= Targets[I] == It->second.JumpTableTargets[I];
      if (!SameTargets) {
        It->second.JumpTableTargets = std::move(Targets);
        MadeProgress = true;
      }
      const std::vector<va_t> &PublishedTargets = It->second.JumpTableTargets;
      if (!PublishedTargets.empty()) {
        if (!ConsumeProposalStageEvidence(
                OrderedLookupWork(EverPublishedJumpTableBranches.size()) + 1))
          break;
        EverPublishedJumpTableBranches.insert(UA);
      }

      if (!ConsumeProposalStageEvidence(
              OrderedLookupWork(CandidateFixedPointExplorationTargets.size())))
        break;
      auto Exploration = CandidateFixedPointExplorationTargets.find(UA);
      if (PublishedTargets.empty() &&
          Exploration != CandidateFixedPointExplorationTargets.end()) {
        if (!PrepayTargetSetOperations(Exploration->second.Targets.size()))
          break;
        for (va_t T : Exploration->second.Targets) {
          MadeProgress |= BlockStarts.insert(T).second;
          if (!ExploredAddrs.count(T)) {
            explore(Img, Dec, T);
            MadeProgress = true;
          }
        }
      }

      if (!PrepayTargetSetOperations(PublishedTargets.size()))
        break;
      for (va_t T : PublishedTargets) {
        // A complete-CFG proof can recover a target only after recursive
        // descent already decoded it as ordinary fall-through.  It is still a
        // distinct jump-table destination and must split the containing block;
        // exploration state answers only whether its bytes need decoding.
        MadeProgress |= BlockStarts.insert(T).second;
        if (!ExploredAddrs.count(T)) {
          explore(Img, Dec, T);
          MadeProgress = true;
        }
      }
    }

    auto RollBackIncompleteProposalStage = [&]() {
      // A shared-stage exhaustion cannot publish the prefix that happened to
      // run first.  Remove every candidate result from this immutable stage,
      // preserve Prior for a bounded retry, and discard all provisional mask
      // exploration.  Decoded bytes may remain cached, but no edge/metadata
      // certificate from the incomplete stage survives rebuildBlocks.
      bool SawForcedUntrackedCandidate = false;
      bool ForcedUntrackedCandidateTargetsCleared = false;
      for (va_t Addr : CandidateProposalStageMutationAddrs) {
        auto It = Insns.find(Addr);
        const bool IsForcedUntrackedCandidate =
            Addr == ForcedUntrackedJumpTableCandidateAddrForTesting;
        if (It != Insns.end()) {
          if (IsForcedUntrackedCandidate)
            UntrackedJumpTableCandidateProvisionalStateObservedForTesting |=
                !It->second.JumpTableTargets.empty();
          It->second.JumpTableTargets.clear();
        }
        const size_t ErasedResolved = ResolvedTableInfo.erase(Addr);
        if (IsForcedUntrackedCandidate) {
          SawForcedUntrackedCandidate = true;
          UntrackedJumpTableCandidateProvisionalStateObservedForTesting |=
              ErasedResolved != 0;
          ForcedUntrackedCandidateTargetsCleared =
              It == Insns.end() || It->second.JumpTableTargets.empty();
        }
      }
      CandidateFixedPointExplorationTargets.clear();
      NextProvisionalRelativeEdges.clear();
      bool ForcedUntrackedCandidateResolvedCleared = false;
      bool ForcedUntrackedCandidateExplorationCleared = false;
      if (SawForcedUntrackedCandidate) {
        ForcedUntrackedCandidateResolvedCleared =
            ResolvedTableInfo.count(
                ForcedUntrackedJumpTableCandidateAddrForTesting) == 0;
        ForcedUntrackedCandidateExplorationCleared =
            CandidateFixedPointExplorationTargets.count(
                ForcedUntrackedJumpTableCandidateAddrForTesting) == 0;
      }
      if (UntrackedJumpTableCandidateExhaustedThisStageForTesting) {
        UntrackedJumpTableCandidateRollbackObservedForTesting = true;
        UntrackedJumpTableCandidateStateClearedOnRollbackForTesting |=
            SawForcedUntrackedCandidate &&
            ForcedUntrackedCandidateTargetsCleared &&
            ForcedUntrackedCandidateResolvedCleared &&
            ForcedUntrackedCandidateExplorationCleared;
      }
      CandidateProposalStageMutationAddrs.clear();
      NextStrongJumpTableProposals.clear();
      StrongJumpTableProposalOutcomes.clear();
      StageAmbiguousI386GOTPCBranches.clear();
      StageReplayedI386GOTPCKeys.clear();
      CandidateProposalStageActive = false;
      RestoreIncompleteBranchMarkers();
      ProposalStageRollbackMutatedQuarantineForTesting |=
          QuarantinedJumpTableProposals.size() != QuarantineSizeAtStageStart;
      rebuildBlocks(Func);
    };
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }

    // A stage observes only PriorStrongJumpTableProposals.  Outcomes are
    // explicit: an independently complete loss is quarantined monotonically,
    // while evidence exhaustion invalidates the whole stage above and can
    // never masquerade as a definitive loss.  Compare the ordered maps
    // manually so every key/vector visit is prepaid before equality or state
    // mutation; std::map's deep operator== would hide attacker-shaped work.
    for (const auto &[Addr, Outcome] : StrongJumpTableProposalOutcomes) {
      (void)Addr;
      if (!ConsumeProposalStageEvidence(1))
        break;
      if (Outcome == StrongJumpTableProposalOutcome::EvidenceIncomplete) {
        CandidateProposalStageEvidenceIncomplete = true;
        break;
      }
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }

    if (PriorProvisionalRelativeEdges.size() >
            std::numeric_limits<size_t>::max() -
                PriorStrongJumpTableProposals.size() ||
        !ConsumeProposalStageEvidence(PriorStrongJumpTableProposals.size() +
                                      PriorProvisionalRelativeEdges.size())) {
      RollBackIncompleteProposalStage();
      continue;
    }
    std::vector<va_t> DefinitiveLosses;
    DefinitiveLosses.reserve(PriorStrongJumpTableProposals.size() +
                             PriorProvisionalRelativeEdges.size());
    size_t QuarantineTailWork = 0;
    auto RecordDefinitiveLoss = [&](va_t LostAddr) {
      // A self-replay token may also have a strong proposal at the same
      // address.  Prepay the duplicate scan and append only one quarantine
      // record, independent of which ordered proposal map observes the loss
      // first.
      if (!ConsumeProposalStageEvidence(DefinitiveLosses.size() + 2))
        return false;
      if (std::find(DefinitiveLosses.begin(), DefinitiveLosses.end(),
                    LostAddr) != DefinitiveLosses.end())
        return true;
      if (DefinitiveLosses.size() > std::numeric_limits<size_t>::max() -
                                        QuarantinedJumpTableProposals.size()) {
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      const size_t Lookup = OrderedLookupWork(
          QuarantinedJumpTableProposals.size() + DefinitiveLosses.size());
      if (Lookup > std::numeric_limits<size_t>::max() - 1 ||
          QuarantineTailWork >
              std::numeric_limits<size_t>::max() - (Lookup + 1)) {
        CandidateProposalStageEvidenceIncomplete = true;
        return false;
      }
      QuarantineTailWork += Lookup + 1;
      DefinitiveLosses.push_back(LostAddr);
      return true;
    };
    size_t PriorClearWork = PriorStrongJumpTableProposals.size();
    auto AccumulatePriorClearWork =
        [&](const StrongJumpTableRoleProposal &Proposal) {
          const size_t Max = std::numeric_limits<size_t>::max();
          if (Proposal.StorageRanges.size() > Max - PriorClearWork) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorClearWork += Proposal.StorageRanges.size();
          if (Proposal.LoadRoles.size() > Max - PriorClearWork) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorClearWork += Proposal.LoadRoles.size();
          if (Proposal.SuppressibleRelocationSlots.size() >
              Max - PriorClearWork) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorClearWork += Proposal.SuppressibleRelocationSlots.size();
          return true;
        };
    auto PreflightComparisonWork = [&](std::optional<size_t> Work) {
      return ConsumeProposalStageEvidence(
          Work.value_or(std::numeric_limits<size_t>::max()));
    };
    auto SameStorageRange = [](const JumpTableStorageRange &Left,
                               const JumpTableStorageRange &Right) {
      bool Same = true;
      Same &= Left.BaseAddr == Right.BaseAddr;
      Same &= Left.EntrySize == Right.EntrySize;
      Same &= Left.EntryStride == Right.EntryStride;
      Same &= Left.PhysicalSlotCount == Right.PhysicalSlotCount;
      return Same;
    };
    auto SameLoadRole = [&](const StrongJumpTableLoadRole &Left,
                            const StrongJumpTableLoadRole &Right) {
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
    auto SameStrongProposal =
        [&](const StrongJumpTableRoleProposal &Left,
            const StrongJumpTableRoleProposal &Right) -> std::optional<bool> {
      if (!PreflightComparisonWork(
              detail::strongJumpTableProposalComparisonWork(
                  Left.StorageRanges.size(), Right.StorageRanges.size(),
                  Left.LoadRoles.size(), Right.LoadRoles.size(),
                  Left.SuppressibleRelocationSlots.size(),
                  Right.SuppressibleRelocationSlots.size())))
        return std::nullopt;
      bool Same = true;
      Same &= Left.StorageRanges.size() == Right.StorageRanges.size();
      for (size_t I = 0;
           I < std::min(Left.StorageRanges.size(), Right.StorageRanges.size());
           ++I)
        Same &= SameStorageRange(Left.StorageRanges[I], Right.StorageRanges[I]);
      const bool LeftHasExact = Left.ExactPhysicalStorageRange.has_value();
      const bool RightHasExact = Right.ExactPhysicalStorageRange.has_value();
      Same &= LeftHasExact == RightHasExact;
      if (LeftHasExact && RightHasExact)
        Same &= SameStorageRange(*Left.ExactPhysicalStorageRange,
                                 *Right.ExactPhysicalStorageRange);
      Same &= Left.LoadRoles.size() == Right.LoadRoles.size();
      for (size_t I = 0;
           I < std::min(Left.LoadRoles.size(), Right.LoadRoles.size()); ++I)
        Same &= SameLoadRole(Left.LoadRoles[I], Right.LoadRoles[I]);
      Same &= Left.SuppressibleRelocationSlots.size() ==
              Right.SuppressibleRelocationSlots.size();
      for (size_t I = 0; I < std::min(Left.SuppressibleRelocationSlots.size(),
                                      Right.SuppressibleRelocationSlots.size());
           ++I)
        Same &= Left.SuppressibleRelocationSlots[I] ==
                Right.SuppressibleRelocationSlots[I];
      Same &= Left.ProofRank == Right.ProofRank;
      return Same;
    };
    auto SameProvisionalProposal =
        [&](const ProvisionalRelativeEdgeProposal &Left,
            const ProvisionalRelativeEdgeProposal &Right)
        -> std::optional<bool> {
      if (!PreflightComparisonWork(
              detail::provisionalRelativeProposalComparisonWork(
                  Left.LoadRoles.size(), Right.LoadRoles.size(),
                  Left.Targets.size(), Right.Targets.size())))
        return std::nullopt;
      bool Same = SameStorageRange(Left.Storage, Right.Storage);
      Same &= Left.AuthenticatesPhysicalStorage ==
              Right.AuthenticatesPhysicalStorage;
      Same &= Left.CompleteDenseRuntimeCoordinates ==
              Right.CompleteDenseRuntimeCoordinates;
      Same &=
          Left.StablePublishedTargetReplay == Right.StablePublishedTargetReplay;
      const bool LeftHasRuntime = Left.CompleteRuntimeStorageRange.has_value();
      const bool RightHasRuntime =
          Right.CompleteRuntimeStorageRange.has_value();
      Same &= LeftHasRuntime == RightHasRuntime;
      if (LeftHasRuntime && RightHasRuntime)
        Same &= SameStorageRange(*Left.CompleteRuntimeStorageRange,
                                 *Right.CompleteRuntimeStorageRange);
      Same &= Left.TargetAnchor == Right.TargetAnchor;
      Same &= Left.EntryScale == Right.EntryScale;
      Same &= Left.IsSigned == Right.IsSigned;
      Same &= Left.LoadRoles.size() == Right.LoadRoles.size();
      for (size_t I = 0;
           I < std::min(Left.LoadRoles.size(), Right.LoadRoles.size()); ++I)
        Same &= SameLoadRole(Left.LoadRoles[I], Right.LoadRoles[I]);
      Same &= Left.Targets.size() == Right.Targets.size();
      for (size_t I = 0;
           I < std::min(Left.Targets.size(), Right.Targets.size()); ++I)
        Same &= Left.Targets[I] == Right.Targets[I];
      Same &= Left.ProofRank == Right.ProofRank;
      return Same;
    };
    bool ProposalUniverseChanged = PriorStrongJumpTableProposals.size() !=
                                   NextStrongJumpTableProposals.size();
    if (!PreflightComparisonWork(detail::orderedProposalMapMergeComparisonWork(
            PriorStrongJumpTableProposals.size(),
            NextStrongJumpTableProposals.size()))) {
      RollBackIncompleteProposalStage();
      continue;
    }
    auto PriorIt = PriorStrongJumpTableProposals.begin();
    auto NextIt = NextStrongJumpTableProposals.begin();
    while (PriorIt != PriorStrongJumpTableProposals.end() ||
           NextIt != NextStrongJumpTableProposals.end()) {
      if (NextIt == NextStrongJumpTableProposals.end() ||
          (PriorIt != PriorStrongJumpTableProposals.end() &&
           PriorIt->first < NextIt->first)) {
        if (!AccumulatePriorClearWork(PriorIt->second)) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        const va_t LostAddr = PriorIt->first;
        if (!ConsumeProposalStageEvidence(
                OrderedLookupWork(StrongJumpTableProposalOutcomes.size())))
          break;
        auto OutcomeIt = StrongJumpTableProposalOutcomes.find(LostAddr);
        if (OutcomeIt == StrongJumpTableProposalOutcomes.end() ||
            OutcomeIt->second ==
                StrongJumpTableProposalOutcome::EvidenceIncomplete) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        if (OutcomeIt->second !=
                StrongJumpTableProposalOutcome::DefinitiveLocalProofLoss &&
            OutcomeIt->second != StrongJumpTableProposalOutcome::
                                     SelfReplayDefinitiveLocalProofLoss &&
            OutcomeIt->second != StrongJumpTableProposalOutcome::
                                     AwaitingSiblingRuntimeCertificate &&
            OutcomeIt->second !=
                StrongJumpTableProposalOutcome::SemanticOpaque) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        if (OutcomeIt->second ==
            StrongJumpTableProposalOutcome::SemanticOpaque) {
          // Complete semantic ambiguity removes the provisional role but is
          // not a monotonic proof loss: the next immutable graph must replay
          // MayDepend before the branch-level opaque certificate can commit.
          ProposalUniverseChanged = true;
          ++PriorIt;
          continue;
        }
        if (OutcomeIt->second ==
            StrongJumpTableProposalOutcome::AwaitingSiblingRuntimeCertificate) {
          // The candidate saw a distinct raw sibling in the frozen prior
          // universe.  Defer quarantine only if it also retained its own edge
          // for the next immutable replay; this carries no strong authority.
          // An ordinary self-edge has the definitive outcome above and cannot
          // spin until the global retry limit.
          if (!ConsumeProposalStageEvidence(
                  OrderedLookupWork(NextProvisionalRelativeEdges.size())))
            break;
          if (NextProvisionalRelativeEdges.count(LostAddr)) {
            ProposalUniverseChanged = true;
            ++PriorIt;
            continue;
          }
        }
        if (!RecordDefinitiveLoss(LostAddr))
          break;
        ProposalUniverseChanged = true;
        ++PriorIt;
        continue;
      }
      if (PriorIt == PriorStrongJumpTableProposals.end() ||
          NextIt->first < PriorIt->first) {
        ProposalUniverseChanged = true;
        ++NextIt;
        continue;
      }

      const StrongJumpTableRoleProposal &Prior = PriorIt->second;
      const StrongJumpTableRoleProposal &Next = NextIt->second;
      const std::optional<bool> Same = SameStrongProposal(Prior, Next);
      if (!Same || !AccumulatePriorClearWork(Prior))
        break;
      ProposalUniverseChanged |= !*Same;
      ++PriorIt;
      ++NextIt;
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }

    // Provisional relative edges form a second immutable universe.  They are
    // compared and swapped transactionally like strong role proposals, but a
    // lost edge is never quarantined: it grants only a temporary resolver-graph
    // overlay and must disappear before the final fixed point can commit.
    bool ProvisionalEdgeUniverseChanged =
        PriorProvisionalRelativeEdges.size() !=
        NextProvisionalRelativeEdges.size();
    if (!PreflightComparisonWork(detail::orderedProposalMapMergeComparisonWork(
            PriorProvisionalRelativeEdges.size(),
            NextProvisionalRelativeEdges.size()))) {
      RollBackIncompleteProposalStage();
      continue;
    }
    size_t PriorProvisionalClearWork = PriorProvisionalRelativeEdges.size();
    auto AccumulateProvisionalClearWork =
        [&](const ProvisionalRelativeEdgeProposal &Proposal) {
          const size_t Max = std::numeric_limits<size_t>::max();
          if (Proposal.LoadRoles.size() > Max - PriorProvisionalClearWork ||
              Proposal.Targets.size() >
                  Max - PriorProvisionalClearWork - Proposal.LoadRoles.size()) {
            CandidateProposalStageEvidenceIncomplete = true;
            return false;
          }
          PriorProvisionalClearWork +=
              Proposal.LoadRoles.size() + Proposal.Targets.size();
          return true;
        };
    auto PriorEdgeIt = PriorProvisionalRelativeEdges.begin();
    auto NextEdgeIt = NextProvisionalRelativeEdges.begin();
    while (PriorEdgeIt != PriorProvisionalRelativeEdges.end() ||
           NextEdgeIt != NextProvisionalRelativeEdges.end()) {
      if (NextEdgeIt == NextProvisionalRelativeEdges.end() ||
          (PriorEdgeIt != PriorProvisionalRelativeEdges.end() &&
           PriorEdgeIt->first < NextEdgeIt->first)) {
        ProvisionalEdgeUniverseChanged = true;
        if (!AccumulateProvisionalClearWork(PriorEdgeIt->second))
          break;
        const va_t LostAddr = PriorEdgeIt->first;
        if (!ConsumeProposalStageEvidence(
                OrderedLookupWork(StrongJumpTableProposalOutcomes.size())))
          break;
        const auto Outcome = StrongJumpTableProposalOutcomes.find(LostAddr);
        if (Outcome == StrongJumpTableProposalOutcomes.end()) {
          CandidateProposalStageEvidenceIncomplete = true;
          break;
        }
        if (Outcome->second == StrongJumpTableProposalOutcome::
                                   SelfReplayDefinitiveLocalProofLoss &&
            !RecordDefinitiveLoss(LostAddr))
          break;
        ++PriorEdgeIt;
        continue;
      }
      if (PriorEdgeIt == PriorProvisionalRelativeEdges.end() ||
          NextEdgeIt->first < PriorEdgeIt->first) {
        ProvisionalEdgeUniverseChanged = true;
        ++NextEdgeIt;
        continue;
      }
      const ProvisionalRelativeEdgeProposal &Prior = PriorEdgeIt->second;
      const ProvisionalRelativeEdgeProposal &Next = NextEdgeIt->second;
      const std::optional<bool> Same = SameProvisionalProposal(Prior, Next);
      if (!Same || !AccumulateProvisionalClearWork(Prior))
        break;
      ProvisionalEdgeUniverseChanged |= !*Same;
      ++PriorEdgeIt;
      ++NextEdgeIt;
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }
    // A producer may retain either its exact complete runtime range or, for a
    // sparse domain, only its exact ordinary published target vector.  Both
    // forms are durable candidate-local evidence rather than resolver edge
    // overlays; neither grants physical ownership.  Any raw edge proposal
    // still prevents stability and must retire in a later immutable stage.
    bool StableDurableCertificatesOnly = true;
    size_t StableRuntimeCertificateRetireWork =
        PriorProvisionalRelativeEdges.size();
    for (const auto &[Addr, Proposal] : PriorProvisionalRelativeEdges) {
      const size_t Max = std::numeric_limits<size_t>::max();
      if (!ConsumeProposalStageEvidence(
              OrderedLookupWork(Insns.size()) +
              detail::kStableRelativeRuntimeCertificateFixedWork) ||
          !ConsumeProposalStageEvidence(Proposal.Targets.size()))
        break;
      const auto Producer = Insns.find(Addr);
      const JumpTableStorageRange *RuntimeRange =
          Proposal.CompleteRuntimeStorageRange
              ? &*Proposal.CompleteRuntimeStorageRange
              : nullptr;
      bool StableCertificate = false;
      if (Producer != Insns.end()) {
        if (Proposal.StablePublishedTargetReplay) {
          StableCertificate =
              !Proposal.AuthenticatesPhysicalStorage &&
              !Proposal.CompleteDenseRuntimeCoordinates && !RuntimeRange &&
              !Proposal.Targets.empty() &&
              Proposal.Targets == Producer->second.JumpTableTargets;
        } else {
          StableCertificate =
              !Proposal.AuthenticatesPhysicalStorage &&
              !Proposal.CompleteDenseRuntimeCoordinates &&
              detail::isStableProvisionalRelativeRuntimeCertificate(
                  RuntimeRange, Proposal.Targets,
                  Producer->second.JumpTableTargets);
        }
      }
      if (!StableCertificate) {
        StableDurableCertificatesOnly = false;
        continue;
      }
      if (Proposal.LoadRoles.size() >
              Max - StableRuntimeCertificateRetireWork ||
          Proposal.Targets.size() > Max - StableRuntimeCertificateRetireWork -
                                        Proposal.LoadRoles.size()) {
        CandidateProposalStageEvidenceIncomplete = true;
        break;
      }
      StableRuntimeCertificateRetireWork +=
          Proposal.LoadRoles.size() + Proposal.Targets.size();
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      continue;
    }
    const bool StableProvisionalUniverse =
        PriorProvisionalRelativeEdges.empty() ||
        (!ProvisionalEdgeUniverseChanged && StableDurableCertificatesOnly);
    const bool StableStage =
        !MadeProgress && !RefreshedProofMetadata && !ProposalUniverseChanged &&
        !ProvisionalEdgeUniverseChanged && StableProvisionalUniverse;
    std::set<va_t> StableSafelyPublishedI386GOTPCBranches;
    if (StableStage) {
      const size_t InsnLookup = OrderedLookupWork(Insns.size());
      const size_t SafeBranchLookup =
          OrderedLookupWork(PendingAmbiguousI386GOTPCBranches.size());
      if (!ConsumeProposalStageProducts(
              {{PendingAmbiguousI386GOTPCBranches.size(),
                InsnLookup + SafeBranchLookup + 4}})) {
        CandidateProposalStageEvidenceIncomplete = true;
      }
      if (!CandidateProposalStageEvidenceIncomplete) {
        for (va_t BranchAddr : PendingAmbiguousI386GOTPCBranches) {
          const auto Branch = Insns.find(BranchAddr);
          if (Branch != Insns.end() && !Branch->second.JumpTableTargets.empty())
            StableSafelyPublishedI386GOTPCBranches.insert(BranchAddr);
        }
      }
      for (const I386GOTOFFAmbiguityReplayKey &Key :
           PendingAmbiguousI386GOTPCKeys) {
        constexpr size_t KeyWork = 5;
        const size_t ReplayLookup =
            OrderedLookupWork(StageReplayedI386GOTPCKeys.size());
        const size_t StablePublishedLookup =
            OrderedLookupWork(StableSafelyPublishedI386GOTPCBranches.size());
        if (!ConsumeProposalStageProducts(
                {// Preflight plus commit each perform the exact ordered
                 // replay/safe-publication membership checks.
                 {KeyWork, 2 * ReplayLookup + 2},
                 {2, StablePublishedLookup},
                 // One possible erase and the insertion-prepaid node cleanup.
                 {1, 1}}))
          break;
      }
      // Rebuild the derived branch index allocation-free after key retirement.
      // Prepay its full branch-by-key membership walk before persistent commit.
      if (!CandidateProposalStageEvidenceIncomplete &&
          !ConsumeProposalStageProducts(
              {{PendingAmbiguousI386GOTPCBranches.size(),
                PendingAmbiguousI386GOTPCKeys.size() + 1}})) {
        CandidateProposalStageEvidenceIncomplete = true;
      }
    }
    if (!CandidateProposalStageEvidenceIncomplete && StableStage &&
        ExhaustStableI386AmbiguityCommitTailForTesting &&
        !ExhaustedStableI386AmbiguityCommitTailForTesting &&
        !PendingAmbiguousI386GOTPCKeys.empty()) {
      ExhaustedStableI386AmbiguityCommitTailForTesting = true;
      ForcedStableAmbiguityCommitTailThisStage = true;
      ForcedPendingKeyCount = PendingAmbiguousI386GOTPCKeys.size();
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      CandidateProposalStageEvidenceRemaining = 0;
      CandidateProposalStageEvidenceIncomplete = true;
    }
    if (CandidateProposalStageEvidenceIncomplete) {
      RollBackIncompleteProposalStage();
      if (ForcedStableAmbiguityCommitTailThisStage &&
          PendingAmbiguousI386GOTPCKeys.size() == ForcedPendingKeyCount)
        CommitTailRollbackRetainedPendingI386AmbiguityForTesting = true;
      continue;
    }
    if (!CandidateProposalStageEvidenceIncomplete &&
        ExhaustProposalStageCommitTailForTesting &&
        !ExhaustedProposalStageCommitTailForTesting &&
        !DefinitiveLosses.empty()) {
      ExhaustedProposalStageCommitTailForTesting = true;
      ForcedProposalCommitTailThisStage = true;
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      CandidateProposalStageEvidenceRemaining = 0;
      CandidateProposalStageEvidenceIncomplete = true;
    }
    // Preflight every ordered insertion and every container traversal needed
    // by the commit before the first persistent mutation.  In particular, an
    // exact budget boundary cannot insert a quarantine record and then fail on
    // swap/clear, which would poison the bounded retry despite rollback.
    if (CandidateProposalStageEvidenceIncomplete ||
        !ConsumeProposalStageProducts(
            {{1, QuarantineTailWork},
             {1, 1}, // Prior/Next swap.
             {1, PriorClearWork},
             {1, 1}, // Provisional-edge Prior/Next swap.
             {1, PriorProvisionalClearWork},
             {1, StableStage ? StableRuntimeCertificateRetireWork : 0},
             {1, StrongJumpTableProposalOutcomes.size()}})) {
      ProposalStageCommitTailEvidenceExhaustedForTesting = true;
      RollBackIncompleteProposalStage();
      if (ForcedProposalCommitTailThisStage &&
          QuarantinedJumpTableProposals.size() == QuarantineSizeAtStageStart)
        ProposalStageForcedCommitTailRollbackPreservedStateForTesting = true;
      continue;
    }
    for (va_t Addr : DefinitiveLosses)
      QuarantinedJumpTableProposals.insert(Addr);
    PriorStrongJumpTableProposals.swap(NextStrongJumpTableProposals);
    NextStrongJumpTableProposals.clear();
    PriorProvisionalRelativeEdges.swap(NextProvisionalRelativeEdges);
    NextProvisionalRelativeEdges.clear();
    if (StableStage && StableDurableCertificatesOnly)
      PriorProvisionalRelativeEdges.clear();
    StrongJumpTableProposalOutcomes.clear();
    CandidateProposalStageMutationAddrs.clear();
    CandidateProposalStageActive = false;

    if (!MadeProgress) {
      // Targets can remain byte-for-byte equal while the complete CFG changes
      // case labels, range/identity metadata, or mutation state.  Publish the
      // freshly cached JumpTableInfo, then require another complete resolver
      // round on that published CFG.  Only an equal target set *and* equal
      // proof metadata in the following round is a fixed point; a metadata
      // cycle consumes the bounded retry budget and is discarded below.
      if (!StableStage) {
        // This stage committed proposal metadata, but the next round observes
        // a different proof universe.  Keep both the prior safety markers and
        // any newly incomplete candidates until a later no-progress round can
        // revalidate their removal on the stable graph.
        RestoreIncompleteBranchMarkers();
        rebuildBlocks(Func);
        continue;
      }
      CommitIncompleteBranchMarkers();
      ReachedFixedPoint = true;
      // A changed graph may publish some other table at the same branch, but
      // that does not replay this exact use/base/occurrence query.
      detail::retireReplayedI386GOTPCAmbiguities(
          PendingAmbiguousI386GOTPCKeys, StageReplayedI386GOTPCKeys,
          &StableSafelyPublishedI386GOTPCBranches);
      for (auto It = PendingAmbiguousI386GOTPCBranches.begin();
           It != PendingAmbiguousI386GOTPCBranches.end();) {
        const va_t BranchAddr = *It;
        const bool HasPendingKey =
            std::any_of(PendingAmbiguousI386GOTPCKeys.begin(),
                        PendingAmbiguousI386GOTPCKeys.end(),
                        [&](const I386GOTOFFAmbiguityReplayKey &Key) {
                          return std::get<0>(Key) == BranchAddr;
                        });
        if (!HasPendingKey)
          It = PendingAmbiguousI386GOTPCBranches.erase(It);
        else
          ++It;
      }
      AmbiguousI386GOTPCBranches.swap(StageAmbiguousI386GOTPCBranches);
      StageAmbiguousI386GOTPCBranches.clear();
      break;
    }

    // Newly decoded targets change reaching definitions and table ownership.
    // Marker deletion is a declassification and therefore cannot commit on a
    // graph-growing round; carry the conservative union into revalidation.
    RestoreIncompleteBranchMarkers();
    completeExactAArch64PageBases(Img);
    splitBlocks();
    rebuildBlocks(Func);

    LLVM_DEBUG(llvm::dbgs()
               << "  multi-stage " << (Stage + 1) << ": rebuilt to "
               << Func.Blocks.size() << " blocks\n");
  }
  CandidateProposalStageActive = false;
  CandidateProposalStageMutationAddrs.clear();

  // A proof-dependent target set may only escape after one whole round saw no
  // new decoded targets and no target-set change.  If the bounded iteration
  // budget is exhausted first, discard those provisional edges rather than
  // publishing a CFG whose validity depends on traversal order.
  if (!ReachedFixedPoint) {
    // A complete ambiguity result from a graph that never stabilized is not a
    // semantic certificate, but neither is it callback evidence.  The
    // insertion-time-prepaid pending set carries those exact candidates into
    // tail-call conversion without allocating after evidence exhaustion.
    bool Changed = false;
    for (auto &[Addr, Rec] : Insns) {
      auto Info = ResolvedTableInfo.find(Addr);
      if (Info == ResolvedTableInfo.end() ||
          !Info->second.RequiresCompleteCFGProof)
        continue;
      Changed |= !Rec.JumpTableTargets.empty();
      Rec.JumpTableTargets.clear();
      ResolvedTableInfo.erase(Info);
    }
    if (Changed)
      rebuildBlocks(Func);
    PriorProvisionalRelativeEdges.clear();
    NextProvisionalRelativeEdges.clear();
    CandidateFixedPointExplorationTargets.clear();
  }
  StageAmbiguousI386GOTPCBranches.clear();
  StageReplayedI386GOTPCKeys.clear();

  // A transient empty target set can recover in a later stage, so classify a
  // lost validated table only after convergence (or the bounded cleanup above)
  // has established the final target state.  These addresses are not generic
  // detector claims: every member published at least one concrete table edge
  // earlier in this build or in a seeded build of this same function.
  for (auto It = EverPublishedJumpTableBranches.begin();
       It != EverPublishedJumpTableBranches.end();) {
    const va_t Addr = *It;
    auto InsnIt = Insns.find(Addr);
    if (InsnIt == Insns.end() || !InsnIt->second.IsBranch ||
        !InsnIt->second.IsIndirect || InsnIt->second.IsCall) {
      It = EverPublishedJumpTableBranches.erase(It);
      continue;
    }
    if (InsnIt->second.JumpTableTargets.empty())
      LostValidatedJumpTableBranches.insert(Addr);
    ++It;
  }
  for (va_t Addr : EverStrongJumpTableProposalBranches) {
    auto InsnIt = Insns.find(Addr);
    if (InsnIt != Insns.end() && InsnIt->second.IsBranch &&
        InsnIt->second.IsIndirect && !InsnIt->second.IsCall &&
        InsnIt->second.JumpTableTargets.empty())
      LostValidatedJumpTableBranches.insert(Addr);
  }
}

} // namespace neverd
