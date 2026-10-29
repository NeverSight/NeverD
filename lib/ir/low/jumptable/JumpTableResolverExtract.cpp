//===- JumpTableResolverExtract.cpp - Jump-table result construction ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Construction of the resolver's results: inverse-normalizing table positions
/// back into the case labels the emitter dispatches on and collecting the
/// per-branch metadata into LowFunc::JumpTables.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/SourceUnwind.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

std::optional<JumpTableStorageRange>
CFGBuilder::proveInstructionLocalTableSource(
    const BinaryImage &Img, const LowFunc &F,
    const RelocatedInstructionAddressOccurrence &Source,
    size_t &WorkRemaining) {
  auto Take = [&](size_t Amount) {
    if (Amount > WorkRemaining) {
      WorkRemaining = 0;
      return false;
    }
    WorkRemaining -= Amount;
    return true;
  };
  const uint16_t PointerSize = Img.getPointerSize();
  if (!Take(32) || (Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
      !F.hasCompleteInstructionLift() || F.Blocks.empty() ||
      !F.FunctionTemporaries.empty() || F.JumpTables.size() != 1 ||
      Source.Authority != RelocatedInstructionAddressProofKind::LoaderField ||
      Source.FieldVA == InvalidVA || Source.DefinesOutput ||
      Source.OutputMayDepend || Source.PCRelativeFromInstructionEnd ||
      Source.InputIndex < 0 || !isExactAddressProvenance(Source.Provenance))
    return std::nullopt;
  const JumpTable &Candidate = F.JumpTables.front();
  if (Candidate.InsnAddr != Source.InstructionAddr || !Candidate.HasBaseAddr ||
      Candidate.BaseAddr != Source.TargetVA || Candidate.IsRelative ||
      Candidate.TwoTableSelect || Candidate.TwoLevelIndex ||
      Candidate.MutatedUnsafe || Candidate.EntrySize != PointerSize ||
      Candidate.StorageRanges.empty())
    return std::nullopt;

  size_t ImportWork = 256;
  auto Add = [&](size_t Count, size_t Cost) {
    return detail::addLinearComparisonWork(ImportWork, Count, Cost);
  };
  if (!Add(F.Blocks.size(), 96) || !Add(Img.Segments.size(), 32) ||
      !Add(Img.Sections.size(), 32) || !Add(Img.Symbols.size(), 32) ||
      !Add(Img.KnownCodeRanges.size(), 16) ||
      !Add(F.ModuleAnalysisRoots.size(), 24) ||
      !Add(F.OrdinaryModuleAnalysisRoots.size(), 24) ||
      !Add(Img.RuntimeFunctionAddrs.size(), 24) ||
      !Add(Img.VerifiedFunctionEntries.size(), 24) ||
      !Add(F.RelocatedInstructionAddressOccurrences.size(), 192) ||
      !Add(Candidate.Targets.size(), 32) ||
      !Add(Candidate.StorageRanges.size(), 512) ||
      !Add(Img.ExceptionMetadata.Functions.size(), 96) ||
      !Add(Img.ExceptionMetadata.FunctionIndex.size(), 16)) {
    WorkRemaining = 0;
    return std::nullopt;
  }
  for (const LowBlock &B : F.Blocks)
    if (ImportWork > WorkRemaining || !Add(B.Ops.size(), 384) ||
        !Add(B.InstructionBoundaries.size(), 192) || !Add(B.Succs.size(), 16) ||
        !Add(B.Preds.size(), 16)) {
      WorkRemaining = 0;
      return std::nullopt;
    }
  for (const auto &Occurrence : F.RelocatedInstructionAddressOccurrences)
    if (ImportWork > WorkRemaining ||
        !Add(Occurrence.ArithmeticProof.size(), 384)) {
      WorkRemaining = 0;
      return std::nullopt;
    }
  size_t BlockComparisonWork = 0;
  if (!detail::addLinearComparisonWork(BlockComparisonWork, F.Blocks.size(),
                                       48) ||
      (F.ExceptionMetadata && F.ExceptionMetadata->ObjC &&
       !Add(F.ExceptionMetadata->ObjC->RuntimeCalls.size(), 16))) {
    WorkRemaining = 0;
    return std::nullopt;
  }
  for (const auto &Metadata : Img.ExceptionMetadata.Functions) {
    // The constant record inventory was charged before this preflight. Bound
    // it before walking records, then prepay every nested range/ObjC scan.
    if (ImportWork > WorkRemaining || !Add(1, BlockComparisonWork) ||
        !Add(Metadata.FragmentRanges.size(), BlockComparisonWork) ||
        (Metadata.ObjC && !Add(Metadata.ObjC->RuntimeCalls.size(), 16))) {
      WorkRemaining = 0;
      return std::nullopt;
    }
  }
  if (!Take(ImportWork))
    return std::nullopt;

  if (F.ExceptionMetadata && !isPlainSourceUnwind(*F.ExceptionMetadata))
    return std::nullopt;
  const ExceptionFunction *RangeMetadata = nullptr;
  for (const auto &Metadata : Img.ExceptionMetadata.Functions) {
    const auto Intersects = [&](const ExceptionAddressRange &Range) {
      return Range.isValid() &&
             std::any_of(F.Blocks.begin(), F.Blocks.end(), [&](const auto &B) {
               return B.StartAddr < Range.End && Range.Begin < B.EndAddr;
             });
    };
    const auto ContainsPoint = [&](va_t Address) {
      return std::any_of(F.Blocks.begin(), F.Blocks.end(), [&](const auto &B) {
        return B.StartAddr <= Address && Address < B.EndAddr;
      });
    };
    const bool Relevant =
        (Metadata.Dwarf && ContainsPoint(Metadata.Dwarf->InitialLocation)) ||
        (!Metadata.CodeRange.isValid() &&
         ContainsPoint(Metadata.CodeRange.Begin)) ||
        (Metadata.FunctionEntry && Metadata.FunctionEntry == F.Entry) ||
        Intersects(Metadata.CodeRange) ||
        std::any_of(Metadata.FragmentRanges.begin(),
                    Metadata.FragmentRanges.end(), Intersects);
    if (!Relevant)
      continue;
    // findFunction returns one most-specific record; an outer, duplicate or
    // fragmented language record must not disappear behind a plain FDE or a
    // cleared/copied final LowFunc annotation.
    if (!isPlainSourceUnwind(Metadata))
      return std::nullopt;
    if (Metadata.Kind == RuntimeFunctionKind::Primary &&
        Metadata.CodeRange.Begin == F.Entry &&
        (!RangeMetadata ||
         Metadata.CodeRange.size() < RangeMetadata->CodeRange.size()))
      RangeMetadata = &Metadata;
  }

  bool LoaderFieldMatches = false;
  const auto CheckField = [&](const auto &Fields) {
    const auto It = Fields.find(Source.FieldVA);
    if (It == Fields.end())
      return true;
    const RelocatedAddressField &Field = It->second;
    if (Field.Width != Source.Width || Field.Width == 0 ||
        Field.Width > PointerSize || Field.PCRelativeFromInstructionEnd ||
        Field.TargetVA != Source.TargetVA ||
        Field.TargetOwnerVA != Source.TargetOwnerVA)
      return false;
    LoaderFieldMatches = true;
    return true;
  };
  if (!CheckField(Img.DataAddressRelocOperands) ||
      !CheckField(Img.CodeAddressRelocOperands) || !LoaderFieldMatches)
    return std::nullopt;

  // This adapter owns no raw instruction cache or previous role certificate.
  // Every operation and root comes from this immutable final function. Reject
  // TOP clones, partial boundary coverage and exceptional graphs until the
  // resolver can represent their distinct invocation contexts exactly.
  CFGBuilder Proof;
  Proof.CurrentImg = &Img;
  Proof.CurrentFuncEntry = F.Entry;
  Proof.JumpTableProofContextComplete = true;
  Proof.PersistentCFGRoots = F.ModuleAnalysisRoots;
  Proof.PersistentCFGRoots.insert(F.Entry);
  Proof.DurableCFGRoots = Proof.PersistentCFGRoots;
  Proof.OrdinaryCFGRoots = F.OrdinaryModuleAnalysisRoots;
  Proof.RelocatedInstructionAddressOccurrences =
      F.RelocatedInstructionAddressOccurrences;
  std::set<va_t> Entries = Img.RuntimeFunctionAddrs;
  Entries.insert(Img.VerifiedFunctionEntries.begin(),
                 Img.VerifiedFunctionEntries.end());
  for (const Symbol &S : Img.Symbols)
    if (S.IsFunc)
      Entries.insert(S.Addr);
  Proof.setKnownFuncEntries(&Entries);
  Proof.establishCurrentFuncRange(Img, RangeMetadata);

  LowFunc Graph;
  Graph.Entry = F.Entry;
  std::map<va_t, int> Starts;
  va_t PreviousBlockEnd = 0;
  for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
    const LowBlock &B = F.Blocks[BI];
    if (B.Id != static_cast<int>(BI) || B.InstructionBoundaries.empty() ||
        !B.ExceptionalPreds.empty() || !B.ExceptionalSuccs.empty() ||
        (BI && B.StartAddr < PreviousBlockEnd) ||
        B.StartAddr != B.InstructionBoundaries.front().Address)
      return std::nullopt;
    PreviousBlockEnd = B.EndAddr;
    if (!Starts.emplace(B.StartAddr, B.Id).second)
      return std::nullopt;
    Proof.BlockStarts.insert(B.StartAddr);
    Proof.PublishedBlockStarts.push_back(B.StartAddr);
    LowBlock GB;
    GB.Id = B.Id;
    GB.StartAddr = B.StartAddr;
    GB.EndAddr = B.EndAddr;
    GB.InstructionBoundaries = B.InstructionBoundaries;
    Graph.Blocks.push_back(std::move(GB));
    size_t NextOp = 0;
    va_t NextAddress = B.StartAddr;
    for (const LowInstructionBoundary &Boundary : B.InstructionBoundaries) {
      if (Boundary.Size == 0 || Boundary.Size > InvalidVA - Boundary.Address ||
          Boundary.Address != NextAddress || Boundary.FirstOp != NextOp ||
          Boundary.OpCount > B.Ops.size() - NextOp ||
          Boundary.Mode != InstructionMode::Default ||
          Boundary.TargetMode != LowInstructionTargetMode::Preserve)
        return std::nullopt;
      InsnRecord Rec{};
      Rec.Addr = Boundary.Address;
      Rec.Size = Boundary.Size;
      Rec.Mode = Boundary.Mode;
      Rec.Immediate = Boundary.Immediate;
      const auto Flag = [&](LowInstructionControlFlag Bit) {
        return hasLowInstructionControlFlag(Boundary.ControlFlags, Bit);
      };
      Rec.IsBranch = Flag(LowInstructionControlFlag::Branch);
      Rec.IsCond = Flag(LowInstructionControlFlag::Conditional);
      Rec.IsCall = Flag(LowInstructionControlFlag::Call);
      Rec.IsRet = Flag(LowInstructionControlFlag::Return);
      Rec.IsIndirect = Flag(LowInstructionControlFlag::Indirect);
      Rec.IsNoReturnCall = Flag(LowInstructionControlFlag::NoReturn);
      if (Flag(LowInstructionControlFlag::Terminator) ||
          Flag(LowInstructionControlFlag::InstructionGuard) ||
          (Rec.IsCall && Rec.IsRet) || (Rec.IsNoReturnCall && !Rec.IsCall) ||
          ((Rec.IsRet || (Rec.IsBranch && !Rec.IsCall) || Rec.IsNoReturnCall) &&
           &Boundary != &B.InstructionBoundaries.back()))
        return std::nullopt;
      std::set<int> Sequences;
      unsigned Controls = 0;
      for (size_t I = NextOp; I < NextOp + Boundary.OpCount; ++I) {
        const LowOp &Op = B.Ops[I];
        if (Op.Addr != Rec.Addr || Op.Seq < 0 ||
            Op.NumInputs > std::size(Op.Inputs) ||
            !Sequences.insert(Op.Seq).second)
          return std::nullopt;
        if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::RETURN) {
          ++Controls;
          if (Op.Opcode == NdOp::RETURN) {
            if (!Rec.IsRet || Rec.IsCall || Rec.IsBranch || Rec.IsCond ||
                Rec.IsIndirect || Rec.IsNoReturnCall)
              return std::nullopt;
          } else {
            const bool Call =
                Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
            const bool Indirect =
                Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INDIR_BR;
            if (Rec.IsRet || (Call && Rec.IsBranch) || Call != Rec.IsCall ||
                Indirect != Rec.IsIndirect || (!Call && !Rec.IsBranch) ||
                !Op.NumInputs || (Op.Opcode == NdOp::COND_BR) != Rec.IsCond)
              return std::nullopt;
            if (!Indirect) {
              if (!Op.Inputs[0].isConst())
                return std::nullopt;
              Rec.BranchTarget = Op.Inputs[0].Offset;
            }
          }
        }
        Rec.Ops.push_back(Op);
      }
      if (Controls != unsigned(Rec.IsCall || Rec.IsBranch || Rec.IsRet))
        return std::nullopt;
      const auto RebuiltBoundary =
          Proof.makeInstructionBoundary(Rec, Boundary.FirstOp);
      if (RebuiltBoundary.Control != Boundary.Control ||
          RebuiltBoundary.ControlFlags != Boundary.ControlFlags)
        return std::nullopt;
      if (Rec.Addr == Candidate.InsnAddr)
        Rec.JumpTableTargets = Candidate.Targets;
      else if (Rec.IsBranch && Rec.IsIndirect)
        return std::nullopt;
      if (!Proof.Insns.emplace(Rec.Addr, std::move(Rec)).second)
        return std::nullopt;
      Proof.PublishedReachableInsns.insert(Boundary.Address);
      Proof.ExploredAddrs.insert(Boundary.Address);
      NextOp += Boundary.OpCount;
      NextAddress += Boundary.Size;
    }
    if (NextOp != B.Ops.size() || NextAddress != B.EndAddr)
      return std::nullopt;
  }
  if (!F.ModuleAnalysisRoots.count(F.Entry))
    return std::nullopt;
  for (va_t Root : F.OrdinaryModuleAnalysisRoots)
    if (!F.ModuleAnalysisRoots.count(Root))
      return std::nullopt;
  for (size_t I = 0; I < F.Blocks.size(); ++I) {
    const auto &B = F.Blocks[I];
    const auto &Last = Proof.Insns.at(B.InstructionBoundaries.back().Address);
    const bool FallsThrough =
        !Last.IsRet &&
        ((Last.IsBranch && !Last.IsCall && Last.IsCond) ||
         ((!Last.IsBranch || Last.IsCall) && !Last.IsNoReturnCall));
    if (FallsThrough &&
        (I + 1 == F.Blocks.size() || F.Blocks[I + 1].StartAddr != B.EndAddr))
      return std::nullopt;
  }
  for (va_t Root : Proof.PersistentCFGRoots)
    if (!Starts.count(Root))
      return std::nullopt;
  for (va_t Target : Candidate.Targets)
    if (!Starts.count(Target))
      return std::nullopt;
  // linkSuccessors drops targets that are not block starts, whereas the
  // resolver's instruction map can attach an interior target to its owner.
  // Replaying that edge at the block head could re-execute a bypassed guard.
  for (const auto &[Addr, Rec] : Proof.Insns)
    if (Rec.IsBranch && !Rec.IsCall && !Rec.IsIndirect &&
        !Starts.count(Rec.BranchTarget))
      return std::nullopt;
  Proof.linkSuccessors(Graph, Starts);
  for (size_t I = 0; I < Graph.Blocks.size(); ++I) {
    auto GS = Graph.Blocks[I].Succs;
    auto GP = Graph.Blocks[I].Preds;
    auto FS = F.Blocks[I].Succs;
    auto FP = F.Blocks[I].Preds;
    std::sort(GS.begin(), GS.end());
    std::sort(GP.begin(), GP.end());
    std::sort(FS.begin(), FS.end());
    std::sort(FP.begin(), FP.end());
    if (GS != FS || GP != FP)
      return std::nullopt;
  }

  const auto Insn = Proof.Insns.find(Source.InstructionAddr);
  if (Insn == Proof.Insns.end())
    return std::nullopt;
  const InsnRecord &Dispatch = Insn->second;
  if (!Dispatch.IsBranch || !Dispatch.IsIndirect || Dispatch.IsCall ||
      Dispatch.IsRet || Dispatch.IsCond || Source.FieldVA < Dispatch.Addr ||
      Source.FieldVA - Dispatch.Addr >= Dispatch.Size ||
      Source.Width > Dispatch.Size - (Source.FieldVA - Dispatch.Addr))
    return std::nullopt;
  const LowOp *EncodedUse = nullptr;
  const LowOp *Load = nullptr;
  unsigned EncodedInputs = 0;
  for (const LowOp &Op : Dispatch.Ops) {
    if (Op.Opcode == NdOp::LOAD) {
      if (Load)
        return std::nullopt;
      Load = &Op;
    }
    for (unsigned I = 0; I < Op.NumInputs; ++I) {
      const NdVar &V = Op.Inputs[I];
      if (V.isConst() && V.Provenance == Source.Provenance &&
          V.Offset == Source.TargetVA &&
          V.AddressOwnerVA == Source.TargetOwnerVA) {
        ++EncodedInputs;
        if (Op.Seq == Source.OpSeq && I == unsigned(Source.InputIndex) &&
            V.Size == PointerSize)
          EncodedUse = &Op;
      }
    }
  }
  if (!EncodedUse || EncodedInputs != 1 || !Load ||
      !EncodedUse->Output.isTemp() || EncodedUse->Output.Size != PointerSize ||
      !((EncodedUse->Opcode == NdOp::INT_ADD && EncodedUse->NumInputs == 2) ||
        (EncodedUse->Opcode == NdOp::COPY && EncodedUse->NumInputs == 1)))
    return std::nullopt;
  for (unsigned I = 0; I < EncodedUse->NumInputs; ++I)
    if (EncodedUse->Inputs[I].Size != PointerSize)
      return std::nullopt;
  const LowMemoryOperandView Memory = lowMemoryOperands(*Load);
  if (!Memory.Complete || !Memory.Address || Memory.AccessSize != PointerSize ||
      !Load->Output.isTemp() || Load->Output.Size != PointerSize)
    return std::nullopt;

  // Until dereference, the encoded address may flow only through full-width
  // temporaries in this one instruction. A second use, including a register
  // COPY after the LOAD, prevents exclusivity even if the dispatch is proved.
  NdVar Address = EncodedUse->Output;
  const LowOp *Producer = EncodedUse;
  auto OverlapsTemporary = [](const NdVar &A, const NdVar &B) {
    if (!A.isTemp() || !B.isTemp())
      return false;
    if (A.Size == 0 || B.Size == 0 || A.Size > InvalidVA - A.Offset ||
        B.Size > InvalidVA - B.Offset)
      return true;
    // TEMP semantic identity does not include address provenance. Reject
    // overlapping widths as well rather than relying on projection details.
    return A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
  };
  while (true) {
    size_t ChainWork = 16;
    if (!detail::addLinearComparisonWork(ChainWork, Dispatch.Ops.size(), 16)) {
      WorkRemaining = 0;
      return std::nullopt;
    }
    if (!Take(ChainWork))
      return std::nullopt;
    const LowOp *Consumer = nullptr;
    for (const LowOp &Op : Dispatch.Ops) {
      if (&Op <= Producer)
        continue;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (OverlapsTemporary(Op.Inputs[I], Address)) {
          if (Op.Inputs[I] != Address || Consumer)
            return std::nullopt;
          Consumer = &Op;
        }
      if (OverlapsTemporary(Op.Output, Address)) {
        // A full in-place arithmetic update or LOAD ends this reaching
        // temporary version. Uses after that definition belong to the next
        // version, which the following iteration checks independently.
        if (Op.Output.Offset != Address.Offset ||
            Op.Output.Size != Address.Size || !Consumer)
          return std::nullopt;
        break;
      }
    }
    if (!Consumer)
      return std::nullopt;
    if (Consumer == Load) {
      if (*Memory.Address != Address)
        return std::nullopt;
      break;
    }
    // x86 computes the address at 32 bits, then zero-extends it into LowIR's
    // 64-bit memory-address carrier. Replay still proves the 32-bit wrap.
    if (PointerSize == 4 && Address.Size == 4 &&
        Consumer->Opcode == NdOp::INT_ZEXT && Consumer->NumInputs == 1 &&
        Consumer->Output.isTemp() && Consumer->Output.Size == 8 &&
        Consumer->Output == *Memory.Address) {
      Address = Consumer->Output;
      Producer = Consumer;
      continue;
    }
    if (!Consumer->Output.isTemp() || Consumer->Output.Size != PointerSize ||
        !((Consumer->Opcode == NdOp::COPY && Consumer->NumInputs == 1) ||
          (Consumer->Opcode == NdOp::INT_ADD && Consumer->NumInputs == 2)))
      return std::nullopt;
    for (unsigned I = 0; I < Consumer->NumInputs; ++I)
      if (Consumer->Inputs[I].Size != PointerSize)
        return std::nullopt;
    Address = Consumer->Output;
    Producer = Consumer;
  }

  // The fresh resolver has no pre-existing marker account. Prepay its bounded
  // failure bookkeeping from the same cumulative replay account.
  constexpr size_t MarkerWork = 16;
  if (!Take(MarkerWork))
    return std::nullopt;
  Proof.IncompleteBranchMarkerEvidenceRemaining = MarkerWork;
  const auto Targets = Proof.resolveJumpTable(Img, Dispatch, &WorkRemaining);
  if (WorkRemaining == 0)
    return std::nullopt;
  const auto Replayed = Proof.ResolvedTableInfo.find(Dispatch.Addr);
  if (Targets != Candidate.Targets || Replayed == Proof.ResolvedTableInfo.end())
    return std::nullopt;
  const JumpTableInfo &Info = Replayed->second;
  if (!Info.IndexDomainAuthenticated || Info.MutatedUnsafe ||
      !Info.HasBaseAddr || Info.IsRelative || Info.TwoTableSelect ||
      Info.TwoLevelIndex || Info.BaseAddr != Candidate.BaseAddr ||
      Info.EntrySize != PointerSize || Info.LoadRoles.size() != 1 ||
      Info.LoadRoles.front().Load.Addr != Load->Addr ||
      Info.LoadRoles.front().Load.Seq != Load->Seq ||
      Info.LoadRoles.front().LoadWidth != Memory.AccessSize)
    return std::nullopt;
  const auto Runtime = implicitJumpTableStorageRange(Info, Targets.size());
  if (!Runtime || !Runtime->storageEnd() || Runtime->EntryStride != PointerSize)
    return std::nullopt;
  // Some loaders inventory a contiguous absolute table one slot at a time.
  // Accept only a complete nonoverlapping partition of this freshly proved
  // runtime envelope, not extra storage from another table or an old witness.
  auto Ranges = Candidate.StorageRanges;
  std::sort(Ranges.begin(), Ranges.end());
  va_t Next = Runtime->BaseAddr;
  for (const auto &Range : Ranges) {
    if (Range.BaseAddr != Next || Range.EntrySize != PointerSize ||
        Range.EntryStride != PointerSize || !Range.storageEnd())
      return std::nullopt;
    Next = *Range.storageEnd();
  }
  if (Next != *Runtime->storageEnd())
    return std::nullopt;
  return Info.ExactPhysicalStorageRange.value_or(*Runtime);
}

