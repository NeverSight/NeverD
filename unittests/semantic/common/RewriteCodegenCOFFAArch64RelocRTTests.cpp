//===- RewriteCodegenCOFFAArch64RelocRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// External-symbol resolution and displacement fixups for COFF AArch64: direct and @PLT external calls, function-address materialization, ARM/Thumb interworking and negative / large section displacements.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_COFF_AArch64, ExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());
  uint32_t stub_a64[] = { 0x0B000000, 0xD65F03C0 };
  uc_mem_write(uc, EXT_FN_VA, stub_a64, sizeof(stub_a64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, wrapVA, retAddr, 0, 500), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::COFF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (negative ADRP delta, COFF)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::COFF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (252MB ADRP delta, COFF)";
  uc_close(uc);
}

// --- COFF AArch64: PLTExternalCall ---
TEST(RewriteCodegen_COFF_AArch64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-pc-windows-msvc", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "external_fn")
      return ExtStub;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF, Resolve);
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

  uint32_t stubCode[] = {0xAA0003E0, 0xD65F03C0};
  uc_mem_write(uc, ExtStub, stubCode, sizeof(stubCode));

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}
