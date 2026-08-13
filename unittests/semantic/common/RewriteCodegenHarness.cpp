//===- RewriteCodegenHarness.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Definitions for the shared rewrite-codegen round-trip harness.  Kept in a
// single TU so the RewriteCodegen*RTTests.cpp translation units that include
// RewriteCodegenHarness.h link against one copy of each builder.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

using namespace neverd;

namespace rwcg {

void ensureLLVMTargets() {
  static bool Done = false;
  if (Done)
    return;
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  Done = true;
}

// Build IR: int add(int a, int b) { return a + b; }
// AArch64/x86-64 ABI: a=first int reg, b=second int reg, return in first.
std::unique_ptr<llvm::Module>
buildAddIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "add", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Sum = B.CreateAdd(F->getArg(0), F->getArg(1));
  B.CreateRet(Sum);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR: int compute(int x) { return x * 3 + 7; }
std::unique_ptr<llvm::Module>
buildComputeIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "compute", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Mul = B.CreateMul(F->getArg(0), B.getInt32(3));
  auto *Add = B.CreateAdd(Mul, B.getInt32(7));
  B.CreateRet(Add);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with data reference:
//   @counter = global i32 42
//   int get_and_inc() { counter++; return counter; }
std::unique_ptr<llvm::Module>
buildDataRefIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *GV = new llvm::GlobalVariable(
      *M, I32, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 42), "counter");
  GV->setDSOLocal(true);

  auto *FTy = llvm::FunctionType::get(I32, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "get_and_inc", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Val = B.CreateLoad(I32, GV);
  auto *Inc = B.CreateAdd(Val, B.getInt32(1));
  B.CreateStore(Inc, GV);
  B.CreateRet(Inc);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with cross-function call:
//   int helper(int x) { return x * 2 + 1; }
//   int caller(int a, int b) { return helper(a) + helper(b); }
std::unique_ptr<llvm::Module>
buildCrossCallIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);

  auto *HelperTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *Helper = llvm::Function::Create(HelperTy,
      llvm::Function::InternalLinkage, "helper", M.get());
  Helper->setDSOLocal(true);
  {
    auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Helper);
    llvm::IRBuilder<> B(BB);
    auto *Mul = B.CreateMul(Helper->getArg(0), B.getInt32(2));
    auto *Add = B.CreateAdd(Mul, B.getInt32(1));
    B.CreateRet(Add);
  }

  auto *CallerTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *Caller = llvm::Function::Create(CallerTy,
      llvm::Function::ExternalLinkage, "caller", M.get());
  {
    auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Caller);
    llvm::IRBuilder<> B(BB);
    auto *R1 = B.CreateCall(Helper, {Caller->getArg(0)});
    auto *R2 = B.CreateCall(Helper, {Caller->getArg(1)});
    auto *Sum = B.CreateAdd(R1, R2);
    B.CreateRet(Sum);
  }

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with external call:
//   declare i32 @external_fn(i32)
//   int wrap_ext(int x) { return external_fn(x + 10); }
// DSOLocal controls whether the call goes direct (dso_local=true) or via @PLT.
std::unique_ptr<llvm::Module>
buildExternalCallIR(llvm::LLVMContext &Ctx, const char *Triple,
                    bool DSOLocal) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *ExtTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *ExtFn = llvm::Function::Create(ExtTy,
      llvm::Function::ExternalLinkage, "external_fn", M.get());
  if (DSOLocal)
    ExtFn->setDSOLocal(true);

  auto *WrapTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *Wrap = llvm::Function::Create(WrapTy,
      llvm::Function::ExternalLinkage, "wrap_ext", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Wrap);
  llvm::IRBuilder<> B(BB);
  auto *Added = B.CreateAdd(Wrap->getArg(0), B.getInt32(10));
  auto *Result = B.CreateCall(ExtFn, {Added});
  B.CreateRet(Result);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with an external function address materialized in data:
