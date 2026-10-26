//===- MedLLVMRegistrationSEHJump.cpp - Native nonlocal jumps -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHJump.h"

#include "../eh/MedLLVMEHHelpers.h"
#include "MedLLVMRegistrationSEHProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace neverd {
namespace {

void createSEHJumpUnwindBridge(llvm::Module &Module, llvm::Function &Runtime,
                               RegistrationSEHJumpCapture::Kind Protocol) {
  using namespace x86_registration;
  const auto Name = getSEHJumpUnwindName(Protocol);
  if (Module.getNamedValue(Name))
    return;
  auto *Bridge =
      llvm::Function::Create(Runtime.getFunctionType(),
                             llvm::GlobalValue::InternalLinkage, Name, Module);
  Bridge->setCallingConv(llvm::CallingConv::X86_StdCall);
  Bridge->addFnAttr(llvm::Attribute::NoInline);
  Bridge->addFnAttr(llvm::Attribute::OptimizeNone);
  llvm::IRBuilder<> B(
      llvm::BasicBlock::Create(Module.getContext(), "entry", Bridge));
  auto *Buffer = Bridge->getArg(0);
  auto *Copy = B.CreateAlloca(llvm::ArrayType::get(
      B.getInt8Ty(), RegistrationSEHJumpCapture::BufferBytes));
  Copy->setAlignment(llvm::Align(4));
  B.CreateMemCpy(Copy, llvm::Align(4), Buffer, llvm::Align(4),
                 RegistrationSEHJumpCapture::BufferBytes);
  // The runtime uses buffer.Ebp when invoking cleanup callbacks. LLVM's SEH
  // entries recover the parent from the virtual EBP just past the chain node.
  // Change only this private copy: longjmp must still restore the captured
  // physical EBP when it resumes the parent.
  auto *Registration = B.CreateLoad(
      B.getPtrTy(), B.CreateInBoundsGEP(B.getInt8Ty(), Buffer, B.getInt32(24)));
  Registration->setAlignment(llvm::Align(4));
  auto *VirtualFrame =
      B.CreateInBoundsGEP(B.getInt8Ty(), Registration, B.getInt32(16));
  B.CreateStore(VirtualFrame, Copy)->setAlignment(llvm::Align(4));
  B.CreateCall(&Runtime, {Copy})
      ->setCallingConv(llvm::CallingConv::X86_StdCall);
  B.CreateRetVoid();
  // The WinEH state pass adds the first use after ordinary IR optimization.
  llvm::appendToCompilerUsed(Module, {Bridge});
}

} // namespace

