//===- MedLLVMRegistrationSEHLocalUnwind.cpp - Native local unwinds -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind full source unwinds before committing the private parent projection.

#include "MedLLVMRegistrationSEHLocalUnwind.h"

#include "MedLLVMRegistrationSEHParentExit.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/X86/X86RegistrationLocalUnwind.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace neverd::x86_registration {
namespace {

llvm::Value *recoveredSlot(llvm::Function &Parent, llvm::Function &Callback,
                           llvm::AllocaInst &Slot) {
  for (auto &I : Parent.getEntryBlock()) {
    auto *Escape = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
    if (!Escape || Escape->getIntrinsicID() != llvm::Intrinsic::localescape)
      continue;
    for (unsigned Index = 0; Index != Escape->arg_size(); ++Index) {
      if (Escape->getArgOperand(Index) != &Slot)
        continue;
      for (auto &R : Callback.getEntryBlock()) {
        auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(&R);
        if (Recover &&
            Recover->getIntrinsicID() == llvm::Intrinsic::localrecover &&
            Recover->getArgOperand(0) == &Parent &&
            Recover->getArgOperand(1) == Callback.getArg(1) &&
            llvm::cast<llvm::ConstantInt>(Recover->getArgOperand(2))
                    ->getZExtValue() == Index)
          return Recover;
      }
    }
  }
  llvm_unreachable("prevalidated local unwind lost its outlined parent slot");
}

} // namespace

std::optional<SEHLocalUnwindPlan> prepareSEHLocalUnwinds(
    const MedFunc &Source, llvm::Function &Parent, llvm::AllocaInst &Frame,
    uint64_t EntrySP, const SEHParentExitPlan &ParentExits,
    const std::map<std::pair<va_t, bool>, size_t> &CallbackIndices,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    const std::map<const llvm::CallInst *, va_t> &Calls,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Memory) {
  const auto Expected = getX86RegistrationSEHLocalUnwinds(Source);
  if (!Expected)
    return std::nullopt;
  SEHLocalUnwindPlan Plan;
  if (Expected->empty())
    return Plan;
  auto &Module = *Parent.getParent();
  const auto &Layout = Module.getDataLayout();
  const auto Extent = Frame.getAllocationSize(Layout);
  if (!Source.Entry || Source.Entry > UINT32_MAX || EntrySP < 20 || !Extent ||
      Extent->isScalable() || EntrySP > Extent->getFixedValue() ||
      Layout.getPointerSize() != 4 || !Frame.isStaticAlloca() ||
      Frame.getParent() != &Parent.getEntryBlock())
    return std::nullopt;
  size_t Work = 0;
  for (const auto &[Address, Bound] : *Expected) {
    const auto &Receipt = *Bound.Source;
    const bool EH4 =
        Receipt.Runtime.Protocol == RegistrationLocalUnwindContract::Kind::EH4;
    const auto Callback = CallbackIndices.find({Receipt.EntryVA, true});
    if (++Work > limits::kMaxRegistrationEHStateWork || !Receipt.EntryVA ||
        Receipt.Bound != uint32_t(EH4 ? -2 : -1) ||
        Callback == CallbackIndices.end() ||
        Callback->second >= Requests.size() ||
        llvm::none_of(ParentExits.Sources, [&](const auto &Exit) {
          return Exit.second.EntryVA == Receipt.EntryVA;
        }))
      return std::nullopt;
    const auto &Request = Requests[Callback->second];
    if (Request.Kind != X86RegistrationCallbackKind::Finally ||
        Request.Frame.SyntheticFrame != &Frame)
      return std::nullopt;
    llvm::CallInst *Call = nullptr;
    for (const auto &[Candidate, VA] : Calls) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (VA == Address) {
        if (Call)
          return std::nullopt;
        Call = const_cast<llvm::CallInst *>(Candidate);
      }
    }
    auto *Runtime = Call ? Call->getCalledFunction() : nullptr;
    if (!Call || Call->getFunction() != &Parent || !Runtime ||
        !llvm::is_contained(Request.Blocks, Call->getParent()) ||
        !Call->getType()->isVoidTy() || !Call->use_empty() ||
        Call->arg_size() != (EH4 ? 3u : 2u) || Call->getNumOperandBundles() ||
        Call->isTailCall() || !Call->getAttributes().isEmpty() ||
        Call->getCallingConv() != llvm::CallingConv::C ||
        !Runtime->isDeclaration() || !Runtime->hasExternalLinkage() ||
        Runtime->getCallingConv() != llvm::CallingConv::C ||
        Runtime->getFunctionType() !=
            localUnwindRuntimeType(Parent.getContext(),
                                   Receipt.Runtime.Protocol) ||
        Runtime->hasDLLImportStorageClass() != Receipt.Runtime.Indirect ||
        !Runtime->getAttributes().isEmpty() ||
        Module.getNamedValue(
            localUnwindBridgeName(Receipt.Runtime.Target, Source.Entry)))
      return std::nullopt;
    auto Identity = rewrite_source::getOriginalVA(*Runtime);
    if (!Identity) {
      llvm::consumeError(Identity.takeError());
      return std::nullopt;
    }
    const auto *Limit =
        llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(EH4 ? 2 : 1));
    int64_t Offset = 0;
    auto *Base = llvm::GetPointerBaseWithConstantOffset(
        Call->getArgOperand(EH4 ? 1 : 0), Offset, Layout);
    if (!*Identity || **Identity != Receipt.Runtime.Target || !Limit ||
        !Limit->getType()->isIntegerTy(32) ||
        Limit->getZExtValue() != Receipt.Bound || Base != &Frame ||
        Offset != int64_t(EntrySP) - 20)
      return std::nullopt;
    if (EH4) {
      const auto *Cookie = Module.getNamedGlobal("__security_cookie");
      if (!Cookie || Call->getArgOperand(0) != Cookie ||
          !Cookie->isDeclaration() || !Cookie->hasExternalLinkage() ||
          !Cookie->getValueType()->isIntegerTy(32) || Cookie->isConstant() ||
          Cookie->isThreadLocal() || Cookie->getAddressSpace() ||
          Cookie->hasDLLImportStorageClass())
        return std::nullopt;
    }
    const auto [It, Added] =
        Plan.Runtimes.emplace(Receipt.Runtime.Target, Runtime);
    if (!Added && It->second != Runtime)
      return std::nullopt;
    Plan.Calls.emplace(Call, SEHLocalUnwindPlan::Call{Bound, Callback->second});
  }
  if (!bindSEHLocalUnwindFrameReads(Plan, Source, EntrySP, ParentExits,
                                    Requests, Memory))
    return std::nullopt;
  return Plan;
}

