//===- JumpTableResolverInterior.cpp - Interior code-pointer roots -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Ownership, root discovery, and scalar relay resolution for relocation-
/// proven pointers to basic blocks inside a function.  These values are not
/// callable function entries and must retain the owning frame.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

void CFGBuilder::establishCurrentFuncRange(const BinaryImage &Img,
                                           const ExceptionFunction *Exception) {
  CurrentFuncRange.reset();
  AuthoritativeCurrentFuncRange.reset();
  va_t End = InvalidVA;
  va_t AuthoritativeEnd = InvalidVA;
  auto ConsiderEnd = [&](va_t Candidate) {
    if (Candidate > CurrentFuncEntry && (End == InvalidVA || Candidate < End))
      End = Candidate;
  };
  auto ConsiderAuthoritativeEnd = [&](va_t Candidate) {
    if (Candidate <= CurrentFuncEntry)
      return;
    if (AuthoritativeEnd == InvalidVA || Candidate < AuthoritativeEnd)
      AuthoritativeEnd = Candidate;
    ConsiderEnd(Candidate);
  };

  // A containing unwind range can also contain a separately callable local
  // entry.  Only a primary record that starts here proves ownership.
  if (Exception && Exception->Kind == RuntimeFunctionKind::Primary &&
      Exception->CodeRange.Begin == CurrentFuncEntry)
    ConsiderAuthoritativeEnd(Exception->CodeRange.End);

  for (const auto &[Begin, RangeEnd] : Img.KnownCodeRanges)
    if (Begin == CurrentFuncEntry)
      ConsiderAuthoritativeEnd(RangeEnd);

  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Addr == CurrentFuncEntry && Sym.Size != 0 &&
        Sym.Size <= InvalidVA - Sym.Addr)
      ConsiderAuthoritativeEnd(Sym.Addr + Sym.Size);

  // Even without sized metadata, the next independently detected entry is a
  // hard upper boundary.  It does not prove an unbounded last function.
  if (KnownFuncEntries) {
    auto Next = KnownFuncEntries->upper_bound(CurrentFuncEntry);
    if (Next != KnownFuncEntries->end())
      ConsiderEnd(*Next);
  }

  if (End != InvalidVA)
    CurrentFuncRange = std::make_pair(CurrentFuncEntry, End);
  if (AuthoritativeEnd != InvalidVA)
    AuthoritativeCurrentFuncRange =
        std::make_pair(CurrentFuncEntry, AuthoritativeEnd);
}

bool CFGBuilder::isOwnedInteriorTarget(const BinaryImage &Img,
                                       va_t Target) const {
  if (!CurrentFuncRange || Target <= CurrentFuncRange->first ||
      Target >= CurrentFuncRange->second)
    return false;
  if (KnownFuncEntries && KnownFuncEntries->count(Target))
    return false;
  if ((Target % getInsnAlignment()) != 0 ||
      !Img.hasExecutableCodeOwnerAt(Target) ||
      !Img.hasExecutableCodeOwnerRange(Target, getInsnAlignment()) ||
      !Img.readVA(Target, 1))
    return false;
  // Never let an x86 data value that happens to point into an instruction
  // turn its suffix into a second decode stream.
  auto AtOrAfter = Insns.upper_bound(Target);
  if (AtOrAfter != Insns.begin()) {
    const auto &Prev = *std::prev(AtOrAfter);
    if (Prev.first < Target &&
        Target < Prev.first + static_cast<va_t>(Prev.second.Size))
      return false;
  }
  return true;
}

