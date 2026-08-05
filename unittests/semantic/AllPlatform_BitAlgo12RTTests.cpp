//===- AllPlatform_BitAlgo12RTTests.cpp - narrow scalar algos ---*- C++ -*-===//
//
// Twelfth batch of clang -O2 algorithm probes.  Targets NARROW-type (u8/u16)
// scalar code with branch-heavy control flow: internet-checksum carry folding,
// bit-by-bit CRC, saturating accumulation, branchless abs/min/max, conditional
// bit set/clear/toggle, nibble-histogram scatter, FNV byte hashing, and a
// bubble-sort pass.  These stress sub-register aliasing (movzx of u8/u16 into
// 32-bit accumulators), flag folding through conditionals, indexed scatter
// stores, and loop-carried narrow state — historically the densest sources of
// optimizer / lift bugs.
//
// Every function loops over inputs so all paths run, and folds to an exact
// integer return value.  Internal arithmetic stays 32-bit (no 64-bit
// multiply/divide that would lower to a runtime library call).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitAlgo12RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitAlgo12RT, Verify) { roundTripX64(GetParam()); }

class A64BitAlgo12RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitAlgo12RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32BitAlgo12RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitAlgo12RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeBit12TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Internet-checksum style: sum 16-bit words, fold carries, complement.
    {p+"_checksum",
     t+" "+p+"_checksum("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<48;i++){\n"
     "    unsigned short buf[16];\n"
     "    for (int j=0;j<16;j++) buf[j]=(unsigned short)(a*(i+1)+j*7);\n"
     "    unsigned sum=0;\n"
     "    for (int j=0;j<16;j++) sum+=buf[j];\n"
     "    while (sum>>16) sum=(sum&0xFFFFu)+(sum>>16);\n"
     "    unsigned short cs=(unsigned short)~sum;\n"
     "    s += (int)cs - i; }\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "BitAlgo12", opt, fl},

    // Bit-by-bit CRC16 (no lookup table): conditional feedback per bit.
    {p+"_crc",
     t+" "+p+"_crc("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned crc=0xFFFFu;\n"
     "    unsigned char data=(unsigned char)(a+i);\n"
     "    crc ^= data;\n"
     "    for (int b=0;b<8;b++){ if (crc&1u) crc=(crc>>1)^0xA001u; else crc>>=1; }\n"
     "    s += (int)(crc&0xFFFFu) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "BitAlgo12", opt, fl},

    // Saturating u8 accumulate: clamp running sum to 255.
    {p+"_satu8",
     t+" "+p+"_satu8("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<64;i++){\n"
     "    unsigned acc=0;\n"
     "    for (int j=0;j<8;j++){ unsigned v=(unsigned char)(a+i*3+j*11);\n"
     "      acc+=v; if (acc>255u) acc=255u; }\n"
     "    s += (int)acc - i; }\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "BitAlgo12", opt, fl},

    // Branchless abs + min/max chain on signed 16-bit-range values.
    {p+"_absmm",
     t+" "+p+"_absmm("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<96;i++){\n"
     "    int x=((int)((unsigned)a+i)&0xFFFF)-0x8000;\n"
     "    int y=((int)((unsigned)a*3+i)&0xFFFF)-0x8000;\n"
     "    int ax=x<0?-x:x; int mn=x<y?x:y; int mx=x>y?x:y;\n"
     "    s += ax + (mx-mn) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "BitAlgo12", opt, fl},

    // Conditional bit set/clear/toggle, loop-carried bitboard.
    {p+"_bitboard",
     t+" "+p+"_bitboard("+t+" a) {\n"
     "  unsigned board=(unsigned)a; int s=0;\n"
     "  for (int i=0;i<96;i++){\n"
     "    int bit=i&31;\n"
     "    if (i&1) board|=(1u<<bit);\n"
     "    else if (i&2) board&=~(1u<<bit);\n"
     "    else board^=(1u<<bit);\n"
     "    s += (int)((board>>(i&7))&0xFFu) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "BitAlgo12", opt, fl},

    // Nibble histogram: scatter-increment an array indexed by each nibble.
    {p+"_nibhist",
     t+" "+p+"_nibhist("+t+" a) {\n"
     "  int h[16]; for(int k=0;k<16;k++) h[k]=0;\n"
     "  for (int i=0;i<128;i++){ unsigned v=(unsigned)a*(unsigned)(i+1);\n"
     "    for (int n=0;n<8;n++) h[(v>>(n*4))&0xFu]++; }\n"
     "  int s=0; for(int k=0;k<16;k++) s += h[k]*(k+1);\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "BitAlgo12", opt, fl},

    // FNV-1a byte hash: u8 xor into 32-bit state then multiply.
    {p+"_fnv",
     t+" "+p+"_fnv("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<48;i++){\n"
     "    unsigned h=2166136261u;\n"
     "    for (int j=0;j<8;j++){ unsigned char b=(unsigned char)(a+i+j*31);\n"
     "      h^=b; h*=16777619u; }\n"
     "    s += (int)(h>>13) - i; }\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "BitAlgo12", opt, fl},

    // Bubble-sort pass over a small stack array: conditional swap.
    {p+"_bubble",
     t+" "+p+"_bubble("+t+" a) {\n"
     "  int arr[12];\n"
     "  for (int k=0;k<12;k++) arr[k]=(int)((unsigned)a*(unsigned)(k+1)+k*7)&0xFFFF;\n"
     "  for (int pp=0;pp<12;pp++)\n"
     "    for (int k=0;k<11;k++)\n"
     "      if (arr[k]>arr[k+1]){ int tmp=arr[k]; arr[k]=arr[k+1]; arr[k+1]=tmp; }\n"
     "  int s=0; for(int k=0;k<12;k++) s += arr[k]*(k+1);\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "BitAlgo12", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Bit12 =
    makeBit12TC("x64b12", "long", 2, "");
static const std::vector<RoundTripTC> kA64Bit12 =
    makeBit12TC("a64b12", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Bit12 =
    makeBit12TC("armb12", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(BitAlgo12, X64BitAlgo12RT,
                         ::testing::ValuesIn(kX64Bit12), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitAlgo12, A64BitAlgo12RT,
                         ::testing::ValuesIn(kA64Bit12), rtTCName);
INSTANTIATE_TEST_SUITE_P(BitAlgo12, ARM32BitAlgo12RT,
                         ::testing::ValuesIn(kARM32Bit12), rtTCName);