void emitSEHLocalUnwinds(
    const SEHLocalUnwindPlan &Plan, llvm::Function &Parent,
    llvm::AllocaInst &Frame, uint64_t EntrySP, llvm::AllocaInst &ParentChain,
    va_t Owner, llvm::ArrayRef<llvm::Function *> Callbacks,
    llvm::ArrayRef<X86RegistrationCallbackInstructionMap> Instructions) {
  // The source state solver proved the value loaded from SavedESP on every
  // invocation. Project that value into the synthetic parent allocation; the
  // original read and its execution marker remain for independent replay.
  for (const auto &[Original, Bound] : Plan.FrameReads) {
    auto &Callback = *Callbacks[Bound.Callback];
    auto *Read =
        llvm::cast<llvm::LoadInst>(Instructions[Bound.Callback].at(Original));
    llvm::IRBuilder<> Builder(Read->getNextNode());
    auto *Pointer = Builder.CreateInBoundsGEP(
        Builder.getInt8Ty(), recoveredSlot(Parent, Callback, Frame),
        Builder.getInt32(EntrySP - 4 + Bound.Offset));
    auto *Value = Builder.CreatePtrToInt(Pointer, Builder.getInt32Ty());
    Read->replaceAllUsesWith(Value);
  }
  std::map<va_t, llvm::Function *> Bridges;
  for (const auto &[Original, Bound] : Plan.Calls) {
    const auto &Receipt = *Bound.Source.Source;
    auto *&Bridge = Bridges[Receipt.Runtime.Target];
    if (!Bridge) {
      auto Created =
          createLocalUnwindBridge(*Plan.Runtimes.at(Receipt.Runtime.Target),
                                  Receipt.Runtime.Protocol, Owner);
      if (!Created)
        llvm::report_fatal_error(Created.takeError());
      Bridge = *Created;
    }
    auto &Callback = *Callbacks[Bound.Callback];
    auto *Call =
        llvm::cast<llvm::CallInst>(Instructions[Bound.Callback].at(Original));
    llvm::IRBuilder<> Builder(Call);
    auto *Head = Builder.CreateLoad(
        Builder.getInt32Ty(), recoveredSlot(Parent, Callback, ParentChain));
    Head->setVolatile(true);
    Head->setAlignment(llvm::Align(4));
    llvm::SmallVector<llvm::Value *, 2> Arguments{
        Builder.CreateIntToPtr(Head, Builder.getPtrTy())};
    if (Receipt.Runtime.Protocol == RegistrationLocalUnwindContract::Kind::EH4)
      Arguments.push_back(
          Parent.getParent()->getNamedGlobal("__security_cookie"));
    auto *Replacement = Builder.CreateCall(Bridge, Arguments);
    Replacement->copyMetadata(*Call);
    auto *State = Builder.CreateInBoundsGEP(
        Builder.getInt8Ty(), recoveredSlot(Parent, Callback, Frame),
        Builder.getInt32(EntrySP - 8));
    auto *Store = Builder.CreateStore(Builder.getInt32(Receipt.Bound), State);
    Store->setVolatile(true);
    Store->setAlignment(llvm::Align(1));
    Call->eraseFromParent();
  }
}

} // namespace neverd::x86_registration
