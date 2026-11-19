//===- WindowsRegistrationFrameTests.cpp - x86 callback frame ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/support/LLVMTargetInitialization.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace neverd;

struct Fixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"registration-frame", Context};
  llvm::Function *Parent = nullptr;
  llvm::BasicBlock *Entry = nullptr;
  llvm::BasicBlock *Filter = nullptr;
  llvm::AllocaInst *Local = nullptr;

  Fixture(bool Variadic = false, unsigned ReturnBits = 32) {
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::IntegerType::get(Context, ReturnBits),
                                {llvm::Type::getInt32Ty(Context)}, Variadic),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *PersonalityTy =
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), {}, true);
    Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
        Module.getOrInsertFunction("_except_handler3", PersonalityTy)
            .getCallee()));
    Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    llvm::IRBuilder<> B(Entry);
    Local = B.CreateAlloca(B.getInt32Ty(), nullptr, "local");
    B.CreateStore(Parent->getArg(0), Local);
    B.CreateRet(llvm::ConstantInt::get(Parent->getReturnType(), 0));
    Filter = llvm::BasicBlock::Create(Context, "filter", Parent);
    llvm::IRBuilder<> F(Filter);
    auto *Result = F.CreateLoad(F.getInt32Ty(), Local);
    F.CreateRet(ReturnBits == 32
                    ? Result
                    : F.CreateZExt(Result, Parent->getReturnType()));
  }

  void clearFilter() {
    while (!Filter->empty())
      Filter->back().eraseFromParent();
  }

  std::string text() {
    std::string Result;
    llvm::raw_string_ostream OS(Result);
    Module.print(OS, nullptr);
    return Result;
  }

  std::vector<llvm::CallInst *> calls(llvm::Function &Function,
                                      llvm::Intrinsic::ID ID) {
    std::vector<llvm::CallInst *> Result;
    for (llvm::BasicBlock &Block : Function)
      for (llvm::Instruction &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
            Call && Call->getIntrinsicID() == ID)
          Result.push_back(Call);
    return Result;
  }
};

llvm::Expected<std::vector<char>> emitObject(llvm::Module &Module) {
  std::string Errors;
  static std::once_flag Once;
  std::call_once(Once, [] { neverd::initializeLLVMCodegenTargets(); });
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(Module.getTargetTriple(), Errors);
  if (!Target)
    return llvm::createStringError(llvm::inconvertibleErrorCode(), Errors);
  llvm::TargetOptions Options;
  std::unique_ptr<llvm::TargetMachine> Machine(Target->createTargetMachine(
      Module.getTargetTriple(), "i686", "", Options, llvm::Reloc::Static));
  if (!Machine)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "could not create i386 target machine");
  Module.setDataLayout(Machine->createDataLayout());
  llvm::SmallVector<char, 0> Bytes;
  llvm::raw_svector_ostream OS(Bytes);
  llvm::legacy::PassManager Passes;
  if (Machine->addPassesToEmitFile(Passes, OS, nullptr,
                                   llvm::CodeGenFileType::ObjectFile))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "could not emit i386 object");
  Passes.run(Module);
  return std::vector<char>(Bytes.begin(), Bytes.end());
}

TEST(WindowsRegistrationFrame, FilterRecoversTheParentsActualLocal) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function &Callback = **Result;
  EXPECT_TRUE(Callback.getReturnType()->isIntegerTy(32));
  EXPECT_EQ(Callback.arg_size(), 0u);
  auto EntryFrames = F.calls(Callback, llvm::Intrinsic::frameaddress);
  ASSERT_EQ(EntryFrames.size(), 1u);
  EXPECT_EQ(llvm::cast<llvm::ConstantInt>(EntryFrames[0]->getArgOperand(0))
                ->getZExtValue(),
            1u);
  auto RecoverFP = F.calls(Callback, llvm::Intrinsic::eh_recoverfp);
  ASSERT_EQ(RecoverFP.size(), 1u);
  EXPECT_EQ(RecoverFP[0]->getArgOperand(0), F.Parent);
  EXPECT_EQ(RecoverFP[0]->getArgOperand(1), EntryFrames[0]);
  auto Recover = F.calls(Callback, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(Recover[0]->getArgOperand(1), RecoverFP[0]);
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  ASSERT_EQ(Escape[0]->arg_size(), 1u);
  EXPECT_EQ(Escape[0]->getArgOperand(0), F.Local);
  std::string Errors;
  llvm::raw_string_ostream OS(Errors);
  EXPECT_FALSE(llvm::verifyModule(F.Module, &OS)) << Errors;
}

