//===- AllPlatform_StringAlgoRTTests.cpp - byte/encoding algos --*- C++ -*-===//
//
// clang -O2 byte-level string / encoding algorithm probes (hex, base64, UTF-8,
// rot13, case folding, digit extraction).  These pack byte extraction/insertion
// (sub-register reads), cross-byte bit splicing, and deep nested-ternary range
// maps (cmov/csel chains) into tight loops — stressing the optimizer's
// sub-register aliasing and flag-folding plus the lifter's byte-width handling
// across x86, AArch64 and ARM32.
//
// Each function derives its bytes from the seed argument (no memory operands,
// no libc) and folds to an exact integer return value.  All internal arithmetic
// is unsigned 32-bit with constant-divisor modulo only (magic multiply, no
// runtime divide), so nothing lowers to a helper Unicorn lacks.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StringAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StringAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64StringAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StringAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32StringAlgoRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32StringAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeStrTC(const char *prefix, const char *T,
                                          int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Hex encode: each byte -> two hex chars via a digit/letter branch, folded
    // through an FNV-1a hash.  Exercises nibble extraction and 8-bit branches.
    {p+"_hexenc",
     t+" "+p+"_hexenc("+t+" a) {\n"
     "  unsigned acc=2166136261u;\n"
     "  for(int i=0;i<128;i++){\n"
     "    unsigned b=(unsigned)(a*(i+7)+i)&0xFFu;\n"
     "    unsigned hi=b>>4, lo=b&0xFu;\n"
     "    unsigned c1=hi<10u?48u+hi:87u+hi;\n"
     "    unsigned c2=lo<10u?48u+lo:87u+lo;\n"
     "    acc=(acc^c1)*16777619u; acc=(acc^c2)*16777619u; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "StringAlgo", opt, fl},

    // Hex decode: char -> nibble (digit vs lowercase branch), packed into a
    // rolling accumulator with an xor-shift mix.
    {p+"_hexdec",
     t+" "+p+"_hexdec("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){\n"
     "    unsigned ch=(unsigned)(a*(i+3))%22u;\n"
     "    unsigned hex=ch<10u?48u+ch:97u+(ch-10u)%6u;\n"
     "    unsigned nib=hex>=97u?hex-97u+10u:hex-48u;\n"
     "    acc=(acc<<4)|(nib&0xFu); acc^=acc>>13; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "StringAlgo", opt, fl},

    // Base64 encode: 3 bytes -> 4 six-bit groups -> chars via a 4-way range map
    // (nested ternaries -> cmov/csel).  Cross-byte bit splicing.
    {p+"_b64enc",
     t+" "+p+"_b64enc("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<60;i++){\n"
     "    unsigned b0=(unsigned)(a*(3*i+1))&0xFFu;\n"
     "    unsigned b1=(unsigned)(a*(3*i+2))&0xFFu;\n"
     "    unsigned b2=(unsigned)(a*(3*i+3))&0xFFu;\n"
     "    unsigned g0=b0>>2, g1=((b0&3u)<<4)|(b1>>4);\n"
     "    unsigned g2=((b1&0xFu)<<2)|(b2>>6), g3=b2&0x3Fu;\n"
     "    unsigned e0=g0<26u?65u+g0:g0<52u?71u+g0:g0<62u?g0-4u:g0==62u?43u:47u;\n"
     "    unsigned e1=g1<26u?65u+g1:g1<52u?71u+g1:g1<62u?g1-4u:g1==62u?43u:47u;\n"
     "    unsigned e2=g2<26u?65u+g2:g2<52u?71u+g2:g2<62u?g2-4u:g2==62u?43u:47u;\n"
     "    unsigned e3=g3<26u?65u+g3:g3<52u?71u+g3:g3<62u?g3-4u:g3==62u?43u:47u;\n"
     "    acc=acc*131u+e0; acc=acc*131u+e1; acc=acc*131u+e2; acc=acc*131u+e3; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "StringAlgo", opt, fl},

    // UTF-8 encode: code point -> 1..4 bytes via a range cascade (multi-way
    // branch, variable byte count, cross-byte shifts/masks).
    {p+"_utf8enc",
     t+" "+p+"_utf8enc("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned cp=(unsigned)(a*(i+1)+i*131)&0x1FFFFu;\n"
     "    unsigned y0,y1,y2,y3,n;\n"
     "    if(cp<0x80u){ y0=cp; y1=y2=y3=0; n=1; }\n"
     "    else if(cp<0x800u){ y0=0xC0u|(cp>>6); y1=0x80u|(cp&0x3Fu); y2=y3=0; n=2; }\n"
     "    else if(cp<0x10000u){ y0=0xE0u|(cp>>12); y1=0x80u|((cp>>6)&0x3Fu);\n"
     "      y2=0x80u|(cp&0x3Fu); y3=0; n=3; }\n"
     "    else { y0=0xF0u|(cp>>18); y1=0x80u|((cp>>12)&0x3Fu);\n"
     "      y2=0x80u|((cp>>6)&0x3Fu); y3=0x80u|(cp&0x3Fu); n=4; }\n"
     "    acc=acc*131u+y0+y1*7u+y2*13u+y3*17u+n; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "StringAlgo", opt, fl},

    // ROT13: rotate letters within their case range with modulo wraparound.
    {p+"_rot13",
     t+" "+p+"_rot13("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){\n"
     "    unsigned ch=(unsigned)(a*(i+5))%128u;\n"
     "    unsigned r=ch;\n"
     "    if(ch>=65u&&ch<=90u) r=65u+(ch-65u+13u)%26u;\n"
     "    else if(ch>=97u&&ch<=122u) r=97u+(ch-97u+13u)%26u;\n"
     "    acc=acc*31u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "StringAlgo", opt, fl},

    // Case fold: toggle the 0x20 bit only for letters (predicated xor), mixed
    // through a rotate (clang lowers the letter test to a compound branch).
    {p+"_caseflip",
     t+" "+p+"_caseflip("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){\n"
     "    unsigned ch=(unsigned)(a*(i+9))&0xFFu;\n"
     "    unsigned r=ch;\n"
     "    if((ch>=65u&&ch<=90u)||(ch>=97u&&ch<=122u)) r=ch^0x20u;\n"
     "    acc^=r; acc=(acc<<3)|(acc>>29); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "StringAlgo", opt, fl},

    // Base64 decode: char -> six-bit value via a 5-way range map, accumulated.
    {p+"_b64dec",
     t+" "+p+"_b64dec("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned c=(unsigned)(a*(i+1))%64u;\n"
     "    unsigned ch=c<26u?65u+c:c<52u?71u+c:c<62u?c-4u:c==62u?43u:47u;\n"
     "    unsigned v=(ch>=65u&&ch<=90u)?ch-65u:(ch>=97u&&ch<=122u)?ch-71u:\n"
     "             (ch>=48u&&ch<=57u)?ch+4u:ch==43u?62u:63u;\n"
     "    acc=acc*64u+v; acc^=acc>>11; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "StringAlgo", opt, fl},

    // Decimal digit sum: inner while extracts digits via constant %10 / /10
    // (magic multiply), summed with a positional weight (nested loop).
    {p+"_digitsum",
     t+" "+p+"_digitsum("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=1;i<=200;i++){\n"
     "    unsigned n=(unsigned)(a*i+i), s=0;\n"
     "    while(n){ s+=n%10u; n/=10u; }\n"
     "    acc+=s*(unsigned)i; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "StringAlgo", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Str =
    makeStrTC("x64s", "long", 2, "");
static const std::vector<RoundTripTC> kA64Str =
    makeStrTC("a64s", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Str =
    makeStrTC("arms", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(StringAlgo, X64StringAlgoRT,
                         ::testing::ValuesIn(kX64Str), rtTCName);
INSTANTIATE_TEST_SUITE_P(StringAlgo, A64StringAlgoRT,
                         ::testing::ValuesIn(kA64Str), rtTCName);
INSTANTIATE_TEST_SUITE_P(StringAlgo, ARM32StringAlgoRT,
                         ::testing::ValuesIn(kARM32Str), rtTCName);
