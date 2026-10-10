//===- COFFRegistrationCxxIR.cpp - Independent PE32 C++ control proof -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "COFFNativeEHProvenance.h"
#include "COFFRegistrationCxxIRProof.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86RegistrationCxxUnwind.h"
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationFrame.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/TargetParser/Triple.h"
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
#include "llvm/IR/WinEHFrame.h"
#endif
#include <functional>

namespace neverd::coff_registration {
llvm::Expected<CxxIRControlProof>
getCheckedCxxControlIRProof(const llvm::Function &Function,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image) {
#ifndef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  return rejectIR("LLVM does not provide complete PE32 C++ control receipts");
#else
  constexpr auto Model =
      windows_eh_md::NativeProvenanceModel::X86RegistrationCxx;
  using Role = windows_eh_md::NativeProvenanceRole;
  const auto Classification =
      classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  const auto Runtime =
      coff_loader::getCheckedX86CxxPersonalityABI(Image, Source);
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Classification.Model != WindowsEHNativeSourceModel::X86RegistrationCxx ||
      !Classification.canLowerNativeIR() || !Runtime ||
      !coff_loader::getCheckedX86CxxMetadataRanges(Image, Source))
    return rejectIR(
        "source C++ graph has no complete native control projection");
  const auto &Cxx = *Source.Cxx;
  const auto UnwindGraph = projectX86RegistrationCxxUnwind(Cxx);
  if (!UnwindGraph)
    return rejectIR("C++ source unwind destinations are incomplete");
  const auto *Module = Function.getParent();
  const auto *Marker = Function.getMetadata(windows_eh_md::NativeAttachment);
  const auto *Kind =
      Marker && Marker->getNumOperands() == 2
          ? llvm::dyn_cast_or_null<llvm::MDString>(Marker->getOperand(1).get())
          : nullptr;
  const auto *Personality =
      Function.hasPersonalityFn()
          ? llvm::dyn_cast<llvm::Function>(Function.getPersonalityFn())
          : nullptr;
  if (!Module || !Marker || Marker->getNumOperands() != 2 ||
      metadataInteger(*Marker, 0, 1) != 1 || !Kind ||
      Kind->getString() != "cxx-x86-registration-native" || !Personality ||
      Personality->getName() != "__CxxFrameHandler3" ||
      !Personality->isDeclaration() || !Personality->hasExternalLinkage() ||
      !Personality->getReturnType()->isIntegerTy(32) ||
      !Personality->isVarArg() || Personality->arg_size() ||
      Personality->getCallingConv() != llvm::CallingConv::C ||
      Module->getModuleFlag("eh-asynch") ||
      Module->getTargetTriple().getArch() != llvm::Triple::x86 ||
      !Module->getTargetTriple().isWindowsMSVCEnvironment() ||
      Module->getDataLayout().getPointerSize() != 4 ||
      !Function.hasFnAttribute(llvm::RewriteWinX86CxxFrameAttribute) ||
      !Function.hasFnAttribute(llvm::Attribute::NoInline) ||
      !Function.hasFnAttribute(llvm::Attribute::OptimizeNone) ||
      Function.getFnAttribute("frame-pointer").getValueAsString() != "all" ||
      !Function.getReturnType()->isIntegerTy(32) || Function.isVarArg() ||
      llvm::verifyFunction(Function))
    return rejectIR("native C++ compiler or target contract changed");
  auto Identity = rewrite_source::getOriginalVA(Function);
  if (!Identity)
    return Identity.takeError();
  auto PersonalityVA = rewrite_source::getOriginalVA(*Personality);
  if (!PersonalityVA)
    return PersonalityVA.takeError();
  if (*Identity != Source.CodeRange.Begin ||
      *PersonalityVA != Runtime->RuntimeVA)
    return rejectIR("C++ parent or CRT dispatcher lost its source identity");
  CxxIRControlProof Result;
  auto Frame = registrationFrame(Function, Source);
  if (!Frame)
    return Frame.takeError();
  Decoder Decoder;
  if (!Decoder.init(Image))
    return rejectIR("cannot replay C++ registration source analysis");
  Result.Source = CFGBuilder().build(Image, Decoder, Source.CodeRange.Begin,
                                     Function.getName().str());
  if (!hasCallerCleanupRegistrationABI(Result.Source, Image,
                                       &Result.CallerPCWrites) ||
      !Result.Source.RegistrationStates)
    return rejectIR("C++ source has no checked call/frame ABI");
  const auto &States = *Result.Source.RegistrationStates;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      !States.CallFrameEffectsComplete || !States.CleanupFrameEffectsComplete ||
      !States.CxxContinuationsComplete || !States.CxxCatchObjectsComplete ||
      !States.RuntimeObjectAccessesComplete || !States.ImageReadsComplete ||
      !States.IncomingFrameAccessesComplete)
    return rejectIR("replayed C++ registration effects are incomplete");
  const auto Coordinate = cxxRegistrationFrameCoordinate(Source, &States);
  const auto Size = Frame->Slot->getAllocationSize(Module->getDataLayout());
  const auto Layout = Coordinate && Size && !Size->isScalable()
                          ? projectX86RegistrationFrame(
                                *Coordinate, Size->getFixedValue(),
                                Frame->EntrySP, Frame->Slot->getAlign().value())
                          : std::nullopt;
  if (!Layout)
    return rejectIR("C++ source frame lost its physical coordinate projection");
  Frame->Establisher = Layout->Establisher;
  Result.Frame = *Frame;
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Result.Source, Arch::X86, BinaryFormat::COFF);
  const auto EntryABI = getX86RegistrationCxxEntryABI(Med, Function);
  if (!EntryABI || Function.getCallingConv() != *EntryABI)
    return rejectIR("C++ parent changed its physical entry ABI");
  const std::array<const llvm::Function *, 1> Functions = {&Function};
  std::set<const llvm::Instruction *> IncomingSetup;
  if (auto Error = validateIncomingCallerFrame(
          Function, Med, Functions, Result.IncomingAccesses, IncomingSetup))
    return std::move(Error);
  auto Segments = validateSourceSegments(Function, Med, {}, Functions, true);
  if (!Segments)
    return Segments.takeError();
  Result.Segments = std::move(*Segments);
  std::map<int, const RegistrationBlockState *> StateByBlock;
  std::map<va_t, int> BlockAt;
  for (const auto &State : States.Blocks)
    if (State.Reached) {
      if (State.Unknown ||
          !StateByBlock.emplace(State.BlockId, &State).second ||
          !BlockAt.emplace(State.Range.Begin, State.BlockId).second ||
          !Result.Segments.count(State.BlockId))
        return rejectIR(
            "C++ source block state has no exact execution segment");
    }
  if (StateByBlock.size() != Result.Segments.size())
    return rejectIR("C++ execution segments lost a reached source block");
  std::map<Role, std::vector<std::pair<const llvm::CallInst *,
                                       coff_native_eh::NativeEHProvenance>>>
      Anchors;
  std::set<const llvm::BasicBlock *> CompilerBlocks;
  CompilerBlocks.insert(&Function.getEntryBlock());
  const llvm::IntrinsicInst *Escape = nullptr;
  std::set<const llvm::CallBase *> ActualCalls;
  for (const auto &Block : Function)
    for (const auto &I : Block) {
      if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
        if (Call->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle)) {
          auto P = coff_native_eh::parseNativeEHProvenance(*Call);
          if (!P || P->Model != Model ||
              P->FunctionVA != Source.CodeRange.Begin)
            return rejectIR(
                "C++ control anchor has a malformed source identity");
          Anchors[P->Role].emplace_back(Call, *P);
        }
      }
      if (const auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
        if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::localescape) {
          if (Escape || &Block != &Function.getEntryBlock() ||
              Intrinsic->arg_size() !=
                  1 + unsigned(!Result.IncomingAccesses.empty()) ||
              Intrinsic->getArgOperand(0) != Frame->Slot ||
              (Intrinsic->arg_size() == 2 &&
               (!llvm::isa<llvm::AllocaInst>(Intrinsic->getArgOperand(1)) ||
                !llvm::cast<llvm::AllocaInst>(Intrinsic->getArgOperand(1))
                     ->getMetadata(
                         windows_eh_md::RegistrationCallerFrameAttachment))))
            return rejectIR("C++ frame lost its unique whole-frame escape");
          Escape = Intrinsic;
        }
      } else if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
        ActualCalls.insert(Call);
      if (llvm::isa<llvm::CatchSwitchInst>(I)) {
        CompilerBlocks.insert(&Block);
      } else if (const auto *Pad = llvm::dyn_cast<llvm::CleanupPadInst>(&I)) {
        std::optional<uint32_t> State;
        for (uint32_t Index = 0; Index < Cxx.UnwindMap.size(); ++Index) {
          auto Token = windows_eh_semantics::getCxxCleanupSemanticToken(
              Source, Arch::X86, Index);
          if (Token && exactSemanticToken(*Pad, *Token))
            State = Index;
        }
        if (!State || !Result.Cleanups.emplace(*State, Pad).second ||
            Pad->getParentPad() !=
                llvm::ConstantTokenNone::get(Function.getContext()) ||
            Pad->arg_size())
          return rejectIR("C++ cleanup lost its source action identity");
        CompilerBlocks.insert(&Block);
      } else if (llvm::isa<llvm::LandingPadInst, llvm::ResumeInst,
                           llvm::IndirectBrInst, llvm::CallBrInst>(I))
        return rejectIR("C++ IR contains an unproved execution transfer");
    }
  if (!Escape)
    return rejectIR("C++ frame has no runtime escape");
  if (auto Error = bindCxxCatches(Result, Med, Function, *Layout))
    return std::move(Error);
  auto BundleIs = [](const llvm::CallBase &Call, const llvm::Value *Pad) {
    auto Bundle = Call.getOperandBundle("funclet");
    return Pad ? Bundle && Bundle->Inputs.size() == 1 &&
                     Bundle->Inputs[0].get() == Pad
               : !Bundle;
  };
  auto UnwindAt = [&](int32_t State) -> const llvm::BasicBlock * {
    if (State < 0 || uint32_t(State) >= UnwindGraph->size())
      return nullptr;
    const auto &Target = (*UnwindGraph)[State];
    switch (Target.TargetKind) {
    case X86RegistrationCxxUnwindTarget::Kind::Caller:
      return nullptr;
    case X86RegistrationCxxUnwindTarget::Kind::Try:
      return Result.Catches.at({Target.Index, 0})
          .Pad->getCatchSwitch()
          ->getParent();
    case X86RegistrationCxxUnwindTarget::Kind::Cleanup:
      return Result.Cleanups.count(Target.Index)
                 ? Result.Cleanups.at(Target.Index)->getParent()
                 : nullptr;
    }
    llvm_unreachable("invalid checked unwind destination");
  };
  for (uint32_t Index = 0; Index < Cxx.TryBlocks.size(); ++Index) {
    const auto &Try = Cxx.TryBlocks[Index];
    const auto *Switch = Result.Catches.at({Index, 0}).Pad->getCatchSwitch();
    if (Switch->getUnwindDest() != UnwindAt(Cxx.UnwindMap[Try.TryLow].ToState))
      return rejectIR(
          "C++ catch dispatch changed its enclosing search context");
  }
  std::set<const llvm::Instruction *> ExpectedAnchors;
  const auto &Dispatch = Anchors[Role::RegionDispatch];
  if (Dispatch.size() != Result.Catches.size())
    return rejectIR("C++ catch dispatch anchor set is incomplete");
  std::set<X86RegistrationCatchIdentity> Clauses;
  for (const auto &[Anchor, P] : Dispatch) {
    const X86RegistrationCatchIdentity Identity{P.Region, P.Clause};
    if (!Result.Catches.count(Identity) || !Clauses.insert(Identity).second)
      return rejectIR("C++ catch dispatch has no unique clause identity");
    const auto &Handler = Cxx.TryBlocks[P.Region].Handlers[P.Clause];
    const auto *Pad = Result.Catches.at(Identity).Pad;
    if (P.SourceVA != Handler.HandlerVA ||
        P.AuxVA != Handler.TypeDescriptorVA || P.Flags != Handler.Adjectives ||
        next(*Pad) != Anchor || !BundleIs(*Anchor, Pad))
      return rejectIR("C++ catch dispatch anchor changed");
    ExpectedAnchors.insert(Anchor);
  }
  const auto &CleanupAnchors = Anchors[Role::RegistrationCallback];
  if (CleanupAnchors.size() != Result.Cleanups.size() ||
      Result.Cleanups.size() != States.CleanupContracts.size())
    return rejectIR("C++ source cleanup set is incomplete");
  for (const auto &Contract : States.CleanupContracts) {
    auto Found = Result.Cleanups.find(Contract.ActionState);
    if (Found == Result.Cleanups.end())
      return rejectIR("C++ source cleanup was omitted");
    const auto *Pad = Found->second;
    const auto *Return = llvm::dyn_cast<llvm::CleanupReturnInst>(
        Pad->getParent()->getTerminator());
    const llvm::CallBase *Borrow = nullptr;
    for (const auto &I : *Pad->getParent())
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call)) {
        if (Borrow)
          return rejectIR("C++ cleanup has an extra callee");
        Borrow = Call;
      }
    const auto *Anchor = llvm::dyn_cast_or_null<llvm::CallInst>(next(*Pad));
    const auto P = Anchor ? coff_native_eh::parseNativeEHProvenance(*Anchor)
                          : std::nullopt;
    if (!Return || Return->getCleanupPad() != Pad ||
        Return->getUnwindDest() !=
            UnwindAt(Cxx.UnwindMap[Contract.ActionState].ToState) ||
        !Borrow || !P || P->Role != Role::RegistrationCallback ||
        P->SourceVA != Contract.RelayTarget ||
        P->Region != Contract.ActionState || P->Clause ||
        P->AuxVA != Contract.Leaf.Target || P->Flags != 1 ||
        !BundleIs(*Anchor, Pad) || !BundleIs(*Borrow, Pad))
      return rejectIR("C++ cleanup changed its action or outer unwind edge");
    ExpectedAnchors.insert(Anchor);
    const auto Offset =
        Source.Registration->cxxSourceFrameOffset(Contract.ObjectFrameOffset);
    if (!Offset)
      return rejectIR("C++ cleanup object has no checked source coordinate");
    Result.Calls.emplace(Borrow, CxxIRCall{Contract.Leaf, *Offset, true});
  }
  std::map<std::pair<va_t, uint32_t>, const llvm::CallBase *> CallsAt;
  for (const auto *Call : ActualCalls)
    if (const auto *MD =
            Call->getMetadata(windows_eh_md::RegistrationOperationAttachment)) {
      auto Address = metadataInteger(*MD, 1, 64);
      auto Seq = metadataInteger(*MD, 2, 32);
      if (!Address || !Seq ||
          !CallsAt.emplace(std::make_pair(*Address, uint32_t(*Seq)), Call)
               .second)
        return rejectIR("C++ source call occurrence was duplicated");
    }
  size_t Protected = 0;
  for (const auto &Effect : States.CallFrameEffects) {
    const auto Found = CallsAt.find({Effect.Address, uint32_t(Effect.OpSeq)});
    const auto *Call = Found == CallsAt.end() ? nullptr : Found->second;
    if (!Call || Effect.CalleeIndex >= States.CalleeContracts.size())
      return rejectIR("C++ source call occurrence was omitted");
    const auto *MD =
        Call->getMetadata(windows_eh_md::RegistrationOperationAttachment);
    const int BlockId = int(*metadataInteger(*MD, 3, 32));
    if (!StateByBlock.count(BlockId))
      return rejectIR("C++ call moved outside a reached source block");
    const auto &State = *StateByBlock.at(BlockId);
    if (State.Levels.size() > 1)
      return rejectIR("C++ source call has ambiguous dispatch states");
    const int32_t Level = State.Levels.empty() ? -1 : State.Levels[0];
    const bool HasTry =
        llvm::any_of(State.CxxSearches,
                     [&](const auto &Search) { return Search.Level == Level; });
    const auto *Unwind = HasTry ? UnwindAt(Level) : nullptr;
    const auto &Contract = States.CalleeContracts[Effect.CalleeIndex];
    if (Unwind) {
      ++Protected;
      const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call);
      const auto *Anchor =
          llvm::dyn_cast_or_null<llvm::CallInst>(Call->getPrevNode());
      auto P = Anchor ? coff_native_eh::parseNativeEHProvenance(*Anchor)
                      : std::nullopt;
      if (!Invoke || Invoke->getUnwindDest() != Unwind || !P ||
          P->Role != Role::ProtectedInvoke || P->SourceVA != Effect.Address ||
          P->Region != uint32_t(Level) || P->Clause != uint32_t(Effect.OpSeq) ||
          P->AuxVA != Effect.Target ||
          P->Flags != uint32_t(Effect.DoesNotReturn) ||
          !BundleIs(*Anchor, nullptr))
        return rejectIR("C++ protected call changed its source dispatch state");
      ExpectedAnchors.insert(Anchor);
    } else if (!llvm::isa<llvm::CallInst>(Call))
      return rejectIR("unprotected C++ call gained an unproved unwind context");
    if (!BundleIs(*Call,
                  State.CallbackOnly
                      ? Result.Catches[Result.SourceCatchOwners.at(BlockId)].Pad
                      : nullptr) ||
        !Result.Calls
             .emplace(Call, CxxIRCall{Contract, Effect.ECXFrameOffset, false})
             .second)
      return rejectIR("C++ call changed its runtime funclet context");
  }
  if (ActualCalls.size() != Result.Calls.size())
    return rejectIR("C++ IR gained an unproved external call");
  for (const auto &[Call, Receipt] : Result.Calls) {
    const auto *Callee = Call->getCalledFunction();
    const bool Throw = Receipt.Contract.isThrow();
    const bool Borrow = Receipt.ObjectFrameOffset.has_value();
    const bool RuntimeRethrow = Receipt.Contract.isRuntimeRethrow();
    if (!Callee || !Callee->isDeclaration() || !Callee->hasExternalLinkage() ||
        Callee->isVarArg() || Call->isInlineAsm() || Call->isTailCall() ||
        Call->arg_size() != (RuntimeRethrow ? 2u : unsigned(Borrow)) ||
        Call->getCallingConv() != (RuntimeRethrow
                                       ? llvm::CallingConv::X86_StdCall
                                   : Borrow ? llvm::CallingConv::X86_ThisCall
                                            : llvm::CallingConv::C) ||
        Call->getCallingConv() != Callee->getCallingConv() ||
        Call->getFunctionType() != Callee->getFunctionType() ||
        (Borrow && !Call->getArgOperand(0)->getType()->isPointerTy()) ||
        (Throw || Receipt.Cleanup ? !Call->getType()->isVoidTy()
                                  : !Call->getType()->isIntegerTy(32)) ||
        Call->doesNotReturn() != Throw || Callee->doesNotReturn() != Throw ||
        (Receipt.Cleanup && !Call->doesNotThrow()) ||
        (!Receipt.Cleanup && Call->doesNotThrow()))
      return rejectIR("C++ preserved callee changed its physical call ABI");
    if (RuntimeRethrow)
      for (unsigned Index = 0; Index != 2; ++Index) {
        const auto *Null = llvm::dyn_cast<llvm::ConstantPointerNull>(
            Call->getArgOperand(Index));
        // Even with the same stdcall convention and null values, inreg can
        // change the physical argument locations and nonnull can introduce UB.
        if (!Null || Null->getType()->getPointerAddressSpace() != 0 ||
            Call->getAttributes().getParamAttrs(Index).hasAttributes() ||
            Callee->getAttributes().getParamAttrs(Index).hasAttributes())
          return rejectIR("C++ rethrow changed its runtime argument contract");
      }
    auto Target = rewrite_source::getOriginalVA(*Callee);
    if (!Target)
      return Target.takeError();
    if (*Target != Receipt.Contract.Target)
      return rejectIR(
          "C++ preserved callee changed its original entry identity");
  }
  std::set<const llvm::CatchReturnInst *> Returns;
  for (const auto &[Anchor, P] : Anchors[Role::HandlerTarget]) {
    const RegistrationCxxContinuation *Resume = nullptr;
    for (const auto &Candidate : States.CxxContinuations)
      if (Candidate.Address == P.SourceVA && Candidate.TryIndex == P.Region &&
          Candidate.CatchIndex == P.Clause)
        Resume = &Candidate;
    if (!Resume)
      return rejectIR("C++ catch lost its checked runtime continuation");
    auto Restored = validateCxxContinuationRestore(
        *Anchor, Result.Frame, *Source.Registration->RegistrationOffset - 4,
        *Resume);
    if (!Restored)
      return Restored.takeError();
    const auto *Return = *Restored;
    const X86RegistrationCatchIdentity Identity{Resume->TryIndex,
                                                Resume->CatchIndex};
    if (!Result.Catches.count(Identity) || P.AuxVA != Resume->TargetVA ||
        P.Flags != uint32_t(Resume->SavedStackOffset) ||
        !BlockAt.count(Resume->TargetVA) || !Return ||
        Return->getCatchPad() != Result.Catches.at(Identity).Pad ||
        Return->getSuccessor() !=
            Result.Segments.at(BlockAt.at(Resume->TargetVA))
                .Enter->getParent() ||
        !BundleIs(*Anchor, Result.Catches.at(Identity).Pad) ||
        !Returns.insert(Return).second)
      return rejectIR("C++ catch changed its checked runtime continuation");
    ExpectedAnchors.insert(Anchor);
  }
  if (Returns.size() != States.CxxContinuations.size())
    return rejectIR("C++ catch omitted a checked runtime continuation");
  std::set<std::pair<va_t, int>> Chain;
  for (const auto &[Anchor, P] : Anchors[Role::RegistrationChainAccess]) {
    const RegistrationChainAccess *Access = nullptr;
    for (const auto &Candidate : States.ChainAccesses)
      if (Candidate.Address == P.SourceVA &&
          uint32_t(Candidate.OpSeq) == P.Clause)
        Access = &Candidate;
    if (!Access || P.Region || P.AuxVA != Access->EndAddress ||
        P.Flags != uint32_t(Access->AccessKind) ||
        !Chain.emplace(Access->Address, Access->OpSeq).second ||
        !BundleIs(*Anchor, nullptr))
      return rejectIR("C++ registration chain lost an exact source occurrence");
    if (Access->AccessKind == RegistrationChainAccess::Kind::ReadPreviousHead) {
      const auto *Head = llvm::dyn_cast_or_null<llvm::LoadInst>(next(*Anchor));
      const auto *Cast =
          Head ? llvm::dyn_cast_or_null<llvm::IntToPtrInst>(next(*Head))
               : nullptr;
      const auto *Prev =
          Cast ? llvm::dyn_cast_or_null<llvm::LoadInst>(next(*Cast)) : nullptr;
      if (!Head || !Head->getType()->isIntegerTy(32) || Head->isVolatile() ||
          Head->isAtomic() ||
          !llvm::isa<llvm::ConstantPointerNull>(Head->getPointerOperand()) ||
          Head->getPointerAddressSpace() != 257 || !Cast ||
          Cast->getOperand(0) != Head || !Head->hasOneUse() || !Prev ||
          Prev->getPointerOperand() != Cast || !Cast->hasOneUse() ||
          !Prev->getType()->isIntegerTy(32) || Prev->isAtomic() ||
          Prev->isVolatile())
        return rejectIR("C++ chain read no longer observes the generated "
                        "previous registration");
      Result.ChainReads.insert(Head);
      Result.ChainReads.insert(Prev);
    }
    ExpectedAnchors.insert(Anchor);
  }
  if (Chain.size() != States.ChainAccesses.size())
    return rejectIR("C++ registration-chain source set is incomplete");
  for (const auto &Block : Function)
    for (const auto &I : Block)
      for (const auto &Operand : I.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257 &&
            !Result.ChainReads.count(&I))
          return rejectIR("C++ IR gained an unproved FS chain observation");
  for (const auto &[Role, List] : Anchors)
    for (const auto &[Anchor, P] : List)
      if (!ExpectedAnchors.count(Anchor))
        return rejectIR("C++ IR gained an unbound control anchor");
  std::map<int, const LowBlock *> SourceBlocks;
  for (const auto &Block : Result.Source.Blocks)
    SourceBlocks.emplace(Block.Id, &Block);
  for (const auto &[Id, Segment] : Result.Segments) {
    const auto *I = Segment.Enter;
    while (I != Segment.Exit) {
      CompilerBlocks.insert(I->getParent());
      I = I->isTerminator() ? &*llvm::cast<llvm::InvokeInst>(I)
                                    ->getNormalDest()
                                    ->getFirstInsertionPt()
                            : next(*I);
    }
    CompilerBlocks.insert(Segment.Exit->getParent());
    const auto *Term = Segment.Exit->getParent()->getTerminator();
    std::set<const llvm::BasicBlock *> Actual, Expected;
    if (const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Term);
        Invoke && Segment.Exit == Invoke && Invoke->doesNotReturn()) {
      const auto *Normal = Invoke->getNormalDest();
      if (Normal->getSinglePredecessor() != Invoke->getParent() ||
          Normal->size() != 1 ||
          !llvm::isa<llvm::UnreachableInst>(Normal->getTerminator()))
        return rejectIR(
            "C++ terminal call gained executable normal continuation code");
      CompilerBlocks.insert(Normal);
    } else if (const auto *Return =
                   llvm::dyn_cast<llvm::CatchReturnInst>(Term)) {
      if (!Returns.count(Return))
        return rejectIR("C++ IR gained an unbound catch return");
    } else {
      if (!SourceBlocks.count(Id))
        return rejectIR("C++ execution segment has no exact source block");
      for (int Successor : SourceBlocks.at(Id)->Succs)
        if (Result.Segments.count(Successor))
          Expected.insert(Result.Segments.at(Successor).Enter->getParent());
      for (unsigned Index = 0; Index < Term->getNumSuccessors(); ++Index)
        Actual.insert(Term->getSuccessor(Index));
      if (Expected != Actual || StateByBlock.at(Id)->CallbackOnly &&
                                    llvm::isa<llvm::ReturnInst>(Term))
        return rejectIR(
            "C++ normal execution changed its checked source edges in block " +
            llvm::Twine(Id));
    }
  }
  const auto *Entry = llvm::dyn_cast<llvm::UncondBrInst>(
      Function.getEntryBlock().getTerminator());
  if (!Entry || !BlockAt.count(Source.CodeRange.Begin) ||
      Entry->getSuccessor(0) !=
          Result.Segments.at(BlockAt.at(Source.CodeRange.Begin))
              .Enter->getParent() ||
      CompilerBlocks.size() != Function.size())
    return rejectIR(
        "C++ IR gained an unbound runtime entry or execution block");
  const auto *Contract =
      Function.getMetadata(exception_rewrite::FunctionAttachment);
  if (!Contract ||
      Contract->getNumOperands() != exception_rewrite::OperandCount ||
      metadataInteger(*Contract, exception_rewrite::Version, 32) !=
          exception_rewrite::SchemaVersion ||
      metadataInteger(*Contract, exception_rewrite::Source, 8) !=
          uint8_t(exception_rewrite::SourceState::Complete) ||
      metadataInteger(*Contract, exception_rewrite::Lowering, 8) !=
          uint8_t(exception_rewrite::LoweringState::Complete) ||
      metadataInteger(*Contract, exception_rewrite::RequiredProtectedCalls,
                      64) != Protected ||
      metadataInteger(*Contract, exception_rewrite::LoweredProtectedCalls,
                      64) != Protected ||
      metadataInteger(*Contract, exception_rewrite::SkippedLandingPads, 64) !=
          0)
    return rejectIR("C++ native contract counters changed");
  if (auto Error = bindCxxCatchStack(Result, Med, Function))
    return std::move(Error);
  return Result;
#endif
}
} // namespace neverd::coff_registration

namespace neverd {
llvm::Error
validateCOFFRegistrationCxxControlIR(const llvm::Function &Function,
                                     const ExceptionFunction &Source,
                                     const BinaryImage &Image) {
  auto Proof =
      coff_registration::getCheckedCxxControlIRProof(Function, Source, Image);
  return Proof ? llvm::Error::success() : Proof.takeError();
}
} // namespace neverd
