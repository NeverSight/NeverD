//===- AllPlatform_OptStress314RTTests.cpp - i386 const-store coverage ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green-guardrail follow-up to OptStress313 (#520): the same i386 PIC condition
// that mis-symbolized a stored small literal — an executable `.text` segment
// that carries a GOTOFF function-pointer reloc slot, so `segHasPtrRelocSlots`
// is true for code — exercised across MORE store contexts than #313's single
// accumulator-seed store:
//
//   * arglit / argind - a small integer LITERAL passed as a call ARGUMENT
//                       (i386 cdecl spills it to the outgoing-arg stack slot, a
//                       STORE of the literal value).
//   * gblw            - small literals stored into a WRITABLE global array and
//                       read back (the writable-data store path).
//   * seedfff         - accumulator seeded with 0xFFF, the value just below the
//                       kMinGlobalDataAddr=0x1000 symbolization threshold.
//   * seedmix         - several distinct small-literal seeds in one kernel.
//   * twofp           - two live function pointers (two GOTOFF reloc slots) plus
//                       a small-literal seed.
//
// Every kernel keeps at least one `static noinline` callee address live through
// an `asm("" : "+r"(fp))` barrier, so the GOTOFF code-pointer reloc slot exists
// in .text (without it the segment carries no pointer slots and the defect class
// cannot surface).  All stored small literals must stay plain integers after the
// #520 fix (`!segIsExecutable` gate on the vtable-base store-value redirect);
// before it they were embedded as fabricated `@__nd_data_*.rodata` code-byte
// globals.  i386 is the target under test; x86-64/aarch64/arm32 are controls.
// i64 math is multiply/shift/xor/add only (libcall-free on 32-bit).  -O0, all
// four targets, LCG-seeded / deterministic.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress314RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress314RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress314RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress314RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress314RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress314RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress314RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress314RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress314TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Small integer LITERAL passed as an INDIRECT-call argument (i386 cdecl
    // spills the literal to the outgoing-arg stack slot).
    {p+"_argind",
     "static int "+p+"_ta(int a,int b,int c) __attribute__((noinline));\n"
     +t+" "+p+"_argind("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  int (*fp)(int,int,int) = "+p+"_ta; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc += (long long)fp(1,(int)w,2) - (long long)fp(3,(int)(w>>5),5);\n"
     "    acc ^= acc>>21; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_ta(int a,int b,int c){ return a*97 + b*3 - c*7 + (b^c); }\n",
     {0x1234u}, "OptStress314", Opt},

    // Small literals stored into a WRITABLE global array, read back indexed.
    {p+"_gblw",
     "static int "+p+"_tg(int a,int b) __attribute__((noinline));\n"
     "static int "+p+"_GW[8];\n"
     +t+" "+p+"_gblw("+t+" a){ unsigned w=(unsigned)a^0x77u; long long acc=0;\n"
     "  int (*fp)(int,int) = "+p+"_tg; __asm__(\"\" : \"+r\"(fp));\n"
     "  "+p+"_GW[0]=1; "+p+"_GW[1]=2; "+p+"_GW[2]=3; "+p+"_GW[3]=7;\n"
     "  for(int i=0;i<40;i++){ w=w*22695477u+1u;\n"
     "    "+p+"_GW[4+(i&3)] = (int)w + "+p+"_GW[i&3];\n"
     "    acc += (long long)fp((int)"+p+"_GW[(i+1)&7],(int)w) + "+p+"_GW[(i+2)&7];\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_tg(int a,int b){ return a*5 - b*2 + (a&0x3ff); }\n",
     {0x2345u}, "OptStress314", Opt},

    // Accumulator seeded with 0xFFF — just below the kMinGlobalDataAddr threshold.
    {p+"_seedfff",
     "static int "+p+"_tf(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_seedfff("+t+" a){ unsigned w=(unsigned)a+0x5u; long long acc=0xFFF;\n"
     "  int (*fp)(int,int) = "+p+"_tf; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*214013u+2531011u;\n"
     "    acc += (long long)fp((int)w,(int)(w>>13));\n"
     "    acc = acc*3 + 0x800; acc ^= acc>>27; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_tf(int a,int b){ return a - b*11 + (a^(b<<3)); }\n",
     {0x3456u}, "OptStress314", Opt},

    // Several distinct small-literal seeds combined in one kernel.
    {p+"_seedmix",
     "static int "+p+"_tx(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_seedmix("+t+" a){ unsigned w=(unsigned)a^0x9u;\n"
     "  long long x=1; long long y=2; long long z=5;\n"
     "  int (*fp)(int,int) = "+p+"_tx; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<32;i++){ w=w*1664525u+1013904223u;\n"
     "    long long r=(long long)fp((int)w,(int)(w>>7));\n"
     "    x += r; y = y*3 + (r^x); z ^= (z<<5) + r;\n"
     "    x ^= x>>29; y ^= y>>31; z ^= z>>23; }\n"
     "  return ("+t+")((x ^ y ^ z) ^ ((x ^ y ^ z)>>32)); }\n"
     "static int "+p+"_tx(int a,int b){ return a*4 - b + (a&b) + 1; }\n",
     {0x55AAu}, "OptStress314", Opt},

    // Two live function pointers (two GOTOFF reloc slots) + small-literal seed.
    {p+"_twofp",
     "static int "+p+"_p1(int a,int b) __attribute__((noinline));\n"
     "static int "+p+"_p2(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_twofp("+t+" a){ unsigned w=(unsigned)a|3u; long long acc=3;\n"
     "  int (*f1)(int,int) = "+p+"_p1; __asm__(\"\" : \"+r\"(f1));\n"
     "  int (*f2)(int,int) = "+p+"_p2; __asm__(\"\" : \"+r\"(f2));\n"
     "  for(int i=0;i<40;i++){ w=w*22695477u+1u;\n"
     "    acc += (long long)f1((int)w,(int)(w>>9)) ^ (long long)f2((int)(w>>3),(int)w);\n"
     "    acc ^= acc>>25; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_p1(int a,int b){ return a*7 - b*2 + (a|1); }\n"
     "static int "+p+"_p2(int a,int b){ return b*5 - a*3 + (b&0x7f); }\n",
     {0x6789u}, "OptStress314", Opt},

    // Small literal both seeded AND passed as an argument, mixed.
    {p+"_argseed",
     "static int "+p+"_ts(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_argseed("+t+" a){ unsigned w=(unsigned)a+0x21u; long long acc=1;\n"
     "  int (*fp)(int,int) = "+p+"_ts; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*1103515245u+12345u;\n"
     "    acc += (long long)fp((int)(acc&0xff)+1,(int)w);\n"
     "    acc = acc*131 + 7; acc ^= acc>>28; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_ts(int a,int b){ return a*9 - b + (a^(b>>1)); }\n",
     {0x78ABu}, "OptStress314", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress314TC("x64o314", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress314TC("x86o314", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress314TC("a64o314", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress314TC("armo314", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress314, X64OptStress314RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress314, X86OptStress314RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress314, A64OptStress314RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress314, ARM32OptStress314RT, ::testing::ValuesIn(kARM), rtTCName);
