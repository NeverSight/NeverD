//===- COFFRegistrationIR.cpp - PE32 SEH source replay --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFNativeEHProvenance.h"
#include "COFFRegistrationCopyEdges.h"
#include "COFFRegistrationCopySlots.h"
#include "COFFRegistrationCxxIRProof.h"
#include "COFFRegistrationFrameProof.h"
#include "COFFRegistrationIRProof.h"
#include "COFFRegistrationSEHCall.h"
#include "COFFRegistrationSEHDynamicScope.h"
#include "COFFRegistrationSEHException.h"
#include "COFFRegistrationSEHFinally.h"
#include "COFFRegistrationSEHJump.h"
#include "COFFRegistrationSEHLocalUnwind.h"
#include "COFFRegistrationSEHParentExit.h"
#include "COFFRegistrationSEHRegisters.h"
#include "COFFRegistrationSEHState.h"
#include "COFFRegistrationSEHThread.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/backend/llvm/X86RegistrationSEHScope.h"
#include "neverd/backend/llvm/X86RegistrationSEHStateStore.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"
#include "neverd/ir/med/X86RegistrationSEHCall.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {
namespace {

using coff_registration::rejectIR;
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
using Role = windows_eh_md::NativeProvenanceRole;
constexpr auto Model = windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
using Provenance = coff_native_eh::NativeEHProvenance;

using coff_registration::CallbackKey;
using coff_registration::callbacks;
using coff_registration::exactSemanticToken;
using coff_registration::integer;
using coff_registration::matchesSourceOperation;
using coff_registration::metadataInteger;
using coff_registration::next;
using coff_registration::RegistrationFrame;
using coff_registration::registrationFrame;
using coff_registration::validateIncomingCallerFrame;
using coff_registration::validateSourceSegments;
bool frameAddress(const llvm::Value *Address, const RegistrationFrame &Frame,
                  uint64_t Offset) {
  const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Address);
  llvm::APInt Actual(32, 0);
  return GEP && GEP->getPointerOperand() == Frame.Slot &&
         GEP->getPointerAddressSpace() == 0 &&
         GEP->accumulateConstantOffset(Frame.Slot->getModule()->getDataLayout(),
                                       Actual) &&
         Actual.getZExtValue() == Offset;
}

#endif
} // namespace

