//===- AllPlatform_OptStress35RTTests.cpp - integer idiom probes -*-C++*-=//
//
// Fresh integer idioms not exercised by the earlier OptStress rounds, each
// stressing a distinct lift/optimizer corner: two-sided saturating narrowing
// casts, conditional abs/negate with the INT_MIN wrap edge, Gray-code
// encode/decode, Morton (Z-order) bit interleave/deinterleave via the magic
// spread masks, multi-precision (two i32 as one i64) compare + conditional
// add/sub, and the count-leading/trailing/popcount/parity builtins guarded at
// the zero-input edge driving control flow.
//
//   * satnarrow - clamp i32 into int8/int16/uint8/uint16, accumulate.
//   * absnegate - x<0?-x:x, -x, abs(diff) chains (INT_MIN wrap is well defined
//                 native-vs-lifted since both run the same compiled code).
//   * graycode  - x ^ (x>>1) encode and the prefix-xor decode, round-tripped.
//   * morton    - interleave two 16-bit halves to 32-bit and deinterleave.
//   * mp64cmp   - {lo,hi} i64 compare (<, ==) + conditional 64-bit add/sub.
//   * clztz     - guarded clz/ctz/popcount/parity driving branches.
//
// Integer-only, single integer return, bounded, no 64-bit divide; all four
// targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress35RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress35RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress35RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress35RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress35RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress35RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress35RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress35RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress35TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Two-sided saturating narrowing casts into int8/int16/uint8/uint16.
    {p+"_satnarrow",
     t+" "+p+"_satnarrow("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; int x=(int)s;\n"
     "    int s8=x<-128?-128:(x>127?127:x);\n"
     "    int s16=x<-32768?-32768:(x>32767?32767:x);\n"
     "    unsigned u8=(unsigned)x>255u?255u:(unsigned)x;\n"
     "    unsigned u16=(unsigned)x>65535u?65535u:(unsigned)x;\n"
     "    h=h*131u+(unsigned)s8+(unsigned)s16+u8+u16; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress35", 2},

    // Conditional abs / negate chains including the INT_MIN wrap edge.
    {p+"_absnegate",
     t+" "+p+"_absnegate("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0; int acc=(int)a;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; int x=(int)s, y=(int)(s>>7);\n"
     "    int ax=x<0?-x:x; int d=x-y; int ad=d<0?-d:d;\n"
     "    acc = (acc<0?-acc:acc) + ax - ad; acc = -acc;\n"
     "    h=h*131u+(unsigned)ax+(unsigned)ad+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress35", 2},

    // Gray-code encode then the prefix-xor decode, round-tripped each iteration.
    {p+"_graycode",
     t+" "+p+"_graycode("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned g=s^(s>>1);\n"
     "    unsigned d=g; d^=d>>1; d^=d>>2; d^=d>>4; d^=d>>8; d^=d>>16;\n"
     "    h=h*131u+g+d; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress35", 2},

    // Morton / Z-order interleave of two 16-bit halves and deinterleave back.
    {p+"_morton",
     t+" "+p+"_morton("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned xx=s&0xffffu, yy=(s>>16)&0xffffu;\n"
     "    unsigned x=xx,y=yy;\n"
     "    x=(x|(x<<8))&0x00ff00ffu; x=(x|(x<<4))&0x0f0f0f0fu;\n"
     "    x=(x|(x<<2))&0x33333333u; x=(x|(x<<1))&0x55555555u;\n"
     "    y=(y|(y<<8))&0x00ff00ffu; y=(y|(y<<4))&0x0f0f0f0fu;\n"
     "    y=(y|(y<<2))&0x33333333u; y=(y|(y<<1))&0x55555555u;\n"
     "    unsigned m=x|(y<<1);\n"
     "    unsigned dx=m&0x55555555u;\n"
     "    dx=(dx|(dx>>1))&0x33333333u; dx=(dx|(dx>>2))&0x0f0f0f0fu;\n"
     "    dx=(dx|(dx>>4))&0x00ff00ffu; dx=(dx|(dx>>8))&0x0000ffffu;\n"
     "    h=h*131u+m+dx; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb2ULL}, "OptStress35", 2},

    // Two i32 treated as one i64: compare (<, ==) and conditional 64-bit add/sub.
    {p+"_mp64cmp",
     t+" "+p+"_mp64cmp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  unsigned long long acc=((unsigned long long)a<<8)|3ull;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long v=((unsigned long long)s<<20)^((unsigned long long)(s>>3));\n"
     "    if(v<acc) acc+=v; else acc-=v;\n"
     "    if((v&0xffffffffull)==(acc&0xffffffffull)) acc^=0x9e3779b97f4a7c15ull;\n"
     "    h=h*131u+(unsigned)acc+(unsigned)(acc>>32); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xc5ULL}, "OptStress35", 2},

    // Guarded clz/ctz/popcount/parity driving branches (zero input handled).
    {p+"_clztz",
     t+" "+p+"_clztz("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; unsigned n=s>>(i&15);\n"
     "    unsigned cl=n?(unsigned)__builtin_clz(n):32u;\n"
     "    unsigned ct=n?(unsigned)__builtin_ctz(n):32u;\n"
     "    unsigned pc=(unsigned)__builtin_popcount(n);\n"
     "    unsigned pa=(unsigned)__builtin_parity(n);\n"
     "    unsigned r=(pa? cl : ct) + pc*3u;\n"
     "    h=h*131u+r+cl+ct; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress35", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress35TC("x64o35", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress35TC("x86o35", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress35TC("a64o35", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress35TC("armo35", "int");

INSTANTIATE_TEST_SUITE_P(OptStress35, X64OptStress35RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress35, X86OptStress35RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress35, A64OptStress35RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress35, ARM32OptStress35RT, ::testing::ValuesIn(kARM), rtTCName);
