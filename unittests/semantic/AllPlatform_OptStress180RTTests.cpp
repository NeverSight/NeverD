//===- AllPlatform_OptStress180RTTests.cpp - Z-func / paren-depth / atoi ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * zfunc     - Z-function: for each position the length of the longest prefix
//                 of the string starting there, via the [l,r] Z-box window.
//                 Pins the Z-box prefix scan (distinct from the KMP failure
//                 function / LCS / edit shapes in #129/#146).
//   * parendep  - bracket validity by running stack depth: open bumps depth,
//                 close drops it, an underflow flags invalid.  Pins a balanced-
//                 delimiter depth scan (distinct from every numeric reduction).
//   * atoisat   - string-to-int parse with saturating clamp and a terminator
//                 byte.  Pins a base-10 accumulate-with-saturate (distinct from
//                 the digit-double Luhn #175 and the rolling hash folds).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress180RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress180RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress180RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress180RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress180RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress180RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress180RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress180RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress180TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Z-function via the [l,r] Z-box window.
    {p+"_zfunc",
     "static const unsigned char "+p+"_zf[24]={1,2,1,1,2,1,3,1,2,1,1,2,1,1,2,3,1,2,1,1,2,1,3,1};\n"
     +t+" "+p+"_zfunc("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=((unsigned)"+p+"_zf[i]^((s>>(i&7))&1u))&3u;\n"
     "    unsigned z[24]; z[0]=0u; int l=0,r=0; unsigned fold=0u;\n"
     "    for(int i=1;i<24;i++){ unsigned zz=0u; if(i<r){ zz=(unsigned)(r-i); if(z[i-l]<zz) zz=z[i-l]; }\n"
     "      while(i+(int)zz<24 && arr[zz]==arr[i+(int)zz]) zz++;\n"
     "      z[i]=zz; if(i+(int)zz>r){ l=i; r=i+(int)zz; } fold=fold*131u+zz; }\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Au}, "OptStress180", 2},

    // bracket validity by running stack depth.
    {p+"_parendep",
     "static const unsigned char "+p+"_pr[24]={1,1,0,1,0,0,1,1,1,0,0,1,0,1,1,0,0,0,1,0,1,1,0,0};\n"
     +t+" "+p+"_parendep("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int depth=0, valid=1; unsigned maxd=0u, fold=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned b=((unsigned)"+p+"_pr[i]^((s>>(i&7))&1u))&1u;\n"
     "      if(b) depth++; else { depth--; if(depth<0){ valid=0; depth=0; } }\n"
     "      if((unsigned)depth>maxd) maxd=(unsigned)depth; fold=fold*131u+(unsigned)depth; }\n"
     "    acc=acc*131u+fold+maxd*7u+(unsigned)valid+(depth==0?17u:0u); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Bu}, "OptStress180", 2},

    // string-to-int parse with saturating clamp and terminator byte.
    {p+"_atoisat",
     "static const unsigned char "+p+"_at[16]={3,7,1,9,4,12,2,8,5,0,6,9,1,15,3,7};\n"
     +t+" "+p+"_atoisat("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, LIM=100000u;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned val=0u; int done=0; unsigned fold=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned d=((unsigned)"+p+"_at[i]^((s>>(i&7))&1u));\n"
     "      if(!done){ if(d<10u){ val=val*10u+d; if(val>LIM){ val=LIM; done=1; } } else done=1; }\n"
     "      fold=fold*131u+val; }\n"
     "    acc=acc*131u+fold+val; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Cu}, "OptStress180", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress180TC("x64o180", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress180TC("x86o180", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress180TC("a64o180", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress180TC("armo180", "int");

INSTANTIATE_TEST_SUITE_P(OptStress180, X64OptStress180RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress180, X86OptStress180RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress180, A64OptStress180RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress180, ARM32OptStress180RT, ::testing::ValuesIn(kARM), rtTCName);
