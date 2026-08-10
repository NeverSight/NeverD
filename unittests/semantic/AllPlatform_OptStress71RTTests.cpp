//===- AllPlatform_OptStress71RTTests.cpp - small struct-by-value -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Probe for the #470 open bug O-6: a small struct with floating-point fields
// returned *by value* is split across multiple return registers, and NeverD
// collapses it to a single value losing the others:
//
//   * x86-64 SysV returns a <=16-byte struct in up to two eightbyte registers;
//     an SSE eightbyte (a float/double field) goes in XMM0/XMM1, not RAX/RDX.
//     {double,double} -> XMM0,XMM1 ; {long,double} -> RAX,XMM0.
//   * AArch64 returns a homogeneous floating aggregate (all float/double) in
//     V0..V3 (S/D registers), e.g. {float,float,float} -> S0,S1,S2.
//   * i386 / ARM32 return such structs via the hidden sret pointer (memory),
//     a path the StructAbi probe already covers — negative control here.
//
// Each kernel calls a noinline maker returning the struct, then folds the
// fields to one integer so native and lifted emulation compare a scalar.
// No libm, no 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress71RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress71RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress71RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress71RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress71RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress71RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress71RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress71RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress71TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // {double,double} -> x86-64 XMM0,XMM1 ; AArch64 HFA V0,V1.
    {p+"_retdd",
     "struct "+p+"DD{ double x,y; };\n"
     "static struct "+p+"DD "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_retdd("+t+" a){ struct "+p+"DD r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")(long long)(r.x*r.y + r.x - r.y); }\n"
     "static struct "+p+"DD "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; double x=(double)((s>>8)&0xffff);\n"
     "  s=s*1103515245u+12345u; double y=(double)((s>>8)&0xff)+1.0;\n"
     "  struct "+p+"DD r={x,y}; return r; }\n",
     {0x41u}, "OptStress71", 2},

    // {long,double} -> x86-64 RAX,XMM0 (INTEGER+SSE eightbyte).
    {p+"_retld",
     "struct "+p+"LD{ long a; double b; };\n"
     "static struct "+p+"LD "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_retld("+t+" a){ struct "+p+"LD r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")(r.a + (long long)r.b); }\n"
     "static struct "+p+"LD "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; long aa=(long)((s>>8)&0xffff);\n"
     "  s=s*1103515245u+12345u; double bb=(double)((s>>8)&0xff)+0.5;\n"
     "  struct "+p+"LD r={aa,bb}; return r; }\n",
     {0x42u}, "OptStress71", 2},

    // {double,long} -> x86-64 XMM0,RAX (SSE+INTEGER eightbyte, reversed order).
    {p+"_retdl",
     "struct "+p+"DL{ double a; long b; };\n"
     "static struct "+p+"DL "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_retdl("+t+" a){ struct "+p+"DL r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")((long long)r.a + r.b); }\n"
     "static struct "+p+"DL "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; double aa=(double)((s>>8)&0xffff)+0.5;\n"
     "  s=s*1103515245u+12345u; long bb=(long)((s>>8)&0xff);\n"
     "  struct "+p+"DL r={aa,bb}; return r; }\n",
     {0x43u}, "OptStress71", 2},

    // {float,float,float} -> AArch64 HFA S0,S1,S2 ; x86-64 XMM0(a,b),XMM1(c).
    {p+"_hfa3f",
     "struct "+p+"F3{ float a,b,c; };\n"
     "static struct "+p+"F3 "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_hfa3f("+t+" a){ struct "+p+"F3 r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")(int)(r.a*r.b + r.c); }\n"
     "static struct "+p+"F3 "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; float a=(float)((s>>8)&0xff);\n"
     "  s=s*1103515245u+12345u; float b=(float)((s>>8)&0x7f);\n"
     "  s=s*1103515245u+12345u; float c=(float)((s>>8)&0xff);\n"
     "  struct "+p+"F3 r={a,b,c}; return r; }\n",
     {0x44u}, "OptStress71", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress71TC("x64o71", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress71TC("x86o71", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress71TC("a64o71", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress71TC("armo71", "int");

INSTANTIATE_TEST_SUITE_P(OptStress71, X64OptStress71RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress71, X86OptStress71RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress71, A64OptStress71RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress71, ARM32OptStress71RT, ::testing::ValuesIn(kARM), rtTCName);