//   int external_fn(int);
//   int (*external_slot)(int) = external_fn;
//   int call_external_ptr(int x) { return external_slot(x); }
std::unique_ptr<llvm::Module>
buildExternalFunctionPointerIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *ExtTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *ExtFn = llvm::Function::Create(
      ExtTy, llvm::Function::ExternalLinkage, "external_fn", M.get());
  ExtFn->setDSOLocal(true);
  auto *Slot = new llvm::GlobalVariable(
      *M, ExtFn->getType(), false, llvm::GlobalValue::ExternalLinkage, ExtFn,
      "external_slot");
  Slot->setDSOLocal(true);

  auto *Wrap = llvm::Function::Create(
      ExtTy, llvm::Function::ExternalLinkage, "call_external_ptr", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Wrap);
  llvm::IRBuilder<> B(BB);
  auto *Fn = B.CreateLoad(ExtFn->getType(), Slot);
  B.CreateRet(B.CreateCall(ExtTy, Fn, {Wrap->getArg(0)}));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with a loop: int sum_to(int n) { int s=0; for(int i=1;i<=n;i++) s+=i; return s; }
std::unique_ptr<llvm::Module>
buildLoopIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "sum_to", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Ctx, "loop", F);
  auto *Exit = llvm::BasicBlock::Create(Ctx, "exit", F);

  llvm::IRBuilder<> B(Entry);
  B.CreateBr(Loop);

  B.SetInsertPoint(Loop);
  auto *IPhi = B.CreatePHI(I32, 2, "i");
  auto *SPhi = B.CreatePHI(I32, 2, "s");
  auto *NewS = B.CreateAdd(SPhi, IPhi);
  auto *NewI = B.CreateAdd(IPhi, B.getInt32(1));
  auto *Cond = B.CreateICmpSLE(NewI, F->getArg(0));
  B.CreateCondBr(Cond, Loop, Exit);
  IPhi->addIncoming(B.getInt32(1), Entry);
  IPhi->addIncoming(NewI, Loop);
  SPhi->addIncoming(B.getInt32(0), Entry);
  SPhi->addIncoming(NewS, Loop);

  B.SetInsertPoint(Exit);
  B.CreateRet(NewS);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with switch:
//   int classify(int x) {
//     switch(x) {
//       case 0: return 10;
//       case 1: return 20;
//       case 2: return 30;
//       case 3: return 40;
//       default: return -1;
//     }
//   }
std::unique_ptr<llvm::Module>
buildSwitchIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "classify", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Case0 = llvm::BasicBlock::Create(Ctx, "case0", F);
  auto *Case1 = llvm::BasicBlock::Create(Ctx, "case1", F);
  auto *Case2 = llvm::BasicBlock::Create(Ctx, "case2", F);
  auto *Case3 = llvm::BasicBlock::Create(Ctx, "case3", F);
  auto *Default = llvm::BasicBlock::Create(Ctx, "default", F);

  llvm::IRBuilder<> B(Entry);
  auto *SW = B.CreateSwitch(F->getArg(0), Default, 4);
  SW->addCase(B.getInt32(0), Case0);
  SW->addCase(B.getInt32(1), Case1);
  SW->addCase(B.getInt32(2), Case2);
  SW->addCase(B.getInt32(3), Case3);

  B.SetInsertPoint(Case0);  B.CreateRet(B.getInt32(10));
  B.SetInsertPoint(Case1);  B.CreateRet(B.getInt32(20));
  B.SetInsertPoint(Case2);  B.CreateRet(B.getInt32(30));
  B.SetInsertPoint(Case3);  B.CreateRet(B.getInt32(40));
  B.SetInsertPoint(Default); B.CreateRet(B.getInt32(-1));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with multiple globals:
