//===- AllPlatform_OptStress234RTTests.cpp - byte buffer reinterpret =====//
//
// Stack byte buffers built a byte at a time and then re-read at wider widths
// (via fixed-size __builtin_memcpy, which always inlines to a single load) plus
// endian swaps.  This crosses three historically fragile subsystems at once:
// sub-register/sub-word width tracking (#157f/#3), stack-resident buffer
// relative addressing (#158/#229), and load/store width modeling.  Mixing
// aligned and unaligned offsets and constant 64-bit slicing (never a variable
// 64-bit shift, so i386/ARM32 stay libcall-free) keeps the LCG fold honest.
//
//   * mkread32 - fill u8[16] byte-by-byte, read back as u32 at 0/1/4/5/8.
//   * bswapmix - read u32 words, __builtin_bswap32, recombine.
//   * halfword - read/swap u16 halves out of the byte buffer.
//   * wide64   - assemble a u64, slice with constant shifts, fold low^high.
//   * punning  - write u32, read two u16 / four u8 views, recombine.
//   * rotbuf   - rotate a byte window in place and re-read.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress234RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress234RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress234RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress234RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress234RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress234RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress234RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress234RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress234TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fill u8[16] then read back as u32 at aligned + unaligned offsets.
    {p+"_mkread32",
     t+" "+p+"_mkread32("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<140;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[16];\n"
     "    for(int k=0;k<16;k++) b[k]=(unsigned char)(h>>(k&3)*8)+ (unsigned char)k;\n"
     "    unsigned w0,w1,w4,w5,w8; \n"
     "    __builtin_memcpy(&w0,b+0,4); __builtin_memcpy(&w1,b+1,4);\n"
     "    __builtin_memcpy(&w4,b+4,4); __builtin_memcpy(&w5,b+5,4);\n"
     "    __builtin_memcpy(&w8,b+8,4);\n"
     "    unsigned r=w0^(w1*3u)^(w4+w5)^(w8>>1);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress234", 2},

    // Read u32 words and byte-swap them before recombining.
    {p+"_bswapmix",
     t+" "+p+"_bswapmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<140;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[12];\n"
     "    for(int k=0;k<12;k++) b[k]=(unsigned char)(h*(k+1u));\n"
     "    unsigned w0,w4,w8; __builtin_memcpy(&w0,b,4);\n"
     "    __builtin_memcpy(&w4,b+4,4); __builtin_memcpy(&w8,b+8,4);\n"
     "    unsigned r=__builtin_bswap32(w0)+__builtin_bswap32(w4)^__builtin_bswap32(w8);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress234", 2},

    // Read and swap u16 halves out of the byte buffer.
    {p+"_halfword",
     t+" "+p+"_halfword("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<140;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[8]; for(int k=0;k<8;k++) b[k]=(unsigned char)(h>>k);\n"
     "    unsigned short s0,s2,s4,s6; __builtin_memcpy(&s0,b,2);\n"
     "    __builtin_memcpy(&s2,b+2,2); __builtin_memcpy(&s4,b+4,2);\n"
     "    __builtin_memcpy(&s6,b+6,2);\n"
     "    unsigned r=((unsigned)s0<<16|s2)^((unsigned)s6<<16|s4);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress234", 2},

    // Assemble a u64 and slice it with constant shifts only.
    {p+"_wide64",
     t+" "+p+"_wide64("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<140;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[8]; for(int k=0;k<8;k++) b[k]=(unsigned char)(h*7u+(unsigned)k);\n"
     "    unsigned long long v; __builtin_memcpy(&v,b,8);\n"
     "    unsigned lo=(unsigned)v, hi=(unsigned)(v>>32);\n"
     "    unsigned mid=(unsigned)(v>>24);\n"
     "    unsigned r=lo^hi^(mid*2654435761u);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress234", 2},

    // Write a u32, read two u16 / four u8 views of the same storage.
    {p+"_punning",
     t+" "+p+"_punning("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<140;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[4]; unsigned w=h^0xA5A5A5A5u; __builtin_memcpy(b,&w,4);\n"
     "    unsigned short s0,s2; __builtin_memcpy(&s0,b,2); __builtin_memcpy(&s2,b+2,2);\n"
     "    unsigned r=(unsigned)b[0]+(unsigned)b[1]*3u+(unsigned)b[2]*5u+(unsigned)b[3]*7u;\n"
     "    r^=((unsigned)s0)+((unsigned)s2<<3);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress234", 2},

    // Rotate a byte window in place, then re-read at a wider width.
    {p+"_rotbuf",
     t+" "+p+"_rotbuf("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<140;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b[8]; for(int k=0;k<8;k++) b[k]=(unsigned char)(h>>(k*3));\n"
     "    unsigned rot=h&7u; unsigned char tmp[8];\n"
     "    for(int k=0;k<8;k++) tmp[k]=b[(k+rot)&7];\n"
     "    unsigned w0,w4; __builtin_memcpy(&w0,tmp,4); __builtin_memcpy(&w4,tmp+4,4);\n"
     "    unsigned r=w0*16777619u ^ w4;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress234", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress234TC("x64o234", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress234TC("x86o234", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress234TC("a64o234", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress234TC("armo234", "int");

INSTANTIATE_TEST_SUITE_P(OptStress234, X64OptStress234RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress234, X86OptStress234RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress234, A64OptStress234RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress234, ARM32OptStress234RT, ::testing::ValuesIn(kARM), rtTCName);
