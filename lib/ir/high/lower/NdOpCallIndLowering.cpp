//===- NdOpCallIndLowering.cpp - Indirect call lowering -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Indirect call target resolution and INDIR_CALL lowering to HighIR.
/// Extracts the indirect call target from four sources (in priority order):
///   1. Direct constant in the INDIR_CALL input nd-var
///   2. Exact captured-target proof, with the original LOAD observation
///   3. Register-to-argument-index mapping
///   4. STORE→LOAD stack-slot offset matching
///
//===----------------------------------------------------------------------===//

#include "AnalysisCall.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallTarget.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DataSymbolBinding.h"
#include "neverd/loader/MachO/ImmutableNativeCalls.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Resolution strategies (static helpers)
//===----------------------------------------------------------------------===//

/// Strategy 1: the lifter already resolved the IAT slot address, so the
/// INDIR_CALL input is a constant that maps directly to a known function name.
static bool tryResolveNamedAddress(va_t Addr,
                                   const std::map<va_t, std::string> *FuncNames,
                                   const BinaryImage *Image,
                                   MedToHighConverter::CallIndTarget &Out) {
  if (FuncNames) {
    auto It = FuncNames->find(Addr);
    if (It != FuncNames->end()) {
      Out.Name = It->second;
      Out.Addr = Addr;
      Out.IsIndirect = false;
      return true;
    }
  }
  if (Image) {
    if (const Import *Imp = Image->findImportAt(Addr);
        Imp && !Imp->Name.empty()) {
      Out.Name = Imp->Name;
      Out.Addr = Imp->IATAddr ? Imp->IATAddr : Addr;
      Out.IsIndirect = false;
      return true;
    }
    std::string FnName = Image->getFunctionNameAt(Addr);
    if (!FnName.empty() && FnName.find(kAutoFuncPrefix) != 0) {
      Out.Name = FnName;
      Out.Addr = Addr;
      Out.IsIndirect = false;
      return true;
    }
  }
  return false;
}

/// Whether the pointer slot at \p Slot holds an address of \p Image's own
/// code, under a relocation or in a fixed image's bytes: a variable the image
/// initializes, which no loader binds to an import.
static bool holdsOwnCodeAddress(const BinaryImage &Image, va_t Slot) {
  if (Image.CodePtrRelocSlots.count(Slot))
    return true;
  const uint32_t Width = Image.getPointerSize();
  const uint8_t *Bytes = Width ? Image.readVA(Slot, Width) : nullptr;
  return Bytes && Image.hasExecutableCodeOwnerAt(static_cast<va_t>(readPtr(
                      Bytes, Width == 8, archLittleEndian(Image.Arch))));
}

