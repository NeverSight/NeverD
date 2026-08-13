//===- AllPlatform_OptStress333RTTests.cpp - vec-init before dispatch ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adjacent probes for #532 bug② (x86-64 PIC jump-table base `lea tab(%rip)`
// materialized at the END of a leading block whose body is a fully-unrolled
// SSE constant-array init — a string of store INTRINSICs that previously stalled
// `foldRegConstant`, degrading the switch to a tail call → CFG collapse →
// recompile FETCH_UNMAPPED).  The fix made INTRINSIC opaque-but-non-halting so
// the linear trace steps over the vectorized body to reach the `lea`.  This
// batch stresses the SAME boundary with shapes the single #532 guardrail did not
// cover, to catch any residual gap in tracing over a vectorized prologue:
//
//   * _initsw   : SSE const init THEN a dense 8-way switch that ALSO indexes the
//                 array — both the jump-table base AND the array base must be
//                 recovered past the vectorized init.
//   * _initcall : SSE const init THEN an indirect call whose target is
//                 materialized after the vector body (the #200/#524 CALL sibling
//                 of bug②'s jump-table form).
//   * _initsw2  : TWO init+switch regions back to back — two independent folding
//                 points, the second past an even longer linear prefix.
//   * _biginit  : a 64-element init (deeper INTRINSIC chain) before the switch,
//                 stressing the trace depth over the vectorized prologue.
//
// All-integer, power-of-two array masks only (no `%`/`/` → ARM32 stays libcall-
// free), unsigned wraparound, deterministic LCG inputs folded to one return.
// x86-64 is the primary target; i386/AArch64/ARM32 are controls.  -O2, 4 targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress333RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress333RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress333RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress333RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress333RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress333RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress333RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress333RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress333TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // SSE const-array init THEN a dense 8-way switch indexing the same array:
    // jump-table base + array base both materialized after the vectorized init.
    {p+"_initsw",
     t+" "+p+"_initsw("+t+" a){\n"
     "  int tab[32]; for(int i=0;i<32;i++) tab[i]=i*i*7+i*3+11;\n"
     "  unsigned acc=0, x=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){ x=x*1664525u+1013904223u;\n"
     "    switch((x>>5)&7u){\n"
     "      case 0: acc += (unsigned)tab[(x>>8)&31]; break;\n"
     "      case 1: acc ^= (unsigned)tab[(x>>9)&31]<<1; break;\n"
     "      case 2: acc -= (unsigned)tab[(x>>10)&31]; break;\n"
     "      case 3: acc += (unsigned)tab[(x>>11)&31]*3u; break;\n"
     "      case 4: acc ^= (unsigned)tab[(x>>12)&31]>>1; break;\n"
     "      case 5: acc += (unsigned)tab[(x>>13)&31]<<2; break;\n"
     "      case 6: acc -= (unsigned)tab[(x>>14)&31]*5u; break;\n"
     "      default: acc ^= (unsigned)tab[(x>>15)&31]; break; } }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "OptStress333", Opt},

    // SSE const init THEN an indirect call whose target is materialized after the
    // vectorized body (the CALL sibling of bug②'s jump-table form).
    {p+"_initcall",
     "static int "+p+"_ic(int v) __attribute__((noinline));\n"
     +t+" "+p+"_initcall("+t+" a){\n"
     "  int tab[32]; for(int i=0;i<32;i++) tab[i]=i*i*5+7;\n"
     "  int (*fp)(int)="+p+"_ic; __asm__(\"\":\"+r\"(fp));\n"
     "  unsigned acc=0, x=(unsigned)a;\n"
     "  for(int i=0;i<48;i++){ x=x*22695477u+1u;\n"
     "    acc += (unsigned)fp(tab[(x>>4)&31]^(int)acc); }\n"
     "  return ("+t+")acc;\n"
     "}\n"
     "static int "+p+"_ic(int v){ return v*131 + (v>>3); }\n",
     {0x2345678ULL}, "OptStress333", Opt},

    // Two init+switch regions back to back: two independent folding points, the
    // second reached past an even longer linear prefix.
    {p+"_initsw2",
     t+" "+p+"_initsw2("+t+" a){\n"
     "  int t1[32]; for(int i=0;i<32;i++) t1[i]=i*i*3+i+5;\n"
     "  unsigned acc=0, x=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ x=x*1664525u+1013904223u;\n"
     "    switch((x>>6)&3u){ case 0:acc+=(unsigned)t1[(x>>8)&31];break;\n"
     "      case 1:acc^=(unsigned)t1[(x>>9)&31]<<1;break;\n"
     "      case 2:acc-=(unsigned)t1[(x>>10)&31];break;\n"
     "      default:acc+=(unsigned)t1[(x>>11)&31]*3u;break; } }\n"
     "  int t2[32]; for(int i=0;i<32;i++) t2[i]=i*i*9+i*2+13;\n"
     "  for(int i=0;i<40;i++){ x=x*22695477u+1u;\n"
     "    switch((x>>4)&3u){ case 0:acc+=(unsigned)t2[(x>>7)&31];break;\n"
     "      case 1:acc^=(unsigned)t2[(x>>8)&31]<<2;break;\n"
     "      case 2:acc-=(unsigned)t2[(x>>9)&31]*5u;break;\n"
     "      default:acc+=(unsigned)t2[(x>>10)&31];break; } }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3456789ULL}, "OptStress333", Opt},

    // 64-element init (deeper INTRINSIC chain) before the switch: stress the
    // linear-trace depth over the vectorized prologue.
    {p+"_biginit",
     t+" "+p+"_biginit("+t+" a){\n"
     "  int tab[64]; for(int i=0;i<64;i++) tab[i]=i*i*7+i*5+3;\n"
     "  unsigned acc=0, x=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){ x=x*1664525u+1013904223u;\n"
     "    switch((x>>5)&7u){ case 0:acc+=(unsigned)tab[(x>>8)&63];break;\n"
     "      case 1:acc^=(unsigned)tab[(x>>9)&63]<<1;break;\n"
     "      case 2:acc-=(unsigned)tab[(x>>10)&63];break;\n"
     "      case 3:acc+=(unsigned)tab[(x>>11)&63]*3u;break;\n"
     "      case 4:acc^=(unsigned)tab[(x>>12)&63]>>1;break;\n"
     "      case 5:acc+=(unsigned)tab[(x>>13)&63]<<2;break;\n"
     "      case 6:acc-=(unsigned)tab[(x>>14)&63]*5u;break;\n"
     "      default:acc^=(unsigned)tab[(x>>15)&63];break; } }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x456789AULL}, "OptStress333", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress333TC("x64o333", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress333TC("x86o333", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress333TC("a64o333", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress333TC("armo333", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress333, X64OptStress333RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress333, X86OptStress333RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress333, A64OptStress333RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress333, ARM32OptStress333RT, ::testing::ValuesIn(kARM), rtTCName);
