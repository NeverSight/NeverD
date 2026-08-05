//===- AllPlatform_CallArgVarietyRTTests.cpp - call ABI stressors -*-C++*-=//
//
// Roundtrip probes for the call-ABI and live-in recovery paths reworked in
// #409: multi-argument pure forwarders (`f(a,b){return g(a,b);}`), argument
// shuffling, small/medium struct-by-value arguments and returns, an out-pointer
// to a local, a loop-carried accumulator argument, and an argument materialised
// in only one branch.  Each kernel folds its result into a single integer return
// so the roundtrip compares one register; all integer (no ARM32 soft-float
// libcall), compiled at -O2 and checked native vs lifted across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CallArgVarietyRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CallArgVarietyRT, Verify) { roundTripX64(GetParam()); }
class X86CallArgVarietyRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CallArgVarietyRT, Verify) { roundTripX86(GetParam()); }
class A64CallArgVarietyRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CallArgVarietyRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CallArgVarietyRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CallArgVarietyRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCallArgTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Two-argument pure forwarder: `f(x,y){return g(x,y);}` never reads its
    // register arguments except to pass them on.
    {p+"_fwd2",
     "static unsigned "+p+"_g2(unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_f2(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_fwd2("+t+" a){\n"
     "  unsigned b=(unsigned)a; return ("+t+")(unsigned long)"+p+"_f2(b, b*2654435761u+1u); }\n"
     "static unsigned "+p+"_f2(unsigned x,unsigned y){ return "+p+"_g2(x,y); }\n"
     "static unsigned "+p+"_g2(unsigned x,unsigned y){\n"
     "  unsigned h=x^0x9e3779b9u; for(unsigned i=0;i<8;i++) h=h*31u+((y>>(i&7))&0xffu)+i; return h; }\n",
     {0x41ULL}, "CallArg", 2},

    // Three-argument forwarder.
    {p+"_fwd3",
     "static unsigned "+p+"_g3(unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_f3(unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_fwd3("+t+" a){\n"
     "  unsigned b=(unsigned)a; return ("+t+")(unsigned long)"+p+"_f3(b, b^0x55u, b*7u); }\n"
     "static unsigned "+p+"_f3(unsigned x,unsigned y,unsigned z){ return "+p+"_g3(x,y,z); }\n"
     "static unsigned "+p+"_g3(unsigned x,unsigned y,unsigned z){\n"
     "  return ((x*31u+y)*31u+z)*2654435761u; }\n",
     {0x53ULL}, "CallArg", 2},

    // Argument-shuffling forwarder: `f(x,y){return g(y,x);}` swaps the order.
    {p+"_swap",
     "static unsigned "+p+"_gs(unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_fs(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_swap("+t+" a){\n"
     "  unsigned b=(unsigned)a; return ("+t+")(unsigned long)"+p+"_fs(b, b+0x1000u); }\n"
     "static unsigned "+p+"_fs(unsigned x,unsigned y){ return "+p+"_gs(y,x); }\n"
     "static unsigned "+p+"_gs(unsigned x,unsigned y){ return x*131u - y*31u + (x^y); }\n",
     {0x67ULL}, "CallArg", 2},

    // 8-byte struct passed by value to a noinline helper.
    {p+"_s8",
     "struct "+p+"R{ unsigned a,b; };\n"
     "static unsigned "+p+"_hr(struct "+p+"R) __attribute__((noinline));\n"
     +t+" "+p+"_s8("+t+" a){\n"
     "  unsigned v=(unsigned)a; struct "+p+"R r={v*2654435761u, v^0x9e3779b9u};\n"
     "  return ("+t+")(unsigned long)"+p+"_hr(r); }\n"
     "static unsigned "+p+"_hr(struct "+p+"R r){ return r.a*31u + r.b; }\n",
     {0x71ULL}, "CallArg", 2},

    // 16-byte (four-int) struct passed by value.
    {p+"_s16",
     "struct "+p+"Q{ unsigned a,b,c,d; };\n"
     "static unsigned "+p+"_hq(struct "+p+"Q) __attribute__((noinline));\n"
     +t+" "+p+"_s16("+t+" a){\n"
     "  unsigned v=(unsigned)a;\n"
     "  struct "+p+"Q q={v*2654435761u, v^0x9e3779b9u, v+0x12345u, v*131u};\n"
     "  return ("+t+")(unsigned long)"+p+"_hq(q); }\n"
     "static unsigned "+p+"_hq(struct "+p+"Q q){ return ((q.a*31u+q.b)*31u+q.c)*31u+q.d; }\n",
     {0x29ULL}, "CallArg", 2},

    // Small struct returned by value, folded into a scalar by the caller.
    {p+"_sret",
     "struct "+p+"P{ unsigned a,b; };\n"
     "static struct "+p+"P "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_sret("+t+" a){\n"
     "  struct "+p+"P pp="+p+"_mk((unsigned)a); return ("+t+")(unsigned long)(pp.a*31u+pp.b); }\n"
     "static struct "+p+"P "+p+"_mk(unsigned x){\n"
     "  struct "+p+"P pp; pp.a=x*2654435761u; pp.b=x^0x9e3779b9u; return pp; }\n",
     {0x35ULL}, "CallArg", 2},

    // Out-pointer: &local handed to a helper that writes two results through it.
    {p+"_outp",
     "static void "+p+"_ow(unsigned*,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_outp("+t+" a){\n"
     "  unsigned r[2]={0,0}; "+p+"_ow(r,(unsigned)a); return ("+t+")(unsigned long)(r[0]*31u+r[1]); }\n"
     "static void "+p+"_ow(unsigned*o,unsigned x){ o[0]=x*2654435761u; o[1]=x^0x9e3779b9u; }\n",
     {0x4DULL}, "CallArg", 2},

    // Loop-carried accumulator argument: `acc = h(acc, i)` keeps acc in arg0
    // across iterations with no in-block re-move before the call.
    {p+"_carry",
     "static unsigned "+p+"_hc(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_carry("+t+" a){\n"
     "  unsigned acc=(unsigned)a; for(unsigned i=0;i<10;i++) acc="+p+"_hc(acc,i); return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_hc(unsigned acc,unsigned i){ return acc*31u + i*2654435761u + 1u; }\n",
     {0x19ULL}, "CallArg", 2},

    // Argument materialised in only one branch (a PHI feeds the call argument).
    {p+"_cond",
     "static unsigned "+p+"_hk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_cond("+t+" a){\n"
     "  unsigned v=(unsigned)a, x;\n"
     "  if(v&1u) x=v*2654435761u; else x=v^0x9e3779b9u;\n"
     "  return ("+t+")(unsigned long)"+p+"_hk(x); }\n"
     "static unsigned "+p+"_hk(unsigned x){ unsigned h=x; for(unsigned i=0;i<6;i++) h=h*131u+i; return h; }\n",
     {0x88ULL}, "CallArg", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCallArgTC("x64cav", "long");
static const std::vector<RoundTripTC> kX86 = makeCallArgTC("x86cav", "int");
static const std::vector<RoundTripTC> kA64 = makeCallArgTC("a64cav", "long");
static const std::vector<RoundTripTC> kARM = makeCallArgTC("armcav", "int");

INSTANTIATE_TEST_SUITE_P(CallArg, X64CallArgVarietyRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CallArg, X86CallArgVarietyRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CallArg, A64CallArgVarietyRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CallArg, ARM32CallArgVarietyRT, ::testing::ValuesIn(kARM), rtTCName);
