//===- AllPlatform_OptStress23RTTests.cpp - opt-stress probes --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes aimed at the signed-edge / division /
// carry-as-value corners of the NeverD hand-written MedIR passes, in shapes
// OptStress1-22 only grazed:
//
//   * negabsedge - INT_MIN-aware negate/abs/min/max ladder (signed-overflow wrap
//                  done through unsigned so it is defined, signed compares drive
//                  the selects).  Exercises the optimizer's sign handling at the
//                  2's-complement edge.
//   * divsub     - signed and unsigned division/modulo whose operands are sub-word
//                  (8/16-bit) views recombined (idiv/div + srem/urem feeding the
//                  RDX/RAX or msub recovery through narrowing).
//   * carrysel   - the carry-out of a 64-bit add used as a *predicate* to select,
//                  so the borrow/carry bit crosses from flag-domain into value.
//   * dshacc     - double-width funnel shifts feeding a sub-register accumulator
//                  (SHLD/SHRD result narrowed to 16-bit and re-widened).
//   * muloflo    - manual signed/unsigned multiply-overflow detection via the high
//                  half (no __builtin_*_overflow), so the optimizer must keep the
//                  hi-part compare honest.
//   * popclz     - popcount/clz/ctz of derived sub-words chained with shifts
//                  (bit-count intrinsics lowered inline, no libcall).
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper (no 64-bit divide / no float), so all four targets are
// checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress23RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress23RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress23RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress23RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress23RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress23RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress23RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress23RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress23TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // INT_MIN-aware negate/abs/min/max ladder (wrap via unsigned, signed compares).
    {p+"_negabsedge",
     t+" "+p+"_negabsedge("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned acc=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u; int x=(int)s;\n"
     "    unsigned n=(0u-(unsigned)x);\n"
     "    unsigned ab=(x<0)?(0u-(unsigned)x):(unsigned)x;\n"
     "    acc += n ^ ab;\n"
     "    acc = ((int)acc < x) ? acc : (unsigned)x;\n"
     "    acc = ((int)acc > (int)n) ? acc : n;\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress23", 2},

    // Signed/unsigned div+mod of sub-word (8/16-bit) views recombined.
    {p+"_divsub",
     t+" "+p+"_divsub("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    short sn=(short)s, sd=(short)((s>>11)|1u);\n"
     "    int sq=(int)sn/(int)sd, sr=(int)sn%(int)sd;\n"
     "    unsigned short un=(unsigned short)(s>>3), ud=(unsigned short)((s>>17)|1u);\n"
     "    unsigned uq=(unsigned)un/(unsigned)ud, ur=(unsigned)un%(unsigned)ud;\n"
     "    h=h*131u+(unsigned)sq+(unsigned)sr+uq+ur; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress23", 2},

    // 64-bit add carry-out used as a select predicate (flag -> value crossing).
    {p+"_carrysel",
     t+" "+p+"_carrysel("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long lo=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long add=((unsigned long long)s<<20)|(s>>5);\n"
     "    unsigned long long nlo=lo+add;\n"
     "    unsigned carry=(nlo<lo);\n"
     "    lo = carry ? (nlo^0x9e3779b9ull) : (nlo+1ull);\n"
     "    h=h*131u+(unsigned)lo+carry*7u+(unsigned)(lo>>32); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress23", 2},

    // Double-width funnel shift narrowed to 16-bit then re-widened.
    {p+"_dshacc",
     t+" "+p+"_dshacc("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned x=s, y=s^0x5bd1e995u; unsigned h=0;\n"
     "  unsigned short w=(unsigned short)a;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=((s>>4)&15u)|1u;\n"
     "    unsigned f=(x<<n)|(y>>(32-n));\n"
     "    w=(unsigned short)(w+(unsigned short)(f>>(n&7u)));\n"
     "    x=y^f; y=(f<<1)+s;\n"
     "    h=h*131u+(unsigned)w+f; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress23", 2},

    // Manual signed/unsigned multiply-overflow detection via the high half.
    {p+"_muloflo",
     t+" "+p+"_muloflo("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<44;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=s, y=s*2654435761u;\n"
     "    unsigned long long up=(unsigned long long)x*(unsigned long long)y;\n"
     "    unsigned uovf=((up>>32)!=0ull);\n"
     "    long long sp=(long long)(int)x*(long long)(int)y;\n"
     "    unsigned sovf=((sp>>31)!=(sp>>63));\n"
     "    h=h*131u+(unsigned)up+uovf*3u+(unsigned)sp+sovf*5u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress23", 2},

    // popcount/clz/ctz of derived sub-words chained with shifts (inline lowering).
    {p+"_popclz",
     t+" "+p+"_popclz("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned v=s|1u;\n"
     "    unsigned pc=(unsigned)__builtin_popcount(v);\n"
     "    unsigned lz=(unsigned)__builtin_clz(v);\n"
     "    unsigned tz=(unsigned)__builtin_ctz(v);\n"
     "    unsigned short hv=(unsigned short)(s>>7); hv|=1u;\n"
     "    unsigned pc16=(unsigned)__builtin_popcount((unsigned)hv);\n"
     "    h=h*131u+(pc<<tz)+(lz*7u)+pc16; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress23", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress23TC("x64o23", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress23TC("x86o23", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress23TC("a64o23", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress23TC("armo23", "int");

INSTANTIATE_TEST_SUITE_P(OptStress23, X64OptStress23RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress23, X86OptStress23RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress23, A64OptStress23RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress23, ARM32OptStress23RT, ::testing::ValuesIn(kARM), rtTCName);
