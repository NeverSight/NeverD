//===- AllPlatform_CFGStressRTTests.cpp - control-flow reconstruction -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// CFG reconstruction is where the most recent real lift bugs lived (#412's
// mutual-recursion tail call fused two function bodies).  These probes feed the
// CFG builder optimizer-transformed control flow it must rebuild exactly:
// tail-recursion clang turns into a loop, three-way mutual recursion, gotos that
// break out of two nested loops, a loop with several distinct exits returning
// different values, a Duff's-device unrolled switch inside a loop, and a
// continue-to-outer-loop via a label.
//
// Every kernel terminates (depth/iteration bounded by the argument), folds to a
// single integer return, and lowers to no runtime helper, so all four targets
// are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CFGStressRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CFGStressRT, Verify) { roundTripX64(GetParam()); }
class X86CFGStressRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CFGStressRT, Verify) { roundTripX86(GetParam()); }
class A64CFGStressRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CFGStressRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CFGStressRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CFGStressRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCFGStressTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Tail-recursive gcd-style reduction clang rewrites into a loop.
    {p+"_tailrec",
     "static unsigned "+p+"_g(unsigned x,unsigned y,unsigned acc){\n"
     "  if(y==0) return acc^x;\n"
     "  return "+p+"_g(y, x%y, acc*131u+x); }\n"
     +t+" "+p+"_tailrec("+t+" a){ unsigned v=(unsigned)a|1u;\n"
     "  return ("+t+")(unsigned)"+p+"_g(v*7u+3u, (v&0xffffu)|1u, 0); }\n",
     {0x4cULL}, "CFGStress", 2},

    // Three-way mutual recursion a->b->c->a, depth bounded by the value.
    {p+"_mut3",
     "static unsigned "+p+"_ra(unsigned),"+p+"_rb(unsigned),"+p+"_rc(unsigned);\n"
     "static unsigned "+p+"_ra(unsigned n){ if(n==0) return 1u; return n*3u+"+p+"_rb(n-1u); }\n"
     "static unsigned "+p+"_rb(unsigned n){ if(n==0) return 2u; return n*5u+"+p+"_rc(n-1u); }\n"
     "static unsigned "+p+"_rc(unsigned n){ if(n==0) return 4u; return n*7u+"+p+"_ra(n-1u); }\n"
     +t+" "+p+"_mut3("+t+" a){ unsigned n=((unsigned)a&15u)+3u;\n"
     "  return ("+t+")(unsigned)"+p+"_ra(n); }\n",
     {0x9bULL}, "CFGStress", 2},

    // Nested loops with a goto that breaks out of both levels.
    {p+"_brk2",
     t+" "+p+"_brk2("+t+" a){ unsigned x=(unsigned)a|1u, h=0; int found=0;\n"
     "  for(int i=0;i<20 && !found;i++)\n"
     "    for(int j=0;j<20;j++){ x=x*1103515245u+12345u;\n"
     "      h=h*31u+x;\n"
     "      if(((x>>10)&0x3ffu)==((unsigned)a&0x3ffu)){ found=1; goto done; } }\n"
     "  done: return ("+t+")(unsigned)(h+(unsigned)found); }\n",
     {0xa7ULL}, "CFGStress", 2},

    // Loop with several distinct exits returning different folded values.
    {p+"_multiexit",
     t+" "+p+"_multiexit("+t+" a){ unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; h=h*31u+x;\n"
     "    if((x&0xffu)==0x42u) return ("+t+")(unsigned)(h^0xaaaaaaaau);\n"
     "    if(((x>>16)&0xffu)==0x99u) return ("+t+")(unsigned)(h+0x12345u);\n"
     "    if(h>0x7fffffffu && i>8) break; }\n"
     "  return ("+t+")(unsigned)(h*2654435761u); }\n",
     {0x35ULL}, "CFGStress", 2},

    // Duff's-device style unrolled switch threaded through a loop.
    {p+"_duff",
     t+" "+p+"_duff("+t+" a){ unsigned x=(unsigned)a|1u, h=0;\n"
     "  int n=(int)(((unsigned)a&31u)+9u); int k=(n+3)/4;\n"
     "  switch(n&3){\n"
     "  case 0: do{ x=x*1103515245u+12345u; h=h*31u+x;\n"
     "  case 3:    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "  case 2:    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "  case 1:    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "          }while(--k>0); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "CFGStress", 2},

    // continue-to-outer-loop via a label (skips the inner remainder).
    {p+"_contlbl",
     t+" "+p+"_contlbl("+t+" a){ unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    for(int j=0;j<16;j++){ x=x*1103515245u+12345u;\n"
     "      if(((x>>7)&3u)==0u) goto next;\n"
     "      h=h*31u+x; }\n"
     "    h+=0x1000u;\n"
     "    next: h^=(unsigned)i; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "CFGStress", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCFGStressTC("x64cfg", "long");
static const std::vector<RoundTripTC> kX86 = makeCFGStressTC("x86cfg", "int");
static const std::vector<RoundTripTC> kA64 = makeCFGStressTC("a64cfg", "long");
static const std::vector<RoundTripTC> kARM = makeCFGStressTC("armcfg", "int");

INSTANTIATE_TEST_SUITE_P(CFGStress, X64CFGStressRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGStress, X86CFGStressRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGStress, A64CFGStressRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGStress, ARM32CFGStressRT, ::testing::ValuesIn(kARM), rtTCName);