TEST(WindowsRegistrationFrame, ProjectsAnExtendedEAXReturnToTheFilterABI) {
  Fixture F(false, 64);
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  for (const auto &Block : **Result)
    if (const auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
      EXPECT_TRUE(Return->getReturnValue()->getType()->isIntegerTy(32));
}

TEST(WindowsRegistrationFrame, RejectsAnUnprovedWideFilterReturnAtomically) {
  Fixture F(false, 64);
  F.clearFilter();
  llvm::IRBuilder<> B(F.Filter);
  B.CreateRet(B.getInt64(uint64_t(1) << 40));
  const auto Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(bool(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, FinallyUsesItsParentFrameArgument) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Finally,
      "finally.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function &Callback = **Result;
  EXPECT_TRUE(Callback.getReturnType()->isVoidTy());
  EXPECT_FALSE(Callback.doesNotReturn());
  ASSERT_EQ(Callback.arg_size(), 2u);
  EXPECT_TRUE(Callback.getArg(0)->getType()->isIntegerTy(8));
  EXPECT_TRUE(Callback.getArg(0)->hasAttribute(llvm::Attribute::ZExt));
  EXPECT_TRUE(F.calls(Callback, llvm::Intrinsic::frameaddress).empty());
  EXPECT_TRUE(F.calls(Callback, llvm::Intrinsic::eh_recoverfp).empty());
  auto Recover = F.calls(Callback, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(Recover[0]->getArgOperand(1), Callback.getArg(1));
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, NonlocalFinallyRequiresAnExactTerminalCall) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    Fixture F;
    F.clearFilter();
    llvm::IRBuilder<> B(F.Filter);
    auto Callee = F.Module.getOrInsertFunction(
        "resume", llvm::FunctionType::get(B.getVoidTy(), false));
    auto *Call = B.CreateCall(Callee);
    if (Mutation != 2)
      Call->addFnAttr(llvm::Attribute::NoReturn);
    if (Mutation == 3)
      B.CreateStore(B.getInt32(4), F.Local);
    B.CreateUnreachable();
    X86RegistrationCallbackRequest Request;
    Request.Entry = F.Filter;
    Request.Blocks = {F.Filter};
    Request.Kind = X86RegistrationCallbackKind::Finally;
    Request.Name = "finally.nonlocal";
    if (Mutation != 1)
      Request.NonReturningCalls.push_back(Call);
    if (Mutation == 4) {
      llvm::IRBuilder<> Entry(F.Entry->getTerminator());
      auto *Other = Entry.CreateCall(Callee);
      Other->addFnAttr(llvm::Attribute::NoReturn);
      Request.NonReturningCalls = {Other};
    }
    if (Mutation == 5)
      Request.NonReturningCalls.push_back(Call);
    ASSERT_FALSE(llvm::verifyModule(F.Module));
    const auto Before = F.text();
    auto Result = outlineX86RegistrationCallbacks(*F.Parent, {Request});
    if (Mutation) {
      ASSERT_FALSE(bool(Result));
      llvm::consumeError(Result.takeError());
      EXPECT_EQ(F.text(), Before);
    } else {
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->size(), 1u);
      EXPECT_TRUE(llvm::isa<llvm::UnreachableInst>(
          Result->front()->back().getTerminator()));
      EXPECT_FALSE(llvm::verifyModule(F.Module));
    }
  }
}

