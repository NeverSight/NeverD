//===- AllPlatform_OptStress304RTTests.cpp - -O0 return-width merge ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O0 sink-difference dual of OptStress303: the SAME return-type-inference
// kernels compiled at -O0.  Low optimization changes the lift surface that
// MedTypePass::inferReturnType / intRetEffWidth must reconstruct: at -O2 the
// mixed-width return paths coalesce into one return-register PHI in a merged
// epilogue (the #514 shape), whereas at -O0 each `return` stores its value to a
// stack slot and the single epilogue *reloads* it, with the narrow paths'
// sign/zero-extend emitted as explicit movzx/movsx (x86) / uxt*/sxt* (arm) into
// the return register.  A high-word drop from a mis-inferred narrow return type
// therefore surfaces through a different instruction shape than 303 (cf. the -O0
// duals in #508/#509/#512 and the 299/302 pairs).
//
// Each function returns the platform's natural width (`long` on x86-64/aarch64,
// `int` on i386/arm32).  On the 64-bit targets the harness reads the full
// RAX/X0, so a dropped high word shows up directly; on the 32-bit targets the
// wide intermediate is truncated into the 32-bit return (EAX / R0:R1 pair).  All
// 64-bit math is multiply/shift/add/logic only (no 64-bit division) to stay
// libcall-free on i386/arm32.  Deterministic (LCG-seeded).  All four, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress304RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress304RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress304RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress304RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress304RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress304RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress304RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress304RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress304TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // i64 accumulator vs sibling `return 0` (the #514 bug-(1) shape).
    {p+"_phi0",
     t+" "+p+"_phi0("+t+" a){ unsigned long long acc=(unsigned)a|1ULL;\n"
     "  for(int i=0;i<40;i++){ acc=acc*6364136223846793005ULL+(unsigned)i;\n"
     "    if(((acc>>17)&0x3FF)==0x123) return ("+t+")0; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234u}, "OptStress304", Opt},

    // Three return paths of different widths reaching one epilogue.
    {p+"_three",
     t+" "+p+"_three("+t+" a){ unsigned long long acc=(unsigned)a^0xA5A5A5A5u; unsigned w=(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ acc=acc*2862933555777941757ULL+3037000493ULL; w=w*1103515245u+12345u;\n"
     "    if((w&0xFFF)==0x7A1) return ("+t+")(int)(w>>8);\n"
     "    if((w&0xFFF)==0x3C2) return ("+t+")0; }\n"
     "  return ("+t+")(acc ^ (acc>>29)); }\n",
     {0x2345u}, "OptStress304", Opt},

    // Signed 64-bit accumulator merged with a signed narrow path (sign matters
    // when folded/extended into the return register).
    {p+"_signed",
     t+" "+p+"_signed("+t+" a){ long long acc=-(long long)((unsigned)a|1u);\n"
     "  for(int i=0;i<40;i++){ acc=acc*1103515245LL-12345LL+(i&7);\n"
     "    if(((acc>>11)&0x1FF)==0x55) return ("+t+")(signed char)(acc>>3); }\n"
     "  return ("+t+")acc; }\n",
     {0x3456u}, "OptStress304", Opt},

    // Recursion returning a 64-bit value with a narrow base case (#508/#514
    // wide-return-through-merge shape; i386/arm32 use the EDX:EAX / R0:R1 pair).
    {p+"_recur",
     "static unsigned long long "+p+"_rec(unsigned n, unsigned long long acc){\n"
     "  if(n==0) return 0;\n"
     "  return acc + "+p+"_rec(n-1, acc*31u + n); }\n"
     +t+" "+p+"_recur("+t+" a){ unsigned long long r="+p+"_rec(40,(unsigned)a|1ULL);\n"
     "  return ("+t+")(r ^ (r>>32)); }\n",
     {0x4567u}, "OptStress304", Opt},

    // Sub-word (char/short) results merged across branches into the return.
    {p+"_subword",
     t+" "+p+"_subword("+t+" a){ unsigned w=(unsigned)a|1u; unsigned long long acc=0;\n"
     "  for(int i=0;i<64;i++){ w=w*1103515245u+12345u;\n"
     "    unsigned char c=(unsigned char)(w>>7); unsigned short s=(unsigned short)(w>>11);\n"
     "    acc=acc*131u + ((i&1)?(unsigned long long)c:(unsigned long long)s); }\n"
     "  if((w&1)==0) return ("+t+")(unsigned short)acc;\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "OptStress304", Opt},

    // 64-bit value carried through a loop PHI, returned vs an early narrow exit.
    {p+"_loopcarry",
     t+" "+p+"_loopcarry("+t+" a){ unsigned long long acc=((unsigned long long)(unsigned)a<<20)|1ULL;\n"
     "  unsigned long long prod=1;\n"
     "  for(int i=1;i<=40;i++){ prod*= (unsigned long long)(i|1); acc+= prod ^ (acc>>13);\n"
     "    if((acc & 0xFFFF)==0xBEEF) return ("+t+")(int)(acc>>3); }\n"
     "  return ("+t+")(acc ^ (acc>>32) ^ prod); }\n",
     {0x6789u}, "OptStress304", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress304TC("x64o304", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress304TC("x86o304", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress304TC("a64o304", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress304TC("armo304", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress304, X64OptStress304RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress304, X86OptStress304RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress304, A64OptStress304RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress304, ARM32OptStress304RT, ::testing::ValuesIn(kARM), rtTCName);
