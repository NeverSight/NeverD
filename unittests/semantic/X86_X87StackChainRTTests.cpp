//===- X86_X87StackChainRTTests.cpp - x87 pop-and-operate chains -*-C++*-=//
//
// Straight-line x87 register-stack chains: an expression tree over `long double`
// values keeps several operands resident and clang evaluates it with runs of
// pop-and-operate ops (faddp/fsubp/fmulp/fdivp) plus `fld st(i)` duplications,
// rather than round-tripping each step through memory.  This exercises the lift
// of consecutive ST-register reads/writes against the same physical slots (the
// "multi-op chain" the earlier single-op x87 tests did not cover).  The result
// is read from the low 64 mantissa bytes of a stored long double so original and
// lifted compare on well-defined state only.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87ChainRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87ChainRT, Verify) { roundTripX64(GetParam()); }
class X86X87ChainRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87ChainRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeChainTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sum of products: x0*y0 + x1*y1 + x2*y2 + x3*y3 keeps partial sums on the
    // stack -> fmulp/faddp pop-and-operate chain.
    {p+"_dotprod",
     t+" "+p+"_dotprod("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double s=0;\n"
     "  for(int i=0;i<60;i++){\n"
     "    long double x0=(long double)(int)(u%13), x1=(long double)(int)((u>>4)%13);\n"
     "    long double x2=(long double)(int)((u>>8)%13), x3=(long double)(int)((u>>12)%13);\n"
     "    long double y0=x0*1.0000001L+1.0L, y1=x1*1.0000002L+2.0L;\n"
     "    long double y2=x2*1.0000003L+3.0L, y3=x3*1.0000004L+4.0L;\n"
     "    s=s+(x0*y0+x1*y1+x2*y2+x3*y3)*0.001L;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)((unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x21ULL}, "X87Chain", 2},

    // Horner polynomial: ((((c4*x+c3)*x+c2)*x+c1)*x+c0 -- x and the running
    // accumulator stay on the stack across the fmulp/faddp run.
    {p+"_horner",
     t+" "+p+"_horner("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double s=0;\n"
     "  for(int i=0;i<60;i++){ long double x=(long double)(int)(u%29)*0.1L;\n"
     "    long double r=((((1.0000001L*x+2.0000002L)*x+3.0000003L)*x\n"
     "                    +4.0000004L)*x+5.0000005L);\n"
     "    s=s*0.5L+r*0.01L;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)((unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x32ULL}, "X87Chain", 2},

    // Deep expression tree mixing +,-,*,/ so faddp/fsubp/fmulp/fdivp interleave
    // with `fld st(i)` reuse of a common subexpression.
    {p+"_tree",
     t+" "+p+"_tree("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double s=0;\n"
     "  for(int i=0;i<55;i++){\n"
     "    long double p1=(long double)(int)(u%17)+1.5L;\n"
     "    long double q=(long double)(int)((u>>5)%17)+2.5L;\n"
     "    long double r=(long double)(int)((u>>10)%17)+3.5L;\n"
     "    long double v=((p1*q+r)-(p1-q*r))*((p1+r)/(q+1.0L))-(p1*r-q);\n"
     "    s=s+v*0.0001L;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)((unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x43ULL}, "X87Chain", 2},

    // fld st(i) duplication: square a running value via `fld st0; fmulp` and feed
    // a longer add chain, stressing copies of stack slots.
    {p+"_sqchain",
     t+" "+p+"_sqchain("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double s=0;\n"
     "  for(int i=0;i<60;i++){ long double x=(long double)(int)(u%23)*0.25L+0.1L;\n"
     "    long double x2=x*x, x3=x2*x, x4=x2*x2;\n"
     "    s=s+(x4-x3+x2-x+1.0L)*0.001L;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&s,8);\n"
     "  return ("+t+")(unsigned)((unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x54ULL}, "X87Chain", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeChainTC("x64chn", "long");
static const std::vector<RoundTripTC> kX86 = makeChainTC("x86chn", "int");

INSTANTIATE_TEST_SUITE_P(X87Chain, X64X87ChainRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87Chain, X86X87ChainRT,
                         ::testing::ValuesIn(kX86), rtTCName);