TEST(WindowsRegistrationFrame, PreservesExistingEscapeIndices) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Prior = B.CreateAlloca(B.getInt32Ty(), nullptr, "prior");
  B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                   &F.Module, llvm::Intrinsic::localescape),
               {Prior});
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  ASSERT_EQ(Escape[0]->arg_size(), 2u);
  EXPECT_EQ(Escape[0]->getArgOperand(0), Prior);
  EXPECT_EQ(Escape[0]->getArgOperand(1), F.Local);
  auto Recover = F.calls(**Result, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Recover[0]->getArgOperand(2))
                ->getZExtValue(),
            1u);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, ParentArgumentsAreRecoveredFromEscapedStorage) {
  Fixture F;
  F.clearFilter();
  llvm::IRBuilder<> B(F.Filter);
  B.CreateRet(
      B.CreateAdd(B.CreateLoad(B.getInt32Ty(), F.Local), F.Parent->getArg(0)));
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(F.calls(**Result, llvm::Intrinsic::localrecover).size(), 2u);
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  EXPECT_EQ(Escape[0]->arg_size(), 2u);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame,
     CopiesExceptionPointersIntoTheBoundedSourceCell) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr, "frame");
  X86RegistrationCallbackFrame Spec;
  Spec.SyntheticFrame = Frame;
  Spec.ExceptionPointersOffset = 40;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  bool HasRuntimeOffset = false;
  bool HasSourceOffset = false;
  bool HasPointerStore = false;
  for (llvm::Instruction &Instruction : (*Result)->getEntryBlock()) {
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&Instruction))
      if (auto *Offset =
              llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1))) {
        HasRuntimeOffset |= Offset->getSExtValue() == -20;
        HasSourceOffset |= Offset->getZExtValue() == 40;
      }
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
      HasPointerStore |= Store->getValueOperand()->getType()->isPointerTy();
  }
  EXPECT_TRUE(HasRuntimeOffset);
  EXPECT_TRUE(HasSourceOffset);
  EXPECT_TRUE(HasPointerStore);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, CallbackStackDoesNotOverwriteTheParentESPSlot) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(F.Parent->getArg(0), ESP);
  auto *Cell = FB.CreateIntToPtr(
      FB.CreateSub(FB.CreateLoad(FB.getInt32Ty(), ESP), FB.getInt32(4)),
      FB.getPtrTy());
  FB.CreateStore(FB.getInt32(17), Cell);
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Cell));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.StackBytes = 128;
  Spec.StackPointerOffset = 128;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  for (llvm::CallInst *Escape :
       F.calls(*F.Parent, llvm::Intrinsic::localescape))
    for (llvm::Value *Operand : Escape->args())
      EXPECT_NE(Operand, ESP);
  for (llvm::BasicBlock &Block : **Result)
    for (llvm::Instruction &Instruction : Block)
      for (llvm::Value *Operand : Instruction.operands())
        EXPECT_NE(Operand, ESP);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, InfersScratchBoundsFromTheCompleteUseClosure) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  auto *Copy = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.copy");
  B.CreateStore(B.getInt32(0), ESP);
  B.CreateStore(B.getInt32(0), Copy);
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(F.Parent->getArg(0), ESP);
  FB.CreateStore(
      FB.CreateSub(FB.CreateLoad(FB.getInt32Ty(), ESP), FB.getInt32(36)), Copy);
  auto *Cell =
      FB.CreateIntToPtr(FB.CreateLoad(FB.getInt32Ty(), Copy), FB.getPtrTy());
  FB.CreateStore(FB.getInt32(17), Cell)->setAlignment(llvm::Align(1));
  auto *Read = FB.CreateLoad(FB.getInt32Ty(), Cell);
  Read->setAlignment(llvm::Align(1));
  FB.CreateRet(Read);
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.InferStackBounds = true;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  unsigned StackCount = 0;
  for (llvm::Instruction &Instruction : (*Result)->getEntryBlock())
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        Slot && Slot->getName() == "callback.stack") {
      ++StackCount;
      EXPECT_EQ(llvm::cast<llvm::ArrayType>(Slot->getAllocatedType())
                    ->getNumElements(),
                48u);
    }
  EXPECT_EQ(StackCount, 1u);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame,
     InferredScratchRejectsPositiveESPUsesAtomically) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(F.Parent->getArg(0), ESP);
  FB.CreateRet(
      FB.CreateAdd(FB.CreateLoad(FB.getInt32Ty(), ESP), FB.getInt32(4)));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.InferStackBounds = true;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame,
     RebuildsEntryFrameAddressesFromRecoveredAllocas) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr, "frame");
  auto *FrameEnd = B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(48));
  auto *FrameInt = B.CreatePtrToInt(FrameEnd, B.getInt64Ty());
  auto *FrameLow = B.CreateTrunc(FrameInt, B.getInt32Ty());
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Cell =
      FB.CreateIntToPtr(FB.CreateSub(FrameLow, FB.getInt32(4)), FB.getPtrTy());
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Cell));
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  for (llvm::BasicBlock &Block : **Result)
    for (llvm::Instruction &Instruction : Block)
      for (llvm::Value *Operand : Instruction.operands())
        if (const auto *Definition = llvm::dyn_cast<llvm::Instruction>(Operand))
          EXPECT_EQ(Definition->getFunction(), *Result);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, CodegenEmitsThePE32FilterTableAndFrameRecovery) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function *Callback = *Result;
  auto *Protected = llvm::BasicBlock::Create(F.Context, "protected", F.Parent);
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Parent);
  auto *Dispatch = llvm::BasicBlock::Create(F.Context, "dispatch", F.Parent);
  auto *Pad = llvm::BasicBlock::Create(F.Context, "pad", F.Parent);
  auto *Handler = llvm::BasicBlock::Create(F.Context, "handler", F.Parent);
  auto *PersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(F.Context), {}, true);
  F.Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      F.Module.getOrInsertFunction("_except_handler3", PersonalityTy)
          .getCallee()));
  F.Module.addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  F.Entry->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> Entry(F.Entry);
  Entry.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                         &F.Module, llvm::Intrinsic::seh_try_begin),
                     Protected, Dispatch);
  llvm::IRBuilder<> Body(Protected);
  auto *Value = Body.CreateLoad(Body.getInt32Ty(), F.Local);
  Value->setVolatile(true);
  Body.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                        &F.Module, llvm::Intrinsic::seh_try_end),
                    Normal, Dispatch);
  llvm::IRBuilder<>(Normal).CreateRet(Value);
  llvm::IRBuilder<> DB(Dispatch);
  auto *Switch =
      DB.CreateCatchSwitch(llvm::ConstantTokenNone::get(F.Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PB(Pad);
  auto *Catch = PB.CreateCatchPad(Switch, {Callback});
  PB.CreateCatchRet(Catch, Handler);
  llvm::IRBuilder<> HB(Handler);
  HB.CreateRet(HB.getInt32(42));
  std::string Errors;
  llvm::raw_string_ostream ErrorStream(Errors);
  ASSERT_FALSE(llvm::verifyModule(F.Module, &ErrorStream)) << Errors;
  auto Emitted = emitObject(F.Module);
  ASSERT_TRUE(static_cast<bool>(Emitted))
      << llvm::toString(Emitted.takeError());
  const auto &Bytes = *Emitted;
  auto Object =
      llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
          llvm::StringRef(Bytes.data(), Bytes.size()), "registration.obj"));
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  auto *COFF = llvm::dyn_cast<llvm::object::COFFObjectFile>(Object->get());
  ASSERT_NE(COFF, nullptr);
  EXPECT_EQ(COFF->getMachine(), llvm::COFF::IMAGE_FILE_MACHINE_I386);
  bool HasScope = false;
  for (llvm::object::SectionRef Section : COFF->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(static_cast<bool>(Name)) << llvm::toString(Name.takeError());
    if (*Name != ".xdata")
      continue;
    auto Contents = Section.getContents();
    ASSERT_TRUE(static_cast<bool>(Contents))
        << llvm::toString(Contents.takeError());
    EXPECT_EQ(Contents->size(), 12u);
    unsigned AbsoluteReferences = 0;
    for (llvm::object::RelocationRef Relocation : Section.relocations())
      AbsoluteReferences +=
          Relocation.getType() == llvm::COFF::IMAGE_REL_I386_DIR32;
    EXPECT_EQ(AbsoluteReferences, 2u);
    HasScope = true;
  }
  EXPECT_TRUE(HasScope);
}

