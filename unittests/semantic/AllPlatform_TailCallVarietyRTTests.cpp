//===- AllPlatform_TailCallVarietyRTTests.cpp - tail call variety -*-C++*-=//
//
// Roundtrip probes that stress the tail-call recovery activated in #412 beyond
// direct mutual recursion: an indirect tail call through a runtime-selected
// function pointer (`jmp *reg` / `br reg` / `bx reg`), an indirect tail call
// dispatched through a `.data.rel.ro` function-pointer table, a conditional
// indirect tail call, a direct tail call to a nine-argument callee that spills
// onto the stack, an eight-argument forwarding tail call straddling the
// register/stack boundary, and a transform-then-indirect-tail mix.  Each folds
// its result into one integer return; all integer (no soft-float libcall),
// compiled at -O2 and checked native vs lifted on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64TailVarietyRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64TailVarietyRT, Verify) { roundTripX64(GetParam()); }
class X86TailVarietyRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86TailVarietyRT, Verify) { roundTripX86(GetParam()); }
class A64TailVarietyRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64TailVarietyRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32TailVarietyRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32TailVarietyRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeTailVarietyTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Indirect tail call through a function-pointer table (.data.rel.ro): the
    // probe ends in `jmp *tab(,idx,k)` / `br`/`bx`.  Combines #402 (code-pointer
    // table) with the #412 tail-call position.
    {p+"_fptable",
     "static unsigned "+p+"_a(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_b(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_c(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_d(unsigned) __attribute__((noinline));\n"
     "typedef unsigned (*"+p+"_fn)(unsigned);\n"
     "static const "+p+"_fn "+p+"_tab[4]={"+p+"_a,"+p+"_b,"+p+"_c,"+p+"_d};\n"
     +t+" "+p+"_fptable("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_tab[v&3u](v); }\n"
     "static unsigned "+p+"_a(unsigned x){ return x*2654435761u+1u; }\n"
     "static unsigned "+p+"_b(unsigned x){ return (x^0x9e3779b9u)*131u; }\n"
     "static unsigned "+p+"_c(unsigned x){ return (x>>3)*7u+x; }\n"
     "static unsigned "+p+"_d(unsigned x){ return x - (x>>5) + 0x1357u; }\n",
     {0x41ULL}, "TailVar", 2},

    // Conditional indirect tail call: a PHI of two function addresses feeds the
    // indirect tail jump.
    {p+"_fpcond",
     "static unsigned "+p+"_p(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_q(unsigned) __attribute__((noinline));\n"
     "typedef unsigned (*"+p+"_fn)(unsigned);\n"
     +t+" "+p+"_fpcond("+t+" a){ unsigned v=(unsigned)a;\n"
     "  volatile "+p+"_fn s0="+p+"_p, s1="+p+"_q;\n"
     "  "+p+"_fn fp=(v&1u)? s1 : s0;\n"
     "  return ("+t+")(unsigned long)fp(v*3u+1u); }\n"
     "static unsigned "+p+"_p(unsigned x){ unsigned h=x; for(unsigned i=0;i<6;i++) h=h*131u+i; return h; }\n"
     "static unsigned "+p+"_q(unsigned x){ return (x*2654435761u)^(x>>7); }\n",
     {0x53ULL}, "TailVar", 2},

    // Direct tail call to a nine-argument callee: the last arguments spill onto
    // the stack, so the tail call must set up stack arguments before the jump.
    {p+"_stackarg",
     "static unsigned "+p+"_w9(unsigned,unsigned,unsigned,unsigned,unsigned,\n"
     "                        unsigned,unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_stackarg("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_w9(v,v^1u,v^2u,v^3u,v^4u,v^5u,v^6u,v^7u,v^8u); }\n"
     "static unsigned "+p+"_w9(unsigned a0,unsigned a1,unsigned a2,unsigned a3,unsigned a4,\n"
     "                         unsigned a5,unsigned a6,unsigned a7,unsigned a8){\n"
     "  return ((((((((a0*31u+a1)*31u+a2)*31u+a3)*31u+a4)*31u+a5)*31u+a6)*31u+a7)*31u+a8); }\n",
     {0x67ULL}, "TailVar", 2},

    // Eight-argument forwarding tail call straddling the register/stack boundary:
    // the forwarder passes its own arguments straight through.
    {p+"_fwd8",
     "static unsigned "+p+"_g8(unsigned,unsigned,unsigned,unsigned,unsigned,\n"
     "                        unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_f8(unsigned,unsigned,unsigned,unsigned,unsigned,\n"
     "                        unsigned,unsigned,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_fwd8("+t+" a){ unsigned v=(unsigned)a;\n"
     "  return ("+t+")(unsigned long)"+p+"_f8(v,v+1u,v+2u,v+3u,v+4u,v+5u,v+6u,v+7u); }\n"
     "static unsigned "+p+"_f8(unsigned a0,unsigned a1,unsigned a2,unsigned a3,unsigned a4,\n"
     "                         unsigned a5,unsigned a6,unsigned a7){\n"
     "  return "+p+"_g8(a0,a1,a2,a3,a4,a5,a6,a7); }\n"
     "static unsigned "+p+"_g8(unsigned a0,unsigned a1,unsigned a2,unsigned a3,unsigned a4,\n"
     "                         unsigned a5,unsigned a6,unsigned a7){\n"
     "  return (((((((a0*31u+a1)*31u+a2)*31u+a3)*31u+a4)*31u+a5)*31u+a6)*31u+a7); }\n",
     {0x71ULL}, "TailVar", 2},

    // Transform-then-indirect-tail: compute a new argument, then indirect tail
    // call through a runtime pointer.
    {p+"_xform",
     "static unsigned "+p+"_h(unsigned) __attribute__((noinline));\n"
     "typedef unsigned (*"+p+"_fn)(unsigned);\n"
     +t+" "+p+"_xform("+t+" a){ unsigned v=(unsigned)a;\n"
     "  volatile "+p+"_fn s="+p+"_h; "+p+"_fn fp=s;\n"
     "  return ("+t+")(unsigned long)fp((v*2654435761u)^(v>>3)); }\n"
     "static unsigned "+p+"_h(unsigned x){ unsigned h=x; for(unsigned i=0;i<8;i++) h=h*31u+((x>>(i&7))&0xffu); return h; }\n",
     {0x29ULL}, "TailVar", 2},

    // Indirect tail call in a dispatch loop folded to a final tail: a state
    // machine selects the next handler each step, the last step tail-calls it.
    {p+"_dispatch",
     "static unsigned "+p+"_s0(unsigned) __attribute__((noinline));\n"
     "static unsigned "+p+"_s1(unsigned) __attribute__((noinline));\n"
     "typedef unsigned (*"+p+"_fn)(unsigned);\n"
     "static const "+p+"_fn "+p+"_st[2]={"+p+"_s0,"+p+"_s1};\n"
     +t+" "+p+"_dispatch("+t+" a){ unsigned v=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<8;i++){ h=h*131u+"+p+"_st[v&1u](v); v=v*1664525u+1013904223u; }\n"
     "  return ("+t+")(unsigned long)("+p+"_st[v&1u](h)+h); }\n"
     "static unsigned "+p+"_s0(unsigned x){ return x*2654435761u+1u; }\n"
     "static unsigned "+p+"_s1(unsigned x){ return (x^0x9e3779b9u)*131u; }\n",
     {0x35ULL}, "TailVar", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeTailVarietyTC("x64tv", "long");
static const std::vector<RoundTripTC> kX86 = makeTailVarietyTC("x86tv", "int");
static const std::vector<RoundTripTC> kA64 = makeTailVarietyTC("a64tv", "long");
static const std::vector<RoundTripTC> kARM = makeTailVarietyTC("armtv", "int");

INSTANTIATE_TEST_SUITE_P(TailVar, X64TailVarietyRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(TailVar, X86TailVarietyRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(TailVar, A64TailVarietyRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(TailVar, ARM32TailVarietyRT, ::testing::ValuesIn(kARM), rtTCName);
