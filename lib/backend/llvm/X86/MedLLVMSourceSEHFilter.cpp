//===- MedLLVMSourceSEHFilter.cpp - Source filter outlining ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Outline the checked CFG without a runtime frame convention. C passes the
/// actual parent storage and GetExceptionInformation to the resulting body.
//===----------------------------------------------------------------------===//

#include "MedLLVMSourceSEHFilter.h"

#include "neverd/Limits.h"
#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/llvm/LLVMSourceBody.h"
#include "neverd/backend/llvm/LLVMSourceSEHFilter.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

namespace neverd::x86_registration {

void discardSourceFilterResults(llvm::Function &Parent) {
  for (auto &Block : Parent)
    if (auto *Return =
            llvm::dyn_cast_or_null<llvm::ReturnInst>(Block.getTerminator()))
      if (auto *Value = sourceSEHFilterResult(*Return)) {
        Return->getPrevNode()->eraseFromParent();
        if (auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Value))
          llvm::RecursivelyDeleteTriviallyDeadInstructions(Instruction);
      }
}

bool isSourceFilterReturn(const MedFunc &Function, const MedOp &Return) {
  if (Return.NumInputs != 1 || Return.Inputs[0].Size != 4 ||
      !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      !Function.RegistrationStates || !Function.RegistrationStates->Display)
    return false;
  const auto &Facts = *Function.RegistrationStates->Display;
  const auto After = llvm::upper_bound(
      Facts.Stretches, Return.Addr,
      [](va_t Address, const RegistrationDisplayFacts::Stretch &Stretch) {
        return Address < Stretch.Range.Begin;
      });
  if (After == Facts.Stretches.begin())
    return false;
  const auto &Stretch = *std::prev(After);
  if (!Stretch.Owner || !Stretch.Range.contains(Return.Addr))
    return false;
  return llvm::any_of(Function.ExceptionMetadata->Registration->Scopes,
                      [&](const RegistrationScopeRecord &Scope) {
                        return !Scope.IsFinally &&
                               Scope.FilterVA == Stretch.Owner;
                      });
}

llvm::Expected<SourceFilterPlan>
prepareSourceFilter(llvm::Function &Parent,
                    const X86RegistrationCallbackRequest &Request,
                    size_t &WorkUsed) {
  auto Prepared = prepareCallback(Parent, Request, WorkUsed, true);
  if (!Prepared)
    return Prepared.takeError();
  SourceFilterPlan Plan{std::move(*Prepared), {}, {}};
  auto &Callback = Plan.Callback;
  const std::set<const llvm::BasicBlock *> Body(Request.Blocks.begin(),
                                                Request.Blocks.end());
  // An emitter SSA slot used only by this filter belongs to each invocation.
  // Keep its explicit unknown seed; moving an uninitialized value must never
  // turn it into a freely chosen LLVM undef. Shared source memory stays shared.
  for (auto *Slot : Callback.Needed) {
    if (Slot == Request.Frame.SyntheticFrame)
      continue;
    bool Private = true;
    for (const auto *User : Slot->users()) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("source filter capture scan exceeds the work budget");
      const auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (Instruction && Body.count(Instruction->getParent()))
        continue;
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
      if (!Store || Store->getParent() != &Parent.getEntryBlock() ||
          Store->getPointerOperand() != Slot || Store->isVolatile() ||
          Store->isAtomic() ||
          (!llvm::isa<llvm::Constant>(Store->getValueOperand()) &&
           !llvm_source_unknown::isValue(Store->getValueOperand()))) {
        Private = false;
        break;
      }
    }
    if (Private)
      Callback.PrivateSlots.insert(Slot);
  }
  for (auto &Argument : Parent.args())
    if (Callback.Arguments.count(&Argument))
      Plan.Captures.push_back(&Argument);
  for (auto &Instruction : Parent.getEntryBlock()) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
      return reject("source filter entry scan exceeds the work budget");
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        Slot && Callback.Needed.count(Slot) &&
        !Callback.PrivateSlots.count(Slot))
      Plan.Captures.push_back(Slot);
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
        Store && Callback.PrivateSlots.count(llvm::dyn_cast<llvm::AllocaInst>(
                     Store->getPointerOperand())))
      Plan.Initializers.push_back(Store);
  }
  return Plan;
}

