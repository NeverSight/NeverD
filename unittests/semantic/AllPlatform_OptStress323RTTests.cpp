//===- AllPlatform_OptStress323RTTests.cpp - -O3 call / ABI shapes -------===//
//
// -O3 arm of the call/ABI-recovery family: at -O3 clang inlines aggressively,
// keeps call arguments register-threaded, and schedules call results across
// branches differently from -O2/-O0.  noinline callees force real call edges so
// ABI recovery (arg/return width, register-pair i64) is exercised under the -O3
// scheduling that prior call probes (#502/#506/#518/#524) did not cover.
//
// All integer, LCG seeded, folded single return; 32-bit targets libcall-free
// (no i64 div, no i64 variable shift, widening multiplies only).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress323RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress323RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress323RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress323RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress323RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress323RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress323RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress323RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress323TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // noinline callee returning i64 (register pair on 32-bit), result consumed
    // across both arms of a branch.
    {p+"_retll",
     "static long long "+p+"_ra(int x,int y) __attribute__((noinline));\n"
     +t+" "+p+"_retll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    long long r="+p+"_ra((int)w,(int)(w>>7));\n"
     "    acc += (r<0)? (r ^ (r>>32)) : (r + (r>>17)); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_ra(int x,int y){\n"
     "  return (long long)x*(long long)y + ((long long)(x^y)<<20); }\n",
     {0x1234u}, "OptStress323", Opt},

    // >4 integer args to a noinline callee (stack args on every ABI).
    {p+"_argspill",
     "static int "+p+"_as(int a,int b,int c,int d,int e,int f,int g,int h) __attribute__((noinline));\n"
     +t+" "+p+"_argspill("+t+" a){ unsigned w=(unsigned)a^0x33u; long long acc=0;\n"
     "  for(int i=0;i<48;i++){ w=w*22695477u+1u;\n"
     "    acc += "+p+"_as((int)w,(int)(w>>1),(int)(w>>2),(int)(w>>3),\n"
     "                    (int)(w>>4),(int)(w>>5),(int)(w>>6),(int)(w>>7)); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_as(int a,int b,int c,int d,int e,int f,int g,int h){\n"
     "  return a*2+b*3+c*5+d*7+e*11+f*13+g*17+h*19; }\n",
     {0x2345u}, "OptStress323", Opt},

    // Indirect call (function pointer) returning i64, kept live by asm barrier.
    {p+"_indll",
     "static long long "+p+"_ib(int x) __attribute__((noinline));\n"
     +t+" "+p+"_indll("+t+" a){ unsigned w=(unsigned)a+0x9u; long long acc=0;\n"
     "  long long (*fp)(int)="+p+"_ib; __asm__(\"\":\"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1664525u+1013904223u;\n"
     "    long long r=fp((int)w); acc += r ^ (r>>32); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_ib(int x){\n"
     "  return (long long)x*(long long)x + ((long long)x<<28); }\n",
     {0x3456u}, "OptStress323", Opt},

    // Chained calls a->b->c with results threaded (inlining vs real edge mix).
    {p+"_chain",
     "static int "+p+"_cc(int x,int y) __attribute__((noinline));\n"
     +t+" "+p+"_chain("+t+" a){ unsigned w=(unsigned)a|7u; long long acc=0;\n"
     "  for(int i=0;i<48;i++){ w=w*1103515245u+12345u;\n"
     "    int r1="+p+"_cc((int)w,(int)(w>>8));\n"
     "    int r2="+p+"_cc(r1,(int)(w>>16));\n"
     "    int r3="+p+"_cc(r2,r1);\n"
     "    acc += (long long)r3*(long long)r1; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_cc(int x,int y){ return (x^y)*131 + (x&y) - (x|y); }\n",
     {0x4567u}, "OptStress323", Opt},

    // Mixed int + i64 args to a noinline callee (register-pair arg on 32-bit).
    {p+"_mixarg",
     "static long long "+p+"_ma(int x,long long y,int z) __attribute__((noinline));\n"
     +t+" "+p+"_mixarg("+t+" a){ unsigned w=(unsigned)a^0xa5u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*22695477u+1u;\n"
     "    long long y=(long long)(int)w*(long long)(int)(w>>9);\n"
     "    long long r="+p+"_ma((int)(w>>3),y,(int)(w>>17));\n"
     "    acc += r ^ (r>>32); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_ma(int x,long long y,int z){\n"
     "  return y + (long long)x*(long long)z + ((y>>32)^(long long)(x+z)); }\n",
     {0x5678u}, "OptStress323", Opt},

    // Recursive accumulation with int + carried state (tail-recursion at -O3).
    {p+"_recur",
     "static int "+p+"_rr(int n,int acc) __attribute__((noinline));\n"
     +t+" "+p+"_recur("+t+" a){ unsigned w=(unsigned)a+0x55u; long long total=0;\n"
     "  for(int i=0;i<24;i++){ w=w*1664525u+1013904223u;\n"
     "    total += (long long)"+p+"_rr((int)(w&31), (int)w); }\n"
     "  return ("+t+")(total ^ (total>>32)); }\n"
     "static int "+p+"_rr(int n,int acc){\n"
     "  if(n<=0) return acc; return "+p+"_rr(n-1, acc*31 + n); }\n",
     {0x6789u}, "OptStress323", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress323TC("x64o323", "long", 3);
static const std::vector<RoundTripTC> kX86 = makeOptStress323TC("x86o323", "int", 3);
static const std::vector<RoundTripTC> kA64 = makeOptStress323TC("a64o323", "long", 3);
static const std::vector<RoundTripTC> kARM = makeOptStress323TC("armo323", "int", 3);

INSTANTIATE_TEST_SUITE_P(OptStress323, X64OptStress323RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress323, X86OptStress323RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress323, A64OptStress323RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress323, ARM32OptStress323RT, ::testing::ValuesIn(kARM), rtTCName);
