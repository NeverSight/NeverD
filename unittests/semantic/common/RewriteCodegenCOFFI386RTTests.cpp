//===- RewriteCodegenCOFFI386RTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-shape and data-reference codegen for COFF i386: straight-line arithmetic, loops, switches, intra-module calls and global data.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_COFF_i386, AddFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 17, 25 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 42u) << "add(17,25) should be 42";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, ComputeFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 22u) << "compute(5) = 5*3+7 = 22";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, DataReference) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "get_and_inc() should return 43 (42+1)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, CrossFunctionCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t callerVA = (uint32_t)findSymbolVA(RR, "caller", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 3, 7 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 22u) << "caller(3,7) should be 22";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 10 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 2 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack_data[] = { retAddr, 10, 25 };
  uc_mem_write(uc, esp, stack_data, sizeof(stack_data));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t entryVA = (uint32_t)findSymbolVA(RR, "chain_entry", Text->VA);
  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 100 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 10000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 110u) << "chain_entry(100) = 100+10 = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 3, 10 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 7u) << "abs_diff(3,10) = 7";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  // i386: i64 args as (lo, hi) pairs on stack
  uint32_t stack[5];
  stack[0] = retAddr;
  stack[1] = 1000000000U; stack[2] = 0; // a = 1000000000 (lo, hi)
  stack[3] = 3;            stack[4] = 0; // b = 3 (lo, hi)
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0, edx = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  uc_reg_read(uc, UC_X86_REG_EDX, &edx);
  uint64_t result = (uint64_t)edx << 32 | eax;
  EXPECT_EQ(result, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}
