//===- AllPlatform_OptStress260RTTests.cpp - local aggregates at -O0 =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Local arrays / structs / pointer walks at -O0 — OptStress229 drove these at
// -O2; this is the -O0 counterpart, where every element access is a fresh
// frame-relative load/store (no register promotion), the induction pointer is
// re-materialized each iteration, and sub-word fields use explicit movzbl/movb.
// That form stresses frame addressing (#158), sub-word load/store, and pointer
// arithmetic on the stack.
//
//   * arrsum  - fill a local u32 array, then walk and reduce.
//   * structf - local struct with mixed-width fields, read/write.
//   * ptrrev  - two-pointer in-place reverse of a local array.
//   * gather  - computed-index gather from a local array.
//   * hist    - local histogram, load-modify-store per bucket.
//   * chase   - pointer arithmetic walk with a non-unit stride.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit element ops — i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress260RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress260RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress260RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress260RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress260RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress260RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress260RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress260RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress260TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fill a local u32 array, then walk and reduce.
    {p+"_arrsum",
     t+" "+p+"_arrsum("+t+" a){ unsigned h=(unsigned)a; unsigned buf[16]; unsigned acc=0;\n"
     "  for(int r=0;r<8;r++){\n"
     "    for(int j=0;j<16;j++){ h=h*1103515245u+12345u; buf[j]=h^(unsigned)(j*2654435761u); }\n"
     "    for(int j=0;j<16;j++) acc=acc*131u + buf[(j*5)&15]; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress260", 0},

    // Local struct with mixed-width fields, read/write.
    {p+"_structf",
     "struct LM{ unsigned char b; unsigned short w; unsigned d; signed char s; };\n"
     +t+" "+p+"_structf("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  struct LM m; m.b=1; m.w=2; m.d=3; m.s=-1;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    m.b=(unsigned char)(m.b+h); m.w=(unsigned short)(m.w^(h>>3)); m.d=m.d+(h>>1); m.s=(signed char)(m.s-(int)(h>>5));\n"
     "    acc=acc*131u + m.b + m.w + m.d + (unsigned)(int)m.s; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress260", 0},

    // Two-pointer in-place reverse of a local array.
    {p+"_ptrrev",
     t+" "+p+"_ptrrev("+t+" a){ unsigned h=(unsigned)a; unsigned buf[12]; unsigned acc=0;\n"
     "  for(int j=0;j<12;j++){ h=h*1103515245u+12345u; buf[j]=h; }\n"
     "  unsigned *lo=buf, *hi=buf+11;\n"
     "  while(lo<hi){ unsigned tv=*lo; *lo=*hi; *hi=tv; lo++; hi--; }\n"
     "  for(int j=0;j<12;j++) acc=acc*131u + buf[j];\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress260", 0},

    // Computed-index gather from a local array.
    {p+"_gather",
     t+" "+p+"_gather("+t+" a){ unsigned h=(unsigned)a; unsigned tab[32]; unsigned acc=0;\n"
     "  for(int j=0;j<32;j++) tab[j]=(unsigned)(j*2246822519u+1u);\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned idx=(h ^ (h>>11)) & 31u; acc=acc*131u + tab[idx]; tab[(idx*7)&31]+=h>>3; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress260", 0},

    // Local histogram, load-modify-store per bucket.
    {p+"_hist",
     t+" "+p+"_hist("+t+" a){ unsigned h=(unsigned)a; unsigned cnt[16]; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++) cnt[j]=0;\n"
     "  for(int i=0;i<256;i++){ h=h*1103515245u+12345u; cnt[h&15u]+=1u; cnt[(h>>4)&15u]+=(h>>8)&7u; }\n"
     "  for(int j=0;j<16;j++) acc=acc*131u + cnt[j];\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress260", 0},

    // Pointer arithmetic walk with a non-unit stride.
    {p+"_chase",
     t+" "+p+"_chase("+t+" a){ unsigned h=(unsigned)a; unsigned buf[24]; unsigned acc=0;\n"
     "  for(int j=0;j<24;j++){ h=h*1103515245u+12345u; buf[j]=h; }\n"
     "  for(unsigned *q=buf; q<buf+24; q+=3){ acc=acc*131u + q[0] + q[1]*2u + q[2]*3u; }\n"
     "  for(unsigned *q=buf+23; q>=buf; q-=5){ acc=acc*7u + *q; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress260", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress260TC("x64o260", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress260TC("x86o260", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress260TC("a64o260", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress260TC("armo260", "int");

INSTANTIATE_TEST_SUITE_P(OptStress260, X64OptStress260RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress260, X86OptStress260RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress260, A64OptStress260RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress260, ARM32OptStress260RT, ::testing::ValuesIn(kARM), rtTCName);
