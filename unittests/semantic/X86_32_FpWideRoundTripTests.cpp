//===- X86_32_FpWideRoundTripTests.cpp - i386 FP + 64-bit roundtrip *- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 straight-line SSE float/double (constants reached through the PIC get-PC
// constant pool) and 64-bit (long long) arithmetic lowered into EDX:EAX register
// pairs.  All kernels take int arguments and fold their result into the int
// return (EAX) so the cdecl harness can compare them.
//
// Scope note: aggressively optimized FP/64-bit *loops* (clang -O2 unrolls and
// auto-vectorizes them into heavy xmm stack spills plus many constant-pool loads
// through one get-PC GOT base) are a deeper, separate i386 target tracked
// separately, not covered here.  64-bit division is also excluded: i386 has no
// native 64-bit divide, so clang emits a __divdi3 library call the harness image
// does not provide.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86FpWideRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FpWideRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86FpWide = {
  // Single-precision SSE arithmetic with constant-pool literals (get-PC).
  {"x86_floatmix",
   "int x86_floatmix(int a){ float x=(float)a*1.5f+2.25f; x=x*x-0.5f; return (int)x; }\n",
   {0x29ULL}, "X86FpWide", 2, ""},

  // Double-precision SSE with division and a constant pool.
  {"x86_doublemix",
   "int x86_doublemix(int a){ double x=(double)a/3.0+1.0; x=x*7.0-2.5; return (int)(x*16.0); }\n",
   {0x3BULL}, "X86FpWide", 2, ""},

  // Straight-line 64-bit shifts, xor and multiply on an EDX:EAX register pair.
  {"x86_shift64",
   "int x86_shift64(int a){ unsigned long long v=((unsigned long long)(unsigned)a<<32)|(unsigned)(a*7+1);\n"
   "  v=(v<<13)|(v>>51); v^=v>>29; v*=0x2545F4914F6CDD1DULL;\n"
   "  return (int)((unsigned)v ^ (unsigned)(v>>32)); }\n",
   {0x2468ULL}, "X86FpWide", 2, ""},

  // Straight-line 64-bit widening multiply (imul -> EDX:EAX) and carry add.
  {"x86_wmul64",
   "int x86_wmul64(int a){ long long p=(long long)a*(long long)(a*3+7);\n"
   "  unsigned long long s=(unsigned long long)p + 0x100000000ULL + (unsigned)a;\n"
   "  return (int)((unsigned)s ^ (unsigned)(s>>32)); }\n",
   {0x1357ULL}, "X86FpWide", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86FpWide, X86FpWideRT, ::testing::ValuesIn(kX86FpWide),
                         rtTCName);
