//===- AllPlatform_CarryChainRTTests.cpp - multi-precision carry --*- C++ -*-===//
//
// Multi-precision (multi-limb) integer arithmetic and overflow-checked builtins.
// clang -O2 lowers 128-bit add/sub into adds/adc and subs/sbc (x86), adds/adc
// and subs/sbc (AArch64), adds/adc and subs/sbc (ARM32 register pairs); the
// carry/borrow flag is produced by one instruction and consumed by the next,
// and across a loop the high-limb accumulator is loop-carried.  This is exactly
// the MedFlags carry/borrow optimizer area that has been mis-folded before
// (#222 paddusw, #253 cmovae carry), so a wrong carry source silently corrupts
// the high limb.  __builtin_*_overflow probes additionally read OF/CF directly.
//
// Every function folds the full 64-bit state into a 32-bit-sensitive return so
// the ARM32 path (R0 = low 32 bits) still detects high-limb errors.  Arithmetic
// avoids 64-bit divide and 64-bit multiply-overflow so no runtime library call
// is emitted on any target.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CarryChainRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CarryChainRT, Verify) { roundTripX64(GetParam()); }

class A64CarryChainRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CarryChainRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32CarryChainRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CarryChainRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeCarryTC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 128-bit accumulator: (hi:lo) += (addhi:addlo) every iteration.  Pure
    // adds/adc carry chain with a loop-carried high limb.
    {p+"_acc128",
     t+" "+p+"_acc128("+t+" a) {\n"
     "  unsigned long long lo=0, hi=0;\n"
     "  for (int i=0;i<200;i++){\n"
     "    unsigned long long addlo=(unsigned)(a*(i+1));\n"
     "    unsigned long long addhi=(unsigned)(a*(i+3))>>1;\n"
     "    unsigned long long nlo=lo+addlo;\n"
     "    hi += addhi + (nlo<lo); lo=nlo; }\n"
     "  unsigned long long r=hi^lo;\n"
     "  return ("+t+")((unsigned)r ^ (unsigned)(r>>32));\n"
     "}\n",
     {0x1234567ULL}, "CarryChain", opt, fl},

    // 128-bit running subtraction: (hi:lo) -= (subhi:sublo).  subs/sbc borrow
    // chain, loop-carried.
    {p+"_sub128",
     t+" "+p+"_sub128("+t+" a) {\n"
     "  unsigned long long lo=0xFFFFFFFFFFFFFFFFULL, hi=0xFFFFFFFFFFFFFFFFULL;\n"
     "  for (int i=0;i<200;i++){\n"
     "    unsigned long long sublo=(unsigned)(a*(i+1));\n"
     "    unsigned long long subhi=(unsigned)(a*(i+5))>>2;\n"
     "    unsigned long long nlo=lo-sublo;\n"
     "    hi -= subhi + (lo<sublo); lo=nlo; }\n"
     "  unsigned long long r=hi^lo;\n"
     "  return ("+t+")((unsigned)r ^ (unsigned)(r>>32));\n"
     "}\n",
     {0x2233445ULL}, "CarryChain", opt, fl},

    // 64-bit multiply-accumulate via 32x32->64 widening (umull/umlal): a
    // loop-carried 64-bit accumulator with carry into the high word.
    {p+"_mac64",
     t+" "+p+"_mac64("+t+" a) {\n"
     "  unsigned long long acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    unsigned x=(unsigned)(a*(i+1)), y=(unsigned)(a+i*7);\n"
     "    acc += (unsigned long long)x * (unsigned long long)y; }\n"
     "  unsigned long long r=acc;\n"
     "  return ("+t+")((unsigned)r ^ (unsigned)(r>>32));\n"
     "}\n",
     {0x3344556ULL}, "CarryChain", opt, fl},

    // 3-limb (96-bit) bignum add of two arrays with ripple carry across limbs;
    // the carry is loop-carried through the inner limb loop.
    {p+"_ripple3",
     t+" "+p+"_ripple3("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int k=0;k<64;k++){\n"
     "    unsigned x[3]={(unsigned)(a*(k+1)),(unsigned)(a*(k+2)),(unsigned)(a*(k+3))};\n"
     "    unsigned y[3]={(unsigned)(a+k),(unsigned)(a*7+k),(unsigned)(a*13+k)};\n"
     "    unsigned r[3]; unsigned long long c=0;\n"
     "    for (int j=0;j<3;j++){ unsigned long long s=(unsigned long long)x[j]+y[j]+c;\n"
     "      r[j]=(unsigned)s; c=s>>32; }\n"
     "    acc ^= r[0]+r[1]*3u+r[2]*5u+(unsigned)c; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "CarryChain", opt, fl},

    // Lexicographic compare of two 128-bit numbers from the high limb down;
    // returns -1/0/1.  Borrow/compare flag chains feeding csel/cmov.
    {p+"_cmp128",
     t+" "+p+"_cmp128("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<200;i++){\n"
     "    unsigned long long alo=(unsigned)(a*(i+1)), ahi=(unsigned)(a*(i+2));\n"
     "    unsigned long long blo=(unsigned)(a*(i+3)), bhi=(unsigned)(a+i);\n"
     "    int c; if(ahi!=bhi) c=(ahi<bhi)?-1:1;\n"
     "    else c=(alo<blo)?-1:((alo>blo)?1:0);\n"
     "    acc += c*(i+1); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "CarryChain", opt, fl},

    // Signed overflow-checked accumulate with saturation (reads OF flag).
    {p+"_ovfadd",
     t+" "+p+"_ovfadd("+t+" a) {\n"
     "  int s=0;\n"
     "  for (int i=0;i<256;i++){ int v=(int)(a*(i*131+7));\n"
     "    int r; if(__builtin_add_overflow(s,v,&r)) s=(v>0)?2000000000:-2000000000; else s=r; }\n"
     "  return ("+t+")s;\n"
     "}\n",
     {0x6677889ULL}, "CarryChain", opt, fl},

    // Signed multiply-overflow-checked running product with reset on overflow.
    {p+"_ovfmul",
     t+" "+p+"_ovfmul("+t+" a) {\n"
     "  int s=0, prod=1;\n"
     "  for (int i=1;i<=200;i++){ int v=((int)(a*i)%7)+2;\n"
     "    int r; if(__builtin_mul_overflow(prod,v,&r)){ s+=prod; prod=v; } else prod=r; }\n"
     "  return ("+t+")(s+prod);\n"
     "}\n",
     {0x778899AULL}, "CarryChain", opt, fl},

    // Unsigned subtract-with-borrow accumulate (reads CF/borrow flag).
    {p+"_ovfsub",
     t+" "+p+"_ovfsub("+t+" a) {\n"
     "  unsigned s=0x80000000u; int acc=0;\n"
     "  for (int i=0;i<256;i++){ unsigned v=(unsigned)(a*(i+1));\n"
     "    unsigned r; if(__builtin_sub_overflow(s,v,&r)){ s+=v; acc-=1; } else { s=r; acc+=1; } }\n"
     "  return ("+t+")(acc + (int)(s&0xFFFFu));\n"
     "}\n",
     {0x88990ABULL}, "CarryChain", opt, fl},

    // 3-limb (192-bit) add-with-carry chain via __builtin_addcll: forces a real
    // add/adc/adc (x86) / adds/adcs/adc (ARM) chain where the MIDDLE carry-out
    // is consumed by the next limb — exposes any adc/adcs flag mis-attribution
    // that a 2-limb add (final carry dead) would hide.  Each 64-bit limb is two
    // 32-bit halves so carries truly propagate.
    {p+"_addc3",
     t+" "+p+"_addc3("+t+" a) {\n"
     "  unsigned long long acc=0;\n"
     "  for (int k=0;k<64;k++){\n"
     "    unsigned long long x0=((unsigned long long)(unsigned)(a*(k+1))<<32)|(unsigned)(a*(k+2));\n"
     "    unsigned long long x1=((unsigned long long)(unsigned)(a*(k+3))<<32)|(unsigned)(a*(k+4));\n"
     "    unsigned long long x2=((unsigned long long)(unsigned)(a*(k+5))<<32)|(unsigned)(a*(k+6));\n"
     "    unsigned long long y0=((unsigned long long)(unsigned)(a+k)<<32)|(unsigned)(a*7+k);\n"
     "    unsigned long long y1=((unsigned long long)(unsigned)(a*11+k)<<32)|(unsigned)(a*13+k);\n"
     "    unsigned long long y2=((unsigned long long)(unsigned)(a*17+k)<<32)|(unsigned)(a*19+k);\n"
     "    unsigned long long c=0;\n"
     "    unsigned long long r0=__builtin_addcll(x0,y0,0,&c);\n"
     "    unsigned long long r1=__builtin_addcll(x1,y1,c,&c);\n"
     "    unsigned long long r2=__builtin_addcll(x2,y2,c,&c);\n"
     "    acc ^= r0+r1*3u+r2*5u+c; }\n"
     "  unsigned long long r=acc;\n"
     "  return ("+t+")((unsigned)r ^ (unsigned)(r>>32));\n"
     "}\n",
     {0x1A2B3C4ULL}, "CarryChain", opt, fl},

    // 3-limb (192-bit) subtract-with-borrow chain via __builtin_subcll: forces a
    // real sub/sbb/sbb (x86) / subs/sbcs/sbc (ARM) chain consuming the middle
    // borrow-out.
    {p+"_subc3",
     t+" "+p+"_subc3("+t+" a) {\n"
     "  unsigned long long acc=0;\n"
     "  for (int k=0;k<64;k++){\n"
     "    unsigned long long x0=((unsigned long long)(unsigned)(a*(k+2))<<32)|(unsigned)(a*(k+1));\n"
     "    unsigned long long x1=((unsigned long long)(unsigned)(a*(k+4))<<32)|(unsigned)(a*(k+3));\n"
     "    unsigned long long x2=((unsigned long long)(unsigned)(a*(k+6))<<32)|(unsigned)(a*(k+5));\n"
     "    unsigned long long y0=((unsigned long long)(unsigned)(a*7+k)<<32)|(unsigned)(a+k);\n"
     "    unsigned long long y1=((unsigned long long)(unsigned)(a*13+k)<<32)|(unsigned)(a*11+k);\n"
     "    unsigned long long y2=((unsigned long long)(unsigned)(a*19+k)<<32)|(unsigned)(a*17+k);\n"
     "    unsigned long long b=0;\n"
     "    unsigned long long r0=__builtin_subcll(x0,y0,0,&b);\n"
     "    unsigned long long r1=__builtin_subcll(x1,y1,b,&b);\n"
     "    unsigned long long r2=__builtin_subcll(x2,y2,b,&b);\n"
     "    acc ^= r0+r1*3u+r2*5u+b; }\n"
     "  unsigned long long r=acc;\n"
     "  return ("+t+")((unsigned)r ^ (unsigned)(r>>32));\n"
     "}\n",
     {0x2B3C4D5ULL}, "CarryChain", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Carry =
    makeCarryTC("x64cc", "long", 2, "");
static const std::vector<RoundTripTC> kA64Carry =
    makeCarryTC("a64cc", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Carry =
    makeCarryTC("armcc", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(CarryChain, X64CarryChainRT,
                         ::testing::ValuesIn(kX64Carry), rtTCName);
INSTANTIATE_TEST_SUITE_P(CarryChain, A64CarryChainRT,
                         ::testing::ValuesIn(kA64Carry), rtTCName);
INSTANTIATE_TEST_SUITE_P(CarryChain, ARM32CarryChainRT,
                         ::testing::ValuesIn(kARM32Carry), rtTCName);
