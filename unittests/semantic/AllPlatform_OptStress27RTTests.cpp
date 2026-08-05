//===- AllPlatform_OptStress27RTTests.cpp - opt-stress probes --*-C++*-=//
//
// This round targets the self-written MedIR optimizer passes (MedFlags,
// MedPropagation, MedDCE, LowToMed sub-register modelling) with the recurring
// miscompile classes: partial-register (8/16-bit) flag idioms, branchless
// select/clamp chains that reuse one comparison, mixed signed/unsigned tests of
// the same operands, and explicit multi-word carry/borrow ripples.
//
//   * satmix   - signed 16-bit saturating accumulate, branchless clamp (cmov).
//   * signsel  - same operands compared signed AND unsigned, results combined.
//   * carry4   - manual 128-bit (4x32) add/sub ripple via 64-bit carry idiom.
//   * cmovsort - 6-element conditional-swap sort (cmov/csel reusing compares).
//   * bittog   - bit toggling + inline popcount trick driving a sign branch.
//   * halfmul  - 16x16->32 multiply with high/low half extraction (sub-reg).
//
// All kernels are integer-only, fold to a single integer return and use no
// 64-bit divide (the only 64-bit op needing a runtime helper on 32-bit), so all
// four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress27RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress27RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress27RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress27RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress27RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress27RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress27RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress27RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress27TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed 16-bit saturating accumulate with branchless clamp (cmov/csel).
    {p+"_satmix",
     t+" "+p+"_satmix("+t+" a){\n"
     "  short acc=(short)a; signed char d=(signed char)(a^0x5a); unsigned h=0;\n"
     "  for(int i=0;i<60;i++){\n"
     "    int v=acc + d*(i+1);\n"
     "    if(v>32767)v=32767; if(v<-32768)v=-32768;\n"
     "    acc=(short)v; d=(signed char)(d*3+7);\n"
     "    h=h*131u+(unsigned short)acc+(unsigned char)d; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x1234ULL}, "OptStress27", 2},

    // Same operands compared signed AND unsigned, plus sign tests, combined.
    {p+"_signsel",
     t+" "+p+"_signsel("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s>>7); unsigned ux=s, uy=s>>7; unsigned r=0;\n"
     "    r+=(x<y)?2u:0u; r+=(ux<uy)?4u:0u;\n"
     "    r+=(x>=0&&y<0)?8u:0u; r+=(x<=y)?1u:0u;\n"
     "    h=h*131u+r+s; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9aULL}, "OptStress27", 2},

    // Manual 128-bit (4x32) add/sub ripple through the 64-bit carry idiom.
    {p+"_carry4",
     t+" "+p+"_carry4("+t+" a){\n"
     "  unsigned w[4]; unsigned s=(unsigned)a|1u;\n"
     "  for(int k=0;k<4;k++){ s=s*1103515245u+12345u; w[k]=s; }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned carry=s;\n"
     "    for(int k=0;k<4;k++){ unsigned long long e=(unsigned long long)w[k]+carry;\n"
     "      w[k]=(unsigned)e; carry=(unsigned)(e>>32); }\n"
     "    unsigned borrow=(s>>3);\n"
     "    for(int k=0;k<4;k++){ unsigned long long e=(unsigned long long)w[k]-borrow;\n"
     "      w[k]=(unsigned)e; borrow=(e>>32)?1u:0u; }\n"
     "    h=h*131u+w[0]+w[1]+w[2]+w[3]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xc5ULL}, "OptStress27", 2},

    // 6-element conditional-swap sort (cmov/csel reusing the compare result).
    {p+"_cmovsort",
     t+" "+p+"_cmovsort("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int it=0;it<24;it++){ int v[6];\n"
     "    for(int k=0;k<6;k++){ s=s*1103515245u+12345u; v[k]=(int)(s>>3); }\n"
     "    for(int i=0;i<6;i++) for(int j=i+1;j<6;j++)\n"
     "      if(v[i]>v[j]){ int tmp=v[i]; v[i]=v[j]; v[j]=tmp; }\n"
     "    for(int k=0;k<6;k++) h=h*131u+(unsigned)v[k]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x77ULL}, "OptStress27", 2},

    // Bit toggling + inline popcount trick driving a sign-dependent branch.
    {p+"_bittog",
     t+" "+p+"_bittog("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned acc=0; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    acc^=(1u<<((s>>11)&31u));\n"
     "    if(acc&(1u<<((s>>3)&31u))) acc+=s; else acc-=s;\n"
     "    unsigned x=acc; x=x-((x>>1)&0x55555555u);\n"
     "    x=(x&0x33333333u)+((x>>2)&0x33333333u);\n"
     "    x=(x+(x>>4))&0x0f0f0f0fu; unsigned pc=(x*0x01010101u)>>24;\n"
     "    h=h*131u+acc+pc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2bULL}, "OptStress27", 2},

    // 16x16->32 multiply with high/low half extraction (partial-register).
    {p+"_halfmul",
     t+" "+p+"_halfmul("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned short x=(unsigned short)s, y=(unsigned short)(s>>13);\n"
     "    unsigned pr=(unsigned)x*(unsigned)y;\n"
     "    unsigned short lo=(unsigned short)pr, hi=(unsigned short)(pr>>16);\n"
     "    short sx=(short)s, sy=(short)(s>>11); int sp=(int)sx*(int)sy;\n"
     "    h=h*131u+lo+hi+(unsigned)sp; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x5eULL}, "OptStress27", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress27TC("x64o27", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress27TC("x86o27", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress27TC("a64o27", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress27TC("armo27", "int");

INSTANTIATE_TEST_SUITE_P(OptStress27, X64OptStress27RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress27, X86OptStress27RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress27, A64OptStress27RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress27, ARM32OptStress27RT, ::testing::ValuesIn(kARM), rtTCName);