/// The function a call reaches through the pointer slot at \p Slot. Only an
/// import slot names it: one the import directory lists, the loader bound, or
/// the linker named `__imp_X`. Any other name at \p Slot labels the slot
/// itself, a variable such as `int (*handler)(int)`: calling by that name
/// would call the variable rather than the pointer stored in it.
static bool tryResolveLoadedSlot(va_t Slot,
                                 const std::map<va_t, std::string> *FuncNames,
                                 const BinaryImage *Image,
                                 MedToHighConverter::CallIndTarget &Out) {
  if (Image) {
    const ImportStorageSlotCollection Storage =
        Image->collectImportStorageSlot(Slot);
    if (Storage.Conflicts.count(Slot))
      return false;
    const Import *Imp = Image->findImportAt(Slot);
    const bool DirectorySlot = Imp && Imp->IATAddr == Slot;
    std::string Name = DirectorySlot ? Imp->Name : std::string();
    // An import directory entry can carry no name (API-set and delay-load
    // imports often do); the symbol for its slot names the import. The linker
    // names any import address slot `__imp_X` (`__imp__X` on i386), but a
    // slot that holds the image's own code is a variable of that name:
    // MinGW's runtime defines `__imp_X` for each function it supplies in
    // place of an import (__MINGW_IMP_SYMBOL), initialized to that function.
    if (Name.empty())
      if (const Symbol *Sym = Image->findSymbolAt(Slot);
          Sym && !Sym->Name.empty() &&
          (DirectorySlot ||
           ((llvm::StringRef(Sym->Name).starts_with("__imp_") ||
             llvm::StringRef(Sym->Name).starts_with("_imp_")) &&
            !holdsOwnCodeAddress(*Image, Slot))))
        Name = Sym->Name;
    // A slot holding `import + addend` reaches no function by that name.
    if (auto It = Storage.Slots.find(Slot); It != Storage.Slots.end()) {
      if (It->second.Addend != 0)
        return false;
      if (Name.empty())
        Name = It->second.Name;
    }
    if (Name.empty())
      return false;
    Out.Name = std::move(Name);
    Out.Addr = Slot;
    Out.IsIndirect = false;
    // The slot of a symbol the image defines itself, which the loader starts
    // at that routine, reaches the routine, by the name the slot is bound by
    // (__lttf2 and __letf2 may name one routine).
    if (DirectorySlot)
      if (const std::optional<va_t> Own = ownRoutineOfImport(*Image, *Imp))
        Out.Addr = *Own;
    return true;
  }
  // Without an image the caller's names are its only slot evidence.
  if (!FuncNames)
    return false;
  auto It = FuncNames->find(Slot);
  if (It == FuncNames->end())
    return false;
  Out.Name = It->second;
  Out.Addr = Slot;
  Out.IsIndirect = false;
  return true;
}

/// x86 lifts a memory-indirect call through a constant slot, `call [rip+disp]`
/// or `call [disp32]`, as an INDIR_CALL of the slot address, while a constant
/// folded into `call rax` is the callee itself. No code runs from a
/// non-executable segment, so a constant there is a slot: the call reaches
/// the pointer stored in it unless the loader bound the slot to an import.
static bool tryResolveConstTarget(const MedOp &CurOp,
                                  const std::map<va_t, std::string> *FuncNames,
                                  const BinaryImage *Image,
                                  MedToHighConverter::CallIndTarget &Out) {
  if (CurOp.NumInputs < 1 || !CurOp.Inputs[0].isConst())
    return false;
  const va_t Addr = CurOp.Inputs[0].ConstVal;
  if (Image) {
    const Segment *Seg = Image->getSegmentFor(Addr);
    if (Seg && !Seg->isExecutable()) {
      if (!tryResolveLoadedSlot(Addr, FuncNames, Image, Out))
        Out.ThroughSlot = true;
      return true;
    }
  }
  return tryResolveNamedAddress(Addr, FuncNames, Image, Out);
}

/// Strategy 3: if the target expression is a register that maps to a known
/// calling-convention argument index, use that.
static bool tryResolveRegTarget(const ExprPtr &TargetExpr, Arch TargetArch,
                                MedToHighConverter::CallIndTarget &Out) {
  // Later SSA versions can hold a returned function pointer in an argument
  // register; register location alone does not make them entry parameters.
  if (TargetExpr->Kind != ExprKind::Var ||
      TargetExpr->Var.Kind != MedVar::Reg || TargetExpr->Var.SSAVer != 0)
    return false;
  int PIdx = getTargetRegInfo(TargetArch).regToArgIdx(TargetExpr->Var.RegOff);
  if (PIdx < 0)
    return false;
  Out.IndirectParam = PIdx;
  Out.Name = "arg" + std::to_string(PIdx);
  return true;
}

