//===- AllPlatform_VectorAlgo49RTTests.cpp - SAD / abs-diff reductions ----===//
//
// Forty-ninth batch of clang -O2 vector probes covering the SUM-OF-ABSOLUTE-
// DIFFERENCES (SAD) family — an auto-vectorized horizontal-reduction idiom that
// prior batches only touched in a single-register, x64-only packed form
// (X64_SatArithShuffleRTTests/sad_bytes, 8 bytes in one GPR).  The real loop
// shape `sum += |x[i]-y[i]|` over stack arrays lowers to dedicated horizontal
// instructions whose lift had no all-target roundtrip guardrail:
//   * x86-64 : PSADBW (byte SAD → per-64-bit-lane horizontal sum) + PADDQ tail.
//   * AArch64: UABD + UADALP / UABAL widening absolute-difference accumulate.
//   * ARM32  : VABD.u8 + VPADAL.u8 (NEON pairwise widening accumulate).
// The 16-bit variant has no PSADBW equivalent, so it exercises the alternate
// PABSD(PSUBW)+widen / SABDL lowering — a distinct lift path from the byte form.
//
// Every kernel folds to one exact integer for a bit-exact original-vs-lifted
// compare.  Accumulators stay in u32 (byte sums ≤ 255·160, word sums ≤ 65535·96)
// so i386/ARM32 stay libcall-free (no i64 divide / variable i64 shift); inputs
// are LCG-seeded from the single argument.  x64 uses -mssse3; a64/arm32 use the
// default NEON baseline.  Three targets (i386 skipped, matching VectorAlgo45-48).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo49RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo49RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo49RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo49RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo49RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo49RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec49TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Classic byte SAD over two 128-element stack arrays: the canonical PSADBW
    // (x86) / UABD+UADALP (a64) / VABD+VPADAL (arm32) auto-vectorized reduction.
    {p+"_sadb",
     t+" "+p+"_sadb("+t+" a){\n"
     "  unsigned char x[128], y[128]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){ s=s*1664525u+1013904223u; x[i]=(unsigned char)(s>>16);\n"
     "    s=s*1664525u+1013904223u; y[i]=(unsigned char)(s>>16); }\n"
     "  unsigned sum=0;\n"
     "  for(int i=0;i<128;i++){ int d=(int)x[i]-(int)y[i]; sum += (unsigned)(d<0?-d:d); }\n"
     "  return ("+t+")sum;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo49", opt, fl},

    // 16-bit abs-diff accumulate: no PSADBW for words, so this drives the
    // alternate PABSD(PSUBW)+widen / SABDL lowering (distinct from the byte path).
    {p+"_sadw",
     t+" "+p+"_sadw("+t+" a){\n"
     "  unsigned short x[96], y[96]; unsigned s=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u; x[i]=(unsigned short)(s>>8);\n"
     "    s=s*1103515245u+12345u; y[i]=(unsigned short)(s>>8); }\n"
     "  unsigned sum=0;\n"
     "  for(int i=0;i<96;i++){ int d=(int)x[i]-(int)y[i]; sum += (unsigned)(d<0?-d:d); }\n"
     "  return ("+t+")sum;\n"
     "}\n",
     {0x2345678ULL}, "VectorAlgo49", opt, fl},

    // Blocked byte SAD (8 blocks × 16): each block's PSADBW partial must be
    // horizontally extracted and folded into a per-block hash — exercises the
    // partial-sum extraction rather than a single whole-array reduction.
    {p+"_sadblk",
     t+" "+p+"_sadblk("+t+" a){\n"
     "  unsigned char x[128], y[128]; unsigned s=(unsigned)a|3u;\n"
     "  for(int i=0;i<128;i++){ s=s*1664525u+1013904223u; x[i]=(unsigned char)s;\n"
     "    s=s*1664525u+1013904223u; y[i]=(unsigned char)s; }\n"
     "  unsigned acc=0;\n"
     "  for(int b=0;b<8;b++){ unsigned bs=0;\n"
     "    for(int i=0;i<16;i++){ int d=(int)x[b*16+i]-(int)y[b*16+i]; bs += (unsigned)(d<0?-d:d); }\n"
     "    acc = acc*131u + bs; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3456789ULL}, "VectorAlgo49", opt, fl},

    // SAD fused with a threshold compare-and-count in one loop body: PSADBW
    // reduction coexists with a PCMPGTB-style mask popcount over the same diffs.
    {p+"_sadcmp",
     t+" "+p+"_sadcmp("+t+" a){\n"
     "  unsigned char x[160], y[160]; unsigned s=(unsigned)a+0x55u;\n"
     "  for(int i=0;i<160;i++){ s=s*22695477u+1u; x[i]=(unsigned char)(s>>9);\n"
     "    s=s*22695477u+1u; y[i]=(unsigned char)(s>>9); }\n"
     "  unsigned sum=0, cnt=0;\n"
     "  for(int i=0;i<160;i++){ int d=(int)x[i]-(int)y[i]; unsigned ad=(unsigned)(d<0?-d:d);\n"
     "    sum += ad; if(ad>32u) cnt++; }\n"
     "  return ("+t+")(sum*131u + cnt);\n"
     "}\n",
     {0x456789AULL}, "VectorAlgo49", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec49 =
    makeVec49TC("x64v49", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec49 =
    makeVec49TC("a64v49", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec49 =
    makeVec49TC("armv49", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo49, X64VectorAlgo49RT,
                         ::testing::ValuesIn(kX64Vec49), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo49, A64VectorAlgo49RT,
                         ::testing::ValuesIn(kA64Vec49), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo49, ARM32VectorAlgo49RT,
                         ::testing::ValuesIn(kARM32Vec49), rtTCName);