void MedLLVMEmitter::declareX86RegistrationJumpRuntimes(const MedFunc &Func) {
  const auto Calls = getX86RegistrationSEHJumpCalls(Func);
  const auto Bound = getX86RegistrationSEHBoundCalls(Func);
  if (!Calls || !Bound || (Calls->empty() && Bound->empty()))
    return;
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto *Void = llvm::Type::getVoidTy(*Ctx);
  auto *Ptr = llvm::PointerType::getUnqual(*Ctx);
  auto Declare = [&](va_t Address, llvm::StringRef Name,
                     llvm::FunctionType *Type,
                     llvm::CallingConv::ID Convention =
                         llvm::CallingConv::C) -> llvm::Function * {
    auto *Existing = Mod->getFunction(Name);
    if (Mod->getNamedValue(Name) && (!Existing || !Existing->isDeclaration() ||
                                     Existing->getFunctionType() != Type))
      return nullptr;
    auto *Runtime = llvm::cast<llvm::Function>(
        Mod->getOrInsertFunction(Name, Type).getCallee());
    auto Identity = rewrite_source::getOriginalVA(*Runtime);
    if (!Identity) {
      llvm::consumeError(Identity.takeError());
      return nullptr;
    }
    if (*Identity && **Identity != Address)
      return nullptr;
    rewrite_source::setOriginalVA(*Runtime, Address);
    Runtime->setCallingConv(Convention);
    EmittedFuncNames[Address] = Name.str();
    FuncNames[Address] = Name.str();
    return Runtime;
  };
  for (const auto &[Address, Call] : *Bound) {
    const auto Import = getX86RegistrationSEHBoundImport(*Call.Source);
    auto *Callee =
        Declare(Call.Source->TargetVA,
                Import.empty()
                    ? "__nd_seh_bound_" + llvm::utohexstr(Call.Source->TargetVA)
                    : Import.str(),
                getX86RegistrationSEHBoundType(*Call.Source, *Ctx),
                getX86RegistrationSEHBoundConvention(*Call.Source));
    if (Callee && !Import.empty())
      Callee->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
  }
  for (const auto &[Address, Call] : *Calls) {
    if (Call.Resume)
      Declare(Call.Operation->Inputs[0].ConstVal, "_longjmpex",
              llvm::FunctionType::get(Void, {Ptr, I32}, false));
    else {
      Declare(Call.Capture->TargetVA, "_setjmp3",
              llvm::FunctionType::get(I32, {Ptr, I32}, true));
      if (auto *Unwind = Declare(Call.Capture->UnwindVA,
                                 x86_registration::getSEHSourceJumpUnwindName(
                                     Call.Capture->Protocol),
                                 llvm::FunctionType::get(Void, {Ptr}, false),
                                 llvm::CallingConv::X86_StdCall))
        createSEHJumpUnwindBridge(*Mod, *Unwind, Call.Capture->Protocol);
    }
  }
}

namespace x86_registration {

std::optional<SEHJumpPlan>
prepareSEHJumps(const MedFunc &Source, llvm::Function &Parent,
                llvm::AllocaInst &Frame, uint64_t EntrySP,
                const std::map<const llvm::CallInst *, va_t> &Calls) {
  const auto Expected = getX86RegistrationSEHJumpCalls(Source);
  const auto Bound = getX86RegistrationSEHBoundCalls(Source);
  if (!Expected || !Bound)
    return std::nullopt;
  SEHJumpPlan Result;
  for (const auto &[Call, Address] : Calls) {
    if (const auto Found = Bound->find(Address); Found != Bound->end()) {
      if (!prepareSEHBoundCall(*Call, *Found->second.Source, Frame, EntrySP) ||
          (!Found->second.ObservesScalarReturn &&
           !collectDiscardedRegistrationResult(
               *const_cast<llvm::CallInst *>(Call), Result.DiscardedResults)))
        return std::nullopt;
      Result.BoundCalls.emplace(const_cast<llvm::CallInst *>(Call),
                                Found->second);
    }
    const auto Found = Expected->find(Address);
    if (Found == Expected->end())
      continue;
    const auto &SourceCall = Found->second;
    const bool Resume = SourceCall.Resume != nullptr;
    if (Resume && !SourceCall.Resume->EntryVA) {
      const auto Bytes =
          Frame.getAllocationSize(Parent.getParent()->getDataLayout());
      const int64_t Offset =
          int64_t(EntrySP) - 4 + *SourceCall.Resume->ValueOffset;
      if (!Bytes || Bytes->isScalable() || Offset < 0 ||
          uint64_t(Offset) + 4 > Bytes->getFixedValue())
        return std::nullopt;
    }
    const auto *Callee = Call->getCalledFunction();
    if (!Callee || !Callee->isDeclaration() ||
        Callee->getName() != (Resume ? "_longjmpex" : "_setjmp3") ||
        Call->getCallingConv() != llvm::CallingConv::C ||
        Call->arg_size() < 2 || (Resume && Call->arg_size() != 2) ||
        !Call->getArgOperand(0)->getType()->isPointerTy() ||
        !Call->getArgOperand(1)->getType()->isIntegerTy(32) ||
        (Resume ? !Call->getType()->isVoidTy()
                : !Call->getType()->isIntegerTy(32)) ||
        Call->isMustTailCall() || Call->getNumOperandBundles())
      return std::nullopt;
    Result.Calls.emplace(const_cast<llvm::CallInst *>(Call), SourceCall);
  }
  if (Result.Calls.size() != Expected->size() ||
      Result.BoundCalls.size() != Bound->size())
    return std::nullopt;
  if (Expected->empty())
    return Result;
  size_t Work = 0;
  for (auto &Block : Parent)
    for (auto &I : Block) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
      if (!Slot || Slot == &Frame)
        continue;
      // Ordinary finally inputs/results belong to an invocation, not the
      // logical register state captured by setjmp. Each call initializes its
      // own inputs; runtime entries have a separate unknown-register path.
      if (Slot->getMetadata(windows_eh_md::RegistrationRegisterAttachment))
        continue;
      if (!Slot->isStaticAlloca() || Slot->getAddressSpace() ||
          !Slot->getAllocatedType()->isSingleValueType() ||
          Slot->isArrayAllocation())
        return std::nullopt;
      Result.Registers.push_back(Slot);
    }
  if (Result.Registers.size() >
      limits::kMaxRegistrationEHStateWork / Expected->size() / 8)
    return std::nullopt;
  return Result;
}

