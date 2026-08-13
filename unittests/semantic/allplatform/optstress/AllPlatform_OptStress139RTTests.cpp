//===- AllPlatform_OptStress139RTTests.cpp - RPN eval / brackets / DFA ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * rpn    - reverse-polish (postfix) evaluator over a rodata token stream
//              using an explicit value stack.  Pins a push/pop stack machine
//              with a `switch` ALU (distinct from any straight-line accumulate).
//   * paren  - multi-type bracket matcher over a rotated rodata stream tracking
//              stack depth and validity.  Pins a LIFO open/close pairing
//              (distinct from the RPN value stack: pure structure, no ALU).
//   * dfa    - table-driven finite automaton over a rodata transition table and
//              a rodata input stream.  Pins a `state=trans[state*K+sym]` indexed
//              transition walk (distinct from the prefix automaton in #129).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress139RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress139RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress139RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress139RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress139RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress139RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress139RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress139RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress139TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // reverse-polish evaluator over a rodata token stream (push/pop + switch ALU).
    {p+"_rpn",
     "static const unsigned char "+p+"_prog[40]={\n"
     "3,5,16,8,17,2,18,9, 16,4,19,7,18,1,16,6, 17,5,18,2,16,3,19,8,\n"
     "16,4,17,9,18,1,16,7, 19,2,16,5,18,3,17,6};\n"
     +t+" "+p+"_rpn("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned st[32]; int sp=0;\n"
     "    for(int i=0;i<40;i++){ unsigned tk="+p+"_prog[i]^((s>>(i&7))&1u);\n"
     "      if(tk<16u){ if(sp<32) st[sp++]=tk; }\n"
     "      else if(sp>=2){ unsigned b=st[--sp], aa=st[--sp], r;\n"
     "        switch(tk&3u){ case 0: r=aa+b; break; case 1: r=aa-b; break;\n"
     "          case 2: r=aa*b; break; default: r=aa^b; }\n"
     "        if(sp<32) st[sp++]=r&0xFFFFu; }\n"
     "      acc=acc*131u+(unsigned)sp+(sp>0?st[sp-1]:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x27u}, "OptStress139", 2},

    // multi-type bracket matcher over a rodata stream (LIFO open/close pairing).
    {p+"_paren",
     "static const unsigned char "+p+"_br[32]={\n"
     "1,3,5,6,4,2,1,1, 2,2,3,5,6,4,5,6, 1,3,3,4,4,2,5,1, 6,2,3,5,6,4,1,2};\n"
     +t+" "+p+"_paren("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned st[32]; int sp=0; unsigned ok=1u, maxd=0u;\n"
     "    for(int i=0;i<32;i++){ unsigned c="+p+"_br[i];\n"
     "      if(c==1u||c==3u||c==5u){ if(sp<32){ st[sp++]=c; if((unsigned)sp>maxd) maxd=(unsigned)sp; } }\n"
     "      else { if(sp>0){ unsigned op=st[--sp]; if(op+1u!=c) ok=0u; } else ok=0u; }\n"
     "      acc=acc*131u+(unsigned)sp; }\n"
     "    acc=acc*131u+ok+maxd+(sp==0?1u:0u); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x44u}, "OptStress139", 2},

    // table-driven finite automaton over a rodata transition table + input.
    {p+"_dfa",
     "static const unsigned char "+p+"_trans[32]={\n"
     "1,2,0,3, 2,3,1,0, 3,0,2,1, 0,1,3,2, 5,6,4,7, 6,7,5,4, 7,4,6,5, 4,5,7,6};\n"
     "static const unsigned char "+p+"_in[40]={\n"
     "0,1,2,3,1,0,3,2, 2,3,0,1,3,2,1,0, 1,2,3,0,0,3,2,1, 3,0,1,2,2,1,0,3,\n"
     "0,2,1,3,3,1,2,0};\n"
     +t+" "+p+"_dfa("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned state=s&7u, visits=0u;\n"
     "    for(int i=0;i<40;i++){ unsigned sym=("+p+"_in[i]^((s>>(i&7))&3u))&3u;\n"
     "      state="+p+"_trans[state*4u+sym]&7u; if(state==0u) visits++; acc=acc*131u+state; }\n"
     "    acc=acc*131u+visits; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x73u}, "OptStress139", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress139TC("x64o139", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress139TC("x86o139", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress139TC("a64o139", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress139TC("armo139", "int");

INSTANTIATE_TEST_SUITE_P(OptStress139, X64OptStress139RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress139, X86OptStress139RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress139, A64OptStress139RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress139, ARM32OptStress139RT, ::testing::ValuesIn(kARM), rtTCName);