TEST(WindowsRegistrationFrame, FinallyTableTargetsTheRuntimeCleanupFunclet) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Finally,
      "finally.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function *Helper = *Result;
  auto *Protected = llvm::BasicBlock::Create(F.Context, "protected", F.Parent);
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Parent);
  auto *Cleanup = llvm::BasicBlock::Create(F.Context, "cleanup", F.Parent);
  F.Module.addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  F.Entry->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> EB(F.Entry);
  EB.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                      &F.Module, llvm::Intrinsic::seh_try_begin),
                  Protected, Cleanup);
  llvm::IRBuilder<> PB(Protected);
  auto *Value = PB.CreateLoad(PB.getInt32Ty(), F.Local);
  Value->setVolatile(true);
  PB.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                      &F.Module, llvm::Intrinsic::seh_try_end),
                  Normal, Cleanup);
  llvm::IRBuilder<> NB(Normal);
  auto *LocalAddress = llvm::Intrinsic::getOrInsertDeclaration(
      &F.Module, llvm::Intrinsic::localaddress);
  NB.CreateCall(Helper, {NB.getInt8(0), NB.CreateCall(LocalAddress)});
  NB.CreateRet(Value);
  llvm::IRBuilder<> CB(Cleanup);
  auto *Pad = CB.CreateCleanupPad(llvm::ConstantTokenNone::get(F.Context), {});
  const llvm::OperandBundleDef Bundle("funclet", Pad);
  CB.CreateCall(Helper, {CB.getInt8(1), CB.CreateCall(LocalAddress)}, {Bundle});
  CB.CreateCleanupRet(Pad, nullptr);
  std::string Errors;
  llvm::raw_string_ostream ES(Errors);
  ASSERT_FALSE(llvm::verifyModule(F.Module, &ES)) << Errors;
  auto Emitted = emitObject(F.Module);
  ASSERT_TRUE(static_cast<bool>(Emitted))
      << llvm::toString(Emitted.takeError());
  const auto &Bytes = *Emitted;
  auto Object = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(llvm::StringRef(Bytes.data(), Bytes.size()),
                            "registration-finally.obj"));
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  bool HasFinally = false;
  for (llvm::object::SectionRef Section : (*Object)->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(static_cast<bool>(Name)) << llvm::toString(Name.takeError());
    if (*Name != ".xdata")
      continue;
    auto Contents = Section.getContents();
    ASSERT_TRUE(static_cast<bool>(Contents))
        << llvm::toString(Contents.takeError());
    ASSERT_EQ(Contents->size(), 12u);
    EXPECT_EQ(Contents->substr(4, 4), llvm::StringRef("\0\0\0\0", 4));
    unsigned Targets = 0;
    for (llvm::object::RelocationRef Relocation : Section.relocations()) {
      EXPECT_EQ(Relocation.getOffset(), 8u);
      EXPECT_EQ(Relocation.getType(), llvm::COFF::IMAGE_REL_I386_DIR32);
      auto Target = Relocation.getSymbol()->getName();
      ASSERT_TRUE(static_cast<bool>(Target))
          << llvm::toString(Target.takeError());
      EXPECT_TRUE(Target->starts_with("?dtor$")) << Target->str();
      EXPECT_FALSE(Target->contains("finally.outlined"));
      ++Targets;
    }
    EXPECT_EQ(Targets, 1u);
    HasFinally = true;
  }
  EXPECT_TRUE(HasFinally);
}

void emitRuntimeFilterFrameProbe(bool EH4, bool GS = false) {
  Fixture F;
  auto *Personality = llvm::cast<llvm::Function>(F.Parent->getPersonalityFn());
  if (EH4) {
    F.Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
        F.Module
            .getOrInsertFunction("_except_handler4",
                                 Personality->getFunctionType())
            .getCallee()));
    Personality->eraseFromParent();
    F.Parent->addFnAttr("llvm.rewrite.win-x86-registration-state");
    if (GS)
      F.Parent->addFnAttr(llvm::Attribute::StackProtectReq);
  } else
    Personality->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
  auto *Counter = new llvm::GlobalVariable(
      F.Module, llvm::Type::getInt32Ty(F.Context), false,
      llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(F.Context), 0), "filters");
  llvm::IRBuilder<> EB(F.Entry->getTerminator());
  auto *Frame = EB.CreateAlloca(llvm::ArrayType::get(EB.getInt8Ty(), 64),
                                nullptr, "source.frame");
#ifdef LLVM_NEVERD_X86_REGISTRATION_GS
  if (GS)
    Frame->setAlignment(llvm::Align(64));
