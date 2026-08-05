//===- AllPlatform_OptStress100RTTests.cpp - data-driven rodata shapes -*-C++-==//
//
// Green guardrails for three rodata/global access SHAPES not previously pinned,
// all address-independent (the folded result depends only on the bytes stored
// in the globals and the control flow, never on an absolute VA), so the
// roundtrip comparison is meaningful on every target.
//
//   * dfa   - a 2D rodata transition table walked as `tt[state*16 + cls]` where
//             the value read becomes the next row index (nested indexed rodata
//             load: the index feeding the next read comes from the prior read),
//             plus a per-state `emit[]` rodata weight.  Exercises the
//             `base + row*stride + col` literal-pool peel with a *data-derived*
//             row that changes every step.
//   * hist  - a rodata input stream classified into a WRITABLE `.bss` histogram
//             (indexed RMW store into a mutable global), re-zeroed each pass,
//             then reduced against a rodata weight table.  Pins indexed `.bss`
//             store + rodata read in one loop.
//   * skip  - a forward bytecode walk whose STRIDE is read from the tape itself
//             (`pc += 1 + (tape[pc]&3)`), a dense `switch` (jump table) per op.
//             Data-driven variable-stride forward walk from the array base.
//
// All start their walks at the array base (index 0, forward) so none depends on
// the deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); all
// four targets run every probe.  Integer in / integer out, file-scope const
// (rodata) + one .bss array, LCG-seeded, folded to one integer return; no float
// / 64-bit divide / libcall.  -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress100RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress100RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress100RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress100RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress100RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress100RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress100RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress100RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress100TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D rodata DFA: tt[state*16+cls] -> next state (data-derived row), emit[].
    {p+"_dfa",
     "static const unsigned char "+p+"_tt[128]={\n"
     "1,2,3,0,4,5,6,7, 2,3,4,1,5,6,7,0,\n"   // state 0
     "3,4,5,2,6,7,0,1, 4,5,6,3,7,0,1,2,\n"   // state 1
     "5,6,7,4,0,1,2,3, 6,7,0,5,1,2,3,4,\n"   // state 2
     "7,0,1,6,2,3,4,5, 0,1,2,7,3,4,5,6,\n"   // state 3
     "2,4,6,0,1,3,5,7, 3,5,7,1,2,4,6,0,\n"   // state 4
     "4,6,0,2,3,5,7,1, 5,7,1,3,4,6,0,2,\n"   // state 5
     "6,0,2,4,5,7,1,3, 7,1,3,5,6,0,2,4,\n"   // state 6
     "0,2,4,6,7,1,3,5, 1,3,5,7,0,2,4,6};\n"  // state 7
     "static const unsigned char "+p+"_emit[8]={11,29,47,5,17,23,3,41};\n"
     +t+" "+p+"_dfa("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned st=0, acc=0;\n"
     "    for(int k=0;k<24;k++){\n"
     "      unsigned cls=((s>>(k&15))^(unsigned)k)&15u;\n"
     "      st="+p+"_tt[st*16u+cls]&7u;\n"
     "      acc=acc*131u+"+p+"_emit[st]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC1u}, "OptStress100", 2},

    // rodata input -> indexed .bss histogram RMW -> rodata-weighted reduce.
    {p+"_hist",
     "static const unsigned char "+p+"_inp[40]={\n"
     "3,9,1,14,7,2,11,5, 0,13,6,8,4,15,10,12,\n"
     "2,7,1,9,3,11,5,0, 14,6,13,4,8,15,10,1,\n"
     "5,2,9,7,3,11,0,14};\n"
     "static const unsigned "+p+"_wt[16]={\n"
     "7u,13u,3u,29u,5u,41u,11u,2u,17u,23u,31u,37u,19u,43u,47u,53u};\n"
     "static unsigned "+p+"_cnt[16];\n"
     +t+" "+p+"_hist("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<32;it++){ s=s*1103515245u+12345u;\n"
     "    for(int i=0;i<16;i++) "+p+"_cnt[i]=0;\n"
     "    for(int i=0;i<40;i++){ unsigned c=("+p+"_inp[i]^(s>>(i&7)))&15u; "+p+"_cnt[c]++; }\n"
     "    unsigned acc=0;\n"
     "    for(int i=0;i<16;i++) acc+="+p+"_cnt[i]*"+p+"_wt[i];\n"
     "    out=out*131u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x28u}, "OptStress100", 2},

    // forward bytecode walk, stride read from the tape, dense switch jump table.
    {p+"_skip",
     "static const unsigned char "+p+"_tape[48]={\n"
     "0,5,1,7,2,4,3,9, 4,3,5,6,6,8,7,2,\n"
     "1,6,2,8,3,11,0,4, 5,1,6,3,7,0,2,9,\n"
     "3,4,4,5,5,2,6,7, 7,1,0,6,1,4,2,3};\n"
     +t+" "+p+"_skip("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<48;it++){ s=s*1103515245u+12345u; unsigned acc=s; int pc=0, steps=0;\n"
     "    while(pc<48 && steps<40){ unsigned op="+p+"_tape[pc]; unsigned st=1u+("+p+"_tape[pc]&3u);\n"
     "      switch(op&7){\n"
     "        case 0: acc+=op*131u; break;\n"
     "        case 1: acc^=(op<<5)|op; break;\n"
     "        case 2: acc=(acc<<(op&15u))|(acc>>((32u-(op&15u))&31u)); break;\n"
     "        case 3: acc-=op*7u; break;\n"
     "        case 4: acc*=(op|1u); break;\n"
     "        case 5: acc|=op<<3; break;\n"
     "        case 6: acc&=~(unsigned)(op<<1); break;\n"
     "        default: acc+=acc>>3; break; }\n"
     "      out=out*31u+acc; pc+=(int)st; steps++; } }\n"
     "  return ("+t+")out; }\n",
     {0x30u}, "OptStress100", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress100TC("x64o100", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress100TC("x86o100", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress100TC("a64o100", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress100TC("armo100", "int");

INSTANTIATE_TEST_SUITE_P(OptStress100, X64OptStress100RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress100, X86OptStress100RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress100, A64OptStress100RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress100, ARM32OptStress100RT, ::testing::ValuesIn(kARM), rtTCName);
