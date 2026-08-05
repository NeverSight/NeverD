//===- AllPlatform_OptStress315RTTests.cpp - -O0 indirect i64 ARGUMENT ----===//
//
// -O0 kernels passing a 64-bit ARGUMENT THROUGH A FUNCTION POINTER — the exact
// DUAL of the indirect i64 RETURN defect OptStress310 flagged and OptStress311
// closed.  #311 fixed the callee-side: a `long long`-returning function reached
// only through a pointer kept a low-word-only i32 return because Pipeline's
// WideRetCallees scan keys on `NdOp::CALL` with a constant address, so an
// indirect-only callee was never return-widened.  The symmetric question this
// probe asks is the ARGUMENT direction: when a `long long` is PASSED to an
// indirect-only callee, does both the caller's argument split and the callee's
// 64-bit parameter recovery survive the roundtrip?
//
// On i386 a `long long` argument occupies two consecutive cdecl stack slots; on
// arm32 (AAPCS) it occupies an even/odd register pair (r0:r1 or r2:r3) and, when
// it cannot fit, spills as an 8-byte aligned pair to the outgoing-argument area.
// A lift that recovers only the low word of an indirect-call argument — or whose
// indirect-only callee reads a 64-bit parameter as i32 — yields a wrong high
// half on i386/arm32 while x86-64/aarch64 (which carry the i64 in one register)
// stay correct.
//
//   * argll  - one i64 arg + int, both halves consumed by the callee.
//   * arg2ll - two i64 args (four 32-bit halves) through the pointer.
//   * argmix - int, i64, int: arm32's even-pair alignment hole (r0, r2:r3, stk).
//   * retll  - i64 arg AND i64 return through the SAME indirect call (stresses
//              the #311 return path and the argument path together).
//   * argstk - int,int,int,i64: the i64 spills to the arm32 outgoing-arg area.
//   * argdir - direct-call control (pointer kept live by the asm barrier so the
//              GOTOFF reloc slot still exists, but the call is DIRECT) — proves
//              any argument-recovery fix is orthogonal to call form.
//
// Each kernel keeps the function pointer live through an `asm("" : "+r"(fp))`
// barrier so the indirect call is genuinely emitted (not devirtualized), seeds
// its accumulator from the ARGUMENT (not a literal) to stay orthogonal to the
// #518/#520 constant-symbolization family, and uses only multiply/shift/xor/add
// i64 math (no 64-bit division) so i386/arm32 stay libcall-free.  Every callee
// genuinely consumes every half of every wide argument, so a dropped high word
// changes the result.  x86-64/aarch64 are controls.  Deterministic (LCG-seeded),
// -O0, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress315RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress315RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress315RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress315RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress315RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress315RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress315RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress315RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress315TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // One i64 argument (+ int) through a function pointer; both halves used.
    {p+"_argll",
     "static int "+p+"_ta(long long x,int b) __attribute__((noinline));\n"
     +t+" "+p+"_argll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=(long long)(unsigned)a;\n"
     "  int (*fp)(long long,int) = "+p+"_ta; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    long long arg = ((long long)w) | ((long long)(w ^ 0x9E3779B9u) << 32);\n"
     "    acc = acc*131 + (long long)fp(arg,(int)(w>>7));\n"
     "    acc ^= acc>>21; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_ta(long long x,int b){\n"
     "  unsigned lo=(unsigned)x, hi=(unsigned)((unsigned long long)x>>32);\n"
     "  return (int)(lo*3u) - (int)(hi*5u) + (int)(x>>40) + b*2; }\n",
     {0x1234u}, "OptStress315", Opt},

    // Two i64 arguments (four 32-bit halves) through a function pointer.
    {p+"_arg2ll",
     "static int "+p+"_tb(long long x,long long y) __attribute__((noinline));\n"
     +t+" "+p+"_arg2ll("+t+" a){ unsigned w=(unsigned)a^0x33u; long long acc=(long long)(unsigned)a;\n"
     "  int (*fp)(long long,long long) = "+p+"_tb; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*22695477u+1u;\n"
     "    long long x=((long long)w)|((long long)(w>>3)<<32);\n"
     "    long long y=((long long)(w>>5))|((long long)(w^0x55u)<<32);\n"
     "    acc += (long long)fp(x,y); acc ^= acc>>19; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_tb(long long x,long long y){\n"
     "  unsigned xl=(unsigned)x, xh=(unsigned)((unsigned long long)x>>32);\n"
     "  unsigned yl=(unsigned)y, yh=(unsigned)((unsigned long long)y>>32);\n"
     "  return (int)(xl - yh*3u + yl*7u - xh*5u) + (int)((x^y)>>40); }\n",
     {0x2345u}, "OptStress315", Opt},

    // int, i64, int — arm32's even/odd register-pair alignment hole.
    {p+"_argmix",
     "static int "+p+"_tc(int q,long long r,int s) __attribute__((noinline));\n"
     +t+" "+p+"_argmix("+t+" a){ unsigned w=(unsigned)a+0x9u; long long acc=(long long)(unsigned)a;\n"
     "  int (*fp)(int,long long,int) = "+p+"_tc; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*1664525u+1013904223u;\n"
     "    long long r=((long long)(w>>2))|((long long)(w>>11)<<32);\n"
     "    acc = acc*131 + (long long)fp((int)w,r,(int)(w>>17));\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_tc(int q,long long r,int s){\n"
     "  unsigned rl=(unsigned)r, rh=(unsigned)((unsigned long long)r>>32);\n"
     "  return q*3 - s*7 + (int)(rl) - (int)(rh*5u) + (int)(r>>40); }\n",
     {0x3456u}, "OptStress315", Opt},

    // i64 ARGUMENT and i64 RETURN through the SAME indirect call.
    {p+"_retll",
     "static long long "+p+"_td(long long x,int b) __attribute__((noinline));\n"
     +t+" "+p+"_retll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=(long long)(unsigned)a;\n"
     "  long long (*fp)(long long,int) = "+p+"_td; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    long long x=((long long)w)|((long long)(w>>4)<<32);\n"
     "    long long r=fp(x,(int)(w>>9));\n"
     "    acc = acc*131 + (r ^ (r>>32)); acc ^= acc>>17; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_td(long long x,int b){\n"
     "  long long r=x*131 + (long long)b;\n"
     "  r ^= (long long)(unsigned)((unsigned long long)x>>32) << 28;\n"
     "  r += ((long long)b) << 33; return r; }\n",
     {0x4567u}, "OptStress315", Opt},

    // int,int,int,i64 — the i64 spills to the arm32 outgoing-argument area.
    {p+"_argstk",
     "static int "+p+"_te(int a,int b,int c,long long d) __attribute__((noinline));\n"
     +t+" "+p+"_argstk("+t+" a){ unsigned w=(unsigned)a^0x5Au; long long acc=(long long)(unsigned)a;\n"
     "  int (*fp)(int,int,int,long long) = "+p+"_te; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*22695477u+1u;\n"
     "    long long d=((long long)(w>>1))|((long long)(w>>13)<<32);\n"
     "    acc = acc*131 + (long long)fp((int)w,(int)(w>>3),(int)(w>>7),d);\n"
     "    acc ^= acc>>25; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_te(int a,int b,int c,long long d){\n"
     "  unsigned dl=(unsigned)d, dh=(unsigned)((unsigned long long)d>>32);\n"
     "  return a*2 - b*3 + c*5 + (int)dl - (int)(dh*7u) + (int)(d>>40); }\n",
     {0x55AAu}, "OptStress315", Opt},

    // Direct-call control: pointer kept live (GOTOFF reloc slot exists) but the
    // call is DIRECT — the argument-recovery behavior must match the indirect
    // form.
    {p+"_argdir",
     "static int "+p+"_tf(long long x,int b) __attribute__((noinline));\n"
     +t+" "+p+"_argdir("+t+" a){ unsigned w=(unsigned)a+0x11u; long long acc=(long long)(unsigned)a;\n"
     "  int (*fp)(long long,int) = "+p+"_tf; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<32;i++){ w=w*22695477u+1u;\n"
     "    long long x=((long long)w)|((long long)(w>>6)<<32);\n"
     "    acc += (long long)"+p+"_tf(x,(int)(w>>3)); acc ^= acc>>21; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_tf(long long x,int b){\n"
     "  unsigned lo=(unsigned)x, hi=(unsigned)((unsigned long long)x>>32);\n"
     "  return (int)(lo*6u) - (int)(hi*2u) + (int)(x>>40) + b; }\n",
     {0x6789u}, "OptStress315", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress315TC("x64o315", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress315TC("x86o315", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress315TC("a64o315", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress315TC("armo315", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress315, X64OptStress315RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress315, X86OptStress315RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress315, A64OptStress315RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress315, ARM32OptStress315RT, ::testing::ValuesIn(kARM), rtTCName);
