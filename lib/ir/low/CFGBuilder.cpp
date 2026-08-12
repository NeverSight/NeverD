//===- CFGBuilder.cpp - Control-flow graph construction ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements recursive-descent disassembly for single-function CFG
/// construction: exploration, block splitting, instruction classification,
/// and successor linking.  Function entry-point detection lives in
/// FuncDetector.cpp.  Jump-table resolution and metadata extraction live
/// in JumpTableResolver.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CFGBuilder.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/libc/LibCNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <queue>

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
    if (Exception->Registration)
      for (const RegistrationScopeRecord &Scope :
           Exception->Registration->Scopes) {
        AddExceptionalRoot(Scope.FilterVA);
        AddExceptionalRoot(Scope.HandlerVA);
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

      InsnRecord Rec;
      Rec.Addr = Cur;
      Rec.Size = static_cast<uint16_t>(Sz);
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
      if (!Rec.IsBranch && !Rec.IsRet && Dec.isFunctionTerminator(DI))
        Rec.IsRet = !Dec.isResumableTrap(DI) ||
                    !codeFollowsTrap(Img, Dec, Cur + static_cast<va_t>(Sz));

      // operator[]= returns the stored element, so bind to it directly rather
      // than doing a second tree lookup for the same key on the decode hot
      // path.
      auto &Saved = (Insns[Cur] = std::move(Rec));

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
          va_t Fall = Cur + Sz;
          BlockStarts.insert(Fall);
          if (!ExploredAddrs.count(Fall))
            Worklist.push(Fall);
        }
        break;
      }

      // A direct call to a no-return libc function (longjmp / abort / exit /
      // ...) is a control-flow terminator.  At -O2 the compiler emits nothing
      // after such a call, so the bytes that follow belong to the NEXT
      // function; continuing the fall-through would absorb them into this CFG —
      // a leaf `bl _longjmp` would otherwise swallow the entire caller laid out
      // after it (the cause of the setjmp/longjmp patch SIGSEGV).  Stop
      // exploring and suppress the fall-through successor edge in
      // linkSuccessors.
      if (Saved.IsCall && !Saved.IsIndirect && isNoReturnCall(Saved)) {
        Saved.IsNoReturnCall = true;
        break;
      }

      Cur += Sz;
    }
  }
}

void CFGBuilder::splitBlocks() {
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.IsBranch && Rec.IsCond)
      BlockStarts.insert(Addr + Rec.Size);
  }
}

void CFGBuilder::normalizeEntryBlock(LowFunc &Func) {
  if (Func.Blocks.empty())
    return;

  auto EntryIt = std::find_if(
      Func.Blocks.begin(), Func.Blocks.end(),
      [&](const LowBlock &Blk) { return Blk.StartAddr == Func.Entry; });
  if (EntryIt == Func.Blocks.end())
    llvm::report_fatal_error("CFGBuilder: function entry block is missing");

  const size_t EntryIndex =
      static_cast<size_t>(std::distance(Func.Blocks.begin(), EntryIt));
  const size_t NumBlocks = Func.Blocks.size();
  std::vector<size_t> NewToOld;
  NewToOld.reserve(NumBlocks);
  NewToOld.push_back(EntryIndex);
  for (size_t I = 0; I < NumBlocks; ++I)
    if (I != EntryIndex)
      NewToOld.push_back(I);

  std::vector<int> OldToNew(NumBlocks, -1);
  for (size_t NewId = 0; NewId < NumBlocks; ++NewId) {
    const int OldId = Func.Blocks[NewToOld[NewId]].Id;
    if (OldId < 0 || static_cast<size_t>(OldId) >= NumBlocks ||
        OldToNew[OldId] != -1)
      llvm::report_fatal_error("CFGBuilder: invalid block identity");
    OldToNew[OldId] = static_cast<int>(NewId);
  }

  std::vector<LowBlock> Normalized;
  Normalized.reserve(NumBlocks);
  for (size_t NewId = 0; NewId < NumBlocks; ++NewId) {
    LowBlock Blk = std::move(Func.Blocks[NewToOld[NewId]]);
    auto RemapEdges = [&](std::vector<int> &Edges) {
      for (int &Id : Edges) {
        if (Id < 0 || static_cast<size_t>(Id) >= NumBlocks || OldToNew[Id] < 0)
          llvm::report_fatal_error("CFGBuilder: invalid block edge");
        Id = OldToNew[Id];
      }
    };
    RemapEdges(Blk.Succs);
    RemapEdges(Blk.Preds);
    Blk.Id = static_cast<int>(NewId);
    Normalized.push_back(std::move(Blk));
  }
  Func.Blocks = std::move(Normalized);
}

