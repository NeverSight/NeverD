//===- AllPlatform_IndCallXformRTTests.cpp - indirect-call probing -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Differential roundtrip probing of indirect-call / function-pointer shapes the
// existing FPtr suite does not exercise, aimed at the INDIR_CALL lowering
// (recoverCallAbi / lowerBranchInd tail-call path) and the code-pointer table
// relocation the recompile backend must rewrite to the *recompiled* leaves.
// Each dispatcher is defined first so it lands at the start of .text (the
// harness entry); every leaf is reached only indirectly.  Kernels keep all
// dispatch indices in range and all values integral so the two runs never
// diverge on UB, and fold a value-dependent hash across all four targets.
//
// Novel shapes vs. FPtr (cb4 / sm / vt):
//   * FP-typed callbacks (double arg + double return through the table);
//   * mixed int+FP callback signatures (reg-class-split argument marshalling);
//   * many-argument callbacks that spill arguments to the stack at the call;
//   * a function that RETURNS a function pointer, which the caller then calls;
//   * a bounded self-recursive indirect call (a function pointer to itself);
//   * a mutable (writable-global) function-pointer table permuted at runtime.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64IndCallRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64IndCallRT, Verify) { roundTripX64(GetParam()); }
class X86IndCallRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86IndCallRT, Verify) { roundTripX86(GetParam()); }
class A64IndCallRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64IndCallRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32IndCallRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32IndCallRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeIndCallTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // FP-typed callbacks: each leaf takes and returns a double, dispatched from
    // a code-pointer table.  Exercises the indirect-call FP ABI (arg in
    // xmm0/d0/s0, return likewise) that the integer FPtr suite never reaches.
    // Values are kept small integers so the double<->int casts are exact.
    {p+"_fpcb",
     "typedef double (*fd_t)(double);\n"
     "static double "+p+"_fa(double);\n"
     "static double "+p+"_fx(double);\n"
     "static double "+p+"_fm(double);\n"
     "static double "+p+"_fs(double);\n"
     +t+" "+p+"_fpcb("+t+" a){\n"
     "  static fd_t tab[4]={"+p+"_fa,"+p+"_fx,"+p+"_fm,"+p+"_fs};\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<60;i++){\n"
     "    double d=(double)(acc & 0x3FFu);\n"
     "    double r=tab[acc&3](d);\n"
     "    acc=(acc*1664525u)+ (unsigned)(long)r + (unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static double "+p+"_fa(double x){ return x+17.0; }\n"
     "static double "+p+"_fx(double x){ return x*2.0-3.0; }\n"
     "static double "+p+"_fm(double x){ return x*3.0; }\n"
     "static double "+p+"_fs(double x){ return x-5.0; }\n",
     {0x12345ULL}, "IndCall", 1},

    // Mixed int+FP callback signature: `unsigned (*)(unsigned, double)`.  The
    // two arguments land in different register classes, so the indirect-call ABI
    // recovery must marshal a GP arg and an FP arg to one INDIR_CALL.
    {p+"_mixcb",
     "typedef unsigned (*mf_t)(unsigned,double);\n"
     "static unsigned "+p+"_m0(unsigned,double);\n"
     "static unsigned "+p+"_m1(unsigned,double);\n"
     "static unsigned "+p+"_m2(unsigned,double);\n"
     +t+" "+p+"_mixcb("+t+" a){\n"
     "  static mf_t tab[3]={"+p+"_m0,"+p+"_m1,"+p+"_m2};\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<66;i++){\n"
     "    double d=(double)((acc>>4)&0xFFu);\n"
     "    acc=tab[(acc)%3u](acc,d)+(unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_m0(unsigned a,double b){ return a+(unsigned)(long)b; }\n"
     "static unsigned "+p+"_m1(unsigned a,double b){ return a^((unsigned)(long)b<<3); }\n"
     "static unsigned "+p+"_m2(unsigned a,double b){ return a*3u-(unsigned)(long)b; }\n",
     {0xABCDEULL}, "IndCall", 1},

    // Many-argument callbacks: 7 integer args force the last one or two onto the
    // stack on every ABI (x86-64 SysV: 6 GP regs; AArch64: 8; i386: all stack;
    // ARM: 4 regs).  The indirect call must marshal the stack args correctly.
    {p+"_argcb",
     "typedef unsigned (*a7_t)(unsigned,unsigned,unsigned,unsigned,unsigned,"
     "unsigned,unsigned);\n"
     "static unsigned "+p+"_g0(unsigned,unsigned,unsigned,unsigned,unsigned,"
     "unsigned,unsigned);\n"
     "static unsigned "+p+"_g1(unsigned,unsigned,unsigned,unsigned,unsigned,"
     "unsigned,unsigned);\n"
     +t+" "+p+"_argcb("+t+" a){\n"
     "  static a7_t tab[2]={"+p+"_g0,"+p+"_g1};\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){\n"
     "    acc=tab[acc&1](acc,acc^1u,acc+2u,acc-3u,acc*5u,acc>>1,(unsigned)i);\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_g0(unsigned a,unsigned b,unsigned c,unsigned d,"
     "unsigned e,unsigned f,unsigned g){ return a+b-c+d^e+f*3u-g; }\n"
     "static unsigned "+p+"_g1(unsigned a,unsigned b,unsigned c,unsigned d,"
     "unsigned e,unsigned f,unsigned g){ return (a^b)+(c*d)-(e|f)+g; }\n",
     {0x33CCULL}, "IndCall", 1},
  };
}