SourceFilter commitSourceFilter(llvm::Function &Parent,
                                const SourceFilterPlan &Plan) {
  auto &Context = Parent.getContext();
  auto &Module = *Parent.getParent();
  const auto &Callback = Plan.Callback;
  const auto &Request = *Callback.Request;
  auto *Pointer = llvm::PointerType::get(Context, 0);
  std::vector<llvm::Type *> Types(Plan.Captures.size() + 1, Pointer);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), Types, false),
      llvm::GlobalValue::InternalLinkage, Request.Name, Module);
  Function->addFnAttr(LLVMSourceBody);
  Function->addFnAttr(LLVMSourceSEHFilter);
  for (auto Kind :
       {llvm::Attribute::NullPointerIsValid, llvm::Attribute::StrictFP})
    if (Parent.hasFnAttribute(Kind))
      Function->addFnAttr(Kind);
  for (llvm::StringRef Name : {"target-cpu", "target-features", "tune-cpu",
                               "denormal-fp-math", "denormal-fp-math-f32"})
    if (Parent.hasFnAttribute(Name))
      Function->addFnAttr(Parent.getFnAttribute(Name));
  auto *Setup = llvm::BasicBlock::Create(Context, "filter.setup", Function);
  llvm::IRBuilder<> Builder(Setup);
  llvm::IRBuilder<> ParentBuilder(
      &*Parent.getEntryBlock().getFirstInsertionPt());
  SourceFilter Result{Function, {}};
  llvm::ValueToValueMapTy Values;
  Function->getArg(0)->setName("exception.pointers");
  for (size_t I = 0; I < Plan.Captures.size(); ++I) {
    auto *Argument = Function->getArg(I + 1);
    Argument->setName(Plan.Captures[I]->getName());
    auto *Capture = Plan.Captures[I];
    if (llvm::isa<llvm::Argument>(Capture)) {
      // A catchpad may carry only constants or allocas. Capture the immutable
      // incoming argument in parent storage, which the source helper reads.
      auto *Home = ParentBuilder.CreateAlloca(Capture->getType(), nullptr,
                                              Capture->getName() + ".filter");
      ParentBuilder.CreateStore(Capture, Home);
      Result.Captures.push_back(Home);
      Values[Capture] = Builder.CreateLoad(Capture->getType(), Argument,
                                           Capture->getName() + ".value");
    } else {
      Result.Captures.push_back(Capture);
      Values[Capture] = Argument;
    }
  }
  for (auto &Instruction : Parent.getEntryBlock())
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        Slot && Callback.PrivateSlots.count(Slot)) {
      auto *Copy = Builder.CreateAlloca(Slot->getAllocatedType(),
                                        Slot->getArraySize(), Slot->getName());
      Copy->setAlignment(Slot->getAlign());
      Values[Slot] = Copy;
    }
  for (auto *Store : Plan.Initializers) {
    auto *Value = Store->getValueOperand();
    if (llvm_source_unknown::isValue(Value)) {
      auto *Copy = llvm::cast<llvm::CallInst>(Value)->clone();
      Builder.Insert(Copy, Value->getName());
      Copy->setDebugLoc({});
      Value = Copy;
    }
    auto *Copy = Builder.CreateStore(Value, Values[Store->getPointerOperand()]);
    Copy->setAlignment(Store->getAlign());
  }
  if (Callback.Frame.ExceptionPointersOffset) {
    auto *Cell = Builder.CreateInBoundsGEP(
        Builder.getInt8Ty(), Values[Callback.Frame.SyntheticFrame],
        Builder.getInt32(*Callback.Frame.ExceptionPointersOffset),
        "source.exception.cell");
    Builder.CreateStore(Function->getArg(0), Cell)
        ->setAlignment(llvm::Align(1));
  }
  for (auto *Recipe : Callback.Recipes) {
    auto *Copy = Recipe->clone();
    llvm::RemapInstruction(Copy, Values, llvm::RF_NoModuleLevelChanges);
    Builder.Insert(Copy, Recipe->getName());
    Copy->setDebugLoc({});
    Values[Recipe] = Copy;
  }
  for (auto *Block : Request.Blocks)
    Values[Block] = llvm::CloneBasicBlock(Block, Values, ".filter", Function);
  for (auto *Block : Request.Blocks) {
    auto *Copy = llvm::cast<llvm::BasicBlock>(Values[Block]);
    for (auto &Instruction : *Copy) {
      llvm::RemapInstruction(&Instruction, Values,
                             llvm::RF_NoModuleLevelChanges);
      Instruction.setDebugLoc({});
      Instruction.setMetadata(language_eh_md::InternalSourceCallAttachment,
                              nullptr);
    }
    if (auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(Copy->getTerminator())) {
      auto *Value = filterResult(*Return, true);
      auto *Marker =
          sourceSEHFilterResult(*Return) ? Return->getPrevNode() : nullptr;
      if (Return->getReturnValue() != Value) {
        llvm::IRBuilder<>(Return).CreateRet(Value);
        Return->eraseFromParent();
      }
      if (Marker)
        Marker->eraseFromParent();
    }
  }
  Builder.CreateBr(llvm::cast<llvm::BasicBlock>(Values[Request.Entry]));
  return Result;
}

} // namespace neverd::x86_registration
