//===- AllPlatform_OptStress170RTTests.cpp - RMQ sparse / Bell tri / ugly num =//
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
//   * rmqsparse - range-minimum sparse table: precompute mins over every
//                 power-of-two interval by doubling, then answer each query with
//                 two overlapping covered intervals.  Pins an immutable
//                 doubling-interval RMQ (distinct from the mutable Fenwick #129
//                 and segment tree #168, and from the sentinel sparse storage in
//                 #54).
//   * belltri   - Bell triangle: each row begins with the previous row's last
//                 entry and accumulates leftward, the row sums counting set
//                 partitions.  Pins a Bell-number triangle recurrence (distinct
//                 from Pascal's triangle in #130 and the Catalan fold in #135).
//   * ugly      - 5-smooth (Hamming / "ugly") numbers via a three-pointer merge
//                 of the 2x/3x/5x streams.  Pins an ordered-multiples merge
//                 (distinct from the prime sieves in #120/#130).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress170RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress170RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress170RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress170RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress170RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress170RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress170RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress170RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress170TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // range-minimum sparse table (doubling intervals) with overlapping queries.
    {p+"_rmqsparse",
     "static const unsigned char "+p+"_rq[16]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31};\n"
     +t+" "+p+"_rmqsparse("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned sp[5][16];\n"
     "    for(int i=0;i<16;i++) sp[0][i]=((unsigned)"+p+"_rq[i]^((s>>(i&7))&3u))&63u;\n"
     "    for(int k=1;k<5;k++){ int len=1<<k; for(int i=0;i+len<=16;i++){ unsigned x=sp[k-1][i], y=sp[k-1][i+(len>>1)]; sp[k][i]=x<y?x:y; } }\n"
     "    for(int q=0;q<8;q++){ unsigned l=(s>>(q&7))&7u, w=((s>>q)&7u)+1u, r=l+w-1u; if(r>15u) r=15u;\n"
     "      unsigned len=r-l+1u; int k=0; while((1u<<(k+1))<=len) k++;\n"
     "      unsigned x=sp[k][l], y=sp[k][r-(1u<<k)+1u]; unsigned mn=x<y?x:y;\n"
     "      acc=acc*131u+mn+(unsigned)k+len; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Eu}, "OptStress170", 2},

    // Bell triangle (single-array in-place leftward accumulation, mod 1009).
    {p+"_belltri",
     "static const unsigned char "+p+"_bt[12]={3,7,1,9,4,12,6,2,10,5,8,0};\n"
     +t+" "+p+"_belltri("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; unsigned M=1009u;\n"
     "    unsigned bell[16]; bell[0]=((unsigned)"+p+"_bt[0]^(s&7u))%M; unsigned len=1u; acc=acc*131u+bell[0];\n"
     "    for(int n=1;n<12;n++){ unsigned prev=(bell[len-1]+((unsigned)"+p+"_bt[n]^((s>>(n&7))&7u)))%M;\n"
     "      for(unsigned k=0;k<len;k++){ unsigned save=bell[k]; bell[k]=prev; prev=(prev+save)%M; }\n"
     "      bell[len]=prev; len=len+1u; for(unsigned k=0;k<len;k++) acc=acc*131u+bell[k]; }\n"
     "    acc=acc*131u+bell[len-1]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Fu}, "OptStress170", 2},

    // 5-smooth (Hamming / "ugly") numbers via a three-pointer ordered merge.
    {p+"_ugly",
     "static const unsigned char "+p+"_ug[8]={5,17,2,29,11,23,7,19};\n"
     +t+" "+p+"_ugly("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned ug[40]; ug[0]=1u; unsigned i2=0u,i3=0u,i5=0u;\n"
     "    for(int n=1;n<40;n++){ unsigned n2=ug[i2]*2u, n3=ug[i3]*3u, n5=ug[i5]*5u;\n"
     "      unsigned mn=n2; if(n3<mn) mn=n3; if(n5<mn) mn=n5;\n"
     "      ug[n]=mn; if(mn==n2) i2++; if(mn==n3) i3++; if(mn==n5) i5++; acc=acc*131u+mn; }\n"
     "    unsigned idx=((unsigned)"+p+"_ug[it&7]^(s&7u))%40u; acc=acc*131u+ug[idx]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x70u}, "OptStress170", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress170TC("x64o170", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress170TC("x86o170", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress170TC("a64o170", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress170TC("armo170", "int");

INSTANTIATE_TEST_SUITE_P(OptStress170, X64OptStress170RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress170, X86OptStress170RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress170, A64OptStress170RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress170, ARM32OptStress170RT, ::testing::ValuesIn(kARM), rtTCName);
