//===- COFFRegistrationSEHJump.cpp - Native nonlocal jump proof -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHJump.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"

#include "llvm/IR/CFG.h"

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHJumpContract>
bindSEHJumps(const MedFunc &Source, const llvm::Function &Parent,
             const RegistrationFrame &Frame,
             const std::set<const llvm::Instruction *> &IncomingSetup,
             const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  using x86_registration::SEHJumpRole;
  const auto Expected = getX86RegistrationSEHJumpCalls(Source);
  if (!Expected)
    return rejectIR("source nonlocal jump has no complete capture contract");
  struct CaptureIR {
    const llvm::AllocaInst *Buffer = nullptr;
    const llvm::StoreInst *State = nullptr;
    const llvm::AllocaInst *CapturedState = nullptr;
    std::set<const llvm::AllocaInst *> Saved;
  };
  std::map<va_t, CaptureIR> Captures;
  for (const auto &[Address, Call] : *Expected)
    if (!Call.Resume)
      Captures.emplace(Address, CaptureIR{});
  std::map<va_t, const llvm::CallBase *> Calls;
  std::map<va_t, const llvm::LoadInst *> Values;
  std::set<const llvm::AllocaInst *> Registers;
  RegistrationSEHJumpContract Result;
  size_t Work = 0;
  std::vector<const llvm::Function *> Functions{&Parent};
  for (const auto &[Key, Callback] : Callbacks)
    Functions.push_back(Callback);
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("native nonlocal jump proof exceeds its work budget");
        const auto *Marker =
            I.getMetadata(windows_eh_md::RegistrationJumpAttachment);
        if (Marker) {
          const auto Address = metadataInteger(*Marker, 1, 64);
          const auto Kind = metadataInteger(*Marker, 2, 8);
          if (Marker->getNumOperands() != 3 ||
              metadataInteger(*Marker, 0, 64) != Source.Entry || !Address ||
              !Kind)
            return rejectIR(
                "native jump state lost its source capture identity");
          if (*Kind == unsigned(SEHJumpRole::ResumeValue)) {
            const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
            if (!Load || !Expected->count(*Address) ||
                !Expected->at(*Address).Resume ||
                !Values.emplace(*Address, Load).second)
              return rejectIR(
                  "native jump value lost its source argument identity");
            continue;
          }
          if (Function != &Parent || !Captures.count(*Address))
            return rejectIR(
                "native jump state lost its source capture identity");
          auto &Capture = Captures.at(*Address);
          if (*Kind == unsigned(SEHJumpRole::StateRestore)) {
            const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
            if (!Store || Capture.State)
              return rejectIR(
                  "native jump state restore is absent or duplicated");
            Capture.State = Store;
          } else {
            const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
            if (!Slot || !Slot->isStaticAlloca() || Slot->getAddressSpace() ||
                Slot->isArrayAllocation())
              return rejectIR(
                  "native jump storage is not a private static slot");
            if (*Kind == unsigned(SEHJumpRole::Buffer)) {
              const auto *Type =
                  llvm::dyn_cast<llvm::ArrayType>(Slot->getAllocatedType());
              if (Capture.Buffer || !Type ||
                  !Type->getElementType()->isIntegerTy(8) ||
                  Type->getNumElements() !=
                      RegistrationSEHJumpCapture::BufferBytes ||
                  Slot->getAlign() < llvm::Align(4))
                return rejectIR(
                    "native jump buffer changed its runtime extent");
              Capture.Buffer = Slot;
            } else if (*Kind == unsigned(SEHJumpRole::CapturedState)) {
              if (Capture.CapturedState ||
                  !Slot->getAllocatedType()->isIntegerTy(32) ||
                  Slot->getAlign() != llvm::Align(4))
                return rejectIR("native capture changed its saved state slot");
              Capture.CapturedState = Slot;
            } else if (*Kind != unsigned(SEHJumpRole::RegisterSnapshot) ||
                       !Capture.Saved.insert(Slot).second)
              return rejectIR("native register snapshot has an unknown role");
          }
        } else if (const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
                   Slot && Function == &Parent && Slot != Frame.Slot &&
                   !IncomingSetup.count(Slot) &&
                   !Slot->getMetadata(
                       windows_eh_md::RegistrationRegisterAttachment))
          // The register-exchange validator independently authenticates every
          // invocation slot and rejects snapshot accesses to that storage.
          Registers.insert(Slot);
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        const auto *Operation =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!Call || !Operation)
          continue;
        const auto Address = metadataInteger(*Operation, 1, 64);
        if (!Address || !Expected->count(*Address))
          continue;
        const auto &ExpectedCall = Expected->at(*Address);
        if (!matchesSourceOperation(I, Source.Entry, *Address, 2) ||
            metadataInteger(*Operation, 2, 32) !=
                uint32_t(ExpectedCall.Operation->OriginSeq) ||
            metadataInteger(*Operation, 3, 32) !=
                uint32_t(ExpectedCall.Block) ||
            !Calls.emplace(*Address, Call).second)
          return rejectIR("native jump call occurrence changed");
      }
  if (Calls.size() != Expected->size())
    return rejectIR("native jump call set is incomplete");
  for (const auto &[Address, Call] : Calls) {
    const auto &ExpectedCall = Expected->at(Address);
    const auto *Callee = Call->getCalledFunction();
    const bool Resume = ExpectedCall.Resume != nullptr;
    const auto &Capture = Captures.at(ExpectedCall.Capture->Address);
    if (!Callee || !Callee->isDeclaration() || !Callee->hasExternalLinkage() ||
        Callee->getName() != (Resume ? "_longjmpex" : "_setjmp3") ||
        Callee->getCallingConv() != llvm::CallingConv::C ||
        Call->getCallingConv() != llvm::CallingConv::C ||
        Callee->arg_size() != 2 || Callee->isVarArg() == Resume ||
        Call->arg_size() != 2 || !Capture.Buffer ||
        !Call->getArgOperand(1)->getType()->isIntegerTy(32) ||
        !Callee->getFunctionType()->getParamType(0)->isPointerTy() ||
        !Callee->getFunctionType()->getParamType(1)->isIntegerTy(32) ||
        Call->getNumOperandBundles() || Call->isMustTailCall() ||
        Call->getMemoryEffects() != llvm::MemoryEffects::unknown() ||
        Call->doesNotThrow() || Call->hasFnAttr(llvm::Attribute::NoCallback) ||
        Call->hasFnAttr(llvm::Attribute::WillReturn) ||
        Call->hasFnAttr(llvm::Attribute::ReturnsTwice) == Resume ||
        Call->doesNotReturn() != Resume ||
        (Resume ? !Call->getType()->isVoidTy()
                : (!Call->getType()->isIntegerTy(32) ||
                   integer(Call->getArgOperand(1), 32) != 0)))
      return rejectIR("native jump changed its runtime or stack cleanup ABI");
    auto Identity = rewrite_source::getOriginalVA(*Callee);
    if (!Identity)
      return Identity.takeError();
    if (!*Identity || **Identity != ExpectedCall.Operation->Inputs[0].ConstVal)
      return rejectIR("native jump runtime changed its source import identity");
    const auto Found = Values.find(Address);
    if (auto Error = validateSEHJumpArguments(
            *Call, Found != Values.end() ? Found->second : nullptr,
            ExpectedCall.Resume, Parent, Frame, *Capture.Buffer, Callbacks))
      return Error;
    Result.Calls.insert(Call);
  }
  std::map<va_t, const llvm::AllocaInst *> Buffers;
  for (const auto &[Address, Capture] : Captures)
    Buffers.emplace(Address, Capture.Buffer);
  auto Bound = bindSEHBoundCalls(Source, Parent, Frame, Buffers, Callbacks);
  if (!Bound)
    return Bound.takeError();
  Result.Calls.insert(Bound->Calls.begin(), Bound->Calls.end());
  Result.RuntimeOutputs = std::move(Bound->RuntimeOutputs);
  Result.NoFrameBorrowCalls = std::move(Bound->NoFrameBorrowCalls);
  for (const auto &[Address, Capture] : Captures) {
    const auto *Call = Calls.at(Address);
    const auto &Receipt = *Expected->at(Address).Capture;
    if (auto Error = validateSEHJumpUnwindBridge(Parent, Receipt))
      return Error;
    if (!Capture.Buffer || !Capture.State)
      return rejectIR(
          "native capture omits its buffer or logical state restore");
    if (auto Error = validateSEHJumpBufferUses(Parent, *Capture.Buffer,
                                               Result.Calls, Callbacks))
      return Error;
    std::set<const llvm::Instruction *> Saves, Restores;
    std::set<const llvm::AllocaInst *> SeenRegisters;
    for (const auto *Slot : Capture.Saved) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("native register snapshot exceeds its work budget");
      const llvm::StoreInst *Save = nullptr;
      const llvm::LoadInst *Reload = nullptr;
      for (const auto *User : Slot->users()) {
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
            Store && Store->getPointerOperand() == Slot && !Save)
          Save = Store;
        else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
                 Load && Load->getPointerOperand() == Slot && !Reload)
          Reload = Load;
        else
          return rejectIR("native register snapshot escapes or has extra uses");
      }
      const auto *Freeze =
          Save ? llvm::dyn_cast<llvm::FreezeInst>(Save->getValueOperand())
               : nullptr;
      const auto *Read =
          Freeze ? llvm::dyn_cast<llvm::LoadInst>(Freeze->getOperand(0))
                 : nullptr;
      const auto *Register =
          Read ? llvm::dyn_cast<llvm::AllocaInst>(Read->getPointerOperand())
               : nullptr;
      const auto *Restore =
          Reload && Reload->hasOneUse()
              ? llvm::dyn_cast<llvm::StoreInst>(*Reload->user_begin())
              : nullptr;
      if (!Save || !Reload || !Read || !Restore || !Register ||
          !Registers.count(Register) ||
          !SeenRegisters.insert(Register).second || !Read->hasOneUse() ||
          !Freeze->hasOneUse() || Restore->getValueOperand() != Reload ||
          Restore->getPointerOperand() != Register ||
          Slot->getAllocatedType() != Register->getAllocatedType() ||
          Read->getType() != Register->getAllocatedType() ||
          Reload->getType() != Read->getType() ||
          Read->getAlign() > Register->getAlign() ||
          Restore->getAlign() > Register->getAlign() ||
          Reload->getAlign() > Slot->getAlign() ||
          Save->getAlign() > Slot->getAlign())
        return rejectIR("native jump changed a logical register snapshot");
      for (const auto *Load : {Read, Reload})
        if (!Load->isVolatile() || Load->isAtomic())
          return rejectIR("native register snapshot lost its memory ordering");
      for (const auto *Store : {Save, Restore})
        if (!Store->isVolatile() || Store->isAtomic())
          return rejectIR("native register restore lost its memory ordering");
      Saves.insert({Read, Freeze, Save});
      Restores.insert({Reload, Restore});
    }
    if (SeenRegisters != Registers)
      return rejectIR("native jump omits a logical register snapshot");
    if (auto Error =
            validateSEHJumpState(Expected->at(Address), *Call, *Capture.State,
                                 Frame, Capture.CapturedState, Saves, Restores))
      return Error;
    Result.StateRestores.insert(Capture.State);
    const llvm::Instruction *Before = Call->getPrevNode();
    while (Before && !Saves.empty()) {
      const auto *Anchor = llvm::dyn_cast<llvm::IntrinsicInst>(Before);
      if (!Saves.erase(Before) &&
          (!Anchor || Anchor->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
           !Anchor->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle)))
        return rejectIR("native register save moved across source execution");
      Before = Before->getPrevNode();
    }
    if (!Saves.empty())
      return rejectIR("native jump register saves do not precede capture");
    const auto *Invoke = llvm::dyn_cast<llvm::InvokeInst>(Call);
    const llvm::Instruction *After =
        Invoke ? &Invoke->getNormalDest()->front() : next(*Call);
    while (After && !Restores.empty()) {
      if (!Restores.erase(After))
        return rejectIR(
            "native register restore moved across source execution");
      After = next(*After);
    }
    if (!Restores.empty())
      return rejectIR("native jump register restores do not follow capture");
    const int64_t Begin = int64_t(Frame.EntrySP) - 4 + Receipt.BufferOffset;
    Result.SourceBuffers.emplace_back(
        Begin, Begin + RegistrationSEHJumpCapture::BufferBytes);
  }
  return Result;
}

} // namespace neverd::coff_registration
