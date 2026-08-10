//===- AllPlatform_OptStress308RTTests.cpp - -O0 by-value structs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O0 kernels passing and returning small aggregates BY VALUE — a distinct ABI
// path from the scalar argument probes 305/306/307: a by-value struct is laid
// across one or more argument slots (lane-split on the caller, reassembled on
// the callee), and a small struct return uses the register-pair result path
// rather than a hidden sret pointer.  At -O0 each field is copied individually
// into / out of the outgoing slot area (no memcpy for these ≤16-byte shapes),
// so the lift must reconstruct each per-field store as the right argument lane:
//
//   * bs2    - struct{int,int} by value (one 8-byte / two-slot aggregate arg).
//   * bs4    - struct{int,int,int,int} by value (16-byte / four-slot arg).
//   * bs2x2  - two struct{int,int} args (adjacent multi-slot aggregates).
//   * bsm    - struct{short,short,int} by value (sub-word fields + padding).
//   * bs2sc  - f(int, struct{int,int}, int): an aggregate between two scalars.
//   * bs2ret - returns struct{int,int} by value in a LOOP.  On i386 SysV a
//              struct is returned through a hidden sret pointer the callee POPS
//              (`ret $4`); #516 found the caller's post-call `+imm` SP fixup was
//              threaded as a foreign SSA variable, so the loop-carried stack
//              pointer drifted each iteration (outgoing-arg base ran off the
//              frame -> unmapped access).  Other targets return the small struct
//              in a register pair.
//
// The entry function is DEFINED FIRST so it lands at the emulation entry
// (CODE_BASE); the helpers follow.  All 64-bit math is multiply-by-constant /
// shift / add / logic only (no 64-bit division) to stay libcall-free on
// i386/arm32; structs are kept ≤16 bytes so -O0 copies them field-by-field
// rather than emitting memcpy.  Deterministic (LCG-seeded).  All four, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress308RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress308RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress308RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress308RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress308RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress308RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress308RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress308RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress308TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // struct{int,int} by value: a two-slot aggregate argument.
    {p+"_bs2",
     "struct S2{int a,b;};\n"
     "static int "+p+"_h(struct S2 s) __attribute__((noinline));\n"
     +t+" "+p+"_bs2("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 s; s.a=(int)w; s.b=(int)(w>>7);\n"
     "    acc = acc*131 + "+p+"_h(s); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h(struct S2 s){ return s.a*7 - s.b*3 + (s.a^s.b); }\n",
     {0x1234u}, "OptStress308", Opt},

    // struct{int,int,int,int} by value: a four-slot 16-byte aggregate argument.
    {p+"_bs4",
     "struct S4{int a,b,c,d;};\n"
     "static long long "+p+"_h(struct S4 s) __attribute__((noinline));\n"
     +t+" "+p+"_bs4("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S4 s; s.a=(int)w; s.b=(int)(w>>3); s.c=(int)(w>>9); s.d=(int)(w>>15);\n"
     "    acc = acc*131 + "+p+"_h(s); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_h(struct S4 s){\n"
     "  return (long long)s.a + (long long)s.b*3 - (long long)s.c*5 + (long long)s.d*7; }\n",
     {0x2345u}, "OptStress308", Opt},

    // two struct{int,int} args: adjacent multi-slot aggregates in one list.
    {p+"_bs2x2",
     "struct S2{int a,b;};\n"
     "static int "+p+"_h(struct S2 x, struct S2 y) __attribute__((noinline));\n"
     +t+" "+p+"_bs2x2("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 x,y; x.a=(int)w; x.b=(int)(w>>5); y.a=(int)(w>>11); y.b=(int)(w>>17);\n"
     "    acc = acc*131 + "+p+"_h(x,y); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h(struct S2 x, struct S2 y){ return x.a - x.b + y.a*3 - y.b*5; }\n",
     {0x3456u}, "OptStress308", Opt},

    // struct{short,short,int} by value: sub-word fields packed in the slots.
    {p+"_bsm",
     "struct SM{short a; short b; int c;};\n"
     "static int "+p+"_h(struct SM s) __attribute__((noinline));\n"
     +t+" "+p+"_bsm("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct SM s; s.a=(short)(w>>3); s.b=(short)(w>>13); s.c=(int)w;\n"
     "    acc = acc*131 + "+p+"_h(s); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h(struct SM s){ return (int)s.a*11 - (int)s.b*5 + s.c; }\n",
     {0x4567u}, "OptStress308", Opt},

    // f(int, struct{int,int}, int): an aggregate argument between two scalars.
    {p+"_bs2sc",
     "struct S2{int a,b;};\n"
     "static int "+p+"_h(int n, struct S2 s, int m) __attribute__((noinline));\n"
     +t+" "+p+"_bs2sc("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 s; s.a=(int)(w>>2); s.b=(int)(w>>10);\n"
     "    acc = acc*131 + "+p+"_h((int)w, s, (int)(w>>20)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h(int n, struct S2 s, int m){ return n - s.a*3 + s.b*5 - m; }\n",
     {0x5678u}, "OptStress308", Opt},

    // returns struct{int,int} by value in a loop: i386 returns it through a
    // hidden sret pointer the callee pops (`ret $4`), so the caller's post-call
    // SP fixup must thread the one stack-pointer SSA variable or the loop-carried
    // SP drifts (the #516 fix); other targets use the register-pair return.
    {p+"_bs2ret",
     "struct S2{int a,b;};\n"
     "static struct S2 "+p+"_h(unsigned w) __attribute__((noinline));\n"
     +t+" "+p+"_bs2ret("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    struct S2 r = "+p+"_h(w);\n"
     "    acc = acc*131 + (long long)r.a - (long long)r.b; }\n"
     "  return ("+t+")acc; }\n"
     "static struct S2 "+p+"_h(unsigned w){ struct S2 r; r.a=(int)(w>>4); r.b=(int)(w*2654435761u); return r; }\n",
     {0x6789u}, "OptStress308", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress308TC("x64o308", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress308TC("x86o308", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress308TC("a64o308", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress308TC("armo308", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress308, X64OptStress308RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress308, X86OptStress308RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress308, A64OptStress308RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress308, ARM32OptStress308RT, ::testing::ValuesIn(kARM), rtTCName);
