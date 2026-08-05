//===- HelloWorldPass.cpp - Test pass for LLVM module --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test IR pass that injects a hello_world function.
///
//===----------------------------------------------------------------------===//

#include "neverd/pass/ir/HelloWorldPass.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

// DEBUG_TYPE must be defined *after* the LLVM headers: several of them (e.g.
// CGSCCPassManager.h) define their own DEBUG_TYPE and #undef it at end-of-file,
// which would otherwise wipe out a definition placed before the includes.
#define DEBUG_TYPE "neverd-hello-pass"

namespace neverd {

llvm::PreservedAnalyses HelloWorldPass::run(llvm::Module &M,
                                            llvm::ModuleAnalysisManager &) {
  auto &Ctx = M.getContext();

  auto *I32Ty = llvm::Type::getInt32Ty(Ctx);
  auto *PtrTy = llvm::PointerType::get(Ctx, 0);
  auto *PutsTy = llvm::FunctionType::get(I32Ty, {PtrTy}, false);
  auto *PutsFn = M.getOrInsertFunction("puts", PutsTy).getCallee();

  auto *FuncTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Func = llvm::Function::Create(FuncTy, llvm::Function::ExternalLinkage,
                                      "hello_world", &M);
  auto *EntryBB = llvm::BasicBlock::Create(Ctx, "entry", Func);
  llvm::IRBuilder<> Builder(EntryBB);

  auto *Str = Builder.CreateGlobalString("hello world", ".str.hello");
  Builder.CreateCall(PutsTy, PutsFn, {Str});
  Builder.CreateRet(llvm::ConstantInt::get(I32Ty, 0));

  LLVM_DEBUG(llvm::dbgs() << "pass: injected hello_world() into module\n");
  return llvm::PreservedAnalyses::none();
}

void HelloWorldPass::inject(llvm::Module &M) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  HelloWorldPass HW;
  HW.run(M, MAM);
}

} // namespace neverd