void CFGBuilder::exploreAddressTakenRoots(const BinaryImage &Img,
                                          Decoder &Dec) {
  if (!CurrentFuncRange)
    return;

  std::set<va_t> Processed;
  for (;;) {
    std::set<va_t> Candidates;
    std::set<va_t> RelocationCandidates;
    std::set<va_t> ProtectedRelativeRelocationCandidates;
    std::set<va_t> CrossFunctionContinuationCandidates;
    std::map<va_t, std::set<va_t>> RelocationSources;
    const uint32_t PtrSz = Img.getPointerSize();
    if (PtrSz != 0)
      for (va_t Slot : Img.CodePtrRelocSlots)
        if (const uint8_t *P = Img.readVA(Slot, PtrSz)) {
          const va_t Target = normalizeCodeAddress(
              static_cast<va_t>(readPtr(P, Img.is64Bit())), Img.Arch, Img.Mode);
          RelocationCandidates.insert(Target);
          RelocationSources[Target].insert(Slot);
        }

    // Relative table entries are normally admitted only through their
    // authenticated selector coordinates.  Module arbitration can explicitly
    // protect a different physical slot, however; that slot must remain an
    // independent CFG root even when an exact sparse selector excludes it.
    // Decode only those protected sources through the width/format-qualified
    // cache.  Do not add them to PersistentCFGRoots here: doing so would let an
    // excluded physical slot participate in the selector proof.  Final block
    // formation joins the decoded boundary back to its protected source and
    // preserves it independently.  The ordinary owner and instruction-boundary
    // checks below still decide whether the target belongs to this function.
    if (ProtectedJumpTableRelocationSlots &&
        !ProtectedJumpTableRelocationSlots->empty()) {
      prepareRelativeRelocationRootSourceCache();
      for (const auto &[Target, Slot] : RelativeRelocationRootSourceCache)
        if (ProtectedJumpTableRelocationSlots->count(Slot))
          ProtectedRelativeRelocationCandidates.insert(Target);
    }
    Candidates.insert(RelocationCandidates.begin(), RelocationCandidates.end());
    Candidates.insert(ProtectedRelativeRelocationCandidates.begin(),
                      ProtectedRelativeRelocationCandidates.end());

    // These references were observed by this builder while decoding this
    // function and therefore may identify an owned local label directly.  Do
    // not substitute Img.CodeRefTargets: that image-global union carries no
    // current-function ownership or EH provenance.
    for (va_t Target : DiscoveredCodeRefs)
      Candidates.insert(normalizeCodeAddress(Target, Img.Arch, Img.Mode));

    // A separately emitted funclet can identify an otherwise unreachable
    // continuation in its parent.  The module EH closure supplies only roots
    // proven for this exact owner.  Recheck the authoritative body here; a
    // weaker next-entry bound cannot turn padding or an undetected function
    // into a disconnected local block.
    if (CrossFunctionContinuationRoots && AuthoritativeCurrentFuncRange)
      for (va_t Target : *CrossFunctionContinuationRoots) {
        Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
        if (Target > AuthoritativeCurrentFuncRange->first &&
            Target < AuthoritativeCurrentFuncRange->second) {
          Candidates.insert(Target);
          CrossFunctionContinuationCandidates.insert(Target);
        }
      }

    bool Added = false;
    for (va_t Target : Candidates) {
      if (!Processed.insert(Target).second ||
          !isOwnedInteriorTarget(Img, Target))
        continue;

      BlockStarts.insert(Target);
      if (RelocationCandidates.count(Target)) {
        PersistentCFGRoots.insert(Target);
        OrdinaryCFGRoots.insert(Target);
        const auto Sources = RelocationSources.find(Target);
        if (Sources != RelocationSources.end())
          RelocationCFGRootSources[Target].insert(Sources->second.begin(),
                                                  Sources->second.end());
      }
      if (CrossFunctionContinuationCandidates.count(Target)) {
        // EH ownership and source-value proofs are established by the module
        // closure.  Once the local range and decode-boundary checks above also
        // pass, the continuation remains a disconnected root independently of
        // relocation-table arbitration.
        PersistentCFGRoots.insert(Target);
        OrdinaryCFGRoots.insert(Target);
        DurableCFGRoots.insert(Target);
      }
      if (!ExploredAddrs.count(Target))
        explore(Img, Dec, Target);
      Added = true;
    }
    if (!Added)
      break;
  }
}