#endif
  auto *EBP = EB.CreateAlloca(EB.getInt32Ty(), nullptr, "ebp.root");
  auto *ESP = EB.CreateAlloca(EB.getInt32Ty(), nullptr, "esp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *FPSeed = FB.CreateStore(F.Parent->getArg(0), EBP);
  auto *SPSeed = FB.CreateStore(F.Parent->getArg(0), ESP);
  auto *FP = FB.CreateLoad(FB.getInt32Ty(), EBP);
  auto *ExceptionCell =
      FB.CreateIntToPtr(FB.CreateSub(FP, FB.getInt32(20)), FB.getPtrTy());
  auto *Pointers = FB.CreateLoad(FB.getPtrTy(), ExceptionCell);
  auto *Record = FB.CreateLoad(FB.getPtrTy(), Pointers);
  auto *Code = FB.CreateLoad(FB.getInt32Ty(), Record);
  auto *SP = FB.CreateLoad(FB.getInt32Ty(), ESP);
  auto *Scratch =
      FB.CreateIntToPtr(FB.CreateSub(SP, FB.getInt32(4)), FB.getPtrTy());
  FB.CreateStore(FB.CreateLoad(FB.getInt32Ty(), F.Local), Scratch);
  auto *ScratchValue = FB.CreateLoad(FB.getInt32Ty(), Scratch);
  auto *Calls = FB.CreateLoad(FB.getInt32Ty(), Counter);
  FB.CreateStore(FB.CreateAdd(Calls, FB.getInt32(1)), Counter);
  auto *Accept = FB.CreateAnd(FB.CreateICmpEQ(Code, FB.getInt32(0xe0421234)),
                              FB.CreateICmpEQ(ScratchValue, FB.getInt32(17)));
  FB.CreateRet(FB.CreateZExt(Accept, FB.getInt32Ty()));
  X86RegistrationRootSeed Seeds[] = {
      {FPSeed, X86RegistrationRootKind::FramePointer},
      {SPSeed, X86RegistrationRootKind::StackPointer}};
  X86RegistrationCallbackFrame Spec;
  Spec.SyntheticFrame = Frame;
  Spec.ExceptionPointersOffset = 28;
  Spec.EstablishedFramePointerOffset = 48;
  Spec.RootSeeds = Seeds;
  Spec.StackBytes = 32;
  Spec.StackPointerOffset = 32;
  auto Outlined = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "runtime.filter", Spec);
  ASSERT_TRUE(static_cast<bool>(Outlined))
      << llvm::toString(Outlined.takeError());
  auto DeclareImport = [&](llvm::StringRef Name, llvm::FunctionType *Type,
                           llvm::CallingConv::ID Convention) {
    auto *Function = llvm::cast<llvm::Function>(
        F.Module.getOrInsertFunction(Name, Type).getCallee());
    Function->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
    Function->setCallingConv(Convention);
    return Function;
  };
  auto *Raise =
      DeclareImport("RaiseException",
                    llvm::FunctionType::get(EB.getVoidTy(),
                                            {EB.getInt32Ty(), EB.getInt32Ty(),
                                             EB.getInt32Ty(), EB.getPtrTy()},
                                            false),
                    llvm::CallingConv::X86_StdCall);
  auto *Exit = DeclareImport(
      "ExitProcess",
      llvm::FunctionType::get(EB.getVoidTy(), {EB.getInt32Ty()}, false),
      llvm::CallingConv::X86_StdCall);
  auto *Printf = DeclareImport(
      "printf", llvm::FunctionType::get(EB.getInt32Ty(), {EB.getPtrTy()}, true),
      llvm::CallingConv::C);
  auto *Protected = llvm::BasicBlock::Create(F.Context, "protected", F.Parent);
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Parent);
  auto *Dispatch = llvm::BasicBlock::Create(F.Context, "dispatch", F.Parent);
  auto *Pad = llvm::BasicBlock::Create(F.Context, "pad", F.Parent);
  auto *Handler = llvm::BasicBlock::Create(F.Context, "handler", F.Parent);
  F.Entry->getTerminator()->eraseFromParent();
  EB.SetInsertPoint(F.Entry);
  EB.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                      &F.Module, llvm::Intrinsic::seh_try_begin),
                  Protected, Dispatch);
  llvm::IRBuilder<> PB(Protected);
  PB.CreateInvoke(Raise, Normal, Dispatch,
                  {PB.getInt32(0xe0421234), PB.getInt32(0), PB.getInt32(0),
                   llvm::ConstantPointerNull::get(PB.getPtrTy())})
      ->setCallingConv(llvm::CallingConv::X86_StdCall);
  llvm::IRBuilder<>(Normal).CreateRet(EB.getInt32(0));
  llvm::IRBuilder<> DB(Dispatch);
  auto *Switch =
      DB.CreateCatchSwitch(llvm::ConstantTokenNone::get(F.Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  auto *Catch = PadBuilder.CreateCatchPad(Switch, {*Outlined});
  PadBuilder.CreateCatchRet(Catch, Handler);
  llvm::IRBuilder<>(Handler).CreateRet(EB.getInt32(42));
  F.Module.addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  auto *Start = llvm::Function::Create(
      llvm::FunctionType::get(EB.getVoidTy(), false),
      llvm::GlobalValue::ExternalLinkage, "mainCRTStartup", F.Module);
#ifdef LLVM_NEVERD_X86_REGISTRATION_GS
  // This fixture's padding is at most 64 bytes. Do not require a CRT stack
  // probing helper for the variable-sized caller allocation below.
  if (GS)
    Start->addFnAttr("no-stack-arg-probe");
#endif
  llvm::IRBuilder<> Main(llvm::BasicBlock::Create(F.Context, "entry", Start));
  llvm::Value *Failures = Main.getInt32(0);
  for (unsigned I = 0; I < 16; ++I) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_GS
    llvm::Value *SavedStack = nullptr;
    llvm::AllocaInst *Padding = nullptr;
    if (GS) {
      SavedStack = Main.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          &F.Module, llvm::Intrinsic::stacksave, {Main.getPtrTy()}));
      auto *Iteration = Main.CreateLoad(Main.getInt32Ty(), Counter);
      Iteration->setVolatile(true);
      auto *Bytes = Main.CreateAdd(Main.CreateMul(Iteration, Main.getInt32(4)),
                                   Main.getInt32(4));
      Padding = Main.CreateAlloca(Main.getInt8Ty(), Bytes, "caller.padding");
      Padding->setAlignment(llvm::Align(4));
      Main.CreateStore(Main.getInt8(I + 1), Padding)->setVolatile(true);
    }
