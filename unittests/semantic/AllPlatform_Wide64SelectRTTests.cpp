//===- AllPlatform_Wide64SelectRTTests.cpp - 64-bit select on 32-bit -C++-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for *selecting whole 64-bit values* on the 32-bit targets,
// where a `long long` is a register pair and a `cond ? a64 : b64` must move both
// halves together off one two-word comparison.  This is distinct from
// AllPlatform_Wide64On32RTTests (i64 arithmetic folded to scalar flags) and
// AllPlatform_Cmp64RTTests (i64 compares feeding a scalar branch): here the
// selected operand is itself 64-bit, so the lifter/optimizer must keep the lo/hi
// halves consistent (no half from a, the other from b) and reconstruct the
// signed/unsigned two-word compare that drives the select.  Kernels: i64 min/max,
// abs, clamp, three-way sign + conditional negate, a loop-carried "max + its
// index" cmov chain, and a select feeding further i64 math.  No 64-bit
// division (a soft-float/runtime libcall on 32-bit); each folds to a 32-bit hash,
// compared native vs lifted at -O2 across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64W64SelRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64W64SelRT, Verify) { roundTripX64(GetParam()); }
class X86W64SelRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86W64SelRT, Verify) { roundTripX86(GetParam()); }
class A64W64SelRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64W64SelRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32W64SelRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32W64SelRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeW64SelTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed and unsigned 64-bit min/max selected as whole values.
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a*0x9E3779B97F4A7C15ull|1ull;\n"
     "  for(int i=0;i<48;i++){\n"
     "    long long x=(long long)(acc^((unsigned long long)i<<40));\n"
     "    long long y=(long long)(acc*0x100000001ull) - 0x4000000000ll;\n"
     "    long long smn=x<y?x:y, smx=x>y?x:y;\n"
     "    unsigned long long ux=(unsigned long long)x, uy=(unsigned long long)y;\n"
     "    unsigned long long umn=ux<uy?ux:uy, umx=ux>uy?ux:uy;\n"
     "    acc=acc*31ull+(unsigned long long)smn+(unsigned long long)smx*7ull\n"
     "       +umn*3ull+umx; }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x41ULL}, "W64Sel", 2},

    // 64-bit absolute value (cond ? -x : x over the whole pair).
    {p+"_abs",
     t+" "+p+"_abs("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  for(int i=0;i<48;i++){\n"
     "    long long x=(long long)(acc*0xC2B2AE3D27D4EB4Full) - (long long)((unsigned long long)i<<55);\n"
     "    long long ax=x<0?-x:x;\n"
     "    acc=acc*31ull+(unsigned long long)ax+(unsigned long long)(ax>>17); }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x53ULL}, "W64Sel", 2},

    // 64-bit clamp to a runtime [lo,hi] window (two whole-value selects).
    {p+"_clamp",
     t+" "+p+"_clamp("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  for(int i=0;i<48;i++){\n"
     "    long long x=(long long)(acc^(acc<<13));\n"
     "    long long lo=-(long long)((unsigned long long)(i+1)<<32);\n"
     "    long long hi=(long long)((unsigned long long)(i+3)<<33);\n"
     "    long long cl=x<lo?lo:(x>hi?hi:x);\n"
     "    acc=acc*1000003ull+(unsigned long long)cl; }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x67ULL}, "W64Sel", 2},

    // Three-way sign (-1/0/1) and conditional negate of a 64-bit value.
    {p+"_sign",
     t+" "+p+"_sign("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  for(int i=0;i<48;i++){\n"
     "    long long x=(long long)(acc*0x165667B19E3779F9ull) - (long long)((unsigned long long)i<<48);\n"
     "    long long sg=(x>0)?1:((x<0)?-1:0);\n"
     "    long long cn=(i&1)?-x:x;\n"
     "    acc=acc*31ull+(unsigned long long)(sg*1000003ll)+(unsigned long long)cn; }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0x71ULL}, "W64Sel", 2},

    // Loop-carried "running max + its index": a 64-bit value and a paired int
    // both updated under the same condition (cmov chain over the pair).
    {p+"_argmax",
     t+" "+p+"_argmax("+t+" a){\n"
     "  unsigned long long st=(unsigned long long)(unsigned)a|1ull;\n"
     "  long long best=(long long)0x8000000000000000ull; int bi=-1; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    st=st*6364136223846793005ull+1442695040888963407ull;\n"
     "    long long v=(long long)st;\n"
     "    if(v>best){ best=v; bi=i; }\n"
     "    h=h*131u+(unsigned)(best>>40)+(unsigned)bi; }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)(best^(best>>32))); }\n",
     {0x9CULL}, "W64Sel", 2},

    // Select result feeding further 64-bit math (no branch; pure data select).
    {p+"_selmath",
     t+" "+p+"_selmath("+t+" a){\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  for(int i=0;i<48;i++){\n"
     "    unsigned long long u=acc*0x9E3779B97F4A7C15ull;\n"
     "    unsigned long long pick=((acc>>i%64)&1ull)?u:(~u);\n"
     "    unsigned long long mixed=pick^(pick>>29);\n"
     "    mixed*=0xBF58476D1CE4E5B9ull; mixed^=mixed>>32;\n"
     "    acc=acc+mixed+((mixed&1ull)?0x100000001ull:0ull); }\n"
     "  return ("+t+")(unsigned long)(unsigned)(acc^(acc>>32)); }\n",
     {0xABULL}, "W64Sel", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeW64SelTC("x64w64s", "long");
static const std::vector<RoundTripTC> kX86 = makeW64SelTC("x86w64s", "int");
static const std::vector<RoundTripTC> kA64 = makeW64SelTC("a64w64s", "long");
static const std::vector<RoundTripTC> kARM = makeW64SelTC("armw64s", "int");

INSTANTIATE_TEST_SUITE_P(W64Sel, X64W64SelRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(W64Sel, X86W64SelRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(W64Sel, A64W64SelRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(W64Sel, ARM32W64SelRT, ::testing::ValuesIn(kARM), rtTCName);
