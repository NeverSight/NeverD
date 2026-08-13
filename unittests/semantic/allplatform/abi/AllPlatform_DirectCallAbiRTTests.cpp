//===- AllPlatform_DirectCallAbiRTTests.cpp - direct inter-fn call ABI -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probing of *direct* calls between distinct functions, the ABI
// surface that self-recursion (#401) and indirect dispatch (#402) only touch
// partially.  Each kernel's entry is defined first so it lands at the start of
// .text (the harness entry), then calls noinline helpers that exercise a
// specific calling-convention shape: more arguments than the platform passes
// in registers (stack-spilled args), small structs by value and by return
// (multi-register aggregate ABI), a four-deep a->b->c->d call chain, a caller-
// saved reload around a call inside a loop, and mutual recursion with sibling
// (tail) calls.  All arguments stay integer so no ARM32 soft-float libcall
// creeps into the call ABI.  All four targets, compared native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CallAbiRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CallAbiRT, Verify) { roundTripX64(GetParam()); }
class X86CallAbiRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CallAbiRT, Verify) { roundTripX86(GetParam()); }
class A64CallAbiRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CallAbiRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CallAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CallAbiRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCallAbiTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Ten integer arguments: forces stack-spilled call arguments on every
    // target (x86-64 spills 4, AArch64 spills 2, ARM32 spills 6, i386 spills
    // all).  The helper folds all ten so none can be dropped, and the result is
    // mixed back into the seed each iteration.
    {p+"_manyarg",
     "static unsigned "+p+"_ma(unsigned,unsigned,unsigned,unsigned,unsigned,"
     "unsigned,unsigned,unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_manyarg("+t+" a){\n"
     "  unsigned s=(unsigned)a; unsigned acc=0x811C9DC5u;\n"
     "  for(int k=0;k<40;k++){\n"
     "    unsigned r="+p+"_ma(s, s^acc, s+(unsigned)k, acc, s*3u,\n"
     "                        acc>>2, s^0x9E3779B9u, acc*7u, s+acc,\n"
     "                        (unsigned)k*131u);\n"
     "    acc=(acc^r)*16777619u; s=s*1664525u+1013904223u; }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_ma(unsigned a,unsigned b,unsigned c,unsigned d,\n"
     "  unsigned e,unsigned f,unsigned g,unsigned h,unsigned i,unsigned j){\n"
     "  return ((a*131u+b)^(c<<3))+(d-e)+(f^g)+(h*5u)-i+(j>>1); }\n",
     {0x1357ULL}, "CallAbi", 2},

    // Small struct by value AND by return: the 8-byte aggregate rides in two
    // registers (or memory on i386 sret); the helper consumes one and produces
    // another each iteration, so both directions must round-trip.
    {p+"_structval",
     "struct "+p+"_sv{ unsigned a,b; };\n"
     "static struct "+p+"_sv "+p+"_step(struct "+p+"_sv,unsigned)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_structval("+t+" a){\n"
     "  struct "+p+"_sv s={(unsigned)a,(unsigned)a^0x9E3779B9u};\n"
     "  for(int k=0;k<50;k++) s="+p+"_step(s,(unsigned)k);\n"
     "  return ("+t+")(unsigned long)(s.a*31u+s.b); }\n"
     "static struct "+p+"_sv "+p+"_step(struct "+p+"_sv s,unsigned k){\n"
     "  struct "+p+"_sv r; r.a=s.a*2654435761u+s.b; r.b=(s.b^k)+(s.a>>3);\n"
     "  return r; }\n",
     {0x2468ULL}, "CallAbi", 2},

    // Four-deep direct call chain a->b->c->d: each frame transforms its argument
    // and mixes the callee result with a value live across the call, stressing
    // the callee-saved save/restore at every level.
    {p+"_chain",
     "static unsigned "+p+"_d(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_c(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_b(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_chain("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<40;i++) acc="+p+"_b(acc^(unsigned)i)+(unsigned)i;\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_b(unsigned x){ return "+p+"_c(x*3u+1u)^(x>>5); }\n"
     "static unsigned "+p+"_c(unsigned x){ return "+p+"_d(x^0x85EBCA6Bu)+(x<<2); }\n"
     "static unsigned "+p+"_d(unsigned x){ return (x*2654435761u)^(x>>15); }\n",
     {0x9ABCULL}, "CallAbi", 2},

    // Call inside a loop with a branchy callee: the accumulator and hash are
    // caller-saved across each call and the callee has its own control flow, so
    // the reload after the call must observe the post-call value, not a stale
    // pre-call copy.
    {p+"_callloop",
     "static unsigned "+p+"_op(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_callloop("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<64;i++){ acc="+p+"_op(acc,(unsigned)i); h=h*31u+acc; }\n"
     "  return ("+t+")(unsigned long)h; }\n"
     "static unsigned "+p+"_op(unsigned acc,unsigned k){\n"
     "  if(acc&1u) return acc*3u+1u;\n"
     "  if(k&2u) return acc>>1;\n"
     "  return (acc^k)+0x9E37u; }\n",
     {0xBEEFULL}, "CallAbi", 2},

    // Mutual recursion between two distinct functions with sibling (tail) calls;
    // depth is bounded by the low bits so any input terminates.  Exercises an
    // a->b->a->b call graph with each frame's value mixed after the call.
    {p+"_mutual",
     "static unsigned "+p+"_odd(unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_even(unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_mutual("+t+" a){\n"
     "  unsigned n=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_even(n&15u, n>>4); }\n"
     "static unsigned "+p+"_even(unsigned d,unsigned v){\n"
     "  if(d==0) return v*2654435761u+1u;\n"
     "  return "+p+"_odd(d-1u, v^0x9E3779B9u)+v; }\n"
     "static unsigned "+p+"_odd(unsigned d,unsigned v){\n"
     "  if(d==0) return v+7u;\n"
     "  return "+p+"_even(d-1u, (v<<1)|1u)*3u; }\n",
     {0xF5ULL}, "CallAbi", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCallAbiTC("x64ca", "long");
static const std::vector<RoundTripTC> kX86 = makeCallAbiTC("x86ca", "int");
static const std::vector<RoundTripTC> kA64 = makeCallAbiTC("a64ca", "long");
static const std::vector<RoundTripTC> kARM = makeCallAbiTC("armca", "int");

INSTANTIATE_TEST_SUITE_P(CallAbi, X64CallAbiRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CallAbi, X86CallAbiRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CallAbi, A64CallAbiRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CallAbi, ARM32CallAbiRT, ::testing::ValuesIn(kARM), rtTCName);
