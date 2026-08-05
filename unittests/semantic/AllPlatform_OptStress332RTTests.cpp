//===- AllPlatform_OptStress332RTTests.cpp - real-world hash/checksum -----===//
//
// Real-algorithm probes: software CRC32 (inner bit loop + mask-from-sign trick),
// FNV-1a / djb2 / MurmurHash3 finalizer multiply-shift-xor chains, xorshift PRNG,
// and Adler-32 modular checksum.  These mix shift / multiply / xor chains, inner
// loops carrying state across iterations, and the `-(x&1)` mask idiom — the kind
// of multi-component data flow where optimizer constant-folding / strength-
// reduction / sub-register tracking interact.  Each folds to one exact integer
// for a bit-exact original-vs-lifted compare; unsigned arithmetic throughout, no
// libcall on any target (no i64 divide / variable i64 shift).  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress332RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress332RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress332RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress332RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress332RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress332RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress332RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress332RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress332TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  return {
    // Software CRC-32: inner bit loop + the -(crc&1) sign-mask reduction idiom.
    {p+"_crc32b",
     t+" "+p+"_crc32b("+t+" a){\n"
     "  unsigned crc=0xFFFFFFFFu;\n"
     "  for(int i=0;i<128;i++){ unsigned char b=(unsigned char)(((unsigned)a*(unsigned)(i+1))>>3);\n"
     "    crc^=b; for(int j=0;j<8;j++) crc=(crc>>1)^(0xEDB88320u & (unsigned)(-(int)(crc&1u))); }\n"
     "  return ("+t+")(~crc);\n"
     "}\n",
     {0x12345u}, "OptStress332", 2, ""},

    // FNV-1a hash: xor-then-multiply chain (32-bit wraparound multiply).
    {p+"_fnv1a",
     t+" "+p+"_fnv1a("+t+" a){\n"
     "  unsigned h=2166136261u;\n"
     "  for(int i=0;i<128;i++){ unsigned char b=(unsigned char)(((unsigned)a*(unsigned)(i+3))>>2);\n"
     "    h=(h^b)*16777619u; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x23456u}, "OptStress332", 2, ""},

    // djb2 hash: h = h*33 + b (the *33 strength-reduces to shl+add).
    {p+"_djb2",
     t+" "+p+"_djb2("+t+" a){\n"
     "  unsigned h=5381u;\n"
     "  for(int i=0;i<128;i++){ unsigned char b=(unsigned char)(((unsigned)a*(unsigned)(i+5))>>1);\n"
     "    h=((h<<5)+h)+b; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x34567u}, "OptStress332", 2, ""},

    // MurmurHash3 32-bit finalizer: multiply-shift-xor avalanche over a stream.
    {p+"_murmur",
     t+" "+p+"_murmur("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ unsigned h=(unsigned)a*(unsigned)(i+1);\n"
     "    h^=h>>16; h*=0x85ebca6bu; h^=h>>13; h*=0xc2b2ae35u; h^=h>>16; acc=acc*131u+h; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x45678u}, "OptStress332", 2, ""},

    // xorshift32 PRNG: three shift-xor steps carrying state across iterations.
    {p+"_xorsh",
     t+" "+p+"_xorsh("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<160;i++){ s^=s<<13; s^=s>>17; s^=s<<5; acc+=s^(s>>11); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x56789u}, "OptStress332", 2, ""},

    // Adler-32: two running sums with a constant modulus (magic-number divide).
    {p+"_adler",
     t+" "+p+"_adler("+t+" a){\n"
     "  unsigned s1=1u, s2=0u;\n"
     "  for(int i=0;i<128;i++){ unsigned char b=(unsigned char)(((unsigned)a*(unsigned)(i+1))>>2);\n"
     "    s1=(s1+b)%65521u; s2=(s2+s1)%65521u; }\n"
     "  return ("+t+")((s2<<16)|s1);\n"
     "}\n",
     {0x6789Au}, "OptStress332", 2, ""},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress332TC("x64o332", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress332TC("x86o332", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress332TC("a64o332", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress332TC("armo332", "int");

INSTANTIATE_TEST_SUITE_P(OptStress332, X64OptStress332RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress332, X86OptStress332RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress332, A64OptStress332RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress332, ARM32OptStress332RT, ::testing::ValuesIn(kARM), rtTCName);
