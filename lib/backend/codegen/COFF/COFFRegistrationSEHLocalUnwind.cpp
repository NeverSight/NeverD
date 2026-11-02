//===- COFFRegistrationSEHLocalUnwind.cpp - Local unwind call proof -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently bind native full unwinds to their source callback and state.

#include "COFFRegistrationSEHLocalUnwind.h"

#include "COFFRegistrationCopySlots.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameAddress.h"
#include "neverd/ir/med/X86/X86RegistrationSEHLocalUnwind.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHLocalUnwindContract> bindSEHLocalUnwinds(
    const MedFunc &Source, const llvm::Function &Parent,
    const RegistrationFrame &Frame,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  const auto Expected = getX86RegistrationSEHLocalUnwinds(Source);
  const auto Exits = getRegistrationSEHParentExits(Source);
  if (!Expected || !Exits)
    return rejectIR("local unwind has no complete source invocation proof");
  RegistrationSEHLocalUnwindContract Result;
  if (Expected->empty())
    return Result;
  size_t Work = 0;
  std::vector<const llvm::Function *> CallbackFunctions;
  for (const auto &[Key, Callback] : Callbacks)
    CallbackFunctions.push_back(Callback);
  RegistrationCopySlots Storage;
  if (!Storage.build(Parent, CallbackFunctions, Work))
    return rejectIR("local unwind lost its private parent frame recovery");
  const auto FrameAliases = Storage.aliases(Frame.Slot);
  std::vector<const llvm::Function *> Functions{&Parent};
  Functions.insert(Functions.end(), CallbackFunctions.begin(),
                   CallbackFunctions.end());
  std::set<const llvm::Function *> Bridges;
  std::set<va_t> Seen;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("local unwind binding exceeds its work budget");
        const auto *Marker =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        const auto Address =
            Marker ? metadataInteger(*Marker, 1, 64) : std::nullopt;
        const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
        if (!Call || !Address || !Expected->count(*Address))
          continue;
        const auto &Bound = Expected->at(*Address);
        const auto &Receipt = *Bound.Source;
        const bool EH4 = Receipt.Runtime.Protocol ==
                         RegistrationLocalUnwindContract::Kind::EH4;
        const auto Callback = Callbacks.find({Receipt.EntryVA, true});
        const auto *Bridge = Call ? Call->getCalledFunction() : nullptr;
        if (!Receipt.EntryVA || Receipt.Bound != uint32_t(EH4 ? -2 : -1) ||
            Callback == Callbacks.end() || Callback->second != Function ||
            llvm::none_of(*Exits,
                          [&](const auto &Exit) {
                            return Exit.second.EntryVA == Receipt.EntryVA;
                          }) ||
            !Call || !Bridge || !Call->getType()->isVoidTy() ||
            Call->arg_size() != (EH4 ? 2u : 1u) ||
            Call->getCallingConv() != llvm::CallingConv::C ||
            Call->getNumOperandBundles() || Call->isTailCall() ||
            !Call->getAttributes().isEmpty() ||
            !matchesSourceOperation(I, Source.Entry, *Address, 2) ||
            metadataInteger(*Marker, 2, 32) != uint32_t(Receipt.OpSeq) ||
            metadataInteger(*Marker, 3, 32) != uint32_t(Bound.Block) ||
            !Seen.insert(*Address).second)
          return rejectIR("local unwind changed its source invocation");
        if (auto Error = validateSEHLocalUnwindBridge(*Bridge, Receipt.Runtime))
          return Error;
        const auto *Pointer =
            llvm::dyn_cast<llvm::IntToPtrInst>(Call->getArgOperand(0));
        const auto *Head =
            Pointer ? llvm::dyn_cast<llvm::LoadInst>(Pointer->getOperand(0))
                    : nullptr;
        if (!Head || !Head->getType()->isIntegerTy(32) || !Head->isVolatile() ||
            Head->isAtomic() || Head->getAlign() != llvm::Align(4) ||
            !Head->hasOneUse() || !Pointer->hasOneUse() ||
            next(*Head) != Pointer || next(*Pointer) != Call ||
            Head->getParent() != Call->getParent() ||
            Head->getMetadata(windows_eh_md::RegistrationOperationAttachment))
          return rejectIR("local unwind changed its captured parent chain");
        if (EH4) {
          const auto *Cookie =
              Parent.getParent()->getNamedGlobal("__security_cookie");
          if (!Cookie || Call->getArgOperand(1) != Cookie ||
              !Cookie->isDeclaration() || !Cookie->hasExternalLinkage() ||
              !Cookie->getValueType()->isIntegerTy(32) ||
              Cookie->isConstant() || Cookie->isThreadLocal() ||
              Cookie->getAddressSpace() || Cookie->hasDLLImportStorageClass())
            return rejectIR("local unwind changed its EH4 cookie storage");
        }
        const auto *Slot =
            llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(next(I));
        const auto *State =
            Slot ? llvm::dyn_cast_or_null<llvm::StoreInst>(next(*Slot))
                 : nullptr;
        if (!Slot || !State || Frame.EntrySP < 8 ||
            !llvm::is_contained(FrameAliases, Slot->getPointerOperand()) ||
            registration_frame::checkedByteGEPOffset(Slot) !=
                int64_t(Frame.EntrySP) - 8 ||
            !Slot->hasOneUse() || State->getPointerOperand() != Slot ||
            integer(State->getValueOperand(), 32) != Receipt.Bound ||
            !State->isVolatile() || State->isAtomic() ||
            State->getAlign() != llvm::Align(1) ||
            State->getMetadata(windows_eh_md::RegistrationOperationAttachment))
          return rejectIR("local unwind changed its logical state restore");
        Result.Calls.insert(Call);
        Result.ChainLoads.insert(Head);
        Result.StateRestores.insert(State);
        Bridges.insert(Bridge);
      }
  if (Seen.size() != Expected->size())
    return rejectIR("local unwind invocation set is incomplete");
  const auto *Used = Parent.getParent()->getNamedGlobal("llvm.compiler.used");
  const auto *Kept =
      Used && Used->hasInitializer()
          ? llvm::dyn_cast<llvm::ConstantArray>(Used->getInitializer())
          : nullptr;
  if (!Used || !Kept || !Used->hasAppendingLinkage() ||
      Used->getSection() != "llvm.metadata" || Used->isThreadLocal() ||
      Used->getAddressSpace())
    return rejectIR("local unwind bridge lost its retained machine ABI");
  for (const auto *Bridge : Bridges) {
    if (llvm::none_of(Kept->operands(), [&](const llvm::Use &Value) {
          return Value.get() == Bridge;
        }))
      return rejectIR("local unwind bridge is not retained");
    for (const auto *User : Bridge->users()) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("local unwind bridge uses exceed the work budget");
      const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
      if (User != Kept && (!Call || !Result.Calls.count(Call)))
        return rejectIR("local unwind bridge has an unbound caller");
    }
  }
  return Result;
}

} // namespace neverd::coff_registration
