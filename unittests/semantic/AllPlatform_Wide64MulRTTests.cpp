//===- AllPlatform_Wide64MulRTTests.cpp - i64 multiply lowering -*-C++*-=//
//
// 64-bit *multiply* kernels, complementing Wide64Opt (which only touches the
// multiply path via a constant multiplier or a single 32x32->64 widening MAC).
// On the 32-bit targets (i386 / ARM32) a full i64 x i64 -> i64 product expands
// inline to three 32-bit multiplies plus the cross-term adds (mul/umull, no
// __muldi3), and a high-64-of-128 product expands to the schoolbook partial
// sum -- carry/cross-term plumbing the in-register arithmetic kernels never
// reach.  Kernels stay libcall-free: full i64xi64 products, 32x32->64 signed
// and unsigned widening multiplies, squaring, multiply-by-wide-constant, and a
// hand-rolled mulhi64; no variable i64 shift and no i64 divide (the only i64
// ops needing __ashldi3 / __udivdi3).  Each folds to a single integer return,
// compiled -O2, native vs lifted on all four targets.
//
//   * mul64    - full i64 x i64 (both runtime) cross-product chain.
//   * mulhi64  - high 64 bits of u64 x u64 via the 32-bit schoolbook halves.
//   * smac     - SIGNED 32x32->64 widening multiply-accumulate, negative inputs.
//   * sqr64    - i64 squaring + difference-of-squares ((u+v)(u-v)) identity.
//   * mixmul   - full i64 x i64 mixed with multiply-by-wide-constant (>2^32).
//   * mulchain - dependent i64 multiply chain (64-bit LCG + squaring feedback).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Wide64MulRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Wide64MulRT, Verify) { roundTripX64(GetParam()); }
class X86Wide64MulRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Wide64MulRT, Verify) { roundTripX86(GetParam()); }
class A64Wide64MulRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Wide64MulRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Wide64MulRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Wide64MulRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeW64Mul(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Full i64 x i64 product (both operands runtime): three 32-bit multiplies
    // plus the cross-term adds on the 32-bit targets.
    {p+"_mul64",
     t+" "+p+"_mul64("+t+" a){\n"
     "  unsigned long long x=(unsigned long long)a|1ull, y=x^0x9e3779b97f4a7c15ull, h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned long long p2=x*y;\n"
     "    x=p2+0x1442695040888963ull; y=(y^p2)*2654435761ull;\n"
     "    h+=p2; h^=(h>>29); h*=0x100000001b3ull; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x41ULL}, "Wide64Mul", 2},

    // High 64 bits of an unsigned 64x64 product via the schoolbook decomposition
    // (the four 32x32->64 partials and the cross-term carry).
    {p+"_mulhi64",
     t+" "+p+"_mulhi64("+t+" a){\n"
     "  unsigned long long s=(unsigned long long)a|1ull, h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*6364136223846793005ull+1442695040888963407ull;\n"
     "    unsigned long long b=s^(s>>31);\n"
     "    unsigned long long alo=s&0xffffffffull, ahi=s>>32;\n"
     "    unsigned long long blo=b&0xffffffffull, bhi=b>>32;\n"
     "    unsigned long long ll=alo*blo, lh=alo*bhi, hl=ahi*blo, hh=ahi*bhi;\n"
     "    unsigned long long cross=(ll>>32)+(lh&0xffffffffull)+(hl&0xffffffffull);\n"
     "    unsigned long long hi=hh+(lh>>32)+(hl>>32)+(cross>>32);\n"
     "    h+=hi; h=h*131u+(h>>32); }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x53ULL}, "Wide64Mul", 2},

    // Signed 32x32->64 widening MAC with negative operands: sign extension into
    // both words of the product before the i64 accumulate.
    {p+"_smac",
     t+" "+p+"_smac("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long long acc=0; unsigned long long h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s^0xa5a5a5a5u);\n"
     "    long long pr=(long long)x*(long long)y;\n"
     "    acc+=pr; acc-=(long long)(int)(s>>3);\n"
     "    unsigned lo=(unsigned)acc, hi=(unsigned)((unsigned long long)acc>>32);\n"
     "    h=h*131u+lo+hi; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x6dULL}, "Wide64Mul", 2},

    // i64 squaring plus the difference-of-squares identity (u+v)(u-v): two full
    // products whose wraparound is well defined and identical native-vs-lifted.
    {p+"_sqr64",
     t+" "+p+"_sqr64("+t+" a){\n"
     "  unsigned long long s=(unsigned long long)a|1ull, h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*6364136223846793005ull+1ull;\n"
     "    unsigned long long u=s>>1, v=s>>3;\n"
     "    unsigned long long sq=u*u, dv=(u+v)*(u-v);\n"
     "    h+=sq^dv; h=h*131u+(h>>32); }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0xb2ULL}, "Wide64Mul", 2},

    // Full i64 x i64 interleaved with multiply-by-wide-constant (>2^32 so it
    // cannot fold to a 32-bit multiply form).
    {p+"_mixmul",
     t+" "+p+"_mixmul("+t+" a){\n"
     "  unsigned long long x=(unsigned long long)a|1ull, y=~x, h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned long long p2=x*y;\n"
     "    unsigned long long q=x*0xdeadbeefcafef00dull;\n"
     "    unsigned long long r=y*0x00000000ffffffffull;\n"
     "    x=p2^q; y=(y+r)^(p2>>17);\n"
     "    h+=p2+q+r; h*=0x100000001b3ull; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x18ULL}, "Wide64Mul", 2},

    // Dependent i64 multiply chain: a 64-bit LCG with a squaring feedback so the
    // products cannot be hoisted or strength-reduced.
    {p+"_mulchain",
     t+" "+p+"_mulchain("+t+" a){\n"
     "  unsigned long long x=(unsigned long long)a|1ull, h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    x=x*0x5851f42d4c957f2dull+0x14057b7ef767814full;\n"
     "    unsigned long long y=x*(x|1ull);\n"
     "    y=y*y+(y>>11);\n"
     "    h^=y; h=h*131u+(h>>32); }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x2fULL}, "Wide64Mul", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeW64Mul("x64w64m", "long");
static const std::vector<RoundTripTC> kX86 = makeW64Mul("x86w64m", "int");
static const std::vector<RoundTripTC> kA64 = makeW64Mul("a64w64m", "long");
static const std::vector<RoundTripTC> kARM = makeW64Mul("armw64m", "int");

INSTANTIATE_TEST_SUITE_P(Wide64Mul, X64Wide64MulRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Mul, X86Wide64MulRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Mul, A64Wide64MulRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Mul, ARM32Wide64MulRT, ::testing::ValuesIn(kARM), rtTCName);
