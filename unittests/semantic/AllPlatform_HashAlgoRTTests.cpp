//===- AllPlatform_HashAlgoRTTests.cpp - hash / mixing algos ----*- C++ -*-===//
//
// clang -O2 hash/mixing algorithm probes.  Hash finalizers and rolling hashes
// pack a lot of multiply / rotate / xor / shift mixing into tight loops, which
// stresses the optimizer's constant-multiply lowering, funnel/variable-rotate
// handling, loop-carried state, and sub-register aliasing — across x86, AArch64
// and ARM32, each of which lowers `* const` and `rotl` quite differently.
//
// Every function loops over inputs so all paths run and folds to an exact
// integer return value.  All internal arithmetic is unsigned 32-bit (no signed
// overflow UB, identical low bits regardless of host word size, and no 64-bit
// divide/multiply that would lower to a runtime library call Unicorn lacks).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64HashAlgoRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64HashAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64HashAlgoRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64HashAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32HashAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32HashAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeHashTC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // MurmurHash3 32-bit finalizer (fmix32): two constant multiplies sandwiched
    // between xor-shift mixes.  Exercises `* 0x85ebca6b` / `* 0xc2b2ae35`.
    {p+"_fmix32",
     t+" "+p+"_fmix32("+t+" a) {\n"
     "  unsigned s=0;\n"
     "  for (int i=0;i<80;i++){\n"
     "    unsigned h=(unsigned)(a+i);\n"
     "    h^=h>>16; h*=0x85ebca6bu; h^=h>>13; h*=0xc2b2ae35u; h^=h>>16;\n"
     "    s += h ^ (unsigned)i; }\n"
     "  return ("+t+")s;\n"
     "}\n",
     {0x1234567ULL}, "HashAlgo", opt, fl},

    // MurmurHash3 body scramble: k *= c1; k = rotl(k,15); k *= c2; then mixed
    // into a loop-carried hash with rotl(h,13)*5+const.
    {p+"_murmur",
     t+" "+p+"_murmur("+t+" a) {\n"
     "  unsigned h=(unsigned)a|1u;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned k=(unsigned)(a*i+i);\n"
     "    k*=0xcc9e2d51u; k=(k<<15)|(k>>17); k*=0x1b873593u;\n"
     "    h^=k; h=(h<<13)|(h>>19); h=h*5u+0xe6546b64u; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2233445ULL}, "HashAlgo", opt, fl},

    // Jenkins one-at-a-time: add, shift-add mixing, loop-carried.
    {p+"_jenkins",
     t+" "+p+"_jenkins("+t+" a) {\n"
     "  unsigned h=0;\n"
     "  for (int i=0;i<128;i++){\n"
     "    h += (unsigned)((a>>(i&7))+i)&0xFFu;\n"
     "    h += h<<10; h ^= h>>6; }\n"
     "  h += h<<3; h ^= h>>11; h += h<<15;\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "HashAlgo", opt, fl},

    // djb2: h = h*33 + c  (clang lowers *33 to shift+add / madd).
    {p+"_djb2",
     t+" "+p+"_djb2("+t+" a) {\n"
     "  unsigned h=5381u;\n"
     "  for (int i=0;i<120;i++){\n"
     "    unsigned c=(unsigned)(a*3+i)&0xFFu;\n"
     "    h=((h<<5)+h)+c; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x4455667ULL}, "HashAlgo", opt, fl},

    // sdbm: h = c + (h<<6) + (h<<16) - h.
    {p+"_sdbm",
     t+" "+p+"_sdbm("+t+" a) {\n"
     "  unsigned h=0;\n"
     "  for (int i=0;i<120;i++){\n"
     "    unsigned c=(unsigned)(a^(i*7))&0xFFu;\n"
     "    h=c+(h<<6)+(h<<16)-h; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x5566778ULL}, "HashAlgo", opt, fl},

    // Thomas Wang 32-bit integer hash: shift/add/xor/multiply cascade.
    {p+"_wang",
     t+" "+p+"_wang("+t+" a) {\n"
     "  unsigned s=0;\n"
     "  for (int i=0;i<80;i++){\n"
     "    unsigned k=(unsigned)(a+i*2654435761u);\n"
     "    k=~k+(k<<15); k^=k>>12; k+=k<<2; k^=k>>4;\n"
     "    k*=2057u; k^=k>>16;\n"
     "    s += k - (unsigned)i; }\n"
     "  return ("+t+")s;\n"
     "}\n",
     {0x6677889ULL}, "HashAlgo", opt, fl},

    // FNV-1a with a rotate stirred in (forces a variable-independent rotl).
    {p+"_fnv1a",
     t+" "+p+"_fnv1a("+t+" a) {\n"
     "  unsigned h=2166136261u;\n"
     "  for (int i=0;i<120;i++){\n"
     "    unsigned c=(unsigned)(a+i*131)&0xFFu;\n"
     "    h^=c; h*=16777619u; h=(h<<7)|(h>>25); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "HashAlgo", opt, fl},

    // xxHash32-style accumulator round: acc += in*PRIME2; acc = rotl(acc,13);
    // acc *= PRIME1.  Four lanes folded together (loop-carried state vector).
    {p+"_xxh32",
     t+" "+p+"_xxh32("+t+" a) {\n"
     "  unsigned v1=2654435761u+0x61C88647u, v2=2246822519u;\n"
     "  unsigned v3=0u, v4=0u-2654435761u;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned in=(unsigned)(a*i+i);\n"
     "    v1+=in*2246822519u; v1=(v1<<13)|(v1>>19); v1*=2654435761u;\n"
     "    v2+=in*2246822519u; v2=(v2<<13)|(v2>>19); v2*=2654435761u;\n"
     "    v3+=in*2246822519u; v3=(v3<<13)|(v3>>19); v3*=2654435761u;\n"
     "    v4+=in*2246822519u; v4=(v4<<13)|(v4>>19); v4*=2654435761u; }\n"
     "  unsigned h=((v1<<1)|(v1>>31))+((v2<<7)|(v2>>25))+\n"
     "            ((v3<<12)|(v3>>20))+((v4<<18)|(v4>>14));\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x88990ABULL}, "HashAlgo", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Hash =
    makeHashTC("x64h", "long", 2, "");
static const std::vector<RoundTripTC> kA64Hash =
    makeHashTC("a64h", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Hash =
    makeHashTC("armh", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(HashAlgo, X64HashAlgoRT,
                         ::testing::ValuesIn(kX64Hash), rtTCName);
INSTANTIATE_TEST_SUITE_P(HashAlgo, A64HashAlgoRT,
                         ::testing::ValuesIn(kA64Hash), rtTCName);
INSTANTIATE_TEST_SUITE_P(HashAlgo, ARM32HashAlgoRT,
                         ::testing::ValuesIn(kARM32Hash), rtTCName);
