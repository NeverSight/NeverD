//===- AllPlatform_OptStress115RTTests.cpp - morph / VM / DFA rodata shapes =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * morph  - 3x3 morphological gradient over a rodata 8x8 image: the
//              neighbourhood max minus min (`max-min`).  Pins a 3x3 window
//              order-statistic reduce over rodata (distinct from the weighted
//              Sobel stencil).
//   * vm     - a tiny stack-machine interpreting a rodata bytecode stream
//              (PUSH/ADD/SUB/MUL/XOR/AND/SHL/DUP) on a stack array.  Pins a
//              computed opcode dispatch over a rodata "program" plus an operand
//              gather (`prog[++pc]`).
//   * dfa    - a deterministic finite automaton walked over a rodata input with
//              a 2D transition table `next[state*4+sym]`.  Pins a transition-
//              table gather driving a state recurrence over a rodata symbol
//              stream (distinct from the KMP failure function).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress115RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress115RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress115RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress115RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress115RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress115RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress115RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress115RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress115TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 3x3 morphological gradient (max-min) over a rodata 8x8 image.
    {p+"_morph",
     "static const unsigned char "+p+"_img[64]={\n"
     "12,200,35,50,7,175,85,95, 225,40,55,170,80,90,3,110, 30,145,65,85,250,105,15,120,\n"
     "35,55,190,95,110,5,130,240, 40,60,80,220,115,130,140,2, 45,235,85,105,120,8,145,155,\n"
     "210,70,90,110,1,140,150,160, 55,75,255,115,130,145,4,165};\n"
     +t+" "+p+"_morph("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int r=1;r<7;r++) for(int c=1;c<7;c++){ unsigned mn=255u, mx=0u;\n"
     "      for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){\n"
     "        unsigned v="+p+"_img[(r+dr)*8+(c+dc)]^((s>>((r+c)&7))&1u);\n"
     "        if(v<mn) mn=v; if(v>mx) mx=v; }\n"
     "      acc=acc*131u+(mx-mn); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Du}, "OptStress115", 2},

    // tiny bytecode stack-machine over a rodata program (computed dispatch).
    {p+"_vm",
     "static const unsigned char "+p+"_prog[40]={\n"
     "0,5,0,9,1,0,3,3,0,7,4,0,2,2,7,1,0,6,5,0,11,3,0,4,1,6,0,13,4,0,2,3,7,5,1,0,8,3,4,1};\n"
     +t+" "+p+"_vm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; unsigned seed=s;\n"
     "    unsigned st[32]; int sp=0;\n"
     "    for(int pc=0;pc<40;pc++){ unsigned op="+p+"_prog[pc]&7u;\n"
     "      switch(op){\n"
     "        case 0: if(pc+1<40){ unsigned v="+p+"_prog[pc+1]^(seed&0xFu); pc++; if(sp<32) st[sp++]=v; } break;\n"
     "        case 1: if(sp>=2){ unsigned b=st[--sp], aa=st[--sp]; st[sp++]=aa+b; } break;\n"
     "        case 2: if(sp>=2){ unsigned b=st[--sp], aa=st[--sp]; st[sp++]=aa-b; } break;\n"
     "        case 3: if(sp>=2){ unsigned b=st[--sp], aa=st[--sp]; st[sp++]=aa*b; } break;\n"
     "        case 4: if(sp>=2){ unsigned b=st[--sp], aa=st[--sp]; st[sp++]=aa^b; } break;\n"
     "        case 5: if(sp>=2){ unsigned b=st[--sp], aa=st[--sp]; st[sp++]=aa&b; } break;\n"
     "        case 6: if(sp>=1){ st[sp-1]=st[sp-1]<<1; } break;\n"
     "        default: if(sp>=1 && sp<32){ st[sp]=st[sp-1]; sp++; } break; }\n"
     "      acc=acc*131u+(sp>0?st[sp-1]:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x56u}, "OptStress115", 2},

    // DFA walk over a rodata input with a 2D transition table next[state*4+sym].
    {p+"_dfa",
     "static const unsigned char "+p+"_next[32]={\n"
     "1,3,0,2, 4,0,5,1, 2,6,1,7, 5,2,3,0, 0,7,6,3, 7,1,4,6, 3,5,2,4, 6,4,7,5};\n"
     "static const unsigned char "+p+"_in[48]={\n"
     "2,1,3,0,1,2,3,1, 0,3,2,1,3,0,1,2, 1,2,0,3,2,1,3,0, 3,1,2,0,1,3,2,1, 0,2,3,1,2,0,3,2, 1,3,0,2,3,1,0,2};\n"
     +t+" "+p+"_dfa("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned st=(s>>5)&7u;\n"
     "    for(int i=0;i<48;i++){ unsigned sym=("+p+"_in[i]^(s>>(i&7)))&3u;\n"
     "      st="+p+"_next[st*4u+sym]&7u; acc=acc*131u+st; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xDFu}, "OptStress115", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress115TC("x64o115", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress115TC("x86o115", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress115TC("a64o115", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress115TC("armo115", "int");

INSTANTIATE_TEST_SUITE_P(OptStress115, X64OptStress115RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress115, X86OptStress115RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress115, A64OptStress115RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress115, ARM32OptStress115RT, ::testing::ValuesIn(kARM), rtTCName);
