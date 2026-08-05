//===- AllPlatform_OptStress36RTTests.cpp - numeric-kernel probes -*-C++*-=//
//
// Fresh numeric kernels not exercised by the earlier OptStress rounds, each
// stressing a distinct lift corner around constant arithmetic, double-operand
// shifts, and widening multiplies:
//
//   * divconst - signed/unsigned divide AND modulo by non-power-of-two
//                constants (3/7/9/13/100/1000): clang lowers each to a
//                magic multiply-high, exercising x86 MUL/IMUL EDX:EAX high
//                half and AArch64 UMULH/SMULH (vs OptStress34's switch%N,
//                which is index recovery, not a folded arithmetic result).
//   * shld32   - 32-bit two-operand funnel shift (SHLD/SHRD on x86, EXTR on
//                AArch64, lsl/lsr/orr on ARM32) with two *distinct* operands,
//                so it is fshl(hi,lo,n) not the single-operand rotate of #28.
//   * bgcd     - binary GCD (Stein): trailing-zero strip loops + subtract,
//                a ctz/shift/subtract control-flow kernel.
//   * isqrt    - bit-by-bit unsigned integer square root (per-bit branch).
//   * fletcher - Fletcher-16 dual running sum mod 255 (constant modulo folded
//                into the value, two interacting accumulators).
//   * q16mac   - Q16.16 fixed-point multiply-accumulate: 32x32->64 widening
//                signed multiply with a constant >>16 high extraction, plus a
//                64-bit *constant* multiply/shift decay (no 64-bit divide).
//
// Integer-only, single integer return, bounded, no 64-bit divide, no library
// calls; all four targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress36RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress36RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress36RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress36RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress36RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress36RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress36RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress36RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress36TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Divide AND modulo by non-power-of-two constants, signed and unsigned, so
    // clang materializes the full magic multiply-high lowering for each.
    {p+"_divconst",
     t+" "+p+"_divconst("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned u=s; int x=(int)s;\n"
     "    unsigned uq=u/3u + u/100u + u/65521u;\n"
     "    unsigned ur=u%7u + u%1000u + u%255u;\n"
     "    int sq=x/-7 + x/13;\n"
     "    int sr=x%9 + x%(-11);\n"
     "    h=h*131u+uq+ur+(unsigned)sq+(unsigned)sr; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x37ULL}, "OptStress36", 2},

    // Two-operand 32-bit funnel shift: SHLD/SHRD (x86), EXTR (a64).  The count
    // is held in [1,31] so the complementary (32-n) shift never hits UB.
    {p+"_shld32",
     t+" "+p+"_shld32("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0, hi=(unsigned)a, lo=~(unsigned)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=(s>>5)&31u; if(n==0u) n=1u;\n"
     "    unsigned l=(hi<<n)|(lo>>(32u-n));\n"
     "    unsigned r=(lo>>n)|(hi<<(32u-n));\n"
     "    hi=hi*1103515245u+l; lo=lo^r;\n"
     "    h=h*131u+l+r; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x59ULL}, "OptStress36", 2},

    // Binary GCD (Stein): common-power-of-two strip, odd-strip loops, subtract
    // step.  Both operands forced odd-or-nonzero so every loop terminates.
    {p+"_bgcd",
     t+" "+p+"_bgcd("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned x=(s>>2)|2u, y=(s*2654435761u)|3u, shift=0;\n"
     "    while(((x|y)&1u)==0u){ x>>=1; y>>=1; shift++; }\n"
     "    while((x&1u)==0u) x>>=1;\n"
     "    do { while((y&1u)==0u) y>>=1;\n"
     "         if(x>y){ unsigned tmp=x; x=y; y=tmp; }\n"
     "         y=y-x;\n"
     "    } while(y!=0u);\n"
     "    h=h*131u+(x<<shift); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x7bULL}, "OptStress36", 2},

    // Bit-by-bit unsigned integer square root: one conditional subtract per bit.
    {p+"_isqrt",
     t+" "+p+"_isqrt("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=s, x=0, bit=1u<<30;\n"
     "    while(bit>n) bit>>=2;\n"
     "    while(bit){ if(n>=x+bit){ n-=x+bit; x=(x>>1)+bit; } else x>>=1; bit>>=2; }\n"
     "    h=h*131u+x; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9dULL}, "OptStress36", 2},

    // Fletcher-16: two running sums reduced mod 255 (constant modulo) per byte.
    {p+"_fletcher",
     t+" "+p+"_fletcher("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0, c0=0, c1=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned b=(s>>16)&0xffu;\n"
     "    c0=(c0+b)%255u; c1=(c1+c0)%255u;\n"
     "    h=h*131u+((c1<<8)|c0); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xc1ULL}, "OptStress36", 2},

    // Q16.16 fixed-point MAC: 32x32->64 widening signed multiply with constant
    // >>16 high extraction, plus a 64-bit constant multiply/shift decay term.
    {p+"_q16mac",
     t+" "+p+"_q16mac("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int acc=(int)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s>>8, y=(int)(s*2654435761u)>>8;\n"
     "    long long pr=(long long)x*(long long)y;\n"
     "    int m=(int)(pr>>16);\n"
     "    acc += m; acc -= (int)(((long long)acc*3)>>4);\n"
     "    h=h*131u+(unsigned)m+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xe3ULL}, "OptStress36", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress36TC("x64o36", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress36TC("x86o36", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress36TC("a64o36", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress36TC("armo36", "int");

INSTANTIATE_TEST_SUITE_P(OptStress36, X64OptStress36RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress36, X86OptStress36RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress36, A64OptStress36RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress36, ARM32OptStress36RT, ::testing::ValuesIn(kARM), rtTCName);
