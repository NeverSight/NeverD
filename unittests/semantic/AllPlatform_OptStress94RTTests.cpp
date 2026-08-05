//===- AllPlatform_OptStress94RTTests.cpp - lone sign on the branch path -==//
//
// #486 / OptStress90-93 pinned the lone sign flag (`(A-B) <s 0`, NOT the
// overflow-corrected `A <s B`) through the SETcc and SELECT folding passes.
// This probe closes the matching gap on the COND_BR path and at 64-bit width:
//
//   * AArch64 `cmp; b.mi/b.pl` and ARM32 `cmp; bmi/bpl` — a conditional BRANCH on
//     the lone N flag.  Pass 1 (COND_BR) folds the branch condition against the
//     in-block CMP, so condIsLoneSignFlag must recognise the branch condition's
//     chain shape on these targets too, not only the x86 `js`/`jns` already in
//     OptStress93.
//   * x86/x64 64-bit `cmpq; js/jns` — the 32-bit `cmpl` cases of OptStress92/93
//     widened to the INT64_MIN/INT64_MAX overflow boundary.
//
// Inputs straddle the signed-overflow boundary so a lone-N misfold to `A <s B`
// diverges from the native run: INT_MIN-1 wraps positive (N=0 while A<sB true)
// and INT_MAX-(-1) wraps negative (N=1 while A<sB false).  Lone-overflow guards
// (`b.vs`/`bvs`/`jo`) confirm the OF/V path is unaffected.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress94RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress94RT, Verify) { roundTripX64(GetParam()); }
class A64OptStress94RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress94RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress94RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress94RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// 32-bit overflow boundary: INT_MIN-1 wraps to INT_MAX, INT_MAX-(-1) to INT_MIN.
static const uint64_t IMIN = 0x80000000ULL, IMAX = 0x7FFFFFFFULL, NEG1 = 0xFFFFFFFFULL;
// 64-bit overflow boundary.
static const uint64_t LMIN = 0x8000000000000000ULL, LMAX = 0x7FFFFFFFFFFFFFFFULL;

// x86/x64: lone-SF via 64-bit cmpq + js/jns branch, plus a lone-OF (jo) guard.
static std::vector<RoundTripTC> makeX64LoneBrTC() {
  std::string p = "x64o94";
  return {
    // cmpq a,b ; jns skip ; else r=300.  r = sign(a-b wrapped) ? 300 : 100, which
    // is NOT (a <s b): a lone-N misfold to (a<sB) flips at the overflow input.
    {p+"_js64_a",
     "long "+p+"_js64_a(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmpq %2,%1\\n\\tmovl $100,%k0\\n\\tjns 1f\\n\\t\"\n"
     "    \"movl $300,%k0\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {LMIN, 1}, "OptStress94"},
    {p+"_js64_b",
     "long "+p+"_js64_b(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmpq %2,%1\\n\\tmovl $100,%k0\\n\\tjns 1f\\n\\t\"\n"
     "    \"movl $300,%k0\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {LMAX, ~0ULL}, "OptStress94"},
    // Control: 64-bit jl IS the signed comparison (SF^OF) -> must equal (a <s b).
    {p+"_jl64",
     "long "+p+"_jl64(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmpq %2,%1\\n\\tmovl $100,%k0\\n\\tjge 1f\\n\\t\"\n"
     "    \"movl $300,%k0\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {LMIN, 1}, "OptStress94"},
    // Lone-OF branch guard: cmpq a,b ; jo.
    {p+"_jo64",
     "long "+p+"_jo64(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmpq %2,%1\\n\\tmovl $100,%k0\\n\\tjno 1f\\n\\t\"\n"
     "    \"movl $300,%k0\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {LMIN, 1}, "OptStress94"},
  };
}

// AArch64: lone-N via cmp + b.mi/b.pl BRANCH (Pass 1 COND_BR fold), both widths,
// plus a lone-V (b.vs) guard.  %w for 32-bit, %x for 64-bit operands.
static std::vector<RoundTripTC> makeA64LoneBrTC() {
  std::string p = "a64o94";
  return {
    // cmp w1,w2 ; b.pl skip ; else r=300.  r = N(a-b wrapped) ? 300 : 100.
    {p+"_bmi32_a",
     "long "+p+"_bmi32_a(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tmov %0,#100\\n\\tb.pl 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress94"},
    {p+"_bmi32_b",
     "long "+p+"_bmi32_b(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tmov %0,#100\\n\\tb.pl 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMAX, NEG1}, "OptStress94"},
    // 64-bit operands at the INT64 overflow boundary.
    {p+"_bmi64_a",
     "long "+p+"_bmi64_a(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %1,%2\\n\\tmov %0,#100\\n\\tb.pl 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {LMIN, 1}, "OptStress94"},
    // Control: b.lt IS the signed comparison (N!=V) -> must equal (a <s b).
    {p+"_blt32",
     "long "+p+"_blt32(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tmov %0,#100\\n\\tb.ge 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress94"},
    // Lone-V branch guard: cmp ; b.vs (overflow flag, INT_SBOR kept).
    {p+"_bvs32",
     "long "+p+"_bvs32(long a,long b){ long r;\n"
     "  __asm__ volatile(\"cmp %w1,%w2\\n\\tmov %0,#100\\n\\tb.vc 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress94"},
  };
}

// ARM32: lone-N via cmp + bmi/bpl BRANCH, plus a lone-V (bvs) guard.
static std::vector<RoundTripTC> makeARM32LoneBrTC() {
  std::string p = "armo94";
  return {
    {p+"_bmi_a",
     "int "+p+"_bmi_a(int a,int b){ int r;\n"
     "  __asm__ volatile(\"cmp %1,%2\\n\\tmov %0,#100\\n\\tbpl 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress94"},
    {p+"_bmi_b",
     "int "+p+"_bmi_b(int a,int b){ int r;\n"
     "  __asm__ volatile(\"cmp %1,%2\\n\\tmov %0,#100\\n\\tbpl 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMAX, NEG1}, "OptStress94"},
    // Control: blt IS the signed comparison (N!=V) -> must equal (a <s b).
    {p+"_blt",
     "int "+p+"_blt(int a,int b){ int r;\n"
     "  __asm__ volatile(\"cmp %1,%2\\n\\tmov %0,#100\\n\\tbge 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress94"},
    // Lone-V branch guard: cmp ; bvs (overflow flag kept).
    {p+"_bvs",
     "int "+p+"_bvs(int a,int b){ int r;\n"
     "  __asm__ volatile(\"cmp %1,%2\\n\\tmov %0,#100\\n\\tbvc 1f\\n\\t\"\n"
     "    \"mov %0,#300\\n\\t1:\":\"=&r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
     "  return r; }\n",
     {IMIN, 1}, "OptStress94"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeX64LoneBrTC();
static const std::vector<RoundTripTC> kA64 = makeA64LoneBrTC();
static const std::vector<RoundTripTC> kARM = makeARM32LoneBrTC();

INSTANTIATE_TEST_SUITE_P(OptStress94, X64OptStress94RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress94, A64OptStress94RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress94, ARM32OptStress94RT, ::testing::ValuesIn(kARM), rtTCName);
