//===- AllPlatform_OptStress104RTTests.cpp - stateful / search rodata shapes =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * rc4    - RC4 key schedule + PRGA: a 256-byte WRITABLE stack permutation
//              state initialised 0..255, mixed against a rodata key with index
//              swaps, then a keystream gather `st[(st[i]+st[j])&255]`.  Pins a
//              large stack array of byte index-RMW + swaps driven by a small
//              rodata key.
//   * boyer  - Boyer-Moore-Horspool search (4-bit alphabet): a bad-character
//              skip table built in a stack array from a rodata pattern, then a
//              VARIABLE forward stride over a rodata text.  Pins a data-derived
//              forward skip from a runtime-built table + a rodata compare.
//   * lfsr   - Galois LFSR whose feedback polynomial is selected each step from
//              a rodata 16-bit tap table.  Pins a halfword rodata load (zext)
//              feeding a shift+conditional-xor bit recurrence.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress104RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress104RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress104RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress104RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress104RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress104RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress104RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress104RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress104TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // RC4 KSA + PRGA: 256-byte stack permutation, rodata key, keystream gather.
    {p+"_rc4",
     "static const unsigned char "+p+"_key[16]={\n"
     "0x53,0x2a,0x9f,0x14,0xc7,0x6b,0xe0,0x38, 0x91,0x4d,0xa6,0x1f,0x72,0xcb,0x05,0xbd};\n"
     +t+" "+p+"_rc4("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<24;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned char st[256];\n"
     "    for(int i=0;i<256;i++) st[i]=(unsigned char)i;\n"
     "    unsigned j=0;\n"
     "    for(int i=0;i<256;i++){ j=(j+st[i]+"+p+"_key[i&15]+(s>>3))&0xFFu;\n"
     "      unsigned char tmp=st[i]; st[i]=st[j]; st[j]=tmp; }\n"
     "    unsigned i2=0, acc=0; j=0;\n"
     "    for(int k=0;k<64;k++){ i2=(i2+1)&0xFFu; j=(j+st[i2])&0xFFu;\n"
     "      unsigned char tmp=st[i2]; st[i2]=st[j]; st[j]=tmp;\n"
     "      acc=acc*131u+st[(st[i2]+st[j])&0xFFu]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC4u}, "OptStress104", 2},

    // Boyer-Moore-Horspool search over rodata text with runtime bad-char skip.
    {p+"_boyer",
     "static const unsigned char "+p+"_pat[8]={3,9,14,2,7,11,5,1};\n"
     "static const unsigned char "+p+"_txt[64]={\n"
     "5,3,9,14,2,7,11,5, 1,8,3,9,14,2,7,11, 5,1,0,6,12,4,10,15,\n"
     "3,9,14,2,7,11,5,1, 13,2,8,5,3,9,14,2, 7,11,5,1,6,0,12,4,\n"
     "9,14,2,7,11,5,1,3, 10,15,8,3,9,14,2,7};\n"
     +t+" "+p+"_boyer("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned bad[16]; for(int i=0;i<16;i++) bad[i]=8u;\n"
     "    for(int i=0;i<7;i++) bad["+p+"_pat[i]&15u]=7u-(unsigned)i;\n"
     "    unsigned pos=0, acc=s, hits=0;\n"
     "    while(pos+8<=64u){ int j=7;\n"
     "      while(j>=0 && ("+p+"_txt[pos+(unsigned)j]&15u)==("+p+"_pat[j]&15u)) j--;\n"
     "      if(j<0){ hits++; acc=acc*131u+pos; pos+=1u; }\n"
     "      else { unsigned sk=bad["+p+"_txt[pos+7u]&15u]; pos+=sk?sk:1u; } }\n"
     "    out=out*1311u+acc+hits; }\n"
     "  return ("+t+")out; }\n",
     {0xB0u}, "OptStress104", 2},

    // Galois LFSR with a rodata 16-bit tap table selected per step.
    {p+"_lfsr",
     "static const unsigned short "+p+"_taps[8]={\n"
     "0xB400,0xA001,0xC002,0x8016, 0x9C00,0xD008,0xE001,0xB008};\n"
     +t+" "+p+"_lfsr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned reg=((s>>7)&0xFFFFu)|1u, acc=0;\n"
     "    for(int k=0;k<64;k++){ unsigned tap="+p+"_taps[k&7u];\n"
     "      unsigned lsb=reg&1u; reg>>=1; if(lsb) reg^=tap;\n"
     "      acc=acc*131u+reg+(lsb<<3); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Fu}, "OptStress104", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress104TC("x64o104", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress104TC("x86o104", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress104TC("a64o104", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress104TC("armo104", "int");

INSTANTIATE_TEST_SUITE_P(OptStress104, X64OptStress104RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress104, X86OptStress104RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress104, A64OptStress104RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress104, ARM32OptStress104RT, ::testing::ValuesIn(kARM), rtTCName);
