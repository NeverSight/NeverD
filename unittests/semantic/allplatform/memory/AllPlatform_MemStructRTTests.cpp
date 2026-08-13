//===- AllPlatform_MemStructRTTests.cpp - memory / struct addressing -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing of memory addressing the arithmetic-focused
// probes do not reach: mixed-width struct field load/store, array-of-struct
// traversal, index-based pointer chasing, strided 2D access, and byte-buffer
// reinterpretation (sub-word loads recombined into wider values).  These stress
// LowToMed effective-address computation, load/store width handling and the
// stack-slot / GEP recovery in the emitter.  Buffers stay on the stack and small
// enough for the bare-metal harness.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MsRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MsRT, Verify) { roundTripX64(GetParam()); }
class X86MsRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MsRT, Verify) { roundTripX86(GetParam()); }
class A64MsRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MsRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32MsRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MsRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeMsTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Mixed-width struct fields: byte/short/int/long stored and reloaded,
    // exercising sub-word store + widening reload off one base.
    {p+"_struct",
     t+" "+p+"_struct("+t+" a){\n"
     "  struct S{ unsigned char b; short h; unsigned w; long long q; signed char sb; } s;\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ s.b=(unsigned char)(acc+i); s.h=(short)(acc>>3);\n"
     "    s.w=acc*2654435761u; s.q=(long long)acc*-7+i; s.sb=(signed char)(acc>>5);\n"
     "    acc=acc*31u+s.b+(unsigned)s.h+s.w+(unsigned)s.q+(unsigned)(int)s.sb; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x33ULL}, "Ms", 2},

    // Array-of-struct traversal with field-wise accumulation.
    {p+"_aos",
     t+" "+p+"_aos("+t+" a){\n"
     "  struct P{ int x; int y; unsigned char tag; } arr[8];\n"
     "  for(int i=0;i<8;i++){ arr[i].x=(int)((unsigned)a*(i+1)); arr[i].y=(int)(a>>i);\n"
     "    arr[i].tag=(unsigned char)(a+i*7); }\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<8;i++)for(int j=0;j<8;j++){\n"
     "    int dx=arr[i].x-arr[j].x, dy=arr[i].y-arr[j].y;\n"
     "    acc=acc*131u+(unsigned)(dx*dx+dy*dy)+arr[i].tag; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ULL}, "Ms", 2},

    // Index-based pointer chasing (next index stored in the array).
    {p+"_chase",
     t+" "+p+"_chase("+t+" a){\n"
     "  unsigned nxt[16], val[16];\n"
     "  for(int i=0;i<16;i++){ nxt[i]=((unsigned)a+(unsigned)i*5u+3u)&15u; val[i]=(unsigned)a*(unsigned)(i+1); }\n"
     "  unsigned acc=0, idx=(unsigned)a&15u;\n"
     "  for(int step=0;step<40;step++){ acc=acc*31u+val[idx]; idx=nxt[idx]; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x5ULL}, "Ms", 2},

    // Strided 2D access over a flat buffer (row*stride+col addressing).
    {p+"_stride2d",
     t+" "+p+"_stride2d("+t+" a){\n"
     "  unsigned m[6*5];\n"
     "  for(int r=0;r<6;r++)for(int c=0;c<5;c++) m[r*5+c]=(unsigned)a*(r+1)+(unsigned)c*7u;\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int c=0;c<5;c++)for(int r=0;r<6;r++){ unsigned v=m[r*5+c];\n"
     "    acc=acc*31u+v+(r>0?m[(r-1)*5+c]:0u); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0xCULL}, "Ms", 2},

    // Byte-buffer reinterpretation: write bytes, read back as short/int — sub-
    // word loads recombined, the endianness-sensitive shape.
    {p+"_bytes",
     t+" "+p+"_bytes("+t+" a){\n"
     "  unsigned char buf[32];\n"
     "  for(int i=0;i<32;i++) buf[i]=(unsigned char)((unsigned)a*(i+1)+i*13);\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<29;i++){ unsigned b=buf[i];\n"
     "    unsigned h=(unsigned)buf[i]|((unsigned)buf[i+1]<<8);\n"
     "    unsigned w=(unsigned)buf[i]|((unsigned)buf[i+1]<<8)|((unsigned)buf[i+2]<<16)|((unsigned)buf[i+3]<<24);\n"
     "    acc=acc*131u+b+h*7u+w; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x41ULL}, "Ms", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeMsTC("x64ms", "long");
static const std::vector<RoundTripTC> kX86 = makeMsTC("x86ms", "int");
static const std::vector<RoundTripTC> kA64 = makeMsTC("a64ms", "long");
static const std::vector<RoundTripTC> kARM = makeMsTC("armms", "int");

INSTANTIATE_TEST_SUITE_P(Ms, X64MsRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ms, X86MsRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ms, A64MsRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ms, ARM32MsRT, ::testing::ValuesIn(kARM), rtTCName);
