//===- RewriteCodegenMIRRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// MIR-pass and intrinsic-lowering round trips: running an MIR pass over a RewriteResult, plus the isolated smax/smin clamp case used to diagnose LLVM fork codegen.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

TEST(RewriteCodegen_MIR, NopPassOnRewriteResult_x64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  size_t OrigTextSize = 0;
  for (auto &S : RR.Sections)
    if (llvm::StringRef(S.Name).contains("text"))
      OrigTextSize = S.Bytes.size();
  ASSERT_GT(OrigTextSize, 0u);

  MIRPassRunner Runner;
  NopPass NP;
  Runner.addPass(&NP);
  // NopPass appends a NOP (size change). runOnRewriteResult must refuse to
  // apply it in the in-place path (the change would break already-resolved
  // cross-function references) and emit a warning. Capture stderr so the
  // expected warning stays out of the test log, and assert it actually fired.
  testing::internal::CaptureStderr();
  Runner.runOnRewriteResult(RR, Arch::X64);
  std::string Warn = testing::internal::GetCapturedStderr();
  EXPECT_NE(Warn.find("size change"), std::string::npos)
      << "expected a size-change rejection warning, got: " << Warn;

  // The rejected size change must leave .text byte-for-byte unchanged.
  auto *TextAfter = findTextSection(RR);
  ASSERT_NE(TextAfter, nullptr);
  EXPECT_EQ(TextAfter->Bytes.size(), OrigTextSize)
      << "size-change pass must not corrupt .text";

  // Verify function still works after pass infrastructure touched it.
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

  uint64_t rdi = 3, rsi = 4;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, TextAfter->VA, retAddr + 1, 0, 500), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "add(3,4)=7 after MIR pass";
  uc_close(uc);
}

TEST(RewriteCodegen_MIR, NopPassOnRewriteResult_AArch64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());

  size_t OrigSize = 0;
  for (auto &S : RR.Sections)
    if (llvm::StringRef(S.Name).contains("text"))
      OrigSize = S.Bytes.size();

  MIRPassRunner Runner;
  NopPass NP;
  Runner.addPass(&NP);
  // See the x64 variant: the size change must be rejected with a warning and
  // .text left unchanged. Capture stderr to keep the expected warning out of
  // the test log while asserting it was emitted.
  testing::internal::CaptureStderr();
  Runner.runOnRewriteResult(RR, Arch::AArch64);
  std::string Warn = testing::internal::GetCapturedStderr();
  EXPECT_NE(Warn.find("size change"), std::string::npos)
      << "expected a size-change rejection warning, got: " << Warn;

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  EXPECT_EQ(Text->Bytes.size(), OrigSize);

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
  uint64_t x0 = 3, x1 = 4;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 500), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 7u) << "add(3,4)=7 after MIR pass";
  uc_close(uc);
}

TEST(RewriteCodegen_SmaxSmin, ClampI16_viaRewrite) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSmaxSminClampIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
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
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_mem_write(uc, rsp, &retAddr, 8);
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  struct { int64_t input; uint64_t expected; } cases[] = {
    {42, 42}, {200, 127}, {-200, 128}, {127, 127}, {-128, 128}, {0, 0},
  };
  for (auto &tc : cases) {
    uint64_t rdi = (uint64_t)tc.input;
    uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
    rsp = STACK_BASE + STACK_SIZE - 0x100;
    uc_mem_write(uc, rsp, &retAddr, 8);
    uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
    ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 200), UC_ERR_OK);
    uint64_t rax = 0;
    uc_reg_read(uc, UC_X86_REG_RAX, &rax);
    EXPECT_EQ(rax, tc.expected)
        << "clamp_i16(" << tc.input << ") rewrite = " << rax;
  }
  uc_close(uc);
}

TEST(RewriteCodegen_SmaxSmin, ClampI16_viaCompile) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSmaxSminClampIR(Ctx, "x86_64-unknown-linux-elf");

  Codegen CG;
  auto CR = CG.compile(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(CR.Success) << "compile failed";
  ASSERT_FALSE(CR.ObjectData.empty());

  auto ObjBuf = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(reinterpret_cast<const char *>(CR.ObjectData.data()),
                      CR.ObjectData.size()),
      "", false);
  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(ObjBuf->getMemBufferRef());
  ASSERT_TRUE(!!ObjOrErr);
  auto &Obj = **ObjOrErr;
  std::vector<uint8_t> TextBytes;
  for (auto &Sec : Obj.sections()) {
    auto NameOrErr = Sec.getName();
    if (!NameOrErr) continue;
    if (*NameOrErr == section_names::elf::Text) {
      auto ContOrErr = Sec.getContents();
      if (ContOrErr)
        TextBytes.assign(ContOrErr->begin(), ContOrErr->end());
      break;
    }
  }
  ASSERT_FALSE(TextBytes.empty());

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uint64_t codeVA = 0x400000;
  uc_mem_map(uc, codeVA, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, codeVA, TextBytes.data(), TextBytes.size());
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  struct { int64_t input; uint64_t expected; } cases[] = {
    {42, 42}, {200, 127}, {-200, 128}, {127, 127}, {-128, 128}, {0, 0},
  };
  for (auto &tc : cases) {
    uint64_t rdi = (uint64_t)tc.input;
    uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
    uint64_t rsp2 = STACK_BASE + STACK_SIZE - 0x100;
    uc_mem_write(uc, rsp2, &retAddr, 8);
    uc_reg_write(uc, UC_X86_REG_RSP, &rsp2);
    ASSERT_EQ(uc_emu_start(uc, codeVA, retAddr, 0, 200), UC_ERR_OK);
    uint64_t rax = 0;
    uc_reg_read(uc, UC_X86_REG_RAX, &rax);
    EXPECT_EQ(rax, tc.expected)
        << "clamp_i16(" << tc.input << ") compile = " << rax;
  }
  uc_close(uc);
}
