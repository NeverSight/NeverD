//===- AllPlatform_OptStress39RTTests.cpp - opt-stress probes --*-C++*-=//
//
// Targets the *inverse* of the #430 sub-register gating boundary.  #430 fixed
// the "entry-block narrow (8/16-bit) seed, wide parent read in a later block"
// gap (Phase B2x in LowToMedX86).  Its three gates deliberately back off when
// the partial write is *in a loop*, leaving the loop-carried machinery to merge
// it.  This round probes the dual shape the gates hand off: a narrow write that
// happens *inside* the loop body whose wide parent is read in a *different*
// block (the post-loop tail, the next nest level, or a second loop) and is not
// itself a wide loop-carried phi.  Each kernel keeps the high bits of a word in
// the entry seed, mutates only a sub-field every iteration via the
// `(w & highmask) | (narrow)` idiom clang lowers to a sub-register write, then
// reads the whole word once control leaves the loop, so a dropped high half or
// an unmerged parent surfaces as a return mismatch.
//
//   * postlo16 - loop mutates only the low 16 bits; high 16 from the entry seed
//                must survive; full word read after the loop.
//   * postlo8  - loop mutates only the low byte; upper 24 bits survive; full
//                word read after the loop.
//   * hihalf   - loop mutates only the high 16 bits (offset != 0 sub-field);
//                low 16 from the seed survive; full word read after.
//   * escwide  - loop writes a byte, reads a halfword mid-iteration (wider than
//                the write), then the full word is read after (escalating reads).
//   * twoloops - first loop touches only the low byte; a second loop reads the
//                full word; the parent crosses the loop boundary.
//   * condlo   - the low byte is updated only on some iterations; untouched
//                iterations must preserve the parent; full word read after.
//
// Integer-only, single integer return, bounded, no 64-bit divide; all four
// targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress39RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress39RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress39RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress39RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress39RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress39RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress39RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress39RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress39TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Loop mutates only the low 16 bits via (w & 0xffff0000)|low; the high half
    // is seeded once and must survive the loop, then the full word is read.
    {p+"_postlo16",
     t+" "+p+"_postlo16("+t+" a){\n"
     "  unsigned w=((unsigned)a<<16)|0xbeefu, s=(unsigned)a|1u;\n"
     "  unsigned short lo=(unsigned short)w;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    lo=(unsigned short)(lo*3u+(s>>5)); }\n"
     "  w=(w&0xffff0000u)|lo;\n"
     "  return ("+t+")(unsigned)(w^(w>>16)); }\n",
     {0x4d2ULL}, "OptStress39", 2},

    // Loop mutates only the low byte; the upper 24 bits are seeded once and must
    // survive; the full word is read after.
    {p+"_postlo8",
     t+" "+p+"_postlo8("+t+" a){\n"
     "  unsigned w=((unsigned)a<<8)|0x7fu, s=(unsigned)a|1u;\n"
     "  unsigned char b=(unsigned char)w;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    b=(unsigned char)(b+ (s>>7) + (s>>11)); }\n"
     "  w=(w&0xffffff00u)|b;\n"
     "  return ("+t+")(unsigned)(w*2654435761u); }\n",
     {0x33ULL}, "OptStress39", 2},

    // Loop mutates only the high 16 bits (an offset-16 sub-field); the low 16
    // from the seed survive; the full word is read after.
    {p+"_hihalf",
     t+" "+p+"_hihalf("+t+" a){\n"
     "  unsigned w=((unsigned)a&0xffffu)|0xa5a50000u, s=(unsigned)a|1u;\n"
     "  unsigned short hi=(unsigned short)(w>>16);\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    hi=(unsigned short)(hi^(s>>9)); hi=(unsigned short)(hi+0x101u); }\n"
     "  w=(w&0x0000ffffu)|((unsigned)hi<<16);\n"
     "  return ("+t+")(unsigned)(w+(w>>16)); }\n",
     {0x9a7ULL}, "OptStress39", 2},

    // Loop writes a byte and reads a halfword (wider than the write) within the
    // iteration; the full word is read after the loop (escalating-width reads).
    {p+"_escwide",
     t+" "+p+"_escwide("+t+" a){\n"
     "  unsigned w=(unsigned)a|0x11223300u, s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char b=(unsigned char)(w + (s>>6));\n"
     "    w=(w&0xffffff00u)|b;\n"
     "    unsigned short hw=(unsigned short)w;\n"
     "    h=h*131u+hw; }\n"
     "  return ("+t+")(unsigned)(h+w); }\n",
     {0x58ULL}, "OptStress39", 2},

    // First loop touches only the low byte of w; a second loop reads the full
    // word; the parent value crosses the loop boundary.
    {p+"_twoloops",
     t+" "+p+"_twoloops("+t+" a){\n"
     "  unsigned w=((unsigned)a<<8)|0x40u, s=(unsigned)a|1u;\n"
     "  unsigned char b=(unsigned char)w;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; b=(unsigned char)(b*5u+(s>>8)); }\n"
     "  w=(w&0xffffff00u)|b;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; w=w*2654435761u+s; h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)(h+w); }\n",
     {0x71ULL}, "OptStress39", 2},

    // The low byte is updated only on some iterations; untouched iterations must
    // preserve the parent's low byte; the full word is read after.
    {p+"_condlo",
     t+" "+p+"_condlo("+t+" a){\n"
     "  unsigned w=((unsigned)a<<8)|0x9cu, s=(unsigned)a|1u;\n"
     "  unsigned char b=(unsigned char)w;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    if(s&1u) b=(unsigned char)(b+(s>>5));\n"
     "    else if(s&6u) b=(unsigned char)(b^(s>>3));\n"
     "    w=(w&0xffffff00u)|b;\n"
     "    if(s&8u) w+=0x100u; }\n"
     "  return ("+t+")(unsigned)(w^(w>>11)); }\n",
     {0x2bULL}, "OptStress39", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress39TC("x64o39", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress39TC("x86o39", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress39TC("a64o39", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress39TC("armo39", "int");

INSTANTIATE_TEST_SUITE_P(OptStress39, X64OptStress39RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress39, X86OptStress39RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress39, A64OptStress39RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress39, ARM32OptStress39RT, ::testing::ValuesIn(kARM), rtTCName);
