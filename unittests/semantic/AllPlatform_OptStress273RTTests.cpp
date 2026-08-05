//===- AllPlatform_OptStress273RTTests.cpp - memory aliasing at -O0 ======//
//
// Memory aliasing / type punning / byte assembly at -O0 — the dual of the -O2
// OptStress97/98 alias probes.  At -O0 every intermediate is spilled to the
// frame, so a union write then a differently-typed read is a real store-to-load
// across the stack slot, overlapping accesses keep their sub-word stores, and
// fixed-size memcpy lowers to explicit load/store width chains.  This stresses
// the lifter's memory model (sub-word store-to-load forwarding, bitcast through
// memory, struct copy) without the optimizer collapsing it.
//
//   * punf  - float<->int union type punning (read bits, scale, read bits back).
//   * ovl    - overlapping u32 / byte stores into one buffer, wide read across.
//   * bcpy   - __builtin_memcpy of a byte buffer then a misaligned wide read.
//   * swf    - sub-word (u16 / u8) array store then indexed read-back.
//   * agg    - whole-struct copy after per-field RMW of mixed-width members.
//   * rev    - manual byte reverse vs __builtin_bswap32 (must agree).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops + fixed small memcpy, so i386/ARM32 stay
// libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress273RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress273RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress273RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress273RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress273RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress273RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress273RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress273RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress273TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // float<->int union type punning: write bits, read float, scale, read bits.
    {p+"_punf",
     t+" "+p+"_punf("+t+" a){ unsigned h=(unsigned)a; union U{ float f; unsigned u; } u; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    u.u=(h&0x3fffffffu)|0x3f000000u; float f=u.f; u.f=f*2.0f;\n"
     "    acc=acc*131u + (u.u>>7); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress273", 0},

    // overlapping u32 / byte stores into one buffer, wide read across them.
    {p+"_ovl",
     t+" "+p+"_ovl("+t+" a){ unsigned h=(unsigned)a; unsigned char buf[8]; unsigned acc=0;\n"
     "  for(int j=0;j<8;j++) buf[j]=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    __builtin_memcpy(buf, &h, 4); buf[1]=(unsigned char)(h>>24); buf[6]=(unsigned char)i;\n"
     "    unsigned w; __builtin_memcpy(&w, buf+2, 4); acc=acc*131u + w + buf[0]; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress273", 0},

    // __builtin_memcpy of a byte buffer then a misaligned wide read.
    {p+"_bcpy",
     t+" "+p+"_bcpy("+t+" a){ unsigned h=(unsigned)a; unsigned char src[16],dst[16]; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++) src[j]=(unsigned char)(h>>(j&3));\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    for(int j=0;j<16;j++) src[j]^=(unsigned char)(h>>(j&7));\n"
     "    __builtin_memcpy(dst, src, 16); unsigned w; __builtin_memcpy(&w, dst+5, 4);\n"
     "    acc=acc*131u + w; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress273", 0},

    // sub-word (u16 / u8) array store then indexed read-back.
    {p+"_swf",
     t+" "+p+"_swf("+t+" a){ unsigned h=(unsigned)a; unsigned short hw[4]; unsigned char by[8]; unsigned acc=0;\n"
     "  for(int j=0;j<4;j++) hw[j]=0; for(int j=0;j<8;j++) by[j]=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    hw[i&3]=(unsigned short)h; by[i&7]=(unsigned char)(h>>16);\n"
     "    acc=acc*131u + hw[(i>>1)&3] + by[(i>>2)&7]; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress273", 0},

    // whole-struct copy after per-field RMW of mixed-width members.  Fields are
    // initialized one at a time (not an aggregate {0} initializer) so clang does
    // not emit a memset call the bare-metal harness cannot resolve; the q=p copy
    // itself stays inlined.
    {p+"_agg",
     "struct P{ unsigned a; unsigned short b; unsigned char c; int d; };\n"
     +t+" "+p+"_agg("+t+" a){ unsigned h=(unsigned)a; struct P p, q;\n"
     "  p.a=h; p.b=(unsigned short)h; p.c=(unsigned char)h; p.d=(int)h; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    p.a+=h; p.b=(unsigned short)(p.b+h); p.c=(unsigned char)(p.c^h); p.d-=(int)h;\n"
     "    q=p; acc=acc*131u + q.a + q.b + q.c + (unsigned)q.d; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress273", 0},

    // manual byte reverse vs __builtin_bswap32 (must agree -> diff stays 0).
    {p+"_rev",
     t+" "+p+"_rev("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[4]; b[0]=(unsigned char)h; b[1]=(unsigned char)(h>>8);\n"
     "    b[2]=(unsigned char)(h>>16); b[3]=(unsigned char)(h>>24);\n"
     "    unsigned r=((unsigned)b[0]<<24)|((unsigned)b[1]<<16)|((unsigned)b[2]<<8)|b[3];\n"
     "    acc=acc*131u + (r ^ __builtin_bswap32(h)) + b[i&3]; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress273", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress273TC("x64o273", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress273TC("x86o273", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress273TC("a64o273", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress273TC("armo273", "int");

INSTANTIATE_TEST_SUITE_P(OptStress273, X64OptStress273RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress273, X86OptStress273RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress273, A64OptStress273RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress273, ARM32OptStress273RT, ::testing::ValuesIn(kARM), rtTCName);