//   @g1 = global i32 111
//   @g2 = global i32 222
//   int sum_globals() { g1 += 1; g2 += 2; return g1 + g2; }
// Expected result: (111+1) + (222+2) = 336
std::unique_ptr<llvm::Module>
buildMultiGlobalIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *G1 = new llvm::GlobalVariable(
      *M, I32, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 111), "g1");
  G1->setDSOLocal(true);
  auto *G2 = new llvm::GlobalVariable(
      *M, I32, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 222), "g2");
  G2->setDSOLocal(true);

  auto *FTy = llvm::FunctionType::get(I32, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "sum_globals", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *V1 = B.CreateLoad(I32, G1);
  auto *Inc1 = B.CreateAdd(V1, B.getInt32(1));
  B.CreateStore(Inc1, G1);
  auto *V2 = B.CreateLoad(I32, G2);
  auto *Inc2 = B.CreateAdd(V2, B.getInt32(2));
  B.CreateStore(Inc2, G2);
  B.CreateRet(B.CreateAdd(Inc1, Inc2));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

llvm::mc_rewrite::RewriteResult
compileRewrite(llvm::Module &Mod, Arch Ar, BinaryFormat Fmt,
          ResolveCallback Resolve) {
  ensureLLVMTargets();

  llvm::mc_rewrite::RewriteOptions Opts;
  Opts.Model.TextVA = CODE_VA;
  Opts.Model.getSectionVA = [](llvm::StringRef Name) -> uint64_t {
    if (Name.contains("text") || Name.contains("TEXT"))
      return CODE_VA;
    if (Name.contains("data") || Name.contains("DATA") ||
        Name.contains("bss") || Name.contains("BSS"))
      return DATA_VA;
    return DATA_VA + 0x1000;
  };
  if (Resolve)
    Opts.Model.resolve = Resolve;
  else
    Opts.Model.resolve = [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
      return std::nullopt;
    };

  Codegen CG;
  return CG.compileForRewrite(Mod, Ar, Opts, Fmt);
}

llvm::mc_rewrite::RewriteResult
compileRewriteWithVAs(llvm::Module &Mod, Arch Ar, BinaryFormat Fmt,
                 uint64_t CodeVA, uint64_t DataVA,
                 ResolveCallback Resolve) {
  ensureLLVMTargets();

  llvm::mc_rewrite::RewriteOptions Opts;
  Opts.Model.TextVA = CodeVA;
  Opts.Model.getSectionVA = [CodeVA, DataVA](llvm::StringRef Name) -> uint64_t {
    if (Name.contains("text") || Name.contains("TEXT"))
      return CodeVA;
    if (Name.contains("data") || Name.contains("DATA") ||
        Name.contains("bss") || Name.contains("BSS"))
      return DataVA;
    return DataVA + 0x1000;
  };
  if (Resolve)
    Opts.Model.resolve = Resolve;
  else
    Opts.Model.resolve = [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
      return std::nullopt;
    };

  Codegen CG;
  return CG.compileForRewrite(Mod, Ar, Opts, Fmt);
}

const llvm::mc_rewrite::RewriteSection *
findDataSection(const llvm::mc_rewrite::RewriteResult &R) {
  for (auto &S : R.Sections) {
    llvm::StringRef N(S.Name);
    if (N.contains("data") || N.contains("DATA") || N.contains("bss"))
      return &S;
  }
  return nullptr;
}

const llvm::mc_rewrite::RewriteSection *
findTextSection(const llvm::mc_rewrite::RewriteResult &R) {
  for (auto &S : R.Sections) {
    llvm::StringRef N(S.Name);
    if (N.contains("text") || N.contains("TEXT"))
      return &S;
  }
  return nullptr;
}

// MachO prefixes symbols with '_'; this helper checks both variants.
uint64_t findSymbolVA(const llvm::mc_rewrite::RewriteResult &R,
                             const char *Name, uint64_t Default) {
  if (R.SymbolAddrs.count(Name))
    return R.SymbolAddrs.at(Name);
  std::string Prefixed = std::string("_") + Name;
  if (R.SymbolAddrs.count(Prefixed))
    return R.SymbolAddrs.at(Prefixed);
  return Default;
}

// ==================== Float via integer wrapper ====================
// int float_add_trunc(int a, int b) { return (int)((float)a + (float)b); }
// Tests FP codegen while keeping Unicorn I/O in integer regs.
std::unique_ptr<llvm::Module>
buildFloatAddTruncIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *F32 = llvm::Type::getFloatTy(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "float_add_trunc", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *FA = B.CreateSIToFP(F->getArg(0), F32);
  auto *FB = B.CreateSIToFP(F->getArg(1), F32);
  auto *Sum = B.CreateFAdd(FA, FB);
  auto *Res = B.CreateFPToSI(Sum, I32);
  B.CreateRet(Res);
  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== Chained functions (10 deep) ====================
// f0(x)=x+1, f1(x)=f0(x)+1, ..., f9(x)=f8(x)+1
// chain_entry(x) = f9(x) → x + 10
std::unique_ptr<llvm::Module>
buildChainedFunctionsIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);

  llvm::Function *Prev = nullptr;
  for (int I = 0; I < 10; ++I) {
    std::string Name = "f" + std::to_string(I);
    auto *F = llvm::Function::Create(FTy, llvm::Function::InternalLinkage,
                                     Name, M.get());
    F->setDSOLocal(true);
    auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
    llvm::IRBuilder<> B(BB);
    llvm::Value *V = F->getArg(0);
    if (Prev)
      V = B.CreateCall(Prev, {V});
    V = B.CreateAdd(V, B.getInt32(1));
    B.CreateRet(V);
    Prev = F;
  }

  auto *Entry = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                       "chain_entry", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Entry);
  llvm::IRBuilder<> B(BB);
  B.CreateRet(B.CreateCall(Prev, {Entry->getArg(0)}));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== 64-bit wide arithmetic ====================
