//===- AllPlatform_OptStress141RTTests.cpp - majority / Gray / Dutch-flag =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * major  - Boyer-Moore majority vote over a rodata stream: a single
//              candidate/counter that increments on a match and cancels
//              otherwise, plus a verification pass counting the winner's
//              frequency.  Pins a streaming vote/cancel scan (distinct from the
//              Boyer-Moore-Horspool *string search* in #104 and from any
//              histogram counting array).
//   * gray   - reflected-binary (Gray) code round-trip over rodata operands:
//              encode `g = n ^ (n>>1)` then decode by the prefix-XOR reduction
//              `d ^= d>>k`.  Pins a bitwise XOR-shift transform with a
//              cumulative-XOR inverse (distinct from any additive prefix scan).
//   * dnf    - Dutch-national-flag three-way partition of a rodata-seeded {0,1,2}
//              buffer with low/mid/high pointers and in-place swaps.  Pins a
//              three-pointer partition march (distinct from the pivot-based
//              two-way quickselect partition in #124).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress141RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress141RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress141RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress141RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress141RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress141RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress141RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress141RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress141TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Boyer-Moore majority vote over a rodata stream (single counter + verify).
    {p+"_major",
     "static const unsigned char "+p+"_vals[36]={\n"
     "4,4,7,4,2,4,4,9, 4,1,4,4,3,4,4,4, 6,4,4,8,4,4,5,4,\n"
     "4,0,4,4,7,4,4,2, 4,4,4,1};\n"
     +t+" "+p+"_major("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cand=0u; int count=0;\n"
     "    for(int i=0;i<36;i++){ unsigned v=(unsigned)"+p+"_vals[i]^((s>>(i&7))&1u);\n"
     "      if(count==0){ cand=v; count=1; } else if(v==cand){ count++; } else { count--; }\n"
     "      acc=acc*131u+cand+(unsigned)count; }\n"
     "    unsigned freq=0u;\n"
     "    for(int i=0;i<36;i++){ if(((unsigned)"+p+"_vals[i]^((s>>(i&7))&1u))==cand) freq++; }\n"
     "    acc=acc*131u+freq+(freq*2u>36u?1u:0u); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x39u}, "OptStress141", 2},

    // reflected-binary (Gray) code encode + prefix-XOR decode over rodata.
    {p+"_gray",
     "static const unsigned char "+p+"_v[16]={\n"
     "0x3a,0x91,0x07,0xe5, 0x6c,0xb8,0x4f,0x12, 0xa3,0x5e,0xd0,0x29, 0x7b,0xc6,0x84,0xfd};\n"
     +t+" "+p+"_gray("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, chk=0u;\n"
     "    for(int q=0;q<32;q++){\n"
     "      unsigned n=(((unsigned)"+p+"_v[q&15]<<4)|((s>>(q&7))&15u))&0xFFFu;\n"
     "      unsigned g=n^(n>>1);\n"
     "      unsigned d=g; for(unsigned tt=g>>1; tt; tt>>=1) d^=tt;\n"
     "      if(d==n) chk++; acc=acc*131u+g+(d<<1); }\n"
     "    acc=acc*131u+chk; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Bu}, "OptStress141", 2},

    // Dutch-national-flag three-way partition of a rodata-seeded {0,1,2} buffer.
    {p+"_dnf",
     "static const unsigned char "+p+"_keys[40]={\n"
     "2,0,1,2,1,0,0,2, 1,1,2,0,1,0,2,2, 0,1,0,2,1,2,0,1,\n"
     "2,0,1,1,0,2,2,1, 0,1,2,0,2,1,0,1};\n"
     +t+" "+p+"_dnf("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned buf[40];\n"
     "    for(int i=0;i<40;i++) buf[i]=((unsigned)"+p+"_keys[i]+((s>>(i&7))&1u))%3u;\n"
     "    int lo=0, mid=0, hi=39;\n"
     "    while(mid<=hi){\n"
     "      if(buf[mid]==0u){ unsigned tmp=buf[lo]; buf[lo]=buf[mid]; buf[mid]=tmp; lo++; mid++; }\n"
     "      else if(buf[mid]==2u){ unsigned tmp=buf[hi]; buf[hi]=buf[mid]; buf[mid]=tmp; hi--; }\n"
     "      else mid++;\n"
     "      acc=acc*131u+(unsigned)(lo*1600+mid*40+hi); }\n"
     "    for(int i=0;i<40;i++) acc=acc*131u+buf[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress141", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress141TC("x64o141", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress141TC("x86o141", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress141TC("a64o141", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress141TC("armo141", "int");

INSTANTIATE_TEST_SUITE_P(OptStress141, X64OptStress141RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress141, X86OptStress141RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress141, A64OptStress141RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress141, ARM32OptStress141RT, ::testing::ValuesIn(kARM), rtTCName);
