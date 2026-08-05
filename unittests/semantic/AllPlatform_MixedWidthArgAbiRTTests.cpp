//===- AllPlatform_MixedWidthArgAbiRTTests.cpp - mixed-width stack args *-C++*-=//
//
// Calling-ABI probes for argument lists that overflow the integer parameter
// registers with MIXED widths and pointers -- a corner the existing ManyArgAbi
// (uniform double/long) and StackArgEscape (uniform long) probes do not reach.
// These stress slot packing, narrow-value sign/zero extension of stack arguments,
// and -- crucially -- pointer arguments symbolized THROUGH a stack slot (combining
// the #473 pointer-arg symbolization with the overflow stack-arg path).
//
//   * narrowovf - 12 char/short args: narrow stack slots + signed extension.
//   * mixw      - char/short/int/long mixed widths overflowing to the stack.
//   * ptrmix    - &G[k] pointers interleaved with ints, overflowing to the stack:
//                 each stack-passed pointer must still be symbolized to the
//                 recompiled global, else the callee dereferences a stale VA.
//   * umix      - unsigned char/short mixed: zero extension of narrow stack slots.
//
// Every callee is external + noinline so clang keeps a standard-ABI call with
// real stack arguments.  Kernels use only add/small mul (no 64-bit lib helper).
// -O2, all four platforms.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MixedWidthArgRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MixedWidthArgRT, Verify) { roundTripX64(GetParam()); }
class A64MixedWidthArgRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MixedWidthArgRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32MixedWidthArgRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MixedWidthArgRT, Verify) { roundTripARM32(GetParam()); }
class X86MixedWidthArgRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MixedWidthArgRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeMixedWidthArgTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 12 signed char/short args: narrow stack slots, signed extension on read.
    {p+"_narrowovf",
     "int n12(char,short,char,short,char,short,char,short,char,short,char,short)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_narrowovf("+t+" x){ int v=(int)x;\n"
     "  return ("+t+")n12((char)v,(short)(v*3),(char)(v+5),(short)(v-7),(char)(v*2),\n"
     "    (short)(v+11),(char)(v-13),(short)(v*5),(char)(v+17),(short)(v-19),\n"
     "    (char)(v*7),(short)(v+23)); }\n"
     "int n12(char a,short b,char c,short d,char e,short f,char g,short h,char i,\n"
     "  short j,char k,short l){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j+11*k+12*l; }\n",
     {0x29ULL}, "MixedWidthArg", 2},

    // char/short/int/long mixed widths overflowing to the stack.
    {p+"_mixw",
     "long m10(char,short,int,long,char,short,int,long,char,int)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_mixw("+t+" x){ long v=(long)(int)x;\n"
     "  return ("+t+")m10((char)v,(short)(v+1),(int)(v+2),v+3,(char)(v+4),\n"
     "    (short)(v+5),(int)(v+6),v+7,(char)(v+8),(int)(v+9)); }\n"
     "long m10(char a,short b,int c,long d,char e,short f,int g,long h,char i,int j){\n"
     "  return (long)a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j; }\n",
     {0x3bULL}, "MixedWidthArg", 2},

    // &G[k] pointers interleaved with ints, overflowing to the stack: every
    // stack-passed pointer must be symbolized to the recompiled global.
    {p+"_ptrmix",
     "static int G[16]={3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59};\n"
     "long pc(int*,int,int*,int,int*,int,int*,int,int*,int)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_ptrmix("+t+" x){ unsigned u=(unsigned)x;\n"
     "  return ("+t+")pc(&G[u&7],1,&G[(u>>3)&7],2,&G[(u>>6)&7],3,\n"
     "    &G[(u>>9)&7],4,&G[(u>>12)&7],5); }\n"
     "long pc(int*p0,int a,int*p1,int b,int*p2,int c,int*p3,int d,int*p4,int e){\n"
     "  return (long)*p0*a+*p1*b+*p2*c+*p3*d+*p4*e; }\n",
     {0x1357ULL}, "MixedWidthArg", 2},

    // unsigned char/short mixed: zero extension of narrow stack slots.
    {p+"_umix",
     "unsigned u12(unsigned char,unsigned short,unsigned char,unsigned short,\n"
     "  unsigned char,unsigned short,unsigned char,unsigned short,unsigned char,\n"
     "  unsigned short,unsigned char,unsigned short) __attribute__((noinline));\n"
     +t+" "+p+"_umix("+t+" x){ unsigned v=(unsigned)x;\n"
     "  return ("+t+")u12((unsigned char)v,(unsigned short)(v*3),(unsigned char)(v+5),\n"
     "    (unsigned short)(v+7),(unsigned char)(v*2),(unsigned short)(v+11),\n"
     "    (unsigned char)(v+13),(unsigned short)(v*5),(unsigned char)(v+17),\n"
     "    (unsigned short)(v+19),(unsigned char)(v*7),(unsigned short)(v+23)); }\n"
     "unsigned u12(unsigned char a,unsigned short b,unsigned char c,unsigned short d,\n"
     "  unsigned char e,unsigned short f,unsigned char g,unsigned short h,\n"
     "  unsigned char i,unsigned short j,unsigned char k,unsigned short l){\n"
     "  return a+2u*b+3u*c+4u*d+5u*e+6u*f+7u*g+8u*h+9u*i+10u*j+11u*k+12u*l; }\n",
     {0x6dULL}, "MixedWidthArg", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeMixedWidthArgTC("x64mw", "long");
static const std::vector<RoundTripTC> kA64 = makeMixedWidthArgTC("a64mw", "long");
static const std::vector<RoundTripTC> kARM = makeMixedWidthArgTC("armmw", "int");
static const std::vector<RoundTripTC> kX86 = makeMixedWidthArgTC("x86mw", "int");

INSTANTIATE_TEST_SUITE_P(MixedWidthArg, X64MixedWidthArgRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MixedWidthArg, A64MixedWidthArgRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MixedWidthArg, ARM32MixedWidthArgRT, ::testing::ValuesIn(kARM), rtTCName);
INSTANTIATE_TEST_SUITE_P(MixedWidthArg, X86MixedWidthArgRT, ::testing::ValuesIn(kX86), rtTCName);
