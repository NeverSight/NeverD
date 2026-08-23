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
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <queue>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

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
  Insns.clear();
  BlockStarts.clear();
  ExploredAddrs.clear();
  CallTargets.clear();
  DiscoveredCodeRefs.clear();
  DiscoveredCodeRefSources.clear();
  ResolvedTableInfo.clear();
  JumpTableProofContextComplete = false;
  RequestedCompleteJumpTableProof = false;
  PersistentCFGRoots.clear();
  DurableCFGRoots.clear();
  RelocationCFGRootSources.clear();
  ActiveJumpTableProofRoots.reset();
  PotentialJumpTableBranches.clear();
  LostValidatedJumpTableBranches.clear();
  PublishedReachableInsns.clear();
  DecodedInstructionCount = 0;
  LiftedInstructionCount = 0;
  DecodedInstructionAddresses.clear();
  LiftedInstructionAddresses.clear();
  DecodeFailureAddresses.clear();
  UnsupportedInstructionAddresses.clear();
  TruncatedPathAddresses.clear();
  RelocatedInstructionAddressOccurrences.clear();
  I386GetPcOccurrences.clear();
  RelocatedInstructionScalarModelOccurrences.clear();

  CurrentFuncEntry = EntryAddr;
  CurrentFuncRange.reset();
  AuthoritativeCurrentFuncRange.reset();
  CurrentImg = &Img;
  // The x87 TOP counter persists across functions in the shared lifter; start
  // each function with an empty stack so the entry block's lift TOP is 0.
  Dec.resetX86FpuState();
  BlockStarts.insert(EntryAddr);
  PersistentCFGRoots.insert(EntryAddr);
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
    if (Exception->SEH)
      for (const SEHScopeRecord &Scope : Exception->SEH->Scopes) {
        AddBoundary(Scope.GuardedRange.Begin);
        if (Scope.GuardedRange.End != Exception->CodeRange.End)
          AddBoundary(Scope.GuardedRange.End);
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
            AddBoundary(Continuation);
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
    if (Published) {
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
              Op.Output == Occurrence.OutputWitness) {
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
    for (va_t Addr : PotentialJumpTableBranches)
      if (Insns.count(Addr))
        Func.UnsafeIndirectBranchAddresses.insert(Addr);
  for (va_t Addr : LostValidatedJumpTableBranches)
    if (Insns.count(Addr))
      Func.UnsafeIndirectBranchAddresses.insert(Addr);

  LLVM_DEBUG(llvm::dbgs() << "CFG built: " << Func.Blocks.size()
                          << " blocks for " << Func.Name << " @ 0x"
                          << llvm::utohexstr(Func.Entry) << "\n");
  return Func;
}

void CFGBuilder::explore(const BinaryImage &Img, Decoder &Dec, va_t Addr) {
  std::queue<va_t> Worklist;
  Worklist.push(Addr);

  while (!Worklist.empty()) {
    va_t Cur = Worklist.front();
    Worklist.pop();

    while (true) {
      if (ExploredAddrs.count(Cur))
        break;
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
              if (!Img.relocatedTargetBelongsToOwner(TargetVA,
                                                     It->second.TargetOwnerVA))
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
      try {
        Dec.liftToLow(DI, Rec.Ops, RelocatedOperands);
      } catch (const UnliftedInstruction &Failure) {
        UnsupportedInstructionAddresses.insert(Failure.getAddr());
        break;
      }
      if (std::optional<I386GetPcOccurrence> GetPc =
              Dec.getX86GetPcOccurrence())
        I386GetPcOccurrences.push_back(*GetPc);
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
          bool Consumed = Matches(Op.Output);
          for (uint8_t I = 0; !Consumed && I < Op.NumInputs; ++I)
            Consumed = Matches(Op.Inputs[I]);
          if (!Consumed)
            continue;
          RelocatedInstructionAddressOccurrences.push_back(
              RelocatedInstructionAddressOccurrence{
                  Reloc.FieldVA, Rec.Addr, Op.Seq, Reloc.TargetVA,
                  Reloc.TargetOwnerVA, Reloc.Width, Reloc.Provenance,
                  Reloc.PCRelativeFromInstructionEnd});
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
        if (Img.hasExecutableCodeOwnerAt(Ref))
          DiscoveredCodeRefs.insert(Ref);
        if (Img.hasExecutableCodeOwnerAt(Ref))
          DiscoveredCodeRefSources[Ref].insert(Cur);
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
            Saved.JumpTableTargets = Targets;
            Insns[Cur].JumpTableTargets = Targets;
            for (va_t T : Targets) {
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
  RelocatedInstructionScalarModelOccurrences.clear();
  if (Img.Arch != Arch::X86 || !Img.isELF() || Img.getPointerSize() != 4 ||
      !JumpTableProofContextComplete || Img.I386GOTPCFields.empty())
    return;

  struct PendingModel {
    va_t FieldVA = InvalidVA;
    const LowOp *Op = nullptr;
    I386GetPcOccurrence Seed;
    size_t QueryIndex = 0;
  };
  std::vector<JumpTableValueQuery> Queries;
  std::vector<PendingModel> Pending;

  for (const auto &[FieldVA, Field] : Img.I386GOTPCFields) {
    const InsnRecord *Containing = nullptr;
    for (const auto &[Addr, Rec] : Insns) {
      if (Rec.Size == 0 || Rec.Size > InvalidVA - Addr || FieldVA < Addr ||
          FieldVA >= Addr + Rec.Size)
        continue;
      if (Containing) {
        Containing = nullptr;
        break;
      }
      Containing = &Rec;
    }
    if (!Containing || Containing->IsInstructionGuard)
      continue;

    const LowOp *Candidate = nullptr;
    uint8_t BaseInput = 0;
    bool Ambiguous = false;
    for (const LowOp &Op : Containing->Ops) {
      if (Op.Opcode != NdOp::INT_ADD || Op.NumInputs != 2 ||
          (!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size != 4)
        continue;
      for (uint8_t ImmediateSide = 0; ImmediateSide < 2; ++ImmediateSide) {
        const NdVar &Immediate = Op.Inputs[ImmediateSide];
        const NdVar &Base = Op.Inputs[1 - ImmediateSide];
        if (!Immediate.isConst() || Immediate.Size != 4 ||
            Immediate.Provenance != ConstantAddressProvenance::Scalar ||
            static_cast<uint32_t>(Immediate.Offset) != Field.EncodedValue ||
            (!Base.isReg() && !Base.isTemp()) || Base.Size != 4)
          continue;
        if (Candidate) {
          Ambiguous = true;
          break;
        }
        Candidate = &Op;
        BaseInput = 1 - ImmediateSide;
      }
      if (Ambiguous)
        break;
    }
    if (Ambiguous || !Candidate)
      continue;

    // The relocation proves only the scalar adjustment.  Bind its other
    // operand to an exact lifter-authenticated call/pop get-PC producer on
    // every feasible incoming path before publishing the model-zero result.
    // A role-neutral Address constant with the same numeric value is not a
    // substitute: it need not move with this relocation at link time.
    std::vector<JumpTableValueOccurrence> PCDefinitions;
    std::vector<I386GetPcOccurrence> Seeds;
    for (const I386GetPcOccurrence &GetPc : I386GetPcOccurrences) {
      if (GetPc.PCValue != Field.ExpectedPCValue ||
          GetPc.OutputOpcode != NdOp::COPY || GetPc.OutputWitness.Size != 4 ||
          (!GetPc.OutputWitness.isReg() && !GetPc.OutputWitness.isTemp()) ||
          !PublishedReachableInsns.count(GetPc.InstructionAddr))
        continue;
      const auto RecIt = Insns.find(GetPc.InstructionAddr);
      if (RecIt == Insns.end() || RecIt->second.IsInstructionGuard)
        continue;
      const LowOp *Producer = nullptr;
      for (const LowOp &Op : RecIt->second.Ops)
        if (Op.Addr == GetPc.InstructionAddr && Op.Seq == GetPc.OpSeq &&
            Op.Opcode == GetPc.OutputOpcode &&
            Op.Output == GetPc.OutputWitness) {
          if (Producer) {
            Producer = nullptr;
            break;
          }
          Producer = &Op;
        }
      if (!Producer || Producer->NumInputs != 1 ||
          !Producer->Inputs[0].isConst() || Producer->Inputs[0].Size != 4 ||
          Producer->Inputs[0].Provenance !=
              ConstantAddressProvenance::Address ||
          Producer->Inputs[0].AddressOwnerVA != InvalidVA ||
          static_cast<uint32_t>(Producer->Inputs[0].Offset) != GetPc.PCValue)
        continue;
      PCDefinitions.push_back({GetPc.OutputWitness, GetPc.InstructionAddr,
                               GetPc.OpSeq, /*DefinedAtPoint=*/true});
      Seeds.push_back(GetPc);
    }
    if (PCDefinitions.empty() || PCDefinitions.size() != Seeds.size())
      continue;

    JumpTableValueQuery Query;
    Query.Candidate = Candidate->Inputs[BaseInput];
    Query.UseAddr = Candidate->Addr;
    Query.UseSeq = Candidate->Seq;
    Query.Alternatives = std::move(PCDefinitions);
    Query.RequireExactAddressOwner = true;
    // All accepted alternatives must be the same exact call/pop producer.
    // Multiple distinct producers can carry the same PC number but do not
    // define one stable cross-layer model witness.
    if (Seeds.size() != 1)
      continue;
    Pending.push_back({FieldVA, Candidate, Seeds.front(), Queries.size()});
    Queries.push_back(std::move(Query));
  }

  if (Queries.empty())
    return;
  bool AnalysisComplete = false;
  const std::vector<bool> Matches =
      tableValuesMatchAtUses(Queries, &AnalysisComplete);
  if (!AnalysisComplete || Matches.size() != Queries.size())
    return;

  for (const PendingModel &Model : Pending) {
    if (!Model.Op || Model.QueryIndex >= Matches.size() ||
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
    Occurrence.SeedInstructionAddr = Model.Seed.InstructionAddr;
    Occurrence.SeedOpSeq = Model.Seed.OpSeq;
    Occurrence.SeedOpcode = Model.Seed.OutputOpcode;
    Occurrence.SeedOutputWitness = Model.Seed.OutputWitness;
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
        Op.Inputs[0].Provenance != ConstantAddressProvenance::AddressFragment ||
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

  if (Queries.empty())
    return;
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
    // arbitration.  An incomplete proof is different — the PAGEOFF output may
    // still carry a reachable page definition, so publish an explicit partial
    // certificate and let module arbitration fail closed.
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
    Occurrence.Provenance = Img.hasExecutableCodeOwnerAt(Materialized.TargetVA)
                                ? ConstantAddressProvenance::CodeAddress
                                : ConstantAddressProvenance::DataAddress;
    Occurrence.DefinesOutput = true;
    Occurrence.OutputMayDepend = !IsExact;
    Occurrence.OutputOpcode = Op.Opcode;
    Occurrence.OutputWitness = Op.Output;
    if (!llvm::is_contained(RelocatedInstructionAddressOccurrences, Occurrence))
      RelocatedInstructionAddressOccurrences.push_back(std::move(Occurrence));
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
  std::set<va_t> EverPublishedJumpTableBranches;
  auto RememberPublishedJumpTables = [&]() {
    for (const auto &[Addr, Rec] : Insns)
      if (Rec.IsBranch && Rec.IsIndirect && !Rec.JumpTableTargets.empty())
        EverPublishedJumpTableBranches.insert(Addr);
  };
  RememberPublishedJumpTables();

  bool ReachedFixedPoint = false;
  for (int Stage = 0; Stage < limits::kMaxMultiStageRetries; ++Stage) {
    std::vector<va_t> CandidateAddrs;
    for (auto &[Addr, Rec] : Insns) {
      if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall)
        continue;
      auto Info = ResolvedTableInfo.find(Addr);
      const bool NeedsRevalidation = Info != ResolvedTableInfo.end() &&
                                     Info->second.RequiresCompleteCFGProof;
      if (Rec.JumpTableTargets.empty() || NeedsRevalidation)
        CandidateAddrs.push_back(Addr);
    }

    bool MadeProgress = false;
    bool RefreshedProofMetadata = false;
    for (va_t UA : CandidateAddrs) {
      auto It = Insns.find(UA);
      if (It == Insns.end())
        continue;

      auto PriorInfo = ResolvedTableInfo.find(UA);
      const bool WasProofDependent = PriorInfo != ResolvedTableInfo.end() &&
                                     PriorInfo->second.RequiresCompleteCFGProof;
      const std::optional<JumpTableInfo> OldInfo =
          PriorInfo == ResolvedTableInfo.end()
              ? std::nullopt
              : std::optional<JumpTableInfo>(PriorInfo->second);

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

      const std::vector<va_t> OldTargets = It->second.JumpTableTargets;
      JumpTableProofContextComplete = true;
      auto Targets = resolveJumpTable(Img, It->second);
      JumpTableProofContextComplete = false;
      auto NewInfo = ResolvedTableInfo.find(UA);
      const bool MetadataChanged =
          OldInfo.has_value() != (NewInfo != ResolvedTableInfo.end()) ||
          (OldInfo && NewInfo != ResolvedTableInfo.end() &&
           *OldInfo != NewInfo->second);
      RefreshedProofMetadata |= WasProofDependent && MetadataChanged;
      if (Targets != OldTargets) {
        It->second.JumpTableTargets = Targets;
        MadeProgress = true;
      }
      if (!Targets.empty())
        EverPublishedJumpTableBranches.insert(UA);

      for (va_t T : Targets) {
        if (!ExploredAddrs.count(T)) {
          BlockStarts.insert(T);
          explore(Img, Dec, T);
        }
      }
    }

    // Align branches that share a jump table so a peeled copy in a messy block
    // inherits the loop body's complete recovery.  This can make progress even
    // when nothing was newly resolved this stage, so check it before bailing.
    if (reconcileSharedTables(Img, Dec))
      MadeProgress = true;
    RememberPublishedJumpTables();

    if (!MadeProgress) {
      // Targets can remain byte-for-byte equal while the complete CFG changes
      // case labels, range/identity metadata, or mutation state.  Publish the
      // freshly cached JumpTableInfo, then require another complete resolver
      // round on that published CFG.  Only an equal target set *and* equal
      // proof metadata in the following round is a fixed point; a metadata
      // cycle consumes the bounded retry budget and is discarded below.
      if (RefreshedProofMetadata) {
        rebuildBlocks(Func);
        continue;
      }
      ReachedFixedPoint = true;
      break;
    }

    completeExactAArch64PageBases(Img);
    splitBlocks();
    rebuildBlocks(Func);

    LLVM_DEBUG(llvm::dbgs()
               << "  multi-stage " << (Stage + 1) << ": rebuilt to "
               << Func.Blocks.size() << " blocks\n");
  }

  // A proof-dependent target set may only escape after one whole round saw no
  // new decoded targets and no target-set change.  If the bounded iteration
  // budget is exhausted first, discard those provisional edges rather than
  // publishing a CFG whose validity depends on traversal order.
  if (!ReachedFixedPoint) {
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
  }

  // A transient empty target set can recover in a later stage, so classify a
  // lost validated table only after convergence (or the bounded cleanup above)
  // has established the final target state.  These addresses are not generic
  // detector claims: every member published at least one concrete table edge
  // earlier in this very build.
  for (va_t Addr : EverPublishedJumpTableBranches) {
    auto It = Insns.find(Addr);
    if (It != Insns.end() && It->second.IsBranch && It->second.IsIndirect &&
        !It->second.IsCall && It->second.JumpTableTargets.empty())
      LostValidatedJumpTableBranches.insert(Addr);
  }
}

} // namespace neverd
