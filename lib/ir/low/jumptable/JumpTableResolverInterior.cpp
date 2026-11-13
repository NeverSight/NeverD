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

#include "../X86/RegistrationStackOwner.h"
#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/CodePointerSources.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

namespace {

template <typename Visitor>
void visitAbsoluteRelocationSources(const BinaryImage &Img, Visitor Visit) {
  for (va_t Slot : Img.CodePtrRelocSlots)
    if (const auto Target = normalizedRelocatedCodeTarget(Img, Slot))
      Visit(*Target, Slot);
}

} // namespace

struct detail::AbsoluteRelocationRootIndex::RuntimeRoleCache {
  std::once_flag Once;
  std::map<va_t, va_t> Sources;
};

detail::AbsoluteRelocationRootIndex::AbsoluteRelocationRootIndex(
    const BinaryImage &Image)
    : Image(&Image), RuntimeRoles(std::make_shared<RuntimeRoleCache>()) {
  Sources.reserve(Image.CodePtrRelocSlots.size());
  visitAbsoluteRelocationSources(Image, [&](va_t Target, va_t Slot) {
    Sources.emplace_back(Target, Slot);
  });
  std::sort(Sources.begin(), Sources.end());
}

bool detail::AbsoluteRelocationRootIndex::collectSources(
    const BinaryImage &Img, va_t Begin, va_t End,
    std::map<va_t, std::set<va_t>> &RootSources) const {
  return collectSources(Img, Begin, End, RootSources, false);
}

bool detail::AbsoluteRelocationRootIndex::collectOrdinarySources(
    const BinaryImage &Img, va_t Begin, va_t End,
    std::map<va_t, std::set<va_t>> &RootSources) const {
  return collectSources(Img, Begin, End, RootSources, true);
}

bool detail::AbsoluteRelocationRootIndex::collectSources(
    const BinaryImage &Img, va_t Begin, va_t End,
    std::map<va_t, std::set<va_t>> &RootSources, bool OrdinaryOnly) const {
  if (Image != &Img)
    return false;
  if (Begin >= End)
    return true;
  const auto First =
      std::upper_bound(Sources.begin(), Sources.end(), Begin,
                       [](va_t Address, const std::pair<va_t, va_t> &Source) {
                         return Address < Source.first;
                       });
  const auto Last =
      std::lower_bound(First, Sources.end(), End,
                       [](const std::pair<va_t, va_t> &Source, va_t Address) {
                         return Source.first < Address;
                       });
  if (First == Last)
    return true;
  // Raw ownership arbitration needs no runtime-role parsing. Publish this
  // immutable image-only result once, including a bounded refusal, before
  // concurrent ordinary-source queries inspect it.
  if (OrdinaryOnly)
    std::call_once(RuntimeRoles->Once, [&] {
      if (auto Roles = coff_loader::getCheckedX86RegistrationPointerRoles(Img))
        RuntimeRoles->Sources = std::move(Roles->Sources);
    });
  for (auto It = First; It != Last; ++It) {
    if (OrdinaryOnly) {
      const auto Runtime = RuntimeRoles->Sources.find(It->second);
      if (Runtime != RuntimeRoles->Sources.end() &&
          Runtime->second == It->first)
        continue;
    }
    RootSources[It->first].insert(It->second);
  }
  return true;
}

const detail::AbsoluteRelocationRootIndex &
CFGBuilder::absoluteRelocationRootIndex(const BinaryImage &Img) {
  if (AbsoluteRelocationRoots && AbsoluteRelocationRoots->ownsImage(Img))
    return *AbsoluteRelocationRoots;
  if (const auto *Roots =
          registration_abi::StackProofOwnerScope::relocationRoots(Img))
    return *Roots;
  if (BorrowedRegistrationCallees &&
      BorrowedRegistrationCallees->ownsImage(Img))
    return BorrowedRegistrationCallees->absoluteRelocationRoots();
  if (!LocalAbsoluteRelocationRoots)
    LocalAbsoluteRelocationRoots.emplace(Img);
  return *LocalAbsoluteRelocationRoots;
}

