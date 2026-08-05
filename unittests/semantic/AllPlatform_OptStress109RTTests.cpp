//===- AllPlatform_OptStress109RTTests.cpp - sort / poly / RLE rodata shapes =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * csort  - counting sort of a rodata byte buffer over a 16-symbol alphabet:
//              a stack histogram (index-RMW), an in-place prefix-sum turning
//              counts into offsets, then a stable scatter into a stack order
//              array.  Pins rodata read -> histogram -> prefix-sum -> scatter.
//   * horner - Horner polynomial evaluation of a rodata coefficient table at
//              runtime points (`h=h*x+coef[i]`).  Pins a forward coefficient
//              sweep in a multiply-accumulate recurrence over rodata.
//   * rle    - run-length encoding of a rodata buffer (8-symbol alphabet): a
//              variable-length forward advance that coalesces equal adjacent
//              elements (`while(buf[i+run]==v) run++`).  Pins data-driven
//              adjacent-equality runs over rodata.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress109RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress109RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress109RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress109RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress109RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress109RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress109RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress109RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress109TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // counting sort: rodata histogram -> prefix-sum offsets -> stable scatter.
    {p+"_csort",
     "static const unsigned char "+p+"_data[40]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d, 0xc9,0x60,0xf5,0x17,0xab,0x3e,0x70,0x9c,\n"
     "0x46,0xd1,0x25,0xb8,0x6f,0x0a,0x93,0x57};\n"
     +t+" "+p+"_csort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[16]; for(int i=0;i<16;i++) cnt[i]=0;\n"
     "    for(int i=0;i<40;i++) cnt[("+p+"_data[i]^(s>>(i&7)))&15u]++;\n"
     "    unsigned pre=0; for(int i=0;i<16;i++){ unsigned c=cnt[i]; cnt[i]=pre; pre+=c; }\n"
     "    unsigned char ord[40];\n"
     "    for(int i=0;i<40;i++){ unsigned k=("+p+"_data[i]^(s>>(i&7)))&15u; ord[cnt[k]++]=(unsigned char)k; }\n"
     "    for(int i=0;i<40;i++) acc=acc*131u+ord[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC5u}, "OptStress109", 2},

    // Horner polynomial evaluation of a rodata coefficient table.
    {p+"_horner",
     "static const unsigned char "+p+"_coef[12]={7,3,11,2,9,5,14,1,6,12,4,10};\n"
     +t+" "+p+"_horner("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<24;q++){ unsigned x=(s>>(q&15))&0x1Fu, h=0;\n"
     "      for(int i=0;i<12;i++) h=h*x+"+p+"_coef[i];\n"
     "      acc=acc*131u+h; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x07u}, "OptStress109", 2},

    // run-length encoding over a rodata buffer (8-symbol alphabet, variable run).
    {p+"_rle",
     "static const unsigned char "+p+"_buf[48]={\n"
     "0x04,0x05,0x07,0x1c,0x1d,0x1f,0x20,0x21, 0x23,0x40,0x41,0x43,0x44,0x45,0x60,0x61,\n"
     "0x62,0x80,0x83,0x84,0x85,0x86,0x87,0xa0, 0xa1,0xc0,0xc1,0xc2,0xc3,0xe0,0xe1,0xe3,\n"
     "0x04,0x06,0x24,0x25,0x26,0x44,0x46,0x47, 0x64,0x65,0x84,0x85,0xa4,0xc4,0xe4,0xe5};\n"
     +t+" "+p+"_rle("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned i=0;\n"
     "    while(i<48u){ unsigned v=("+p+"_buf[i]>>5)&7u, run=1u;\n"
     "      while(i+run<48u && (("+p+"_buf[i+run]>>5)&7u)==v) run++;\n"
     "      acc=acc*131u+(v<<8)+run; i+=run; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Eu}, "OptStress109", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress109TC("x64o109", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress109TC("x86o109", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress109TC("a64o109", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress109TC("armo109", "int");

INSTANTIATE_TEST_SUITE_P(OptStress109, X64OptStress109RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress109, X86OptStress109RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress109, A64OptStress109RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress109, ARM32OptStress109RT, ::testing::ValuesIn(kARM), rtTCName);
