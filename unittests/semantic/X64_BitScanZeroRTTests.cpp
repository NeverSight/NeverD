//===- X64_BitScanZeroRTTests.cpp - BSF/BSR zero-source dest -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the x86 BSF/BSR "zero source" quirk: when the source
// operand is 0, ZF=1 and the DESTINATION IS LEFT UNCHANGED.  The Intel manual
// calls the destination "undefined" in that case, but real hardware and
// QEMU/Unicorn preserve the prior destination value (QEMU uses a conditional
// move), and real programs depend on it.
//
// The lifter wrote the computed value unconditionally, which for a zero source
// is (Bits-1)-clz(0) = -1 — clobbering the destination.  Each "keep" probe
// seeds the destination register with a runtime sentinel (the second argument),
// runs bsf/bsr on a zero source, and returns the destination: original keeps
// the sentinel, the buggy lift returns -1.  The "nz" controls drive a non-zero
// source so the computed bit index is returned (and the sentinel ignored),
// confirming the normal path is unchanged.
//
// The pre-existing flag probes (X64_FlagEdgeProbeRTTests) deliberately returned
// "ZF only" on a zero source because the destination was not modelled — these
// probes close that gap.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitScanZeroRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitScanZeroRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64BitScanZero = {

  // ===== Zero source: destination must be left UNCHANGED (RED before fix). =====
  {"bsf_zero_keep",
   "long f(long a,long s){long r=s;"
   "__asm__ volatile(\"bsfq %1,%0\":\"+r\"(r):\"r\"(a):\"cc\");"
   "return r;}\n",
   {0, 0x123456789ABULL}, "BitScanZero"},

  {"bsr_zero_keep",
   "long f(long a,long s){long r=s;"
   "__asm__ volatile(\"bsrq %1,%0\":\"+r\"(r):\"r\"(a):\"cc\");"
   "return r;}\n",
   {0, 0x123456789ABULL}, "BitScanZero"},

  // 32-bit forms: dest unchanged AND the upper 32 bits zero-extended.
  {"bsf_zero32_keep",
   "long f(long a,long s){unsigned r=(unsigned)s;"
   "__asm__ volatile(\"bsfl %1,%0\":\"+r\"(r):\"r\"((unsigned)a):\"cc\");"
   "return (long)(unsigned)r;}\n",
   {0, 0xCAFEF00DULL}, "BitScanZero"},

  {"bsr_zero32_keep",
   "long f(long a,long s){unsigned r=(unsigned)s;"
   "__asm__ volatile(\"bsrl %1,%0\":\"+r\"(r):\"r\"((unsigned)a):\"cc\");"
   "return (long)(unsigned)r;}\n",
   {0, 0xCAFEF00DULL}, "BitScanZero"},

  // ZF must still be set on a zero source (fold ZF into the return alongside
  // the preserved destination).
  {"bsf_zero_zf_and_dst",
   "long f(long a,long s){long r=s;unsigned char zf;"
   "__asm__ volatile(\"bsfq %2,%0\\n\\tsetz %1\":\"+r\"(r),\"=r\"(zf):\"r\"(a):\"cc\");"
   "return ((unsigned long)zf<<48)|(r&0xFFFFFFFFFFFFULL);}\n",
   {0, 0x123456789ABULL}, "BitScanZero"},

  // ===== Non-zero source controls: computed index returned, sentinel ignored. =====
  {"bsf_nz_keep",
   "long f(long a,long s){long r=s;"
   "__asm__ volatile(\"bsfq %1,%0\":\"+r\"(r):\"r\"(a):\"cc\");"
   "return r;}\n",
   {0x100, 0x123456789ABULL}, "BitScanZero"},

  {"bsr_nz_keep",
   "long f(long a,long s){long r=s;"
   "__asm__ volatile(\"bsrq %1,%0\":\"+r\"(r):\"r\"(a):\"cc\");"
   "return r;}\n",
   {0x8000, 0x123456789ABULL}, "BitScanZero"},

  {"bsf_nz32_keep",
   "long f(long a,long s){unsigned r=(unsigned)s;"
   "__asm__ volatile(\"bsfl %1,%0\":\"+r\"(r):\"r\"((unsigned)a):\"cc\");"
   "return (long)(unsigned)r;}\n",
   {0x10000, 0xCAFEF00DULL}, "BitScanZero"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BitScanZero, X64BitScanZeroRT,
                         ::testing::ValuesIn(kX64BitScanZero), rtTCName);
