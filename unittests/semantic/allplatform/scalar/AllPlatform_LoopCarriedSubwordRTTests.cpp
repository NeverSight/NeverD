//===- AllPlatform_LoopCarriedSubwordRTTests.cpp - subword loop carries -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probes for *loop-carried sub-register values*, the case
// NeverD's own MedIR passes (fixupSubRegisters, buildSsa phi placement,
// mergeLoopCarriedPartialReads, propagate) must model exactly.  clang -O2 keeps
// a byte/halfword accumulator in a sub-register (AL / W-byte / r0:b) across the
// back-edge while wider reads of the same register observe the narrow value
// zero/sign-extended; a phi must merge the narrow carry, and a wide read must
// reconstruct from it.  Each kernel folds the carry into a value-dependent hash
// so any width/aliasing slip diverges the return value.  All four targets,
// native vs lifted, optimizer ON, no libcalls.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64LcSwRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64LcSwRT, Verify) { roundTripX64(GetParam()); }
class X86LcSwRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86LcSwRT, Verify) { roundTripX86(GetParam()); }
class A64LcSwRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64LcSwRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32LcSwRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32LcSwRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeLcSwTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Unsigned-char accumulator carried across the back-edge, read widened to
    // 32 bits every iteration.  The byte phi + zero-extended wide read is the
    // canonical mergeLoopCarriedPartialReads shape.
    {p+"_ucbyte",
     t+" "+p+"_ucbyte("+t+" a){\n"
     "  unsigned char c=(unsigned char)a; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    c=(unsigned char)(c*31u+(unsigned)i+1u);\n"
     "    h += (unsigned)c*131u; h ^= (unsigned)c << (i&7);\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1234ULL}, "LcSw", 2},

    // Signed-char accumulator: the wide reads are sign-extended, so the phi
    // carries a byte and each use is an 8->32 SEXT, not ZEXT.
    {p+"_scbyte",
     t+" "+p+"_scbyte("+t+" a){\n"
     "  signed char c=(signed char)a; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    c=(signed char)(c*7-(int)(i&15)-1);\n"
     "    int w=(int)c;\n"
     "    h += (unsigned)w*131u + (unsigned)(w<0?-w:w);\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x55AAULL}, "LcSw", 2},

    // Unsigned-short accumulator carried, mixed with 32-bit math and a 16-bit
    // truncating store-back so the halfword phi must survive.
    {p+"_ushalf",
     t+" "+p+"_ushalf("+t+" a){\n"
     "  unsigned short s=(unsigned short)a; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    s=(unsigned short)(s*2654435761u + (unsigned)i);\n"
     "    h += (unsigned)s*131u; h = (h<<1)|(h>>31);\n"
     "    s=(unsigned short)(s ^ (h&0xFFFFu));\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9E37ULL}, "LcSw", 2},

    // Two byte accumulators carried at once, each read wide and feeding the
    // other; two narrow phis live across the back-edge simultaneously.
    {p+"_twobyte",
     t+" "+p+"_twobyte("+t+" a){\n"
     "  unsigned char x=(unsigned char)a, y=(unsigned char)(a>>3);\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned char nx=(unsigned char)(x*5u+y+1u);\n"
     "    unsigned char ny=(unsigned char)(y*3u^x);\n"
     "    x=nx; y=ny;\n"
     "    h += (unsigned)x*131u + (unsigned)y*7u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xBEEFULL}, "LcSw", 2},

    // Nested loop: the inner loop carries a byte, the outer a word; the inner
    // byte phi is reborn each outer iteration and its final value feeds the
    // outer accumulator.
    {p+"_nestbyte",
     t+" "+p+"_nestbyte("+t+" a){\n"
     "  unsigned h=(unsigned)a|1u;\n"
     "  for(int i=0;i<24;i++){\n"
     "    unsigned char c=(unsigned char)(h>>(i&7));\n"
     "    for(int j=0;j<11;j++) c=(unsigned char)(c*31u+(unsigned)j+1u);\n"
     "    h = h*1664525u + 1013904223u + (unsigned)c*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xC3C3ULL}, "LcSw", 2},

    // Stack byte array carried across iterations and reread word-wide (union
    // reinterpret): forces correct load widths plus frame-slot aliasing for the
    // narrow stores and the wide reload.
    {p+"_bytearr",
     t+" "+p+"_bytearr("+t+" a){\n"
     "  unsigned char buf[4]; unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  buf[0]=(unsigned char)x; buf[1]=(unsigned char)(x>>8);\n"
     "  buf[2]=(unsigned char)(x>>16); buf[3]=(unsigned char)(x>>24);\n"
     "  for(int i=0;i<160;i++){\n"
     "    buf[i&3]=(unsigned char)(buf[i&3]*31u+(unsigned)i+1u);\n"
     "    unsigned w; __builtin_memcpy(&w,buf,4);\n"
     "    h += w*131u; h ^= (unsigned)buf[(i>>2)&3]<<((i&3)*8);\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x0FF0ULL}, "LcSw", 2},

    // 8-bit rotate carried across the loop: the rotate is built so it stays a
    // byte op, the result reread wide each step.
    {p+"_rotbyte",
     t+" "+p+"_rotbyte("+t+" a){\n"
     "  unsigned char c=(unsigned char)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){\n"
     "    unsigned r=(unsigned)(i&7);\n"
     "    c=(unsigned char)((c<<r)|(c>>((8u-r)&7u)));\n"
     "    c=(unsigned char)(c+ (unsigned char)(h&0xFFu));\n"
     "    h += (unsigned)c*131u + r;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABCDULL}, "LcSw", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeLcSwTC("x64lcsw", "long");
static const std::vector<RoundTripTC> kX86 = makeLcSwTC("x86lcsw", "int");
static const std::vector<RoundTripTC> kA64 = makeLcSwTC("a64lcsw", "long");
static const std::vector<RoundTripTC> kARM = makeLcSwTC("armlcsw", "int");

INSTANTIATE_TEST_SUITE_P(LcSw, X64LcSwRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(LcSw, X86LcSwRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(LcSw, A64LcSwRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(LcSw, ARM32LcSwRT, ::testing::ValuesIn(kARM), rtTCName);
