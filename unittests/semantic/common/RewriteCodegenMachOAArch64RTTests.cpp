//===- RewriteCodegenMachOAArch64RTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-shape and data-reference codegen for MachO AArch64: straight-line arithmetic, loops, switches, intra-module calls and global data.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_MachO_AArch64, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for MachO AArch64";
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

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

TEST(RewriteCodegen_MachO_AArch64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

TEST(RewriteCodegen_MachO_AArch64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

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

TEST(RewriteCodegen_MachO_AArch64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

TEST(RewriteCodegen_MachO_AArch64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "aarch64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

TEST(RewriteCodegen_MachO_AArch64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "aarch64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

TEST(RewriteCodegen_MachO_AArch64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

// --- MachO AArch64: FloatAddTrunc ---
TEST(RewriteCodegen_MachO_AArch64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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
  uint64_t x0 = 10, x1 = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- MachO AArch64: ChainedFunctions10 ---
TEST(RewriteCodegen_MachO_AArch64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 100;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- MachO AArch64: AbsDiff ---
TEST(RewriteCodegen_MachO_AArch64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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

TEST(RewriteCodegen_MachO_AArch64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
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
