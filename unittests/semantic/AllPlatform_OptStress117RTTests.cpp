//===- AllPlatform_OptStress117RTTests.cpp - hash / match / convolve shapes =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * htbl   - open-addressing hash table (linear probing) built in stack arrays
//              from rodata keys: a multiplicative hash reduced by a CONSTANT
//              prime modulus picks a bucket, then `(h+probe)%37` walks the probe
//              sequence.  Pins a hash -> bucket -> probe chain over a stack table.
//   * wild   - wildcard pattern match (`*`/`?`) of a rodata pattern against a
//              rodata text via a rolling DP row with `*` run-propagation.  Pins
//              a two-array DP with star-state carry (distinct from edit distance).
//   * polymul- polynomial multiply of two rodata coefficient arrays via the
//              outer-product scatter `r[i+j]+=a[i]*b[j]`.  Pins an offset-summed
//              scatter-accumulate (distinct from the sliding FIR dot product).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress117RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress117RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress117RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress117RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress117RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress117RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress117RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress117RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress117TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // open-addressing hash table (linear probe) over rodata keys.
    {p+"_htbl",
     "static const unsigned char "+p+"_keys[24]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d};\n"
     +t+" "+p+"_htbl("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned sk[37], sv[37]; for(int i=0;i<37;i++){ sk[i]=0; sv[i]=0; }\n"
     "    for(int i=0;i<24;i++){ unsigned key=("+p+"_keys[i]^(s&0xFFu))+1u;\n"
     "      unsigned h=(key*2654435761u)%37u;\n"
     "      for(int probe=0;probe<37;probe++){ unsigned idx=(h+(unsigned)probe)%37u;\n"
     "        if(sk[idx]==0u || sk[idx]==key){ sk[idx]=key; sv[idx]+=key+(unsigned)i; break; } } }\n"
     "    for(int i=0;i<37;i++) acc=acc*131u+sk[i]+sv[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x47u}, "OptStress117", 2},

    // wildcard (*/?) match of a rodata pattern against rodata text (rolling DP).
    {p+"_wild",
     "static const unsigned char "+p+"_pat[12]={2,1,0,5,1,0,9,3,1,0,7,4};\n"
     "static const unsigned char "+p+"_txt[16]={2,8,5,3,9,6,1,5,9,3,2,7,4,0,7,6};\n"
     +t+" "+p+"_wild("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dp[17]; for(int j=0;j<=16;j++) dp[j]=0u; dp[0]=1u;\n"
     "    for(int i=0;i<12;i++){ unsigned pc="+p+"_pat[i]; unsigned ndp[17];\n"
     "      for(int j=0;j<=16;j++) ndp[j]=0u;\n"
     "      if(pc==0u){ unsigned run=0u; for(int j=0;j<=16;j++){ run|=dp[j]; ndp[j]=run; } }\n"
     "      else { for(int j=1;j<=16;j++){ unsigned tc=("+p+"_txt[j-1]^(s&7u))&0xFu;\n"
     "          unsigned m=(pc==1u)||(pc==tc); ndp[j]=(dp[j-1]&&m)?1u:0u; } }\n"
     "      for(int j=0;j<=16;j++) dp[j]=ndp[j]; acc=acc*131u+dp[16]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x57u}, "OptStress117", 2},

    // polynomial multiply of two rodata coefficient arrays (outer-product scatter).
    {p+"_polymul",
     "static const unsigned char "+p+"_pa[12]={7,3,11,2,9,5,14,1,6,12,4,10};\n"
     "static const unsigned char "+p+"_pb[12]={2,8,5,1,7,3,9,6,4,10,12,11};\n"
     +t+" "+p+"_polymul("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned r[23]; for(int i=0;i<23;i++) r[i]=0u;\n"
     "    for(int i=0;i<12;i++){ unsigned ai="+p+"_pa[i]^((s>>(i&7))&3u);\n"
     "      for(int j=0;j<12;j++) r[i+j]+=ai*(unsigned)"+p+"_pb[j]; }\n"
     "    for(int i=0;i<23;i++) acc=acc*131u+(r[i]&0xFFFFu);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x9Du}, "OptStress117", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress117TC("x64o117", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress117TC("x86o117", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress117TC("a64o117", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress117TC("armo117", "int");

INSTANTIATE_TEST_SUITE_P(OptStress117, X64OptStress117RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress117, X86OptStress117RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress117, A64OptStress117RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress117, ARM32OptStress117RT, ::testing::ValuesIn(kARM), rtTCName);
