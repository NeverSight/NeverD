//===- AllPlatform_OptStress340RTTests.cpp - struct return of arguments ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A callee returns a small struct built from its own arguments.  Where the
// argument registers are the return registers (x86-64 xmm0:xmm1, AArch64
// x0:x1 and v0:v1), `return (S){a, b};` is a lone `ret`: nothing reads or
// writes the registers, yet each field is the argument passed there.  The
// lifted callee once returned zeros (and AArch64's integer pair nothing),
// because the registers became neither parameters nor return fields.
//
//   * passdd  - returns {double,double} of its two double arguments.
//   * passll  - returns {long,long} of its two integer arguments.
//   * swapdd  - returns them swapped: the callee moves the registers.
//   * halfdd  - computes the first field and passes the second through.
//
// The caller folds both fields into an integer accumulator across a loop, so
// the result is value-only.  All four targets, -O2; the 32-bit targets return
// the struct through memory.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"
#include "SourceRouteCases.h"

class X64OptStress340RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress340RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress340RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress340RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress340RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress340RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress340RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {
};
TEST_P(ARM32OptStress340RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress340TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // The double pair returns in the registers it arrived in.
    {p+"_passdd",
     "typedef struct{double a,b;}"+p+"_DD;\n"
     +p+"_DD "+p+"_mkdd(double a,double b) __attribute__((noinline));\n"
     +t+" "+p+"_passdd("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_DD r="+p+"_mkdd((double)(int)(h>>8),(double)(int)(h>>3)*0.5);\n"
     "    acc=acc*131u+(unsigned)((int)r.a ^ ((int)r.b*9))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_DD "+p+"_mkdd(double a,double b){ return ("+p+"_DD){a,b}; }\n",
     {0x13579u}, "OptStress340", 2},

    // The integer pair returns in x0:x1 untouched on AArch64.
    {p+"_passll",
     "typedef struct{"+t+" a,b;}"+p+"_LL;\n"
     +p+"_LL "+p+"_mkll("+t+" a,"+t+" b) __attribute__((noinline));\n"
     +t+" "+p+"_passll("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_LL r="+p+"_mkll(("+t+")(h>>5),("+t+")(h^0x5a5au));\n"
     "    acc=acc*131u+(unsigned)(r.a ^ (r.b*9))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_LL "+p+"_mkll("+t+" a,"+t+" b){ return ("+p+"_LL){a,b}; }\n",
     {0x2468Au}, "OptStress340", 2},

    // Swapped fields move the argument registers into each other.
    {p+"_swapdd",
     "typedef struct{double a,b;}"+p+"_SD;\n"
     +p+"_SD "+p+"_swap(double a,double b) __attribute__((noinline));\n"
     +t+" "+p+"_swapdd("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_SD r="+p+"_swap((double)(int)(h>>9),(double)(int)(h>>4)+0.25);\n"
     "    acc=acc*131u+(unsigned)((int)r.a ^ ((int)r.b*7))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_SD "+p+"_swap(double a,double b){ return ("+p+"_SD){b,a}; }\n",
     {0x369CEu}, "OptStress340", 2},

    // The first field is computed; the second is the argument passed there.
    {p+"_halfdd",
     "typedef struct{double a,b;}"+p+"_HD;\n"
     +p+"_HD "+p+"_half(double a,double b) __attribute__((noinline));\n"
     +t+" "+p+"_halfdd("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_HD r="+p+"_half((double)(int)(h>>7),(double)(int)(h>>2));\n"
     "    acc=acc*131u+(unsigned)((int)r.a ^ ((int)r.b*5))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_HD "+p+"_half(double a,double b){ return ("+p+"_HD){a*0.5,b}; }\n",
     {0x48D15u}, "OptStress340", 2},
  };
  return v;
}

static const std::vector<RoundTripTC> kX64 = makeOptStress340TC("x64o340", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress340TC("x86o340", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress340TC("a64o340", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress340TC("armo340", "int");

INSTANTIATE_TEST_SUITE_P(OptStress340, X64OptStress340RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress340, X86OptStress340RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress340, A64OptStress340RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress340, ARM32OptStress340RT, ::testing::ValuesIn(kARM), rtTCName);

// The register a pair-aligned double skips is a loop join of the caller's
// own entry value, which no callee reads: the C is checked by execution.
INSTANTIATE_TEST_SUITE_P(OptStress340HighC, ARM32OptStress340RT, ::testing::ValuesIn(onHighC(kARM, {"_halfdd", "_passdd", "_swapdd"})), rtTCName);
// clang-format on