/// Strategy 4: match STORE→LOAD via stack-slot offset to recover the
/// indirect parameter index.
static bool tryResolveStackSlotTarget(const MedOp &CurOp,
                                      const MedBlock &CurBlock, Arch TargetArch,
                                      MedToHighConverter::CallIndTarget &Out) {
  if (CurOp.NumInputs < 1)
    return false;

  auto &TVar = CurOp.Inputs[0];
  int64_t LoadSpOff = INT64_MIN;
  for (auto &BOp : CurBlock.Ops) {
    if (BOp.Opcode == NdOp::LOAD && BOp.Output.Id == TVar.Id &&
        BOp.Output.SSAVer == TVar.SSAVer && BOp.NumInputs >= 1 &&
        BOp.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      auto &LA = BOp.Inputs[0];
      for (auto &ROp : CurBlock.Ops) {
        if (ROp.Opcode == NdOp::INT_ADD && ROp.Output.Id == LA.Id &&
            ROp.Output.SSAVer == LA.SSAVer && ROp.NumInputs >= 2 &&
            ROp.Inputs[1].isConst()) {
          LoadSpOff = static_cast<int64_t>(ROp.Inputs[1].ConstVal);
          break;
        }
      }
      break;
    }
  }
  if (LoadSpOff == INT64_MIN)
    return false;

  const auto &TRI = getTargetRegInfo(TargetArch);
  for (auto &BOp : CurBlock.Ops) {
    if (BOp.Opcode != NdOp::STORE || BOp.NumInputs < 2 ||
        BOp.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (BOp.Inputs[1].Kind != MedVar::Reg || BOp.Inputs[1].SSAVer != 0)
      continue;
    int PIdx = TRI.regToArgIdx(BOp.Inputs[1].RegOff);
    if (PIdx < 0)
      continue;
    auto &SA = BOp.Inputs[0];
    for (auto &ROp : CurBlock.Ops) {
      if (ROp.Opcode == NdOp::INT_ADD && ROp.Output.Id == SA.Id &&
          ROp.Output.SSAVer == SA.SSAVer && ROp.NumInputs >= 2 &&
          ROp.Inputs[1].isConst() &&
          static_cast<int64_t>(ROp.Inputs[1].ConstVal) == LoadSpOff) {
        Out.IndirectParam = PIdx;
        Out.Name = "arg" + std::to_string(PIdx);
        return true;
      }
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// resolveCallIndTarget
//===----------------------------------------------------------------------===//

MedToHighConverter::CallIndTarget MedToHighConverter::resolveCallIndTarget(
    const MedBlock &CurBlock, const MedOp &CurOp, const ExprPtr &TargetExpr) {
  CallIndTarget Result;

  if (tryResolveConstTarget(CurOp, FuncNames, Image, Result))
    return Result;
  size_t CallIdx = 0;
  for (; CallIdx < CurBlock.Ops.size(); ++CallIdx)
    if (&CurBlock.Ops[CallIdx] == &CurOp)
      break;
  const auto ReadReaches = [&](const MedVar &Observation) {
    CallTargetUse = {&CurBlock, CallIdx};
    const bool Reaches = memoryReadReachesCallTarget(Observation);
    CallTargetUse = {};
    return Reaches;
  };
  if (CurOp.NumInputs && !CurOp.Inputs[0].isConst() &&
      CallIdx < CurBlock.Ops.size()) {
    if (const auto Cached =
            resolveMedCallTarget(CurBlock, CallIdx, CurOp.Inputs[0])) {
      if (Cached->ThroughSlot &&
          tryResolveLoadedSlot(Cached->Address, FuncNames, Image, Result)) {
        // Naming a captured import has the same memory premise as
        // inlining its original read at the call. Copies and private
        // spills do not change which observation owns that premise.
        if (ReadReaches(CurBlock.Ops[Cached->LoadIndex].Output))
          return Result;
        CallIndTarget Captured;
        Captured.Addr = Result.Addr;
        return Captured;
      }
      if (!Cached->ThroughSlot &&
          tryResolveNamedAddress(Cached->Address, FuncNames, Image, Result))
        return Result;
    }
  }
  // A predecessor's observation can still identify the captured value's
  // signature. Naming it requires the same reaching-memory proof as a local
  // read; otherwise keep the runtime operand even when the import is known.
  if (TargetExpr && TargetExpr->Kind == ExprKind::Var) {
    ExprPtr Definition = TargetExpr;
    MedVar Observation = Definition->Var;
    for (int Depth = 0; Depth < limits::kCallTargetSlotDepth && Definition &&
                        Definition->Kind == ExprKind::Var &&
                        Definition->Var.Id >= 0 && !Definition->Var.isConst();
         ++Depth) {
      Observation = Definition->Var;
      const auto It = DefExpr.find(varKey(Definition->Var));
      Definition = It == DefExpr.end() ? nullptr : It->second;
    }
    if (Definition && Definition->Kind == ExprKind::Load &&
        Definition->Operands.size() == 1 && Definition->Operands[0] &&
        Definition->Operands[0]->Kind == ExprKind::Const &&
        Definition->MemoryOrdering == NdMemoryOrdering::None &&
        Definition->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        tryResolveLoadedSlot(Definition->Operands[0]->ConstVal, FuncNames,
                             Image, Result)) {
      if (ReadReaches(Observation))
        return Result;
      CallIndTarget Captured;
      Captured.Addr = Result.Addr;
      return Captured;
    }
  }
  if (tryResolveRegTarget(TargetExpr, TargetArch, Result))
    return Result;
  if (tryResolveStackSlotTarget(CurOp, CurBlock, TargetArch, Result))
    return Result;

  return Result;
}

//===----------------------------------------------------------------------===//
// lowerCallInd
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerCallInd(HighFunc &Func, const MedBlock &CurBlock,
                                      const MedOp &CurOp) {
  if (high_detail::appendAnalysisX87Call(Func, CurOp, [&](const MedVar &Value) {
        return medvarToExpr(Value);
      })) {
    CallOutputs.insert({CurOp.Output.Id, CurOp.Output.SSAVer});
    return;
  }
  HighStmt S;
  S.Kind = StmtKind::Call;
  S.Addr = CurOp.Addr;

  ExprPtr TargetExpr = (CurOp.NumInputs >= 1) ? medvarToExpr(CurOp.Inputs[0])
                                              : HighExpr::makeConst(0, 8);

  const MedOp *Authentication = AArch64AuthenticatedCalls.lookup(&CurOp);
  CallIndTarget Target;
  if (Authentication) {
    std::vector<ExprPtr> Operands;
    for (unsigned I = 1; I < Authentication->NumInputs; ++I)
      Operands.push_back(medvarToExpr(Authentication->Inputs[I]));
    TargetExpr =
        HighExpr::makeCall({}, Authentication->Addr, std::move(Operands));
    TargetExpr->IntrinsicId = Intrinsic::A64_AuthBranch;
    TargetExpr->Type = NdType::makeInt(Authentication->Output.Size);
    Target.IsIndirect = true;
  } else {
    Target = resolveCallIndTarget(CurBlock, CurOp, TargetExpr);
  }

  size_t CI = 0;
  for (size_t K = 0; K < CurBlock.Ops.size(); ++K)
    if (&CurBlock.Ops[K] == &CurOp) {
      CI = K;
      break;
    }
  auto Args = collectCallArgs(CurBlock, CI, Target.Addr);

  auto Call = HighExpr::makeCall(
      Target.Name, Target.Addr ? Target.Addr : CurOp.Addr, std::move(Args));
  Call->CallSiteAddr = CurOp.Addr;
  Call->IsIndirectCall = Target.IsIndirect;
  Call->IndirectParamIdx = Target.IndirectParam;
  Call->DoesNotReturn = CurOp.DoesNotReturn;
  if (Target.IsIndirect) {
    if (Authentication) {
      Call->IndirectTarget = TargetExpr;
    } else if (CurOp.SourceCallHint &&
               CurOp.SourceCallHint->CallKind ==
                   SourceCallTypeHint::Kind::SwiftVirtual) {
      // A bound Swift virtual call must use the target loaded before the
      // intervening retain/release effects; reloading its table slot here can
      // change both dispatch and the order of observable memory operations.
      Call->IndirectTarget = TargetExpr;
    } else if (Target.ThroughSlot) {
      const uint16_t PointerBytes = CurOp.Inputs[0].Size;
      Call->IndirectTarget = HighExpr::makeLoad(
          HighExpr::makeConst(CurOp.Inputs[0].ConstVal, PointerBytes,
                              ConstantAddressProvenance::Address),
          NdType::makeInt(PointerBytes), NdMemoryOrdering::None,
          NdMemoryAddressSpace::Default);
    } else {
      CallTargetUse = {&CurBlock, CI};
      ExprPtr Callee = TargetExpr;
      if (Callee && Callee->Kind == ExprKind::Var && Callee->Var.Id >= 0 &&
          memoryReadReachesCallTarget(Callee->Var)) {
        auto It = DefExpr.find(varKey(Callee->Var));
        if (It != DefExpr.end() && It->second &&
            It->second->Kind == ExprKind::Load)
          Callee = It->second;
      }
      Call->IndirectTarget = forceInlineCallTarget(Callee);
      CallTargetUse = {};
    }
  }
  Call->SourceCallHint = CurOp.SourceCallHint;
  if (auto Signature = boundFloatingCallArguments(
          CurOp, TargetArch,
          Image ? Image->abiFormat() : BinaryFormat::Unknown))
    Call->FloatingCallSignature =
        std::make_shared<FloatingCallArguments>(std::move(*Signature));
  else if (CurOp.CalleeFloatReturnBytes && CurMed)
    if (const auto *Info = CurMed->findCall(CurBlock.Id, CI))
      if (auto Signature = recoveredFloatingCallArguments(
              CurOp, *Info, TargetArch,
              Image ? Image->abiFormat() : BinaryFormat::Unknown))
        Call->FloatingCallSignature =
            std::make_shared<FloatingCallArguments>(std::move(*Signature));
  if (CurOp.SourceCallHint) {
    Call->CallAddr = CurOp.SourceCallHint->TargetAddress;
    // A bound void call has no output SSA value, but its expression still
    // carries the authenticated result type rather than a guessed X0 value.
    Call->Type = sourceCallResultType(CurOp);
    if (!Authentication &&
        isImmutableNativeCallHint(*CurOp.SourceCallHint, Func.Entry,
                                  TargetArch) &&
        CurOp.SourceCallHint->ImmutableNativeCall->Site.Instruction ==
            CurOp.Addr &&
        CurOp.SourceCallHint->ImmutableNativeCall->Site.Sequence ==
            CurOp.OriginSeq) {
      // Only the immutable, nonvolatile target load is projected away. Keep
      // the machine INDIR_CALL in MedIR and its original occurrence receipt;
      // source publication independently rebuilds that proof and callee ABI.
      Call->IsIndirectCall = false;
      Call->IndirectParamIdx = -1;
      Call->IndirectTarget.reset();
      Call->CallTarget.clear();
    }
  }
  if (ExpressionObserver && CurOp.Addr != InvalidVA && CurOp.OriginSeq >= 0)
    ExpressionObserver(CurOp, Call);
  if (CurOp.Output.Id >= 0 && CurOp.Output.Size > 0) {
    Call->Type = sourceCallResultType(CurOp);
    S.Kind = StmtKind::Assign;
    const bool SourceFloat = SourceFloatCallValues.count(varKey(CurOp.Output));
    S.Dst = HighExpr::makeVar(
        CurOp.Output,
        SourceFloat || (Call->Type && Call->Type->Kind == NdTypeKind::Struct)
            ? Call->Type
            : nullptr);
    S.Val = Call;
    if (!SourceFloat && CurOp.Output.Kind == MedVar::Reg &&
        CurOp.Output.Size == 10 &&
        getTargetRegInfo(TargetArch).isX87StackReg(CurOp.Output.RegOff)) {
      if (Call->Type && Call->Type->Size != CurOp.Output.Size) {
        S.Val = HighExpr::makeUnary(NdOp::FLOAT_FLOAT2FLOAT, S.Val);
        S.Val->Type = NdType::makeFloat(CurOp.Output.Size);
      }
      S.Val = HighExpr::makeBitCast(S.Val,
                                    NdType::makeInt(CurOp.Output.Size, false));
    }
  }
  CallOutputs.insert({CurOp.Output.Id, CurOp.Output.SSAVer});
  if (S.Kind == StmtKind::Call)
    S.CallExpr = Call;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd
