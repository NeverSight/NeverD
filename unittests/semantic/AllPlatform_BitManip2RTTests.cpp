//===- AllPlatform_BitManip2RTTests.cpp - bit-twiddling lowerings ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing of bit-manipulation idioms clang -O2 lowers to
// native count/scan/reverse/byte-swap instructions (POPCNT/LZCNT/TZCNT, BSR/BSF,
// BSWAP/MOVBE, RBIT/REV, CLZ/CTZ, BT-family) and the surrounding shift/mask
// stitching.  Combining several per kernel stresses the lift's sub-register and
// width handling in shapes the existing per-op bit tests do not reach.  All four
// targets, compared native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BM2RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BM2RT, Verify) { roundTripX64(GetParam()); }
class X86BM2RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86BM2RT, Verify) { roundTripX86(GetParam()); }
class A64BM2RT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BM2RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32BM2RT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BM2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeBM2TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // popcount / clz / ctz mixed into a hash — count instructions + the +1/-1
    // edge handling around zero inputs.
    {p+"_count",
     t+" "+p+"_count("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<48;i++){ unsigned x=acc*2654435761u+(unsigned)i;\n"
     "    unsigned pc=(unsigned)__builtin_popcount(x);\n"
     "    unsigned lz=x?(unsigned)__builtin_clz(x):32u;\n"
     "    unsigned tz=x?(unsigned)__builtin_ctz(x):32u;\n"
     "    acc=acc*31u+pc*7u+lz*131u+tz; acc^=(acc<<5); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x33ULL}, "BM2", 2},

    // byte swap (16/32) + nibble/byte reversal — BSWAP/MOVBE/REV.
    {p+"_swap",
     t+" "+p+"_swap("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ unsigned x=acc+(unsigned)i*0x01010101u;\n"
     "    unsigned b32=__builtin_bswap32(x);\n"
     "    unsigned short hi=(unsigned short)(x>>16);\n"
     "    unsigned b16=(unsigned)__builtin_bswap16(hi);\n"
     "    acc=acc*131u+b32+b16*7u; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x44ULL}, "BM2", 2},

    // single-bit test / set / clear / toggle driven by a moving index — the
    // BT/BTS/BTR/BTC family plus shift-by-variable masking.
    {p+"_bittest",
     t+" "+p+"_bittest("+t+" a){\n"
     "  unsigned acc=(unsigned)a, flags=0;\n"
     "  for(int i=0;i<64;i++){ unsigned bit=(acc>>(i&15))&31u;\n"
     "    if((acc>>bit)&1u) flags+=bit; else acc|=(1u<<bit);\n"
     "    acc^=(1u<<((bit+7u)&31u)); acc&=~(1u<<((bit+13u)&31u));\n"
     "    acc=(acc<<1)|(acc>>31); acc+=flags; }\n"
     "  return ("+t+")(unsigned long)(acc^flags); }\n",
     {0x5ULL}, "BM2", 2},

    // bit reversal built from shift/mask stages (clang may fold to RBIT on
    // AArch64) interleaved with rotates.
    {p+"_reverse",
     t+" "+p+"_reverse("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ unsigned x=acc+(unsigned)i;\n"
     "    x=((x&0x55555555u)<<1)|((x>>1)&0x55555555u);\n"
     "    x=((x&0x33333333u)<<2)|((x>>2)&0x33333333u);\n"
     "    x=((x&0x0F0F0F0Fu)<<4)|((x>>4)&0x0F0F0F0Fu);\n"
     "    x=((x&0x00FF00FFu)<<8)|((x>>8)&0x00FF00FFu);\n"
     "    x=(x<<16)|(x>>16);\n"
     "    acc=acc*31u+x; acc=(acc>>3)|(acc<<29); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x6ULL}, "BM2", 2},

    // isolate-lowest-set / clear-lowest-set / mask-up-to-lowest (BMI BLSI/BLSR/
    // BLSMSK idioms) — clang emits these or the equivalent and/neg sequences.
    {p+"_lowbit",
     t+" "+p+"_lowbit("+t+" a){\n"
     "  unsigned acc=(unsigned)a|0x80000001u;\n"
     "  for(int i=0;i<50;i++){ unsigned x=acc^(unsigned)(i*0x9E3779B1u);\n"
     "    unsigned blsi=x&(0u-x); unsigned blsr=x&(x-1u); unsigned blsmsk=x^(x-1u);\n"
     "    acc=acc*131u+blsi+blsr*7u+blsmsk; acc^=(acc<<7)|(acc>>25); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x7ULL}, "BM2", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeBM2TC("x64bm2", "long");
static const std::vector<RoundTripTC> kX86 = makeBM2TC("x86bm2", "int");
static const std::vector<RoundTripTC> kA64 = makeBM2TC("a64bm2", "long");
static const std::vector<RoundTripTC> kARM = makeBM2TC("armbm2", "int");

INSTANTIATE_TEST_SUITE_P(BM2, X64BM2RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(BM2, X86BM2RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(BM2, A64BM2RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(BM2, ARM32BM2RT, ::testing::ValuesIn(kARM), rtTCName);
