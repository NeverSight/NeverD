//===- AllPlatform_OptStress116RTTests.cpp - modpow / DDA / Newton shapes --==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * modpow - modular exponentiation of rodata (base,exp) pairs by square-and-
//              multiply under a CONSTANT modulus 65521 (folds to a magic
//              multiply, products stay < 2^32, no 64-bit / libcall).  Pins a
//              bit-driven square-and-multiply recurrence over rodata operands.
//   * bres   - Bresenham integer line walk between rodata endpoint quads: the
//              error recurrence `e2=2*err` with `err+=dx`/`err-=dy`.  Pins an
//              all-integer DDA error accumulator (no divide) over rodata points.
//   * recip  - Newton-Raphson fixed-point reciprocal refinement seeded by a
//              rodata initial-guess table indexed by the divisor's leading bits.
//              Pins a multiplicative refine loop combined with a rodata seed
//              gather (distinct from the bitwise sqrt).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress116RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress116RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress116RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress116RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress116RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress116RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress116RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress116RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress116TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // modular exponentiation by square-and-multiply (constant modulus 65521).
    {p+"_modpow",
     "static const unsigned char "+p+"_base[16]={\n"
     "0x57,0x83,0x1f,0xc4,0x6a,0x9e,0x2d,0xb8, 0x05,0xf1,0x4c,0xa7,0x39,0xd0,0x6e,0x92};\n"
     "static const unsigned char "+p+"_exp[16]={\n"
     "13,7,21,4,9,30,2,18, 25,11,6,28,3,15,22,8};\n"
     +t+" "+p+"_modpow("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<16;i++){\n"
     "      unsigned base=("+p+"_base[i]^(s&0xFFu))%65521u;\n"
     "      unsigned e="+p+"_exp[i]+((s>>8)&0xFu), r=1u;\n"
     "      while(e){ if(e&1u) r=(r*base)%65521u; base=(base*base)%65521u; e>>=1; }\n"
     "      acc=acc*131u+r; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Fu}, "OptStress116", 2},

    // Bresenham integer line walk between rodata endpoint quads (DDA error term).
    {p+"_bres",
     "static const unsigned char "+p+"_pts[32]={\n"
     "2,3,40,30, 50,5,8,48, 12,60,55,2, 33,9,1,44, 20,52,60,18, 7,28,48,3, 36,14,2,50, 25,40,60,6};\n"
     +t+" "+p+"_bres("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int L=0;L+4<=32;L+=4){\n"
     "      int x0="+p+"_pts[L]+(int)(s&1u), y0="+p+"_pts[L+1];\n"
     "      int x1="+p+"_pts[L+2], y1="+p+"_pts[L+3];\n"
     "      int dx=x1-x0; if(dx<0) dx=-dx; int dy=y1-y0; if(dy<0) dy=-dy;\n"
     "      int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy, guard=0;\n"
     "      while(guard++<256){ acc=acc*131u+(unsigned)(x0*7+y0*13);\n"
     "        if(x0==x1 && y0==y1) break; int e2=2*err;\n"
     "        if(e2>-dy){ err-=dy; x0+=sx; } if(e2<dx){ err+=dx; y0+=sy; } } }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB7u}, "OptStress116", 2},

    // Newton-Raphson fixed-point reciprocal refine seeded by a rodata table.
    {p+"_recip",
     "static const unsigned char "+p+"_seed[16]={\n"
     "250,210,180,156,138,124,112,102, 94,87,81,76,71,67,63,60};\n"
     "static const unsigned char "+p+"_x[24]={\n"
     "3,17,42,9,128,75,200,33, 61,150,7,99,255,18,84,46, 23,177,5,112,68,201,38,90};\n"
     +t+" "+p+"_recip("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<24;i++){ unsigned x="+p+"_x[i]|1u;\n"
     "      unsigned idx=(x>>4)&15u; unsigned y=(unsigned)"+p+"_seed[idx]+1u;\n"
     "      for(int k=0;k<5;k++){ unsigned t2=(x*y)>>8; unsigned d=(t2<512u)?(512u-t2):0u;\n"
     "        y=((y*d)>>8)+((s>>(k&7))&1u); y&=0xFFFFu; }\n"
     "      acc=acc*131u+y; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Cu}, "OptStress116", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress116TC("x64o116", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress116TC("x86o116", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress116TC("a64o116", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress116TC("armo116", "int");

INSTANTIATE_TEST_SUITE_P(OptStress116, X64OptStress116RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress116, X86OptStress116RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress116, A64OptStress116RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress116, ARM32OptStress116RT, ::testing::ValuesIn(kARM), rtTCName);
