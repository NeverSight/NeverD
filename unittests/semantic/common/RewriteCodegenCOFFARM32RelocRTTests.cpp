//===- RewriteCodegenCOFFARM32RelocRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// External-symbol resolution and displacement fixups for COFF ARM32: direct and @PLT external calls, function-address materialization, ARM/Thumb interworking and negative / large section displacements.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_COFF_ARM32, ExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        // Bit 0 = 1 indicates Thumb mode target → backend uses BL (stays in Thumb)
        if (Sym == "external_fn") return EXT_FN_VA | 1;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // Thumb stub: adds r0, r0, r0; bx lr
  uint8_t stub[] = { 0x00, 0x44, 0x70, 0x47 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, wrapVA | 1, retAddr, 0, 2000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "wrap_ext(5): external_fn(15)=30 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, ExternalFunctionPointer) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod =
      buildExternalFunctionPointerIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(
      *Mod, Arch::ARM, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn")
          return EXT_FN_VA | 1;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  auto *Data = findDataSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(Data, nullptr);

  uint64_t SlotVA = findSymbolVA(RR, "external_slot", Data->VA);
  ASSERT_GE(SlotVA, Data->VA);
  ASSERT_LE(SlotVA - Data->VA + sizeof(uint32_t), Data->Bytes.size());
  uint32_t StoredFn = 0;
  std::memcpy(&StoredFn, Data->Bytes.data() + (SlotVA - Data->VA),
              sizeof(StoredFn));
  EXPECT_EQ(StoredFn, uint32_t(EXT_FN_VA | 1));

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size()),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(uc, Data->VA, Data->Bytes.data(), Data->Bytes.size()),
            UC_ERR_OK);

  // Thumb stub: add r0, r0; bx lr.
  uint8_t Stub[] = {0x00, 0x44, 0x70, 0x47};
  ASSERT_EQ(uc_mem_write(uc, EXT_FN_VA, Stub, sizeof(Stub)), UC_ERR_OK);

  uint32_t R0 = 7;
  uint32_t SP = uint32_t(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t RetAddr = uint32_t(CODE_VA + 0xf000);
  uc_reg_write(uc, UC_ARM_REG_R0, &R0);
  uc_reg_write(uc, UC_ARM_REG_SP, &SP);
  uc_reg_write(uc, UC_ARM_REG_LR, &RetAddr);

  uint64_t WrapVA = findSymbolVA(RR, "call_external_ptr", Text->VA);
  uc_err Err = uc_emu_start(uc, WrapVA | 1, RetAddr, 0, 2000);
  ASSERT_EQ(Err, UC_ERR_OK)
      << "Unicorn Thumb function-pointer call failed: " << uc_strerror(Err);
  uc_reg_read(uc, UC_ARM_REG_R0, &R0);
  EXPECT_EQ(R0, 14u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, ThumbToArmInterwork) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn")
          return EXT_FN_VA;  // even = ARM mode
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
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
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA | 1, retAddr, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn Thumb→ARM interwork failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u)
      << "wrap_ext(5): external_fn(15)=30 via Thumb→ARM interwork";
  uc_close(uc);
}

// --- COFF ARM32: PLTExternalCall ---
TEST(RewriteCodegen_COFF_ARM32, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "thumbv7-pc-windows-msvc", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "external_fn")
      return ExtStub | 1;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint16_t stubThumb[] = {0x4600, 0x4770};
  uc_mem_write(uc, ExtStub, stubThumb, sizeof(stubThumb));

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::COFF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = 0x40F000;
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (negative delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::COFF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = 0x40F000;
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}
