//===- AllPlatform_VectorAlgo47RTTests.cpp - const-pool × control-flow ----===//
//
// Forty-seventh batch of clang -O2 vector probes covering the INTERSECTION of a
// constant-pool-dense NEON region and a switch jump table within one function.
// On ARM32 both the NEON constant pool and the PC-relative switch offset table
// live in the executable `.text`; #531 rebuilds the pool as one GEP'd global via
// embedExecSegmentRun while the jump-table resolver independently recovers the
// switch — this batch verifies the two coexist (neither the pool's single-global
// rebuild nor the switch recovery corrupts the other).  Earlier batches probed
// pure-SIMD or pure-switch functions but never their mix.
//
// Each kernel folds to one exact integer for a bit-exact compare.  Unsigned
// arithmetic keeps the scalar wraparound well-defined; bitmask gather stays in a
// u32 accumulator.  x64 uses -mssse3; a64/arm32 use the default NEON baseline.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo47RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo47RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo47RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo47RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo47RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo47RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec47TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 8-way switch accumulate, THEN a NEON sign-bit bitmask gather: the switch
    // jump table and the NEON constant pool share `.text` on ARM32.
    {p+"_swvec",
     t+" "+p+"_swvec("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int i=0;i<64;i++){ unsigned x=(unsigned)a*(unsigned)(i+1);\n"
     "    switch((x>>3)&7u){ case 0:acc+=x*7u;break; case 1:acc^=x<<2;break;\n"
     "      case 2:acc-=x>>1;break; case 3:acc+=x*13u;break; case 4:acc^=x>>3;break;\n"
     "      case 5:acc+=x<<1;break; case 6:acc-=x*5u;break; default:acc^=x;break; } }\n"
     "  short v[64]; for(int i=0;i<64;i++) v[i]=(short)(((unsigned)a*(unsigned)(i+3))>>2);\n"
     "  for(int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<16;i++) if(v[b*16+i]<0) m|=(1u<<i); acc=acc*131u+m; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo47", opt, fl},

    // NEON gather FIRST, then an 8-way switch dispatched on the gathered mask:
    // the constant pool precedes the jump table in `.text`.
    {p+"_vecsw",
     t+" "+p+"_vecsw("+t+" a) {\n"
     "  signed char v[128]; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++) v[i]=(signed char)(((unsigned)a*(unsigned)(i+1))>>1);\n"
     "  for(int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(v[b*32+i]<0) m|=(1u<<i);\n"
     "    switch((m>>2)&7u){ case 0:acc+=m;break; case 1:acc^=m*3u;break;\n"
     "      case 2:acc-=m>>1;break; case 3:acc+=m<<1;break; case 4:acc^=m*5u;break;\n"
     "      case 5:acc+=m>>2;break; case 6:acc-=m*7u;break; default:acc^=m<<2;break; } }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2345678ULL}, "VectorAlgo47", opt, fl},

    // Two distinct NEON constant pools in one function (byte sign gather + int16
    // constant dot product): both must share ONE `.text` segment global.
    {p+"_twovec",
     t+" "+p+"_twovec("+t+" a) {\n"
     "  signed char u[128]; long long acc=0;\n"
     "  for(int i=0;i<128;i++) u[i]=(signed char)(((unsigned)a*(unsigned)(i+1))>>1);\n"
     "  for(int b=0;b<4;b++){ unsigned m=0; for(int i=0;i<32;i++) if(u[b*32+i]<0) m|=(1u<<i); acc+=(long long)m*(b+1); }\n"
     "  short x[128]; static const short W[8]={101,-103,107,-109,113,-127,131,-137};\n"
     "  for(int i=0;i<128;i++) x[i]=(short)(((unsigned)a*(unsigned)(i+1))>>4);\n"
     "  for(int i=0;i<120;i++){ long long s=0; for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)W[k]); acc+=s; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x3456789ULL}, "VectorAlgo47", opt, fl},

    // Switch accumulate THEN a constant-coefficient dot product (jump table +
    // coefficient pool); the switch clobbers no NEON state between them.
    {p+"_swdot",
     t+" "+p+"_swdot("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ unsigned x=(unsigned)a*(unsigned)(i+2);\n"
     "    switch((x>>4)&7u){ case 0:acc+=x;break; case 1:acc^=x<<1;break;\n"
     "      case 2:acc-=x>>2;break; case 3:acc+=x*3u;break; case 4:acc^=x>>4;break;\n"
     "      case 5:acc+=x<<3;break; case 6:acc-=x*9u;break; default:acc^=x>>1;break; } }\n"
     "  short x[128]; static const short C[8]={7,-11,13,-17,19,-23,29,-31};\n"
     "  for(int i=0;i<128;i++) x[i]=(short)(((unsigned)a*(unsigned)(i+5))>>4);\n"
     "  long long d=0; for(int i=0;i<120;i++){ long long s=0; for(int k=0;k<8;k++) s+=(long long)((int)x[i+k]*(int)C[k]); d+=s; }\n"
     "  acc = acc*131u + (unsigned)d;\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x456789AULL}, "VectorAlgo47", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec47 =
    makeVec47TC("x64v47", "long", 2, "-mssse3");
static const std::vector<RoundTripTC> kA64Vec47 =
    makeVec47TC("a64v47", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec47 =
    makeVec47TC("armv47", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo47, X64VectorAlgo47RT,
                         ::testing::ValuesIn(kX64Vec47), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo47, A64VectorAlgo47RT,
                         ::testing::ValuesIn(kA64Vec47), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo47, ARM32VectorAlgo47RT,
                         ::testing::ValuesIn(kARM32Vec47), rtTCName);
