//===- AllPlatform_KernelAlgoRTTests.cpp - high-yield algo probes -*- C++ -*-=//
//
// clang -O2 algorithm-level "high-yield probing": each kernel compiles to a
// dense, distinct instruction mix across x64 / aarch64 / arm32, exercising many
// lowerings at once (the project's most productive bug-finding method).  Kernels
// chosen to hit patterns under-represented by the existing AllPlatform_*Algo
// suites:
//   * isqrt    - bit-by-bit integer square root (shift / compare / conditional
//                subtract loop -> csel/cmov + carry).
//   * bitonic8 - 8-element bitonic sort network (compare-exchange -> smin/smax /
//                cmov / NEON min-max).
//   * morton   - 2D bit interleave / Morton code (per-bit shift+mask spread).
//   * crc16    - reflected CRC-16 (per-bit shift/xor with conditional poly).
//   * median3  - sliding 3-tap median filter (min/max chains).
//   * b64      - base64 encode (rodata table lookup + 24-bit pack/extract).
//
// Everything is bounded 16/32-bit with constant-divisor shifts and local arrays,
// so nothing lowers to a libcall Unicorn lacks.  Algorithm correctness is
// irrelevant — only determinism matters: the harness runs native vs lifted and
// compares the folded return, so any lowering divergence surfaces as a mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64KernelAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64KernelAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64KernelAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64KernelAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32KernelAlgoRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32KernelAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeKernelTC(const char *prefix, const char *T,
                                             int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Bit-by-bit integer square root.
    {p+"_isqrt",
     t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<48;k++){\n"
     "    unsigned n=(unsigned)(a*7u+k*131u)&0xFFFFFFu;\n"
     "    unsigned res=0, bit=1u<<22;\n"
     "    while(bit>n) bit>>=2;\n"
     "    while(bit){ if(n>=res+bit){ n-=res+bit; res=(res>>1)+bit; } else res>>=1; bit>>=2; }\n"
     "    acc=acc*131u+res; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "KernelAlgo", opt, fl},

    // 8-element bitonic sort network (compare-exchange -> min/max).
    {p+"_bitonic8",
     t+" "+p+"_bitonic8("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<28;k++){\n"
     "    int v[8];\n"
     "    for(int i=0;i<8;i++) v[i]=(int)((unsigned)(a*7u+i*97u+k*13u)%200u)-100;\n"
     "    for(int s=2;s<=8;s<<=1)\n"
     "      for(int st=s>>1;st>0;st>>=1)\n"
     "        for(int i=0;i<8;i++){ int j=i^st; if(j>i){ int up=((i&s)==0);\n"
     "          int hi=v[i]>v[j]; if(hi==up){ int tmp=v[i]; v[i]=v[j]; v[j]=tmp; } } }\n"
     "    for(int i=0;i<8;i++) acc=acc*31u+(unsigned)v[i]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "KernelAlgo", opt, fl},

    // 2D Morton code (bit interleave).
    {p+"_morton",
     t+" "+p+"_morton("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<40;k++){\n"
     "    unsigned x=(unsigned)(a*(k+1))&0xFFFFu, y=(unsigned)(a*3u+k)&0xFFFFu, m=0;\n"
     "    for(int i=0;i<16;i++){ m|=((x>>i)&1u)<<(2*i); m|=((y>>i)&1u)<<(2*i+1); }\n"
     "    acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "KernelAlgo", opt, fl},

    // Reflected CRC-16 (0xA001) over a byte buffer.
    {p+"_crc16",
     t+" "+p+"_crc16("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<28;k++){\n"
     "    unsigned char buf[24];\n"
     "    for(int i=0;i<24;i++) buf[i]=(unsigned char)(a*(i+1)+k*7);\n"
     "    unsigned crc=0xFFFFu;\n"
     "    for(int i=0;i<24;i++){ crc^=buf[i];\n"
     "      for(int b=0;b<8;b++) crc=(crc&1u)?((crc>>1)^0xA001u):(crc>>1); }\n"
     "    acc=acc*131u+(crc&0xFFFFu); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "KernelAlgo", opt, fl},

    // Sliding 3-tap median filter (min/max chains).
    {p+"_median3",
     t+" "+p+"_median3("+t+" a){\n"
     "  unsigned acc=0; short x[64];\n"
     "  for(int i=0;i<64;i++) x[i]=(short)((a*(i+1))&0x7FFF)-16384;\n"
     "  for(int i=1;i<63;i++){\n"
     "    int pr=x[i-1], c=x[i], nx=x[i+1];\n"
     "    int mx=pr>c?pr:c, mn=pr<c?pr:c;\n"
     "    int med=nx>mx?mx:(nx<mn?mn:nx);\n"
     "    acc=acc*131u+(unsigned)(med&0xFFFF); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "KernelAlgo", opt, fl},
    // NOTE: the base64 kernel (hoisted rodata table indexed inside a loop while
    // the function also stores to a stack array) lives in
    // AllPlatform_RodataHoistRTTests.cpp.  It exposed a real emitter bug (table
    // redirection disabled whenever the function indexed-stored to its frame)
    // now fixed by keying redirection on the base being a read-only data symbol.
  };
}

static const std::vector<RoundTripTC> kX64K   = makeKernelTC("x64k", "long", 2, "");
static const std::vector<RoundTripTC> kA64K   = makeKernelTC("a64k", "long", 2, "");
static const std::vector<RoundTripTC> kARM32K = makeKernelTC("armk", "int",  2, "");
// clang-format on

INSTANTIATE_TEST_SUITE_P(KernelAlgo, X64KernelAlgoRT,
                         ::testing::ValuesIn(kX64K), rtTCName);
INSTANTIATE_TEST_SUITE_P(KernelAlgo, A64KernelAlgoRT,
                         ::testing::ValuesIn(kA64K), rtTCName);
INSTANTIATE_TEST_SUITE_P(KernelAlgo, ARM32KernelAlgoRT,
                         ::testing::ValuesIn(kARM32K), rtTCName);