static std::vector<RoundTripTC> makeIndCall2TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A function that RETURNS a function pointer (a selector), which the caller
    // then calls indirectly — the code pointer flows through a return value, not
    // a table load, so the backend must symbolize the returned leaf address.
    {p+"_retfp",
     "typedef unsigned (*fn_t)(unsigned);\n"
     "static unsigned "+p+"_ra(unsigned);\n"
     "static unsigned "+p+"_rb(unsigned);\n"
     "static fn_t "+p+"_pick(unsigned);\n"
     +t+" "+p+"_retfp("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<70;i++){\n"
     "    fn_t f="+p+"_pick(acc);\n"
     "    acc=f(acc)+(unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_ra(unsigned x){ return x+0x9E3779B9u; }\n"
     "static unsigned "+p+"_rb(unsigned x){ return x^(x>>13)^(x<<7); }\n"
     "static fn_t "+p+"_pick(unsigned s){ return (s&1u)?"+p+"_rb:"+p+"_ra; }\n",
     {0x2468ULL}, "IndCall2", 1},

    // Bounded self-recursive indirect call: the function takes its own address
    // through a pointer and recurses through it with a decrementing depth, so
    // the INDIR_CALL targets the enclosing function itself.
    {p+"_selfrec",
     "typedef unsigned (*sr_t)(unsigned,unsigned);\n"
     "static unsigned "+p+"_work(unsigned,unsigned);\n"
     +t+" "+p+"_selfrec("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<48;i++) acc="+p+"_work(acc,(acc&3u))+(unsigned)i;\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_work(unsigned x,unsigned d){\n"
     "  volatile sr_t self="+p+"_work;\n"
     "  x=x*1664525u+1013904223u;\n"
     "  if(d==0u) return x;\n"
     "  return self(x^(d*0x9E37u),d-1u); }\n",
     {0x9ABCULL}, "IndCall2", 1},

    // Mutable function-pointer table in a WRITABLE global, rotated at runtime
    // before each call — the recovered call site cannot assume a fixed target,
    // and the backend must relocate every slot of the writable code-pointer
    // array (not just a read-only one).
    {p+"_muttab",
     "typedef unsigned (*fn_t)(unsigned);\n"
     "static unsigned "+p+"_u0(unsigned);\n"
     "static unsigned "+p+"_u1(unsigned);\n"
     "static unsigned "+p+"_u2(unsigned);\n"
     "static unsigned "+p+"_u3(unsigned);\n"
     "static fn_t "+p+"_g[4]={"+p+"_u0,"+p+"_u1,"+p+"_u2,"+p+"_u3};\n"
     +t+" "+p+"_muttab("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){\n"
     "    acc="+p+"_g[acc&3u](acc)+(unsigned)i;\n"
     "    fn_t t0="+p+"_g[0]; "+p+"_g[0]="+p+"_g[1]; "+p+"_g[1]="+p+"_g[2];\n"
     "    "+p+"_g[2]="+p+"_g[3]; "+p+"_g[3]=t0;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_u0(unsigned x){ return x+0x9E37u; }\n"
     "static unsigned "+p+"_u1(unsigned x){ return x^(x<<5); }\n"
     "static unsigned "+p+"_u2(unsigned x){ return x*3u; }\n"
     "static unsigned "+p+"_u3(unsigned x){ return (x<<7)|(x>>25); }\n",
     {0xC0FFEEULL}, "IndCall2", 1},
  };
}

