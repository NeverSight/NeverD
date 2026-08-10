//===- AllPlatform_OptStress152RTTests.cpp - atoi / Caesar / word count =//
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
//   * atoip  - base-10 string-to-integer parse over a rodata digit run: a Horner
//              `val=val*10+d` accumulates while characters stay in the digit
//              range.  Pins a radix-10 parse accumulation (distinct from any
//              hash/checksum fold).
//   * caesar - Caesar/ROT shift cipher over a rodata letter stream with a rolling
//              per-position shift, plus the modular inverse to recover the letter.
//              Pins a modular add/sub cipher (distinct from the XOR-feedback
//              LFSR in #149 and the Gray code in #141).
//   * wordcnt- whitespace-delimited word counter over a rodata character stream:
//              an in-word/at-space state machine bumps the count on each rising
//              edge and tracks the longest token.  Pins a two-state token scan
//              (distinct from the run-length encoder in #147).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress152RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress152RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress152RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress152RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress152RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress152RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress152RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress152RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress152TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // base-10 string-to-integer parse over a rodata digit run (Horner).
    {p+"_atoip",
     "static const unsigned char "+p+"_aps[20]={49,50,51,57,48,32,55,56,49,32,57,52,50,32,48,51,54,55,32,49};\n"
     +t+" "+p+"_atoip("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<8;q++){ int i=(int)((q*3u+(s&7))%18u); unsigned val=0u;\n"
     "      while(i<20 && "+p+"_aps[i]>=48 && "+p+"_aps[i]<=57){ val=val*10u+(unsigned)("+p+"_aps[i]-48); i++; }\n"
     "      acc=acc*131u+val; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Cu}, "OptStress152", 2},

    // Caesar/ROT shift cipher over a rodata letter stream (modular add + inverse).
    {p+"_caesar",
     "static const unsigned char "+p+"_cz[32]={0,4,11,11,14,21,7,4,17,4,24,1,8,3,8,19,2,0,4,18,0,17,2,8,15,7,4,17,19,4,23,19};\n"
     +t+" "+p+"_caesar("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; unsigned key=s&15u;\n"
     "    for(int i=0;i<32;i++){ unsigned c=(unsigned)"+p+"_cz[i]%26u; unsigned sh=(key+(unsigned)i)%26u;\n"
     "      unsigned enc=(c+sh)%26u; unsigned dec=(enc+26u-sh)%26u; acc=acc*131u+enc+((dec==c)?1u:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Du}, "OptStress152", 2},

    // whitespace-delimited word counter over a rodata character stream.
    {p+"_wordcnt",
     "static const unsigned char "+p+"_wc[40]={7,9,0,12,5,0,0,2,9,9,1,0,3,3,0,0,5,7,1,0,11,3,0,1,0,3,7,14,0,1,0,2,2,2,0,1,9,12,0,9};\n"
     +t+" "+p+"_wordcnt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, words=0u, inword=0u, maxlen=0u, cur=0u;\n"
     "    for(int i=0;i<40;i++){ unsigned ch=(unsigned)"+p+"_wc[i]^((s>>(i&7))&1u); unsigned isspace=(ch<3u)?1u:0u;\n"
     "      if(!isspace){ if(!inword){ words++; inword=1u; cur=0u; } cur++; if(cur>maxlen) maxlen=cur; } else inword=0u;\n"
     "      acc=acc*131u+words+cur; }\n"
     "    acc=acc*131u+words+maxlen; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Bu}, "OptStress152", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress152TC("x64o152", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress152TC("x86o152", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress152TC("a64o152", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress152TC("armo152", "int");

INSTANTIATE_TEST_SUITE_P(OptStress152, X64OptStress152RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress152, X86OptStress152RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress152, A64OptStress152RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress152, ARM32OptStress152RT, ::testing::ValuesIn(kARM), rtTCName);
