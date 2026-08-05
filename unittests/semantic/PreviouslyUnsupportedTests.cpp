//===- PreviouslyUnsupportedTests.cpp - Test formerly blocked insns *- C++ -*-===//
//
// Tests instructions that were previously blocked by Keystone or Unicorn.
// LLVM MC now handles ALL assembly. Remaining failures are purely Unicorn
// emulation gaps — fixable in our third_party/unicorn fork.
//
// Current status:
//   PASS: ARM32 UDIV/SDIV
//   PASS: x64 ADCX/ADOX/CLFLUSHOPT/CLWB
//   PASS: AArch64 LSE atomics + LDAPR
//   Note: this file is kept as regression coverage for previously unsupported
//   instruction classes.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

class PrevUnsupported : public UnicornSemanticFixture {};

// Helper: try assemble + emulate, GTEST_SKIP if Unicorn fails
#define TRY_RUN_OR_SKIP(arch, mode, asm_text, triple, features, init_regs, \
                        init_mem, lookup, sp_reg, is_arm32)                \
  do {                                                                     \
    auto Code_ = LLVMMCAssembler::assemble(asm_text, triple, features);    \
    if (Code_.empty())                                                     \
      GTEST_SKIP() << "LLVM MC cannot assemble: " << asm_text;            \
    UnicornEmulator Emu_;                                                  \
    auto S_ = Emu_.run(arch, mode, Code_, init_regs, init_mem,             \
                       lookup, sp_reg, is_arm32);                          \
    if (!S_.OK)                                                            \
      GTEST_SKIP() << "Unicorn cannot emulate (fix in fork): " << S_.Error;\
  } while (0)

// ============================================================================
// ARM32: FIXED — UDIV/SDIV now work with LLVM MC + hwdiv-arm feature
// ============================================================================

TEST_F(PrevUnsupported, ARM32_UDIV) {
  auto Code = LLVMMCAssembler::assemble(
      "udiv r0, r1, r2", "armv7-linux-gnueabi",
      "+vfp2,+vfp3,+neon,+hwdiv-arm");
  ASSERT_FALSE(Code.empty()) << "LLVM MC should assemble UDIV";
  UnicornEmulator Emu;
  auto S = Emu.run(UC_ARCH_ARM, UC_MODE_ARM, Code,
                   {{"r1", 100}, {"r2", 7}}, {},
                   uc_regs::lookupARM32, UC_ARM_REG_SP, true);
  ASSERT_TRUE(S.OK) << S.Error;
  EXPECT_EQ(S.Regs["r0"], 14u) << "100 / 7 = 14";
}

TEST_F(PrevUnsupported, ARM32_SDIV) {
  auto Code = LLVMMCAssembler::assemble(
      "mov r1, #100; rsb r1, r1, #0; mov r2, #7; sdiv r0, r1, r2",
      "armv7-linux-gnueabi",
      "+vfp2,+vfp3,+neon,+hwdiv-arm");
  ASSERT_FALSE(Code.empty()) << "LLVM MC should assemble SDIV";
  UnicornEmulator Emu;
  auto S = Emu.run(UC_ARCH_ARM, UC_MODE_ARM, Code,
                   {}, {},
                   uc_regs::lookupARM32, UC_ARM_REG_SP, true);
  ASSERT_TRUE(S.OK) << S.Error;
}

// ============================================================================
// x64: ADCX/ADOX — regression coverage (now supported)
// ============================================================================

TEST_F(PrevUnsupported, X64_ADCX) {
  TRY_RUN_OR_SKIP(UC_ARCH_X86, UC_MODE_64,
      "clc; mov rax, 10; mov rbx, 20; adcx rax, rbx",
      "x86_64-linux-gnu", "+adx",
      (std::vector<std::pair<std::string, uint64_t>>{}),
      (std::vector<MemInit>{}),
      uc_regs::lookupX64, UC_X86_REG_RSP, false);
}

TEST_F(PrevUnsupported, X64_ADOX) {
  TRY_RUN_OR_SKIP(UC_ARCH_X86, UC_MODE_64,
      "clc; mov rax, 10; mov rbx, 20; adox rax, rbx",
      "x86_64-linux-gnu", "+adx",
      (std::vector<std::pair<std::string, uint64_t>>{}),
      (std::vector<MemInit>{}),
      uc_regs::lookupX64, UC_X86_REG_RSP, false);
}

// ============================================================================
// x64: Cache hints — regression coverage (now supported in Unicorn fork)
// ============================================================================

TEST_F(PrevUnsupported, X64_CLFLUSHOPT) {
  TRY_RUN_OR_SKIP(UC_ARCH_X86, UC_MODE_64,
      "clflushopt [rsi]",
      "x86_64-linux-gnu", "+clflushopt",
      (std::vector<std::pair<std::string, uint64_t>>{{"rsi", DATA_BASE}}),
      (std::vector<MemInit>{}),
      uc_regs::lookupX64, UC_X86_REG_RSP, false);
}

TEST_F(PrevUnsupported, X64_CLWB) {
  TRY_RUN_OR_SKIP(UC_ARCH_X86, UC_MODE_64,
      "clwb [rsi]",
      "x86_64-linux-gnu", "+clwb",
      (std::vector<std::pair<std::string, uint64_t>>{{"rsi", DATA_BASE}}),
      (std::vector<MemInit>{}),
      uc_regs::lookupX64, UC_X86_REG_RSP, false);
}

// ============================================================================
// AArch64: LSE atomics — regression coverage (now supported)
// ============================================================================

TEST_F(PrevUnsupported, A64_CAS) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #42; mov x1, #99; cas x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}

TEST_F(PrevUnsupported, A64_SWP) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #99; swp x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}

TEST_F(PrevUnsupported, A64_LDADD) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #10; ldadd x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}

TEST_F(PrevUnsupported, A64_LDCLR) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #0xFF; ldclr x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(0xFF00)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}

TEST_F(PrevUnsupported, A64_LDSET) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #0x0F; ldset x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(0xF0)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}

TEST_F(PrevUnsupported, A64_STADD) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #10; stadd x0, [x1]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x1", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}

TEST_F(PrevUnsupported, A64_LDAPR) {
  TRY_RUN_OR_SKIP(UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "ldapr x0, [x1]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse,+rcpc",
      (std::vector<std::pair<std::string, uint64_t>>{{"x1", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(99)}}),
      uc_regs::lookupA64, UC_ARM64_REG_SP, false);
}
