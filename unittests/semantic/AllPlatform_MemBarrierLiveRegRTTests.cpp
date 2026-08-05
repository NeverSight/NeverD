//===- AllPlatform_MemBarrierLiveRegRTTests.cpp - barrier vs live reg ----===//
//
// A memory barrier / fence is a side-effect-only instruction: it must NOT
// define a register.  The lifter once emitted the barriers DMB/DSB/ISB/CLREX
// (ARM/AArch64) and MFENCE/LFENCE/SFENCE (x86) as an intrinsic whose default
// output was R0/X0/RAX, so the never-assigned (zero-defaulted) output shadowed a
// live value in that register -- the ARM32 `__sync` RMW miscompile, where a
// `dmb` between an LCG update and the following `lsr r0` zeroed the operand.
//
// Each probe pins a computed value in the EXACT register the barrier used to
// clobber, READS it back after the barrier, and returns it.  A clobbering
// barrier yields 7 instead of 2*a+7, so the roundtrip mismatches on every
// target.  Single-threaded in Unicorn the barriers are plain no-ops.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MemBarrierRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MemBarrierRT, Verify) { roundTripX64(GetParam()); }
class X86MemBarrierRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MemBarrierRT, Verify) { roundTripX86(GetParam()); }
class A64MemBarrierRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MemBarrierRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32MemBarrierRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MemBarrierRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// x86/x64: value pinned in EAX/RAX across the fence, then read back.
static RoundTripTC makeX86(const std::string &name, const std::string &barrier,
                           const std::string &sfx, const std::string &acc,
                           const std::string &T, uint64_t arg) {
  return {name,
    T+" f("+T+" a){ "+T+" r;\n"
    "  __asm__ volatile(\"mov"+sfx+" %1, %%"+acc+"\\n\\t"
    "shl"+sfx+" $1, %%"+acc+"\\n\\t"+barrier+"\\n\\t"
    "add"+sfx+" $7, %%"+acc+"\\n\\tmov"+sfx+" %%"+acc+", %0\"\n"
    "    :\"=r\"(r):\"r\"(a):\""+acc+"\",\"memory\");\n"
    "  return r; }\n",
    {arg}, "MemBarrier"};
}

// ARM/AArch64: value pinned in R0/X0 across the barrier, then read back.
static RoundTripTC makeArm(const std::string &name, const std::string &barrier,
                           const std::string &reg, const std::string &T,
                           uint64_t arg) {
  return {name,
    T+" f("+T+" a){ "+T+" r;\n"
    "  __asm__ volatile(\"lsl "+reg+", %1, #1\\n\\t"+barrier+"\\n\\t"
    "add "+reg+", "+reg+", #7\\n\\tmov %0, "+reg+"\"\n"
    "    :\"=r\"(r):\"r\"(a):\""+reg+"\",\"memory\");\n"
    "  return r; }\n",
    {arg}, "MemBarrier"};
}

static const std::vector<RoundTripTC> kX64 = {
  makeX86("x64_mfence", "mfence", "q", "rax", "long", 0x1111ULL),
  makeX86("x64_lfence", "lfence", "q", "rax", "long", 0x2222ULL),
  makeX86("x64_sfence", "sfence", "q", "rax", "long", 0x3333ULL),
};
static const std::vector<RoundTripTC> kX86 = {
  makeX86("x86_mfence", "mfence", "l", "eax", "int", 0x4444ULL),
  makeX86("x86_lfence", "lfence", "l", "eax", "int", 0x5555ULL),
  makeX86("x86_sfence", "sfence", "l", "eax", "int", 0x6666ULL),
};
static const std::vector<RoundTripTC> kA64 = {
  makeArm("a64_dmb",   "dmb ish", "x0", "long", 0x7777ULL),
  makeArm("a64_dsb",   "dsb ish", "x0", "long", 0x8888ULL),
  makeArm("a64_isb",   "isb",     "x0", "long", 0x9999ULL),
  makeArm("a64_clrex", "clrex",   "x0", "long", 0xAAAAULL),
};
static const std::vector<RoundTripTC> kARM = {
  makeArm("arm_dmb",   "dmb ish", "r0", "int", 0xBBBBULL),
  makeArm("arm_dsb",   "dsb ish", "r0", "int", 0xCCCCULL),
  makeArm("arm_isb",   "isb",     "r0", "int", 0xDDDDULL),
  makeArm("arm_clrex", "clrex",   "r0", "int", 0xEEEEULL),
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(MemBarrier, X64MemBarrierRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemBarrier, X86MemBarrierRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemBarrier, A64MemBarrierRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemBarrier, ARM32MemBarrierRT, ::testing::ValuesIn(kARM), rtTCName);
