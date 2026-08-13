//===- RewriteCodegenX64RelocRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// External-symbol resolution and displacement fixups for ELF x86-64: direct and @PLT external calls, function-address materialization, ARM/Thumb interworking and negative / large section displacements.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_x64, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // x86-64 stub: lea eax, [rdi+rdi]; ret  (doubles arg)
  uint8_t stub_x64[] = { 0x8D, 0x04, 0x3F, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub_x64, sizeof(stub_x64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  // wrap_ext(5) = external_fn(5+10) = double(15) = 30
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_x64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
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
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43";
  uc_close(uc);
}

TEST(RewriteCodegen_x64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
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
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_x64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-unknown-linux-elf", false);
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // x86-64 stub: lea eax, [rdi+rdi]; ret  (doubles arg)
  uint8_t stub[] = { 0x8D, 0x04, 0x3F, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}