bool CFGBuilder::resolveRelocatedInteriorBranch(const BinaryImage &Img,
                                                InsnRecord &Rec) {
  if (!CurrentFuncRange || !Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall ||
      Rec.IsCond || Rec.IsRet || Img.getPointerSize() == 0)
    return false;

  auto BI = BlockStarts.upper_bound(Rec.Addr);
  if (BI == BlockStarts.begin())
    return false;
  --BI;
  const va_t BranchBlockStart = *BI;

  std::set<va_t> Visited{BranchBlockStart};
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);
  std::sort(Preds.begin(), Preds.end());
  Preds.erase(std::unique(Preds.begin(), Preds.end()), Preds.end());
  if (Preds.size() > 1)
    return false;

  std::vector<LowOp> Ops = collectPathOps(BranchBlockStart, Rec.Addr);
  int BranchIdx = -1;
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
    if (Ops[I].Opcode == NdOp::INDIR_BR && Ops[I].NumInputs >= 1) {
      BranchIdx = I;
      break;
    }
  if (BranchIdx < 0)
    return false;

  auto traceLoad = [&](NdVar Value, int From) -> int {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!Value.isReg() && !Value.isTemp())
        return -1;
      int D = reachingDefIdx(Ops, From, Value);
      if (D < 0)
        return -1;
      const LowOp &Def = Ops[D];
      if (Def.Opcode == NdOp::LOAD)
        return D;
      if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
           Def.Opcode == NdOp::INT_SEXT ||
           (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
            Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0)) &&
          Def.NumInputs >= 1) {
        Value = Def.Inputs[0];
        From = D - 1;
        continue;
      }
      return -1;
    }
    return -1;
  };

  const auto &TRI = getTargetRegInfo(Img.Arch);
  int ValueLoad = traceLoad(Ops[BranchIdx].Inputs[0], BranchIdx - 1);
  if (ValueLoad < 0)
    return false;

  auto loadAddress = [&](const LowOp &Load) -> const NdVar * {
    if (Load.Opcode != NdOp::LOAD || Load.NumInputs < 1)
      return nullptr;
    return &Load.Inputs[Load.NumInputs >= 2 ? 1 : 0];
  };

  // A branch usually reloads the target from a frame slot.  Forward exactly
  // the last dominating store; an unknown intervening store or call makes the
  // value ambiguous and therefore ineligible for folding.
  const NdVar *FirstAddr = loadAddress(Ops[ValueLoad]);
  if (!FirstAddr)
    return false;
  uint64_t SlotBase = InvalidVA;
  int64_t SlotOff = 0;
  if (frameSlotKey(Ops, ValueLoad - 1, *FirstAddr, TRI, SlotBase, SlotOff)) {
    int StoreIdx = -1;
    for (int I = ValueLoad - 1; I >= 0; --I) {
      const LowOp &Candidate = Ops[I];
      if (Candidate.Opcode == NdOp::CALL ||
          Candidate.Opcode == NdOp::INDIR_CALL)
        return false;
      if (Candidate.Opcode != NdOp::STORE || Candidate.NumInputs < 2)
        continue;
      uint64_t Base = InvalidVA;
      int64_t Off = 0;
      if (!frameSlotKey(Ops, I - 1, Candidate.Inputs[0], TRI, Base, Off))
        return false; // a potentially aliasing store on the dominating path
      if (Base == SlotBase && Off == SlotOff) {
        StoreIdx = I;
        break;
      }
    }
    if (StoreIdx < 0)
      return false;
    ValueLoad = traceLoad(Ops[StoreIdx].Inputs[1], StoreIdx - 1);
    if (ValueLoad < 0)
      return false;
  }

  const LowOp &SourceLoad = Ops[ValueLoad];
  if (SourceLoad.Output.Size != Img.getPointerSize())
    return false;
  const NdVar *SourceAddr = loadAddress(SourceLoad);
  if (!SourceAddr)
    return false;

  std::function<std::optional<uint64_t>(NdVar, int, int)> foldValue =
      [&](NdVar Value, int From, int Depth) -> std::optional<uint64_t> {
    if (Depth > limits::kMaxQuasiCopyDepth)
      return std::nullopt;
    if (Value.isConst())
      return Value.Offset;
    if (!Value.isReg() && !Value.isTemp())
      return std::nullopt;
    int D = reachingDefIdx(Ops, From, Value);
    if (D < 0)
      return std::nullopt;
    const LowOp &Def = Ops[D];
    if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
         Def.Opcode == NdOp::INT_SEXT ||
         (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
          Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0)) &&
        Def.NumInputs >= 1)
      return foldValue(Def.Inputs[0], D - 1, Depth + 1);
    if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB) &&
        Def.NumInputs >= 2) {
      auto A = foldValue(Def.Inputs[0], D - 1, Depth + 1);
      auto B = foldValue(Def.Inputs[1], D - 1, Depth + 1);
      if (!A || !B)
        return std::nullopt;
      return Def.Opcode == NdOp::INT_ADD ? *A + *B : *A - *B;
    }
    return std::nullopt;
  };

  auto SlotVA = foldValue(*SourceAddr, ValueLoad - 1, 0);
  if (!SlotVA || !Img.CodePtrRelocSlots.count(*SlotVA))
    return false;
  const Segment *SlotSeg = Img.getSegmentFor(*SlotVA);
  if (!SlotSeg ||
      (SlotSeg->isWritable() &&
       !section_names::isReadOnlyAfterRelocSectionName(SlotSeg->Name)))
    return false;

  const uint8_t *P = Img.readVA(*SlotVA, Img.getPointerSize());
  if (!P)
    return false;
  const va_t RawTarget = static_cast<va_t>(readPtr(P, Img.is64Bit()));
  const va_t Target = normalizeCodeAddress(RawTarget, Img.Arch, Img.Mode);
  if (!isOwnedInteriorTarget(Img, Target))
    return false;

  for (LowOp &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      const uint16_t Width =
          Op.Inputs[0].Size ? Op.Inputs[0].Size : Img.getPointerSize();
      Op.Opcode = NdOp::BRANCH;
      Op.Inputs[0] = NdVar::cst(Target, Width);
      Rec.IsIndirect = false;
      Rec.BranchTarget = Target;
      Rec.Immediate = Target;
      if (Img.Arch == Arch::ARM)
        Rec.TargetMode = (RawTarget & 1) ? LowInstructionTargetMode::Thumb
                                         : LowInstructionTargetMode::ARM;
      else
        Rec.TargetMode = LowInstructionTargetMode::Preserve;
      return true;
    }
  return false;
}

} // namespace neverd
