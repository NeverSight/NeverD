//===- AllPlatform_VectorAlgo52RTTests.cpp - vector reduction → switch ----===//
//
// Fifty-second batch of clang -O2 vector probes targeting a data path no prior
// batch has driven: the SCALAR result of an auto-vectorized HORIZONTAL REDUCTION
// is immediately consumed as a SWITCH selector, so a jump-table dispatch block
// is reached right after the vector reduction epilogue (PSADBW/PHADDW/PSHUFD
// extract on x86, ADDV/UADDLV on a64, VPADD/VPADAL on arm32).  This is the
// intersection of two recently-fragile mechanisms:
//   * #532 bug②: a vectorized loop body stalled `NdOpEmulator` so the jump-table
//                base `lea` could not be folded (INTRINSIC non-stall fix).  Here
//                the vector intrinsics dominate the dispatch block, re-exercising
//                that folding path with the reduced value ALSO feeding the index.
//   * #530/#533: jump-table index normalization — the switch selector is derived
//                from a reduction result (`sum & 7`, `(sum>>2)&15`), so the
//                resolver must anchor the table while the index traces back
//                through the horizontal-reduction extract, not a plain register.
//
// VectorAlgo50 `_sadmin` fed a vector reduction into a min/argmin BRANCH; nothing
// has fed one into a multi-way SWITCH (jump table).  Each kernel wraps the
// reduce-then-dispatch in an outer loop with a loop-carried u32 accumulator and
// mutates the source buffer per iteration so successive reductions (and thus the
// selected arm) differ.  All accumulators are u32 (sums ≤ 255·256), shifts are
// constant, no i64 divide → i386/ARM32 would be libcall-free, but i386 is skipped
// (matching VectorAlgo45-51).  Each kernel folds to one exact integer for a
// bit-exact original-vs-lifted compare; inputs are LCG-seeded from the argument.
// x64 uses -mssse3 (PSADBW/PHADDW); a64/arm32 use the default NEON baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo52RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo52RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo52RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo52RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo52RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo52RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec52TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Byte-sum horizontal reduction (PADDW/PSADBW widening) feeding an 8-way
    // switch (dense jump table): the reduced scalar is the dispatch selector and
    // the table base materializes right after the reduction epilogue.
    {p+"_redsw8",
     t+" "+p+"_redsw8("+t+" a){\n"
     "  unsigned char buf[128]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1664525u+1013904223u; buf[i]=(unsigned char)(s>>16); }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<40;r++){\n"
     "    unsigned sum=0;\n"
     "    for(int i=0;i<128;i++) sum += buf[i];\n"
     "    switch(sum & 7u){\n"
     "      case 0: acc += sum; break;\n"
     "      case 1: acc ^= sum<<3; break;\n"
     "      case 2: acc -= sum>>1; break;\n"
     "      case 3: acc += sum*3u; break;\n"
     "      case 4: acc ^= sum<<7; break;\n"
     "      case 5: acc -= sum; break;\n"
     "      case 6: acc += sum<<5; break;\n"
     "      default: acc ^= sum*131u; break; }\n"
     "    buf[r & 127] = (unsigned char)(acc + (unsigned)r);\n"
     "    acc ^= acc>>11; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo52", opt, fl},

    // 16-bit sum reduction feeding a 16-way DENSE jump table on (sum>>2)&15: a
    // wider table reached from a word-reduction extract.
    {p+"_redsw16",
     t+" "+p+"_redsw16("+t+" a){\n"
     "  unsigned short buf[96]; unsigned s=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u; buf[i]=(unsigned short)(s>>8); }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<36;r++){\n"
     "    unsigned sum=0;\n"
     "    for(int i=0;i<96;i++) sum += buf[i];\n"
     "    switch((sum>>2)&15u){\n"
     "      case 0: acc+=sum; break;\n"
     "      case 1: acc^=sum<<2; break;\n"
     "      case 2: acc-=sum<<1; break;\n"
     "      case 3: acc+=sum*3u; break;\n"
     "      case 4: acc^=sum<<5; break;\n"
     "      case 5: acc-=sum; break;\n"
     "      case 6: acc+=sum<<7; break;\n"
     "      case 7: acc^=sum*5u; break;\n"
     "      case 8: acc+=sum<<9; break;\n"
     "      case 9: acc^=sum>>3; break;\n"
     "      case 10: acc-=sum<<3; break;\n"
     "      case 11: acc+=sum*9u; break;\n"
     "      case 12: acc^=sum<<11; break;\n"
     "      case 13: acc-=sum<<2; break;\n"
     "      case 14: acc+=sum<<4; break;\n"
     "      default: acc^=sum*131u; break; }\n"
     "    buf[r % 96] = (unsigned short)(acc + (unsigned)r*7u);\n"
     "    acc ^= acc>>13; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2345678ULL}, "VectorAlgo52", opt, fl},

    // SAD (PSADBW / UABD+UADALP / VABD+VPADAL) reduction feeding an 8-way switch:
    // the dispatch selector is taken from a sum-of-absolute-differences extract,
    // a distinct horizontal-reduction opcode from the plain byte sum.
    {p+"_redsad",
     t+" "+p+"_redsad("+t+" a){\n"
     "  unsigned char x[128], y[128]; unsigned s=(unsigned)a|3u;\n"
     "  for(int i=0;i<128;i++){ s=s*1664525u+1013904223u; x[i]=(unsigned char)(s>>16);\n"
     "    s=s*1664525u+1013904223u; y[i]=(unsigned char)(s>>16); }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<40;r++){\n"
     "    unsigned sad=0;\n"
     "    for(int i=0;i<128;i++){ int d=(int)x[i]-(int)y[i]; sad += (unsigned)(d<0?-d:d); }\n"
     "    switch(sad & 7u){\n"
     "      case 0: acc += sad; break;\n"
     "      case 1: acc ^= sad<<4; break;\n"
     "      case 2: acc -= sad>>2; break;\n"
     "      case 3: acc += sad*3u; break;\n"
     "      case 4: acc ^= sad<<6; break;\n"
     "      case 5: acc -= sad<<1; break;\n"
     "      case 6: acc += sad<<8; break;\n"
     "      default: acc ^= sad*131u; break; }\n"
     "    x[r & 127] = (unsigned char)(acc + (unsigned)r);\n"
     "    y[(r*3) & 127] = (unsigned char)(acc>>3);\n"
     "    acc ^= acc>>7; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3456789ULL}, "VectorAlgo52", opt, fl},

    // Threshold-count (PCMPGTB mask reduction) AND a byte-sum in the same loop,
    // selector mixes both reductions `(cnt+sum)&7` — two distinct horizontal
    // reductions both feeding the dispatch index before the jump.
    {p+"_redcnt",
     t+" "+p+"_redcnt("+t+" a){\n"
     "  unsigned char buf[160]; unsigned s=(unsigned)a+0x55u;\n"
     "  for(int i=0;i<160;i++){ s=s*22695477u+1u; buf[i]=(unsigned char)(s>>9); }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<32;r++){\n"
     "    unsigned sum=0, cnt=0;\n"
     "    for(int i=0;i<160;i++){ unsigned b=buf[i]; sum += b; if(b>128u) cnt++; }\n"
     "    switch((cnt+sum) & 7u){\n"
     "      case 0: acc += sum; break;\n"
     "      case 1: acc ^= cnt<<5; break;\n"
     "      case 2: acc -= sum>>1; break;\n"
     "      case 3: acc += cnt*7u; break;\n"
     "      case 4: acc ^= sum<<3; break;\n"
     "      case 5: acc -= cnt; break;\n"
     "      case 6: acc += (sum^cnt)<<2; break;\n"
     "      default: acc ^= (sum+cnt)*131u; break; }\n"
     "    buf[r % 160] = (unsigned char)(acc + cnt);\n"
     "    buf[(r*5) % 160] = (unsigned char)(sum + (unsigned)r);\n"
     "    acc ^= acc>>9; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x456789AULL}, "VectorAlgo52", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec52 =
    makeVec52TC("x64v52", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec52 =
    makeVec52TC("a64v52", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec52 =
    makeVec52TC("armv52", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo52, X64VectorAlgo52RT,
                         ::testing::ValuesIn(kX64Vec52), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo52, A64VectorAlgo52RT,
                         ::testing::ValuesIn(kA64Vec52), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo52, ARM32VectorAlgo52RT,
                         ::testing::ValuesIn(kARM32Vec52), rtTCName);
