//===- RegistrationLocalUnwindBridgeTests.cpp - CRT frame ABI ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify CRT calling conventions and reject mutations of the unwind bridge.

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHLocalUnwind.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/X86/X86RegistrationLocalUnwind.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"

using namespace neverd;

namespace {

class LocalUnwindBridge : public testing::Test {
protected:
  llvm::LLVMContext Context;
  llvm::Module Module{"local-unwind", Context};
  llvm::Function *Runtime = nullptr;
  llvm::Function *Bridge = nullptr;
  RegistrationLocalUnwindContract Source;
  std::string ErrorText;

  void create(bool EH4, bool Indirect = false) {
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S32");
    Source = {Indirect ? 0x405000u : 0x402000u, Indirect,
              EH4 ? RegistrationLocalUnwindContract::Kind::EH4
                  : RegistrationLocalUnwindContract::Kind::EH3,
              0x405000};
    Runtime = llvm::Function::Create(
        x86_registration::localUnwindRuntimeType(Context, Source.Protocol),
        llvm::GlobalValue::ExternalLinkage,
        EH4 ? "_local_unwind4" : "_local_unwind2", Module);
    if (Indirect)
      Runtime->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
    rewrite_source::setOriginalVA(*Runtime, Source.Target);
    auto Created =
        x86_registration::createLocalUnwindBridge(*Runtime, Source.Protocol);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Bridge = *Created;
    auto *Caller = llvm::Function::Create(Bridge->getFunctionType(),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "caller", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Caller));
    llvm::SmallVector<llvm::Value *, 2> Arguments;
    for (auto &Argument : Caller->args())
      Arguments.push_back(&Argument);
    B.CreateCall(Bridge, Arguments);
    B.CreateRetVoid();
  }

  bool valid() {
    auto Error =
        coff_registration::validateSEHLocalUnwindBridge(*Bridge, Source);
    ErrorText.clear();
    if (!Error)
      return true;
    ErrorText = llvm::toString(std::move(Error));
    llvm::raw_string_ostream Stream(ErrorText);
    Bridge->print(Stream);
    return false;
  }

  void optimize() {
    llvm::PassBuilder Passes;
    llvm::LoopAnalysisManager Loops;
    llvm::FunctionAnalysisManager Functions;
    llvm::CGSCCAnalysisManager SCCs;
    llvm::ModuleAnalysisManager Modules;
    Passes.registerModuleAnalyses(Modules);
    Passes.registerCGSCCAnalyses(SCCs);
    Passes.registerFunctionAnalyses(Functions);
    Passes.registerLoopAnalyses(Loops);
    Passes.crossRegisterProxies(Loops, Functions, SCCs, Modules);
    auto Pipeline =
        Passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    Pipeline.run(Module, Modules);
  }
};

TEST_F(LocalUnwindBridge, EH3PreservesAnExceptionalCRTCallThroughOptimization) {
  create(false);
  ASSERT_TRUE(Bridge);
  EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  EXPECT_TRUE(valid()) << ErrorText;
  optimize();
  EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  EXPECT_TRUE(valid()) << ErrorText;
  EXPECT_FALSE(Bridge->doesNotThrow());
}

TEST_F(LocalUnwindBridge,
       EH4PreservesCookieAndImportedRuntimeThroughOptimization) {
  create(true, true);
  ASSERT_TRUE(Bridge);
  EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  EXPECT_TRUE(valid()) << ErrorText;
  optimize();
  EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  EXPECT_TRUE(valid()) << ErrorText;
  EXPECT_FALSE(Bridge->doesNotThrow());
}

TEST_F(LocalUnwindBridge, RefusesChangedFrameAndUnwindProtocols) {
  create(true);
  ASSERT_TRUE(Bridge);
  auto *Call = llvm::cast<llvm::CallInst>(&Bridge->getEntryBlock().front());
  auto *Original = llvm::cast<llvm::InlineAsm>(Call->getCalledOperand());
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    std::string Text = Original->getAsmString().str();
    std::string Constraints = Original->getConstraintString().str();
    if (Mutation == 0)
      Text.replace(Text.find("$$16"), 4, "$$12");
    if (Mutation == 1)
      Text.replace(Text.find("$$-2"), 4, "$$-1");
    if (Mutation == 2)
      Text.replace(Text.find("$$12"), 4, "$$8");
    if (Mutation == 3)
      Constraints.erase(Constraints.find("~{memory},"), 10);
    Call->setCalledOperand(llvm::InlineAsm::get(
        Original->getFunctionType(), Text, Constraints, Mutation != 4,
        Mutation == 5, llvm::InlineAsm::AD_ATT, Mutation != 6));
    EXPECT_FALSE(valid());
    Call->setCalledOperand(Original);
    EXPECT_TRUE(valid());
  }
  Call->setArgOperand(0, Bridge->getArg(1));
  EXPECT_FALSE(valid());
  Call->setArgOperand(0, Bridge->getArg(0));
  Call->setArgOperand(2, Bridge->getArg(0));
  EXPECT_FALSE(valid());
}

TEST_F(LocalUnwindBridge, RefusesAlteredRuntimeIdentityAndEffects) {
  create(false);
  ASSERT_TRUE(Bridge);
  rewrite_source::setOriginalVA(*Runtime, Source.Target + 1);
  EXPECT_FALSE(valid());
  rewrite_source::setOriginalVA(*Runtime, Source.Target);
  Runtime->addFnAttr(llvm::Attribute::NoUnwind);
  EXPECT_FALSE(valid());
  Runtime->removeFnAttr(llvm::Attribute::NoUnwind);
  EXPECT_TRUE(valid());
  Bridge->addFnAttr(llvm::Attribute::NoCallback);
  EXPECT_FALSE(valid());
  Bridge->removeFnAttr(llvm::Attribute::NoCallback);
  Bridge->removeFnAttr(llvm::Attribute::OptimizeNone);
  EXPECT_FALSE(valid());
  Bridge->addFnAttr(llvm::Attribute::OptimizeNone);
  EXPECT_TRUE(valid());
  llvm::IRBuilder<> B(Bridge->getEntryBlock().getTerminator());
  B.CreateCall(Runtime, {Bridge->getArg(0), B.getInt32(-1)});
  EXPECT_FALSE(valid());
}

TEST_F(LocalUnwindBridge, RejectsUnknownDeclarationsAndOccupiedNames) {
  create(false);
  ASSERT_TRUE(Bridge);
  auto Duplicate =
      x86_registration::createLocalUnwindBridge(*Runtime, Source.Protocol);
  EXPECT_FALSE(bool(Duplicate));
  llvm::consumeError(Duplicate.takeError());
  Module.setTargetTriple(llvm::Triple("x86_64-pc-windows-msvc"));
  EXPECT_FALSE(valid());
  auto WrongTarget =
      x86_registration::createLocalUnwindBridge(*Runtime, Source.Protocol);
  EXPECT_FALSE(bool(WrongTarget));
  llvm::consumeError(WrongTarget.takeError());
}

} // namespace
