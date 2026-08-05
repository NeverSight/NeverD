//===- AllPlatform_OptStress312RTTests.cpp - -O0 indirect stack args ------===//
//
// -O0 kernels calling THROUGH A FUNCTION POINTER with MORE integer arguments
// than the register-argument file holds — the KNOWN-OPEN #2 defect flagged by
// OptStress310: ARM32 indirect-call STACK-argument recovery.
//
// AAPCS passes the first four integer arguments in r0-r3 and the rest on the
// stack; an indirect call with >4 integer arguments therefore spills arguments
// 5+ to the outgoing-argument area, and the (indirectly-reached) callee reads
// them back as incoming stack parameters at [entry_sp + 0/4].
//
// The breaking defect was a VARIADIC false-positive: an ordinary -O0 arm32
// callee spills its first argument register r0 into the top frame slot
// (entry_sp - slot), which detectVariadic mistook for a variadic GP save area
// (whose tell-tale is really the LAST register r3 sitting there, contiguous
// with the overflow area).  Being "variadic", the callee skipped stack-param
// recovery and read e/f as raw out-of-frame memory -> garbage.  Fixed by
// requiring specifically the last parameter register at entry_sp-slot.
//
//   * ind6 - 6 int args (4 register + 2 stack on arm32) via a function pointer.
//   * ind8 - 8 int args (4 register + 4 stack on arm32) via a function pointer.
//
// x86-64 / aarch64 carry 6-8 integer arguments entirely in registers; i386 puts
// every argument on the stack (cdecl).  An opaque `asm("" : "+r"(fp))` barrier
// keeps the pointer from being devirtualized; the entry is DEFINED FIRST so it
// lands at CODE_BASE.  Each callee genuinely consumes every argument so a
// dropped stack argument changes the result.  Deterministic (LCG-seeded).  All
// four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress312RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress312RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress312RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress312RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress312RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress312RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress312RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress312RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress312TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 6 int args (arm32: r0-r3 + 2 stack) through a function pointer.
    {p+"_ind6",
     "static int "+p+"_t6(int a,int b,int c,int d,int e,int f) __attribute__((noinline));\n"
     +t+" "+p+"_ind6("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  int (*fp)(int,int,int,int,int,int) = "+p+"_t6; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + fp((int)w,(int)(w>>3),(int)(w>>7),\n"
     "                       (int)(w>>11),(int)(w>>17),(int)(w>>23)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_t6(int a,int b,int c,int d,int e,int f){\n"
     "  return a*2 - b*3 + c*5 - d*7 + e*11 - f*13 + (a^f) + (c&d); }\n",
     {0x1234u}, "OptStress312", Opt},

    // 8 int args (arm32: r0-r3 + 4 stack) through a function pointer.
    {p+"_ind8",
     "static int "+p+"_t8(int a,int b,int c,int d,int e,int f,int g,int h) __attribute__((noinline));\n"
     +t+" "+p+"_ind8("+t+" a){ unsigned w=(unsigned)a^0x5A5Au; long long acc=0;\n"
     "  int (*fp)(int,int,int,int,int,int,int,int) = "+p+"_t8; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1664525u+1013904223u;\n"
     "    acc = acc*131 + fp((int)w,(int)(w>>2),(int)(w>>5),(int)(w>>9),\n"
     "                       (int)(w>>13),(int)(w>>19),(int)(w>>24),(int)(w>>28)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_t8(int a,int b,int c,int d,int e,int f,int g,int h){\n"
     "  return a - b*2 + c*3 - d*4 + e*5 - f*6 + g*7 - h*8 + (e^h) + (f&g); }\n",
     {0x7F00u}, "OptStress312", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress312TC("x64o312", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress312TC("x86o312", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress312TC("a64o312", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress312TC("armo312", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress312, X64OptStress312RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress312, X86OptStress312RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress312, A64OptStress312RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress312, ARM32OptStress312RT, ::testing::ValuesIn(kARM), rtTCName);
