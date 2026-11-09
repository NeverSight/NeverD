//===- WindowsRegistrationCallbackPlan.cpp - PE32 callback preflight ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowsRegistrationFramePrivate.h"

#include "neverd/Limits.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/llvm/LLVMSourceSEHFilter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/TargetParser/Triple.h"

#include <functional>

namespace neverd::x86_registration {

llvm::Error reject(llvm::StringRef Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "x86 registration callback: %s",
                                 Detail.str().c_str());
}

llvm::Value *filterResult(const llvm::ReturnInst &Return,
                          bool SourceProjection) {
  if (SourceProjection)
    if (auto *Value = sourceSEHFilterResult(Return))
      return Value;
  llvm::Value *Value = Return.getReturnValue();
  if (Value && Value->getType()->isIntegerTy(32))
    return Value;
  const auto *Extension = llvm::dyn_cast_or_null<llvm::CastInst>(Value);
  if (Extension &&
      (Extension->getOpcode() == llvm::Instruction::ZExt ||
       Extension->getOpcode() == llvm::Instruction::SExt) &&
      Extension->getSrcTy()->isIntegerTy(32) &&
      Extension->getDestTy()->isIntegerTy(64))
    return Extension->getOperand(0);
  return nullptr;
}

bool isStaticEntryAlloca(const llvm::AllocaInst &Slot,
                         const llvm::Function &Parent) {
  return Slot.getFunction() == &Parent && Slot.isStaticAlloca() &&
         Slot.getParent() == &Parent.getEntryBlock() &&
         Slot.getAddressSpace() == 0 && !Slot.isSwiftError() &&
         !Slot.isUsedWithInAlloca();
}

namespace {

bool hasBlockAddress(const llvm::Constant *Value,
                     std::set<const llvm::Constant *> &Seen, size_t &WorkUsed,
                     unsigned Depth = 0) {
  if (++WorkUsed > limits::kMaxRegistrationEHStateWork ||
      Depth > ::neverd::limits::backend::
                  kWindowsRegistrationCallbackPlanHasBlockAddressDepthLimit)
    return true;
  if (llvm::isa<llvm::BlockAddress>(Value))
    return true;
  if (!Seen.insert(Value).second)
    return false;
  if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value))
    return Global->hasInitializer() &&
           hasBlockAddress(Global->getInitializer(), Seen, WorkUsed, Depth + 1);
  if (const auto *Alias = llvm::dyn_cast<llvm::GlobalAlias>(Value))
    return hasBlockAddress(Alias->getAliasee(), Seen, WorkUsed, Depth + 1);
  if (llvm::isa<llvm::GlobalValue>(Value))
    return false;
  for (const llvm::Use &Operand : Value->operands())
    if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Operand.get());
        Constant && hasBlockAddress(Constant, Seen, WorkUsed, Depth + 1))
      return true;
  return false;
}

// Reconstruct a pointer into a recovered alloca. Arbitrary parent computations
// and memory reads are values at another execution point, not address recipes.
bool isEntryAddressRecipe(const llvm::Instruction &Instruction,
                          const llvm::Function &Parent) {
  if (Instruction.getParent() != &Parent.getEntryBlock())
    return false;
  if (llvm::isa<llvm::GetElementPtrInst>(Instruction) ||
      llvm::isa<llvm::CastInst>(Instruction))
    return true;
  if (Instruction.getOpcode() != llvm::Instruction::Add &&
      Instruction.getOpcode() != llvm::Instruction::Sub)
    return false;
  return llvm::isa<llvm::ConstantInt>(Instruction.getOperand(1));
}

} // namespace

