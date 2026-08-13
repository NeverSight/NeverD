//===- AllPlatform_NestedCallRTTests.cpp - nested/tail call ABI -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes that push the call-ABI and live-in recovery paths (the #408
// to #410 bug cluster) harder than AllPlatform_CallArgVarietyRTTests: a call
// expression tree where two callee return values feed a third call's arguments,
// a deep tail-call chain, mutual tail recursion, a conditional tail call, a
// callee result reused for several arguments, nested self-calls (`f(f(f(x)))`),
// sub-word (signed char / short) value arguments crossing the call boundary, and
// a ten-argument mix straddling the register/stack boundary where some arguments
// come from a prior call's return.  Each kernel folds everything into one integer
// return so the roundtrip compares a single register; all integer (no ARM32
// soft-float libcall), compiled at -O2 and checked native vs lifted on all four
// targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64NestedCallRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64NestedCallRT, Verify) { roundTripX64(GetParam()); }
class X86NestedCallRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86NestedCallRT, Verify) { roundTripX86(GetParam()); }
class A64NestedCallRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64NestedCallRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32NestedCallRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32NestedCallRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeNestedCallTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Call expression tree: two callee return values feed a third call's args.
    // Forces the ABI recovery to thread each prior CALL's result (which lands in
    // the result/arg0 register) into the next call's argument slots.
    {p+"_tree",
     "static unsigned "+p+"_g(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_h(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_f(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_tree("+t+" a){\n"
     "  unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_f("+p+"_g(v), "+p+"_h(v)); }\n"
     "static unsigned "+p+"_g(unsigned x){ return x*2654435761u+1u; }\n"
     "static unsigned "+p+"_h(unsigned x){ return (x^0x9e3779b9u)*131u; }\n"
     "static unsigned "+p+"_f(unsigned x,unsigned y){ return x*31u - y + (x^y); }\n",
     {0x41ULL}, "NestCall", 2},

    // Deep tail-call chain a->b->c->d->e, each transforming the single argument
    // before tail-jumping to the next.  clang -O2 lowers each `return next(...)`
    // to a jump; the function body never spills the argument register.
    {p+"_tailchain",
     "static unsigned "+p+"_e(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_d(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_c(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_b(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_tailchain("+t+" a){ return ("+t+")(unsigned long)"+p+"_b((unsigned)a); }\n"
     "static unsigned "+p+"_b(unsigned x){ return "+p+"_c(x*31u+1u); }\n"
     "static unsigned "+p+"_c(unsigned x){ return "+p+"_d(x^0x55aa55aau); }\n"
     "static unsigned "+p+"_d(unsigned x){ return "+p+"_e(x*2654435761u); }\n"
     "static unsigned "+p+"_e(unsigned x){ return x - (x>>3) + 7u; }\n",
     {0x53ULL}, "NestCall", 2},

    // Mutual tail recursion: ev/od ping-pong, depth set by the runtime argument
    // so it cannot be folded.  The tail call sits behind a predicate.
    {p+"_mutual",
     "static unsigned "+p+"_ev(unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_od(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_mutual("+t+" a){\n"
     "  return ("+t+")(unsigned long)"+p+"_ev((unsigned)a & 63u, (unsigned)a); }\n"
     "static unsigned "+p+"_ev(unsigned n,unsigned acc){\n"
     "  if(!n) return acc*31u+1u; return "+p+"_od(n-1u, acc*3u+n); }\n"
     "static unsigned "+p+"_od(unsigned n,unsigned acc){\n"
     "  if(!n) return acc*7u+2u; return "+p+"_ev(n-1u, acc^(n*2654435761u)); }\n",
     {0x67ULL}, "NestCall", 2},

    // Conditional tail call: the callee selected depends on a runtime predicate,
    // each branch a separate tail jump with its own argument shape.
    {p+"_condtail",
     "static unsigned "+p+"_p(unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_q(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_condtail("+t+" a){\n"
     "  unsigned v=(unsigned)a;\n"
     "  if(v&1u) return ("+t+")(unsigned long)"+p+"_p(v, v*7u);\n"
     "  return ("+t+")(unsigned long)"+p+"_q(v^0x12345u); }\n"
     "static unsigned "+p+"_p(unsigned x,unsigned y){ return x*131u + y; }\n"
     "static unsigned "+p+"_q(unsigned x){ unsigned h=x; for(unsigned i=0;i<5;i++) h=h*31u+i; return h; }\n",
     {0x71ULL}, "NestCall", 2},

    // Callee result reused for several arguments: g(v) computed once, fed to two
    // argument slots that the optimizer must not duplicate the call for.
    {p+"_reusearg",
     "static unsigned "+p+"_g(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_f3(unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_reusearg("+t+" a){\n"
     "  unsigned v=(unsigned)a; unsigned w="+p+"_g(v);\n"
     "  return ("+t+")(unsigned long)"+p+"_f3(w, w*3u+1u, v^w); }\n"
     "static unsigned "+p+"_g(unsigned x){ return x*2654435761u+0x9e3779b9u; }\n"
     "static unsigned "+p+"_f3(unsigned x,unsigned y,unsigned z){ return (x*31u+y)*31u+z; }\n",
     {0x29ULL}, "NestCall", 2},

    // Nested self-calls f(f(f(x))): the same function's return repeatedly becomes
    // its own next argument.  Stresses the result==arg0 reaching-definition path.
    {p+"_selfnest",
     "static unsigned "+p+"_s(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_selfnest("+t+" a){\n"
     "  unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_s("+p+"_s("+p+"_s(v))); }\n"
     "static unsigned "+p+"_s(unsigned x){ return x*31u + (x>>5) + 0x1357u; }\n",
     {0x35ULL}, "NestCall", 2},

    // Sub-word value arguments: a signed char, a short and an int cross the call
    // boundary, exercising the narrow-argument register slices the recompiled ABI
    // must sign/zero extend exactly as the callee expects.
    {p+"_subword",
     "static int "+p+"_sw(signed char,short,int) __attribute__((noinline));\n"
     +t+" "+p+"_subword("+t+" a){\n"
     "  unsigned v=(unsigned)a; unsigned r=0;\n"
     "  for(int i=0;i<8;i++){\n"
     "    signed char c=(signed char)(v>>(i&7)); short s=(short)(v*131u+i);\n"
     "    int n=(int)(v^(unsigned)(i*2654435761u));\n"
     "    r = r*131u + (unsigned)"+p+"_sw(c,s,n); v=v*1664525u+1013904223u; }\n"
     "  return ("+t+")(unsigned long)r; }\n"
     "static int "+p+"_sw(signed char c,short s,int n){ return (int)c*7 + (int)s*3 + n; }\n",
     {0x4DULL}, "NestCall", 2},

    // Ten-argument mix straddling the register/stack boundary, with arg0 supplied
    // by a prior call's return.  Forces full-arity stack-argument recovery plus
    // the result-as-argument path on the same call.
    {p+"_manymix",
     "static unsigned "+p+"_g(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_w(unsigned,unsigned,unsigned,unsigned,unsigned,\n"
     "                       unsigned,unsigned,unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_manymix("+t+" a){\n"
     "  unsigned v=(unsigned)a; unsigned g="+p+"_g(v);\n"
     "  return ("+t+")(unsigned long)"+p+"_w(g, v, v^1u, v^2u, v^3u, v^4u, v^5u, v^6u, v^7u, v^8u); }\n"
     "static unsigned "+p+"_g(unsigned x){ return x*2654435761u+1u; }\n"
     "static unsigned "+p+"_w(unsigned a0,unsigned a1,unsigned a2,unsigned a3,unsigned a4,\n"
     "                        unsigned a5,unsigned a6,unsigned a7,unsigned a8,unsigned a9){\n"
     "  return (((((((((a0*31u+a1)*31u+a2)*31u+a3)*31u+a4)*31u+a5)*31u+a6)*31u+a7)*31u+a8)*31u+a9); }\n",
     {0x19ULL}, "NestCall", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeNestedCallTC("x64nc", "long");
static const std::vector<RoundTripTC> kX86 = makeNestedCallTC("x86nc", "int");
static const std::vector<RoundTripTC> kA64 = makeNestedCallTC("a64nc", "long");
static const std::vector<RoundTripTC> kARM = makeNestedCallTC("armnc", "int");

INSTANTIATE_TEST_SUITE_P(NestCall, X64NestedCallRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(NestCall, X86NestedCallRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(NestCall, A64NestedCallRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(NestCall, ARM32NestedCallRT, ::testing::ValuesIn(kARM), rtTCName);
