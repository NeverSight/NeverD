//===- AllPlatform_OptStress137RTTests.cpp - Adler-32 / CRC-16 / Luhn ====//
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
//   * adler  - Adler-32 checksum over rodata bytes: two running sums reduced mod
//              65521.  Pins a dual modular accumulator (distinct from the
//              multiply-mix string hashes already covered).
//   * crc16  - bit-serial CRC-16-CCITT (poly 0x1021) over rodata bytes.  Pins a
//              shift-and-conditional-xor polynomial remainder (distinct from a
//              table-driven CRC or a hardware CRC).
//   * luhn   - Luhn check over rodata digits: double every other position,
//              cast-out nines, sum mod 10.  Pins an alternating-position digit
//              transform (distinct from any plain accumulation).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress137RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress137RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress137RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress137RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress137RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress137RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress137RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress137RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress137TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Adler-32: dual running sums reduced mod 65521 over rodata bytes.
    {p+"_adler",
     "static const unsigned char "+p+"_data[32]={\n"
     "0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14, 0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28,\n"
     "0x71,0xca,0x35,0xec,0x53,0x0a,0x97,0x4e, 0xb3,0x18,0x6f,0xd4,0x21,0x88,0xff,0x5c};\n"
     +t+" "+p+"_adler("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A=1u, B=0u;\n"
     "    for(int i=0;i<32;i++){ A=(A+("+p+"_data[i]^((s>>(i&7))&1u)))%65521u;\n"
     "      B=(B+A)%65521u; acc=acc*131u+B; }\n"
     "    acc=acc*131u+((B<<16)|A); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Fu}, "OptStress137", 2},

    // bit-serial CRC-16-CCITT (poly 0x1021) over rodata bytes.
    {p+"_crc16",
     "static const unsigned char "+p+"_msg[24]={\n"
     "0x31,0x41,0x59,0x26,0x53,0x58,0x97,0x93, 0x23,0x84,0x62,0x64,0x33,0x83,0x27,0x95,\n"
     "0x02,0x88,0x41,0x97,0x16,0x93,0x99,0x37};\n"
     +t+" "+p+"_crc16("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, crc=0xFFFFu;\n"
     "    for(int i=0;i<24;i++){ unsigned b="+p+"_msg[i]^((s>>(i&7))&1u); crc^=(b<<8);\n"
     "      for(int k=0;k<8;k++){ if(crc&0x8000u) crc=((crc<<1)^0x1021u)&0xFFFFu;\n"
     "        else crc=(crc<<1)&0xFFFFu; } acc=acc*131u+crc; }\n"
     "    acc=acc*131u+crc; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x57u}, "OptStress137", 2},

    // Luhn check over rodata digits: double alternate positions, cast out nines.
    {p+"_luhn",
     "static const unsigned char "+p+"_dig[20]={\n"
     "4,9,2,7,1,6,3,8, 5,0,7,2,9,4,1,6, 3,8,5,0};\n"
     +t+" "+p+"_luhn("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int start=0;start<8;start++){ unsigned sum=0u; unsigned parity=(s>>(start&7))&1u;\n"
     "      for(int i=0;i<16;i++){ unsigned d=("+p+"_dig[(start+i)%20]+((s>>(i&7))&1u))%10u;\n"
     "        if(((unsigned)i&1u)==parity){ d*=2u; if(d>9u) d-=9u; } sum+=d; }\n"
     "      acc=acc*131u+sum+(sum%10u==0u?1u:0u); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Du}, "OptStress137", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress137TC("x64o137", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress137TC("x86o137", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress137TC("a64o137", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress137TC("armo137", "int");

INSTANTIATE_TEST_SUITE_P(OptStress137, X64OptStress137RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress137, X86OptStress137RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress137, A64OptStress137RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress137, ARM32OptStress137RT, ::testing::ValuesIn(kARM), rtTCName);