void CFGBuilder::rebuildBlocks(LowFunc &Func) {
  Func.Blocks.clear();
  Func.JumpTables.clear();

  // A boundary can name an address recursive descent never decoded: the end of
  // a guarded range an exception table declared, or the fall-through of a
  // conditional branch at the edge of the mapped image.  A block there would
  // carry no instruction and so would stand for nothing, so it is dropped.  The
  // entry survives unconditionally — the function is defined by it.
  std::vector<va_t> Starts;
  Starts.reserve(BlockStarts.size());
  for (va_t Start : BlockStarts)
    if (Start == Func.Entry || Insns.count(Start))
      Starts.push_back(Start);
  std::sort(Starts.begin(), Starts.end());

  std::map<va_t, int> AddrToBlock;
  for (size_t I = 0; I < Starts.size(); ++I) {
    LowBlock Blk;
    Blk.Id = static_cast<int>(I);
    Blk.StartAddr = Starts[I];

    va_t End = (I + 1 < Starts.size()) ? Starts[I + 1] : InvalidVA;

    for (auto It = Insns.lower_bound(Starts[I]); It != Insns.end(); ++It) {
      if (It->first >= End)
        break;
      for (auto &Op : It->second.Ops)
        Blk.Ops.push_back(Op);
      Blk.EndAddr = It->first + It->second.Size;
    }

    AddrToBlock[Starts[I]] = Blk.Id;
    Func.Blocks.push_back(std::move(Blk));
  }

  linkSuccessors(Func, AddrToBlock);
  normalizeEntryBlock(Func);
  linkExceptionalSuccessors(Func);
  extractJumpTables(Func);
  fixupFpuStack(Func);
}

//===----------------------------------------------------------------------===//
// fixupFpuStack — the x86/x86-64 x87 stack-pointer (TOP) fixup — is
// architecture-gated, so it is defined in CFGBuilderX86Fpu.cpp following the
// target-dispatch split used by the jump-table detectors.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// convertIndirectTailCalls — model `bx reg`/`br reg`/`jmp *reg` (function
// pointer, not a jump table) as an indirect call + return.
//===----------------------------------------------------------------------===//

void CFGBuilder::convertIndirectTailCalls(LowFunc &Func) {
  bool Changed = false;
  for (auto &[Addr, Rec] : Insns) {
    // Only an unconditional indirect branch with no resolved jump-table targets
    // is a function-pointer tail call.  A resolved INDIR_BR (switch / computed
    // goto) keeps its successors; a conditional one and a return are
    // unaffected.
    if (!Rec.IsBranch || !Rec.IsIndirect || Rec.IsCall || Rec.IsRet ||
        Rec.IsCond || !Rec.JumpTableTargets.empty())
      continue;
    rewriteAsIndirectTailCall(Rec);
    Changed = true;
  }
  if (Changed)
    rebuildBlocks(Func);
}

//===----------------------------------------------------------------------===//
// classifyInsn — set control-flow flags from decoded ops
//===----------------------------------------------------------------------===//

