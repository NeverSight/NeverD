//===- AllPlatform_OptStress155RTTests.cpp - RPN eval / Luhn / SP-network =//
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
//   * rpn  - reverse-Polish expression evaluation over a rodata token stream:
//            digits push onto an operand stack, operators pop two and push the
//            result.  Pins a stack-machine evaluator (distinct from the recursion
//            stack of the #151 quicksort and any straight-line fold).
//   * luhn - Luhn mod-10 checksum over rodata digit windows: every second digit
//            from the right is doubled and cast-out-nines folded, then the check
//            digit is derived.  Pins a positional double-and-sum checksum
//            (distinct from the CRC/Adler-style folds and the digit root in #153).
//   * sbox - substitution-permutation rounds over a rodata nibble block: an
//            S-box substitutes each nibble and a permutation table scatters them,
//            repeated for several rounds.  Pins an SPN round (distinct from the
//            move-to-front and bit-reversal permutations elsewhere).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress155RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress155RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress155RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress155RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress155RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress155RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress155RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress155RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress155TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // reverse-Polish expression evaluation over a rodata token stream.
    {p+"_rpn",
     "static const unsigned char "+p+"_rp[24]={3,4,250,5,252,2,251,7,1,250,252,6,3,250,251,8,2,252,4,250,9,251,5,250};\n"
     +t+" "+p+"_rpn("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned st[24]; int sp=0;\n"
     "    for(int i=0;i<24;i++){ unsigned tk=(unsigned)"+p+"_rp[i];\n"
     "      if(tk<10u){ if(sp<24) st[sp++]=tk+((s>>(i&7))&1u); }\n"
     "      else { if(sp>=2){ unsigned b=st[--sp], aa=st[--sp], r;\n"
     "        if(tk==250u) r=aa+b; else if(tk==251u) r=aa-b; else r=aa*b; r&=0xFFFFu; if(sp<24) st[sp++]=r; acc=acc*131u+r; } }\n"
     "      acc=acc*131u+(unsigned)sp; }\n"
     "    if(sp>0) acc=acc*131u+st[sp-1]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Fu}, "OptStress155", 2},

    // Luhn mod-10 checksum over rodata digit windows (double-and-sum).
    {p+"_luhn",
     "static const unsigned char "+p+"_ln[20]={4,9,2,7,1,8,3,6,5,0,9,1,4,7,2,8,3,6,1,5};\n"
     +t+" "+p+"_luhn("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<4;q++){ unsigned sum=0u; int start=(int)((q*5u)%16u);\n"
     "      for(int i=0;i<16;i++){ unsigned d=((unsigned)"+p+"_ln[(start+i)%20]+((s>>(i&7))&1u))%10u;\n"
     "        if((((16-1-i)&1))==1){ d*=2u; if(d>9u) d-=9u; } sum+=d; }\n"
     "      unsigned chk=(10u-(sum%10u))%10u; acc=acc*131u+sum+chk; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Cu}, "OptStress155", 2},

    // substitution-permutation rounds over a rodata nibble block (SPN).
    {p+"_sbox",
     "static const unsigned char "+p+"_box[16]={6,11,0,13,9,2,15,4,8,14,3,1,12,7,10,5};\n"
     "static const unsigned char "+p+"_perm[16]={3,7,0,12,5,9,14,1,11,2,15,8,4,10,6,13};\n"
     "static const unsigned char "+p+"_in[16]={1,9,4,12,7,2,15,5,8,0,13,6,11,3,14,10};\n"
     +t+" "+p+"_sbox("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned st[16]; for(int i=0;i<16;i++) st[i]=((unsigned)"+p+"_in[i]^((s>>(i&7))&15u))&15u;\n"
     "    for(int round=0;round<4;round++){ unsigned sub[16],per[16];\n"
     "      for(int i=0;i<16;i++) sub[i]=(unsigned)"+p+"_box[st[i]&15];\n"
     "      for(int i=0;i<16;i++) per[(unsigned)"+p+"_perm[i]&15]=sub[i];\n"
     "      for(int i=0;i<16;i++){ st[i]=per[i]^(unsigned)round; acc=acc*131u+st[i]; } }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+st[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x60u}, "OptStress155", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress155TC("x64o155", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress155TC("x86o155", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress155TC("a64o155", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress155TC("armo155", "int");

INSTANTIATE_TEST_SUITE_P(OptStress155, X64OptStress155RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress155, X86OptStress155RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress155, A64OptStress155RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress155, ARM32OptStress155RT, ::testing::ValuesIn(kARM), rtTCName);
