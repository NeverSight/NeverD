//===- AllPlatform_WideHash64RTTests.cpp - 64-bit multi-word arith C++ ----===//
//
// Probes that run every kernel through a 64-bit hash accumulator (most existing
// probes use a 32-bit one).  On i386/ARM32 every 64-bit op lowers to a
// multi-word sequence -- ADC/SBB carry chains, SHLD/SHRD funnel shifts, cmp/sbb
// multi-word compares, sign-extend-to-64 -- so the same kernel exercises the
// wide-arithmetic lift + optimizer paths that 32-bit hashes never reach.  Each
// kernel folds its high word back into the low word (h ^= h>>k) so a high-word
// divergence still shows up in the truncated 32-bit return on i386/ARM32.  No
// 64-bit divide/modulo or variable 64-bit shift (those call libgcc on 32-bit).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64WideHash64RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64WideHash64RT, Verify) { roundTripX64(GetParam()); }

class X86WideHash64RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86WideHash64RT, Verify) { roundTripX86(GetParam()); }

class A64WideHash64RT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64WideHash64RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32WideHash64RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32WideHash64RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeWH64TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 64-bit carry/borrow chains: wide add/sub crossing the 2^32 boundary.
    {p+"_carry",
     t+" "+p+"_carry("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a*0x9E3779B97F4A7C15ULL+1;\n"
     "  for(int i=0;i<80;i++){ unsigned long long x=(unsigned long long)(unsigned)(a+i)*0xFFFFFFFB00000007ULL;\n"
     "    h+=x; h^=h>>31; h-=(x>>11); h+=h<<7; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x1234567ULL}, "WideHash64", opt, fl},

    // 64-bit constant rotates (shld/shrd pairs on x86-32, EXTR/funnel on a64).
    {p+"_rot",
     t+" "+p+"_rot("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a+0x123456789ABCDEF0ULL;\n"
     "  for(int i=0;i<90;i++){ unsigned long long x=h^((unsigned long long)i*0x9E3779B97F4A7C15ULL);\n"
     "    unsigned long long r1=(x<<13)|(x>>51), r2=(x<<40)|(x>>24);\n"
     "    h=r1^r2^(h*131ULL); h^=h>>33; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2345678ULL}, "WideHash64", opt, fl},

    // 64-bit signed multi-word compare feeding branchless min/max selects.
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a*2862933555777941757ULL+3037000493ULL;\n"
     "  for(int i=0;i<96;i++){ long long x=(long long)((unsigned long long)a*(i+1))-((long long)i<<32);\n"
     "    long long y=(long long)((unsigned long long)a^((unsigned long long)i<<32))+i;\n"
     "    long long mn=x<y?x:y, mx=x<y?y:x;\n"
     "    h+=(unsigned long long)(mx-mn); h^=h>>29; h*=131ULL; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3456789ULL}, "WideHash64", opt, fl},

    // 64-bit loads/stores to a stack array (stresses wide stack-slot modeling).
    {p+"_memfwd",
     t+" "+p+"_memfwd("+t+" a){\n"
     "  unsigned long long buf[8];\n"
     "  for(int i=0;i<8;i++) buf[i]=(unsigned long long)a*(i+1)+0x1111ULL*i;\n"
     "  unsigned long long h=0;\n"
     "  for(int i=0;i<120;i++){ int j=i&7,k=(i*3)&7;\n"
     "    buf[j]+=buf[k]^((unsigned long long)i<<20);\n"
     "    buf[j]=(buf[j]<<11)|(buf[j]>>53);\n"
     "    h^=buf[(i*5)&7]; h+=h>>23; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x456789AULL}, "WideHash64", opt, fl},

    // Sign/zero extension of 8/16/32-bit values up to 64 bits.
    {p+"_sext64",
     t+" "+p+"_sext64("+t+" a){\n"
     "  unsigned long long h=0;\n"
     "  for(int i=0;i<100;i++){ unsigned v=(unsigned)(a+i*0x9E3779B1u);\n"
     "    long long s=(long long)(int)v+(long long)(signed char)v+(long long)(short)v;\n"
     "    unsigned long long u=(unsigned long long)v+(unsigned long long)(unsigned char)v+(unsigned long long)(unsigned short)v;\n"
     "    h+=(unsigned long long)s^u; h=h*131ULL+(h>>31); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x56789ABULL}, "WideHash64", opt, fl},

    // 64-bit branchless abs via arithmetic sign-mask (sar #63 + xor/sub).
    {p+"_absmask",
     t+" "+p+"_absmask("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a;\n"
     "  for(int i=0;i<100;i++){ long long x=(long long)((unsigned long long)a*(i+3))-((long long)i<<28);\n"
     "    unsigned long long mask=(unsigned long long)(x>>63);\n"
     "    unsigned long long absx=((unsigned long long)x^mask)-mask;\n"
     "    h+=absx&0x00FFFFFFFFFFFFFFULL; h^=(mask&0xAAAAAAAAAAAAAAAAULL);\n"
     "    h+=h<<9; h^=h>>34; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x6789ABCULL}, "WideHash64", opt, fl},

    // 64x64->64 multiply chains interleaved with wide xor/add mixing.
    {p+"_wmul",
     t+" "+p+"_wmul("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a|1ULL;\n"
     "  for(int i=0;i<88;i++){ unsigned long long x=h+(unsigned long long)i*0x100000001ULL;\n"
     "    unsigned long long y=(unsigned long long)a*0xC2B2AE3D27D4EB4FULL+i;\n"
     "    h=(x*y)^((x^y)>>27); h+=h<<5; h^=h>>37; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x789ABCDULL}, "WideHash64", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeWH64TC("x64wh64", "long", 2, "");
static const std::vector<RoundTripTC> kX86 = makeWH64TC("x86wh64", "int", 2, "");
static const std::vector<RoundTripTC> kA64 = makeWH64TC("a64wh64", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeWH64TC("armwh64", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(WideHash64, X64WideHash64RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(WideHash64, X86WideHash64RT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(WideHash64, A64WideHash64RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(WideHash64, ARM32WideHash64RT,
                         ::testing::ValuesIn(kARM), rtTCName);