static std::vector<RoundTripTC> makeIndCall3TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Function-pointer TAIL call: the indirect call is the last operation on the
    // path, so clang may emit a `jmp *reg` tail call (the lowerBranchInd tail-
    // call path, distinct from a call+return).  A helper does the tail dispatch;
    // the driver folds its result so the value still round-trips.
    {p+"_tailcb",
     "typedef unsigned (*fn_t)(unsigned);\n"
     "static unsigned "+p+"_ta(unsigned);\n"
     "static unsigned "+p+"_tb(unsigned);\n"
     "static unsigned "+p+"_tc(unsigned);\n"
     "static unsigned "+p+"_tdisp(unsigned);\n"
     +t+" "+p+"_tailcb("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<72;i++) acc="+p+"_tdisp(acc)+(unsigned)i;\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_ta(unsigned x){ return x+0x9E3779B9u; }\n"
     "static unsigned "+p+"_tb(unsigned x){ return x^(x>>13)^(x<<7); }\n"
     "static unsigned "+p+"_tc(unsigned x){ return x*2654435761u; }\n"
     "static unsigned "+p+"_tdisp(unsigned x){\n"
     "  static fn_t tab[3]={"+p+"_ta,"+p+"_tb,"+p+"_tc};\n"
     "  return tab[x%3u](x); }\n",
     {0x33CCULL}, "IndCall3", 1},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeIndCallTC("x64ic", "long");
static const std::vector<RoundTripTC> kX86 = makeIndCallTC("x86ic", "int");
static const std::vector<RoundTripTC> kA64 = makeIndCallTC("a64ic", "long");
static const std::vector<RoundTripTC> kARM = makeIndCallTC("armic", "int");

INSTANTIATE_TEST_SUITE_P(IndCall, X64IndCallRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall, X86IndCallRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall, A64IndCallRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall, ARM32IndCallRT, ::testing::ValuesIn(kARM), rtTCName);

static const std::vector<RoundTripTC> kX64B = makeIndCall2TC("x64ic2", "long");
static const std::vector<RoundTripTC> kX86B = makeIndCall2TC("x86ic2", "int");
static const std::vector<RoundTripTC> kA64B = makeIndCall2TC("a64ic2", "long");
static const std::vector<RoundTripTC> kARMB = makeIndCall2TC("armic2", "int");

INSTANTIATE_TEST_SUITE_P(IndCall2, X64IndCallRT, ::testing::ValuesIn(kX64B), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall2, X86IndCallRT, ::testing::ValuesIn(kX86B), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall2, A64IndCallRT, ::testing::ValuesIn(kA64B), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall2, ARM32IndCallRT, ::testing::ValuesIn(kARMB), rtTCName);

static const std::vector<RoundTripTC> kX64C = makeIndCall3TC("x64ic3", "long");
static const std::vector<RoundTripTC> kX86C = makeIndCall3TC("x86ic3", "int");
static const std::vector<RoundTripTC> kA64C = makeIndCall3TC("a64ic3", "long");
static const std::vector<RoundTripTC> kARMC = makeIndCall3TC("armic3", "int");

INSTANTIATE_TEST_SUITE_P(IndCall3, X64IndCallRT, ::testing::ValuesIn(kX64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall3, X86IndCallRT, ::testing::ValuesIn(kX86C), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall3, A64IndCallRT, ::testing::ValuesIn(kA64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(IndCall3, ARM32IndCallRT, ::testing::ValuesIn(kARMC), rtTCName);
