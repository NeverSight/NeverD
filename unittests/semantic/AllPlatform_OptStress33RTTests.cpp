//===- AllPlatform_OptStress33RTTests.cpp - opt-stress probes --*-C++*-=//
//
// A further optimizer / sub-register probe targeting corners OptStress27/28/32
// did not cover.  The recurring miscompile vein is partial-register (8/16-bit)
// writes whose wide parent is read in another block, plus the self-written
// cross-block merge pass (mergePartialWritesCrossBlockX86) reconciling them.
// These kernels hit that machinery from angles the earlier rounds left open:
//
//   * condmerge - a byte computed in a branch (PHI) then re-packed into a wider
//                 word read cross-block (conditional partial-register merge).
//   * fieldsw   - a switch/jump-table updating distinct sub-fields (byte0/half/
//                 byte3/full) of one loop-carried word read in the latch.
//   * wmix      - an i64 accumulator (fixed 64-bit rotate + multiply) with
//                 narrow 8/16-bit extraction carried across the loop.
//   * bswapmix  - manual bswap32 + bswap16 endian idioms feeding the hash.
//   * nestbyte  - inner loop builds a byte accumulator, outer loop merges it
//                 into the wide parent (cross-loop-nest partial merge).
//   * csext     - a signed char written conditionally then sign-extended read
//                 into i32, with i16 truncation and a signed narrow compare.
//
// Integer-only, single integer return, bounded, no 64-bit divide; all four
// targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress33RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress33RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress33RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress33RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress33RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress33RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress33RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress33RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress33TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A byte computed in a branch (PHI) then re-packed into a wider word that is
    // read cross-block (next iter + hash).  The high half is touched separately
    // so the wide parent value truly matters past the byte merge.
    {p+"_condmerge",
     t+" "+p+"_condmerge("+t+" a){\n"
     "  unsigned acc=(unsigned)a|0x12340000u, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char b=(unsigned char)acc;\n"
     "    if(s&1u) b=(unsigned char)(b+(s>>3));\n"
     "    else if(s&2u) b=(unsigned char)(b^(s>>5));\n"
     "    acc=(acc&0xffffff00u)|b;\n"
     "    if(s&4u) acc+=0x10000u;\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress33", 2},

    // A switch (jump table) updating byte0 / low-half / byte3 / full of one
    // loop-carried word, the word read again in the latch and next iteration.
    {p+"_fieldsw",
     t+" "+p+"_fieldsw("+t+" a){\n"
     "  unsigned w=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>2)&3u){\n"
     "      case 0: w=(w&0xffffff00u)|(unsigned char)(w+s); break;\n"
     "      case 1: w=(w&0xffff0000u)|(unsigned short)(w*5u+s); break;\n"
     "      case 2: w=(w&0x00ffffffu)|(((w>>24)+(s>>7))<<24); break;\n"
     "      default: w^=(s>>1); break; }\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress33", 2},

    // i64 accumulator (fixed 64-bit rotate + multiply) with narrow 8/16-bit
    // extraction carried across the loop (no 64-bit divide -> no runtime helper).
    {p+"_wmix",
     t+" "+p+"_wmix("+t+" a){\n"
     "  unsigned long long st=((unsigned long long)a<<1)|1ull; unsigned s=(unsigned)a|1u, h=0;\n"
     "  unsigned char acc=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    st=st*6364136223846793005ull+(unsigned long long)s;\n"
     "    unsigned long long r=(st<<13)|(st>>51); st^=r;\n"
     "    acc=(unsigned char)(acc+(unsigned)st+(unsigned)(st>>32));\n"
     "    unsigned short w=(unsigned short)(st>>16);\n"
     "    h=h*131u+(unsigned)st+(unsigned)(st>>32)+acc+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress33", 2},

    // Manual bswap32 + bswap16 endian idioms (clang -> bswap / rev / rev16).
    {p+"_bswapmix",
     t+" "+p+"_bswapmix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned be=((s&0xffu)<<24)|((s&0xff00u)<<8)|((s>>8)&0xff00u)|((s>>24)&0xffu);\n"
     "    unsigned short w=(unsigned short)s;\n"
     "    unsigned short wbe=(unsigned short)((w<<8)|(w>>8));\n"
     "    unsigned mix=be^((unsigned)wbe<<3)^(be>>16);\n"
     "    h=h*131u+be+wbe+mix; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress33", 2},

    // Inner loop builds a byte accumulator; outer loop merges it into the wide
    // parent and uses the parent widely (cross-loop-nest partial merge).
    {p+"_nestbyte",
     t+" "+p+"_nestbyte("+t+" a){\n"
     "  unsigned acc=(unsigned)a, s=(unsigned)a|1u, h=0;\n"
     "  for(int o=0;o<16;o++){ unsigned char b=(unsigned char)acc;\n"
     "    for(int i=0;i<6;i++){ s=s*1103515245u+12345u; b=(unsigned char)(b*3u+(s>>4)); }\n"
     "    acc=(acc&0xffffff00u)|b;\n"
     "    acc=acc*2654435761u+s;\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x18ULL}, "OptStress33", 2},

    // A signed char written conditionally then sign-extended read into i32, with
    // i16 truncation and a signed narrow compare driving more arithmetic.
    {p+"_csext",
     t+" "+p+"_csext("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int acc=0; signed char c=(signed char)a;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    if(s&1u) c=(signed char)(s>>3); else c=(signed char)(c-1);\n"
     "    acc+=(int)c;\n"
     "    short w=(short)acc;\n"
     "    if(w<0) acc+=((int)w>>1);\n"
     "    h=h*131u+(unsigned)acc+(unsigned char)c+(unsigned short)w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress33", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress33TC("x64o33", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress33TC("x86o33", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress33TC("a64o33", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress33TC("armo33", "int");

INSTANTIATE_TEST_SUITE_P(OptStress33, X64OptStress33RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress33, X86OptStress33RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress33, A64OptStress33RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress33, ARM32OptStress33RT, ::testing::ValuesIn(kARM), rtTCName);