#endif
    auto *Result = Main.CreateCall(F.Parent, {Main.getInt32(17)});
    Failures = Main.CreateAdd(
        Failures, Main.CreateZExt(Main.CreateICmpNE(Result, Main.getInt32(42)),
                                  Main.getInt32Ty()));
#ifdef LLVM_NEVERD_X86_REGISTRATION_GS
    if (GS) {
      auto *Value = Main.CreateLoad(Main.getInt8Ty(), Padding);
      Value->setVolatile(true);
      Failures = Main.CreateAdd(
          Failures,
          Main.CreateZExt(Main.CreateICmpNE(Value, Main.getInt8(I + 1)),
                          Main.getInt32Ty()));
      Main.CreateCall(
          llvm::Intrinsic::getOrInsertDeclaration(
              &F.Module, llvm::Intrinsic::stackrestore, {Main.getPtrTy()}),
          {SavedStack});
    }
#endif
  }
  auto *Count = Main.CreateLoad(Main.getInt32Ty(), Counter);
  auto *Format = Main.CreateGlobalString(
      "neverd-registration-frame: filters=%u failures=%u\n");
  Main.CreateCall(Printf, {Format, Count, Failures});
  auto *Status = Main.CreateOr(
      Failures, Main.CreateZExt(Main.CreateICmpNE(Count, Main.getInt32(16)),
                                Main.getInt32Ty()));
  Main.CreateCall(Exit, {Status})
      ->setCallingConv(llvm::CallingConv::X86_StdCall);
  Main.CreateUnreachable();
  ASSERT_FALSE(llvm::verifyModule(F.Module));
  auto Emitted = emitObject(F.Module);
  ASSERT_TRUE(static_cast<bool>(Emitted))
      << llvm::toString(Emitted.takeError());
  if (const char *Path =
          std::getenv(GS    ? "NEVERD_REGISTRATION_EH4_GS_RUNTIME_OBJECT"
                      : EH4 ? "NEVERD_REGISTRATION_EH4_RUNTIME_OBJECT"
                            : "NEVERD_REGISTRATION_RUNTIME_OBJECT")) {
    std::error_code Error;
    llvm::raw_fd_ostream Output(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Output.write(Emitted->data(), Emitted->size());
    Output.close();
    ASSERT_FALSE(Output.has_error());
  }
}

