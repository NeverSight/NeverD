//===- AllPlatform_StackArgEscapeRTTests.cpp - overflow stack-arg guards *-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Guards for detectStackParams' handling of integer arguments that overflow the
// parameter registers onto the stack -- the AArch64/ARM/x64 stack-argument path
// the #493 mutable-home work touched but had no dedicated probe for (only i386
// cdecl was covered, by X86_32_StackParamUpdate).  The escaped-address scan must
// NOT treat the bare stack pointer as a slot escaping: SP is the base of every
// [sp+k] address, and on AArch64/ARM the first stack slot is at [sp+0], so the
// block-entry `COPY SP SP` would otherwise mis-mark slot 0 mutable and corrupt
// every read-only overflow argument there (the ManyArgAbi regression #494 fixed).
//
//   * roovf   - 9 read-only overflow args summed: every slot must fold to its
//               incoming parameter (direct cover of the bare-SP mis-mark).
//   * escstk  - an overflow arg copied to a local whose address escapes to a
//               noinline writer (the local escapes; the stack home stays read-only).
//   * updstk  - an overflow arg updated in a register-carried local loop.
//   * wideovf - 8-byte long long overflow args (two-slot WidePair reconstruction
//               on 32-bit targets; boundary in-register case on 64-bit).
//
// Every callee is external + noinline so clang keeps a standard-ABI call with
// real stack arguments.  Kernels use only add/mul (no 64-bit lib helper), -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StackArgEscapeRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StackArgEscapeRT, Verify) { roundTripX64(GetParam()); }
class A64StackArgEscapeRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StackArgEscapeRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32StackArgEscapeRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32StackArgEscapeRT, Verify) { roundTripARM32(GetParam()); }
class X86StackArgEscapeRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86StackArgEscapeRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeStackArgEscapeTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 9 read-only overflow integer args (slot 0 must fold to its parameter).
    {p+"_roovf",
     t+" ro9("+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+")"
     " __attribute__((noinline));\n"
     +t+" "+p+"_roovf("+t+" x){\n"
     "  return ro9(x,x+1,x+2,x+3,x+4,x+5,x+6,x+7,x+8); }\n"
     +t+" ro9("+t+" a,"+t+" b,"+t+" c,"+t+" d,"+t+" e,"+t+" f,"+t+" g,"+t+" h,"+t+" i){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i; }\n",
     {0x1234ULL}, "StackArgEscape", 2},

    // An overflow arg copied to a local whose address escapes to a noinline
    // writer: the genuine escape must still be detected (the read at the consuming
    // call op), while the stack home itself stays a read-only parameter.
    {p+"_escstk",
     "void eadd("+t+" *p) __attribute__((noinline));\n"
     +t+" es9("+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+")"
     " __attribute__((noinline));\n"
     +t+" "+p+"_escstk("+t+" x){\n"
     "  return es9(x,x+1,x+2,x+3,x+4,x+5,x+6,x+7,x+8); }\n"
     "void eadd("+t+" *p){ *p += 0x1000; }\n"
     +t+" es9("+t+" a,"+t+" b,"+t+" c,"+t+" d,"+t+" e,"+t+" f,"+t+" g,"+t+" h,"+t+" i){\n"
     "  "+t+" v=i; eadd(&v); return a+b+c+d+e+f+g+h+v; }\n",
     {0x2222ULL}, "StackArgEscape", 2},

    // An overflow arg updated in a register-carried local loop.
    {p+"_updstk",
     t+" up9("+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+","+t+")"
     " __attribute__((noinline));\n"
     +t+" "+p+"_updstk("+t+" x){\n"
     "  return up9(x,x+1,x+2,x+3,x+4,x+5,x+6,x+7,x+8); }\n"
     +t+" up9("+t+" a,"+t+" b,"+t+" c,"+t+" d,"+t+" e,"+t+" f,"+t+" g,"+t+" h,"+t+" i){\n"
     "  for(int k=0;k<7;k++) i=i*3+1;\n"
     "  return a+b+c+d+e+f+g+h+i; }\n",
     {0x3333ULL}, "StackArgEscape", 2},

    // 8-byte overflow args: WidePair (two 4-byte slots) on 32-bit targets.
    {p+"_wideovf",
     "long long w5(long long,long long,long long,long long,long long)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_wideovf("+t+" x){ long long a=(long long)x;\n"
     "  return ("+t+")w5(a,a+1,a+2,a+3,a+4); }\n"
     "long long w5(long long a,long long b,long long c,long long d,long long e){\n"
     "  return a+b*2+c*3+d*4+e*5; }\n",
     {0x40ULL}, "StackArgEscape", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeStackArgEscapeTC("x64se", "long");
static const std::vector<RoundTripTC> kA64 = makeStackArgEscapeTC("a64se", "long");
static const std::vector<RoundTripTC> kARM = makeStackArgEscapeTC("armse", "int");
static const std::vector<RoundTripTC> kX86 = makeStackArgEscapeTC("x86se", "int");

INSTANTIATE_TEST_SUITE_P(StackArgEscape, X64StackArgEscapeRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(StackArgEscape, A64StackArgEscapeRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(StackArgEscape, ARM32StackArgEscapeRT, ::testing::ValuesIn(kARM), rtTCName);
INSTANTIATE_TEST_SUITE_P(StackArgEscape, X86StackArgEscapeRT, ::testing::ValuesIn(kX86), rtTCName);
