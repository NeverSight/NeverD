//===- AllPlatform_OptStress123RTTests.cpp - rolling / stack / diag shapes --=//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * rabin  - Rabin-Karp rolling polynomial hash search of a rodata pattern in
//              a rodata text under a CONSTANT modulus: `h=(h-old*B^k)*B+new`.
//              Pins a sliding rolling-hash recurrence + hash compare (distinct
//              from the KMP/Boyer character automata).
//   * brackets- multi-kind bracket matcher over a rodata token stream using a
//              stack array: push on open, pop+check on close.  Pins a LIFO stack
//              discipline driven by rodata tokens.
//   * diag   - anti-diagonal traversal reduce of a rodata 8x8 matrix: each
//              anti-diagonal `img[i*8+(d-i)]` is folded with a guard `0<=j<8`.
//              Pins a sheared 2D index walk (row +8, col -1 in lockstep) of
//              pure rodata reads with no write-back recurrence.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress123RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress123RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress123RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress123RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress123RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress123RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress123RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress123RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress123TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Rabin-Karp rolling-hash search of a rodata pattern in rodata text.
    {p+"_rabin",
     "static const unsigned char "+p+"_pat[6]={5,2,9,2,5,1};\n"
     "static const unsigned char "+p+"_txt[48]={\n"
     "5,2,9,2,5,1,3,2, 9,2,5,1,0,4,5,2, 9,2,5,1,6,1,5,2, 9,2,5,1,7,3,2,5, 2,1,5,2,9,2,5,1, 0,6,4,7,5,2,9,2};\n"
     +t+" "+p+"_rabin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned M=65521u, B=131u;\n"
     "    unsigned ph=0u; for(int i=0;i<6;i++) ph=(ph*B+("+p+"_pat[i]&0xFu))%M;\n"
     "    unsigned pw=1u; for(int i=0;i<5;i++) pw=(pw*B)%M;\n"
     "    unsigned h=0u; for(int i=0;i<6;i++) h=(h*B+(("+p+"_txt[i]^(s&7u))&0xFu))%M;\n"
     "    unsigned hits=0u;\n"
     "    for(int i=0;i+6<=48;i++){ if(h==ph) hits++; acc=acc*131u+h;\n"
     "      if(i+6<48){ unsigned old=("+p+"_txt[i]^(s&7u))&0xFu, nw=("+p+"_txt[i+6]^(s&7u))&0xFu;\n"
     "        h=(h + M - (old*pw)%M)%M; h=(h*B+nw)%M; } }\n"
     "    acc=acc*131u+hits; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xABu}, "OptStress123", 2},

    // multi-kind bracket matcher over a rodata token stream (LIFO stack).
    {p+"_brackets",
     "static const unsigned char "+p+"_tok[48]={\n"
     "0,1,4,3,2,5,6,0, 3,1,4,2,5,6,0,1, 4,3,2,0,3,5,6,1, 4,2,5,0,3,6,1,4, 2,5,6,0,1,3,4,2, 5,6,0,3,1,4,2,5};\n"
     +t+" "+p+"_brackets("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned stv[48]; int sp=0; unsigned bal=1u, dmax=0u;\n"
     "    for(int i=0;i<48;i++){ unsigned tk=("+p+"_tok[i]+((s>>(i&7))&1u))%7u;\n"
     "      if(tk<3u){ if(sp<48) stv[sp++]=tk; if((unsigned)sp>dmax) dmax=(unsigned)sp; }\n"
     "      else if(tk<6u){ unsigned want=tk-3u; if(sp>0 && stv[sp-1]==want) sp--; else bal=0u; }\n"
     "      acc=acc*131u+(unsigned)sp*3u+tk; }\n"
     "    acc=acc*131u+bal+dmax+(unsigned)sp; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Bu}, "OptStress123", 2},

    // anti-diagonal traversal reduce of a rodata 8x8 matrix (sheared 2D walk).
    {p+"_diag",
     "static const unsigned char "+p+"_img[64]={\n"
     "10,20,35,50,60,75,85,95, 25,40,55,70,80,90,100,110, 30,45,65,85,95,105,115,120,\n"
     "35,55,75,95,110,125,130,140, 40,60,80,100,115,130,140,150, 45,65,85,105,120,135,145,155,\n"
     "50,70,90,110,125,140,150,160, 55,75,95,115,130,145,155,165};\n"
     +t+" "+p+"_diag("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int d=0;d<15;d++){ unsigned dsum=0u;\n"
     "      for(int i=0;i<8;i++){ int j=d-i; if(j>=0 && j<8)\n"
     "        dsum=dsum*7u+("+p+"_img[i*8+j]^((s>>(d&7))&1u)); }\n"
     "      acc=acc*131u+dsum; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Au}, "OptStress123", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress123TC("x64o123", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress123TC("x86o123", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress123TC("a64o123", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress123TC("armo123", "int");

INSTANTIATE_TEST_SUITE_P(OptStress123, X64OptStress123RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress123, X86OptStress123RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress123, A64OptStress123RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress123, ARM32OptStress123RT, ::testing::ValuesIn(kARM), rtTCName);