// int64_t wide_mul(int64_t a, int64_t b) { return a * b + 7; }
// On i386 this exercises register pairs for 64-bit values.
std::unique_ptr<llvm::Module>
buildWideMulIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I64 = llvm::Type::getInt64Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I64, {I64, I64}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "wide_mul", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Prod = B.CreateMul(F->getArg(0), F->getArg(1));
  auto *Res = B.CreateAdd(Prod, B.getInt64(7));
  B.CreateRet(Res);
  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== Conditional with multiple BBs ====================
// int abs_diff(int a, int b) {
//   int d = a - b;
//   return d >= 0 ? d : -d;
// }
std::unique_ptr<llvm::Module>
buildAbsDiffIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "abs_diff", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Pos = llvm::BasicBlock::Create(Ctx, "pos", F);
  auto *Neg = llvm::BasicBlock::Create(Ctx, "neg", F);
  auto *Merge = llvm::BasicBlock::Create(Ctx, "merge", F);

  llvm::IRBuilder<> B(Entry);
  auto *D = B.CreateSub(F->getArg(0), F->getArg(1));
  auto *Cmp = B.CreateICmpSGE(D, B.getInt32(0));
  B.CreateCondBr(Cmp, Pos, Neg);

  B.SetInsertPoint(Pos);
  B.CreateBr(Merge);

  B.SetInsertPoint(Neg);
  auto *NegD = B.CreateNeg(D);
  B.CreateBr(Merge);

  B.SetInsertPoint(Merge);
  auto *Phi = B.CreatePHI(I32, 2);
  Phi->addIncoming(D, Pos);
  Phi->addIncoming(NegD, Neg);
  B.CreateRet(Phi);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ---------------------------------------------------------------------------
// Isolated smax/smin clamp test — diagnose LLVM fork codegen
// ---------------------------------------------------------------------------
std::unique_ptr<llvm::Module>
buildSmaxSminClampIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("smaxtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *I16 = llvm::Type::getInt16Ty(Ctx);
  auto *I8 = llvm::Type::getInt8Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "clamp_i16", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Trunc16 = B.CreateTrunc(F->getArg(0), I16);
  auto *SmaxFn = llvm::Intrinsic::getOrInsertDeclaration(
      M.get(), llvm::Intrinsic::smax, {I16});
  auto *SminFn = llvm::Intrinsic::getOrInsertDeclaration(
      M.get(), llvm::Intrinsic::smin, {I16});
  auto *Clamped1 = B.CreateCall(SmaxFn, {Trunc16, B.getInt16(-128)});
  auto *Clamped2 = B.CreateCall(SminFn, {Clamped1, B.getInt16(127)});
  auto *Narrow = B.CreateTrunc(Clamped2, I8);
  auto *Result = B.CreateZExt(Narrow, I32);
  B.CreateRet(Result);
  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

} // namespace rwcg
