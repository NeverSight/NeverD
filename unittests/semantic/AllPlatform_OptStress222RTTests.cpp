//===- AllPlatform_OptStress222RTTests.cpp - FP-field struct RETURN ABI ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The return-value dual of OptStress221: a callee RETURNS a small struct whose
// fields fall in mixed classes, consumed in a loop where the caller also reuses
// the FP register file as scratch.  This exercises the multi-register struct
// return path (x86-64 RAX:XMM0 / XMM0:XMM1, AArch64 X0:X1 / HFA V0..V3, i386
// sret / x87) inside a loop -- the dual of the call-return routing that bit
// OptStress221.  OptStress204 covered struct returns but not FP-field ones
// driven in a tight loop with FP-register scratch reuse.
//
//   * retid - returns {int,double}  : INTEGER + SSE eightbyte (RAX + XMM0).
//   * retdi - returns {double,int}  : SSE + INTEGER eightbyte (XMM0 + RAX).
//   * retff - returns {float,float} : one all-float eightbyte packed in XMM0.
//   * retdd - returns {double,double}: XMM0:XMM1 (x64) / HFA V0,V1 (a64).
//   * ret3f - returns {float,float,float}: 3-float HFA (AArch64 S0..S2).
//   * retfi - returns {float,int}   : one MIXED eightbyte -> one GP register.
//
// The callee builds the struct from the seed; the caller folds the returned
// fields into an integer accumulator across the loop, so the result is
// value-only.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress222RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress222RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress222RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress222RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress222RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress222RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress222RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress222RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress222TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // returns {int,double}: RAX + XMM0 (x64) / X0:X1 (a64) / sret (i386,arm32).
    {p+"_retid",
     "typedef struct{int a; double b;}"+p+"_ID;\n"
     +p+"_ID "+p+"_mkid(int x) __attribute__((noinline));\n"
     +t+" "+p+"_retid("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_ID r="+p+"_mkid((int)h);\n"
     "    acc=acc*131u+(unsigned)(r.a ^ ((int)r.b*7))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_ID "+p+"_mkid(int x){ "+p+"_ID r; r.a=x*3+1; r.b=(double)(x>>2)+0.5; return r; }\n",
     {0x12345u}, "OptStress222", 2},

    // returns {double,int}: XMM0 + RAX (x64) / X0:X1 (a64).
    {p+"_retdi",
     "typedef struct{double a; int b;}"+p+"_DI;\n"
     +p+"_DI "+p+"_mkdi(int x) __attribute__((noinline));\n"
     +t+" "+p+"_retdi("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_DI r="+p+"_mkdi((int)h);\n"
     "    acc=acc*131u+(unsigned)(((int)r.a*3) ^ r.b)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_DI "+p+"_mkdi(int x){ "+p+"_DI r; r.a=(double)(x>>1)-2.5; r.b=x*5+3; return r; }\n",
     {0x23456u}, "OptStress222", 2},

    // returns {float,float}: one all-float eightbyte packed in XMM0 (x64) / HFA.
    {p+"_retff",
     "typedef struct{float a,b;}"+p+"_FF;\n"
     +p+"_FF "+p+"_mkff(int x) __attribute__((noinline));\n"
     +t+" "+p+"_retff("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_FF r="+p+"_mkff((int)h);\n"
     "    acc=acc*131u+(unsigned)((int)r.a ^ ((int)r.b*5))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_FF "+p+"_mkff(int x){ "+p+"_FF r; r.a=(float)(x>>3); r.b=(float)(x>>1)+1.0f; return r; }\n",
     {0x34567u}, "OptStress222", 2},

    // returns {double,double}: XMM0:XMM1 (x64) / HFA V0,V1 (a64).
    {p+"_retdd",
     "typedef struct{double a,b;}"+p+"_DD;\n"
     +p+"_DD "+p+"_mkdd(int x) __attribute__((noinline));\n"
     +t+" "+p+"_retdd("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_DD r="+p+"_mkdd((int)h);\n"
     "    acc=acc*131u+(unsigned)((int)r.a ^ ((int)r.b*9))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_DD "+p+"_mkdd(int x){ "+p+"_DD r; r.a=(double)(x>>2)+0.25; r.b=(double)(x>>4)-1.75; return r; }\n",
     {0x45678u}, "OptStress222", 2},

    // returns {float,float,float}: 3-float HFA (AArch64 S0..S2).
    {p+"_ret3f",
     "typedef struct{float a,b,c;}"+p+"_F3;\n"
     +p+"_F3 "+p+"_mk3f(int x) __attribute__((noinline));\n"
     +t+" "+p+"_ret3f("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_F3 r="+p+"_mk3f((int)h);\n"
     "    acc=acc*131u+(unsigned)((int)r.a ^ ((int)r.b*3) ^ ((int)r.c*5))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_F3 "+p+"_mk3f(int x){ "+p+"_F3 r; r.a=(float)(x>>2); r.b=(float)(x>>5); r.c=(float)(x>>1); return r; }\n",
     {0x56789u}, "OptStress222", 2},

    // returns {float,int}: one MIXED eightbyte -> one GP register (x64).
    {p+"_retfi",
     "typedef struct{float a; int b;}"+p+"_FI;\n"
     +p+"_FI "+p+"_mkfi(int x) __attribute__((noinline));\n"
     +t+" "+p+"_retfi("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_FI r="+p+"_mkfi((int)h);\n"
     "    acc=acc*131u+(unsigned)(((int)r.a*9) ^ r.b)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     +p+"_FI "+p+"_mkfi(int x){ "+p+"_FI r; r.a=(float)(x>>3); r.b=x*7+1; return r; }\n",
     {0x6789Au}, "OptStress222", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress222TC("x64o222", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress222TC("x86o222", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress222TC("a64o222", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress222TC("armo222", "int");

INSTANTIATE_TEST_SUITE_P(OptStress222, X64OptStress222RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress222, X86OptStress222RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress222, A64OptStress222RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress222, ARM32OptStress222RT, ::testing::ValuesIn(kARM), rtTCName);
