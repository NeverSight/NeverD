//===- AllPlatform_OptStress310RTTests.cpp - -O0 indirect calls ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O0 kernels calling through a FUNCTION POINTER — the indirect-call (INDIR_CALL)
// argument/return-recovery path, distinct from the direct-call path probed by
// 305/306/307.  An opaque `asm("" : "+r"(fp))` barrier keeps the pointer from
// being devirtualized so a genuine indirect call is emitted.
//
//   * ind3 - 3 int args via a function pointer (register-only on every target).
//
// FOLLOW-UPS (two distinct defects this probe surfaced; both now RESOLVED in
// dedicated probes, not exercised here):
//
//   1. [FIXED — OptStress311] Wide (i64) return through an INDIRECT-only callee.
//      On i386/arm32 a `long long` is returned in a register pair (EDX:EAX /
//      R1:R0).  The pair splice that gives such a callee an i64 return type was
//      driven by Pipeline's WideRetCallees, populated only from DIRECT call
//      sites; a function reached *only* through a function pointer kept a
//      low-word-only i32 return while the indirect call site expected i64, so
//      the high word was wrong.  Fixed by also widening an address-taken
//      register-pair callee when the program has an indirect i64 call.
//
//   2. [FIXED — OptStress312] ARM32 indirect-call STACK-argument recovery.  An
//      indirect call passing arguments beyond r0-r3 dropped the indirectly-
//      reached callee's incoming stack params: an ordinary -O0 callee spills its
//      FIRST arg register r0 into the top frame slot (entry_sp-slot), which
//      detectVariadic mistook for a variadic GP save area (whose tell-tale is
//      really the LAST register r3 there, contiguous with the overflow area).
//      Being "variadic", the callee skipped stack-param recovery and read the
//      [sp]/[sp+4] args as raw out-of-frame memory.  Fixed by requiring
//      specifically the last parameter register (r3) at entry_sp-slot.
//
// The entry function is DEFINED FIRST so it lands at the emulation entry
// (CODE_BASE); the pointed-to target follows.  Deterministic (LCG-seeded).
// All four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress310RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress310RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress310RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress310RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress310RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress310RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress310RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress310RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress310TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 3 int args through a function pointer (register-only on every ABI).
    {p+"_ind3",
     "static int "+p+"_t(int a,int b,int c) __attribute__((noinline));\n"
     +t+" "+p+"_ind3("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  int (*fp)(int,int,int) = "+p+"_t; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + fp((int)w,(int)(w>>5),(int)(w>>11)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_t(int a,int b,int c){ return a*3 - b*5 + c*7 + (a^c); }\n",
     {0x1234u}, "OptStress310", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress310TC("x64o310", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress310TC("x86o310", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress310TC("a64o310", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress310TC("armo310", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress310, X64OptStress310RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress310, X86OptStress310RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress310, A64OptStress310RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress310, ARM32OptStress310RT, ::testing::ValuesIn(kARM), rtTCName);
