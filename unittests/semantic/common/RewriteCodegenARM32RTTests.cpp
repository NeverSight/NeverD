//===- RewriteCodegenARM32RTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-shape and data-reference codegen for ELF ARM32: straight-line arithmetic, loops, switches, intra-module calls and global data.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_ARM32, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM_REG_R0, &a);
  uc_reg_write(uc, UC_ARM_REG_R1, &b);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t callerVA = (uint32_t)findSymbolVA(RR, "caller", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, callerVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for ARM32";
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t a = 17, b = 25;
  uc_reg_write(uc, UC_ARM_REG_R0, &a);
  uc_reg_write(uc, UC_ARM_REG_R1, &b);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for ARM32";
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for ARM32";

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 200);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t n = 10;
  uc_reg_write(uc, UC_ARM_REG_R0, &n);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t x = 2;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  // Enable VFP: CPACR allows CP10/CP11, FPEXC.EN=1
  uint32_t cpacr = 0x00F00000;
  uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr);
  uint32_t fpexc = (1u << 30);
  uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc);

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 10, r1 = 25;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x100000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x1000);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF0000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint32_t r0 = 100;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 110u) << "chain_entry(100) = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 3, r1 = 10;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  // ARM32 AAPCS: i64 in r0:r1 (lo:hi), second i64 in r2:r3
  uint32_t r0 = 1000000000U, r1 = 0; // a = 1000000000
  uint32_t r2 = 3, r3 = 0;            // b = 3
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  uc_reg_write(uc, UC_ARM_REG_R2, &r2);
  uc_reg_write(uc, UC_ARM_REG_R3, &r3);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uint64_t result = (uint64_t)r1 << 32 | r0;
  EXPECT_EQ(result, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}
