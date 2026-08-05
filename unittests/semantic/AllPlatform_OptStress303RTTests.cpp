//===- AllPlatform_OptStress303RTTests.cpp - return-width merge probe ----===//
//
// Stresses integer return-type inference (MedTypePass::inferReturnType /
// intRetEffWidth), the machinery #514 patched so an i64 result arriving through
// a merged-epilogue return-register PHI is not mistyped narrow by a sibling
// `return 0` path's zero-extend.  These kernels build the adjacent shapes:
// several return paths of MIXED width (i32 `return 0` / truncated-int siblings
// vs a 64-bit accumulator), recursion with a narrow base case, and sub-word
// (char/short) merges — all reaching a single epilogue.
//
// Each function returns the platform's natural width (`long` on x86-64/aarch64,
// `int` on i386/arm32).  On the 64-bit targets the harness reads the full
// RAX/X0, so a dropped high word from a mis-inferred narrow return type shows up
// directly; on the 32-bit targets the wide intermediate is truncated into the
// 32-bit return.  All 64-bit math is multiply/shift/add/logic only (no 64-bit
// division) to stay libcall-free on i386/arm32.  Deterministic (LCG-seeded).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress303RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress303RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress303RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress303RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress303RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress303RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress303RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress303RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress303TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // i64 accumulator vs sibling `return 0` (the #514 bug-(1) shape).
    {p+"_phi0",
     t+" "+p+"_phi0("+t+" a){ unsigned long long acc=(unsigned)a|1ULL;\n"
     "  for(int i=0;i<40;i++){ acc=acc*6364136223846793005ULL+(unsigned)i;\n"
     "    if(((acc>>17)&0x3FF)==0x123) return ("+t+")0; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234u}, "OptStress303", Opt},

    // Three return paths of different widths reaching one epilogue.
    {p+"_three",
     t+" "+p+"_three("+t+" a){ unsigned long long acc=(unsigned)a^0xA5A5A5A5u; unsigned w=(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ acc=acc*2862933555777941757ULL+3037000493ULL; w=w*1103515245u+12345u;\n"
     "    if((w&0xFFF)==0x7A1) return ("+t+")(int)(w>>8);\n"
     "    if((w&0xFFF)==0x3C2) return ("+t+")0; }\n"
     "  return ("+t+")(acc ^ (acc>>29)); }\n",
     {0x2345u}, "OptStress303", Opt},

    // Signed 64-bit accumulator merged with a signed narrow path (sign matters
    // when folded/extended into the return register).
    {p+"_signed",
     t+" "+p+"_signed("+t+" a){ long long acc=-(long long)((unsigned)a|1u);\n"
     "  for(int i=0;i<40;i++){ acc=acc*1103515245LL-12345LL+(i&7);\n"
     "    if(((acc>>11)&0x1FF)==0x55) return ("+t+")(signed char)(acc>>3); }\n"
     "  return ("+t+")acc; }\n",
     {0x3456u}, "OptStress303", Opt},

    // Recursion returning a 64-bit value with a narrow base case (#508/#514
    // wide-return-through-merge shape; i386/arm32 use the EDX:EAX / R0:R1 pair).
    {p+"_recur",
     "static unsigned long long "+p+"_rec(unsigned n, unsigned long long acc){\n"
     "  if(n==0) return 0;\n"
     "  return acc + "+p+"_rec(n-1, acc*31u + n); }\n"
     +t+" "+p+"_recur("+t+" a){ unsigned long long r="+p+"_rec(40,(unsigned)a|1ULL);\n"
     "  return ("+t+")(r ^ (r>>32)); }\n",
     {0x4567u}, "OptStress303", Opt},

    // Sub-word (char/short) results merged across branches into the return.
    {p+"_subword",
     t+" "+p+"_subword("+t+" a){ unsigned w=(unsigned)a|1u; unsigned long long acc=0;\n"
     "  for(int i=0;i<64;i++){ w=w*1103515245u+12345u;\n"
     "    unsigned char c=(unsigned char)(w>>7); unsigned short s=(unsigned short)(w>>11);\n"
     "    acc=acc*131u + ((i&1)?(unsigned long long)c:(unsigned long long)s); }\n"
     "  if((w&1)==0) return ("+t+")(unsigned short)acc;\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "OptStress303", Opt},

    // 64-bit value carried through a loop PHI, returned vs an early narrow exit.
    {p+"_loopcarry",
     t+" "+p+"_loopcarry("+t+" a){ unsigned long long acc=((unsigned long long)(unsigned)a<<20)|1ULL;\n"
     "  unsigned long long prod=1;\n"
     "  for(int i=1;i<=40;i++){ prod*= (unsigned long long)(i|1); acc+= prod ^ (acc>>13);\n"
     "    if((acc & 0xFFFF)==0xBEEF) return ("+t+")(int)(acc>>3); }\n"
     "  return ("+t+")(acc ^ (acc>>32) ^ prod); }\n",
     {0x6789u}, "OptStress303", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress303TC("x64o303", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress303TC("x86o303", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress303TC("a64o303", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress303TC("armo303", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress303, X64OptStress303RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress303, X86OptStress303RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress303, A64OptStress303RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress303, ARM32OptStress303RT, ::testing::ValuesIn(kARM), rtTCName);
