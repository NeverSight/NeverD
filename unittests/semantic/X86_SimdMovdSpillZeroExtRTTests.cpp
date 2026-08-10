//===- X86_SimdMovdSpillZeroExtRTTests.cpp - movd/movq upper-lane zero --*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// MOVD/MOVQ (gpr/mem -> xmm) and the MEMORY forms of MOVSS/MOVSD zero-extend
// the destination XMM: the lanes above the moved element become 0 (Intel SDM).
// The lifter modeled the reg destination as a plain COPY of the NARROW source
// into the wide XMM, leaving the upper lanes at their prior value.  A direct
// read happened to be fixed up, but a later full-width read — a `movdqa` spill
// to the stack that is reloaded by `psadbw` — exposed the stale upper bytes.
//
// This is exactly the divergence the i386 SSE2-vectorized SAD kernels hit: with
// >=5 live psadbw accumulators clang spills a movd-zero-extended tail vector and
// reloads it, and the stale upper bytes inflated the sum-of-absolute-differences
// (i386 only, because x86-64 had the register pressure to avoid the spill).
//
// Each probe: write an all-ones vector V1 to a stack slot, keep it live, then
// store a freshly zero-extended narrow value V2 to the SAME slot via a movdqa
// spill, and `psadbw` the slot reducing BOTH halves.  If the upper lanes are
// stale (V1's 0xFF bytes) the recompiled SAD is far larger than the original.
// The reduced value is seeded from the argument so the oracle stays address-
// independent.  Same lifter path on x86-64 and i386, so both are covered.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MovdSpillRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MovdSpillRT, Verify) { roundTripX64(GetParam()); }

class X86MovdSpillRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MovdSpillRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kProbes = {

  // movd r32 -> xmm : zero-extends bits [127:32].
  {"movd_r32_spill",
   "unsigned f(unsigned a){\n"
   "  unsigned char buf[16] __attribute__((aligned(16)));\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"pcmpeqd %%xmm0,%%xmm0\\n\\t\"\n"
   "    \"movdqa %%xmm0, %1\\n\\t\"\n"
   "    \"movdqa %1, %%xmm1\\n\\t\"\n"
   "    \"paddb %%xmm1,%%xmm1\\n\\t\"\n"
   "    \"movd %2, %%xmm2\\n\\t\"\n"
   "    \"movdqa %%xmm2, %1\\n\\t\"\n"
   "    \"pxor %%xmm3,%%xmm3\\n\\t\"\n"
   "    \"psadbw %1, %%xmm3\\n\\t\"\n"
   "    \"pshufd $0xee,%%xmm3,%%xmm4\\n\\t\"\n"
   "    \"paddd %%xmm4,%%xmm3\\n\\t\"\n"
   "    \"movd %%xmm3, %0\\n\\t\"\n"
   "    :\"=r\"(out),\"=m\"(buf):\"r\"(a)\n"
   "    :\"xmm0\",\"xmm1\",\"xmm2\",\"xmm3\",\"xmm4\",\"cc\",\"memory\");\n"
   "  return out;}\n",
   {0x01030507ULL}, "MovdSpill", 0},

  // movss m32 -> xmm : memory form zero-extends bits [127:32].
  {"movss_m32_spill",
   "unsigned f(unsigned a){\n"
   "  unsigned char buf[16] __attribute__((aligned(16)));\n"
   "  volatile unsigned v=a;\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"pcmpeqd %%xmm0,%%xmm0\\n\\t\"\n"
   "    \"movdqa %%xmm0, %1\\n\\t\"\n"
   "    \"movdqa %1, %%xmm1\\n\\t\"\n"
   "    \"paddb %%xmm1,%%xmm1\\n\\t\"\n"
   "    \"movss %2, %%xmm2\\n\\t\"\n"
   "    \"movdqa %%xmm2, %1\\n\\t\"\n"
   "    \"pxor %%xmm3,%%xmm3\\n\\t\"\n"
   "    \"psadbw %1, %%xmm3\\n\\t\"\n"
   "    \"pshufd $0xee,%%xmm3,%%xmm4\\n\\t\"\n"
   "    \"paddd %%xmm4,%%xmm3\\n\\t\"\n"
   "    \"movd %%xmm3, %0\\n\\t\"\n"
   "    :\"=r\"(out),\"=m\"(buf):\"m\"(v)\n"
   "    :\"xmm0\",\"xmm1\",\"xmm2\",\"xmm3\",\"xmm4\",\"cc\",\"memory\");\n"
   "  return out;}\n",
   {0x090B0D0FULL}, "MovdSpill", 0},