void CFGBuilder::classifyInsn(InsnRecord &Rec) {
  for (auto &Op : Rec.Ops) {
    switch (Op.Opcode) {
    case NdOp::BRANCH:
      Rec.IsBranch = true;
      if (Op.Inputs[0].isConst())
        Rec.BranchTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::COND_BR:
      Rec.IsBranch = true;
      Rec.IsCond = true;
      if (Op.Inputs[0].isConst())
        Rec.BranchTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::INDIR_BR:
      Rec.IsBranch = true;
      Rec.IsIndirect = true;
      break;
    case NdOp::CALL:
      Rec.IsCall = true;
      if (Op.Inputs[0].isConst())
        CallTargets.insert(Op.Inputs[0].Offset);
      break;
    case NdOp::INDIR_CALL:
      Rec.IsCall = true;
      Rec.IsIndirect = true;
      break;
    case NdOp::RETURN:
      Rec.IsRet = true;
      break;
    default:
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// isTailCallTarget / rewriteAsTailCall — model `jmp other_func` as call + ret
//===----------------------------------------------------------------------===//

bool CFGBuilder::isTailCallTarget(va_t Target) const {
  if (Target == InvalidVA || Target == CurrentFuncEntry)
    return false;
  if (KnownFuncEntries && KnownFuncEntries->count(Target) > 0)
    return true;
  // A direct branch landing on a registered import veneer is a tail call to
  // that external function.  Without this the veneer is followed and inlined
  // into the caller as an untyped indirect call, dropping import identity and
  // ABI.  BinaryImage resolves both the format-native IAT address and exact
  // executable stubs, so this also covers COFF thunks whose IATAddr stays a
  // data slot.
  if (CurrentImg && CurrentImg->findImportAt(Target))
    return true;
  return false;
}

bool CFGBuilder::isNoReturnCall(const InsnRecord &Rec) const {
  if (!CurrentImg)
    return false;
  va_t Target = InvalidVA;
  for (const auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
        Op.Inputs[0].isConst()) {
      Target = Op.Inputs[0].Offset;
      break;
    }
  if (Target == InvalidVA)
    return false;
  // External libc through any registered import address or executable veneer.
  if (const Import *Imp = CurrentImg->findImportAt(Target))
    if (libc::isNoReturnFunction(Imp->Name))
      return true;
  // Statically-linked / internal: the target is the function entry itself.
  if (const Symbol *Sym = CurrentImg->findSymbolAt(Target))
    if (libc::isNoReturnFunction(Sym->Name))
      return true;
  return false;
}

void CFGBuilder::rewriteAsTailCall(InsnRecord &Rec) {
  if (!CurrentImg)
    return;
  const auto &TRI = getTargetRegInfo(CurrentImg->Arch);
  NdVar RetReg = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  va_t Target = Rec.BranchTarget;
  va_t At = Rec.Addr;

  // The instruction's only effect is the control transfer, so drop every op it
  // lifted to (e.g. x86 emits a lone BRANCH; ARM also emits a `COPY PC, target`
  // pipeline model whose leftover constant would otherwise pollute
  // call-argument recovery) and replace them with CALL(retReg, target) +
  // RETURN(retReg), mirroring a real `call target; ret`.  Downstream ABI
  // recovery then recovers the call arguments set up before the branch.
  Rec.Ops.clear();

  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = RetReg;
  Call.addInput(NdVar::cst(Target, TRI.PointerSize));
  Call.Addr = At;
  Rec.Ops.push_back(Call);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(RetReg);
  Ret.Addr = At;
  Rec.Ops.push_back(Ret);

  Rec.IsBranch = false;
  Rec.IsCond = false;
  Rec.IsIndirect = false;
  Rec.IsCall = true;
  Rec.IsRet = true;
  Rec.BranchTarget = InvalidVA;
  CallTargets.insert(Target);
}

void CFGBuilder::rewriteAsIndirectTailCall(InsnRecord &Rec) {
  if (!CurrentImg)
    return;

  // Capture the indirect branch target (the function-pointer register/temp)
  // before clearing the instruction's lifted ops.
  NdVar Target;
  bool Found = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      Target = Op.Inputs[0];
      Found = true;
      break;
    }
  if (!Found)
    return;

  const auto &TRI = getTargetRegInfo(CurrentImg->Arch);
  NdVar RetReg = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  va_t At = Rec.Addr;

  // Replace `bx reg` (+ any ARM `COPY PC, reg` pipeline model) with an indirect
  // call to the target followed by a return of the result, mirroring a real
  // `blx reg; bx lr`.  Downstream ABI recovery threads the call arguments set
  // up before the branch into the INDIR_CALL.
  Rec.Ops.clear();

  LowOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Output = RetReg;
  Call.addInput(Target);
  Call.Addr = At;
  Rec.Ops.push_back(Call);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(RetReg);
  Ret.Addr = At;
  Rec.Ops.push_back(Ret);

  Rec.IsBranch = false;
  Rec.IsCond = false;
  Rec.IsIndirect = true;
  Rec.IsCall = true;
  Rec.IsRet = true;
  Rec.BranchTarget = InvalidVA;
}

//===----------------------------------------------------------------------===//
// linkSuccessors — wire up block successor/predecessor edges
//===----------------------------------------------------------------------===//

void CFGBuilder::linkSuccessors(LowFunc &Func,
                                const std::map<va_t, int> &AddrToBlock) {
  for (size_t I = 0; I < Func.Blocks.size(); ++I) {
    auto &Blk = Func.Blocks[I];
    if (Blk.Ops.empty())
      continue;

    va_t LastAddr = Blk.Ops.back().Addr;
    auto It = Insns.find(LastAddr);
    if (It == Insns.end())
      continue;
    auto &Rec = It->second;

    if (Rec.IsRet && Rec.IsCond && Rec.IsBranch) {
      auto BIt = AddrToBlock.find(Rec.BranchTarget);
      if (Rec.BranchTarget != InvalidVA && BIt != AddrToBlock.end())
        Blk.Succs.push_back(BIt->second);
    } else if (Rec.IsRet) {
      // Terminal — no successors.
    } else if (Rec.IsBranch && Rec.IsIndirect &&
               !Rec.JumpTableTargets.empty()) {
      for (va_t T : Rec.JumpTableTargets) {
        auto TIt = AddrToBlock.find(T);
        if (TIt != AddrToBlock.end())
          Blk.Succs.push_back(TIt->second);
      }
    } else if (Rec.IsBranch && !Rec.IsIndirect && !Rec.IsCall) {
      if (Rec.IsCond) {
        va_t Fall = Rec.Addr + Rec.Size;
        auto FIt = AddrToBlock.find(Fall);
        if (FIt != AddrToBlock.end())
          Blk.Succs.push_back(FIt->second);
      }
      auto BIt = AddrToBlock.find(Rec.BranchTarget);
      if (Rec.BranchTarget != InvalidVA && BIt != AddrToBlock.end())
        Blk.Succs.push_back(BIt->second);
    } else if (!Rec.IsBranch || Rec.IsCall) {
      // A no-return call (longjmp/abort/exit/...) ends control flow: do not
      // wire a fall-through edge to the next block (the emitter then terminates
      // the block with a dead `ret`, which the noreturn-marked callee folds
      // away).
      if (!Rec.IsNoReturnCall && I + 1 < Func.Blocks.size())
        Blk.Succs.push_back(static_cast<int>(I + 1));
    }

    for (int S : Blk.Succs) {
      if (S >= 0 && S < static_cast<int>(Func.Blocks.size()))
        Func.Blocks[S].Preds.push_back(Blk.Id);
    }
  }
}

void CFGBuilder::linkExceptionalSuccessors(LowFunc &Func) {
  for (LowBlock &Block : Func.Blocks) {
    Block.ExceptionalSuccs.clear();
    Block.ExceptionalPreds.clear();
  }
  if (!Func.ExceptionMetadata)
    return;
  const ExceptionFunction &Metadata = *Func.ExceptionMetadata;

  auto TargetBlockId = [&](va_t TargetVA) {
    if (LowBlock *Target = Func.blockFor(TargetVA))
      return Target->Id;
    return -1;
  };
  auto AddEdge = [&](LowBlock &Source, va_t TargetVA, ExceptionalEdgeKind Kind,
                     uint32_t Region, int32_t State) {
    if (TargetVA == 0)
      return;
    ExceptionalEdge Edge;
    Edge.BlockId = TargetBlockId(TargetVA);
    Edge.TargetVA = TargetVA;
    Edge.Kind = Kind;
    Edge.RegionIndex = Region;
    Edge.State = State;
    if (std::find(Source.ExceptionalSuccs.begin(),
                  Source.ExceptionalSuccs.end(),
                  Edge) == Source.ExceptionalSuccs.end())
      Source.ExceptionalSuccs.push_back(Edge);
    if (Edge.BlockId >= 0 &&
        Edge.BlockId < static_cast<int>(Func.Blocks.size())) {
      ExceptionalEdge Pred = Edge;
      Pred.BlockId = Source.Id;
      auto &Preds = Func.Blocks[Edge.BlockId].ExceptionalPreds;
      if (std::find(Preds.begin(), Preds.end(), Pred) == Preds.end())
        Preds.push_back(Pred);
    }
  };
  auto ForProtectedBlocks = [&](const ExceptionAddressRange &Range, auto &&Fn) {
    for (LowBlock &Block : Func.Blocks)
      if (Block.StartAddr < Range.End && Block.EndAddr > Range.Begin)
        Fn(Block);
  };

  if (Metadata.SEH) {
    for (size_t I = 0; I < Metadata.SEH->Scopes.size(); ++I) {
      const SEHScopeRecord &Scope = Metadata.SEH->Scopes[I];
      ForProtectedBlocks(Scope.GuardedRange, [&](LowBlock &Block) {
        switch (Scope.Kind) {
        case SEHScopeKind::Filter:
          AddEdge(Block, Scope.FilterOrFinallyVA,
                  ExceptionalEdgeKind::SEHFilter, static_cast<uint32_t>(I), -1);
          AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHHandler,
                  static_cast<uint32_t>(I), -1);
          break;
        case SEHScopeKind::CatchAll:
          AddEdge(Block, Scope.HandlerVA, ExceptionalEdgeKind::SEHHandler,
                  static_cast<uint32_t>(I), -1);
          break;
        case SEHScopeKind::Finally:
          AddEdge(Block, Scope.FilterOrFinallyVA,
                  ExceptionalEdgeKind::SEHFinally, static_cast<uint32_t>(I),
                  -1);
          break;
        }
      });
    }
  }

  if (Metadata.Cxx) {
    const CxxExceptionInfo &Cxx = *Metadata.Cxx;
    for (LowBlock &Block : Func.Blocks) {
      int32_t State = -1;
      for (const CxxIPState &IPState : Cxx.IPMap) {
        if (IPState.IP > Block.StartAddr)
          break;
        State = IPState.State;
      }
      if (State < 0 || State >= static_cast<int32_t>(Cxx.UnwindMap.size()))
        continue;

      const CxxUnwindAction &Cleanup = Cxx.UnwindMap[State];
      if (Cleanup.ActionVA != 0)
        AddEdge(Block, Cleanup.ActionVA, ExceptionalEdgeKind::CxxCleanup,
                static_cast<uint32_t>(State), State);

      for (size_t I = 0; I < Cxx.TryBlocks.size(); ++I) {
        const CxxTryBlock &Try = Cxx.TryBlocks[I];
        if (State < Try.TryLow || State > Try.TryHigh)
          continue;
        for (const CxxCatchHandler &Catch : Try.Handlers)
          AddEdge(Block, Catch.HandlerVA, ExceptionalEdgeKind::CxxCatch,
                  static_cast<uint32_t>(I), State);
      }
    }
  }

  // Itanium.  A Rust frame is deliberately not walked separately: its landing
  // pads are a reclassification of these same call sites (or, on MSVC targets,
  // of the `Cxx` maps handled above), so reading both would double every edge.
  if (Metadata.Itanium && Metadata.Itanium->IsCallSiteAddressForm) {
    const ItaniumEHInfo &Itanium = *Metadata.Itanium;
    auto FindAction = [&](uint64_t Offset) -> const ItaniumAction * {
      for (const ItaniumAction &Action : Itanium.Actions)
        if (Action.TableOffset == Offset)
          return &Action;
      return nullptr;
    };
    for (size_t I = 0; I < Itanium.CallSites.size(); ++I) {
      const ItaniumCallSite &Site = Itanium.CallSites[I];
      // A zero landing pad is how the table spells "no local handler": the
      // exception leaves the frame instead of entering it, so there is no edge.
      if (Site.LandingPadVA == 0)
        continue;

      // One clause per distinct (kind, filter) the chain names.  A chain that
      // repeats a kind describes one pad entry, not several.
      llvm::SmallVector<std::pair<ExceptionalEdgeKind, int32_t>, 4> Clauses;
      auto AddClause = [&](ExceptionalEdgeKind Kind, int64_t Filter) {
        auto Clause = std::make_pair(Kind, static_cast<int32_t>(Filter));
        if (std::find(Clauses.begin(), Clauses.end(), Clause) == Clauses.end())
          Clauses.push_back(Clause);
      };
      if (!Site.FirstActionOffset) {
        // The ABI defines a landing pad with no action record as an
        // unconditional cleanup, which is the shape every destructor-only
        // frame has.
        AddClause(ExceptionalEdgeKind::ItaniumCleanupPad, 0);
      } else {
        std::optional<uint64_t> Offset = Site.FirstActionOffset;
        // The chain is a linked list inside a table the decoder already
        // bounded, so a step budget of the action count both terminates a
        // cycle and cannot cut a well-formed chain short.
        for (size_t Step = 0; Offset && Step <= Itanium.Actions.size(); ++Step) {
          const ItaniumAction *Action = FindAction(*Offset);
          if (!Action)
            break;
          AddClause(Action->isCleanup()   ? ExceptionalEdgeKind::ItaniumCleanupPad
                    : Action->isCatch()   ? ExceptionalEdgeKind::ItaniumCatchPad
                                          : ExceptionalEdgeKind::ItaniumSpecPad,
                    Action->TypeFilter);
          Offset = Action->NextActionOffset;
        }
      }
      ForProtectedBlocks(Site.GuardedRange, [&](LowBlock &Block) {
        for (const auto &[Kind, Filter] : Clauses)
          AddEdge(Block, Site.LandingPadVA, Kind, static_cast<uint32_t>(I),
                  Filter);
      });
    }
  }

  // Delphi.  A `TExcFrame` has no scope table and no per-scope range: one
  // frame guards one region, which runs from the instruction that linked the
  // record onto the chain up to the descriptor.  The descriptor bounds it
  // because Delphi lays the dispatch code and the handler bodies out after the
  // guarded body — the same layout the decoder already reads the arms from.
  if (Metadata.Delphi) {
    const DelphiFrameInfo &Delphi = *Metadata.Delphi;
    ExceptionAddressRange Guarded;
    Guarded.Begin = Metadata.CodeRange.contains(Delphi.ChainInstallVA)
                        ? Delphi.ChainInstallVA
                        : Metadata.CodeRange.Begin;
    Guarded.End = Metadata.CodeRange.contains(Delphi.DescriptorVA) &&
                          Delphi.DescriptorVA > Guarded.Begin
                      ? Delphi.DescriptorVA
                      : Metadata.CodeRange.End;
    if (Guarded.isValid())
      ForProtectedBlocks(Guarded, [&](LowBlock &Block) {
        switch (Delphi.Kind) {
        case DelphiHandlerKind::Finally:
          AddEdge(Block, Delphi.FinallyBodyVA,
                  ExceptionalEdgeKind::DelphiFinally, 0, -1);
          break;
        case DelphiHandlerKind::AnyException:
        case DelphiHandlerKind::AutoException:
          AddEdge(Block, Delphi.ExceptBodyVA, ExceptionalEdgeKind::DelphiExcept,
                  0, -1);
          break;
        case DelphiHandlerKind::OnException:
          for (size_t I = 0; I < Delphi.OnExceptions.size(); ++I)
            AddEdge(Block, Delphi.OnExceptions[I].HandlerVA,
                    ExceptionalEdgeKind::DelphiOnException,
                    static_cast<uint32_t>(I), -1);
          break;
        case DelphiHandlerKind::Unknown:
          break;
        }
      });
  }

  // Delphi on x86-64, where the frame does carry a scope table and so names
  // the exact range each handler guards.
  if (Metadata.DelphiScopes) {
    const std::vector<DelphiScopeRecord> &Scopes =
        Metadata.DelphiScopes->Scopes;
    for (size_t I = 0; I < Scopes.size(); ++I) {
      const DelphiScopeRecord &Scope = Scopes[I];
      ForProtectedBlocks(Scope.GuardedRange, [&](LowBlock &Block) {
        switch (Scope.Kind) {
        case DelphiScopeKind::Finally:
          AddEdge(Block, Scope.TargetVA, ExceptionalEdgeKind::DelphiFinally,
                  static_cast<uint32_t>(I), -1);
          break;
        case DelphiScopeKind::SafecallCatch:
        case DelphiScopeKind::CatchAll:
          AddEdge(Block, Scope.TargetVA, ExceptionalEdgeKind::DelphiExcept,
                  static_cast<uint32_t>(I), -1);
          break;
        case DelphiScopeKind::OnException:
          for (size_t J = 0; J < Scope.OnExceptions.size(); ++J)
            AddEdge(Block, Scope.OnExceptions[J].HandlerVA,
                    ExceptionalEdgeKind::DelphiOnException,
                    static_cast<uint32_t>(I), static_cast<int32_t>(J));
          break;
        }
      });
    }
  }

  // Go.  The runtime resumes a panicking frame at `deferreturn` to run what
  // the frame deferred, so that address is entered without any branch reaching
  // it.  The sites that can start that transfer are the ones that register a
  // defer and the ones that raise, which is what the frame's own metadata
  // names; a `recover` site gets no edge here because recovery resumes the
  // frame that deferred rather than the deferred frame the call sits in.
  if (Metadata.Go && Metadata.Go->DeferReturnOffset) {
    va_t DeferReturn =
        Metadata.CodeRange.Begin + *Metadata.Go->DeferReturnOffset;
    if (Metadata.CodeRange.contains(DeferReturn)) {
      auto AddGoEdge = [&](va_t SiteVA) {
        if (LowBlock *Block = Func.blockFor(SiteVA))
          AddEdge(*Block, DeferReturn, ExceptionalEdgeKind::GoDeferReturn, 0,
                  -1);
      };
      for (const GoDeferSite &Defer : Metadata.Go->Defers)
        AddGoEdge(Defer.CallVA);
      for (const GoPanicSite &Panic : Metadata.Go->Panics)
        AddGoEdge(Panic.CallVA);
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
