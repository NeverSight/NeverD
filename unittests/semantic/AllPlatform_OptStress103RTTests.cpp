//===- AllPlatform_OptStress103RTTests.cpp - fold / permuted rodata shapes -==//
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
//   * horner - Horner polynomial evaluation `acc=acc*x+coef[i]` over a rodata
//              coefficient table walked forward.  Pins a multiply-accumulate
//              fold whose only memory input is the indexed rodata coefficient.
//   * adler  - Adler-32 rolling checksum over a rodata buffer: two running sums
//              with a constant `% 65521` reduction (magic-number division, no
//              libcall) folded into `(B<<16)|A`.  Pins a stream fold with a
//              constant modulo on an indexed rodata load.
//   * zigzag - JPEG zig-zag permutation gather: read `dat[zz[k]]` where `zz` is
//              a rodata index permutation (a data-derived index into a second
//              rodata data block) and weight it by lane.  Pins a permuted
//              one-level rodata indirection plus a positional weight.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress103RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress103RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress103RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress103RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress103RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress103RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress103RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress103RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress103TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Horner polynomial evaluation over a rodata coefficient table.
    {p+"_horner",
     "static const unsigned char "+p+"_coef[16]={\n"
     "3,7,1,9,2,8,5,13, 4,11,6,15,10,1,14,12};\n"
     +t+" "+p+"_horner("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=(s>>7)&0xFFu, acc=0;\n"
     "    for(int i=0;i<16;i++) acc=acc*x+"+p+"_coef[i];\n"
     "    out=out*131u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress103", 2},

    // Adler-32 rolling checksum over a rodata buffer, constant % 65521.
    {p+"_adler",
     "static const unsigned char "+p+"_buf[48]={\n"
     "0x12,0x9a,0x3f,0x71,0xc4,0x05,0xbe,0x68, 0x2d,0xf1,0x47,0x83,0x1c,0xe9,0x56,0xa0,\n"
     "0x7b,0x34,0xd8,0x0f,0x92,0x61,0xae,0x25, 0xcf,0x53,0x18,0xfa,0x46,0x8d,0x21,0xb7,\n"
     "0x6c,0x09,0xe3,0x57,0x90,0x3a,0xd1,0x4e, 0x82,0x15,0xff,0x63,0xab,0x27,0x5c,0xc8};\n"
     +t+" "+p+"_adler("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned A=1u, B=0u;\n"
     "    for(int i=0;i<48;i++){ A=(A+"+p+"_buf[i]+((s>>(i&7))&1u))%65521u; B=(B+A)%65521u; }\n"
     "    out=out*131u+((B<<16)|A); }\n"
     "  return ("+t+")out; }\n",
     {0x3Au}, "OptStress103", 2},

    // JPEG zig-zag permutation gather: dat[zz[k]] weighted by lane.
    {p+"_zigzag",
     "static const unsigned char "+p+"_zz[64]={\n"
     "0,1,8,16,9,2,3,10, 17,24,32,25,18,11,4,5,\n"
     "12,19,26,33,40,48,41,34, 27,20,13,6,7,14,21,28,\n"
     "35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,\n"
     "58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63};\n"
     "static const unsigned char "+p+"_dat[64]={\n"
     "7,19,3,28,11,40,5,33, 22,9,47,1,30,14,52,8,\n"
     "16,38,2,25,44,6,31,12, 49,20,4,37,10,55,17,29,\n"
     "41,13,26,0,53,21,34,15, 48,27,5,39,18,45,9,32,\n"
     "23,50,11,36,7,42,19,54, 3,28,46,14,51,24,38,60};\n"
     +t+" "+p+"_zigzag("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=0;\n"
     "    unsigned rot=(s>>5)&63u;\n"
     "    for(int k=0;k<64;k++){ unsigned idx="+p+"_zz[(k+rot)&63u]&63u;\n"
     "      acc=acc*131u+(unsigned)"+p+"_dat[idx]*((k&7u)+1u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Au}, "OptStress103", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress103TC("x64o103", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress103TC("x86o103", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress103TC("a64o103", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress103TC("armo103", "int");

INSTANTIATE_TEST_SUITE_P(OptStress103, X64OptStress103RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress103, X86OptStress103RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress103, A64OptStress103RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress103, ARM32OptStress103RT, ::testing::ValuesIn(kARM), rtTCName);
