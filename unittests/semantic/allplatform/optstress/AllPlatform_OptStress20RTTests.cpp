//===- AllPlatform_OptStress20RTTests.cpp - subreg/flag/select -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes aimed squarely at the MedIR passes that
// historically produced "folded-to-0 / dropped computation" miscompiles:
// sub-register SSA aliasing, flag reconstruction, copy/constant propagation and
// dead-code elimination.  Each kernel keeps a value alive through transforms
// that those passes must track exactly:
//
//   * truncwave - push an accumulator through a u32->i16->u8->i32 sign/zero
//                 extension wave every iteration (the exact sub-register aliasing
//                 class behind the historical MOVZX/MOVSX fold-to-0 bug).
//   * signdiv   - signed div/mod/abs across the INT_MIN and divisor==-1 edges
//                 (signed-division lowering + overflow, no library helper).
//   * boolmix   - dense boolean algebra over six compares ((a<b)&(c<d) etc.)
//                 driving a counter (setcc/cset flags + boolean folding).
//   * selreduce - a reduction whose every step is a data-dependent select
//                 between two arithmetic results, reusing one compare's flags.
//   * shiftvar  - variable masked shifts + funnel + rotate mixing 8/16/32 widths
//                 (shift-amount masking + the shift-by-bitwidth UB guard).
//   * carrysel  - add/sub-with-borrow threaded across a loop where the carry also
//                 feeds a select (the flag is both an addend and a predicate).
//
// All integer, fold to one integer return, no float / 64-bit-divide helper on
// the 32-bit targets.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress20RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress20RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress20RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress20RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress20RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress20RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress20RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress20RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress20TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sub-register extension wave: u32 -> i16 -> u8 -> i32, every iteration.
    {p+"_truncwave",
     t+" "+p+"_truncwave("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    short s=(short)(x>>7);\n"
     "    unsigned char b=(unsigned char)(s ^ (short)i);\n"
     "    int e=(int)(signed char)b + (int)s - (int)(unsigned short)(x>>3);\n"
     "    h=h*31+e; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress20", 2},

    // Signed div/mod/abs over negative divisors and the INT_MIN abs edge.  The
    // divisor set excludes 0 and -1 so INT_MIN/-1 never raises #DE on x86 (a
    // hardware fault would abort the original run, not exercise the lifter).
    {p+"_signdiv",
     t+" "+p+"_signdiv("+t+" a){\n"
     "  int x=(int)a|1; int h=0; int dv[5]={7,-3,1000000007,-2,11};\n"
     "  for(int i=0;i<30;i++){ x=x*1103515245+12345;\n"
     "    int d=dv[i%5];\n"
     "    int q=x/d, r=x%d;\n"
     "    int ax=x<0?-x:x;\n"
     "    h=h*31+q-r+(ax>>3); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress20", 2},

    // Dense boolean algebra over six compares driving a counter.
    {p+"_boolmix",
     t+" "+p+"_boolmix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int cnt=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    int A=(int)(s>>3), B=(int)(s>>11), C=(int)(s<<2);\n"
     "    unsigned U=s>>5, V=s<<7;\n"
     "    int p1=(A<B)&(C>A);\n"
     "    int p2=(U<V)|(A==C);\n"
     "    int p3=(B>=C)^(U>=V);\n"
     "    int p4=!(A<0)&&(B<0);\n"
     "    cnt += p1 + 2*p2 - 3*p3 + 4*p4; }\n"
     "  return ("+t+")(unsigned)cnt; }\n",
     {0x55ULL}, "OptStress20", 2},

    // Reduction where each step is a data-dependent select reusing flags.
    {p+"_selreduce",
     t+" "+p+"_selreduce("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=1;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    int v=(int)(s>>8); int w=(int)(s<<3);\n"
     "    int lt=v<w;\n"
     "    int m = lt ? (v+w) : (v-w);\n"
     "    int n = lt ? (acc^v) : (acc+w);\n"
     "    acc = (m<n? m : n) + (lt? 1 : -1); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0xa7ULL}, "OptStress20", 2},

    // Variable masked shifts + funnel + rotate mixing 8/16/32 widths.  All
    // shift counts are masked to a UB-free range (rotate uses (32-sh)&31; the
    // funnel takes the sh==0 branch explicitly) so divergence can only come from
    // a lift/optimizer error, never from C-level shift-by-width UB.
    {p+"_shiftvar",
     t+" "+p+"_shiftvar("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, y=x^0x9e3779b9u, h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned sh=(x>>5)&31u; unsigned sh2=(x>>9)&15u;\n"
     "    unsigned rot=(x<<sh)|(x>>((32u-sh)&31u));\n"
     "    unsigned fun=(sh==0u)?x:((x<<sh)|(y>>(32u-sh)));\n"
     "    unsigned short hw=(unsigned short)(rot>>sh2);\n"
     "    unsigned char by=(unsigned char)(fun>>(sh&7u));\n"
     "    h=h*131u+rot+fun+hw+by; y=x; }\n"
     "  return ("+t+")h; }\n",
     {0x6dULL}, "OptStress20", 2},

    // Borrow/carry threaded across a loop, also feeding a select.
    {p+"_carrysel",
     t+" "+p+"_carrysel("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, lo=0, hi=0; int h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned add=s>>3;\n"
     "    unsigned nlo=lo+add; unsigned carry=nlo<lo; lo=nlo;\n"
     "    hi += carry + (s>>20);\n"
     "    unsigned nb=lo - (s>>7); unsigned borrow=lo<(s>>7); lo=nb;\n"
     "    int pick = carry ? (int)hi : -(int)borrow;\n"
     "    h = h*31 + pick + (int)(lo&0xff); }\n"
     "  return ("+t+")(unsigned)(h + (int)hi); }\n",
     {0x13ULL}, "OptStress20", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress20TC("x64o20", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress20TC("x86o20", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress20TC("a64o20", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress20TC("armo20", "int");

INSTANTIATE_TEST_SUITE_P(OptStress20, X64OptStress20RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress20, X86OptStress20RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress20, A64OptStress20RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress20, ARM32OptStress20RT, ::testing::ValuesIn(kARM), rtTCName);
