//===- COFFRegistrationSEHJumpArguments.cpp - Jump storage ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationSEHCall.h"
#include "COFFRegistrationSEHJump.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

namespace neverd::coff_registration {
namespace {

const llvm::IntrinsicInst *escape(const llvm::Function &Parent) {
  const llvm::IntrinsicInst *Result = nullptr;
  for (const auto &I : Parent.getEntryBlock())
    if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape) {
      if (Result)
        return nullptr;
      Result = Call;
    }
  return Result;
}

bool recovers(const llvm::IntrinsicInst &Call, const llvm::Function &Parent,
              const llvm::IntrinsicInst *Escape,
              const llvm::AllocaInst &Buffer) {
  const auto Index =
      Call.arg_size() == 3 ? integer(Call.getArgOperand(2), 32) : std::nullopt;
  return Call.getIntrinsicID() == llvm::Intrinsic::localrecover && Escape &&
         Index && Call.getArgOperand(0) == &Parent &&
         *Index < Escape->arg_size() &&
         Escape->getArgOperand(*Index) == &Buffer;
}

} // namespace

llvm::Error validateSEHJumpArguments(
    const llvm::CallBase &Call, const llvm::LoadInst *Value,
    const RegistrationSEHJumpResume *Resume, const llvm::Function &Parent,
    const RegistrationFrame &Frame, const llvm::AllocaInst &Buffer,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  const auto *Storage = Frame.Slot;
  int64_t Base = int64_t(Frame.EntrySP) - 4;
  if (Resume && Resume->EntryVA) {
    const auto Owner = Callbacks.lower_bound({Resume->EntryVA, false});
    const auto *Recover =
        llvm::dyn_cast<llvm::IntrinsicInst>(Call.getArgOperand(0));
    if (Owner == Callbacks.end() || Owner->first.first != Resume->EntryVA ||
        Call.getFunction() != Owner->second || !Recover ||
        !recovers(*Recover, Parent, escape(Parent), Buffer) ||
        Recover->getArgOperand(1) !=
            getX86RegistrationSEHJumpParentFrame(*Owner->second, Parent))
      return rejectIR("callback jump lost its exact parent buffer recovery");
    const auto Stack = getX86RegistrationSEHJumpStack(*Owner->second);
    if (!Stack)
      return rejectIR("callback jump has no checked private stack");
    Storage = Stack->Slot;
    Base = Stack->EntrySP;
  } else if (Call.getFunction() != &Parent || Call.getArgOperand(0) != &Buffer)
    return rejectIR("native jump buffer belongs to another invocation");
  if (!Resume)
    return llvm::Error::success();
  const auto *GEP =
      Value
          ? llvm::dyn_cast<llvm::GetElementPtrInst>(Value->getPointerOperand())
          : nullptr;
  const auto &Layout = Parent.getParent()->getDataLayout();
  llvm::APInt Offset(32, 0);
  const int64_t ExpectedOffset = Base + *Resume->ValueOffset;
  const auto Bytes = Storage->getAllocationSize(Layout);
  if (!Value || Call.getArgOperand(1) != Value || !Value->hasOneUse() ||
      !Value->isVolatile() || Value->isAtomic() ||
      Value->getAlign() != llvm::Align(1) || !GEP ||
      GEP->getPointerOperand() != Storage ||
      !GEP->accumulateConstantOffset(Layout, Offset) || ExpectedOffset < 0 ||
      !Bytes || Bytes->isScalable() ||
      uint64_t(ExpectedOffset) + 4 > Bytes->getFixedValue() ||
      Offset.getZExtValue() != uint64_t(ExpectedOffset) ||
      Value->getParent() != Call.getParent() || !Value->comesBefore(&Call))
    return rejectIR("native jump value changed its original argument slot");
  // Preserve the read at its runtime call. Protected calls carry one
  // separately authenticated provenance anchor between the two.
  const auto *Following = next(*Value);
  if (Following != &Call) {
    const auto *Anchor = llvm::dyn_cast_or_null<llvm::IntrinsicInst>(Following);
    if (!Anchor ||
        (!llvm::isa<llvm::InvokeInst>(Call) &&
         dynamicSEHCall(*Anchor) != &Call) ||
        Anchor->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
        Anchor->arg_size() || Anchor->getNumOperandBundles() != 1 ||
        Anchor->countOperandBundlesOfType(windows_eh_md::ProvenanceBundle) !=
            1 ||
        next(*Anchor) != &Call)
      return rejectIR("native jump value moved across source execution");
  }
  return llvm::Error::success();
}

llvm::Error validateSEHJumpBufferUses(
    const llvm::Function &Parent, const llvm::AllocaInst &Buffer,
    const std::set<const llvm::CallBase *> &Calls,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  const auto *Escape = escape(Parent);
  bool Escaped = false, Recovered = false;
  size_t Work = 0;
  for (const auto *User : Buffer.users()) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return rejectIR("native jump buffer uses exceed the work budget");
    if (User == Escape && !Escaped) {
      Escaped = true;
      continue;
    }
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    if (!Call || !Calls.count(Call) ||
        !llvm::any_of(Call->args(), [&](const llvm::Use &Argument) {
          return Argument.get() == &Buffer;
        }))
      return rejectIR("native jump buffer escapes its checked invocations");
  }
  for (const auto &[Key, Callback] : Callbacks)
    for (const auto &Block : *Callback)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR(
              "native jump buffer recovery exceeds the work budget");
        const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        if (!Recover || !recovers(*Recover, Parent, Escape, Buffer))
          continue;
        Recovered = true;
        if (Recover->use_empty())
          return rejectIR(
              "native jump buffer has an unowned callback recovery");
        for (const auto *User : Recover->users()) {
          const auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
          if (++Work > limits::kMaxRegistrationEHStateWork || !Call ||
              !Calls.count(Call) || Call->getArgOperand(0) != Recover)
            return rejectIR(
                "recovered jump buffer escapes its checked invocations");
        }
      }
  if (Escaped != Recovered)
    return rejectIR("native jump buffer has no complete callback use closure");
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
