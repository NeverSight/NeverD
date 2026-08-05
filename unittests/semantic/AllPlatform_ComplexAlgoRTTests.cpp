//===- AllPlatform_ComplexAlgoRTTests.cpp - complex algorithm RT ---*- C++ -*-===//
//
// Realistic algorithm roundtrip tests that combine many instructions and
// stress stack-local arrays, nested loops, and complex addressing.  These
// exercise the whole pipeline holistically and are high-yield for exposing
// interaction bugs in stack analysis (bugs #49/#51/#55), loop SSA, and
// memory addressing.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ComplexAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ComplexAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64ComplexAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ComplexAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ComplexAlgoRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ComplexAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// `T` is the integer type (long for 64-bit arches, int for ARM32).
static std::vector<RoundTripTC> makeAlgoTC(const char *prefix, const char *T,
                                           int opt) {
  std::string p = prefix, t = T;
  return {
    // Bubble sort of an 8-element stack array seeded from `a`, return sum of
    // the two smallest -> exercises array load/store, swaps, nested loops.
    {p+"_bubble_min2",
     t+" "+p+"_bubble_min2("+t+" a) {\n"
     "  "+t+" v[8];\n"
     "  for (int i = 0; i < 8; i++) v[i] = ((a >> (i*3)) & 0xFF) ^ (i*7);\n"
     "  for (int i = 0; i < 8; i++)\n"
     "    for (int j = 0; j < 7-i; j++)\n"
     "      if (v[j] > v[j+1]) { "+t+" tmp=v[j]; v[j]=v[j+1]; v[j+1]=tmp; }\n"
     "  return v[0] + v[1];\n"
     "}\n",
     {0x123456789ABCDEFULL}, "ComplexAlgo", opt},

    // Binary search in a sorted stack array.
    {p+"_binsearch",
     t+" "+p+"_binsearch("+t+" key) {\n"
     "  "+t+" v[10]; for (int i=0;i<10;i++) v[i]=i*i;\n"
     "  int lo=0, hi=9, found=-1;\n"
     "  while (lo<=hi) { int mid=(lo+hi)/2;\n"
     "    if (v[mid]==key) {found=mid;break;}\n"
     "    else if (v[mid]<key) lo=mid+1; else hi=mid-1; }\n"
     "  return found;\n"
     "}\n",
     {49}, "ComplexAlgo", opt},

    // FNV-1a hash over the bytes of the argument.  Iterate exactly
    // sizeof(a) bytes so the shift count never reaches the type width
    // (avoids C shift-UB on 32-bit ARM where `a >> 32` is undefined).
    {p+"_fnv1a",
     "unsigned "+t+" "+p+"_fnv1a("+t+" a) {\n"
     "  unsigned "+t+" h = 2166136261u;\n"
     "  for (int i = 0; i < (int)sizeof(a); i++) {\n"
     "    unsigned char b = (unsigned char)(a >> (i*8));\n"
     "    h ^= b; h *= 16777619u;\n"
     "  }\n"
     "  return h & 0xFFFFFFFFu;\n"
     "}\n",
     {0xDEADBEEFCAFEBABEULL}, "ComplexAlgo", opt},

    // CRC8 over the argument bytes (bit-by-bit polynomial division).
    {p+"_crc8",
     t+" "+p+"_crc8("+t+" a) {\n"
     "  unsigned char crc = 0;\n"
     "  for (int i = 0; i < (int)sizeof(a); i++) {\n"
     "    crc ^= (unsigned char)(a >> (i*8));\n"
     "    for (int b = 0; b < 8; b++)\n"
     "      crc = (crc & 0x80) ? (unsigned char)((crc<<1)^0x07) : (unsigned char)(crc<<1);\n"
     "  }\n"
     "  return crc;\n"
     "}\n",
     {0x1122334455667788ULL}, "ComplexAlgo", opt},

    // Run-length: count distinct adjacent runs in a derived array.
    {p+"_rle_runs",
     t+" "+p+"_rle_runs("+t+" a) {\n"
     "  unsigned char v[12];\n"
     "  for (int i=0;i<12;i++) v[i]=(unsigned char)((a>>(i*2))&0x3);\n"
     "  int runs=1;\n"
     "  for (int i=1;i<12;i++) if (v[i]!=v[i-1]) runs++;\n"
     "  return runs;\n"
     "}\n",
     {0x0F0F0F0F0F0F0F0FULL}, "ComplexAlgo", opt},

    // Integer square root via Newton-ish bit method.
    {p+"_isqrt",
     "unsigned "+t+" "+p+"_isqrt(unsigned "+t+" a) {\n"
     "  unsigned "+t+" x=a, res=0, bit=1;\n"
     "  while (bit*4 <= x && bit*4 > bit) bit *= 4;\n"
     "  while (bit != 0) {\n"
     "    if (x >= res+bit) { x -= res+bit; res = (res>>1)+bit; }\n"
     "    else res >>= 1;\n"
     "    bit >>= 2;\n"
     "  }\n"
     "  return res;\n"
     "}\n",
     {1000000}, "ComplexAlgo", opt},

    // Dot product of two derived arrays accumulated wide.
    {p+"_dotprod",
     t+" "+p+"_dotprod("+t+" a, "+t+" b) {\n"
     "  int x[6], y[6];\n"
     "  for (int i=0;i<6;i++){ x[i]=(int)((a>>(i*4))&0xF)-8; y[i]=(int)((b>>(i*4))&0xF)-8; }\n"
     "  "+t+" acc=0;\n"
     "  for (int i=0;i<6;i++) acc += (long long)x[i]*y[i];\n"
     "  return acc;\n"
     "}\n",
     {0x123456, 0x654321}, "ComplexAlgo", opt},

    // Insertion sort, return the median element.
    {p+"_median",
     t+" "+p+"_median("+t+" a) {\n"
     "  "+t+" v[7];\n"
     "  for (int i=0;i<7;i++) v[i]=((a>>(i*5))&0x1F)*3 - 20;\n"
     "  for (int i=1;i<7;i++){ "+t+" k=v[i]; int j=i-1;\n"
     "    while(j>=0 && v[j]>k){ v[j+1]=v[j]; j--; } v[j+1]=k; }\n"
     "  return v[3];\n"
     "}\n",
     {0x7FFFFFFFULL}, "ComplexAlgo", opt},
  };
}

static const std::vector<RoundTripTC> kX64Algo = makeAlgoTC("x64ca", "long", 2);
static const std::vector<RoundTripTC> kA64Algo = makeAlgoTC("a64ca", "long", 2);
static const std::vector<RoundTripTC> kARM32Algo = makeAlgoTC("armca", "int", 2);

// clang-format on

INSTANTIATE_TEST_SUITE_P(ComplexAlgo, X64ComplexAlgoRT,
                         ::testing::ValuesIn(kX64Algo), rtTCName);
INSTANTIATE_TEST_SUITE_P(ComplexAlgo, A64ComplexAlgoRT,
                         ::testing::ValuesIn(kA64Algo), rtTCName);
INSTANTIATE_TEST_SUITE_P(ComplexAlgo, ARM32ComplexAlgoRT,
                         ::testing::ValuesIn(kARM32Algo), rtTCName);