std::optional<JumpTableStorageRange>
CFGBuilder::implicitJumpTableStorageRange(const JumpTableInfo &Info,
                                          size_t TargetCount) {
  if (!Info.HasBaseAddr || Info.EntrySize == 0)
    return std::nullopt;
  uint64_t SlotCount = Info.MaxEntries;
  if (SlotCount == 0)
    SlotCount = !Info.EntryIndices.empty()
                    ? static_cast<uint64_t>(Info.EntryIndices.back()) + 1
                    : TargetCount;
  return JumpTableStorageRange{
      Info.BaseAddr, Info.EntrySize,
      Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize, SlotCount};
}

std::optional<bool> CFGBuilder::resolvedJumpTableOwnsStorageAddress(
    va_t Address, const std::set<va_t> *ReachableInsnFilter,
    size_t *EvidenceBudget) const {
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };
  auto consumeEvidence = [&](size_t Amount = 1) {
    if (!EvidenceBudget)
      return true;
    if (Amount > *EvidenceBudget) {
      *EvidenceBudget = 0;
      return false;
    }
    *EvidenceBudget -= Amount;
    return true;
  };
  for (const auto &[BranchAddr, Info] : ResolvedTableInfo) {
    if (!consumeEvidence() || !consumeEvidence(orderedLookupWork(Insns.size())))
      return std::nullopt;
    auto Rec = Insns.find(BranchAddr);
    if (Rec == Insns.end() || Rec->second.JumpTableTargets.empty())
      continue;
    if (ReachableInsnFilter) {
      if (!consumeEvidence(orderedLookupWork(ReachableInsnFilter->size())))
        return std::nullopt;
      if (!ReachableInsnFilter->count(BranchAddr))
        continue;
    } else if (!PublishedReachableInsns.empty()) {
      if (!consumeEvidence(orderedLookupWork(PublishedReachableInsns.size())))
        return std::nullopt;
      if (!PublishedReachableInsns.count(BranchAddr))
        continue;
    }

    if (!Info.StorageRanges.empty()) {
      for (const JumpTableStorageRange &Range : Info.StorageRanges) {
        if (!consumeEvidence())
          return std::nullopt;
        if (Range.ownsStorageAddress(Address))
          return true;
      }
      continue;
    }

    const auto Range = implicitJumpTableStorageRange(
        Info, Rec->second.JumpTableTargets.size());
    if (!Range)
      continue;
    if (!consumeEvidence())
      return std::nullopt;
    if (Range->ownsStorageAddress(Address))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// recoverCaseLabels — inverse-normalize table indices to original case values
//===----------------------------------------------------------------------===//

std::optional<uint64_t> recoverCaseLabelBitPattern(uint64_t EntryIndex,
                                                   uint32_t Stride,
                                                   uint32_t NormShift,
                                                   int64_t NormBase) {
  if (NormShift >= 64)
    return std::nullopt;
  llvm::APInt Label(64, EntryIndex, /*isSigned=*/false,
                    /*implicitTrunc=*/true);
  Label *= llvm::APInt(64, Stride, /*isSigned=*/false,
                       /*implicitTrunc=*/true);
  Label <<= NormShift;
  Label += llvm::APInt(64, static_cast<uint64_t>(NormBase),
                       /*isSigned=*/false, /*implicitTrunc=*/true);
  return Label.getZExtValue();
}

bool CFGBuilder::recoverCaseLabels(JumpTable &JT,
                                   const JumpTableInfo &Info) const {
  JT.CaseLabels.clear();
  if (!Info.RuntimeCaseLabels.empty()) {
    if (Info.RuntimeCaseLabels.size() != JT.Targets.size()) {
      return false;
    }
    std::set<uint32_t> Seen;
    JT.CaseLabels.reserve(Info.RuntimeCaseLabels.size());
    for (uint32_t Label : Info.RuntimeCaseLabels) {
      if (!Seen.insert(Label).second) {
        JT.CaseLabels.clear();
        return false;
      }
      JT.CaseLabels.push_back(static_cast<int64_t>(Label));
    }
    return true;
  }
  // The recovered case labels must be expressed in the SAME coordinate the
  // emitter dispatches on, which depends on the table kind:
  //
  //   * Resolver-register dispatch (AArch64 compact byte/halfword tables and
  //     pre-scaled computed gotos) dispatches on the recovered index register,
  //     the *pre-normalization* switch variable — so the labels invert the
  //     normalization: label = (position * Stride << NormShift) + NormBase.
  //     This mirrors MedLLVMEmitter::emitJumpTableSwitch's own path selection
  //     `(HasTargetBase && EntrySize <= 2) || PreScaledIndex`.
  //
  //   * Every other (regular relative/absolute) table is dispatched via
  //     MedLLVMEmitter::findSwitchIndex, which returns the value feeding the
  //     table-address scale — the *post-normalization* index, after any
  //     `sub base` / mask / shift that computed it.  Its labels are therefore
  //     the raw table positions 0..N-1 (Stride still scales a pre-scaled byte
  //     index).  Folding NormBase/NormShift in here would shift them into the
  //     pre-normalization coordinate the emitter never compares against,
  //     sending every arm to the wrong target — e.g. an ARM32 inline
  //     PC-relative word table for `switch(st)`, st in [1,5], with index
  //     st-1 in [0,4]: the emitter dispatches on st-1 but labels {1..5} would
  //     then match nothing (a masked index already zeroes NormBase in
  //     traceIndexTransform, so only the non-masked `sub` form was affected).
  //
  // EntryIndices (a bounded sparse table's kept slot indices) is always the
  // real table position and so is coordinate-correct for either path.
  bool HasGap = !Info.EntryIndices.empty();
  bool DispatchesPreNormIndex =
      (Info.HasTargetBase && Info.EntrySize <= 2) || Info.PreScaledIndex;
  int64_t NormBase = DispatchesPreNormIndex ? Info.NormBase : 0;
  uint32_t NormShift = DispatchesPreNormIndex ? Info.NormShift : 0;

  if (!HasGap && NormBase == 0 && NormShift == 0 && Info.Stride <= 1)
    return true;

  std::set<uint64_t> Seen;
  JT.CaseLabels.reserve(JT.Targets.size());
  for (size_t I = 0; I < JT.Targets.size(); ++I) {
    uint64_t EntryIndex = (HasGap && I < Info.EntryIndices.size())
                              ? Info.EntryIndices[I]
                              : static_cast<uint64_t>(I);
    auto Label = recoverCaseLabelBitPattern(EntryIndex, Info.Stride, NormShift,
                                            NormBase);
    if (!Label || !Seen.insert(*Label).second) {
      JT.CaseLabels.clear();
      return false;
    }
    llvm::APInt SignedBits(64, *Label, /*isSigned=*/false,
                           /*implicitTrunc=*/true);
    JT.CaseLabels.push_back(SignedBits.getSExtValue());
  }

  LLVM_DEBUG({
    llvm::dbgs() << "  case-labels: recovered " << JT.CaseLabels.size()
                 << " labels (base=" << NormBase << ", shift=" << NormShift
                 << ", stride=" << Info.Stride << ")\n";
  });
  return true;
}

//===----------------------------------------------------------------------===//
// extractJumpTables — collect jump-table metadata from decoded instructions
//===----------------------------------------------------------------------===//

void CFGBuilder::extractJumpTables(LowFunc &Func) {
  std::set<va_t> ReachableInsns;
  for (const LowBlock &Block : Func.Blocks)
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
      ReachableInsns.insert(Boundary.Address);

  auto selectorUseRef = [&](const JumpTableValueOccurrence &Occurrence)
      -> std::optional<JumpTableSelectorUseRef> {
    // A constant has no SSA lifetime to bind. The shared dispatch owner uses
    // the exact authenticated load's coordinate for these constant arms.
    if (Occurrence.Value.isConst())
      return std::nullopt;
    if (Occurrence.Addr == InvalidVA || Occurrence.Seq < 0 ||
        Occurrence.Value.Size == 0 || !ReachableInsns.count(Occurrence.Addr))
      return std::nullopt;
    auto InsnIt = Insns.find(Occurrence.Addr);
    if (InsnIt == Insns.end())
      return std::nullopt;

    const LowOp *UseOp = nullptr;
    std::optional<uint8_t> MatchedInputNo;
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (Op.Addr != Occurrence.Addr || Op.Seq != Occurrence.Seq)
        continue;

      std::optional<uint8_t> CandidateInputNo;
      if (Occurrence.DefinedAtPoint) {
        if (Op.Output != Occurrence.Value)
          continue;
      } else {
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          if (Op.Inputs[I] != Occurrence.Value)
            continue;
          if (CandidateInputNo)
            return std::nullopt;
          CandidateInputNo = I;
        }
        if (!CandidateInputNo)
          continue;
      }
      if (UseOp)
        return std::nullopt;
      UseOp = &Op;
      MatchedInputNo = CandidateInputNo;
    }
    if (!UseOp)
      return std::nullopt;

    JumpTableSelectorUseRef Ref;
    Ref.Addr = UseOp->Addr;
    Ref.Seq = UseOp->Seq;
    Ref.ExpectedOpcode = UseOp->Opcode;
    Ref.ExpectedSize = Occurrence.Value.Size;
    if (Occurrence.DefinedAtPoint) {
      Ref.Role = JumpTableSelectorUseRef::ValueRole::Output;
      return Ref;
    }
    if (!MatchedInputNo)
      return std::nullopt;
    Ref.Role = JumpTableSelectorUseRef::ValueRole::Input;
    Ref.InputNo = *MatchedInputNo;
    return Ref;
  };

  for (auto &[Addr, Rec] : Insns) {
    if (!ReachableInsns.count(Addr))
      continue;
    if (Rec.JumpTableTargets.empty())
      continue;

    JumpTable JT;
    JT.InsnAddr = Addr;
    JT.Targets = Rec.JumpTableTargets;

    if ((Rec.ScalarBranchInput || Rec.ComputedScalarTargets) &&
        Rec.IsIndirect) {
      JT.OriginalAddressSelector = Rec.ScalarBranchInput.has_value();
      // A logical dispatch on the original numeric branch value. There is no
      // physical table, LOAD, slot map or relocation-suppression authority.
      // Bind the exact use after SSA; never infer a selector by register name.
      for (const LowOp &Op : Rec.Ops)
        if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs == 1 &&
            (!Rec.ScalarBranchInput || Op.Inputs[0] == *Rec.ScalarBranchInput))
          JT.SelectorUseRefs.push_back(
              {Op.Addr, Op.Seq, NdOp::INDIR_BR,
               JumpTableSelectorUseRef::ValueRole::Input, 0,
               Op.Inputs[0].Size});
      if (JT.SelectorUseRefs.size() != 1)
        continue;
      for (va_t Target : JT.Targets)
        JT.CaseLabels.push_back(static_cast<int64_t>(Target));
      Func.JumpTables.push_back(std::move(JT));
      continue;
    }

    // Reuse the cached analysis from resolveJumpTable when available,
    // avoiding the duplicate backward-slicing logic.
    auto CachedIt = ResolvedTableInfo.find(Addr);
    if (CachedIt != ResolvedTableInfo.end()) {
      auto &Info = CachedIt->second;
      JT.BaseAddr = Info.BaseAddr;
      JT.HasBaseAddr = Info.HasBaseAddr;
      JT.EntrySize = Info.EntrySize;
      JT.EntryStride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      JT.StorageRanges = Info.StorageRanges;
      JT.ExactPhysicalStorageRange = Info.ExactPhysicalStorageRange;
      JT.SuppressibleRelocationSlots = Info.SuppressibleRelocationSlots;
      for (const auto &Role : Info.LoadRoles)
        JT.AuthenticatedTableLoads.push_back(
            {Role.Load.Addr, Role.Load.Seq, Role.LoadWidth});
      std::sort(JT.AuthenticatedTableLoads.begin(),
                JT.AuthenticatedTableLoads.end());
      JT.AuthenticatedTableLoads.erase(
          std::unique(JT.AuthenticatedTableLoads.begin(),
                      JT.AuthenticatedTableLoads.end()),
          JT.AuthenticatedTableLoads.end());
      JT.HasDispatchSlotMap = !Info.TwoTableSelect && !Info.TwoLevelIndex;
      if (JT.HasDispatchSlotMap) {
        if (!Info.RuntimeSlotIndices.empty())
          JT.SlotIndices = Info.RuntimeSlotIndices;
        else if (!Info.EntryIndices.empty())
          JT.SlotIndices = Info.EntryIndices;
        else {
          JT.SlotIndices.resize(JT.Targets.size());
          std::iota(JT.SlotIndices.begin(), JT.SlotIndices.end(), 0u);
        }
      }
      if (JT.StorageRanges.empty() && Info.HasBaseAddr && Info.EntrySize != 0) {
        uint64_t SlotCount = Info.MaxEntries;
        if (SlotCount == 0)
          SlotCount = !Info.EntryIndices.empty()
                          ? static_cast<uint64_t>(Info.EntryIndices.back()) + 1
                          : JT.Targets.size();
        JT.StorageRanges.push_back(JumpTableStorageRange{
            Info.BaseAddr, Info.EntrySize, JT.EntryStride, SlotCount});
      }
      JT.IsRelative = Info.IsRelative;
      JT.IsSigned = Info.IsSigned;
      JT.TargetBase = Info.TargetBase;
      JT.HasTargetBase = Info.HasTargetBase;
      JT.IsPEImageRelativeRVA = Info.IsPEImageRelativeRVA;
      JT.TableLoadAddr = Info.TableLoadAddr;
      JT.PreScaledIndex = Info.PreScaledIndex;
      JT.TwoTableSelect = Info.TwoTableSelect;
      JT.TwoTableOffset = Info.TwoTableOffset;
      JT.TwoTableHiPositive = Info.TwoTableHiPositive;
      JT.TwoLevelIndex = Info.TwoLevelIndex;
      JT.MutatedUnsafe = Info.MutatedUnsafe;
      if (!Info.UseSharedDispatchSelector && Info.IndexReg != InvalidVA)
        JT.IndexRegOff = static_cast<int>(Info.IndexReg);
      if (Info.TwoTableSelect) {
        const JumpTableLoadRole *CompositeRole = nullptr;
        for (const JumpTableLoadRole &Role : Info.LoadRoles) {
          if (Role.HasBaseSelect == Role.HasBaseMaskBlend ||
              Role.AddressIndex.Value.Size == 0 ||
              Role.SelectCondition.Value.Size == 0)
            continue;
          if (CompositeRole) {
            CompositeRole = nullptr;
            break;
          }
          CompositeRole = &Role;
        }
        if (CompositeRole) {
          auto ByteIndex = selectorUseRef(CompositeRole->AddressIndex);
          auto Condition = selectorUseRef(CompositeRole->SelectCondition);
          if (ByteIndex && Condition && ByteIndex->ExpectedSize != 0) {
            JumpTableCompositeSelectorUseRef Composite;
            Composite.ByteIndex = *ByteIndex;
            Composite.Condition = *Condition;
            Composite.ResultSize = ByteIndex->ExpectedSize;
            Composite.TrueOffset =
                Info.TwoTableHiPositive ? Info.TwoTableOffset : 0;
            Composite.FalseOffset =
                Info.TwoTableHiPositive ? 0 : Info.TwoTableOffset;
            const unsigned ResultBits = Composite.ResultSize * CHAR_BIT;
            const bool OffsetsFit =
                ResultBits >= 64 ||
                ((Composite.TrueOffset >> ResultBits) == 0 &&
                 (Composite.FalseOffset >> ResultBits) == 0);
            if (OffsetsFit)
              JT.CompositeSelectorUseRef = std::move(Composite);
          }
        }
      } else if (!Info.UseSharedDispatchSelector) {
        std::vector<JumpTableValueOccurrence> IndexOccurrences =
            Info.IndexValueAlternatives;
        if (IndexOccurrences.empty())
          IndexOccurrences.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                                      Info.IndexUseSeq,
                                      Info.IndexValueDefinedAtUse});
        bool CompleteSelectorRefs = !IndexOccurrences.empty();
        for (const JumpTableValueOccurrence &IndexOccurrence :
             IndexOccurrences) {
          auto Ref = selectorUseRef(IndexOccurrence);
          if (!Ref) {
            CompleteSelectorRefs = false;
            break;
          }
          if (std::find(JT.SelectorUseRefs.begin(), JT.SelectorUseRefs.end(),
                        *Ref) == JT.SelectorUseRefs.end())
            JT.SelectorUseRefs.push_back(*Ref);
        }
        if (!CompleteSelectorRefs)
          JT.SelectorUseRefs.clear();
      }
    } else {
      // Fallback: quick extraction from NdOp ops.
      bool FoundBase = false;
      bool FoundSize = false;
      bool SawLoad = false;
      uint16_t LoadWidth = 0;
      bool SawSext = false;

      for (auto &Op : Rec.Ops) {
        switch (Op.Opcode) {
        case NdOp::INT_ADD:
          if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            JT.BaseAddr = Op.Inputs[1].Offset;
            JT.HasBaseAddr = true;
            FoundBase = true;
          }
          break;
        case NdOp::INT_MULT:
          if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            JT.EntrySize = static_cast<uint16_t>(Op.Inputs[1].Offset);
            FoundSize = true;
          }
          break;
        case NdOp::INT_LEFT:
          if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            uint64_t Shift = Op.Inputs[1].Offset;
            if (Shift <= limits::kMaxShiftForEntrySize) {
              JT.EntrySize = static_cast<uint16_t>(1u << Shift);
              FoundSize = true;
            }
          }
          break;
        case NdOp::LOAD:
          SawLoad = true;
          LoadWidth = Op.Output.Size;
          break;
        case NdOp::INT_SEXT:
          SawSext = true;
          break;
        default:
          break;
        }
      }

      if (SawLoad && !FoundSize && LoadWidth > 0 &&
          LoadWidth <= limits::kMaxEntryBytes) {
        JT.EntrySize = LoadWidth;
        FoundSize = true;
      }
      if (SawLoad && FoundBase && LoadWidth > 0 &&
          LoadWidth < limits::kMaxEntryBytes)
        JT.IsRelative = true;
      JT.IsSigned = SawSext || (SawLoad && LoadWidth < limits::kMaxEntryBytes);
    }

    auto CachedIt2 = ResolvedTableInfo.find(Addr);
    if (CachedIt2 != ResolvedTableInfo.end()) {
      const JumpTableInfo &Info = CachedIt2->second;
      if ((!Info.RuntimeCaseLabels.empty() ||
           !Info.RuntimeSlotIndices.empty()) &&
          (Info.RuntimeCaseLabels.size() != JT.Targets.size() ||
           Info.RuntimeSlotIndices.size() != JT.Targets.size()))
        continue;
      if (!recoverCaseLabels(JT, CachedIt2->second))
        continue;
    }

    if (JT.EntryStride == 0)
      JT.EntryStride = JT.EntrySize;
    if (JT.SlotIndices.empty() && !JT.Targets.empty() && !JT.TwoTableSelect &&
        !JT.TwoLevelIndex) {
      JT.SlotIndices.resize(JT.Targets.size());
      std::iota(JT.SlotIndices.begin(), JT.SlotIndices.end(), 0u);
      JT.HasDispatchSlotMap = true;
    }
    if (JT.StorageRanges.empty() && JT.HasBaseAddr && !JT.Targets.empty() &&
        JT.EntrySize != 0 && JT.EntryStride != 0) {
      uint64_t SlotCount =
          !JT.SlotIndices.empty()
              ? static_cast<uint64_t>(JT.SlotIndices.back()) + 1
              : JT.Targets.size();
      JT.StorageRanges.push_back(JumpTableStorageRange{
          JT.BaseAddr, JT.EntrySize, JT.EntryStride, SlotCount});
    }

    Func.JumpTables.push_back(JT);
  }
}

} // namespace neverd
