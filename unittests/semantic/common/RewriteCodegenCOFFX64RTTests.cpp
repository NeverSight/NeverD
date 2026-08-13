//===- RewriteCodegenCOFFX64RTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Function-shape and data-reference codegen for COFF x86-64: straight-line arithmetic, loops, switches, intra-module calls and global data.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_COFF_x64, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for COFF x64";
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_X86_REG_RCX, &a);
  uc_reg_write(uc, UC_X86_REG_RDX, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
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
  uc_reg_write(uc, UC_X86_REG_RCX, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
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
  uc_reg_write(uc, UC_X86_REG_RCX, &a);
  uc_reg_write(uc, UC_X86_REG_RDX, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, PreservesNativeSectionTraits) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-pc-windows-msvc");
  auto Resolve = [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
    if (Sym == "external_fn")
      return EXT_FN_VA;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());

  auto Find =
      [&](llvm::StringRef Name) -> const llvm::mc_rewrite::RewriteSection * {
    for (const auto &S : RR.Sections)
      if (S.Name == Name)
        return &S;
    return nullptr;
  };

  const auto *Text = Find(".text");
  const auto *PData = Find(".pdata");
  const auto *XData = Find(".xdata");
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(PData, nullptr);
  ASSERT_NE(XData, nullptr);
  EXPECT_EQ(Text->Kind, llvm::mc_rewrite::RewriteSectionKind::Code);
  EXPECT_EQ(PData->Kind, llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData);
  EXPECT_EQ(XData->Kind, llvm::mc_rewrite::RewriteSectionKind::ReadOnlyData);
  EXPECT_TRUE(Text->IsAllocated);
  EXPECT_TRUE(PData->IsAllocated);
  EXPECT_TRUE(XData->IsAllocated);
  EXPECT_GE(Text->Alignment, 1u);
  EXPECT_GE(PData->Alignment, 4u);
  EXPECT_GE(XData->Alignment, 4u);

  llvm::LLVMContext Ctx2;
  auto Mod2 = buildExternalCallIR(Ctx2, "x86_64-pc-windows-msvc");
  constexpr uint64_t TestImageBase = 0x100000;
  CompiledImage Image = compileImageForPatch(
      *Mod2, Arch::X64, BinaryFormat::COFF, CODE_VA,
      [&](llvm::StringRef Sym, uint32_t Specifier) -> std::optional<uint64_t> {
        return Resolve(Sym, Specifier);
      },
      TestImageBase);
  ASSERT_TRUE(Image.Success);
  ASSERT_EQ(Image.Sections.size(), RR.Sections.size());
  for (const CompiledSection &S : Image.Sections) {
    EXPECT_GE(S.VA, Image.BaseVA);
    EXPECT_EQ(S.Offset, S.VA - Image.BaseVA);
    EXPECT_LE(S.Offset + S.Size, Image.Bytes.size());
  }
  auto FindCompiled = [&](llvm::StringRef Name) -> const CompiledSection * {
    for (const CompiledSection &S : Image.Sections)
      if (S.Name == Name)
        return &S;
    return nullptr;
  };
  const CompiledSection *CompiledPData = FindCompiled(".pdata");
  const CompiledSection *CompiledXData = FindCompiled(".xdata");
  ASSERT_NE(CompiledPData, nullptr);
  ASSERT_NE(CompiledXData, nullptr);
  ASSERT_GE(CompiledPData->Size, 12u);
  const uint8_t *Runtime = Image.Bytes.data() + CompiledPData->Offset;
  uint32_t BeginRVA = readLE<uint32_t>(Runtime);
  uint32_t EndRVA = readLE<uint32_t>(Runtime + 4);
  uint32_t UnwindRVA = readLE<uint32_t>(Runtime + 8);
  EXPECT_GE(BeginRVA, CODE_VA - TestImageBase);
  EXPECT_GT(EndRVA, BeginRVA);
  EXPECT_GE(UnwindRVA, CompiledXData->VA - TestImageBase);
  EXPECT_LT(UnwindRVA, CompiledXData->VA - TestImageBase + CompiledXData->Size);
}

TEST(RewriteCodegen_COFF_x64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
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
  uc_reg_write(uc, UC_X86_REG_RCX, &n);
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

TEST(RewriteCodegen_COFF_x64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
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
  uc_reg_write(uc, UC_X86_REG_RCX, &x);
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

TEST(RewriteCodegen_COFF_x64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
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

// --- COFF x64: FloatAddTrunc ---
TEST(RewriteCodegen_COFF_x64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
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
  uint64_t rcx = 10, rdx = 25;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  uc_reg_write(uc, UC_X86_REG_RDX, &rdx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- COFF x64: ChainedFunctions10 ---
TEST(RewriteCodegen_COFF_x64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rcx = 100;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- COFF x64: AbsDiff ---
TEST(RewriteCodegen_COFF_x64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rcx = 3, rdx = 10;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  uc_reg_write(uc, UC_X86_REG_RDX, &rdx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
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
  // Win64 ABI: rcx=a, rdx=b
  uint64_t rcx = 1000000000ULL, rdx = 3;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  uc_reg_write(uc, UC_X86_REG_RDX, &rdx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}
