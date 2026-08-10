//===- AllPlatform_OptStress186RTTests.cpp - Fletcher16 / CRC8 / Adler32 ====//
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
//   * fletcher16 - Fletcher-16 checksum: two running sums reduced mod 255.  Pins
//                  a coupled dual-accumulator checksum (distinct from the single
//                  rolling hashes and the Luhn digit-double #175).
//   * crc8       - bitwise CRC-8 (polynomial 0x07, no lookup table).  Pins a
//                  per-bit shift/xor remainder (distinct from the table CRC32
//                  #ARM32_Crc32 and the popcount loops).
//   * adler32    - Adler-32 checksum: two sums reduced mod 65521.  Pins a
//                  large-prime modular dual accumulator (distinct from the
//                  Fletcher mod-255 pair above).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress186RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress186RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress186RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress186RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress186RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress186RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress186RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress186RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress186TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fletcher-16: two running sums reduced mod 255.
    {p+"_fletcher16",
     "static const unsigned char "+p+"_fl[24]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47,11,60,23,5};\n"
     +t+" "+p+"_fletcher16("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned sum1=0u, sum2=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned d=((unsigned)"+p+"_fl[i]^((s>>(i&7))&255u)); sum1=(sum1+d)%255u; sum2=(sum2+sum1)%255u; }\n"
     "    unsigned chk=(sum2<<8)|sum1; acc=acc*131u+chk; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x42u}, "OptStress186", 2},

    // bitwise CRC-8, polynomial 0x07, no lookup table.
    {p+"_crc8",
     "static const unsigned char "+p+"_cr[16]={211,97,143,38,176,52,9,250,17,88,201,4,159,61,7,244};\n"
     +t+" "+p+"_crc8("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned crc=0u;\n"
     "    for(int i=0;i<16;i++){ crc^=((unsigned)"+p+"_cr[i]^((s>>(i&7))&255u))&0xFFu;\n"
     "      for(int b=0;b<8;b++){ if(crc&0x80u) crc=((crc<<1)^0x07u)&0xFFu; else crc=(crc<<1)&0xFFu; } }\n"
     "    acc=acc*131u+crc; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x51u}, "OptStress186", 2},

    // Adler-32: two sums reduced mod 65521.
    {p+"_adler32",
     "static const unsigned char "+p+"_ad[24]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47,11,60,23,5};\n"
     +t+" "+p+"_adler32("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, MOD=65521u;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A=1u, B=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned d=((unsigned)"+p+"_ad[i]^((s>>(i&7))&255u)); A=(A+d)%MOD; B=(B+A)%MOD; }\n"
     "    unsigned chk=(B<<16)|A; acc=acc*131u+chk; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x82u}, "OptStress186", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress186TC("x64o186", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress186TC("x86o186", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress186TC("a64o186", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress186TC("armo186", "int");

INSTANTIATE_TEST_SUITE_P(OptStress186, X64OptStress186RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress186, X86OptStress186RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress186, A64OptStress186RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress186, ARM32OptStress186RT, ::testing::ValuesIn(kARM), rtTCName);
