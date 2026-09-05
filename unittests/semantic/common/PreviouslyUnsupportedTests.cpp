//===- PreviouslyUnsupportedTests.cpp - Test formerly blocked insns *- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
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

// These instruction classes are part of the supported regression surface.
// Keep the emulator and result visible so each test can validate semantics,
// rather than silently turning a capability regression into a skip.
#define RUN_SUPPORTED(arch, mode, asm_text, triple, features, init_regs,       \
                      init_mem, lookup, sp_reg, is_arm32, cpu_model)           \
  auto Code = LLVMMCAssembler::assemble(asm_text, triple, features);           \
  ASSERT_FALSE(Code.empty()) << "LLVM MC cannot assemble: " << asm_text;       \
  UnicornEmulator Emu;                                                         \
  auto S = Emu.run(arch, mode, Code, init_regs, init_mem, lookup, sp_reg,      \
                   is_arm32, cpu_model);                                       \
  ASSERT_TRUE(S.OK) << "Unicorn cannot emulate supported instruction: "        \
                    << S.Error

// ============================================================================
// ARM32: FIXED — UDIV/SDIV now work with LLVM MC + hwdiv-arm feature
// ============================================================================

TEST_F(PrevUnsupported, ARM32_UDIV) {
  auto Code = LLVMMCAssembler::assemble(
      "udiv r0, r1, r2", "armv7-linux-gnueabi", "+vfp2,+vfp3,+neon,+hwdiv-arm");
  ASSERT_FALSE(Code.empty()) << "LLVM MC should assemble UDIV";
  UnicornEmulator Emu;
  auto S = Emu.run(UC_ARCH_ARM, UC_MODE_ARM, Code, {{"r1", 100}, {"r2", 7}}, {},
                   uc_regs::lookupARM32, UC_ARM_REG_SP, true);
  ASSERT_TRUE(S.OK) << S.Error;
  EXPECT_EQ(S.Regs["r0"], 14u) << "100 / 7 = 14";
}

TEST_F(PrevUnsupported, ARM32_SDIV) {
  auto Code = LLVMMCAssembler::assemble(
      "mov r1, #100; rsb r1, r1, #0; mov r2, #7; sdiv r0, r1, r2",
      "armv7-linux-gnueabi", "+vfp2,+vfp3,+neon,+hwdiv-arm");
  ASSERT_FALSE(Code.empty()) << "LLVM MC should assemble SDIV";
  UnicornEmulator Emu;
  auto S = Emu.run(UC_ARCH_ARM, UC_MODE_ARM, Code, {}, {}, uc_regs::lookupARM32,
                   UC_ARM_REG_SP, true);
  ASSERT_TRUE(S.OK) << S.Error;
}

// ============================================================================
// x64: ADCX/ADOX — regression coverage (now supported)
// ============================================================================

TEST_F(PrevUnsupported, X64_ADCX) {
  RUN_SUPPORTED(
      UC_ARCH_X86, UC_MODE_64, "clc; mov rax, 10; mov rbx, 20; adcx rax, rbx",
      "x86_64-linux-gnu", "+adx",
      (std::vector<std::pair<std::string, uint64_t>>{}),
      (std::vector<MemInit>{}), uc_regs::lookupX64, UC_X86_REG_RSP, false, -1);
}

TEST_F(PrevUnsupported, X64_ADOX) {
  RUN_SUPPORTED(
      UC_ARCH_X86, UC_MODE_64, "clc; mov rax, 10; mov rbx, 20; adox rax, rbx",
      "x86_64-linux-gnu", "+adx",
      (std::vector<std::pair<std::string, uint64_t>>{}),
      (std::vector<MemInit>{}), uc_regs::lookupX64, UC_X86_REG_RSP, false, -1);
}

// ============================================================================
// x64: Cache hints — regression coverage (now supported in Unicorn fork)
// ============================================================================

TEST_F(PrevUnsupported, X64_CLFLUSHOPT) {
  RUN_SUPPORTED(
      UC_ARCH_X86, UC_MODE_64, "clflushopt [rsi]", "x86_64-linux-gnu",
      "+clflushopt",
      (std::vector<std::pair<std::string, uint64_t>>{{"rsi", DATA_BASE}}),
      (std::vector<MemInit>{}), uc_regs::lookupX64, UC_X86_REG_RSP, false, -1);
}

