//===- AllPlatform_OptStress175RTTests.cpp - Josephus / Dutch-flag / Luhn ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * josephus  - Josephus survivor by the pos=(pos+k) mod i recurrence, with
//                 the modulo done as a bounded subtract loop (no division).
//                 Pins a ring-elimination recurrence (distinct from any sort,
//                 census or DP shape).
//   * dutchflag - Dutch national flag three-way partition: lo/mid/hi pointers
//                 sweep the array into <,=,> buckets in one pass.  Pins a
//                 three-pointer in-place partition (distinct from the two-pointer
//                 rain-water #172 and quickselect-style binary partitions).
//   * luhn      - Luhn-style checksum: alternating digit doubling with a 9-fold,
//                 then the mod-10 check digit.  Pins the alternating digit-double
//                 checksum (distinct from CRC #ARM32_Crc32 and the rolling hash
//                 folds).  Forward base+index rodata read keeps the symbol at
//                 offset 0 (avoids the deferred i386/ARM32 interior-pointer #477).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress175RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress175RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress175RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress175RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress175RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress175RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress175RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress175RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress175TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Josephus survivor via pos=(pos+k) mod i, modulo as a bounded subtract loop.
    {p+"_josephus",
     "static const unsigned char "+p+"_jo[8]={9,17,5,13,7,19,3,11};\n"
     +t+" "+p+"_josephus("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=(((unsigned)"+p+"_jo[s&7u]^(s&15u))%20u)+2u; unsigned k=((s>>4)&7u)+1u;\n"
     "    unsigned pos=0u; for(unsigned i=2u;i<=n;i++){ pos=pos+k; while(pos>=i) pos-=i; acc=acc*131u+pos; }\n"
     "    acc=acc*131u+pos+n*100u+k; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x35u}, "OptStress175", 2},

    // Dutch national flag three-way partition (lo/mid/hi single pass).
    {p+"_dutchflag",
     "static const unsigned char "+p+"_df[24]={2,0,1,2,1,0,2,2,1,0,0,2,1,1,2,0,1,2,0,2,1,0,2,1};\n"
     +t+" "+p+"_dutchflag("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int arr[24]; for(int i=0;i<24;i++) arr[i]=(int)(((unsigned)"+p+"_df[i]^((s>>(i&7))&3u))%3u);\n"
     "    int lo=0, mid=0, hi=23;\n"
     "    while(mid<=hi){ if(arr[mid]==0){ int tmp=arr[lo]; arr[lo]=arr[mid]; arr[mid]=tmp; lo++; mid++; }\n"
     "      else if(arr[mid]==2){ int tmp=arr[mid]; arr[mid]=arr[hi]; arr[hi]=tmp; hi--; }\n"
     "      else mid++; }\n"
     "    unsigned hh=0u; for(int i=0;i<24;i++) hh=hh*131u+(unsigned)arr[i]; acc=acc*131u+hh+(unsigned)lo+(unsigned)hi; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x46u}, "OptStress175", 2},

    // Luhn checksum: alternating digit-double with 9-fold, then mod-10 check.
    {p+"_luhn",
     "static const unsigned char "+p+"_lu[16]={4,9,1,7,3,8,2,6,5,0,9,4,7,1,3,8};\n"
     +t+" "+p+"_luhn("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned sum=0u; int parity=0;\n"
     "    for(int i=0;i<16;i++){ unsigned d=((unsigned)"+p+"_lu[i]^((s>>(i&7))&1u))%10u;\n"
     "      if(parity){ d*=2u; if(d>9u) d-=9u; } sum+=d; parity^=1; }\n"
     "    unsigned chk=(10u-(sum%10u))%10u; acc=acc*131u+sum*10u+chk; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x77u}, "OptStress175", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress175TC("x64o175", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress175TC("x86o175", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress175TC("a64o175", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress175TC("armo175", "int");

INSTANTIATE_TEST_SUITE_P(OptStress175, X64OptStress175RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress175, X86OptStress175RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress175, A64OptStress175RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress175, ARM32OptStress175RT, ::testing::ValuesIn(kARM), rtTCName);