  // movq m64 -> xmm : memory form zero-extends bits [127:64].
  {"movq_m64_spill",
   "unsigned f(unsigned a){\n"
   "  unsigned char buf[16] __attribute__((aligned(16)));\n"
   "  volatile unsigned long long v=(unsigned long long)a*0x100000001ULL+0x11ULL;\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"pcmpeqd %%xmm0,%%xmm0\\n\\t\"\n"
   "    \"movdqa %%xmm0, %1\\n\\t\"\n"
   "    \"movdqa %1, %%xmm1\\n\\t\"\n"
   "    \"paddb %%xmm1,%%xmm1\\n\\t\"\n"
   "    \"movq %2, %%xmm2\\n\\t\"\n"
   "    \"movdqa %%xmm2, %1\\n\\t\"\n"
   "    \"pxor %%xmm3,%%xmm3\\n\\t\"\n"
   "    \"psadbw %1, %%xmm3\\n\\t\"\n"
   "    \"pshufd $0xee,%%xmm3,%%xmm4\\n\\t\"\n"
   "    \"paddd %%xmm4,%%xmm3\\n\\t\"\n"
   "    \"movd %%xmm3, %0\\n\\t\"\n"
   "    :\"=r\"(out),\"=m\"(buf):\"m\"(v)\n"
   "    :\"xmm0\",\"xmm1\",\"xmm2\",\"xmm3\",\"xmm4\",\"cc\",\"memory\");\n"
   "  return out;}\n",
   {0x13171D1FULL}, "MovdSpill", 0},

  // movsd m64 -> xmm : memory form zero-extends bits [127:64].
  {"movsd_m64_spill",
   "unsigned f(unsigned a){\n"
   "  unsigned char buf[16] __attribute__((aligned(16)));\n"
   "  volatile unsigned long long v=(unsigned long long)a*0x100000001ULL+0x23ULL;\n"
   "  unsigned out;\n"
   "  __asm__ volatile(\n"
   "    \"pcmpeqd %%xmm0,%%xmm0\\n\\t\"\n"
   "    \"movdqa %%xmm0, %1\\n\\t\"\n"
   "    \"movdqa %1, %%xmm1\\n\\t\"\n"
   "    \"paddb %%xmm1,%%xmm1\\n\\t\"\n"
   "    \"movsd %2, %%xmm2\\n\\t\"\n"
   "    \"movdqa %%xmm2, %1\\n\\t\"\n"
   "    \"pxor %%xmm3,%%xmm3\\n\\t\"\n"
   "    \"psadbw %1, %%xmm3\\n\\t\"\n"
   "    \"pshufd $0xee,%%xmm3,%%xmm4\\n\\t\"\n"
   "    \"paddd %%xmm4,%%xmm3\\n\\t\"\n"
   "    \"movd %%xmm3, %0\\n\\t\"\n"
   "    :\"=r\"(out),\"=m\"(buf):\"m\"(v)\n"
   "    :\"xmm0\",\"xmm1\",\"xmm2\",\"xmm3\",\"xmm4\",\"cc\",\"memory\");\n"
   "  return out;}\n",
   {0x25292B2DULL}, "MovdSpill", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X64, X64MovdSpillRT, ::testing::ValuesIn(kProbes),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X86, X86MovdSpillRT, ::testing::ValuesIn(kProbes),
                         rtTCName);
