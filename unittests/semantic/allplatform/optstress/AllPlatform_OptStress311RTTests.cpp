//===- AllPlatform_OptStress311RTTests.cpp - -O0 indirect i64 return ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O0 kernels calling a `long long`-returning callee THROUGH A FUNCTION POINTER
// — the KNOWN-OPEN #1 defect flagged by OptStress310: a wide (i64) return
// through an INDIRECT-only callee.
//
// On i386/arm32 a `long long` is returned in a register pair (EDX:EAX / R1:R0).
// LowToMed's modelCallWideIntReturn already remodels the *indirect call site*
// (INDIR_CALL) into one producing a 64-bit temp whenever the caller consumes both
// halves straight-line (the -O0 `long long r = fp(...)` spill reads EAX and
// EDX).  But the *callee* return type is widened to i64 only for functions that
// appear as a DIRECT call target (Pipeline's WideRetCallees scan keys on
// `NdOp::CALL` with a constant address).  A function reached *only* through a
// function pointer therefore keeps a low-word-only i32 return, its RETURN
// splices only EAX, and the high word the indirect caller reads from EDX is
// stale → the upper 32 bits of every indirect i64 result are wrong.
//
//   * indll  - i64 result folded via `r ^ (r>>32)` (both halves consumed).
//   * indll2 - i64 result accumulated into a 64-bit running sum (high half
//              flows on, not immediately folded).
//
// The callee uses only 32x32->64 widening multiplies and shifts/xors (no 64-bit
// division) so i386/arm32 stay libcall-free; both halves are genuinely computed
// so a correct lift must thread EDX/R1 through the callee's RETURN.
//
// KNOWN-OPEN (deliberately avoided here — an ORTHOGONAL i386 symbolization
// defect this probe surfaced, left for a focused follow-up):
//
//   The i64 accumulator is seeded from the argument rather than the literal `1`.
//   Initializing it to a small integer literal (`long long sum = 1`) makes the
//   i386 emitter mis-symbolize that constant as a fabricated rodata pointer
//   (`@__nd_data_1.rodata`, even though the object has no .rodata) — the same
//   class of "small constant collides with a relocation-target VA and getVar
//   redirects it to a pointer" defect tracked by #456/#459.  That is a constant-
//   symbolization bug in MedLLVMEmitter, unrelated to the register-pair return
//   path this file exercises, so the seed is kept argument-derived to keep the
//   probe focused on the indirect i64 return.
//
// The entry function is DEFINED FIRST so it lands at the emulation entry
// (CODE_BASE); the pointed-to target follows.  An opaque `asm("" : "+r"(fp))`
// barrier keeps the pointer from being devirtualized so a genuine indirect call
// is emitted.  Deterministic (LCG-seeded).  All four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress311RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress311RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress311RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress311RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress311RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress311RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress311RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress311RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress311TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // i64 result through a function pointer, both halves folded straight-line.
    {p+"_indll",
     "static long long "+p+"_t(int a,int b,int c) __attribute__((noinline));\n"
     +t+" "+p+"_indll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  long long (*fp)(int,int,int) = "+p+"_t; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    long long r = fp((int)w,(int)(w>>5),(int)(w>>11));\n"
     "    acc = acc*131 + (r ^ (r>>32)); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_t(int a,int b,int c){\n"
     "  long long x = (long long)a * (long long)b;\n"
     "  x += (long long)(unsigned)c << 31;\n"
     "  x ^= (long long)(a + c) << 28;\n"
     "  return x - ((long long)(b ^ a) << 35); }\n",
     {0x1234u}, "OptStress311", Opt},

    // i64 result accumulated into a 64-bit running sum (high half flows on).
    // Seed sum from the argument (NOT a small literal — see KNOWN-OPEN above).
    {p+"_indll2",
     "static long long "+p+"_u(int a,int b) __attribute__((noinline));\n"
     +t+" "+p+"_indll2("+t+" a){ unsigned w=(unsigned)a^0xABCu; long long sum=(long long)(unsigned)a;\n"
     "  long long (*fp)(int,int) = "+p+"_u; __asm__(\"\" : \"+r\"(fp));\n"
     "  for(int i=0;i<40;i++){ w=w*22695477u+1u;\n"
     "    sum += fp((int)w,(int)(w>>9)); sum ^= sum>>17; }\n"
     "  return ("+t+")(sum ^ (sum>>32)); }\n"
     "static long long "+p+"_u(int a,int b){\n"
     "  long long x = (long long)a * (long long)b;\n"
     "  x ^= (long long)(a ^ b) << 34;\n"
     "  return x + ((long long)(a | 1) << 30); }\n",
     {0x55AAu}, "OptStress311", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress311TC("x64o311", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress311TC("x86o311", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress311TC("a64o311", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress311TC("armo311", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress311, X64OptStress311RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress311, X86OptStress311RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress311, A64OptStress311RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress311, ARM32OptStress311RT, ::testing::ValuesIn(kARM), rtTCName);
