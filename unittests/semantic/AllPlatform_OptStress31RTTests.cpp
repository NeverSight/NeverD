//===- AllPlatform_OptStress31RTTests.cpp - realistic codec kernels -*-C++*-=//
//
// Gnarly but realistic codecs that combine nested loops, switch/branch dispatch,
// mixed-width (8/16/32-bit) state, byte-stream inspection and bit packing in one
// function — the shape that historically surfaced sub-register / flag / SSA
// miscompiles (ComplexAlgo #157c-g).  Each is integer-only, table-free (computed
// rather than rodata lookup), bounded, and folds to a single integer return, so
// it runs native vs lifted on all four targets at -O2.
//
//   * utf8len  - UTF-8 code-point count with continuation-byte validation.
//   * b64pair  - two base64-style nested-ternary byte encoders summed each iter.
//   * base64   - full 4-group base64 (i386 clang software-pipeline dual-byte
//                packing into BL/BH — regression guard for #442.1).
//   * base64e0 - byte array + 24-bit word + single base64 ternary.
//   * bytepair - two byte states carried one iteration late (delay line).
//   * crc16    - table-free CRC-16/CCITT bit loop (16-bit shift/xor).
//   * lzmatch  - greedy longest-match length finder over a small window.
//
// (RLE/stack-VM kernels were dropped: clang emits a `memset` libcall / a rodata
// jump table the bare-metal Unicorn harness cannot resolve on the ORIGINAL side,
// so they exercise a harness limit rather than the NeverD lift.)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress31RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress31RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress31RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress31RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress31RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress31RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress31RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress31RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress31TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // UTF-8 code-point counter with continuation-byte validation.
    {p+"_utf8len",
     t+" "+p+"_utf8len("+t+" a){\n"
     "  unsigned char buf[64]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; buf[i]=(unsigned char)(s>>11); }\n"
     "  unsigned cps=0, bad=0, h=0; int i=0;\n"
     "  while(i<64){ unsigned char c=buf[i]; int n;\n"
     "    if(c<0x80) n=1; else if((c&0xe0)==0xc0) n=2;\n"
     "    else if((c&0xf0)==0xe0) n=3; else if((c&0xf8)==0xf0) n=4; else { bad++; i++; continue; }\n"
     "    int ok=1; for(int j=1;j<n;j++){ if(i+j>=64 || (buf[i+j]&0xc0)!=0x80){ ok=0; break; } }\n"
     "    if(ok){ cps++; i+=n; } else { bad++; i++; }\n"
     "    h=h*131u+cps*7u+bad; }\n"
     "  return ("+t+")(unsigned)(h+cps*1000u+bad); }\n",
     {0x41ULL}, "OptStress31", 2},

    // Two base64-style nested-ternary byte encoders packed/summed each iteration
    // (isolates the sub-register packing of two computed byte values; the full
    // base64 below adds 3->4 bit packing on top).
    {p+"_b64pair",
     t+" "+p+"_b64pair("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char g0=(unsigned char)((s>>5)&0x3f), g3=(unsigned char)((s>>11)&0x3f);\n"
     "    unsigned char e0=(unsigned char)(g0<26?g0+65:g0<52?g0+71:g0<62?g0-4:g0==62?43:47);\n"
     "    unsigned char e3=(unsigned char)(g3<26?g3+65:g3<52?g3+71:g3<62?g3-4:g3==62?43:47);\n"
     "    h=h*131u+e0+e3*251u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress31", 2},

    // Reduced base64 (single e0 ternary, no e3/g1/g2) — bisect guard isolating
    // the dual-byte register-packing defect the full kernel above exercises.
    {p+"_base64e0",
     t+" "+p+"_base64e0("+t+" a){\n"
     "  unsigned char in[45]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<45;i++){ s=s*1103515245u+12345u; in[i]=(unsigned char)(s>>7); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i+2<45;i+=3){\n"
     "    unsigned w=((unsigned)in[i]<<16)|((unsigned)in[i+1]<<8)|in[i+2];\n"
     "    unsigned char g0=(unsigned char)((w>>18)&0x3f);\n"
     "    unsigned char e0=(unsigned char)(g0<26?g0+65:g0<52?g0+71:g0<62?g0-4:g0==62?43:47);\n"
     "    h=h*131u+e0; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress31", 2},

    // Full 4-group base64 (deferred i386 bug #442.1 reproducer).
    {p+"_base64",
     t+" "+p+"_base64("+t+" a){\n"
     "  unsigned char in[45]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<45;i++){ s=s*1103515245u+12345u; in[i]=(unsigned char)(s>>7); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i+2<45;i+=3){\n"
     "    unsigned w=((unsigned)in[i]<<16)|((unsigned)in[i+1]<<8)|in[i+2];\n"
     "    unsigned char g0=(unsigned char)((w>>18)&0x3f), g1=(unsigned char)((w>>12)&0x3f);\n"
     "    unsigned char g2=(unsigned char)((w>>6)&0x3f), g3=(unsigned char)(w&0x3f);\n"
     "    unsigned char e0=(unsigned char)(g0<26?g0+65:g0<52?g0+71:g0<62?g0-4:g0==62?43:47);\n"
     "    unsigned char e3=(unsigned char)(g3<26?g3+65:g3<52?g3+71:g3<62?g3-4:g3==62?43:47);\n"
     "    h=h*131u+e0+g1*3u+g2*7u+e3; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress31", 2},

    // Two byte-wide states carried across iterations and consumed one iteration
    // late (delay-line) — attempts to force clang's dual byte-lane register
    // packing (BL/BH of one GPR) that the full base64 triggers via software
    // pipelining, as a minimal reproducer for the deferred i386 bug.
    {p+"_bytepair",
     t+" "+p+"_bytepair("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; unsigned char p0=0,p1=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned char g0=(unsigned char)((s>>5)&0x3f), g3=(unsigned char)((s>>11)&0x3f);\n"
     "    unsigned char e0=(unsigned char)(g0<26?g0+65:g0<52?g0+71:g0<62?g0-4:g0==62?43:47);\n"
     "    unsigned char e3=(unsigned char)(g3<26?g3+65:g3<52?g3+71:g3<62?g3-4:g3==62?43:47);\n"
     "    h=h*131u+p0+p1*251u; p0=e0; p1=e3; }\n"
     "  return ("+t+")(unsigned)(h+p0+p1); }\n",
     {0x77ULL}, "OptStress31", 2},

    // Table-free CRC-16/CCITT bit loop (16-bit shift/xor accumulator).
    {p+"_crc16",
     t+" "+p+"_crc16("+t+" a){\n"
     "  unsigned char buf[40]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; buf[i]=(unsigned char)(s>>17); }\n"
     "  unsigned short crc=0xffff; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ crc=(unsigned short)(crc ^ ((unsigned short)buf[i]<<8));\n"
     "    for(int b=0;b<8;b++){ if(crc&0x8000u) crc=(unsigned short)((crc<<1)^0x1021u);\n"
     "      else crc=(unsigned short)(crc<<1); }\n"
     "    h=h*131u+crc; }\n"
     "  return ("+t+")(unsigned)(h+crc); }\n",
     {0x18ULL}, "OptStress31", 2},

    // Greedy longest-match length finder over a small sliding window.
    {p+"_lzmatch",
     t+" "+p+"_lzmatch("+t+" a){\n"
     "  unsigned char buf[64]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; buf[i]=(unsigned char)((s>>15)&7u); }\n"
     "  unsigned h=0;\n"
     "  for(int i=8;i<64;i++){ int bestLen=0, bestOff=0;\n"
     "    for(int off=1;off<=8 && off<=i;off++){ int len=0;\n"
     "      while(i+len<64 && len<18 && buf[i+len]==buf[i-off+ (len%off)]) len++;\n"
     "      if(len>bestLen){ bestLen=len; bestOff=off; } }\n"
     "    h=h*131u+(unsigned)bestLen*7u+(unsigned)bestOff; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress31", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress31TC("x64o31", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress31TC("x86o31", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress31TC("a64o31", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress31TC("armo31", "int");

INSTANTIATE_TEST_SUITE_P(OptStress31, X64OptStress31RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress31, X86OptStress31RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress31, A64OptStress31RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress31, ARM32OptStress31RT, ::testing::ValuesIn(kARM), rtTCName);
