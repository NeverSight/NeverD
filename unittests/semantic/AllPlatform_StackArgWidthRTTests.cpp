//===- AllPlatform_StackArgWidthRTTests.cpp - mixed-width stack args -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The call-ABI probes added in #405-#417 forwarded mostly uniform 4-byte stack
// arguments.  These probes cross the call boundary with arguments of *mixed*
// widths and beyond the register-argument count, exercising the stack-argument
// recovery where an 8-byte argument (`long long`) occupies two 4-byte i386 slots
// next to 4-byte `int` slots — a case the slot-size assumption in recoverCallAbi
// (`StackOff / PointerSize`) and the #417 wide-load/store handling must keep
// straight.  Every kernel is integer-only (64-bit math lowers to add/adc/shift,
// never a libcall), folds to one integer return, and is checked native vs lifted
// at -O2 on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StackArgWidthRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StackArgWidthRT, Verify) { roundTripX64(GetParam()); }
class X86StackArgWidthRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86StackArgWidthRT, Verify) { roundTripX86(GetParam()); }
class A64StackArgWidthRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StackArgWidthRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32StackArgWidthRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32StackArgWidthRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeStackArgWidthTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Alternating int / long long arguments: on i386 the 8-byte longs straddle
    // two stack slots interleaved with 4-byte ints, stressing slot indexing.
    {p+"_mixw8",
     "static unsigned "+p+"_h8(int,long long,int,long long,int,long long,int,long long)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_mixw8("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_h8((int)v,(long long)v+1,(int)(v^2u),\n"
     "    (long long)v*3,(int)(v+4u),(long long)v-5,(int)(v|6u),(long long)v+7); }\n"
     "static unsigned "+p+"_h8(int a0,long long a1,int a2,long long a3,\n"
     "                         int a4,long long a5,int a6,long long a7){\n"
     "  unsigned h=(unsigned)a0;\n"
     "  h=h*131u+(unsigned)(a1^(a1>>32));\n"
     "  h=h*131u+(unsigned)a2;\n"
     "  h=h*131u+(unsigned)(a3^(a3>>32));\n"
     "  h=h*131u+(unsigned)a4;\n"
     "  h=h*131u+(unsigned)(a5^(a5>>32));\n"
     "  h=h*131u+(unsigned)a6;\n"
     "  h=h*131u+(unsigned)(a7^(a7>>32));\n"
     "  return h; }\n",
     {0x41ULL}, "StackArgW", 2},

    // Six long long arguments: on i386 all six are 8-byte stack arguments.
    {p+"_many6ll",
     "static unsigned "+p+"_h6("
     "long long,long long,long long,long long,long long,long long)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_many6ll("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_h6((long long)v,(long long)v+1,\n"
     "    (long long)v*3,(long long)v^0x55,(long long)v-2,(long long)v+9); }\n"
     "static unsigned "+p+"_h6(long long a0,long long a1,long long a2,\n"
     "                         long long a3,long long a4,long long a5){\n"
     "  unsigned long long h=(unsigned long long)a0;\n"
     "  h=h*1000003u+(unsigned long long)a1;\n"
     "  h=h*1000003u+(unsigned long long)a2;\n"
     "  h=h*1000003u+(unsigned long long)a3;\n"
     "  h=h*1000003u+(unsigned long long)a4;\n"
     "  h=h*1000003u+(unsigned long long)a5;\n"
     "  return (unsigned)(h^(h>>32)); }\n",
     {0x53ULL}, "StackArgW", 2},

    // Ten int arguments (beyond every register-argument count), weighted sum.
    {p+"_wide10",
     "static unsigned "+p+"_w10(unsigned,unsigned,unsigned,unsigned,unsigned,\n"
     "                         unsigned,unsigned,unsigned,unsigned,unsigned)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_wide10("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_w10(v,v+1u,v+2u,v+3u,v+4u,\n"
     "    v+5u,v+6u,v+7u,v+8u,v+9u); }\n"
     "static unsigned "+p+"_w10(unsigned a0,unsigned a1,unsigned a2,unsigned a3,\n"
     "                          unsigned a4,unsigned a5,unsigned a6,unsigned a7,\n"
     "                          unsigned a8,unsigned a9){\n"
     "  return (((((((((a0*31u+a1)*31u+a2)*31u+a3)*31u+a4)*31u+a5)*31u+a6)\n"
     "          *31u+a7)*31u+a8)*31u+a9); }\n",
     {0x67ULL}, "StackArgW", 2},

    // Forward four long long arguments straight through to a second callee.
    {p+"_llfwd",
     "static unsigned "+p+"_use4("
     "long long,long long,long long,long long) __attribute__((noinline));\n"
     "static unsigned "+p+"_fwd4("
     "long long,long long,long long,long long) __attribute__((noinline));\n"
     +t+" "+p+"_llfwd("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_fwd4((long long)v,(long long)v+1,\n"
     "    (long long)v*5,(long long)v^0x33); }\n"
     "static unsigned "+p+"_fwd4(long long a0,long long a1,long long a2,long long a3){\n"
     "  return "+p+"_use4(a0,a1,a2,a3); }\n"
     "static unsigned "+p+"_use4(long long a0,long long a1,long long a2,long long a3){\n"
     "  unsigned long long h=(unsigned long long)a0;\n"
     "  h=h*1000003u+(unsigned long long)a1;\n"
     "  h=h*1000003u+(unsigned long long)a2;\n"
     "  h=h*1000003u+(unsigned long long)a3;\n"
     "  return (unsigned)(h^(h>>32)); }\n",
     {0x29ULL}, "StackArgW", 2},

    // Mixed int + long long with a trailing int after the wide slots, then an
    // accumulator loop over the call result.
    {p+"_mixacc",
     "static unsigned "+p+"_hm("
     "unsigned,long long,unsigned,long long,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_mixacc("+t+" a){ unsigned v=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){\n"
     "    acc=acc*131u+"+p+"_hm(v+(unsigned)i,(long long)v*(i+1),v^(unsigned)i,\n"
     "      (long long)v-i,v+(unsigned)(i*3)); }\n"
     "  return ("+t+")(unsigned)acc; }\n"
     "static unsigned "+p+"_hm(unsigned a0,long long a1,unsigned a2,\n"
     "                         long long a3,unsigned a4){\n"
     "  unsigned h=a0*31u+(unsigned)(a1^(a1>>32));\n"
     "  h=h*31u+a2; h=h*31u+(unsigned)(a3^(a3>>32)); h=h*31u+a4;\n"
     "  return h; }\n",
     {0x35ULL}, "StackArgW", 2},
  };
}
// clang-format on

// All four targets now recover the mixed int + `long long` argument forms.
// i386 splits an 8-byte `long long` across EDX + the first stack slot (#420);
// ARM AAPCS passes it in an even-odd register pair (r2:r3 with r1 wasted) and
// 8-byte-aligns it on the stack, leaving gap lanes the call-ABI recovery now
// fills with zero up to the callee arity (#421).
static const std::vector<RoundTripTC> kX64 = makeStackArgWidthTC("x64saw", "long");
static const std::vector<RoundTripTC> kX86 = makeStackArgWidthTC("x86saw", "int");
static const std::vector<RoundTripTC> kA64 = makeStackArgWidthTC("a64saw", "long");
static const std::vector<RoundTripTC> kARM = makeStackArgWidthTC("armsaw", "int");

INSTANTIATE_TEST_SUITE_P(StackArgW, X64StackArgWidthRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(StackArgW, X86StackArgWidthRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(StackArgW, A64StackArgWidthRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(StackArgW, ARM32StackArgWidthRT, ::testing::ValuesIn(kARM), rtTCName);
