//===- AllPlatform_OptStress293RTTests.cpp - parity/byte-parity probe ====//
//
// -O2 integer kernels stressing parity, byte-reversal and nibble/byte
// manipulation codegen paths:
//
//   * parity8   - per-byte odd-parity fold (XOR reduction per byte).
//   * parity32  - 32-bit word parity via SWAR.
//   * bswapmix  - byte-swap then mix with shifts.
//   * nibbleinv - nibble invert (XOR 0xF) + inter-nibble add.
//   * bytezip   - even/odd byte zip (deinterleave).
//   * graycode  - binary-to-Gray and back (XOR shift chain).
//
// All operations are UB-free in the unsigned domain, and there is no division
// -- so native and lifted builds agree bit-for-bit.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress293RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress293RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress293RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress293RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress293RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress293RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress293RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress293RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress293TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // per-byte odd-parity fold (XOR reduction per byte).
    {p+"_parity8",
     t+" "+p+"_parity8("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b=h&0xFFu; b^=b>>4; b^=b>>2; b^=b>>1; b&=1u;\n"
     "    acc=acc*131u+b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress293", 2},

    // 32-bit word parity via SWAR.
    {p+"_parity32",
     t+" "+p+"_parity32("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    x^=x>>16; x^=x>>8; x^=x>>4; x^=x>>2; x^=x>>1; x&=1u;\n"
     "    acc=acc*131u+x+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress293", 2},

    // byte-swap then mix with shifts.
    {p+"_bswapmix",
     t+" "+p+"_bswapmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=__builtin_bswap32(h); r=(r<<3)|(r>>29); acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress293", 2},

    // nibble invert (XOR 0xF) + inter-nibble add.
    {p+"_nibbleinv",
     t+" "+p+"_nibbleinv("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u; unsigned x=h;\n"
     "    unsigned lo=(x&0xFu)^0xFu, hi=((x>>4)&0xFu)^0xFu;\n"
     "    acc=acc*131u+lo+hi+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress293", 2},

    // even/odd byte zip (deinterleave).
    {p+"_bytezip",
     t+" "+p+"_bytezip("+t+" a){ unsigned char buf[32]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u; buf[i]=(unsigned char)(h>>5); }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<16;i++){ acc=acc*131u+buf[i*2]+((unsigned)buf[i*2+1]<<8)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress293", 2},

    // binary-to-Gray and back (XOR shift chain).
    {p+"_graycode",
     t+" "+p+"_graycode("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned g=h^(h>>1); unsigned b=g^(g>>1)^(g>>2)^(g>>3);\n"
     "    acc=acc*131u+g+b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress293", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress293TC("x64o293", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress293TC("x86o293", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress293TC("a64o293", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress293TC("armo293", "int");

INSTANTIATE_TEST_SUITE_P(OptStress293, X64OptStress293RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress293, X86OptStress293RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress293, A64OptStress293RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress293, ARM32OptStress293RT, ::testing::ValuesIn(kARM), rtTCName);
