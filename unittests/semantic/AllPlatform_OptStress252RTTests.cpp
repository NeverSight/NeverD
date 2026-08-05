//===- AllPlatform_OptStress252RTTests.cpp - bit manipulation ===========//
//
// Runtime bit indexing into bitset arrays + variable-width bitfield extract /
// deposit + multi-word bit scans.  clang lowers these to bt/bts/btr/btc (x86),
// ubfx/bfi (AArch64), and shift+mask chains (ARM32) — the bit-addressing path
// (word = n>>5, mask = 1<<(n&31)) mixes a scaled index load with a sub-word RMW,
// a combination not exercised by the earlier shift/rotate probes.
//
//   * bset   - set bits by runtime index into a bitset, popcount the result.
//   * bclr   - fill then clear bits by runtime index, count remaining.
//   * bextr  - extract a variable-width bitfield at a runtime offset.
//   * bdep   - deposit a bitfield at a runtime offset (read-modify-write).
//   * bscan  - find-first / find-last set across a multi-word bitset.
//   * bmix   - interleave test/set/toggle + popcount into one accumulator.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress252RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress252RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress252RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress252RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress252RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress252RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress252RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress252RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress252TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Set bits by runtime index into a 256-bit bitset, then popcount.
    {p+"_bset",
     t+" "+p+"_bset("+t+" a){ unsigned h=(unsigned)a; unsigned bs[8];\n"
     "  for(int i=0;i<8;i++) bs[i]=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned n=(h>>7)&255u;\n"
     "    bs[n>>5]|=1u<<(n&31u); }\n"
     "  unsigned acc=0; for(int i=0;i<8;i++) acc+=(unsigned)__builtin_popcount(bs[i]);\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress252", 2},

    // Fill all then clear bits by runtime index, count remaining set bits.
    {p+"_bclr",
     t+" "+p+"_bclr("+t+" a){ unsigned h=(unsigned)a; unsigned bs[8];\n"
     "  for(int i=0;i<8;i++) bs[i]=0xffffffffu;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned n=(h>>6)&255u;\n"
     "    bs[n>>5]&=~(1u<<(n&31u)); }\n"
     "  unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+(unsigned)__builtin_popcount(bs[i]);\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress252", 2},

    // Extract a variable-width bitfield (width 1..16) at a runtime offset.
    {p+"_bextr",
     t+" "+p+"_bextr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned off=h&15u, w=((h>>4)&15u)+1u; unsigned x=h^0x9e3779b9u;\n"
     "    unsigned mask=(w>=32u)?0xffffffffu:((1u<<w)-1u);\n"
     "    unsigned f=(x>>off)&mask; acc=acc*131u+f+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress252", 2},

    // Deposit a bitfield at a runtime offset (clear then OR in = RMW within word).
    {p+"_bdep",
     t+" "+p+"_bdep("+t+" a){ unsigned h=(unsigned)a; unsigned w=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned off=(h&7u)*4u; unsigned v=(h>>8)&15u;\n"
     "    w=(w&~(15u<<off))|(v<<off); }\n"
     "  return ("+t+")w; }\n",
     {0x45678u}, "OptStress252", 2},

    // Find-first and find-last set across a multi-word bitset.
    {p+"_bscan",
     t+" "+p+"_bscan("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<48;rep++){ h=h*1103515245u+12345u;\n"
     "    unsigned bs[4]={h,h*2654435761u,h^0x55aa55aau,h+0x12345678u};\n"
     "    int ffs=-1,fls=-1;\n"
     "    for(int i=0;i<4&&ffs<0;i++) if(bs[i]) ffs=i*32+__builtin_ctz(bs[i]);\n"
     "    for(int i=3;i>=0&&fls<0;i--) if(bs[i]) fls=i*32+(31-__builtin_clz(bs[i]));\n"
     "    acc=acc*131u+(unsigned)(ffs+1)+(unsigned)(fls+1)*7u; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress252", 2},

    // Interleave test / set / toggle + popcount into one accumulator.
    {p+"_bmix",
     t+" "+p+"_bmix("+t+" a){ unsigned h=(unsigned)a; unsigned w=0xdeadbeefu; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned n=(h>>5)&31u;\n"
     "    unsigned bit=(w>>n)&1u;\n"
     "    if(h&0x10000u) w|=1u<<n; else if(h&0x20000u) w&=~(1u<<n); else w^=1u<<n;\n"
     "    acc=acc*131u+bit+(unsigned)__builtin_popcount(w); }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress252", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress252TC("x64o252", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress252TC("x86o252", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress252TC("a64o252", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress252TC("armo252", "int");

INSTANTIATE_TEST_SUITE_P(OptStress252, X64OptStress252RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress252, X86OptStress252RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress252, A64OptStress252RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress252, ARM32OptStress252RT, ::testing::ValuesIn(kARM), rtTCName);
