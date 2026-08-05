//===- AllPlatform_OptStress166RTTests.cpp - bit-pack / grid DP / XOR basis =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * bitpack  - pack three small rodata fields into one word and unpack them
//                back, checking the round-trip.  Pins a bitfield pack/unpack
//                (distinct from the Morton interleave in #149 and any byte swap).
//   * gridpath - count monotone lattice paths through a rodata 5x5 grid with
//                obstacles via a row-major additive DP.  Pins a 2-D path-count
//                recurrence (distinct from the 1-D knapsack/coin DPs in #150).
//   * xorbasis - build a GF(2) linear basis of rodata vectors by Gaussian
//                elimination and report the rank.  Pins an XOR-elimination basis
//                (distinct from any integer Gaussian or arithmetic reduction).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress166RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress166RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress166RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress166RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress166RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress166RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress166RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress166RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress166TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // pack three small rodata fields into a word and unpack (round-trip).
    {p+"_bitpack",
     "static const unsigned char "+p+"_bp[24]={5,9,12,3,7,14,2,8,11,6,1,15,4,10,13,0,9,5,12,3,7,2,8,6};\n"
     +t+" "+p+"_bitpack("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[24]; for(int i=0;i<24;i++) d[i]=(unsigned)"+p+"_bp[i]^((s>>(i&7))&1u);\n"
     "    for(int q=0;q<8;q++){ unsigned f0=d[q*3+0]&7u,f1=d[q*3+1]&15u,f2=d[q*3+2]&31u;\n"
     "      unsigned packed=f0|(f1<<3)|(f2<<7); unsigned u0=packed&7u,u1=(packed>>3)&15u,u2=(packed>>7)&31u;\n"
     "      acc=acc*131u+packed+((u0==f0&&u1==f1&&u2==f2)?1u:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Au}, "OptStress166", 2},

    // count monotone lattice paths through a rodata 5x5 obstacle grid (2-D DP).
    {p+"_gridpath",
     "static const unsigned char "+p+"_gp[25]={1,2,3,1,2,2,0,1,3,1,3,1,2,0,2,1,2,3,1,2,2,1,0,2,3};\n"
     +t+" "+p+"_gridpath("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned blk[25]; for(int i=0;i<25;i++) blk[i]=((((unsigned)"+p+"_gp[i]^((s>>(i&7))&1u))&3u)==0u)?1u:0u;\n"
     "    unsigned dp[25];\n"
     "    for(int r=0;r<5;r++) for(int c=0;c<5;c++){ int idx=r*5+c;\n"
     "      if(blk[idx]){ dp[idx]=0u; continue; } if(r==0&&c==0){ dp[idx]=1u; continue; }\n"
     "      unsigned v=0u; if(r>0) v+=dp[idx-5]; if(c>0) v+=dp[idx-1]; dp[idx]=v; acc=acc*131u+dp[idx]; }\n"
     "    acc=acc*131u+dp[24]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Bu}, "OptStress166", 2},

    // GF(2) linear basis of rodata vectors by Gaussian elimination (rank).
    {p+"_xorbasis",
     "static const unsigned char "+p+"_xb[20]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28,0x4d,0xf2,0x81,0x3c};\n"
     +t+" "+p+"_xorbasis("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned basis[16]; for(int i=0;i<16;i++) basis[i]=0u; unsigned rank=0u;\n"
     "    for(int i=0;i<20;i++){ unsigned v=(((unsigned)"+p+"_xb[i]<<4)|((s>>(i&7))&15u))&0xFFFu;\n"
     "      for(int b=11;b>=0;b--){ if(!((v>>b)&1u)) continue; if(!basis[b]){ basis[b]=v; rank++; break; } v^=basis[b]; }\n"
     "      acc=acc*131u+v+rank; }\n"
     "    acc=acc*131u+rank; for(int i=0;i<16;i++) acc=acc*131u+basis[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Cu}, "OptStress166", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress166TC("x64o166", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress166TC("x86o166", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress166TC("a64o166", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress166TC("armo166", "int");

INSTANTIATE_TEST_SUITE_P(OptStress166, X64OptStress166RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress166, X86OptStress166RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress166, A64OptStress166RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress166, ARM32OptStress166RT, ::testing::ValuesIn(kARM), rtTCName);
