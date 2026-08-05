//===- AllPlatform_OptStress237RTTests.cpp - vector constant immediates ==//
//
// Loops over stack arrays with constant operands that clang auto-vectorizes
// into vector-immediate forms: ARM32 `vmov.i*/vmvn.i*/vbic.i*/vorr.i*`,
// AArch64 `movi/mvni/bic/orr #imm`, x86 broadcast / all-ones (`pcmpeqd`).
// This is the family that surfaced the #505 `vmov.i64` shift-by-64 mask bug,
// so it probes every lane width and the and/or/not/compare immediate variants
// directly.
//
//   * andmask - per-element AND with a constant mask.
//   * bicor   - (a & ~K1) | K2  (clears + sets constant bit groups).
//   * notxor  - ~a ^ K          (vmvn + constant xor).
//   * clampk  - clamp each element to a constant [LO,HI] window.
//   * eqcount - count elements equal to a constant (compare→mask→sum).
//   * addk    - add a constant broadcast to every element.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress237RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress237RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress237RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress237RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress237RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress237RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress237RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress237RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress237TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Per-element AND with a constant mask (vand + vmov.i / movi).
    {p+"_andmask",
     t+" "+p+"_andmask("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=h; }\n"
     "    unsigned s=0; for(int k=0;k<16;k++){ x[k]&=0x0F0F0F0Fu; s+=x[k]; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress237", 2},

    // (a & ~K1) | K2 — clear one constant bit group, set another.
    {p+"_bicor",
     t+" "+p+"_bicor("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=h; }\n"
     "    unsigned s=0; for(int k=0;k<16;k++){ x[k]=(x[k]&~0xFF00FF00u)|0x00120034u; s^=x[k]; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress237", 2},

    // ~a ^ K  (vmvn + constant xor).
    {p+"_notxor",
     t+" "+p+"_notxor("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=h; }\n"
     "    unsigned s=0; for(int k=0;k<16;k++){ x[k]=~x[k]^0xA5A5A5A5u; s+=x[k]*3u; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress237", 2},

    // Clamp each element to a constant window [LO,HI] (broadcast min/max).
    {p+"_clampk",
     t+" "+p+"_clampk("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    int x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=(int)h; }\n"
     "    int s=0; for(int k=0;k<16;k++){ int v=x[k]; if(v<-1000)v=-1000; if(v>1000)v=1000; s+=v; }\n"
     "    acc=acc*131u+(unsigned)s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress237", 2},

    // Count elements equal to a constant (compare → mask → sum).
    {p+"_eqcount",
     t+" "+p+"_eqcount("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned char x[32]; for(int k=0;k<32;k++){ h=h*1664525u+1013904223u; x[k]=(unsigned char)(h>>13); }\n"
     "    unsigned s=0; for(int k=0;k<32;k++){ if(x[k]==0x42u) s+=7u; if((x[k]&0x80u)!=0) s+=1u; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress237", 2},

    // Add a constant broadcast to every element (vadd + vmov.i broadcast).
    {p+"_addk",
     t+" "+p+"_addk("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<80;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned short x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=(unsigned short)(h>>7); }\n"
     "    unsigned s=0; for(int k=0;k<16;k++){ x[k]=(unsigned short)(x[k]+0x1234u); s+=x[k]; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress237", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress237TC("x64o237", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress237TC("x86o237", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress237TC("a64o237", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress237TC("armo237", "int");

INSTANTIATE_TEST_SUITE_P(OptStress237, X64OptStress237RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress237, X86OptStress237RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress237, A64OptStress237RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress237, ARM32OptStress237RT, ::testing::ValuesIn(kARM), rtTCName);