std::map<llvm::CallInst *, llvm::CallInst *>
emitSEHJumps(const SEHJumpPlan &Plan, llvm::Function &Parent,
             llvm::AllocaInst &Frame, uint64_t EntrySP, va_t Owner,
             const std::map<va_t, llvm::Function *> &Callbacks) {
  std::map<llvm::CallInst *, llvm::CallInst *> Result;
  llvm::IRBuilder<> Entry(&*Parent.getEntryBlock().getFirstInsertionPt());
  std::map<va_t, llvm::AllocaInst *> Buffers;
  auto Marker = [&](va_t Capture, SEHJumpRole Kind) {
    return llvm::MDNode::get(
        Parent.getContext(),
        {med_llvm_eh::mdUInt(Parent.getContext(), Owner, 64),
         med_llvm_eh::mdUInt(Parent.getContext(), Capture, 64),
         med_llvm_eh::mdUInt(Parent.getContext(), unsigned(Kind), 8)});
  };
  for (const auto &[Call, Source] : Plan.Calls) {
    if (Source.Resume)
      continue;
    auto *Buffer = Entry.CreateAlloca(
        llvm::ArrayType::get(Entry.getInt8Ty(),
                             RegistrationSEHJumpCapture::BufferBytes),
        nullptr, "registration.jump.buffer");
    Buffer->setAlignment(llvm::Align(4));
    Buffer->setMetadata(windows_eh_md::RegistrationJumpAttachment,
                        Marker(Source.Capture->Address, SEHJumpRole::Buffer));
    Buffers.emplace(Source.Capture->Address, Buffer);
  }
  const auto CallbackCalls =
      emitSEHJumpCallbackBindings(Plan, Parent, Buffers, Callbacks);
  for (const auto &[Original, Source] : Plan.Calls) {
    const auto Callback = CallbackCalls.find(Original);
    auto *Call =
        Callback == CallbackCalls.end() ? Original : Callback->second.Call;
    llvm::IRBuilder<> Before(Call);
    llvm::IRBuilder<> After(Call->getNextNode());
    const auto &Capture = *Source.Capture;
    llvm::Value *Buffer = Callback == CallbackCalls.end()
                              ? Buffers.at(Capture.Address)
                              : Callback->second.Buffer;
    llvm::CallInst *Replacement = nullptr;
    if (Source.Resume) {
      auto *Storage =
          Callback == CallbackCalls.end()
              ? &Frame
              : const_cast<llvm::AllocaInst *>(Callback->second.Stack.Slot);
      const auto Base = Callback == CallbackCalls.end()
                            ? EntrySP - 4
                            : Callback->second.Stack.EntrySP;
      auto *Slot = Before.CreateInBoundsGEP(
          Before.getInt8Ty(), Storage,
          Before.getInt32(Base + *Source.Resume->ValueOffset));
      auto *Value = Before.CreateLoad(Before.getInt32Ty(), Slot);
      Value->setAlignment(llvm::Align(1));
      Value->setVolatile(true);
      Value->setMetadata(
          windows_eh_md::RegistrationJumpAttachment,
          Marker(Source.Resume->Address, SEHJumpRole::ResumeValue));
      Replacement =
          Before.CreateCall(Call->getCalledFunction(), {Buffer, Value});
      Replacement->addFnAttr(llvm::Attribute::NoReturn);
    } else {
      // The LLVM x86 EH state pass expands (buffer, 0) with the generated
      // unwind callback and current native state, including its new PC/EBP.
      // Logical SSA spills need their own snapshot across a returns-twice
      // call. Source frame bytes are deliberately not restored: finally
      // callbacks may have modified them during the nonlocal unwind.
      for (auto *Slot : Plan.Registers) {
        auto *Saved = Entry.CreateAlloca(Slot->getAllocatedType(), nullptr,
                                         "registration.jump.saved");
        Saved->setAlignment(Slot->getAlign());
        Saved->setMetadata(
            windows_eh_md::RegistrationJumpAttachment,
            Marker(Capture.Address, SEHJumpRole::RegisterSnapshot));
        auto *Read = Before.CreateLoad(Slot->getAllocatedType(), Slot);
        Read->setVolatile(true);
        Read->setAlignment(Slot->getAlign());
        auto *Save = Before.CreateStore(Before.CreateFreeze(Read), Saved);
        Save->setVolatile(true);
        Save->setAlignment(Saved->getAlign());
        auto *Reload = After.CreateLoad(Slot->getAllocatedType(), Saved);
        Reload->setVolatile(true);
        Reload->setAlignment(Saved->getAlign());
        auto *Restore = After.CreateStore(Reload, Slot);
        Restore->setVolatile(true);
        Restore->setAlignment(Slot->getAlign());
      }
      llvm::Value *Captured = nullptr;
      if (Capture.Levels.size() > 1) {
        auto *Saved = Entry.CreateAlloca(Before.getInt32Ty(), nullptr,
                                         "registration.jump.level");
        Saved->setAlignment(llvm::Align(4));
        Saved->setMetadata(windows_eh_md::RegistrationJumpAttachment,
                           Marker(Capture.Address, SEHJumpRole::CapturedState));
        auto *State = Before.CreateInBoundsGEP(Before.getInt8Ty(), &Frame,
                                               Before.getInt32(EntrySP - 8));
        auto *Read = Before.CreateLoad(Before.getInt32Ty(), State);
        Read->setVolatile(true);
        Read->setAlignment(llvm::Align(1));
        auto *Save = Before.CreateStore(Read, Saved);
        Save->setVolatile(true);
        Save->setAlignment(llvm::Align(4));
        auto *Reload = After.CreateLoad(After.getInt32Ty(), Saved);
        Reload->setVolatile(true);
        Reload->setAlignment(llvm::Align(4));
        Captured = Reload;
      }
      auto *State = After.CreateInBoundsGEP(After.getInt8Ty(), &Frame,
                                            After.getInt32(EntrySP - 8));
      auto *Level = emitSEHJumpState(Source, *Call, *State, Captured, After);
      auto *Restore = After.CreateStore(Level, State);
      Restore->setAlignment(llvm::Align(1));
      Restore->setVolatile(true);
      Restore->setMetadata(windows_eh_md::RegistrationJumpAttachment,
                           Marker(Capture.Address, SEHJumpRole::StateRestore));
      Replacement = Before.CreateCall(Call->getCalledFunction(),
                                      {Buffer, Before.getInt32(0)});
      Replacement->addFnAttr(llvm::Attribute::ReturnsTwice);
    }
    Replacement->copyMetadata(*Call);
    Result.emplace(Call, Replacement);
  }
  auto Bound = emitSEHBoundCalls(Plan, Frame, EntrySP, Buffers);
  Result.insert(Bound.begin(), Bound.end());
  return Result;
}

} // namespace x86_registration
} // namespace neverd
