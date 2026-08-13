//===- AllPlatform_OptStress102RTTests.cpp - magic-index rodata shapes ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES not previously pinned,
// all address-independent (the folded result depends only on the bytes stored
// in the globals + the control flow, never on an absolute VA) and all reached
// by pure index arithmetic from the array base (`tab[idx]`, never an interior
// pointer), so none touches the deferred i386/ARM32 PIC rodata *interior*-
// pointer model (#477/#487) and every probe runs on all four targets.
//
//   * deb    - de Bruijn lowest-set-bit scan: isolate `x & -x`, multiply by the
//              0x077CB531 de Bruijn constant and shift right 27 to form a 5-bit
//              index into a 32-entry rodata position table.  Pins a multiply-
//              shift *hash* feeding a small rodata gather (the index is derived
//              arithmetically, not read from another table).
//   * interp - fixed-point piecewise-linear interpolation: read the ADJACENT
//              pair `lut[i]` and `lut[i+1]` from one rodata curve and blend
//              `(lo*(256-f)+hi*f)>>8`.  Pins a same-table adjacent-element read
//              (base+i and base+i+1) plus an all-integer 8.8 lerp.
//   * morton - Z-order (Morton) bit-interleave of two nibbles via a 16-entry
//              rodata "spread" table: `spr[x]|(spr[y]<<1)`, then a dependent
//              re-gather `spr[m&15]`.  Pins bit-spreading through a tiny rodata
//              table with a value-derived follow-up index.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress102RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress102RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress102RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress102RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress102RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress102RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress102RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress102RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress102TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // de Bruijn lowest-set-bit scan: multiply-shift hash -> 32-entry rodata table.
    {p+"_deb",
     "static const unsigned char "+p+"_dbp[32]={\n"
     "0,1,28,2,29,14,24,3, 30,22,20,15,25,17,4,8,\n"
     "31,27,13,23,21,19,16,7, 26,12,18,6,11,5,10,9};\n"
     +t+" "+p+"_deb("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<24;k++){\n"
     "      unsigned x=(s^((unsigned)k*0x9E3779B1u))|1u;\n"
     "      unsigned lsb=x&(0u-x);\n"
     "      unsigned idx=(lsb*0x077CB531u)>>27;\n"
     "      acc=acc*131u+"+p+"_dbp[idx]; acc^=acc>>5; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xD1u}, "OptStress102", 2},

    // fixed-point lerp: adjacent rodata pair lut[i]/lut[i+1], integer 8.8 blend.
    {p+"_interp",
     "static const unsigned char "+p+"_lut[65]={\n"
     "8,20,33,47,60,72,85,99, 110,121,130,138,145,151,156,160,\n"
     "163,165,166,166,165,163,160,156, 151,145,138,130,121,110,99,88,\n"
     "77,66,55,45,36,28,21,15, 10,6,3,1,0,1,3,6,\n"
     "10,15,21,28,36,45,55,66, 77,88,99,110,121,130,138,145, 151};\n"
     +t+" "+p+"_interp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=0;\n"
     "    for(int k=0;k<32;k++){ s=s*1103515245u+12345u;\n"
     "      unsigned i=(s>>8)&63u, f=s&0xFFu;\n"
     "      unsigned lo="+p+"_lut[i], hi="+p+"_lut[i+1];\n"
     "      unsigned v=(lo*(256u-f)+hi*f)>>8;\n"
     "      acc=acc*131u+v; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Au}, "OptStress102", 2},

    // Morton (Z-order) interleave of two nibbles via a 16-entry rodata spread.
    {p+"_morton",
     "static const unsigned char "+p+"_spr[16]={\n"
     "0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15,\n"
     "0x40,0x41,0x44,0x45,0x50,0x51,0x54,0x55};\n"
     +t+" "+p+"_morton("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<32;k++){ s=s*1103515245u+12345u;\n"
     "      unsigned x=(s>>3)&15u, y=(s>>11)&15u;\n"
     "      unsigned m="+p+"_spr[x]|((unsigned)"+p+"_spr[y]<<1);\n"
     "      acc=acc*131u+m+"+p+"_spr[m&15u]; acc^=acc>>6; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x27u}, "OptStress102", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress102TC("x64o102", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress102TC("x86o102", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress102TC("a64o102", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress102TC("armo102", "int");

INSTANTIATE_TEST_SUITE_P(OptStress102, X64OptStress102RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress102, X86OptStress102RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress102, A64OptStress102RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress102, ARM32OptStress102RT, ::testing::ValuesIn(kARM), rtTCName);