llvm::Expected<CallbackPlan>
prepareCallback(llvm::Function &Parent,
                const X86RegistrationCallbackRequest &Request, size_t &WorkUsed,
                bool SourceProjection) {
  if (!Request.Entry)
    return reject("callback has no entry");
  llvm::BasicBlock &Entry = *Request.Entry;
  llvm::ArrayRef<llvm::BasicBlock *> Blocks = Request.Blocks;
  const X86RegistrationCallbackKind Kind = Request.Kind;
  if (Kind != X86RegistrationCallbackKind::Filter &&
      Kind != X86RegistrationCallbackKind::Finally)
    return reject("unknown callback kind");
  if (SourceProjection && Kind != X86RegistrationCallbackKind::Filter)
    return reject("source callback is not a filter");
  llvm::StringRef Name = Request.Name;
  X86RegistrationCallbackFrame Frame = Request.Frame;
  llvm::Module *Module = Parent.getParent();
  if (!Module || Parent.empty() || !Parent.getEntryBlock().getTerminator())
    return reject("parent has no complete entry block");
  if (Parent.hasGC())
    return reject("callback depends on a parent GC strategy");
  const llvm::Triple Triple(Module->getTargetTriple());
  const llvm::DataLayout &Layout = Module->getDataLayout();
  if (Triple.getArch() != llvm::Triple::x86 || !Triple.isOSWindows() ||
      !Triple.isOSBinFormatCOFF() || Layout.getPointerSize(0) != 4)
    return reject("requires the PE32 Windows x86 ABI");
  if (Name.empty() || Module->getNamedValue(Name))
    return reject("callback name is empty or already owned");
  const auto *Personality =
      Parent.hasPersonalityFn()
          ? llvm::dyn_cast<llvm::Function>(
                Parent.getPersonalityFn()->stripPointerCasts())
          : nullptr;
  if (!SourceProjection &&
      (!Personality ||
       (Personality->getName() != "_except_handler3" &&
        Personality->getName() != "_except_handler4") ||
       !Personality->isDeclaration() || !Personality->hasExternalLinkage() ||
       Personality->getCallingConv() != llvm::CallingConv::C ||
       !Personality->getReturnType()->isIntegerTy(32) ||
       !Personality->isVarArg() || Personality->arg_size() != 0))
    return reject("parent has no authenticated EH3/EH4 personality");
  if (Blocks.empty() || Blocks.size() > limits::kMaxRegistrationEHStateWork)
    return reject("callback block set is empty or exceeds the work budget");
  std::set<llvm::BasicBlock *> Body;
  for (llvm::BasicBlock *Block : Blocks) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
      return reject("callback block identities exceed the work budget");
    if (!Block || Block->getParent() != &Parent ||
        Block == &Parent.getEntryBlock() || !Block->getTerminator() ||
        !Body.insert(Block).second)
      return reject("callback blocks do not have unique parent identities");
  }
  if (!Body.count(&Entry))
    return reject("callback entry is outside its body");
  for (llvm::BasicBlock *Block : Blocks) {
    if (Block->hasAddressTaken())
      return reject("callback block address escapes its outlined body");
    for (llvm::User *User : Block->users()) {
      auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (!Instruction || !Body.count(Instruction->getParent()))
        return reject("callback block has a user outside its outlined body");
    }
  }
  if (!Entry.phis().empty())
    return reject("callback entry PHIs have no proven runtime incoming value");

  std::set<llvm::BasicBlock *> Reached;
  std::vector<llvm::BasicBlock *> Work{&Entry};
  while (!Work.empty()) {
    llvm::BasicBlock *Block = Work.back();
    Work.pop_back();
    if (!Reached.insert(Block).second)
      continue;
    for (llvm::BasicBlock *Pred : llvm::predecessors(Block))
      if (!Body.count(Pred))
        return reject("ordinary parent flow enters the callback");
    for (llvm::BasicBlock *Succ : llvm::successors(Block)) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback traversal exceeds the work budget");
      if (!Body.count(Succ))
        return reject("callback flow leaves its outlined body");
      Work.push_back(Succ);
    }
  }
  if (Reached != Body)
    return reject("callback contains an unreachable block");

  std::map<llvm::StoreInst *, X86RegistrationRootKind> Seeds;
  std::set<llvm::AllocaInst *> PrivateSlots;
  bool NeedsFP = false;
  bool NeedsSP = false;
  if (Frame.RootSeeds.size() > limits::kMaxRegistrationEHRecords)
    return reject("callback runtime seeds exceed the work budget");
  for (const X86RegistrationRootSeed &Seed : Frame.RootSeeds) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
      return reject("callback runtime seed checks exceed the work budget");
    llvm::StoreInst *Definition = Seed.Definition;
    if (!Definition || Definition->getParent() != &Entry ||
        Definition->isVolatile() || Definition->isAtomic() ||
        !Definition->getValueOperand()->getType()->isIntegerTy(32) ||
        !Seeds.emplace(Definition, Seed.Kind).second)
      return reject(
          "runtime register seed is not a unique entry i32 definition");
    auto *Slot =
        llvm::dyn_cast<llvm::AllocaInst>(Definition->getPointerOperand());
    if (!Slot || !isStaticEntryAlloca(*Slot, Parent) ||
        !Slot->getAllocatedType()->isIntegerTy(32) ||
        !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne())
      return reject("runtime register seed does not define a scalar SSA slot");
    if (!PrivateSlots.insert(Slot).second)
      return reject("runtime register seeds share a destination slot");
    for (llvm::User *User : Slot->users()) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback runtime seed uses exceed the work budget");
      if (User == Definition || isPrivateSlotInitializer(User, *Slot))
        continue;
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
      if (!Load || Load->getType() != Slot->getAllocatedType() ||
          !Body.count(Load->getParent()) || Load->isVolatile() ||
          Load->isAtomic() ||
          (Load->getParent() == &Entry && !Definition->comesBefore(Load)))
        return reject(
            "runtime register seed has unproved storage or dominance");
    }
    switch (Seed.Kind) {
    case X86RegistrationRootKind::FramePointer:
      NeedsFP = true;
      break;
    case X86RegistrationRootKind::StackPointer:
      NeedsSP = true;
      break;
    default:
      return reject("unknown runtime register seed kind");
    }
  }
  if (Frame.InferStackBounds) {
    if (!NeedsSP || Frame.StackBytes || Frame.StackPointerOffset)
      return reject("inferred private stack conflicts with explicit bounds");
    Frame.StackBytes = limits::kMaxRegistrationEHStateWork;
    Frame.StackPointerOffset = Frame.StackBytes;
  }
  if (NeedsSP != (Frame.StackBytes != 0) ||
      NeedsSP != Frame.StackPointerOffset.has_value() ||
      Frame.StackBytes > limits::kMaxRegistrationEHStateWork ||
      (NeedsSP && *Frame.StackPointerOffset > Frame.StackBytes))
    return reject(
        "private callback stack has no proven bounded size and anchor");
  if (NeedsFP != Frame.EstablishedFramePointerOffset.has_value())
    return reject("source EBP seed has no proven synthetic-frame offset");
  const bool HasExceptionCell = Frame.ExceptionPointersOffset.has_value();
  if (HasExceptionCell && Kind != X86RegistrationCallbackKind::Filter)
    return reject("exception-pointer cell is not a filter cell");
  if ((HasExceptionCell || NeedsFP) != (Frame.SyntheticFrame != nullptr))
    return reject("source frame bindings are incomplete");
  if (Frame.SyntheticFrame) {
    llvm::AllocaInst &Slot = *Frame.SyntheticFrame;
    if (!isStaticEntryAlloca(Slot, Parent))
      return reject("synthetic frame is not a shared entry alloca");
    const auto Bytes = Slot.getAllocationSize(Layout);
    if (!Bytes || Bytes->isScalable() || Bytes->getFixedValue() == 0 ||
        Bytes->getFixedValue() > std::numeric_limits<uint32_t>::max())
      return reject("synthetic frame has no bounded PE32 allocation");
    if (HasExceptionCell &&
        (Bytes->getFixedValue() < 4 ||
         *Frame.ExceptionPointersOffset > Bytes->getFixedValue() - 4))
      return reject("exception-pointer cell lies outside the synthetic frame");
    if (NeedsFP &&
        *Frame.EstablishedFramePointerOffset >= Bytes->getFixedValue())
      return reject("source EBP lies outside the synthetic frame");
  }
  if (NeedsSP)
    if (llvm::Error Error = checkPrivateStack(Layout, Entry, Body, Seeds, Frame,
                                              PrivateSlots, WorkUsed))
      return std::move(Error);

  std::set<llvm::AllocaInst *> Needed;
  std::set<llvm::Argument *> Arguments;
  std::vector<llvm::Instruction *> Recipes;
  std::set<llvm::Instruction *> Visiting;
  std::set<llvm::Instruction *> Visited;
  std::set<const llvm::Constant *> Constants;
  bool InvalidDependency = false;
  std::function<void(llvm::Value *, unsigned)> Inspect = [&](llvm::Value *Value,
                                                             unsigned Depth) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork ||
        Depth >
            ::neverd::limits::backend::
                kWindowsRegistrationCallbackPlanPrepareCallbackInspectDepthLimit) {
      InvalidDependency = true;
      return;
    }
    if (auto *Constant = llvm::dyn_cast<llvm::Constant>(Value)) {
      InvalidDependency |= hasBlockAddress(Constant, Constants, WorkUsed);
      return;
    }
    if (auto *Argument = llvm::dyn_cast<llvm::Argument>(Value)) {
      if (Argument->getParent() != &Parent || !Argument->getType()->isSized())
        InvalidDependency = true;
      else
        Arguments.insert(Argument);
      return;
    }
    if (auto *Block = llvm::dyn_cast<llvm::BasicBlock>(Value)) {
      InvalidDependency |= !Body.count(Block);
      return;
    }
    auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Value);
    if (!Instruction) {
      InvalidDependency = true;
      return;
    }
    if (Body.count(Instruction->getParent()))
      return;
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Instruction)) {
      if (!isStaticEntryAlloca(*Slot, Parent))
        InvalidDependency = true;
      else if (!PrivateSlots.count(Slot))
        Needed.insert(Slot);
      return;
    }
    if (Visited.count(Instruction))
      return;
    if (!isEntryAddressRecipe(*Instruction, Parent) ||
        !Visiting.insert(Instruction).second) {
      InvalidDependency = true;
      return;
    }
    for (llvm::Value *Operand : Instruction->operands())
      Inspect(Operand, Depth + 1);
    Visiting.erase(Instruction);
    Visited.insert(Instruction);
    Recipes.push_back(Instruction);
  };

  std::set<const llvm::CallInst *> NonReturningCalls;
  for (auto *Call : Request.NonReturningCalls) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork || !Call ||
        !Body.count(Call->getParent()) || !Call->doesNotReturn() ||
        !NonReturningCalls.insert(Call).second ||
        !llvm::isa<llvm::UnreachableInst>(Call->getParent()->getTerminator()))
      return reject("callback nonlocal exit has no exact terminal call");
    for (auto *Next = Call->getNextNode(); Next && !Next->isTerminator();
         Next = Next->getNextNode()) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback nonlocal exits exceed the work budget");
      const auto *Anchor = llvm::dyn_cast<llvm::IntrinsicInst>(Next);
      if (!Anchor || Anchor->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
          Anchor->arg_size() ||
          !Anchor->getMetadata(windows_eh_md::RegistrationBlockAttachment))
        return reject(
            "callback executes source operations after a nonlocal exit");
    }
  }
  bool HasExit = !NonReturningCalls.empty();
  for (llvm::BasicBlock *Block : Blocks)
    for (llvm::Instruction &Instruction : *Block) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback instructions exceed the work budget");
      if (Instruction.isEHPad() || llvm::isa<llvm::InvokeInst>(Instruction) ||
          llvm::isa<llvm::CallBrInst>(Instruction) ||
          llvm::isa<llvm::IndirectBrInst>(Instruction) ||
          llvm::isa<llvm::ResumeInst>(Instruction) ||
          llvm::isa<llvm::AllocaInst>(Instruction))
        return reject("callback contains an unsupported entry or control form");
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
        const auto *Return =
            llvm::dyn_cast_or_null<llvm::ReturnInst>(Call->getNextNode());
        const bool SourceResult =
            SourceProjection && Return && sourceSEHFilterResult(*Return);
        if ((llvm::isa<llvm::CallInst>(Call) &&
             llvm::cast<llvm::CallInst>(Call)->isMustTailCall()) ||
            Call->isInlineAsm() ||
            (Call->hasOperandBundles() && !SourceResult) ||
            Call->hasFnAttr(llvm::Attribute::ReturnsTwice))
          return reject("callback call depends on its original ABI context");
      }
      if (const auto *Intrinsic =
              llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction))
        switch (Intrinsic->getIntrinsicID()) {
        case llvm::Intrinsic::frameaddress:
        case llvm::Intrinsic::returnaddress:
        case llvm::Intrinsic::localaddress:
        case llvm::Intrinsic::localescape:
        case llvm::Intrinsic::localrecover:
        case llvm::Intrinsic::stacksave:
        case llvm::Intrinsic::stackrestore:
        case llvm::Intrinsic::vastart:
        case llvm::Intrinsic::vaend:
        case llvm::Intrinsic::vacopy:
        case llvm::Intrinsic::experimental_deoptimize:
          return reject(
              "callback intrinsic depends on its original frame or signature");
        default:
          break;
        }
      if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        HasExit = true;
        if (Kind == X86RegistrationCallbackKind::Filter &&
            !filterResult(*Return, SourceProjection))
          return reject("filter does not return an exact i32 result");
      }
      for (llvm::Use &Operand : Instruction.operands()) {
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
            Store && Seeds.count(Store) && Operand.getOperandNo() == 0)
          continue;
        Inspect(Operand.get(), 0);
      }
    }
  if (!HasExit || InvalidDependency)
    return reject("callback return or parent-value dependencies are unproven");
  if (Frame.SyntheticFrame)
    Needed.insert(Frame.SyntheticFrame);
  return CallbackPlan{&Request,
                      Frame,
                      std::move(Needed),
                      std::move(Arguments),
                      std::move(Recipes),
                      std::move(Seeds),
                      std::move(PrivateSlots)};
}

} // namespace neverd::x86_registration
