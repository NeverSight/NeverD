//===- AllPlatform_VectorAlgo32RTTests.cpp - explicit packed-SIMD probes --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Thirty-second batch: instead of relying on the autovectorizer, these kernels
// use GCC vector extensions plus clang's __builtin_shufflevector /
// __builtin_convertvector so clang *must* emit packed SIMD (narrow/widen,
// cross-lane permute, interleave, packed FMA, compare-select blend, lane
// reverse, lane broadcast).  These force the per-lane / cross-lane lift paths
// (PSHUFB/TBL, PACK/convert, PMAX/blend, DUP, REV) that the docs flag as the
// most fragile.  Each folds to one exact integer (FP via bit pattern) for a
// bit-exact original-vs-lifted comparison.
//
// x64 uses -mssse3 (PSHUFB + packed integer); a64/arm32 use default NEON.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo32RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo32RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo32RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo32RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo32RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo32RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// Shared vector typedefs + a small seeding helper, prepended to every kernel.
static const char *kVecPrelude =
  "typedef int v4si __attribute__((vector_size(16)));\n"
  "typedef short v8hi __attribute__((vector_size(16)));\n"
  "typedef short v4hi __attribute__((vector_size(8)));\n"
  "typedef unsigned char v16qi __attribute__((vector_size(16)));\n"
  "typedef float v4sf __attribute__((vector_size(16)));\n";

static std::vector<RoundTripTC> makeVec32TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags, pre = kVecPrelude;
  return {
    // Truncating narrow int32x8 -> int16x8, then horizontal xor reduce (PACK/XTN).
    {p+"_narrow",
     pre + t+" "+p+"_narrow("+t+" a) {\n"
     "  v4si x={(int)a,(int)(a*3),(int)(a*7),(int)(a*11)};\n"
     "  v4si y={(int)(a*13),(int)(a*17),(int)(a*19),(int)(a*23)};\n"
     "  v4hi nx=__builtin_convertvector(x,v4hi), ny=__builtin_convertvector(y,v4hi);\n"
     "  v8hi n={nx[0],nx[1],nx[2],nx[3],ny[0],ny[1],ny[2],ny[3]};\n"
     "  unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+(unsigned short)n[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo32", opt, fl},

    // Widening short->int multiply then lane sum (SMULL/VMULL widen path).
    {p+"_widen",
     pre + t+" "+p+"_widen("+t+" a) {\n"
     "  v4hi x={(short)a,(short)(a*5),(short)(a*9),(short)(a*15)};\n"
     "  v4hi y={(short)(a*2),(short)(a*4),(short)(a*8),(short)(a*16)};\n"
     "  v4si wx=__builtin_convertvector(x,v4si), wy=__builtin_convertvector(y,v4si);\n"
     "  v4si pr=wx*wy; long long acc=0; for(int i=0;i<4;i++) acc+=(long long)pr[i];\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo32", opt, fl},

    // Fixed-mask byte permute (PSHUFB / TBL).
    {p+"_permute",
     pre + t+" "+p+"_permute("+t+" a) {\n"
     "  v16qi v;\n"
     "  for(int i=0;i<16;i++) v[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  v16qi r=__builtin_shufflevector(v,v, 3,7,11,15, 2,6,10,14, 1,5,9,13, 0,4,8,12);\n"
     "  unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+r[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo32", opt, fl},

    // Interleave (zip) two int vectors (ZIP1/ZIP2 / PUNPCK).
    {p+"_interleave",
     pre + t+" "+p+"_interleave("+t+" a) {\n"
     "  v4si x={(int)a,(int)(a*3),(int)(a*7),(int)(a*11)};\n"
     "  v4si y={(int)(a*2),(int)(a*5),(int)(a*8),(int)(a*13)};\n"
     "  v4si lo=__builtin_shufflevector(x,y, 0,4,1,5);\n"
     "  v4si hi=__builtin_shufflevector(x,y, 2,6,3,7);\n"
     "  unsigned acc=0; for(int i=0;i<4;i++) acc=acc*131u+(unsigned)lo[i];\n"
     "  for(int i=0;i<4;i++) acc=acc*131u+(unsigned)hi[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo32", opt, fl},

    // Packed FP multiply-add a*b+c (FMLA / mulps+addps), returned as bits.
    {p+"_fma",
     pre + t+" "+p+"_fma("+t+" a) {\n"
     "  v4sf x,y,z;\n"
     "  for(int i=0;i<4;i++){ x[i]=(float)(int)(((a*(i+1))>>10)&0xFF); \n"
     "    y[i]=(float)(int)(((a*(i+3))>>12)&0x3F); z[i]=(float)(int)(((a*(i+5))>>9)&0xFF); }\n"
     "  v4sf r=x*y+z; float s=r[0]+r[1]+r[2]+r[3];\n"
     "  unsigned u; __builtin_memcpy(&u,&s,4); return ("+t+")u;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo32", opt, fl},

    // Per-lane compare-select max (PMAXSD / SMAX / VMAX via blend).
    {p+"_blendmax",
     pre + t+" "+p+"_blendmax("+t+" a) {\n"
     "  v4si x={(int)a,(int)(a*3),(int)(a*7),(int)(a*11)};\n"
     "  v4si y={(int)(a*5)-99,(int)(a*2)+7,(int)(a*9)-3,(int)(a*4)+1};\n"
     "  v4si r; for(int i=0;i<4;i++) r[i]=(x[i]>y[i])?x[i]:y[i];\n"
     "  unsigned acc=0; for(int i=0;i<4;i++) acc=acc*131u+(unsigned)r[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo32", opt, fl},

    // Lane reverse of a 4-wide int vector (REV64+EXT / shuffle).
    {p+"_revlanes",
     pre + t+" "+p+"_revlanes("+t+" a) {\n"
     "  v4si x={(int)a,(int)(a*3),(int)(a*7),(int)(a*11)};\n"
     "  v4si r=__builtin_shufflevector(x,x, 3,2,1,0);\n"
     "  unsigned acc=0; for(int i=0;i<4;i++) acc=acc*131u+(unsigned)r[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo32", opt, fl},

    // Broadcast lane 2 to all lanes (DUP / PSHUFD imm), then combine.
    {p+"_bcast",
     pre + t+" "+p+"_bcast("+t+" a) {\n"
     "  v4si x={(int)a,(int)(a*3),(int)(a*7),(int)(a*11)};\n"
     "  v4si b=__builtin_shufflevector(x,x, 2,2,2,2);\n"
     "  v4si r=x ^ b; unsigned acc=0; for(int i=0;i<4;i++) acc=acc*131u+(unsigned)r[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo32", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec32 =
    makeVec32TC("x64v32", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec32 =
    makeVec32TC("a64v32", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec32 =
    makeVec32TC("armv32", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo32, X64VectorAlgo32RT,
                         ::testing::ValuesIn(kX64Vec32), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo32, A64VectorAlgo32RT,
                         ::testing::ValuesIn(kA64Vec32), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo32, ARM32VectorAlgo32RT,
                         ::testing::ValuesIn(kARM32Vec32), rtTCName);
