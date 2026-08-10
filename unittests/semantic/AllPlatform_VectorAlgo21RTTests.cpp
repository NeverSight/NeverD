//===- AllPlatform_VectorAlgo21RTTests.cpp - numeric/FP kernels -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Twenty-first batch of clang -O2 algorithm probes, weighted toward scalar and
// packed floating point (where the COMISS/SQRTSS width bugs surfaced) plus a few
// integer bit kernels.  Each kernel folds its results — float results via their
// exact IEEE bit pattern — into one integer so any native-vs-lifted lowering
// divergence (scalar-vs-double misinfer, lane drop, gather miscompile) shows up:
//   * fnorm     - L2 norm of a vector (sum of squares + sqrtf, bit-hash).
//   * fvariance - streaming mean + variance over a float array.
//   * fdot      - dot product of two float arrays (FMA/mulps+addps).
//   * fpolyh    - Horner polynomial evaluation in float.
//   * fminmax   - float min/max scan keeping the bit pattern of each.
//   * frecip    - integer Newton-Raphson reciprocal (no FP libcall).
//   * bitrev    - bit-reversal permutation (shift/mask reassembly).
//   * hamming   - XOR + popcount Hamming-distance accumulation.
//   * clampacc  - saturating accumulate with branchy clamps.
//   * median3   - median-of-three sliding filter (compare/select tree).
//
// FP work uses -fno-math-errno so sqrtf lowers to a bare sqrt instruction (no
// errno-setting libcall branch); all divisors are runtime-nonzero and results
// stay finite, so nothing lowers to a libcall Unicorn lacks.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo21RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo21RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo21RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo21RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo21RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo21RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA21TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // L2 norm: sum of squares then sqrtf, return the result's IEEE bits.
    {p+"_fnorm",
     t+" "+p+"_fnorm("+t+" a){\n"
     "  float v[48]; for(int i=0;i<48;i++) v[i]=(float)((a+i*7)%97)-48.0f;\n"
     "  float s=0; for(int i=0;i<48;i++) s+=v[i]*v[i];\n"
     "  float r=__builtin_sqrtf(s);\n"
     "  unsigned o; __builtin_memcpy(&o,&r,4); return ("+t+")o;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo21", opt, fl},

    // Streaming mean + variance (two-pass), hash both float bit patterns.
    {p+"_fvariance",
     t+" "+p+"_fvariance("+t+" a){\n"
     "  float v[64]; for(int i=0;i<64;i++) v[i]=(float)((a*(i+1))%211)-105.0f;\n"
     "  float mean=0; for(int i=0;i<64;i++) mean+=v[i]; mean/=64.0f;\n"
     "  float var=0; for(int i=0;i<64;i++){ float d=v[i]-mean; var+=d*d; }\n"
     "  var/=64.0f;\n"
     "  unsigned a0,a1; __builtin_memcpy(&a0,&mean,4); __builtin_memcpy(&a1,&var,4);\n"
     "  return ("+t+")(a0*131u+a1);\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo21", opt, fl},

    // Dot product of two float arrays (mulps + horizontal add / FMA).
    {p+"_fdot",
     t+" "+p+"_fdot("+t+" a){\n"
     "  float x[40],y[40];\n"
     "  for(int i=0;i<40;i++){ x[i]=(float)((a+i)%53)-26.0f; y[i]=(float)((a*3+i*5)%61)-30.0f; }\n"
     "  float s=0; for(int i=0;i<40;i++) s+=x[i]*y[i];\n"
     "  unsigned o; __builtin_memcpy(&o,&s,4); return ("+t+")o;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo21", opt, fl},

    // Horner polynomial evaluation in float (chained mul+add).
    {p+"_fpolyh",
     t+" "+p+"_fpolyh("+t+" a){\n"
     "  float c[8]; for(int i=0;i<8;i++) c[i]=(float)(((a>>i)&7)+1);\n"
     "  float x=(float)((a%7)+1)/4.0f, acc=0;\n"
     "  for(int i=0;i<8;i++) acc=acc*x+c[i];\n"
     "  unsigned o; __builtin_memcpy(&o,&acc,4); return ("+t+")o;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo21", opt, fl},

    // Float min/max scan; combine the bit patterns of both extremes.
    {p+"_fminmax",
     t+" "+p+"_fminmax("+t+" a){\n"
     "  float v[50]; for(int i=0;i<50;i++) v[i]=(float)((a*(i+3))%337)-168.0f;\n"
     "  float mn=v[0],mx=v[0];\n"
     "  for(int i=1;i<50;i++){ if(v[i]<mn)mn=v[i]; if(v[i]>mx)mx=v[i]; }\n"
     "  unsigned a0,a1; __builtin_memcpy(&a0,&mn,4); __builtin_memcpy(&a1,&mx,4);\n"
     "  return ("+t+")(a0^(a1*2654435761u));\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo21", opt, fl},

    // Integer Newton-Raphson reciprocal (Q16 fixed point), no FP at all.
    {p+"_frecip",
     t+" "+p+"_frecip("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int k=1;k<40;k++){ unsigned d=(unsigned)((a%251)+k)|1u;\n"
     "    unsigned x=(1u<<16)/d;\n"
     "    for(int it=0;it<3;it++){ unsigned long long t=(unsigned long long)x*((2u<<16)-d*x);\n"
     "      x=(unsigned)(t>>16); }\n"
     "    acc=acc*131u+x; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo21", opt, fl},

    // Bit-reversal permutation of a byte, accumulated.
    {p+"_bitrev",
     t+" "+p+"_bitrev("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ unsigned v=(unsigned)(a+i)&0xFF, r=0;\n"
     "    for(int b=0;b<8;b++) r=(r<<1)|((v>>b)&1u);\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo21", opt, fl},

    // Hamming distance accumulation via XOR + popcount.
    {p+"_hamming",
     t+" "+p+"_hamming("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ unsigned x=(unsigned)(a*(i+1)), y=(unsigned)(a+i*131);\n"
     "    unsigned d=x^y; d=d-((d>>1)&0x55555555u);\n"
     "    d=(d&0x33333333u)+((d>>2)&0x33333333u);\n"
     "    d=(d+(d>>4))&0x0F0F0F0Fu; d=(d*0x01010101u)>>24;\n"
     "    acc+=d; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo21", opt, fl},

    // Saturating accumulate with branchy clamps to [-32768, 32767].
    {p+"_clampacc",
     t+" "+p+"_clampacc("+t+" a){\n"
     "  int acc=0; unsigned h=0;\n"
     "  for(int i=0;i<120;i++){ int d=(int)((a*(i+1))%1024)-512;\n"
     "    acc+=d; if(acc>32767)acc=32767; if(acc<-32768)acc=-32768;\n"
     "    h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo21", opt, fl},

    // Median-of-three sliding filter (compare/select tree).
    {p+"_median3",
     t+" "+p+"_median3("+t+" a){\n"
     "  int v[130]; for(int i=0;i<130;i++) v[i]=(int)((a*(i+1)+i*7)&0x3FF);\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ int x=v[i],y=v[i+1],z=v[i+2],m;\n"
     "    if(x>y){int t=x;x=y;y=t;} if(y>z){y=z;} m=x>y?x:y;\n"
     "    acc=acc*131u+(unsigned)m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo21", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeVA21TC("x64v21", "long", 2, "-fno-math-errno");
static const std::vector<RoundTripTC> kA64 =
    makeVA21TC("a64v21", "long", 2, "-fno-math-errno");
static const std::vector<RoundTripTC> kARM =
    makeVA21TC("armv21", "int", 2, "-fno-math-errno");

INSTANTIATE_TEST_SUITE_P(VectorAlgo21, X64VectorAlgo21RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo21, A64VectorAlgo21RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo21, ARM32VectorAlgo21RT,
                         ::testing::ValuesIn(kARM), rtTCName);
