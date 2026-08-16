//===- RewriteCodegenAArch64RTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-shape and data-reference codegen for ELF AArch64: straight-line arithmetic, loops, switches, intra-module calls and global data.
//
//===----------------------------------------------------------------------===//

#include "RewriteCodegenHarness.h"
#include "UnicornSemanticFixture.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsAArch64.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_AArch64, BfmmlaEnablesBF16) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = std::make_unique<llvm::Module>("bfmmla", Ctx);
  Mod->setTargetTriple(llvm::Triple("aarch64-unknown-linux-elf"));
  auto *V4F32 = llvm::FixedVectorType::get(llvm::Type::getFloatTy(Ctx), 4);
  auto *V8BF16 = llvm::FixedVectorType::get(llvm::Type::getBFloatTy(Ctx), 8);
  auto *FnTy = llvm::FunctionType::get(V4F32, {V4F32, V8BF16, V8BF16}, false);
  auto *Fn = llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                                    "bfmmla", *Mod);
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", Fn);
  llvm::IRBuilder<> Builder(Entry);
  auto Arg = Fn->arg_begin();
  llvm::Value *Acc = &*Arg++;
  llvm::Value *A = &*Arg++;
  llvm::Value *B = &*Arg;
  auto *BFMMLA = llvm::Intrinsic::getOrInsertDeclaration(
      Mod.get(), llvm::Intrinsic::aarch64_neon_bfmmla);
  Builder.CreateRet(Builder.CreateCall(BFMMLA, {Acc, A, B}));

  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_FALSE(Text->Bytes.empty());
}

TEST(RewriteCodegen_AArch64, MTEIntrinsicsEnableMTE) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = std::make_unique<llvm::Module>("mte", Ctx);
  Mod->setTargetTriple(llvm::Triple("aarch64-unknown-linux-elf"));
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  auto *FnTy = llvm::FunctionType::get(PtrTy, {PtrTy, PtrTy}, false);
  auto *Fn = llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                                    "mte_round_trip", *Mod);
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", Fn);
  llvm::IRBuilder<> Builder(Entry);
  auto Arg = Fn->arg_begin();
  llvm::Value *TagSource = &*Arg++;
  llvm::Value *Address = &*Arg;
  auto *STG = llvm::Intrinsic::getOrInsertDeclaration(
      Mod.get(), llvm::Intrinsic::aarch64_stg);
  auto *LDG = llvm::Intrinsic::getOrInsertDeclaration(
      Mod.get(), llvm::Intrinsic::aarch64_ldg);
  Builder.CreateCall(STG, {TagSource, Address});
  Builder.CreateRet(Builder.CreateCall(LDG, {Address, Address}));

  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_FALSE(Text->Bytes.empty());
}

TEST(RewriteCodegen_AArch64, MTEAddgSubgIntrinsicsEnableMTE) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = std::make_unique<llvm::Module>("mte_addg_subg", Ctx);
  Mod->setTargetTriple(llvm::Triple("aarch64-unknown-linux-elf"));
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  auto *FnTy = llvm::FunctionType::get(PtrTy, {PtrTy}, false);
  auto *Fn = llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                                    "mte_addg_subg", *Mod);
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", Fn);
  llvm::IRBuilder<> Builder(Entry);
  llvm::Value *Address = &*Fn->arg_begin();
  auto *ADDG = llvm::Intrinsic::getOrInsertDeclaration(
      Mod.get(), llvm::Intrinsic::aarch64_addg);
  auto *SUBG = llvm::Intrinsic::getOrInsertDeclaration(
      Mod.get(), llvm::Intrinsic::aarch64_subg);
  llvm::Value *Tagged = Builder.CreateCall(ADDG, {Address, Builder.getInt64(5)});
  Builder.CreateRet(Builder.CreateCall(
      SUBG, {Tagged, Builder.getInt64(112), Builder.getInt64(9)}));

  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_FALSE(Text->Bytes.empty());
}

TEST(RewriteCodegen_AArch64, I128AtomicAndEnablesLSE128) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = std::make_unique<llvm::Module>("lse128", Ctx);
  Mod->setTargetTriple(llvm::Triple("aarch64-unknown-linux-elf"));
  auto *I128Ty = llvm::IntegerType::get(Ctx, 128);
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  auto *FnTy = llvm::FunctionType::get(I128Ty, {PtrTy, I128Ty}, false);
  auto *Fn = llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                                    "atomic_clear_pair", *Mod);
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", Fn);
  llvm::IRBuilder<> Builder(Entry);
  auto Arg = Fn->arg_begin();
  llvm::Value *Address = &*Arg++;
  llvm::Value *ClearMask = &*Arg;
  auto *Old = Builder.CreateAtomicRMW(
      llvm::AtomicRMWInst::And, Address, Builder.CreateNot(ClearMask),
      llvm::MaybeAlign(16), llvm::AtomicOrdering::AcquireRelease);
  Builder.CreateRet(Old);

  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_FALSE(Text->Bytes.empty());
  bool HasLdclrp = false;
  for (size_t I = 0; I + 4 <= Text->Bytes.size(); I += 4) {
    uint32_t Word = static_cast<uint32_t>(Text->Bytes[I]) |
                    (static_cast<uint32_t>(Text->Bytes[I + 1]) << 8) |
                    (static_cast<uint32_t>(Text->Bytes[I + 2]) << 16) |
                    (static_cast<uint32_t>(Text->Bytes[I + 3]) << 24);
    // LSE128 LDCLRP{,A,L,AL}: fixed opcode bits, ignoring ordering/registers.
    HasLdclrp |= (Word & 0xff20fc00U) == 0x19201000U;
  }
  EXPECT_TRUE(HasLdclrp) << "expected a native LDCLRP-family instruction";
}

TEST(RewriteCodegen_AArch64, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  // AArch64 returns via BX LR — set LR to a sentinel end address
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  // Map a contiguous region covering code (0x400000) through data (0x510000)
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, callerVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &n);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 10, x1 = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x100000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x1000;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t x0 = 100;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 110u) << "chain_entry(100) = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 1000000000ULL, x1 = 3;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 3, x1 = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}