TEST(WindowsRegistrationFrame, EmitsAnExecutableRuntimeFilterFrameProbe) {
  emitRuntimeFilterFrameProbe(false);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_COOKIES
TEST(WindowsRegistrationFrame, EmitsAnExecutableEH4CookieFrameProbe) {
  emitRuntimeFilterFrameProbe(true);
}

TEST(WindowsRegistrationFrame, EmitsAnExecutableEH4GSCookieFrameProbe) {
  emitRuntimeFilterFrameProbe(true, true);
}
#endif

TEST(WindowsRegistrationFrame, RejectsNonlocalValuesBeforeMutatingTheModule) {
  Fixture F;
  llvm::IRBuilder<> Entry(F.Entry->getTerminator());
  auto *Value = Entry.CreateLoad(Entry.getInt32Ty(), F.Local, "parent.value");
  F.clearFilter();
  llvm::IRBuilder<>(F.Filter).CreateRet(Value);
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("dependencies"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsOrdinaryFlowIntoRuntimeCallback) {
  Fixture F;
  F.Entry->getTerminator()->eraseFromParent();
  llvm::IRBuilder<>(F.Entry).CreateBr(F.Filter);
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("outside"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsAnOutOfBoundsExceptionCellAtomically) {
  Fixture F;
  X86RegistrationCallbackFrame Spec;
  Spec.SyntheticFrame = F.Local;
  Spec.ExceptionPointersOffset = 1;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("outside"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RepositionsEscapeAfterNewStaticAllocas) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *OldEscape = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                     &F.Module, llvm::Intrinsic::localescape),
                                 {F.Local});
  auto *Later = B.CreateAlloca(B.getInt32Ty(), nullptr, "later");
  B.CreateStore(B.getInt32(9), Later);
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Later));
  ASSERT_TRUE(OldEscape->comesBefore(Later));
  ASSERT_FALSE(llvm::verifyModule(F.Module));
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  EXPECT_TRUE(Later->comesBefore(Escape[0]));
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, CapturesArgumentsBeforeTheFirstFaultingCall) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry, F.Entry->getFirstInsertionPt());
  auto Callee = F.Module.getOrInsertFunction(
      "may_fault", llvm::FunctionType::get(B.getVoidTy(), {}, false));
  auto *Fault = B.CreateCall(Callee);
  F.clearFilter();
  llvm::IRBuilder<>(F.Filter).CreateRet(F.Parent->getArg(0));
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  ASSERT_EQ(Escape[0]->arg_size(), 1u);
  auto *Capture = llvm::cast<llvm::AllocaInst>(Escape[0]->getArgOperand(0));
  bool InitializedBeforeFault = false;
  for (llvm::User *User : Capture->users())
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User))
      InitializedBeforeFault |=
          Store->getValueOperand() == F.Parent->getArg(0) &&
          Store->comesBefore(Fault);
  EXPECT_TRUE(InitializedBeforeFault);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame,
     ARejectedLaterCallbackLeavesTheWholeBatchIntact) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Nonlocal = B.CreateLoad(B.getInt32Ty(), F.Local);
  auto *Second = llvm::BasicBlock::Create(F.Context, "second", F.Parent);
  llvm::IRBuilder<>(Second).CreateRet(Nonlocal);
  std::vector<X86RegistrationCallbackRequest> Requests{
      {F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter, "first", {}},
      {Second, {Second}, X86RegistrationCallbackKind::Filter, "second", {}}};
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallbacks(*F.Parent, Requests);
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, BindsSourceEBPToTheRecoveredSyntheticFrame) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr,
                               "synthetic.frame");
  auto *EBP = B.CreateAlloca(B.getInt32Ty(), nullptr, "ebp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(F.Parent->getArg(0), EBP);
  auto *Address = FB.CreateIntToPtr(
      FB.CreateSub(FB.CreateLoad(FB.getInt32Ty(), EBP), FB.getInt32(4)),
      FB.getPtrTy());
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Address));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::FramePointer};
  X86RegistrationCallbackFrame Spec;
  Spec.SyntheticFrame = Frame;
  Spec.EstablishedFramePointerOffset = 48;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  ASSERT_EQ(Escape[0]->arg_size(), 1u);
  EXPECT_EQ(Escape[0]->getArgOperand(0), Frame);
  auto *SourceEBP = (*Result)->getValueSymbolTable()->lookup("source.ebp");
  auto *GEP = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(SourceEBP);
  ASSERT_NE(GEP, nullptr);
  auto Recover = F.calls(**Result, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(GEP->getPointerOperand(), Recover[0]);
  EXPECT_EQ(llvm::cast<llvm::ConstantInt>(GEP->getOperand(1))->getZExtValue(),
            48u);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, RejectsASeedSlotSharedWithTheInterruptedParent) {
  Fixture F;
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(FB.getInt32(0), F.Local);
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), F.Local));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.StackBytes = 32;
  Spec.StackPointerOffset = 32;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("storage"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsPrivateESPAsAnObservableReturnValue) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(FB.getInt32(0), ESP);
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), ESP));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.StackBytes = 32;
  Spec.StackPointerOffset = 32;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("observable"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsAnEntryStackCellRead) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(FB.getInt32(0), ESP);
  auto *Cell =
      FB.CreateIntToPtr(FB.CreateLoad(FB.getInt32Ty(), ESP), FB.getPtrTy());
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Cell));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.StackBytes = 32;
  Spec.StackPointerOffset = 32;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("entry cell"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsUninitializedDispatcherStackContents) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  F.clearFilter();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Seed = FB.CreateStore(FB.getInt32(0), ESP);
  auto *Cell = FB.CreateIntToPtr(
      FB.CreateSub(FB.CreateLoad(FB.getInt32Ty(), ESP), FB.getInt32(4)),
      FB.getPtrTy());
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Cell));
  X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
  X86RegistrationCallbackFrame Spec;
  Spec.RootSeeds = llvm::ArrayRef(Root);
  Spec.StackBytes = 32;
  Spec.StackPointerOffset = 32;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("definitely initialized"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsLossyOrMisalignedPrivateStackAddresses) {
  enum Shape {
    PartialRoot,
    PartialCopy,
    MisalignedAnchor,
    Segment,
    Overaligned,
    IntermediateInBounds,
    IntegerNoWrap
  };
  for (Shape Case : {PartialRoot, PartialCopy, MisalignedAnchor, Segment,
                     Overaligned, IntermediateInBounds, IntegerNoWrap}) {
    SCOPED_TRACE(int(Case));
    Fixture F;
    llvm::IRBuilder<> B(F.Entry->getTerminator());
    auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
    auto *Copy = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.copy");
    F.clearFilter();
    llvm::IRBuilder<> FB(F.Filter);
    auto *Seed = FB.CreateStore(FB.getInt32(0), ESP);
    llvm::Value *SP = FB.CreateLoad(
        Case == PartialRoot ? FB.getInt8Ty() : FB.getInt32Ty(), ESP);
    if (Case == PartialRoot)
      SP = FB.CreateZExt(SP, FB.getInt32Ty());
    if (Case == PartialCopy) {
      FB.CreateStore(SP, Copy);
      SP = FB.CreateZExt(FB.CreateLoad(FB.getInt16Ty(), Copy), FB.getInt32Ty());
    }
    llvm::Value *Cell = nullptr;
    if (Case == IntermediateInBounds) {
      auto *Top = FB.CreateIntToPtr(SP, FB.getPtrTy());
      auto *Above = FB.CreateInBoundsGEP(FB.getInt8Ty(), Top, FB.getInt32(4));
      Cell = FB.CreateInBoundsGEP(FB.getInt8Ty(), Above, FB.getInt32(-8));
    } else {
      auto *Displaced = FB.CreateSub(SP, FB.getInt32(4));
      if (Case == IntegerNoWrap)
        llvm::cast<llvm::BinaryOperator>(Displaced)->setHasNoSignedWrap();
      Cell = FB.CreateIntToPtr(
          Displaced,
          llvm::PointerType::get(F.Context, Case == Segment ? 257 : 0));
    }
    auto *Store = FB.CreateStore(FB.getInt32(17), Cell);
    auto *Load = FB.CreateLoad(FB.getInt32Ty(), Cell);
    if (Case == Overaligned) {
      Store->setAlignment(llvm::Align(32));
      Load->setAlignment(llvm::Align(32));
    }
    FB.CreateRet(Load);
    ASSERT_FALSE(llvm::verifyModule(F.Module));
    X86RegistrationRootSeed Root{Seed, X86RegistrationRootKind::StackPointer};
    X86RegistrationCallbackFrame Spec;
    Spec.RootSeeds = llvm::ArrayRef(Root);
    Spec.StackBytes = 32;
    Spec.StackPointerOffset = Case == MisalignedAnchor ? 5 : 32;
    const std::string Before = F.text();
    auto Result = outlineX86RegistrationCallback(
        *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
        "filter.outlined", Spec);
    ASSERT_FALSE(static_cast<bool>(Result));
    llvm::consumeError(Result.takeError());
    EXPECT_EQ(F.text(), Before);
  }
}

