//===- RewriteCodegenARM32RelocRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// External-symbol resolution and displacement fixups for ELF ARM32: direct and @PLT external calls, function-address materialization, ARM/Thumb interworking and negative / large section displacements.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_ARM32, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF,
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
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM32 stub: add r0, r0, r0; bx lr
  uint32_t stub_arm[] = { 0xE0800000, 0xE12FFF1E };
  uc_mem_write(uc, EXT_FN_VA, stub_arm, sizeof(stub_arm));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "armv7-unknown-linux-gnueabihf", false);
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF,
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
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM stub: add r0, r0, r0; bx lr  (doubles arg)
  uint8_t stub[] = {
    0x00, 0x00, 0x80, 0xE0, // add r0, r0, r0
    0x1E, 0xFF, 0x2F, 0xE1  // bx lr
  };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, ThumbToArmInterwork) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn")
          return EXT_FN_VA;  // even address = ARM mode
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM stub at even address: add r0, r0, r0; bx lr
  uint8_t armStub[] = {
    0x00, 0x00, 0x80, 0xE0,  // add r0, r0, r0
    0x1E, 0xFF, 0x2F, 0xE1   // bx lr
  };
  uc_mem_write(uc, EXT_FN_VA, armStub, sizeof(armStub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn ARM interwork failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u)
      << "wrap_ext(5): external_fn(15)=30 via ARM interwork";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(0x400000 + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (negative delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(0x400000 + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}