llvm::Error validateCOFFRegistrationIR(const llvm::Function &Function,
                                       const ExceptionFunction &Source,
                                       const BinaryImage &Image) {
  if (Source.Cxx)
    return validateCOFFRegistrationCxxIR(Function, Source, Image);
#ifndef LLVM_NEVERD_X86_REGISTRATION_EH
  return rejectIR("LLVM does not provide indexed x86 SEH output receipts");
#else
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      !classifyWindowsEHNativeSource(Source, Arch::X86, BinaryFormat::COFF,
                                     WindowsEHNativeCapability::IRLowering)
           .canLowerNativeIR())
    return rejectIR("source is not a checked x86 registration SEH function");
  if (Source.Personality == ExceptionPersonality::ExceptHandler3 &&
      !coff_loader::isCheckedX86SEH3Personality(Image, Source.PersonalityVA))
    return rejectIR(
        "SEH3 personality has no checked CRT import forwarding path");
  const auto *Marker = Function.getMetadata(windows_eh_md::NativeAttachment);
  const auto *Version = Marker && Marker->getNumOperands() == 2
                            ? llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
                                  Marker->getOperand(0).get())
                            : nullptr;
  const auto *Kind =
      Marker && Marker->getNumOperands() == 2
          ? llvm::dyn_cast_or_null<llvm::MDString>(Marker->getOperand(1).get())
          : nullptr;
  const llvm::StringRef Name =
      Source.Personality == ExceptionPersonality::ExceptHandler4
          ? "_except_handler4"
          : "_except_handler3";
  const auto *Personality =
      Function.hasPersonalityFn()
          ? llvm::dyn_cast<llvm::Function>(Function.getPersonalityFn())
          : nullptr;
  if (!Version || integer(Version->getValue(), 1) != 1 || !Kind ||
      Kind->getString() != "seh-x86-registration-native" || !Personality ||
      Personality->getName() != Name || !Personality->isDeclaration() ||
      !Personality->hasExternalLinkage() ||
      !Personality->getReturnType()->isIntegerTy(32) ||
      !Personality->isVarArg() || Personality->arg_size() ||
      !Function.hasFnAttribute(llvm::Attribute::NoInline) ||
      !Function.hasFnAttribute(llvm::Attribute::OptimizeNone) ||
      !Function.hasFnAttribute("llvm.rewrite.win-x86-registration-state") ||
      Function.getFnAttribute("frame-pointer").getValueAsString() != "all" ||
      llvm::verifyFunction(Function))
    return rejectIR("native x86 SEH compiler contract changed");
  auto Identity = rewrite_source::getOriginalVA(Function);
  if (!Identity)
    return Identity.takeError();
  if (!*Identity || **Identity != Source.CodeRange.Begin)
    return rejectIR("native function has no exact source entry identity");
  auto Frame = registrationFrame(Function, Source);
  if (!Frame)
    return Frame.takeError();
  const auto &Triple = Function.getParent()->getTargetTriple();
  if (Triple.getArch() != llvm::Triple::x86 || !Triple.isOSWindows() ||
      !Triple.isOSBinFormatCOFF() || !Triple.isWindowsMSVCEnvironment() ||
      Function.getParent()->getDataLayout().getPointerSize() != 4 ||
      Function.getParent()->getModuleFlag("eh-asynch") == nullptr ||
      integer(llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                  Function.getParent()->getModuleFlag("eh-asynch")),
              32) != 1 ||
      (Source.Personality == ExceptionPersonality::ExceptHandler4 &&
       Source.Registration->GSCookieOffset != -2 &&
       !Function.hasFnAttribute(llvm::Attribute::StackProtectReq)))
    return rejectIR(
        "native x86 SEH lost its target or asynchronous protection contract");
  Decoder Decoder;
  if (!Decoder.init(Image))
    return rejectIR("cannot replay source registration-state analysis");
  CFGBuilder Builder;
  LowFunc Replay = Builder.build(Image, Decoder, Source.CodeRange.Begin,
                                 Function.getName().str());
  std::vector<ExceptionAddressRange> SourceCallerPCWrites;
  const bool Stdcall =
      Function.getCallingConv() == llvm::CallingConv::X86_StdCall;
  const auto ParentPop = Stdcall ? getCheckedX86RegistrationSEHParentABI(
                                       Replay, Image, &SourceCallerPCWrites)
                                 : std::nullopt;
  if (Stdcall ? !ParentPop || !*ParentPop
              : Function.getCallingConv() != llvm::CallingConv::C ||
                    !hasCallerCleanupRegistrationABI(Replay, Image,
                                                     &SourceCallerPCWrites))
    return rejectIR(
        "source or preserved callee has an unproved x86 stack cleanup ABI");
  if (!Replay.RegistrationStates || !Replay.RegistrationStates->Complete ||
      !Replay.RegistrationStates->CallbackStatesComplete ||
      !Replay.RegistrationStates->RegistrationLifetimeComplete ||
      !Replay.RegistrationStates->ChainOperationsComplete)
    return rejectIR(
        "source registration lifetime or dispatch state is incomplete");
  if (Source.Personality == ExceptionPersonality::ExceptHandler4) {
    const auto &States = *Replay.RegistrationStates;
    const auto *Cookie =
        Function.getParent()->getNamedGlobal("__security_cookie");
    if (!States.SecurityCookiesComplete || !States.SecurityCookieVA ||
        States.SecurityCookieVA !=
            Image.Base + Image.DynInfo.SecurityCookieRVA ||
        !coff_loader::getCheckedX86EH4CookieCheck(Image,
                                                  Source.PersonalityVA) ||
        !Cookie || !Cookie->getValueType()->isIntegerTy(32) ||
        !Cookie->isDeclaration() || !Cookie->hasExternalLinkage() ||
        Cookie->isConstant() || Cookie->isThreadLocal() ||
        Cookie->getAddressSpace() || Cookie->hasDLLImportStorageClass())
      return rejectIR(
          "EH4 cookie frame, image storage or CRT wrapper is unproved");
    if (Source.Registration->GSCookieOffset != -2) {
      const auto *Check =
          Function.getParent()->getFunction("__security_check_cookie");
      if (!Check || !hasX86RegistrationSecurityCheckABI(*Check))
        return rejectIR(
            "EH4 GS cookie checker has an incompatible runtime ABI");
      auto VA = rewrite_source::getOriginalVA(*Check);
      if (!VA)
        return VA.takeError();
      if (!*VA || **VA != *coff_loader::getCheckedX86EH4CookieCheck(
                              Image, Source.PersonalityVA))
        return rejectIR("EH4 GS cookie checker changed its original identity");
      if (!coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, **VA))
        return rejectIR("EH4 GS cookie checker has no checked success path");
    }
  }
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  MedFunc ReplayMed = Converter.convert(Replay, Arch::X86, BinaryFormat::COFF);
  if (Stdcall && !hasX86RegistrationEntryABI(ReplayMed, Function))
    return rejectIR("SEH parent changed its physical stdcall entry ABI");
  const auto *PhysicalStates = getX86RegistrationSEHPhysicalStates(ReplayMed);
  if (!PhysicalStates ||
      !hasCheckedX86RegistrationSEHPhysicalFrame(Image, ReplayMed))
    return rejectIR(
        "shared SEH4 source has no independently replayed physical frame");
  std::map<CallbackKey, std::set<uint8_t>> ExpectedRoots;
  for (const auto &Scope : Source.Registration->Scopes) {
    const CallbackKey Key{Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                          Scope.IsFinally};
    auto &Roots = ExpectedRoots[Key];
    for (const auto &Block : ReplayMed.Blocks)
      if (Block.StartAddr == Key.first)
        for (const auto &Op : Block.Ops)
          if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
            Roots.insert(
                Op.RegistrationRoot ==
                        MedOp::RegistrationRootKind::EstablishedFramePointer
                    ? 0
                    : 1);
  }
  std::map<const llvm::Function *, const llvm::StoreInst *> ExceptionBridges;
  auto CallbackMap =
      callbacks(Function, Source, &ExpectedRoots, &ExceptionBridges);
  if (!CallbackMap)
    return CallbackMap.takeError();
  auto LocalUnwinds = coff_registration::bindSEHLocalUnwinds(
      ReplayMed, Function, *Frame, *CallbackMap);
  if (!LocalUnwinds)
    return LocalUnwinds.takeError();
  auto ParentExits = coff_registration::bindSEHParentExitContext(
      Function, Source.CodeRange.Begin, *CallbackMap, LocalUnwinds->ChainLoads);
  if (!ParentExits)
    return ParentExits.takeError();
  if (auto Error =
          coff_registration::bindSEHParentExitSources(*ParentExits, ReplayMed))
    return Error;
  std::vector<const llvm::Function *> CallbackFunctions;
  for (const auto &[Key, Callback] : *CallbackMap)
    CallbackFunctions.push_back(Callback);
  std::vector<const llvm::Function *> NativeFunctions{&Function};
  NativeFunctions.insert(NativeFunctions.end(), CallbackFunctions.begin(),
                         CallbackFunctions.end());
  auto ExceptionFields = bindSEHExceptionReads(
      ReplayMed, *Frame, *CallbackMap, ExceptionBridges, NativeFunctions);
  if (!ExceptionFields)
    return ExceptionFields.takeError();
  auto ThreadReads = coff_registration::bindSEHThreadReads(ReplayMed, Function,
                                                           NativeFunctions);
  if (!ThreadReads)
    return ThreadReads.takeError();
  std::set<const llvm::Instruction *> IncomingAccesses, IncomingSetup;
  if (llvm::Error Error =
          validateIncomingCallerFrame(Function, ReplayMed, NativeFunctions,
                                      IncomingAccesses, IncomingSetup))
    return Error;
  auto Jumps = coff_registration::bindSEHJumps(ReplayMed, Function, *Frame,
                                               IncomingSetup, *CallbackMap);
  if (!Jumps)
    return Jumps.takeError();
  for (const auto &Block : Function)
    for (const auto &I : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
          Call && Call->getCallingConv() != llvm::CallingConv::C &&
          !Jumps->Calls.count(Call))
        return rejectIR(
            "generated call changes the checked x86 stack cleanup ABI");
  const auto &States = *PhysicalStates;
  const auto &Scopes = Source.Registration->Scopes;
  struct Dispatch {
    const llvm::BasicBlock *Unwind = nullptr;
    const llvm::CatchSwitchInst *Switch = nullptr;
    const llvm::CleanupReturnInst *Cleanup = nullptr;
    const llvm::CatchReturnInst *Return = nullptr;
    const llvm::BasicBlock *Handler = nullptr;
  };
  std::map<uint32_t, Dispatch> Dispatches;
  std::map<uint32_t, const llvm::BasicBlock *> HandlerTargets;
  using RangeKey = std::pair<uint32_t, uint32_t>;
  std::map<RangeKey, std::pair<va_t, const llvm::InvokeInst *>> Enters, Exits;
  struct BlockTarget {
    Provenance Identity;
    const llvm::CallInst *Anchor;
  };
  std::map<uint32_t, BlockTarget> BlockEntries, BlockExits;
  std::set<const llvm::InvokeInst *> Invokes;
  std::set<const llvm::CallBase *> AnchoredDynamicCalls;
  std::set<const llvm::CallBase *> ScopeBoundaries;
  auto ScopeWrites = Jumps->StateRestores;
  ScopeWrites.insert(LocalUnwinds->StateRestores.begin(),
                     LocalUnwinds->StateRestores.end());
  std::set<const llvm::CallBase *> AbnormalFinallyIR;
  std::map<va_t, uint32_t> Protected;
  auto Chain = ParentExits->SourceChainRemovals;
  auto ChainProtocolReads = ParentExits->ChainReads;
  size_t FSReads = 0;
  size_t ExpectedFSReads = ParentExits->ChainReads.size();
  for (const auto &Block : Function)
    for (const auto &Instruction : Block) {
      for (const auto &Operand : Instruction.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257) {
          const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction);
          if (ThreadReads->count(Load) &&
              Load->getPointerOperand() == Operand.get())
            continue;
          if (!Load || Load->getPointerOperand() != Operand.get() ||
              !llvm::isa<llvm::ConstantPointerNull>(Operand) ||
              !Load->getType()->isIntegerTy(32) || Load->isAtomic() ||
              !Load->isVolatile())
            return rejectIR(
                "native parent has an unauthenticated FS operation");
          ++FSReads;
        }
      const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
      if (!Call ||
          !Call->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle))
        continue;
      auto P = coff_native_eh::parseNativeEHProvenance(*Call);
      if (!P || P->Model != Model || P->FunctionVA != Source.CodeRange.Begin)
        return rejectIR("parent registration anchor is malformed");
      if (P->Role == Role::RegistrationDynamicCall) {
        const auto *Protected = coff_registration::dynamicSEHCall(*Call);
        if (!Protected || !AnchoredDynamicCalls.insert(Protected).second)
          return rejectIR("dynamic SEH call lost its exact source receipt");
        continue;
      }
      if (P->Role == Role::RegistrationChainAccess) {
        auto Found =
            llvm::find_if(States.ChainAccesses, [&](const auto &Access) {
              return Access.Address == P->SourceVA &&
                     Access.OpSeq == int(P->Clause) &&
                     Access.EndAddress == P->AuxVA &&
                     uint32_t(Access.AccessKind) == P->Flags;
            });
        if (P->Region || Call->getNumOperandBundles() != 1 ||
            Found == States.ChainAccesses.end() ||
            !Chain.emplace(P->SourceVA, P->Clause).second)
          return rejectIR(
              "chain replacement does not match its source occurrence");
        if (Found->AccessKind ==
            RegistrationChainAccess::Kind::ReadPreviousHead) {
          const auto *Head =
              llvm::dyn_cast_or_null<llvm::LoadInst>(next(*Call));
          const auto *Pointer =
              Head ? llvm::dyn_cast_or_null<llvm::IntToPtrInst>(next(*Head))
                   : nullptr;
          const auto *Previous =
              Pointer ? llvm::dyn_cast_or_null<llvm::LoadInst>(next(*Pointer))
                      : nullptr;
          if (!Head ||
              Head->getPointerOperand()->getType()->getPointerAddressSpace() !=
                  257 ||
              !Pointer || Pointer->getOperand(0) != Head ||
              !Head->hasOneUse() || !Pointer->hasOneUse() || !Previous ||
              Previous->getPointerOperand() != Pointer ||
              !Previous->getType()->isIntegerTy(32) ||
              !Previous->isVolatile() || Previous->isAtomic() ||
              Previous->getAlign() != llvm::Align(1))
            return rejectIR("source previous head is not loaded through the "
                            "compiler registration");
          ++ExpectedFSReads;
          ChainProtocolReads.insert(Head);
          ChainProtocolReads.insert(Previous);
        } else if (Found->AccessKind ==
                   RegistrationChainAccess::Kind::ReadInstalledHead) {
          const auto *Address = next(*Call);
          const auto *Value =
              Address
                  ? llvm::dyn_cast_or_null<llvm::PtrToIntInst>(next(*Address))
                  : nullptr;
          if (!Address || !frameAddress(Address, *Frame, Frame->EntrySP - 20) ||
              !Value || !Value->getType()->isIntegerTy(32) ||
              Value->getPointerOperand() != Address)
            return rejectIR("source installed head no longer names its logical "
                            "registration");
        }
        continue;
      }
      if (P->Role == Role::RangeEnterTarget ||
          P->Role == Role::RangeExitTarget) {
        auto &Targets =
            P->Role == Role::RangeEnterTarget ? BlockEntries : BlockExits;
        if (P->Clause || Call->getNumOperandBundles() != 1 ||
            !Targets.emplace(P->Region, BlockTarget{*P, Call}).second)
          return rejectIR("source registration block identity is duplicated");
        if (P->Role == Role::RangeEnterTarget) {
          const auto *End =
              llvm::dyn_cast_or_null<llvm::IntrinsicInst>(next(*Call));
          if (Call != &*Block.getFirstInsertionPt() || !End ||
              End->getIntrinsicID() != llvm::Intrinsic::seh_scope_end ||
              End->arg_size() || End->getNumOperandBundles())
            return rejectIR(
                "inactive block does not establish its source sentinel");
          ScopeBoundaries.insert(End);
        } else if (next(*Call) != Block.getTerminator())
          return rejectIR(
              "source block exit no longer precedes its original terminator");
        continue;
      }
      if (P->Region >= Scopes.size())
        return rejectIR("registration anchor names an absent source scope");
      const auto &Scope = Scopes[P->Region];
      if (P->Role == Role::HandlerTarget) {
        if (Scope.IsFinally || P->SourceVA != Scope.HandlerVA || P->Clause ||
            P->AuxVA || P->Flags || Call->getNumOperandBundles() != 1 ||
            !HandlerTargets.emplace(P->Region, &Block).second)
          return rejectIR(
              "handler continuation does not match the source scope");
        continue;
      }
      if (P->Role == Role::ProtectedInvoke || P->Role == Role::RangeEnter ||
          P->Role == Role::RangeExit) {
        const auto *Invoke =
            llvm::dyn_cast_or_null<llvm::InvokeInst>(next(*Call));
        if (!Invoke || P->AuxVA || P->Flags ||
            Call->getNumOperandBundles() != 1 || !Invokes.insert(Invoke).second)
          return rejectIR("protected source occurrence lost its exact invoke");
        const auto *Callee = Invoke->getCalledFunction();
        if (P->Role == Role::ProtectedInvoke) {
          if (P->Clause || (Callee && Callee->isIntrinsic()) ||
              !matchesSourceOperation(*Invoke, Source.CodeRange.Begin,
                                      P->SourceVA, 2) ||
              !Protected.emplace(P->SourceVA, P->Region).second)
            return rejectIR("protected call has no exact source instruction");
        } else {
          const auto ID = P->Role == Role::RangeEnter
                              ? llvm::Intrinsic::seh_scope_begin
                              : llvm::Intrinsic::seh_scope_end;
          auto &Ranges = P->Role == Role::RangeEnter ? Enters : Exits;
          if (!Callee || Callee->getIntrinsicID() != ID || Invoke->arg_size() ||
              Invoke->getNumOperandBundles() ||
              !Ranges
                   .emplace(RangeKey{P->Region, P->Clause},
                            std::make_pair(P->SourceVA, Invoke))
                   .second)
            return rejectIR("asynchronous scope boundary changed");
          ScopeBoundaries.insert(Invoke);
        }
        continue;
      }
      if (P->Role != Role::RegionDispatch || P->Clause ||
          P->SourceVA != Scope.HandlerVA ||
          P->AuxVA != (Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA) ||
          P->Flags != uint32_t(Scope.IsFinally) ||
          Call->getNumOperandBundles() != 2)
        return rejectIR("source scope dispatch anchor changed");
      auto Bundle = Call->getOperandBundle("funclet");
      const auto *Pad =
          Bundle && Bundle->Inputs.size() == 1
              ? llvm::dyn_cast<llvm::Instruction>(Bundle->Inputs[0])
              : nullptr;
      auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
          Source, Arch::X86, P->Region);
      if (!Pad || Pad->getParent() != &Block || !Token ||
          !exactSemanticToken(*Pad, *Token))
        return rejectIR("dispatch pad lost its source semantic token");
      const auto *Address = next(*Call);
      const auto *Sync =
          Address ? llvm::dyn_cast_or_null<llvm::StoreInst>(next(*Address))
                  : nullptr;
      if (!Address || !frameAddress(Address, *Frame, Frame->EntrySP - 8) ||
          !Sync || Sync->getPointerOperand() != Address ||
          integer(Sync->getValueOperand(), 32) !=
              uint32_t(Scope.EnclosingLevel) ||
          !Sync->isVolatile() || Sync->isAtomic() ||
          Sync->getAlign() != llvm::Align(1))
        return rejectIR(
            "handler dispatch lost its source try-level synchronization");
      ScopeWrites.insert(Sync);
      Dispatch D;
      const auto *Callback =
          CallbackMap->at({Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA,
                           Scope.IsFinally});
      if (Scope.IsFinally) {
        const auto *Cleanup = llvm::dyn_cast<llvm::CleanupPadInst>(Pad);
        D.Cleanup =
            llvm::dyn_cast<llvm::CleanupReturnInst>(Block.getTerminator());
        if (!Cleanup || !D.Cleanup || D.Cleanup->getCleanupPad() != Cleanup)
          return rejectIR("finally dispatch lost its cleanup continuation");
        D.Unwind = &Block;
        unsigned Calls = 0;
        for (const auto &I : Block)
          if (const auto *C = llvm::dyn_cast<llvm::CallInst>(&I);
              C && C->getCalledFunction() == Callback) {
            auto F = C->getOperandBundle("funclet");
            const auto *Frame =
                C->arg_size() == 2
                    ? llvm::dyn_cast<llvm::IntrinsicInst>(C->getArgOperand(1))
                    : nullptr;
            if (C->arg_size() != 2 || integer(C->getArgOperand(0), 8) != 1 ||
                !Frame ||
                Frame->getIntrinsicID() != llvm::Intrinsic::localaddress ||
                !F || F->Inputs.size() != 1 || F->Inputs[0].get() != Pad ||
                ++Calls != 1)
              return rejectIR(
                  "finally callback lost its abnormal parent frame");
            AbnormalFinallyIR.insert(C);
          }
        if (Calls != 1)
          return rejectIR("finally dispatch omits its callback");
      } else {
        const auto *Catch = llvm::dyn_cast<llvm::CatchPadInst>(Pad);
        const auto *Return =
            llvm::dyn_cast<llvm::CatchReturnInst>(Block.getTerminator());
        if (!Catch || Catch->arg_size() != 1 ||
            Catch->getArgOperand(0) != Callback || !Return ||
            Return->getCatchPad() != Catch)
          return rejectIR("filter or handler continuation changed");
        D.Switch = Catch->getCatchSwitch();
        if (D.Switch->getNumHandlers() != 1 ||
            D.Switch->getParentPad() !=
                llvm::ConstantTokenNone::get(Function.getContext()))
          return rejectIR(
              "scope catchswitch has extra handlers or a parent pad");
        D.Unwind = D.Switch->getParent();
        D.Handler = Return->getSuccessor();
        D.Return = Return;
      }
      if (!Dispatches.emplace(P->Region, D).second)
        return rejectIR("source scope dispatch is duplicated");
    }
  if (Chain.size() != States.ChainAccesses.size() ||
      FSReads != ExpectedFSReads || Dispatches.size() != Scopes.size())
    return rejectIR("registration chain or dispatch set is incomplete");
  std::vector<ExceptionAddressRange> ImmutableImageRanges;
  auto SourceTable = coff_loader::getX86RegistrationSEHScopeTableRange(Source);
  if (!SourceTable)
    return rejectIR("source registration scope-table extent is invalid");
  ImmutableImageRanges.push_back(*SourceTable);
  if (Source.Personality == ExceptionPersonality::ExceptHandler4) {
    ImmutableImageRanges.push_back(
        {States.SecurityCookieVA, States.SecurityCookieVA + 4});
  }
  auto ScopeState = coff_registration::bindDynamicSEHState(
      ReplayMed, *Frame, NativeFunctions, ScopeWrites);
  if (!ScopeState)
    return ScopeState.takeError();
  for (const auto &[Region, Dispatch] : Dispatches)
    if (Dispatch.Return &&
        !ScopeState->CatchReturns
             .emplace(Dispatch.Return->getCatchPad(), Dispatch.Return)
             .second)
      return rejectIR("SEH return belongs to multiple dispatches");
  auto SourceSegments = validateSourceSegments(
      Function, ReplayMed, *CallbackMap, NativeFunctions, false, &*ParentExits);
  if (!SourceSegments)
    return SourceSegments.takeError();
  if (!hasCheckedX86RegistrationSEHComputedStores(ReplayMed, Function,
                                                  *Frame->Slot, Frame->EntrySP))
    return rejectIR(
        "computed SEH state store changed its source result or frame");
  if (auto Error = coff_registration::validateSEHRegisterStorage(
          Function, ReplayMed, *CallbackMap))
    return Error;
  auto PhysicalWrites = coff_registration::bindSEHCallbackStateWrites(
      ReplayMed, Function, *SourceSegments, NativeFunctions);
  if (!PhysicalWrites)
    return PhysicalWrites.takeError();
  ScopeState->PhysicalWrites = PhysicalWrites->Writes;
  ScopeState->CallbackWrites = PhysicalWrites->Stores;
  if (llvm::Error Error = coff_registration::validateFramePrivacy(
          Function, CallbackFunctions, Frame->Slot, ExceptionBridges,
          IncomingAccesses, SourceCallerPCWrites, ChainProtocolReads,
          States.SecurityCookieVA, ImmutableImageRanges, nullptr,
          &*ExceptionFields, &*Jumps, &*ScopeState, &*ParentExits,
          &*LocalUnwinds, *ThreadReads))
    return Error;
  auto NormalFinallyIR = coff_registration::bindSEHFinallyCalls(
      ReplayMed, Function, *CallbackMap, *SourceSegments, NativeFunctions);
  if (!NormalFinallyIR)
    return NormalFinallyIR.takeError();
  for (size_t I = 0; I < Scopes.size(); ++I) {
    const auto &D = Dispatches.at(I);
    const auto *Outer = Scopes[I].EnclosingLevel < 0
                            ? nullptr
                            : Dispatches.at(Scopes[I].EnclosingLevel).Unwind;
    const auto *Actual =
        D.Cleanup ? D.Cleanup->getUnwindDest() : D.Switch->getUnwindDest();
    if (Actual != Outer || (!Scopes[I].IsFinally && !HandlerTargets.count(I)))
      return rejectIR("scope enclosing state or handler target changed");
  }
  size_t ActiveRanges = 0, ActiveBlocks = 0;
  const auto CallbackOwners = registrationSEHCallbackOwners(ReplayMed);
  if (!CallbackOwners)
    return rejectIR("source SEH callback ownership is incomplete");
  std::set<const llvm::CallBase *> DynamicCalls;
  std::map<int, const llvm::BasicBlock *> SourceEntries, SourceExits;
  std::map<va_t, uint32_t> ExpectedProtected;
  for (const auto &State : States.Blocks) {
    if (CallbackOwners->count(State.BlockId))
      continue;
    if (State.Unknown)
      return rejectIR("source scope is not determined at a CFG block");
    if (State.levels().size() > 1) {
      const auto Begin = BlockEntries.find(State.BlockId);
      const auto End = BlockExits.find(State.BlockId);
      if (Begin == BlockEntries.end() || End == BlockExits.end() ||
          Begin->second.Identity.SourceVA != State.Range.Begin ||
          Begin->second.Identity.AuxVA != State.Range.End ||
          End->second.Identity.SourceVA != State.Range.End ||
          End->second.Identity.AuxVA != State.Range.Begin ||
          Begin->second.Identity.Flags !=
              uint32_t(x86_registration::DynamicScopeLevel) ||
          End->second.Identity.Flags !=
              uint32_t(x86_registration::DynamicScopeLevel))
        return rejectIR("dynamic source block lost its exact state identity");
      std::map<int32_t, coff_registration::DynamicSEHScopeCase> Cases;
      for (int32_t Level : State.levels()) {
        if (Level < 0) {
          if (Level != *Source.Registration->SeededTryLevel)
            return rejectIR("dynamic source scope has an unknown sentinel");
          continue;
        }
        const RangeKey Key{uint32_t(Level), uint32_t(State.BlockId)};
        if (!Dispatches.count(Level) || !Enters.count(Key) ||
            !Exits.count(Key) || Enters.at(Key).first != State.Range.Begin ||
            Exits.at(Key).first != State.Range.End)
          return rejectIR("dynamic source scope has an incomplete interval");
        Cases.emplace(Level, coff_registration::DynamicSEHScopeCase{
                                 Enters.at(Key).second, Exits.at(Key).second,
                                 Dispatches.at(Level).Unwind});
      }
      if (auto Error = coff_registration::validateDynamicSEHScope(
              State, *Frame, SourceSegments->at(State.BlockId),
              *Begin->second.Anchor, *End->second.Anchor, Cases, DynamicCalls))
        return Error;
      ActiveRanges += Cases.size();
      SourceEntries.emplace(State.BlockId, Begin->second.Anchor->getParent());
      SourceExits.emplace(State.BlockId, End->second.Anchor->getParent());
      continue;
    }
    const int32_t Level = State.levels().empty()
                              ? *Source.Registration->SeededTryLevel
                              : State.levels().front();
    auto Target = BlockExits.find(State.BlockId);
    if (Target == BlockExits.end() ||
        Target->second.Identity.SourceVA != State.Range.End ||
        Target->second.Identity.AuxVA != State.Range.Begin ||
        Target->second.Identity.Flags != uint32_t(Level))
      return rejectIR("source registration block has no exact exit identity");
    SourceExits.emplace(State.BlockId, Target->second.Anchor->getParent());
    const auto &Segment = SourceSegments->at(State.BlockId);
    if (Level < 0) {
      auto Entry = BlockEntries.find(State.BlockId);
      if (Entry == BlockEntries.end() ||
          Entry->second.Identity.SourceVA != State.Range.Begin ||
          Entry->second.Identity.AuxVA != State.Range.End ||
          Entry->second.Identity.Flags != uint32_t(Level) ||
          Entry->second.Anchor->getParent() !=
              Target->second.Anchor->getParent())
        return rejectIR(
            "inactive source block lost its exact sentinel interval");
      if (Segment.Enter->getParent() != Entry->second.Anchor->getParent() ||
          Segment.Exit->getParent() != Target->second.Anchor->getParent() ||
          !Entry->second.Anchor->comesBefore(Segment.Enter) ||
          !Segment.Exit->comesBefore(Target->second.Anchor))
        return rejectIR(
            "inactive source execution segment lost its state interval");
      SourceEntries.emplace(State.BlockId, Entry->second.Anchor->getParent());
      continue;
    }
    ++ActiveRanges;
    ++ActiveBlocks;
    const uint32_t Region = Level;
    const RangeKey Key{Region, uint32_t(State.BlockId)};
    if (!Enters.count(Key) || !Exits.count(Key) ||
        Enters.at(Key).first != State.Range.Begin ||
        Exits.at(Key).first != State.Range.End)
      return rejectIR(
          "native protected interval disagrees with source state flow");
    const auto *Exit = Exits.at(Key).second;
    const auto *Enter = Enters.at(Key).second;
    if (Segment.Enter->getParent() != Enter->getNormalDest() ||
        Segment.Exit->getParent() != Exit->getParent() ||
        !Segment.Exit->comesBefore(Exit))
      return rejectIR(
          "active source execution segment lost its state interval");
    if (Exit->getNormalDest() != Target->second.Anchor->getParent() ||
        Enter->getParent()->size() != 2 ||
        !SourceEntries.emplace(State.BlockId, Enter->getParent()).second)
      return rejectIR("active source block lost its exact boundary targets");
    if (Exit->getUnwindDest() != Dispatches.at(Region).Unwind ||
        Enter->getUnwindDest() != Dispatches.at(Region).Unwind)
      return rejectIR("scope boundary unwinds to a different source state");
    const llvm::BasicBlock *Part = Enter->getNormalDest();
    const llvm::BasicBlock *Previous = Enter->getParent();
    std::set<const llvm::BasicBlock *> Seen;
    auto ValidateActiveCalls = [&](const llvm::BasicBlock &Part) {
      for (const auto &I : Part)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call && !llvm::isa<llvm::IntrinsicInst>(Call) &&
            !llvm::isa<llvm::InvokeInst>(Call))
          return false;
      return true;
    };
    while (Part != Exit->getParent()) {
      if (!ValidateActiveCalls(*Part))
        return rejectIR(
            "active scope has an unanchored call that can reset its "
            "runtime state");
      const auto *Invoke =
          llvm::dyn_cast<llvm::InvokeInst>(Part->getTerminator());
      if (Part->getSinglePredecessor() != Previous ||
          !Seen.insert(Part).second || !Invoke || !Invokes.count(Invoke) ||
          Invoke->getUnwindDest() != Dispatches.at(Region).Unwind)
        return rejectIR("a protected interval bypasses its checked scope exit");
      Previous = Part;
      Part = Invoke->getNormalDest();
    }
    if (!ValidateActiveCalls(*Part))
      return rejectIR("active scope has an unanchored call that can reset its "
                      "runtime state");
    if (Part->getSinglePredecessor() != Previous ||
        Exit->getNormalDest()->getSinglePredecessor() != Part)
      return rejectIR("a protected interval bypasses its checked scope entry");
    for (const auto &B : Replay.Blocks)
      if (B.Id == State.BlockId)
        for (const auto &Op : B.Ops)
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
            if (!ExpectedProtected.emplace(Op.Addr, Region).second)
              return rejectIR("source protected call occurrence is ambiguous");
  }
  if (ActiveRanges != Enters.size() || ActiveRanges != Exits.size() ||
      SourceEntries.size() != SourceExits.size() ||
      SourceExits.size() != BlockExits.size() ||
      SourceEntries.size() - ActiveBlocks != BlockEntries.size() ||
      ExpectedProtected != Protected)
    return rejectIR("native scope ranges or protected call set is incomplete");
  for (size_t I = 0; I < Scopes.size(); ++I) {
    if (Scopes[I].IsFinally)
      continue;
    const RegistrationBlockState *HandlerState = nullptr;
    for (const auto &State : States.Blocks)
      if (State.Range.Begin == Scopes[I].HandlerVA && !State.CallbackOnly)
        HandlerState = &State;
    if (!HandlerState || !SourceEntries.count(HandlerState->BlockId))
      return rejectIR("handler target has no checked source block");
    const auto *Entry = SourceEntries.at(HandlerState->BlockId);
    const llvm::BasicBlock *Body = Entry;
    if (!HandlerState->levels().empty() && HandlerState->levels().front() >= 0)
      Body = Enters
                 .at({uint32_t(HandlerState->levels().front()),
                      uint32_t(HandlerState->BlockId)})
                 .second->getNormalDest();
    // A nested except body remains inside its outer scope. Its catchret must
    // enter the authenticated scope-begin wrapper before reaching that body.
    if (Dispatches.at(I).Handler != Entry || HandlerTargets.at(I) != Body)
      return rejectIR("scope handler target bypasses its checked source entry");
  }
  size_t CopyWork = 0;
  const auto AdjacentExits = getRegistrationSEHAdjacentExits(ReplayMed);
  if (!AdjacentExits)
    return rejectIR("source adjacent finally exit is not proven");
  coff_registration::RegistrationCopySlots CopySlots;
  if (!CopySlots.build(Function, CallbackFunctions, CopyWork))
    return rejectIR("parent copy storage has no closed recovery graph");
  if (auto Error = coff_registration::validateSEHParentExitTargets(
          *ParentExits, SourceEntries))
    return Error;
  std::set<const llvm::InvokeInst *> RetainedScopes;
  std::map<uint32_t, const llvm::BasicBlock *> ScopeUnwinds;
  for (const auto &[Region, Dispatch] : Dispatches)
    ScopeUnwinds.emplace(Region, Dispatch.Unwind);
  for (const auto &Block : Replay.Blocks) {
    auto Entry = SourceEntries.find(Block.Id);
    if (Entry == SourceEntries.end())
      continue;
    if (Block.StartAddr == Source.CodeRange.Begin) {
      auto Retention = coff_registration::validateSEHRetainedScopes(
          ReplayMed, Function, *Entry->second, ScopeUnwinds,
          ParentExits->NormalEntry);
      if (!Retention)
        return Retention.takeError();
      RetainedScopes = *Retention;
      Invokes.insert(Retention->begin(), Retention->end());
    }
    if (Block.StartAddr == Source.CodeRange.Begin &&
        Entry->second != &Function.getEntryBlock()) {
      const auto &Setup = Function.getEntryBlock();
      for (const auto &I : Setup) {
        if (IncomingSetup.count(&I) || ParentExits->Setup.count(&I))
          continue;
        if (I.isTerminator() || llvm::isa<llvm::AllocaInst>(I) ||
            llvm::isa<llvm::GetElementPtrInst>(I) ||
            llvm::isa<llvm::CastInst>(I))
          continue;
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
            Store && !Store->isAtomic() &&
            llvm::isa<llvm::AllocaInst>(Store->getPointerOperand()))
          continue;
        if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
            Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
          continue;
        return rejectIR("frame setup contains an unowned source operation");
      }
    }
    std::set<const llvm::BasicBlock *> Expected;
    const auto Adjacent = AdjacentExits->find(Block.Id);
    if (Adjacent != AdjacentExits->end() &&
        Block.Succs != std::vector<int>{Adjacent->second})
      return rejectIR("source adjacent finally successor changed");
    for (int Successor : Block.Succs) {
      if (Adjacent != AdjacentExits->end())
        continue;
      auto Destination = SourceEntries.find(Successor);
      if (Destination == SourceEntries.end())
        return rejectIR(
            "source normal edge enters an unowned registration block");
      Expected.insert(Destination->second);
    }
    const auto *Terminator = SourceExits.at(Block.Id)->getTerminator();
    std::set<const llvm::BasicBlock *> Actual;
    for (unsigned I = 0; I < Terminator->getNumSuccessors(); ++I) {
      const auto *Next = Terminator->getSuccessor(I);
      if (!Expected.count(Next)) {
        Next = coff_registration::registrationCopyDestination(
            *Terminator->getParent(), *Next, Expected, CopyWork, true,
            &CopySlots);
        if (!Next)
          return rejectIR("SEH source edge gained unproved copy instructions");
      }
      Actual.insert(Next);
    }
    if (Actual != Expected)
      return rejectIR(
          "native normal control flow bypasses a source registration boundary");
  }
  if (AnchoredDynamicCalls != DynamicCalls)
    return rejectIR(
        "dynamic SEH call receipts do not match their scope bodies");
  if (auto Error = coff_registration::validateDynamicSEHCallMarkers(
          NativeFunctions, DynamicCalls))
    return Error;
  for (const auto *NativeFunction : NativeFunctions)
    for (const auto &Block : *NativeFunction)
      for (const auto &I : Block) {
        if (I.getMetadata(
                windows_eh_md::RegistrationScopeRetentionAttachment) &&
            !RetainedScopes.count(llvm::dyn_cast<llvm::InvokeInst>(&I)))
          return rejectIR("scope retention marker has no checked entry edge");
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call && Call->getCalledFunction() &&
            (Call->getCalledFunction()->getIntrinsicID() ==
                 llvm::Intrinsic::seh_scope_begin ||
             Call->getCalledFunction()->getIntrinsicID() ==
                 llvm::Intrinsic::seh_scope_end) &&
            !ScopeBoundaries.count(Call))
          return rejectIR(
              "native parent contains an unanchored SEH scope state change");
      }
  for (const auto &Block : Function)
    if (const auto *Invoke =
            llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
        Invoke && !Invokes.count(Invoke))
      return rejectIR("native parent contains an unanchored exceptional edge");
  for (const auto *NativeFunction : NativeFunctions)
    for (const auto &Block : *NativeFunction)
      for (const auto &I : Block)
        if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
            Call &&
            llvm::is_contained(CallbackFunctions, Call->getCalledFunction()) &&
            !NormalFinallyIR->count(Call) && !AbnormalFinallyIR.count(Call))
          return rejectIR(
              "generated callback has no checked runtime entry protocol");
  LowFunc GeneratedCalleeABI;
  GeneratedCalleeABI.Entry = Source.CodeRange.Begin;
  GeneratedCalleeABI.ExceptionMetadata = Source;
  GeneratedCalleeABI.Blocks.emplace_back();
  for (const auto *NativeFunction : NativeFunctions)
    for (const auto &Block : *NativeFunction)
      for (const auto &I : Block) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call)
          continue;
        const auto *Callee = Call->getCalledFunction();
        if (Jumps->Calls.count(Call) || LocalUnwinds->Calls.count(Call))
          continue;
        if (Callee && Callee->isIntrinsic())
          continue;
        if (!Callee || Call->getCallingConv() != llvm::CallingConv::C ||
            Callee->getCallingConv() != llvm::CallingConv::C ||
            Callee->hasFnAttribute(llvm::Attribute::Naked))
          return rejectIR(
              "generated callee has an unproved x86 stack cleanup ABI");
        if (!Callee->isDeclaration())
          continue;
        auto Address = rewrite_source::getOriginalVA(*Callee);
        if (!Address)
          return Address.takeError();
        if (!*Address)
          return rejectIR(
              "generated external call has no checked source stack ABI");
        for (const auto &Scope : Source.Registration->Scopes)
          if (**Address == Scope.FilterVA || **Address == Scope.HandlerVA)
            return rejectIR("original callback code has no recovered runtime "
                            "entry protocol");
        LowOp Op;
        Op.Opcode = NdOp::CALL;
        Op.NumInputs = 1;
        Op.Inputs[0] = NdVar::cst(**Address, 4);
        GeneratedCalleeABI.Blocks.front().Ops.push_back(Op);
      }
  std::vector<ExceptionAddressRange> GeneratedCallerPCWrites;
  if (!hasCallerCleanupRegistrationABI(GeneratedCalleeABI, Image,
                                       &GeneratedCallerPCWrites))
    return rejectIR(
        "generated preserved callee has an unproved x86 stack cleanup ABI");
  if (GeneratedCallerPCWrites.size() != SourceCallerPCWrites.size() ||
      !std::equal(GeneratedCallerPCWrites.begin(),
                  GeneratedCallerPCWrites.end(), SourceCallerPCWrites.begin(),
                  [](const auto &A, const auto &B) {
                    return A.Begin == B.Begin && A.End == B.End;
                  }))
    if (auto Error = coff_registration::validateFramePrivacy(
            Function, CallbackFunctions, Frame->Slot, ExceptionBridges,
            IncomingAccesses, GeneratedCallerPCWrites, ChainProtocolReads,
            States.SecurityCookieVA, ImmutableImageRanges, nullptr,
            &*ExceptionFields, &*Jumps, &*ScopeState, &*ParentExits,
            &*LocalUnwinds, *ThreadReads))
      return Error;
  return llvm::Error::success();
#endif
}

} // namespace neverd
