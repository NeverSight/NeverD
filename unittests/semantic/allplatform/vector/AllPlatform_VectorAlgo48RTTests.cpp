//===- AllPlatform_VectorAlgo48RTTests.cpp - large constant-pool functions -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Forty-eighth batch: LARGE NEON functions whose `.text` (ARM32, with the
// constant pool embedded inline) exceeds the single-global embed cap, exercising
// #531's embedExecSegmentRun / embedRodataRun at scale.  Before the cap was
// raised to kMaxSingleGlobalEmbedLen, an 8 KB ARM32 `.text` literal pool fell
// back to ~979 overlapping per-constant copies (~2.1 MB of redundant `.rodata`),
// which blew up codegen / link / the emulator mapping; now each function's pool
// is one GEP'd global (~9 KB).  Each kernel chains many bitmask-gather / FIR /
// dot-product idioms to push `.text` well past 4 KB, folded to one exact integer.
//
// x64 uses -mssse3; a64/arm32 use the default NEON baseline.  Unsigned bitmask
// accumulators; i64-accumulate-by-+= for the widening MACs (no i64 multiply), so
// libcall-free on the 32-bit targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo48RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo48RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo48RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo48RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo48RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo48RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec48TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Six distinct bitmask-gather idioms + a constant-coefficient FIR chained in
    // one function: ARM32 `.text` ~8 KB (constant pool inline, was ~2.1 MB O(N)).
    {p+"_bigmix",
     t+" "+p+"_bigmix("+t+" a){\n"
     "  unsigned acc=0; signed char v[128]; unsigned char u[128];\n"
     "  for(int i=0;i<128;i++){ v[i]=(signed char)((a*(i+1))>>1); u[i]=(unsigned char)((a*(i+1))>>1); }\n"
     "  for(int b=0;b<4;b++){unsigned m=0;for(int i=0;i<32;i++)if(v[b*32+i]>0)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<4;b++){unsigned m=0;for(int i=0;i<32;i++)if((u[b*32+i]>>2)&1u)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<4;b++){unsigned m=0;for(int i=0;i<32;i++)if(v[b*32+i]>10)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<4;b++){unsigned m=0;for(int i=0;i<32;i++)if(u[b*32+i]>=0x80u)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<4;b++){unsigned m=0;for(int i=0;i<32;i++)if((u[b*32+i]&0xFu)==7u)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  short w[128]; for(int i=0;i<128;i++)w[i]=(short)((a*(i+3))>>2);\n"
     "  for(int b=0;b<8;b++){unsigned m=0;for(int i=0;i<16;i++)if(w[b*16+i]<0)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  short x[128]; static const short C[8]={101,-103,107,-109,113,-127,131,-137};\n"
     "  for(int i=0;i<128;i++)x[i]=(short)((a*(i+1))>>4);\n"
     "  long long d=0; for(int i=0;i<120;i++){long long s=0;for(int k=0;k<8;k++)s+=(long long)((int)x[i+k]*(int)C[k]);d+=s;}\n"
     "  acc=acc*131u+(unsigned)d; return ("+t+")acc;\n"
     "}\n",
     {0x13572468ULL}, "VectorAlgo48", opt, fl},

    // Several constant-coefficient FIR / dot products with different taps: a wide
    // coefficient pool in `.text` (ARM32) past the cap.
    {p+"_bigfir",
     t+" "+p+"_bigfir("+t+" a){\n"
     "  short x[256]; long long acc=0;\n"
     "  static const short A[8]={3,-5,7,-11,13,-17,19,-23};\n"
     "  static const short B[8]={29,-31,37,-41,43,-47,53,-59};\n"
     "  static const short C[8]={61,-67,71,-73,79,-83,89,-97};\n"
     "  static const short D[8]={101,-103,107,-109,113,-127,131,-137};\n"
     "  for(int i=0;i<256;i++) x[i]=(short)((a*(i+1))>>5);\n"
     "  for(int i=0;i<248;i++){ long long s=0;\n"
     "    for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)A[k]);\n"
     "    for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)B[k]);\n"
     "    for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)C[k]);\n"
     "    for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)D[k]);\n"
     "    acc+=s*(i+1); }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x2468ACE0ULL}, "VectorAlgo48", opt, fl},

    // Many bitmask gathers across byte and halfword widths with varied predicates
    // — a large gather pool (sign / zero / threshold / pick-bit / nibble-eq).
    {p+"_biggather",
     t+" "+p+"_biggather("+t+" a){\n"
     "  unsigned acc=0; signed char v[256]; unsigned char u[256];\n"
     "  for(int i=0;i<256;i++){ v[i]=(signed char)((a*(i+1))>>1); u[i]=(unsigned char)((a*(i+2))>>1); }\n"
     "  for(int b=0;b<8;b++){unsigned m=0;for(int i=0;i<32;i++)if(v[b*32+i]<0)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<8;b++){unsigned m=0;for(int i=0;i<32;i++)if(v[b*32+i]==0)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<8;b++){unsigned m=0;for(int i=0;i<32;i++)if((u[b*32+i]>>5)&1u)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  for(int b=0;b<8;b++){unsigned m=0;for(int i=0;i<32;i++)if((u[b*32+i]&0xFu)==3u)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  short w[256]; for(int i=0;i<256;i++)w[i]=(short)((a*(i+3))>>2);\n"
     "  for(int b=0;b<16;b++){unsigned m=0;for(int i=0;i<16;i++)if(w[b*16+i]<0)m|=(1u<<i);acc=acc*131u+m;}\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x369CF258ULL}, "VectorAlgo48", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec48 =
    makeVec48TC("x64v48", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec48 =
    makeVec48TC("a64v48", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec48 =
    makeVec48TC("armv48", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo48, X64VectorAlgo48RT,
                         ::testing::ValuesIn(kX64Vec48), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo48, A64VectorAlgo48RT,
                         ::testing::ValuesIn(kA64Vec48), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo48, ARM32VectorAlgo48RT,
                         ::testing::ValuesIn(kARM32Vec48), rtTCName);
