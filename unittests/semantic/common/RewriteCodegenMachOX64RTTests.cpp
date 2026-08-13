//===- RewriteCodegenMachOX64RTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-shape and data-reference codegen for MachO x86-64: straight-line arithmetic, loops, switches, intra-module calls and global data.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_MachO_x64, AddFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_X86_REG_RDI, &a);
  uc_reg_write(uc, UC_X86_REG_RSI, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 42u);
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, DataReference) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u);
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, CrossFunctionCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_X86_REG_RDI, &a);
  uc_reg_write(uc, UC_X86_REG_RSI, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u);
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, ComputeFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_X86_REG_RDI, &n);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

// --- MachO x64: FloatAddTrunc ---
TEST(RewriteCodegen_MachO_x64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 10, rsi = 25;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- MachO x64: ChainedFunctions10 ---
TEST(RewriteCodegen_MachO_x64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 100;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- MachO x64: AbsDiff ---
TEST(RewriteCodegen_MachO_x64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 3, rsi = 10;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 1000000000ULL, rsi = 3;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}
