//===- AllPlatform_OptStress15RTTests.cpp - MedFlags probes ----*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Flag-reconstruction stress probes aimed squarely at NeverD's hand-written
// MedFlags pass, the historical source of signed-condition-fold / carry / OF
// bugs.  Earlier OptStress rounds leaned on unsigned carry and value paths;
// these concentrate the pressure on the *flag* side:
//
//   * ofdetect  - signed-overflow (OF) detection driving a branch (seto/jo or
//                 the sign-compare idiom MedFlags must fold consistently).
//   * cmp3way   - signed and unsigned three-way compares (-1/0/1) from one
//                 subtraction feeding SF/ZF/CF simultaneously.
//   * flagreuse - one compare consumed by a chained signed/equal/unsigned
//                 select cascade (same flags, multiple consumers).
//   * bytecmp   - 8/16-bit signed+unsigned compares (partial-flag from byte/word
//                 ops) summed as predicates.
//   * carryiter - a 16-bit add carry threaded across loop iterations (the flag
//                 is loop-carried, exercising cross-block flag liveness).
//   * negflags  - neg/dec/sub flag side effects (CF/ZF/SF) consumed by a select.
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper, so all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress15RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress15RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress15RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress15RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress15RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress15RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress15RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress15RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress15TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed-overflow detection driving a branch.
    {p+"_ofdetect",
     t+" "+p+"_ofdetect("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int acc=0; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; int v=(int)x>>2;\n"
     "    int s=acc+v;\n"
     "    int ovf=((acc>0&&v>0&&s<0)||(acc<0&&v<0&&s>=0));\n"
     "    acc=ovf?(acc>>1):s; h=h*131u+(unsigned)acc+(unsigned)ovf; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress15", 2},

    // Signed and unsigned three-way compares from one subtraction.
    {p+"_cmp3way",
     t+" "+p+"_cmp3way("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    int v=(int)x, w=(int)(x>>7); unsigned uv=x, uw=x>>7;\n"
     "    int sc=(v>w)-(v<w); int uc=(int)(uv>uw)-(int)(uv<uw);\n"
     "    h=h*131u+(unsigned)(sc*3+uc); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress15", 2},

    // One compare consumed by a chained signed/equal/unsigned select cascade.
    {p+"_flagreuse",
     t+" "+p+"_flagreuse("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int acc=0; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    int v=(int)x>>3, w=(int)(x>>11);\n"
     "    int d=v-w;\n"
     "    int r=(d<0)?(acc+v):((d==0)?(acc^w):(acc-w));\n"
     "    r+=((unsigned)v<(unsigned)w)?5:0;\n"
     "    acc=r; h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress15", 2},

    // 8/16-bit signed+unsigned compares summed as predicates.
    {p+"_bytecmp",
     t+" "+p+"_bytecmp("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned char bx=(unsigned char)x, by=(unsigned char)(x>>8);\n"
     "    unsigned short wx=(unsigned short)(x>>3), wy=(unsigned short)(x>>17);\n"
     "    unsigned pr=(bx<by)+(unsigned)((signed char)bx<(signed char)by)\n"
     "      +(wx>wy)+(unsigned)((short)wx>(short)wy)+(bx==by);\n"
     "    h=h*131u+pr*7u+bx+wx; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress15", 2},

    // A 16-bit add carry threaded across loop iterations (loop-carried flag).
    {p+"_carryiter",
     t+" "+p+"_carryiter("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, carry=0, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned s=(x&0xffffu)+(x>>16)+carry;\n"
     "    carry=s>>16; h=h*131u+(s&0xffffu)+carry; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress15", 2},

    // neg/dec/sub flag side effects consumed by a select.
    {p+"_negflags",
     t+" "+p+"_negflags("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int acc=0; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; int v=(int)x>>4;\n"
     "    int n=-v; int d=v-1;\n"
     "    acc+=(n<0)?v:(-v); acc+=(d==0)?100:((d<0)?-7:3);\n"
     "    acc^=(v!=0)?(acc>>1):0; h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress15", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress15TC("x64o15", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress15TC("x86o15", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress15TC("a64o15", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress15TC("armo15", "int");

INSTANTIATE_TEST_SUITE_P(OptStress15, X64OptStress15RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress15, X86OptStress15RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress15, A64OptStress15RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress15, ARM32OptStress15RT, ::testing::ValuesIn(kARM), rtTCName);
