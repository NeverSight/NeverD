//===- AllPlatform_OptStress313RTTests.cpp - i386 small-literal symbolize -===//
//
// Roots out the KNOWN-OPEN i386 constant-symbolization defect flagged by
// OptStress311 (#518): a SMALL INTEGER LITERAL seed (`long long sum = 1`) is
// mis-symbolized into a fabricated rodata pointer (`@__nd_data_1.rodata`) on
// i386, so the accumulator starts at a stale code/data address instead of 1 and
// every result is wrong.  OptStress311's indll2 deliberately seeded its
// accumulator from the argument to AVOID this; here we seed from the literal so
// the defect is exercised head-on.
//
// Root cause (MedLLVMEmitter STORE-value symbolization): i386 PIC takes a static
// function's address via `leal fn@GOTOFF(%ebx)`, whose relocation slot the
// loader records in CodePtrRelocSlots.  That slot sits INSIDE the executable
// `.text` segment, so `segHasPtrRelocSlots(.text)` is true.  The store-value
// "vtable / dispatch-table base" path then redirected ANY stored integer whose
// value merely equals a low .text VA — the literal seed `1`, or the i386 PIC
// get-PC constant `0x0D` (`pop %eax` after `call .+0`) — into a fabricated data
// global, embedding raw code bytes as `.rodata` and corrupting the value.  A
// vtable base lives in read-only DATA, never executable code, so the fix gates
// that redirect on `!segIsExecutable` (the #456/#459/#499 reloc-collision
// family).
//
// Each kernel takes the address of a `static noinline` callee through a function
// pointer (the GOTOFF that creates the .text code-pointer reloc slot — without
// it the segment carries no pointer slots and the defect cannot surface) and
// seeds a 64-bit accumulator with a SMALL INTEGER LITERAL that genuinely flows
// into the folded result.  i386 is the target under test; x86-64/aarch64/arm32
// are controls (no PIC GOTOFF-into-.text form, so they were always green).  The
// i64 math is multiply/shift/xor/add only (no 64-bit division) to stay libcall-
// free on the 32-bit targets.  Deterministic (LCG-seeded), -O0, all four
// targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress313RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress313RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress313RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress313RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress313RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress313RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress313RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress313RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress313TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // long long sum = 1 — the exact #518 KNOWN-OPEN trigger.
    {p+"_litsum",
     "static int "+p+"_ts(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_litsum("+t+" a){ unsigned w=(unsigned)a|1u; long long sum=1;\n"
     "  int (*fp)(int,int) = "+p+"_ts; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    sum += fp((int)w,(int)(w>>7));\n"
     "    sum ^= sum>>19; }\n"
     "  return ("+t+")(sum ^ (sum>>32)); }\n"
     "static int "+p+"_ts(int a,int b){ return a*3 - b*5 + (a^b); }\n",
     {0x1234u}, "OptStress313", Opt},

    // long long acc = 2 — small literal as a running 64-bit base, shifted high.
    {p+"_litp2",
     "static int "+p+"_t2(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_litp2("+t+" a){ unsigned w=(unsigned)a^0x33u; long long acc=2;\n"
     "  int (*fp)(int,int) = "+p+"_t2; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*22695477u+1u;\n"
     "    acc += (long long)fp((int)w,(int)(w>>11)) << 13;\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc + (acc>>32)); }\n"
     "static int "+p+"_t2(int a,int b){ return a - b*7 + (a&b); }\n",
     {0x2345u}, "OptStress313", Opt},

    // long long m = 3 — small literal seed multiplied each iteration.
    {p+"_litp3",
     "static int "+p+"_t3(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_litp3("+t+" a){ unsigned w=(unsigned)a+0x9u; long long m=3;\n"
     "  int (*fp)(int,int) = "+p+"_t3; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*1664525u+1013904223u;\n"
     "    m = m*131 + (long long)fp((int)w,(int)(w>>5));\n"
     "    m ^= m>>29; }\n"
     "  return ("+t+")(m ^ (m>>32)); }\n"
     "static int "+p+"_t3(int a,int b){ return a*5 + b*3 - (a^(b<<1)); }\n",
     {0x3456u}, "OptStress313", Opt},

    // long long h = 5 — small literal seed in an xor/shift chain.
    {p+"_litp5",
     "static int "+p+"_t5(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_litp5("+t+" a){ unsigned w=(unsigned)a|7u; long long h=5;\n"
     "  int (*fp)(int,int) = "+p+"_t5; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*214013u+2531011u;\n"
     "    h ^= (long long)fp((int)w,(int)(w>>17)) << 7;\n"
     "    h += h<<11; h ^= h>>21; }\n"
     "  return ("+t+")(h ^ (h>>32)); }\n"
     "static int "+p+"_t5(int a,int b){ return (a|1)*9 - b*4 + (a&0x7f); }\n",
     {0x4567u}, "OptStress313", Opt},

    // Two small-literal seeds (1 and 7) combined — both must stay integers.
    {p+"_litmix",
     "static int "+p+"_tm(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_litmix("+t+" a){ unsigned w=(unsigned)a^0x5Au;\n"
     "  long long s=1; long long g=7;\n"
     "  int (*fp)(int,int) = "+p+"_tm; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<36;i++){ w=w*1103515245u+12345u;\n"
     "    long long r=(long long)fp((int)w,(int)(w>>9));\n"
     "    s += r; g = g*3 + (r ^ s); s ^= s>>27; g ^= g>>31; }\n"
     "  return ("+t+")((s ^ g) ^ ((s ^ g)>>32)); }\n"
     "static int "+p+"_tm(int a,int b){ return a*2 - b + (a^(b>>2)); }\n",
     {0x55AAu}, "OptStress313", Opt},

    // Direct-call variant: the callee's ADDRESS is still taken (function pointer
    // kept live by the asm barrier) so the GOTOFF code-pointer reloc slot exists
    // in .text, but the call goes DIRECT — proves the symbolization fix is
    // orthogonal to call form.  The pointer value itself is never folded into the
    // result (it would differ after relink); only the small literal seed and the
    // call results do.
    {p+"_litdir",
     "static int "+p+"_td(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_litdir("+t+" a){ unsigned w=(unsigned)a+0x11u; long long acc=1;\n"
     "  int (*fp)(int,int) = "+p+"_td; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<32;i++){ w=w*22695477u+1u;\n"
     "    acc += (long long)"+p+"_td((int)w,(int)(w>>3));\n"
     "    acc ^= acc>>25; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_td(int a,int b){ return a*6 - b*2 + (a&b); }\n",
     {0x6789u}, "OptStress313", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress313TC("x64o313", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress313TC("x86o313", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress313TC("a64o313", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress313TC("armo313", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress313, X64OptStress313RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress313, X86OptStress313RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress313, A64OptStress313RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress313, ARM32OptStress313RT, ::testing::ValuesIn(kARM), rtTCName);
