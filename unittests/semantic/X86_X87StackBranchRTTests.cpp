//===- X86_X87StackBranchRTTests.cpp - x87 stack across branches -*-C++*-=//
//
// The x87 register stack is a *physical* rotating file: ST(i) names the slot
// (TOP+i)&7.  NeverD tracks TOP (`FPUTop`) as a single counter that advances in
// the order instructions are *lifted* (worklist exploration order), not in CFG
// order.  When a conditional branch leaves a value resident on the x87 stack and
// the taken arm net-changes the stack depth, the fall-through arm is lifted with
// the taken arm's exit TOP rather than the branch's TOP -- so its ST(i) names the
// wrong physical slot.  These kernels keep a `long double` accumulator (and a
// resident compare constant) live across an in-loop multi-way branch, exactly
// the clang -O2 idiom (`fucomi %st(k),%st` against a stack-resident constant)
// that the earlier X87 probes deliberately avoided.  The result is read from the
// low 64 mantissa bytes of the stored f80 so only well-defined state compares.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87StkBrRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87StkBrRT, Verify) { roundTripX64(GetParam()); }
class X86X87StkBrRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87StkBrRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeStkBrTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Resident compare constants (1000.0 / -1000.0) across a 3-way in-loop
    // branch: clang keeps them on the x87 stack and compares with fucomi against
    // a deep slot, so the fall-through arm's ST(i) shifts if TOP is mistracked.
    {p+"_wrap3",
     t+" "+p+"_wrap3("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double acc=0.0L; unsigned h=0;\n"
     "  for(int i=0;i<120;i++){ acc=acc*1.0000001L+(long double)(int)(u%50);\n"
     "    if(acc>1000.0L){ acc=acc-1000.0L; h=h*131u+1u; }\n"
     "    else if(acc<-1000.0L){ acc=acc+1000.0L; h=h*131u+3u; }\n"
     "    else { h=h*131u+7u; }\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&acc,8);\n"
     "  return ("+t+")(unsigned)(h ^ (unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x31ULL}, "X87StkBr", 2},

    // Two resident accumulators (lo/hi) plus a branch that pops only one of them
    // in the taken arm -- asymmetric net stack change across the branch.
    {p+"_twoacc",
     t+" "+p+"_twoacc("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double lo=0.5L, hi=2.0L; unsigned h=0;\n"
     "  for(int i=0;i<100;i++){ long double v=(long double)(int)(u%97);\n"
     "    lo=lo*1.0000003L+v*0.25L; hi=hi*0.9999997L+v*0.5L;\n"
     "    if(lo>hi){ long double t2=lo; lo=hi; hi=t2; h=h*131u+1u; }\n"
     "    else { h=h*131u+5u; }\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m1,m2; __builtin_memcpy(&m1,&lo,8); __builtin_memcpy(&m2,&hi,8);\n"
     "  return ("+t+")(unsigned)(h ^ (unsigned)m1 ^ (unsigned)m2); }\n",
     {0x44ULL}, "X87StkBr", 2},

    // min/max selection chain: clang emits fucomi + fcmov / branch keeping both
    // operands resident; the loser is popped in one arm only.
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double mn=1e18L, mx=-1e18L, s=0;\n"
     "  for(int i=0;i<150;i++){ long double v=(long double)(int)(u%1000)-500.0L;\n"
     "    v=v*1.0000001L;\n"
     "    if(v<mn) mn=v; if(v>mx) mx=v; s=s+v*0.001L;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  long double r=mx-mn+s;\n"
     "  unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)((unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x55ULL}, "X87StkBr", 2},

    // Nested conditionals, each comparing the resident accumulator against a
    // resident constant -- deepest stack residency across the most branches.
    {p+"_nested",
     t+" "+p+"_nested("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=1.0L; unsigned h=0;\n"
     "  for(int i=0;i<110;i++){ x=x*1.0000005L+(long double)(int)(u%17)-8.0L;\n"
     "    if(x>100.0L){ if(x>500.0L){ x=x*0.5L; h+=1u; } else { x=x-100.0L; h+=2u; } }\n"
     "    else if(x<-100.0L){ if(x<-500.0L){ x=x*0.5L; h+=4u; } else { x=x+100.0L; h+=8u; } }\n"
     "    else { x=x+1.0L; h+=16u; } h*=131u;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  unsigned long long m; __builtin_memcpy(&m,&x,8);\n"
     "  return ("+t+")(unsigned)(h ^ (unsigned)m ^ (unsigned)(m>>32)); }\n",
     {0x66ULL}, "X87StkBr", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeStkBrTC("x64stk", "long");
static const std::vector<RoundTripTC> kX86 = makeStkBrTC("x86stk", "int");

INSTANTIATE_TEST_SUITE_P(X87StkBr, X64X87StkBrRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87StkBr, X86X87StkBrRT,
                         ::testing::ValuesIn(kX86), rtTCName);
