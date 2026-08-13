//===- AllPlatform_OptStress118RTTests.cpp - CORDIC / IIR / mode shapes ----==//
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
//   * cordic - CORDIC vector rotation toward a rodata-seeded target angle: a
//              shift-add iteration steered by a rodata arctan table `atan[k]`
//              (multiply-free, divide-free).  Pins a coupled shift-add rotation
//              with a per-step rodata angle read + direction decision.
//   * iir    - second-order (biquad) IIR filter over a rodata signal with rodata
//              (signed) coefficients: a feedback recurrence `y=b0*x+...-a1*y1-
//              a2*y2`.  Pins a recursive filter with carried state (distinct from
//              the feed-forward FIR).
//   * wmode  - sliding-window statistical mode over a rodata buffer: a per-window
//              16-bin histogram + arg-max.  Pins a windowed histogram followed by
//              an arg-max scan (distinct from full counting sort / median).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress118RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress118RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress118RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress118RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress118RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress118RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress118RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress118RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress118TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // CORDIC vector rotation steered by a rodata arctan table (shift-add only).
    {p+"_cordic",
     "static const unsigned char "+p+"_atan[12]={45,27,14,7,4,2,1,1,1,1,1,1};\n"
     "static const unsigned char "+p+"_seed[24]={\n"
     "60,3,130,7,20,1,200,4, 95,6,150,2,40,5,170,0, 110,7,30,3,180,1,75,6};\n"
     +t+" "+p+"_cordic("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<12;q++){ int x=64, y=0;\n"
     "      int z=(int)"+p+"_seed[q*2]-128+(int)((s>>4)&7u);\n"
     "      for(int k=0;k<12;k++){ int d=(z>=0)?1:-1;\n"
     "        int nx=x-d*(y>>k); int ny=y+d*(x>>k); z-=d*(int)"+p+"_atan[k]; x=nx; y=ny; }\n"
     "      acc=acc*131u+(unsigned)(x&0xFFFF)+(unsigned)(y&0xFFFF); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC0u}, "OptStress118", 2},

    // biquad IIR filter over a rodata signal with rodata signed coefficients.
    {p+"_iir",
     "static const signed char "+p+"_co[5]={51,102,51,-92,43};\n"
     "static const unsigned char "+p+"_x[40]={\n"
     "5,12,30,18,7,44,21,9, 33,16,52,3,27,14,40,8, 19,38,2,25,11,47,6,31,\n"
     "13,49,20,4,37,10,55,17, 29,41,1,26,53,22,15,48};\n"
     +t+" "+p+"_iir("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int b0="+p+"_co[0], b1="+p+"_co[1], b2="+p+"_co[2], a1="+p+"_co[3], a2="+p+"_co[4];\n"
     "    int x1=0,x2=0,y1=0,y2=0;\n"
     "    for(int i=0;i<40;i++){ int x=(int)("+p+"_x[i]^((s>>(i&7))&3u));\n"
     "      int y=(b0*x+b1*x1+b2*x2 - a1*y1 - a2*y2)>>8;\n"
     "      x2=x1; x1=x; y2=y1; y1=y; acc=acc*131u+(unsigned)(y&0xFFFF); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x11u}, "OptStress118", 2},

    // sliding-window statistical mode over a rodata buffer (histogram + argmax).
    {p+"_wmode",
     "static const unsigned char "+p+"_data[48]={\n"
     "0x3a,0x91,0x47,0xe5,0x12,0x8d,0x5b,0xc3, 0x29,0xf6,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d, 0xc9,0x67,0xf5,0x14,0xab,0x3e,0x70,0x9c,\n"
     "0x46,0xd1,0x25,0xb8,0x6f,0x0a,0x93,0x57, 0xe4,0x1b,0x88,0x32,0xcd,0x60,0xf1,0x07};\n"
     +t+" "+p+"_wmode("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i+8<=48;i++){ unsigned cnt[16]; for(int k=0;k<16;k++) cnt[k]=0u;\n"
     "      for(int j=0;j<8;j++) cnt[("+p+"_data[i+j]^(s>>(j&7)))&15u]++;\n"
     "      unsigned best=0u, bestc=0u;\n"
     "      for(int k=0;k<16;k++) if(cnt[k]>bestc){ bestc=cnt[k]; best=(unsigned)k; }\n"
     "      acc=acc*131u+(best<<4)+bestc; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Du}, "OptStress118", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress118TC("x64o118", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress118TC("x86o118", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress118TC("a64o118", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress118TC("armo118", "int");

INSTANTIATE_TEST_SUITE_P(OptStress118, X64OptStress118RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress118, X86OptStress118RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress118, A64OptStress118RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress118, ARM32OptStress118RT, ::testing::ValuesIn(kARM), rtTCName);
