//===- AllPlatform_OptStress5RTTests.cpp - optimizer-path stressors -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fifth batch of high-yield roundtrip probes for NeverD's own MedIR passes,
// deliberately distinct from OptStress / 2 / 3 / 4: short-circuit boolean
// ladders that lower to real branch lattices (MedCFGSimplify + MedFlags),
// a single subtraction whose carry/zero/sign/borrow flags feed several distinct
// conditions (MedFlags must not corrupt one consumer while folding another),
// a balanced nested-ternary tree mixing signed/unsigned min/max (select trees),
// a stack buffer with same-iteration store-then-load through possibly-aliasing
// indices (memory dependence / store-to-load forwarding), a branchless boolean
// mask combined from several comparisons, and a doubly-nested loop with a
// runtime inner trip count and an inner break (loop PHI + CFG).  Each kernel
// returns a value-dependent hash compiled at -O2; the roundtrip compares native
// vs lifted execution across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Opt5RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Opt5RT, Verify) { roundTripX64(GetParam()); }
class X86Opt5RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Opt5RT, Verify) { roundTripX86(GetParam()); }
class A64Opt5RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Opt5RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Opt5RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Opt5RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOpt5TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Short-circuit boolean ladders: && / || with side-effect-free predicates
    // that clang lowers to a lattice of conditional branches (not just select).
    // Stresses MedCFGSimplify block merging and MedFlags on each branch arm.
    {p+"_scbool",
     t+" "+p+"_scbool("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned q=x^(unsigned)i, r=x>>3, s=x*2654435761u;\n"
     "    if((s>q && r<x) || (q&1u)) h+=s;\n"
     "    if((s&2u) || (q>r && (r^s)>5u)) h^=q*131u; else h+=r*7u;\n"
     "    if((s<q) && (q<r) && (r<x)) h=h*3u+1u;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1234ULL}, "Opt5", 2},

    // One subtraction, many flag consumers: borrow (CF), zero (ZF), sign (SF)
    // and the __builtin borrow are each read by a separate condition off the
    // SAME x-y subtraction.  MedFlags must keep every consumer pinned to that
    // subtraction and not fold any to a stale or earlier comparison.
    {p+"_flagreuse",
     t+" "+p+"_flagreuse("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned y=x ^ ((unsigned)i*0x9E3779B9u);\n"
     "    unsigned d; unsigned borrow=__builtin_sub_overflow(x,y,&d);\n"
     "    int zero=(d==0); int neg=((int)d<0); int below=(x<y);\n"
     "    unsigned r=0;\n"
     "    if(below) r+=d*131u;\n"
     "    if(zero) r+=7u; else r+=(d&0xFFu);\n"
     "    if(neg) r^=0xABCDu;\n"
     "    r += borrow*17u;\n"
     "    h=h*31u+r; x=(x*1664525u+1013904223u)^d; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x55AAULL}, "Opt5", 2},

    // Balanced nested ternary tree mixing signed branches with unsigned ones,
    // then min/max — a select tree the optimizer pairs across compare polarity.
    {p+"_terntree",
     t+" "+p+"_terntree("+t+" a){\n"
     "  int x=(int)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    int t1 = x>0 ? (x>100? x-100 : x*2) : (x<-50? x+50 : -x);\n"
     "    unsigned u=(unsigned)x;\n"
     "    unsigned t2 = (u&1u) ? (u>0x8000u? u>>1 : u<<1)\n"
     "                         : (u>0x4000u? u^0xFFu : u+7u);\n"
     "    int mn = t1<(int)t2 ? t1 : (int)t2;\n"
     "    int mx = t1>(int)t2 ? t1 : (int)t2;\n"
     "    h += (unsigned)mn*131u + (unsigned)mx*7u + t2;\n"
     "    x = (int)(h ^ (unsigned)(t1+i)); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x7F00ULL}, "Opt5", 2},

    // Same-iteration store then load through possibly-aliasing stack indices:
    // the write index and read index are independent runtime values, so the
    // optimizer must respect the may-alias and neither forward a stale value
    // (ri!=wi) nor read the wrong slot (ri==wi).
    {p+"_memfwd",
     t+" "+p+"_memfwd("+t+" a){\n"
     "  unsigned buf[8]; for(int i=0;i<8;i++) buf[i]=(unsigned)a+(unsigned)i;\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned wi=(x>>2)&7u, ri=(x>>5)&7u;\n"
     "    buf[wi]=x*131u+(unsigned)i;\n"
     "    unsigned v=buf[ri];\n"
     "    h=h*31u+v+wi*7u+ri;\n"
     "    x=(x*1664525u+1013904223u)^v; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xC3C3ULL}, "Opt5", 2},

    // Branchless boolean mask: several comparisons combined with &/|/^/~ into a
    // single mask that drives a select.  Stresses flag-to-bool materialization
    // (setcc / cset / movCC) without intervening branches.
    {p+"_boolmask",
     t+" "+p+"_boolmask("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned a0=(x>0x1000u), a1=(x<0x8000u);\n"
     "    unsigned a2=((x&0xFu)>7u), a3=((int)x<0);\n"
     "    unsigned m=(a0 & a1) | (a2 ^ a3) | (a0 & ~a3);\n"
     "    unsigned sel = m ? (x*2654435761u) : (x^0x9E3779B9u);\n"
     "    h += sel + m*131u + a0 + a1*3u + a2*5u + a3*7u;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x0FF0ULL}, "Opt5", 2},

    // Doubly-nested loop: a runtime inner trip count plus an inner break, with
    // the inner result fed to the outer accumulator.  Stresses loop PHIs and
    // the CFG simplifier across a non-trivial loop nest.
    {p+"_nestloop",
     t+" "+p+"_nestloop("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<24;i++){\n"
     "    unsigned inner=0;\n"
     "    for(unsigned j=0;j<(x&15u)+1u;j++){\n"
     "      inner=inner*31u+(x^j)+(unsigned)i;\n"
     "      if((inner&0x100u) && j>2u) break; }\n"
     "    h=h*131u+inner+(unsigned)i*7u;\n"
     "    x=(x*1664525u+1013904223u)^inner; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABCDULL}, "Opt5", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOpt5TC("x64opt5", "long");
static const std::vector<RoundTripTC> kX86 = makeOpt5TC("x86opt5", "int");
static const std::vector<RoundTripTC> kA64 = makeOpt5TC("a64opt5", "long");
static const std::vector<RoundTripTC> kARM = makeOpt5TC("armopt5", "int");

INSTANTIATE_TEST_SUITE_P(Opt5, X64Opt5RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt5, X86Opt5RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt5, A64Opt5RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt5, ARM32Opt5RT, ::testing::ValuesIn(kARM), rtTCName);
