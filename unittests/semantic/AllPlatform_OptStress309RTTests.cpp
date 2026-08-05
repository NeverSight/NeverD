//===- AllPlatform_OptStress309RTTests.cpp - -O0 struct-return variety ---===//
//
// -O0 kernels exercising struct RETURN-by-value in many shapes — hardening the
// path #516 fixed (i386 returns a struct through a hidden sret pointer the
// callee POPS via `ret $4`; the caller's post-call stack-pointer fixup must
// thread the one SP SSA variable or a loop-carried SP drifts).  Each kernel
// returns a small aggregate in a loop and consumes its fields, across struct
// sizes / shapes and through nested and back-to-back returning calls:
//
//   * r3i   - returns struct{int,int,int} (12B: i386 sret; x86-64 two eightbytes).
//   * r1ll  - returns struct{long long} (8B: a single wide field).
//   * s2io  - struct{int,int} IN and struct{int,int} OUT (aggregate arg + ret).
//   * rnest - a struct-returning callee that itself calls a struct-returning
//             callee (nested hidden-sret frames on i386).
//   * r2x   - two struct-returning calls back-to-back feeding one accumulator.
//   * rc4   - returns struct{char,char,char,char} (4B: sub-word packed fields).
//
// The entry function is DEFINED FIRST so it lands at the emulation entry
// (CODE_BASE); the helpers follow.  All 64-bit math is multiply-by-constant /
// shift / add / logic only (no 64-bit division) to stay libcall-free on
// i386/arm32; structs are kept ≤16 bytes so -O0 copies them field-by-field
// rather than emitting memcpy.  Deterministic (LCG-seeded).  All four, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress309RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress309RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress309RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress309RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress309RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress309RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress309RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress309RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress309TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // returns struct{int,int,int} (12B): i386 hidden sret pointer; x86-64 two
    // eightbytes returned in RAX:RDX.
    {p+"_r3i",
     "struct S3{int a,b,c;};\n"
     "static struct S3 "+p+"_h(unsigned w) __attribute__((noinline));\n"
     +t+" "+p+"_r3i("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S3 r = "+p+"_h(w);\n"
     "    acc = acc*131 + (long long)r.a - (long long)r.b + (long long)r.c; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static struct S3 "+p+"_h(unsigned w){ struct S3 r; r.a=(int)(w>>3); r.b=(int)(w*2654435761u); r.c=(int)(w^0x55u); return r; }\n",
     {0x1234u}, "OptStress309", Opt},

    // returns struct{long long} (8B single wide field).
    {p+"_r1ll",
     "struct SL{long long v;};\n"
     "static struct SL "+p+"_h(unsigned w) __attribute__((noinline));\n"
     +t+" "+p+"_r1ll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct SL r = "+p+"_h(w);\n"
     "    acc = acc*131 + r.v; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static struct SL "+p+"_h(unsigned w){ struct SL r; r.v=((long long)(unsigned)w<<20) ^ ((long long)(int)w); return r; }\n",
     {0x2345u}, "OptStress309", Opt},

    // struct{int,int} IN and struct{int,int} OUT: aggregate argument + return.
    {p+"_s2io",
     "struct S2{int a,b;};\n"
     "static struct S2 "+p+"_h(struct S2 s) __attribute__((noinline));\n"
     +t+" "+p+"_s2io("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 s; s.a=(int)w; s.b=(int)(w>>5);\n"
     "    struct S2 r = "+p+"_h(s);\n"
     "    acc = acc*131 + (long long)r.a - (long long)r.b; }\n"
     "  return ("+t+")acc; }\n"
     "static struct S2 "+p+"_h(struct S2 s){ struct S2 r; r.a=s.a*7 - s.b; r.b=s.a ^ s.b; return r; }\n",
     {0x3456u}, "OptStress309", Opt},

    // nested struct-returning calls: a struct-returning callee itself calls a
    // struct-returning callee (stacked hidden-sret frames on i386).
    {p+"_rnest",
     "struct S2{int a,b;};\n"
     "static struct S2 "+p+"_in(unsigned w) __attribute__((noinline));\n"
     "static struct S2 "+p+"_out(unsigned w) __attribute__((noinline));\n"
     +t+" "+p+"_rnest("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 r = "+p+"_out(w);\n"
     "    acc = acc*131 + (long long)r.a - (long long)r.b; }\n"
     "  return ("+t+")acc; }\n"
     "static struct S2 "+p+"_in(unsigned w){ struct S2 r; r.a=(int)(w>>4); r.b=(int)(w*131u); return r; }\n"
     "static struct S2 "+p+"_out(unsigned w){ struct S2 t="+p+"_in(w ^ 0x1234u); struct S2 r; r.a=t.a + t.b; r.b=t.a - t.b; return r; }\n",
     {0x4567u}, "OptStress309", Opt},

    // two struct-returning calls back-to-back feeding one accumulator.
    {p+"_r2x",
     "struct S2{int a,b;};\n"
     "static struct S2 "+p+"_f1(unsigned w) __attribute__((noinline));\n"
     "static struct S2 "+p+"_f2(unsigned w) __attribute__((noinline));\n"
     +t+" "+p+"_r2x("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 x = "+p+"_f1(w);\n"
     "    struct S2 y = "+p+"_f2(w ^ 0xABCDu);\n"
     "    acc = acc*131 + (long long)x.a - (long long)y.b + (long long)x.b - (long long)y.a; }\n"
     "  return ("+t+")acc; }\n"
     "static struct S2 "+p+"_f1(unsigned w){ struct S2 r; r.a=(int)(w>>2); r.b=(int)(w*7u); return r; }\n"
     "static struct S2 "+p+"_f2(unsigned w){ struct S2 r; r.a=(int)(w*3u); r.b=(int)(w>>6); return r; }\n",
     {0x5678u}, "OptStress309", Opt},

    // returns struct{char,char,char,char} (4B sub-word packed fields).
    {p+"_rc4",
     "struct SC{char a,b,c,d;};\n"
     "static struct SC "+p+"_h(unsigned w) __attribute__((noinline));\n"
     +t+" "+p+"_rc4("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct SC r = "+p+"_h(w);\n"
     "    acc = acc*131 + (long long)(signed char)r.a - (long long)(unsigned char)r.b\n"
     "        + (long long)(signed char)r.c - (long long)(unsigned char)r.d; }\n"
     "  return ("+t+")acc; }\n"
     "static struct SC "+p+"_h(unsigned w){ struct SC r; r.a=(char)w; r.b=(char)(w>>8); r.c=(char)(w>>16); r.d=(char)(w>>24); return r; }\n",
     {0x6789u}, "OptStress309", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress309TC("x64o309", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress309TC("x86o309", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress309TC("a64o309", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress309TC("armo309", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress309, X64OptStress309RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress309, X86OptStress309RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress309, A64OptStress309RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress309, ARM32OptStress309RT, ::testing::ValuesIn(kARM), rtTCName);