TEST(WindowsRegistrationFrame, PreservesInstructionSemanticFunctionAttributes) {
  Fixture F;
  F.Parent->addFnAttr(llvm::Attribute::NullPointerIsValid);
  F.Parent->addFnAttr(llvm::Attribute::StrictFP);
  F.Parent->addFnAttr("target-features", "+sse2");
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE((*Result)->hasFnAttribute(llvm::Attribute::NullPointerIsValid));
  EXPECT_TRUE((*Result)->hasFnAttribute(llvm::Attribute::StrictFP));
  EXPECT_EQ((*Result)->getFnAttribute("target-features").getValueAsString(),
            "+sse2");
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, RejectsASignatureDependentVarargsIntrinsic) {
  Fixture F(true);
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *List = B.CreateAlloca(B.getPtrTy(), nullptr, "va.list");
  llvm::IRBuilder<> FB(F.Filter->getTerminator());
  FB.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                    &F.Module, llvm::Intrinsic::vastart, {FB.getPtrTy()}),
                {List});
  ASSERT_FALSE(llvm::verifyModule(F.Module));
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame,
     RejectsResumeBeforeRemovingTheParentPersonality) {
  Fixture F;
  auto *Return = llvm::BasicBlock::Create(F.Context, "return", F.Parent);
  auto *Resume = llvm::BasicBlock::Create(F.Context, "resume", F.Parent);
  F.clearFilter();
  llvm::IRBuilder<> B(F.Filter);
  B.CreateCondBr(B.CreateICmpNE(F.Parent->getArg(0), B.getInt32(0)), Return,
                 Resume);
  llvm::IRBuilder<>(Return).CreateRet(B.getInt32(1));
  auto *ExceptionTy = llvm::StructType::get(B.getPtrTy(), B.getInt32Ty());
  llvm::IRBuilder<>(Resume).CreateResume(
      llvm::ConstantAggregateZero::get(ExceptionTy));
  ASSERT_FALSE(llvm::verifyModule(F.Module));
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter, Return, Resume},
      X86RegistrationCallbackKind::Filter, "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsBlockAddressesHiddenInAGlobal) {
  Fixture F;
  auto *Address = llvm::BlockAddress::get(F.Parent, F.Filter);
  auto *Table = new llvm::GlobalVariable(F.Module, Address->getType(), true,
                                         llvm::GlobalValue::InternalLinkage,
                                         Address, "callback.address");
  llvm::IRBuilder<> B(F.Filter->getTerminator());
  B.CreateLoad(B.getPtrTy(), Table);
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsMustTailBeforeChangingTheSignature) {
  Fixture F;
  F.clearFilter();
  llvm::IRBuilder<> B(F.Filter);
  auto Callee =
      F.Module.getOrInsertFunction("callee", F.Parent->getFunctionType());
  auto *Call = B.CreateCall(Callee, {F.Parent->getArg(0)});
  Call->setTailCallKind(llvm::CallInst::TCK_MustTail);
  B.CreateRet(Call);
  ASSERT_FALSE(llvm::verifyModule(F.Module));
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("ABI context"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsAConflictingRuntimePersonality) {
  Fixture F;
  F.Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      F.Module
          .getOrInsertFunction("__CxxFrameHandler3",
                               llvm::FunctionType::get(
                                   llvm::Type::getInt32Ty(F.Context), {}, true))
          .getCallee()));
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  llvm::consumeError(Result.takeError());
  EXPECT_EQ(F.text(), Before);
}

} // namespace
#include <cstdlib>
