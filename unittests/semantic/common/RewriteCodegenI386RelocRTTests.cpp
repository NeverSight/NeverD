//===- RewriteCodegenI386RelocRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// External-symbol resolution and displacement fixups for ELF i386: direct and @PLT external calls, function-address materialization, ARM/Thumb interworking and negative / large section displacements.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_i386, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF,
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
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // i386 stub: mov eax, [esp+4]; add eax, eax; ret  (doubles arg from stack)
  uint8_t stub_i386[] = {
    0x8B, 0x44, 0x24, 0x04,  // mov eax, [esp+4]
    0x01, 0xC0,              // add eax, eax
    0xC3                     // ret
  };
  uc_mem_write(uc, EXT_FN_VA, stub_i386, sizeof(stub_i386));

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  // wrap_ext(5) = external_fn(5+10) = double(15) = 30
  EXPECT_EQ(eax, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_i386, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "i386-unknown-linux-gnu", false);
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF,
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
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // i386 stub: mov eax, [esp+4]; add eax, eax; ret  (doubles arg from stack)
  uint8_t stub[] = { 0x8B, 0x44, 0x24, 0x04, 0x01, 0xC0, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  // cdecl stack layout at function entry: [esp]=retAddr, [esp+4]=arg
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X86, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t retAddr = 0x4FF000;
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t stackData[] = { retAddr };
  uc_mem_write(uc, esp, stackData, sizeof(stackData));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "counter=42, inc to 43 (negative delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X86, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t retAddr = 0x4FF000;
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t stackData[] = { retAddr };
  uc_mem_write(uc, esp, stackData, sizeof(stackData));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}
