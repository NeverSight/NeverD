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

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
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
  if (!Seg || !Seg->isExecutable() || Next < Seg->VA)
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
  const bool IsTrap = Size > 0 && Dec.isResumableTrap(Peek);
  Dec.setDetail(PreviousDetail);
  return Size > 0 && !IsTrap;
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
  ResolvedTableInfo.clear();
  DecodedInstructionCount = 0;
  LiftedInstructionCount = 0;
  DecodeFailureAddresses.clear();
  UnsupportedInstructionAddresses.clear();
  TruncatedPathAddresses.clear();

  CurrentFuncEntry = EntryAddr;
  CurrentImg = &Img;
  // The x87 TOP counter persists across functions in the shared lifter; start
  // each function with an empty stack so the entry block's lift TOP is 0.
  Dec.resetX86FpuState();
  BlockStarts.insert(EntryAddr);

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

  convertIndirectTailCalls(Func);

  Func.CodeRefTargets.assign(DiscoveredCodeRefs.begin(),
                             DiscoveredCodeRefs.end());
  Func.DecodedInstructionCount = DecodedInstructionCount;
  Func.LiftedInstructionCount = LiftedInstructionCount;
  Func.DecodeFailureAddresses.assign(DecodeFailureAddresses.begin(),
                                     DecodeFailureAddresses.end());
  Func.UnsupportedInstructionAddresses.assign(
      UnsupportedInstructionAddresses.begin(),
      UnsupportedInstructionAddresses.end());
  Func.TruncatedPathAddresses.assign(TruncatedPathAddresses.begin(),
                                     TruncatedPathAddresses.end());

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
      if (!Seg || !Seg->isExecutable()) {
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
      ++DecodedInstructionCount;

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
      try {
        Dec.liftToLow(DI, Rec.Ops);
      } catch (const UnliftedInstruction &Failure) {
        UnsupportedInstructionAddresses.insert(Failure.getAddr());
        break;
      }
      ++LiftedInstructionCount;
      Rec.FpuTopOut = Dec.getX86FpuTop();
      Rec.FpuReset = Dec.x86FpuDidReset();

      // A relocation-free PC-relative `lea` taking the address of executable
      // code is a same-section function pointer the assembler resolved (so the
      // loader saw no relocation).  Record the target so the emitter symbolizes
      // the folded constant to `ptrtoint @func` rather than the stale VA.
      if (va_t Ref = Dec.pcRelCodeRefTarget(DI); Ref != InvalidVA) {
        const auto *RefSeg = Img.getSegmentFor(Ref);
        if (RefSeg && RefSeg->isExecutable())
          DiscoveredCodeRefs.insert(Ref);
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

//===----------------------------------------------------------------------===//
// multiStageResolve — retry unresolved INDIR_BR with more context
//===----------------------------------------------------------------------===//

void CFGBuilder::multiStageResolve(const BinaryImage &Img, Decoder &Dec,
                                   LowFunc &Func) {
  for (int Stage = 0; Stage < limits::kMaxMultiStageRetries; ++Stage) {
    std::vector<va_t> UnresolvedAddrs;
    for (auto &[Addr, Rec] : Insns) {
      if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall)
        continue;
      if (!Rec.JumpTableTargets.empty())
        continue;
      UnresolvedAddrs.push_back(Addr);
    }

    bool MadeProgress = false;
    for (va_t UA : UnresolvedAddrs) {
      auto It = Insns.find(UA);
      if (It == Insns.end())
        continue;

      auto Targets = resolveJumpTable(Img, It->second);
      if (Targets.empty())
        continue;

      It->second.JumpTableTargets = Targets;
      MadeProgress = true;

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

    if (!MadeProgress)
      break;

    splitBlocks();
    rebuildBlocks(Func);

    LLVM_DEBUG(llvm::dbgs()
               << "  multi-stage " << (Stage + 1) << ": rebuilt to "
               << Func.Blocks.size() << " blocks\n");
  }
}

} // namespace neverd
