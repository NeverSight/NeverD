//===- AllPlatform_OptStress192RTTests.cpp - cyclesort / shoelace / Damm ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * cyclesort - cycle sort over a rodata-parameterised permutation: each value
//                 is placed in its final slot by counting how many are smaller,
//                 in minimal writes.  Pins a write-minimising cycle-walk sort
//                 (distinct from every comparison/insertion/heap sort).
//   * shoelace  - twice-signed polygon area by the shoelace cross-sum over rodata
//                 vertices.  Pins a wrap-around edge cross accumulation (distinct
//                 from the single orientation triple #138 — here a closed-ring
//                 reduce over all edges).
//   * damm      - Damm check-digit over rodata digits using a 10x10 anti-symmetric
//                 quasigroup table (rodata).  Pins a table-driven running-state
//                 fold (distinct from the Luhn doubling #175 and the modular
//                 checksums Adler/CRC).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress192RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress192RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress192RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress192RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress192RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress192RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress192RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress192RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress192TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // cycle sort over a rodata-parameterised permutation of 0..15.
    {p+"_cyclesort",
     "static const unsigned char "+p+"_cy[8]={3,7,11,13,5,9,1,15};\n"
     +t+" "+p+"_cyclesort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned mul=((unsigned)"+p+"_cy[s&7u])|1u; unsigned off=(s>>3)&15u;\n"
     "    unsigned v[16]; for(int i=0;i<16;i++) v[i]=((unsigned)i*mul+off)&15u;\n"
     "    unsigned writes=0u;\n"
     "    for(int cs=0; cs<15; cs++){ unsigned item=v[cs]; int pos=cs;\n"
     "      for(int i=cs+1;i<16;i++) if(v[i]<item) pos++;\n"
     "      if(pos==cs) continue;\n"
     "      { unsigned tmp=v[pos]; v[pos]=item; item=tmp; } writes++;\n"
     "      while(pos!=cs){ pos=cs;\n"
     "        for(int i=cs+1;i<16;i++) if(v[i]<item) pos++;\n"
     "        { unsigned tmp=v[pos]; v[pos]=item; item=tmp; } writes++; }\n"
     "      acc=acc*131u+(unsigned)pos+writes; }\n"
     "    unsigned fold=0u; for(int i=0;i<16;i++) fold=fold*131u+v[i];\n"
     "    acc=acc*131u+fold+writes; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC1u}, "OptStress192", 2},

    // twice-signed polygon area via the shoelace cross-sum over rodata vertices.
    {p+"_shoelace",
     "static const unsigned char "+p+"_sl[20]={5,2,9,3,14,7,11,12,4,15, 2,10,13,6,8,1,15,9,3,5};\n"
     +t+" "+p+"_shoelace("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int xs[10], ys[10];\n"
     "    for(int i=0;i<10;i++){ xs[i]=(int)((unsigned)"+p+"_sl[i*2]^((s>>(i&7))&7u));\n"
     "      ys[i]=(int)((unsigned)"+p+"_sl[i*2+1]^((s>>((i+1)&7))&7u)); }\n"
     "    int sum2=0;\n"
     "    for(int i=0;i<10;i++){ int j=(i+1)==10?0:(i+1); sum2 += xs[i]*ys[j] - xs[j]*ys[i]; }\n"
     "    unsigned area2=(unsigned)(sum2<0? -sum2 : sum2);\n"
     "    acc=acc*131u+area2+((unsigned)sum2&0x7FFFu); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC2u}, "OptStress192", 2},

    // Damm check-digit over rodata digits via a 10x10 anti-symmetric quasigroup.
    {p+"_damm",
     "static const unsigned char "+p+"_dm[100]={\n"
     "0,3,1,7,5,9,8,6,4,2, 7,0,9,2,1,5,4,8,6,3, 4,2,0,6,8,7,1,3,5,9, 1,7,5,0,9,8,3,4,2,6,\n"
     "6,1,2,3,0,4,5,9,7,8, 3,6,7,4,2,0,9,5,8,1, 5,8,6,9,7,2,0,1,3,4, 8,9,4,5,3,6,2,0,1,7,\n"
     "9,4,3,8,6,1,7,2,0,5, 2,5,8,1,4,3,6,7,9,0};\n"
     +t+" "+p+"_damm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned interim=0u, fold=0u;\n"
     "    for(int i=0;i<12;i++){ unsigned d=((s>>(i*2))^(s>>i))%10u;\n"
     "      interim=(unsigned)"+p+"_dm[interim*10u+d]; fold=fold*131u+interim; }\n"
     "    acc=acc*131u+interim*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xC3u}, "OptStress192", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress192TC("x64o192", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress192TC("x86o192", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress192TC("a64o192", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress192TC("armo192", "int");

INSTANTIATE_TEST_SUITE_P(OptStress192, X64OptStress192RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress192, X86OptStress192RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress192, A64OptStress192RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress192, ARM32OptStress192RT, ::testing::ValuesIn(kARM), rtTCName);