TEST_F(PrevUnsupported, X64_CLWB) {
  RUN_SUPPORTED(
      UC_ARCH_X86, UC_MODE_64, "clwb [rsi]", "x86_64-linux-gnu", "+clwb",
      (std::vector<std::pair<std::string, uint64_t>>{{"rsi", DATA_BASE}}),
      (std::vector<MemInit>{}), uc_regs::lookupX64, UC_X86_REG_RSP, false, -1);
}

// ============================================================================
// x64: SSE4.2 packed-string comparison — exact semantic regression
// ============================================================================

TEST_F(PrevUnsupported, X64_PCMPISTRI_EqualOrderedBoundary) {
  auto Code =
      LLVMMCAssembler::assemble("movdqu xmm0, [rsi]; movdqu xmm1, [rsi + 16]; "
                                "pcmpistri xmm0, xmm1, 0x4c",
                                "x86_64-linux-gnu", "+sse4.2");
  ASSERT_FALSE(Code.empty()) << "LLVM MC should assemble PCMPISTRI";

  Bytes Inputs = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k',
                  'l', 'm', 'n', 'o', 'p', 'b', 'c', 'd', 'e', 'f', 'g',
                  'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'a'};
  UnicornEmulator Emu;
  auto S =
      Emu.run(UC_ARCH_X86, UC_MODE_64, Code, {{"rsi", DATA_BASE}},
              {{DATA_BASE, Inputs}}, uc_regs::lookupX64, UC_X86_REG_RSP, false);
  ASSERT_TRUE(S.OK) << S.Error;
  EXPECT_EQ(S.Regs["rcx"], 15u);
}

// ============================================================================
// AArch64: LSE atomics — regression coverage (now supported)
// ============================================================================

TEST_F(PrevUnsupported, A64_CAS) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #42; mov x1, #99; cas x0, x1, [x2]", "aarch64-linux-gnu",
      "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(S.Regs["x0"], 42u);
  EXPECT_EQ(Emu.readMem(DATA_BASE, 8), packU64(99));
}

TEST_F(PrevUnsupported, A64_SWP) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0), "mov x0, #99; swp x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(S.Regs["x1"], 42u);
  EXPECT_EQ(Emu.readMem(DATA_BASE, 8), packU64(99));
}

TEST_F(PrevUnsupported, A64_LDADD) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0), "mov x0, #10; ldadd x0, x1, [x2]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(S.Regs["x1"], 42u);
  EXPECT_EQ(Emu.readMem(DATA_BASE, 8), packU64(52));
}

TEST_F(PrevUnsupported, A64_LDCLR) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #0xFF; ldclr x0, x1, [x2]", "aarch64-linux-gnu",
      "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(0xFFFF)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(S.Regs["x1"], 0xFFFFu);
  EXPECT_EQ(Emu.readMem(DATA_BASE, 8), packU64(0xFF00));
}

TEST_F(PrevUnsupported, A64_LDSET) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0),
      "mov x0, #0x0F; ldset x0, x1, [x2]", "aarch64-linux-gnu",
      "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x2", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(0xF0)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(S.Regs["x1"], 0xF0u);
  EXPECT_EQ(Emu.readMem(DATA_BASE, 8), packU64(0xFF));
}

TEST_F(PrevUnsupported, A64_STADD) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0), "mov x0, #10; stadd x0, [x1]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse",
      (std::vector<std::pair<std::string, uint64_t>>{{"x1", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(42)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(Emu.readMem(DATA_BASE, 8), packU64(52));
}

TEST_F(PrevUnsupported, A64_LDAPR) {
  RUN_SUPPORTED(
      UC_ARCH_ARM64, static_cast<uc_mode>(0), "ldapr x0, [x1]",
      "aarch64-linux-gnu", "+neon,+fp-armv8,+crc,+crypto,+lse,+rcpc",
      (std::vector<std::pair<std::string, uint64_t>>{{"x1", DATA_BASE}}),
      (std::vector<MemInit>{{DATA_BASE, packU64(99)}}), uc_regs::lookupA64,
      UC_ARM64_REG_SP, false, UC_CPU_ARM64_MAX);
  EXPECT_EQ(S.Regs["x0"], 99u);
}

#undef RUN_SUPPORTED
