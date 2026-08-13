//===- AllPlatform_OptStress204RTTests.cpp - by-value struct return ABI ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for by-value aggregate RETURNS across the size classes that
// pick different ABI paths -- small (one return register), medium (a register
// pair / two eightbytes), mixed int+FP, and large (the hidden sret pointer the
// i386 callee pops on return).  These harden the #495 work (x86-64 multi-register
// struct return, i386 sret callee-cleanup) and the loop / both-directions corners.
//
//   * rets2i  - return struct{int,int} (8B: x64 one RAX eightbyte, i386 EDX:EAX).
//   * rets3i  - return struct{int,int,int} (12B: x64 RAX:RDX, i386 sret pop).
//   * retmix  - return struct{int,double} (x64 RAX+XMM0 mixed; i386 sret pop).
//   * retloop - struct{int,int} returned inside a loop, fields accumulated.
//   * argret  - takes a struct by value AND returns one (both ABI directions).
//   * ret8i   - return struct{8 ints} (32B: the large sret class everywhere).
//
// Each entry has a plain T(T) ABI and folds the returned aggregate to one integer
// (FP fields truncated to int) so the harness compares a single return value.
// Helpers are noinline; structs <= 32B so clang inlines the copies (no memcpy).
// Integer/FP add/xor/mul only, -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress204RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress204RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress204RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress204RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress204RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress204RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress204RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress204RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress204TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // struct{int,int}: one 8-byte return (x64 RAX, i386 EDX:EAX).
    {p+"_rets2i",
     "typedef struct{int a,b;}"+p+"_S2;\n"
     +p+"_S2 "+p+"_mk2(int x) __attribute__((noinline));\n"
     +t+" "+p+"_rets2i("+t+" x){ "+p+"_S2 r="+p+"_mk2((int)x);\n"
     "  return ("+t+")(r.a^(r.b*3)); }\n"
     +p+"_S2 "+p+"_mk2(int x){ "+p+"_S2 r; r.a=x*7+1; r.b=x-5; return r; }\n",
     {0x123ULL}, "OptStress204", 2},

    // struct{int,int,int}: 12 bytes (x64 RAX:RDX, i386 sret callee-pop).
    {p+"_rets3i",
     "typedef struct{int a,b,c;}"+p+"_S3;\n"
     +p+"_S3 "+p+"_mk3(int x) __attribute__((noinline));\n"
     +t+" "+p+"_rets3i("+t+" x){ "+p+"_S3 r="+p+"_mk3((int)x);\n"
     "  return ("+t+")(r.a^(r.b*3)^(r.c*5)); }\n"
     +p+"_S3 "+p+"_mk3(int x){ "+p+"_S3 r; r.a=x+1; r.b=x*3; r.c=x-7; return r; }\n",
     {0x456ULL}, "OptStress204", 2},

    // struct{int,double}: mixed int+FP return (x64 RAX+XMM0; i386 sret pop).
    {p+"_retmix",
     "typedef struct{int a; double b;}"+p+"_SM;\n"
     +p+"_SM "+p+"_mkm(int x) __attribute__((noinline));\n"
     +t+" "+p+"_retmix("+t+" x){ "+p+"_SM r="+p+"_mkm((int)x);\n"
     "  return ("+t+")(r.a ^ (int)r.b); }\n"
     +p+"_SM "+p+"_mkm(int x){ "+p+"_SM r; r.a=x*3+1; r.b=(double)(x*2)+0.5; return r; }\n",
     {0x78ULL}, "OptStress204", 2},

    // struct{int,int} returned inside a loop, fields accumulated.
    {p+"_retloop",
     "typedef struct{int a,b;}"+p+"_L2;\n"
     +p+"_L2 "+p+"_mkl(int s) __attribute__((noinline));\n"
     +t+" "+p+"_retloop("+t+" x){ unsigned s=(unsigned)x; "+t+" acc=0;\n"
     "  for(int k=0;k<64;k++){ s=s*1103515245u+12345u;\n"
     "    "+p+"_L2 r="+p+"_mkl((int)(s>>8));\n"
     "    acc+=("+t+")(r.a ^ (r.b*7)); }\n"
     "  return acc; }\n"
     +p+"_L2 "+p+"_mkl(int s){ "+p+"_L2 r; r.a=s*3+1; r.b=s^0x55; return r; }\n",
     {0x9ULL}, "OptStress204", 2},

    // Takes a struct by value AND returns one (both ABI directions at once).
    {p+"_argret",
     "typedef struct{int a,b;}"+p+"_A2;\n"
     +p+"_A2 "+p+"_xf("+p+"_A2 s) __attribute__((noinline));\n"
     +t+" "+p+"_argret("+t+" x){ "+p+"_A2 s; s.a=(int)x; s.b=(int)x+9;\n"
     "  "+p+"_A2 r="+p+"_xf(s);\n"
     "  return ("+t+")(r.a ^ (r.b*3)); }\n"
     +p+"_A2 "+p+"_xf("+p+"_A2 s){ "+p+"_A2 r; r.a=s.a*5+s.b; r.b=s.a-s.b*2; return r; }\n",
     {0x33ULL}, "OptStress204", 2},

    // struct{8 ints} (32B): the large sret class on every target.
    {p+"_ret8i",
     "typedef struct{int a,b,c,d,e,f,g,h;}"+p+"_S8;\n"
     +p+"_S8 "+p+"_mk8(int x) __attribute__((noinline));\n"
     +t+" "+p+"_ret8i("+t+" x){ "+p+"_S8 r="+p+"_mk8((int)x);\n"
     "  return ("+t+")(r.a+2*r.b+3*r.c+4*r.d+5*r.e+6*r.f+7*r.g+8*r.h); }\n"
     +p+"_S8 "+p+"_mk8(int x){ "+p+"_S8 r;\n"
     "  r.a=x; r.b=x+1; r.c=x+2; r.d=x+3; r.e=x+4; r.f=x+5; r.g=x+6; r.h=x+7;\n"
     "  return r; }\n",
     {0x11ULL}, "OptStress204", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress204TC("x64o204", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress204TC("x86o204", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress204TC("a64o204", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress204TC("armo204", "int");

INSTANTIATE_TEST_SUITE_P(OptStress204, X64OptStress204RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress204, X86OptStress204RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress204, A64OptStress204RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress204, ARM32OptStress204RT, ::testing::ValuesIn(kARM), rtTCName);
