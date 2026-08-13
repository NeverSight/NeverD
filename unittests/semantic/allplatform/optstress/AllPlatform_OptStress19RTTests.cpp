//===- AllPlatform_OptStress19RTTests.cpp - dense int algorithms -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes built from dense, real integer algorithms
// not covered by OptStress1-18 / OptStress17's algorithm set.  Each kernel mixes
// transforms that NeverD's own MedIR passes (flag reconstruction, sub-register
// SSA, copy/constant propagation, DCE) must keep semantically exact:
//
//   * xtea     - a few XTEA block-cipher rounds (32-bit wraparound add/shift/xor
//                with a delta accumulator and key[sum&3] table indexing).
//   * mttemper - Mersenne-Twister tempering over an LCG-seeded stream (the
//                shift/and/xor cascade y^=y>>11; y^=(y<<7)&M; ...).
//   * adler32  - the Adler-32 rolling checksum: two accumulators reduced mod
//                65521 (div/mod by a constant prime -> magic-multiply mulhi).
//   * dec10    - repeated div/mod by 10 to walk a number's decimal digits, then
//                a digit-weighted fold (constant div + remainder reuse).
//   * graypop  - binary<->Gray round-trip plus a SWAR population count and a
//                parity fold (no __builtin_popcount, so no library helper).
//   * cordic   - fixed-point CORDIC vector-rotation magnitude (shift/add with a
//                runtime-signed direction decision per iteration).
//
// Everything is unsigned/signed 32-bit so no 64-bit divide or float helper is
// emitted on the 32-bit targets; each folds to one integer return.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress19RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress19RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress19RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress19RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress19RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress19RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress19RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress19RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress19TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A few XTEA rounds: 32-bit wraparound add/shift/xor + delta accumulator.
    {p+"_xtea",
     t+" "+p+"_xtea("+t+" a){\n"
     "  unsigned v0=(unsigned)a, v1=(unsigned)a^0x9e3779b9u, sum=0;\n"
     "  unsigned key[4]={0x12345678u,0x9abcdef0u,0x0f1e2d3cu,0xdeadbeefu};\n"
     "  const unsigned delta=0x9e3779b9u;\n"
     "  for(int i=0;i<12;i++){\n"
     "    v0 += (((v1<<4)^(v1>>5))+v1) ^ (sum+key[sum&3]);\n"
     "    sum += delta;\n"
     "    v1 += (((v0<<4)^(v0>>5))+v0) ^ (sum+key[(sum>>11)&3]); }\n"
     "  return ("+t+")(v0^v1); }\n",
     {0x1234567ULL}, "OptStress19", 2},

    // Mersenne-Twister tempering over an LCG stream.
    {p+"_mttemper",
     t+" "+p+"_mttemper("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<24;i++){\n"
     "    s=s*1664525u+1013904223u; unsigned y=s;\n"
     "    y ^= y>>11;\n"
     "    y ^= (y<<7)&0x9d2c5680u;\n"
     "    y ^= (y<<15)&0xefc60000u;\n"
     "    y ^= y>>18;\n"
     "    h = h*16777619u ^ y; }\n"
     "  return ("+t+")h; }\n",
     {0xabcdULL}, "OptStress19", 2},

    // Adler-32: two accumulators reduced mod 65521 (constant-prime div/mod).
    {p+"_adler32",
     t+" "+p+"_adler32("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, A=1, B=0;\n"
     "  for(int i=0;i<48;i++){\n"
     "    s=s*1103515245u+12345u; unsigned byte=(s>>16)&0xffu;\n"
     "    A=(A+byte)%65521u; B=(B+A)%65521u; }\n"
     "  return ("+t+")((B<<16)|A); }\n",
     {0x55ULL}, "OptStress19", 2},

    // Decimal-digit walk: div/mod by 10 with weighted digit fold.
    {p+"_dec10",
     t+" "+p+"_dec10("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int k=0;k<20;k++){\n"
     "    x=x*2654435761u+1u; unsigned n=x, w=1, acc=0;\n"
     "    while(n){ unsigned d=n%10u; n/=10u; acc+=d*w; w+=3; }\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")h; }\n",
     {0x9bULL}, "OptStress19", 2},

    // Binary<->Gray round-trip + SWAR popcount + parity fold.
    {p+"_graypop",
     t+" "+p+"_graypop("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<24;i++){\n"
     "    s=s*1103515245u+12345u; unsigned g=s^(s>>1);\n"
     "    unsigned b=g; b^=b>>16; b^=b>>8; b^=b>>4; b^=b>>2; b^=b>>1;\n"
     "    unsigned c=g - ((g>>1)&0x55555555u);\n"
     "    c=(c&0x33333333u)+((c>>2)&0x33333333u);\n"
     "    c=(c+(c>>4))&0x0f0f0f0fu; c=(c*0x01010101u)>>24;\n"
     "    h=h*131u+(c<<1)+(b&1u); }\n"
     "  return ("+t+")h; }\n",
     {0x6dULL}, "OptStress19", 2},

    // Fixed-point CORDIC magnitude: per-iteration signed direction decision.
    {p+"_cordic",
     t+" "+p+"_cordic("+t+" a){\n"
     "  int x=0x9b74+((int)a&0x3ff), y=((int)a>>3)&0x7ff, h=0;\n"
     "  for(int rot=0;rot<14;rot++){\n"
     "    for(int i=0;i<12;i++){\n"
     "      int nx,ny;\n"
     "      if(y<0){ nx=x-(y>>i); ny=y+(x>>i); }\n"
     "      else   { nx=x+(y>>i); ny=y-(x>>i); }\n"
     "      x=nx; y=ny; }\n"
     "    h=h*131+(x&0xffff)-(y&0xff); y=(y^0x55)+rot; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress19", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress19TC("x64o19", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress19TC("x86o19", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress19TC("a64o19", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress19TC("armo19", "int");

INSTANTIATE_TEST_SUITE_P(OptStress19, X64OptStress19RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress19, X86OptStress19RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress19, A64OptStress19RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress19, ARM32OptStress19RT, ::testing::ValuesIn(kARM), rtTCName);