void CFGBuilder::establishCurrentFuncRange(const BinaryImage &Img,
                                           const ExceptionFunction *Exception) {
  CurrentFuncRange.reset();
  AuthoritativeCurrentFuncRange.reset();
  // Win64 requires an unwind record for every function that allocates stack
  // or saves a register; one without operations describes neither.  An
  // image with no exception directory at all proves nothing either way.
  const bool HasExceptionDirectory =
      !Img.ExceptionMetadata.Functions.empty() || !Img.COFFPDataRecords.empty();
  CurrentFuncIsFramelessLeaf =
      Img.Format == BinaryFormat::COFF && Img.Arch == Arch::X64 &&
      HasExceptionDirectory &&
      (!Exception ||
       (Exception->Kind == RuntimeFunctionKind::Primary &&
        Exception->CodeRange.Begin == CurrentFuncEntry &&
        Exception->ParseStatus == ExceptionParseStatus::Complete &&
        Exception->UnwindOperations.empty() && Exception->FrameRegister == 0));
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

  ConsiderAuthoritativeEnd(
      Img.getFunctionMetadataEnd(CurrentFuncEntry, ExecutableCodeOwners));

  // Even without sized metadata, the next independently detected entry is a
  // hard upper boundary.  It does not prove an unbounded last function.
  if (const va_t Next = nextKnownFunctionEntry(CurrentFuncEntry);
      Next != InvalidVA)
    ConsiderEnd(Next);

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
  if (isKnownFunctionEntry(Target) && !isCurrentExceptionalEntry(Target) &&
      !isCurrentOwnedFragment(Target))
    return false;
  if ((Target % getInsnAlignment()) != 0 ||
      !Img.hasExecutableCodeOwnerAt(Target, ExecutableCodeOwners) ||
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

  // Runtime metadata can name callbacks outside its installing function.
  // Those exact fields cannot seed an unrelated function whose conservative
  // decode envelope happens to contain the callback. The shared loader role
  // proof excludes only checked sources, preserving every ordinary alias.
  // Function ownership remains local and is checked after each graph extension.
  std::set<va_t> RelocationCandidates;
  std::map<va_t, std::set<va_t>> RelocationSources;
  absoluteRelocationRootIndex(Img).collectOrdinarySources(
      Img, CurrentFuncRange->first, CurrentFuncRange->second,
      RelocationSources);
  const ExceptionFunction *Exception =
      Img.ExceptionMetadata.findFunction(CurrentFuncEntry);
  for (auto &[Target, Sources] : RelocationSources) {
    // A pointer table among the code is read by the function dispatching
    // through it, whose jump-table resolver recovers what it names; it roots
    // no function's code.  The range of a function before the dispatcher
    // reaches the dispatcher's cases when its entry is only a guess.
    std::erase_if(Sources, [&](va_t Slot) {
      return Img.hasExecutableCodeOwnerAt(Slot, ExecutableCodeOwners);
    });
    if (Exception && Exception->Registration &&
        (Exception->Encoding == ExceptionEncoding::X86ScopeTableEH3 ||
         Exception->Encoding == ExceptionEncoding::X86ScopeTableEH4))
      std::erase_if(Sources, [&](va_t Slot) {
        return Exception->Registration->scopePointerTarget(Slot) == Target;
      });
    if (!Sources.empty())
      RelocationCandidates.insert(Target);
  }

  std::set<va_t> Processed;
  for (;;) {
    std::set<va_t> Candidates;
    std::set<va_t> ProtectedRelativeRelocationCandidates;
    std::set<va_t> CrossFunctionContinuationCandidates;

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

void CFGBuilder::dispatchUnprovedBranchesByValue(const BinaryImage &Img,
                                                 LowFunc &Func) {
  if (!CurrentFuncRange)
    return;
  // The labels a jump of this function can name: the blocks whose addresses
  // relocation-proven pointers outside the code hold.  They are roots of this
  // function already, entered with its frame.
  std::vector<va_t> Labels;
  for (const auto &[Target, Sources] : RelocationCFGRootSources)
    if (!Sources.empty() && OrdinaryCFGRoots.count(Target) &&
        Insns.count(Target) && isOwnedInteriorTarget(Img, Target))
      Labels.push_back(Target);
  if (Labels.empty() ||
      Labels.size() > static_cast<size_t>(limits::kMaxJumpTableEntries))
    return;
  std::vector<va_t> Jumps;
  for (const auto &[Addr, Rec] : Insns)
    if (Rec.IsBranch && Rec.IsIndirect && !Rec.IsCall && !Rec.IsRet &&
        !Rec.IsCond && Rec.JumpTableTargets.empty() && !Rec.ScalarBranchInput)
      Jumps.push_back(Addr);
  if (Jumps.empty())
    return;

  // Every label is a candidate of every jump, which says more than any one
  // jump can do: a table has entries its index never selects, and another
  // jump's table is not this one's.  An edge the program cannot take must not
  // make the function read what it never wrote, or a register its caller
  // keeps would become one of its inputs.  So an edge is kept only where
  // every such register the label's code reads first has been written on
  // every path to the jump.  A jump to a label left out traps, as every jump
  // of this kind did before.
  const auto &TRI = getTargetRegInfo(Img.Arch);
  const llvm::ArrayRef<uint64_t> Kept = TRI.CalleeSaveRegs;
  if (Kept.size() > 64)
    return;
  const uint16_t Pointer = TRI.PointerSize;
  // A write of four bytes or more to a 64-bit register sets all of it.
  const uint16_t WholeWrite = Pointer == 8 ? 4 : Pointer;
  auto Bits = [&](const NdVar &Value, bool Whole) {
    uint64_t Mask = 0;
    if (!Value.isReg() || Value.Size == 0)
      return Mask;
    for (size_t I = 0; I < Kept.size(); ++I) {
      const uint64_t Base = Kept[I];
      if (Whole ? Value.Offset == Base && Value.Size >= WholeWrite
                : Value.Offset < Base + Pointer &&
                      Base < Value.Offset + Value.Size)
        Mask |= uint64_t{1} << I;
    }
    return Mask;
  };
  const size_t Count = Func.Blocks.size();
  std::map<va_t, size_t> BlockAt;
  std::vector<uint64_t> Reads(Count, 0), Writes(Count, 0);
  for (size_t B = 0; B < Count; ++B) {
    const LowBlock &Block = Func.Blocks[B];
    BlockAt.emplace(Block.StartAddr, B);
    for (const LowOp &Op : Block.Ops) {
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Reads[B] |= Bits(Op.Inputs[I], false) & ~Writes[B];
      Writes[B] |= Bits(Op.Output, true);
    }
  }
  std::vector<size_t> LabelBlocks;
  for (va_t Label : Labels) {
    const auto At = BlockAt.find(Label);
    if (At == BlockAt.end())
      return;
    LabelBlocks.push_back(At->second);
  }
  std::vector<size_t> JumpBlocks;
  for (va_t Jump : Jumps) {
    size_t Found = Count;
    for (size_t B = 0; B < Count && Found == Count; ++B)
      if (!Func.Blocks[B].Ops.empty() && Func.Blocks[B].Ops.back().Addr == Jump)
        Found = B;
    if (Found == Count)
      return;
    JumpBlocks.push_back(Found);
  }
  size_t Entry = Count;
  if (const auto At = BlockAt.find(Func.Entry); At != BlockAt.end())
    Entry = At->second;
  if (Entry == Count)
    return;

  // A jump whose table is known can take only what that table holds: the
  // label slots from where it starts to the next address another field of
  // the code names, which starts another table.  The table is the one an
  // exact field names, or the one a frame copy the jump reads was made from.
  std::vector<std::vector<bool>> Allowed(
      Jumps.size(), std::vector<bool>(Labels.size(), true));
  std::optional<std::set<va_t>> Anchors;
  for (size_t J = 0; J < Jumps.size(); ++J) {
    std::optional<va_t> Table =
        jumpReadsOwnLabelsThroughI386GOTOFF(Insns.at(Jumps[J]));
    if (!Table)
      if (const auto Copied = FrameLabelTableSources.find(Jumps[J]);
          Copied != FrameLabelTableSources.end())
        Table = Copied->second;
    if (!Table)
      continue;
    if (!Anchors) {
      Anchors.emplace();
      for (const auto &[Field, Operand] : Img.DataAddressRelocOperands)
        Anchors->insert(Operand.TargetVA);
    }
    std::set<va_t> Held;
    for (va_t Slot = *Table;
         Held.size() <= static_cast<size_t>(limits::kMaxJumpTableEntries) &&
         Img.CodePtrRelocSlots.count(Slot) &&
         (Slot == *Table || !Anchors->count(Slot)) &&
         Slot <= InvalidVA - Pointer;
         Slot += Pointer) {
      const auto Target = normalizedRelocatedCodeTarget(Img, Slot);
      if (!Target)
        break;
      Held.insert(*Target);
    }
    for (size_t L = 0; L < Labels.size(); ++L)
      Allowed[J][L] = Held.count(Labels[L]) != 0;
  }

  // Edges[J][L]: jump J reaches label L.
  std::vector<std::vector<bool>> Edges(Jumps.size(),
                                       std::vector<bool>(Labels.size(), false));
  std::vector<bool> Reached, Decided(Jumps.size(), false);
  std::vector<uint64_t> Written, Needed;
  auto Successors = [&](size_t B, auto Visit) {
    for (int S : Func.Blocks[B].Succs)
      if (S >= 0 && static_cast<size_t>(S) < Count)
        Visit(static_cast<size_t>(S));
    for (size_t J = 0; J < Jumps.size(); ++J)
      if (JumpBlocks[J] == B)
        for (size_t L = 0; L < Labels.size(); ++L)
          if (Edges[J][L])
            Visit(LabelBlocks[L]);
  };
  // Written[B]: the registers every path from the entry has written when B
  // ends.  Needed[B]: those some path from B's start reads before a write.
  auto Solve = [&] {
    Reached.assign(Count, false);
    Written.assign(Count, ~uint64_t{0});
    std::vector<uint64_t> AtStart(Count, ~uint64_t{0});
    std::vector<size_t> Work{Entry};
    Reached[Entry] = true;
    AtStart[Entry] = 0;
    Written[Entry] = Writes[Entry];
    while (!Work.empty()) {
      const size_t B = Work.back();
      Work.pop_back();
      Successors(B, [&](size_t S) {
        const uint64_t Meet = AtStart[S] & Written[B];
        if (Reached[S] && Meet == AtStart[S])
          return;
        Reached[S] = true;
        AtStart[S] = Meet;
        Written[S] = Meet | Writes[S];
        Work.push_back(S);
      });
    }
    Needed.assign(Count, 0);
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (size_t B = Count; B-- > 0;) {
        uint64_t After = 0;
        Successors(B, [&](size_t S) { After |= Needed[S]; });
        const uint64_t Now = Reads[B] | (After & ~Writes[B]);
        if (Now != Needed[B]) {
          Needed[B] = Now;
          Changed = true;
        }
      }
    }
  };
  // Decide the jumps in the order control reaches them, each with the edges
  // decided before it.
  for (bool Progress = true; Progress;) {
    Progress = false;
    Solve();
    for (size_t J = 0; J < Jumps.size(); ++J) {
      if (Decided[J] || !Reached[JumpBlocks[J]])
        continue;
      for (size_t L = 0; L < Labels.size(); ++L)
        Edges[J][L] = Allowed[J][L] &&
                      (Needed[LabelBlocks[L]] & ~Written[JumpBlocks[J]]) == 0;
      Decided[J] = true;
      Progress = true;
      break;
    }
  }
  // An edge decided early can fail once later ones exist: drop it.  Each
  // removal only makes the remaining ones easier to keep.
  for (bool Removed = true; Removed;) {
    Removed = false;
    Solve();
    for (size_t J = 0; J < Jumps.size(); ++J)
      for (size_t L = 0; L < Labels.size(); ++L)
        if (Edges[J][L] &&
            (!Reached[JumpBlocks[J]] ||
             (Needed[LabelBlocks[L]] & ~Written[JumpBlocks[J]]) != 0)) {
          Edges[J][L] = false;
          Removed = true;
        }
  }

  bool Changed = false;
  for (size_t J = 0; J < Jumps.size(); ++J) {
    std::vector<va_t> Targets;
    for (size_t L = 0; L < Labels.size(); ++L)
      if (Edges[J][L])
        Targets.push_back(Labels[L]);
    if (Targets.empty())
      continue;
    InsnRecord &Rec = Insns.at(Jumps[J]);
    Rec.JumpTableTargets = std::move(Targets);
    Rec.ValueDispatch = true;
    ResolvedTableInfo.erase(Jumps[J]);
    Changed = true;
  }
  if (Changed)
    rebuildBlocks(Func);
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
  if (!Img.readOnlyAfterRelocations(*SlotVA, Img.getPointerSize()))
    return false;

  const uint8_t *P = Img.readVA(*SlotVA, Img.getPointerSize());
  if (!P)
    return false;
  const va_t RawTarget =
      static_cast<va_t>(readPtr(P, Img.is64Bit(), archLittleEndian(Img.Arch)));
  va_t Target = normalizeCodeAddress(RawTarget, Img.Arch, Img.Mode);
  std::optional<InstructionMode> MIPSMode;
  if (hasCompressedMIPSCode(Img.Mode)) {
    auto Destination = canonicalizeLowControlTarget(
        RawTarget, Rec.Mode, Rec.TargetMode, Img.getPointerSize());
    if (!Destination) {
      llvm::consumeError(Destination.takeError());
      return false;
    }
    if (Img.instructionModeAt(Destination->Address, Destination->Mode) !=
        Destination->Mode)
      return false;
    Target = Destination->Address;
    MIPSMode = Destination->Mode;
  }
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
      else if (MIPSMode)
        Rec.TargetMode = *MIPSMode == InstructionMode::MIPS16
                             ? LowInstructionTargetMode::MIPS16
                         : *MIPSMode == InstructionMode::MicroMIPS
                             ? LowInstructionTargetMode::MicroMIPS
                             : LowInstructionTargetMode::MIPSClassic;
      else
        Rec.TargetMode = LowInstructionTargetMode::Preserve;
      return true;
    }
  return false;
}

} // namespace neverd
