//===- AllPlatform_OptStress278RTTests.cpp - array/pointer RMW at -O0 ====//
//
// Compound-assignment read-modify-write through arrays and pointers at -O0 —
// histograms, in-place transforms, pointer-based scatter, and writable-global
// RMW.  At -O0 each `a[i] op= x` is a separate load / op / store to a computed
// address (no register promotion), so this stresses the lifter's memory model,
// computed-address GEP recovery, sub-word RMW, and (for the global cases) the
// writable-global symbolization path under PIC.
//
//   * hist    - local histogram: a[idx]++ with a runtime index.
//   * inplace - in-place transform a[i] = f(a[i], a[i-1]).
//   * scatter - pointer-based scatter p[idx] op= v.
//   * subw    - sub-word (u8 / u16) array RMW.
//   * gbss    - writable .bss global histogram (RMW into a zero-init global).
//   * gdata   - writable .data global accumulate (RMW into an initialized global).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress278RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress278RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress278RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress278RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress278RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress278RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress278RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress278RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress278TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // local histogram: a[idx]++ with a runtime index.
    {p+"_hist",
     t+" "+p+"_hist("+t+" a){ unsigned h=(unsigned)a; unsigned cnt[16]; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++) cnt[j]=0;\n"
     "  for(int i=0;i<300;i++){ h=h*1103515245u+12345u; cnt[(h>>7)&15u]++; }\n"
     "  for(int j=0;j<16;j++) acc=acc*131u+cnt[j];\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress278", 0},

    // in-place transform a[i] = f(a[i], a[i-1]).
    {p+"_inplace",
     t+" "+p+"_inplace("+t+" a){ unsigned h=(unsigned)a; unsigned buf[24]; unsigned acc=0;\n"
     "  for(int j=0;j<24;j++){ h=h*1103515245u+12345u; buf[j]=h; }\n"
     "  for(int rep=0;rep<6;rep++) for(int j=1;j<24;j++) buf[j]=(buf[j]^buf[j-1])*131u+1u;\n"
     "  for(int j=0;j<24;j++) acc=acc*131u+buf[j];\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress278", 0},

    // pointer-based scatter p[idx] op= v through a moving base.
    {p+"_scatter",
     t+" "+p+"_scatter("+t+" a){ unsigned h=(unsigned)a; unsigned tab[20]; unsigned acc=0;\n"
     "  for(int j=0;j<20;j++) tab[j]=(unsigned)(j+1);\n"
     "  unsigned *base=tab;\n"
     "  for(int i=0;i<240;i++){ h=h*1103515245u+12345u; unsigned idx=(h>>5)%20u;\n"
     "    if(h&1u) base[idx]+=h>>9; else base[idx]^=h>>11; }\n"
     "  for(int j=0;j<20;j++) acc=acc*131u+tab[j];\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress278", 0},

    // sub-word (u8 / u16) array read-modify-write.
    {p+"_subw",
     t+" "+p+"_subw("+t+" a){ unsigned h=(unsigned)a; unsigned char b[16]; unsigned short w[8]; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++) b[j]=0; for(int j=0;j<8;j++) w[j]=0;\n"
     "  for(int i=0;i<240;i++){ h=h*1103515245u+12345u;\n"
     "    b[(h>>4)&15u]=(unsigned char)(b[(h>>4)&15u]+h); w[(h>>8)&7u]=(unsigned short)(w[(h>>8)&7u]^h); }\n"
     "  for(int j=0;j<16;j++) acc=acc*131u+b[j]; for(int j=0;j<8;j++) acc=acc*131u+w[j];\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress278", 0},

    // writable .bss global histogram (RMW into a zero-init global).
    {p+"_gbss",
     "static unsigned GHIST[16];\n"
     +t+" "+p+"_gbss("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++) GHIST[j]=0;\n"
     "  for(int i=0;i<300;i++){ h=h*1103515245u+12345u; GHIST[(h>>6)&15u]+=(h>>10)&0xffu; }\n"
     "  for(int j=0;j<16;j++) acc=acc*131u+GHIST[j];\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress278", 0},

    // writable .data global accumulate (RMW into an initialized global).
    {p+"_gdata",
     "static unsigned GACC[8]={1u,2u,3u,4u,5u,6u,7u,8u};\n"
     +t+" "+p+"_gdata("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<8;j++) GACC[j]=(unsigned)(j+1);\n"
     "  for(int i=0;i<240;i++){ h=h*1103515245u+12345u; GACC[(h>>5)&7u]=GACC[(h>>5)&7u]*131u+h; }\n"
     "  for(int j=0;j<8;j++) acc=acc*131u+GACC[j];\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress278", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress278TC("x64o278", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress278TC("x86o278", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress278TC("a64o278", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress278TC("armo278", "int");

INSTANTIATE_TEST_SUITE_P(OptStress278, X64OptStress278RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress278, X86OptStress278RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress278, A64OptStress278RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress278, ARM32OptStress278RT, ::testing::ValuesIn(kARM), rtTCName);
