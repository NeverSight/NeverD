//===- AllPlatform_FuncPtrCallRTTests.cpp - function-pointer dispatch ---===//
//
// High-yield roundtrip probing of indirect calls through a function-pointer
// table — the callback-table / vtable / plugin-dispatch shape.  This is the
// "code-pointer dual" of the constant-pool data mapping (#397) and the
// computed-goto code-address table (#400): clang materialises each leaf
// function's address into a `.data.rel.ro` array of absolute code-pointer
// relocations, then issues `call *reg` / `blr` / `blx reg` through it.  The
// lifter must recover those slots as references to the *recompiled* functions
// (not the original absolute VAs, which point nowhere after relinking at
// CODE_BASE).  The dispatcher is defined first so it lands at the start of
// .text (the harness entry); every leaf is a non-leaf participant reached only
// indirectly.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPtrRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPtrRT, Verify) { roundTripX64(GetParam()); }
class X86FPtrRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPtrRT, Verify) { roundTripX86(GetParam()); }
class A64FPtrRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPtrRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32FPtrRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPtrRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPtrTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 4-way callback table; the dispatcher is defined before the leaves so it is
    // first in .text.  The index is data-dependent so clang cannot devirtualize.
    {p+"_cb4",
     "typedef unsigned (*fn_t)(unsigned);\n"
     "static unsigned "+p+"_a(unsigned);\n"
     "static unsigned "+p+"_x(unsigned);\n"
     "static unsigned "+p+"_m(unsigned);\n"
     "static unsigned "+p+"_r(unsigned);\n"
     +t+" "+p+"_cb4("+t+" a){\n"
     "  static fn_t tab[4]={"+p+"_a,"+p+"_x,"+p+"_m,"+p+"_r};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<64;i++) acc=tab[acc&3](acc)+(unsigned)i;\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_a(unsigned x){ return x+0x9E3779B9u; }\n"
     "static unsigned "+p+"_x(unsigned x){ return x^(x>>13)^(x<<7); }\n"
     "static unsigned "+p+"_m(unsigned x){ return x*2654435761u; }\n"
     "static unsigned "+p+"_r(unsigned x){ return (x<<11)|(x>>21); }\n",
     {0x12345ULL}, "FPtr", 1},

    // Two-argument handlers selected from the running accumulator, returning the
    // next state — exercises the spill/reload of caller-saved values around an
    // indirect call plus argument marshalling.
    {p+"_sm",
     "typedef unsigned (*op_t)(unsigned,unsigned);\n"
     "static unsigned "+p+"_o0(unsigned,unsigned);\n"
     "static unsigned "+p+"_o1(unsigned,unsigned);\n"
     "static unsigned "+p+"_o2(unsigned,unsigned);\n"
     "static unsigned "+p+"_o3(unsigned,unsigned);\n"
     "static unsigned "+p+"_o4(unsigned,unsigned);\n"
     +t+" "+p+"_sm("+t+" a){\n"
     "  static op_t ops[5]={"+p+"_o0,"+p+"_o1,"+p+"_o2,"+p+"_o3,"+p+"_o4};\n"
     "  unsigned acc=(unsigned)a|1u, h=0x811C9DC5u;\n"
     "  for(int i=0;i<80;i++){ unsigned k=(acc^(unsigned)i)%5u;\n"
     "    acc=ops[k](acc,h); h=(h^acc)*16777619u; }\n"
     "  return ("+t+")(unsigned long)(acc^h); }\n"
     "static unsigned "+p+"_o0(unsigned a,unsigned b){ return a+b+0x9E37u; }\n"
     "static unsigned "+p+"_o1(unsigned a,unsigned b){ return a^(b>>3); }\n"
     "static unsigned "+p+"_o2(unsigned a,unsigned b){ return a*3u-b; }\n"
     "static unsigned "+p+"_o3(unsigned a,unsigned b){ return (a<<5)|(b>>27); }\n"
     "static unsigned "+p+"_o4(unsigned a,unsigned b){ return a-(b<<1); }\n",
     {0xABCDEULL}, "FPtr", 1},

    // A struct of function pointers (vtable shape): the dispatcher walks a small
    // array of objects, each carrying its own handler, and folds the results.
    {p+"_vt",
     "typedef unsigned (*vf_t)(unsigned);\n"
     "static unsigned "+p+"_h0(unsigned);\n"
     "static unsigned "+p+"_h1(unsigned);\n"
     "static unsigned "+p+"_h2(unsigned);\n"
     "struct obj{ vf_t f; unsigned k; };\n"
     +t+" "+p+"_vt("+t+" a){\n"
     "  struct obj v[3]={{"+p+"_h0,3u},{"+p+"_h1,5u},{"+p+"_h2,7u}};\n"
     "  unsigned acc=(unsigned)a; \n"
     "  for(int i=0;i<66;i++){ struct obj *o=&v[(acc>>2)%3u];\n"
     "    acc=o->f(acc)+o->k*(unsigned)i; }\n"
     "  return ("+t+")(unsigned long)acc; }\n"
     "static unsigned "+p+"_h0(unsigned x){ return (x*2654435761u)^(x>>15); }\n"
     "static unsigned "+p+"_h1(unsigned x){ return x+0x85EBCA6Bu; }\n"
     "static unsigned "+p+"_h2(unsigned x){ return (x<<13)|(x>>19); }\n",
     {0x2222ULL}, "FPtr", 1},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeFPtrTC("x64fp", "long");
static const std::vector<RoundTripTC> kX86 = makeFPtrTC("x86fp", "int");
static const std::vector<RoundTripTC> kA64 = makeFPtrTC("a64fp", "long");
static const std::vector<RoundTripTC> kARM = makeFPtrTC("armfp", "int");

INSTANTIATE_TEST_SUITE_P(FPtr, X64FPtrRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPtr, X86FPtrRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPtr, A64FPtrRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPtr, ARM32FPtrRT, ::testing::ValuesIn(kARM), rtTCName);
