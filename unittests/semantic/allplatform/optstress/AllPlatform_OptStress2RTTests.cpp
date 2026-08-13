//===- AllPlatform_OptStress2RTTests.cpp - optimizer-path stressors -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing aimed at NeverD's own MedIR passes (MedFlags
// flag folding, MedDCE liveness, MedPropagation const/copy prop, LowToMed
// sub-register synthesis), which historically harbour the subtlest semantic
// divergences.  Each kernel is a plain C expression compiled at -O2 so clang's
// optimizer produces the dense flag/sub-register/overflow idioms those passes
// must model exactly; the roundtrip then compares native vs lifted execution.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Opt2RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Opt2RT, Verify) { roundTripX64(GetParam()); }
class X86Opt2RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Opt2RT, Verify) { roundTripX86(GetParam()); }
class A64Opt2RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Opt2RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Opt2RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Opt2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOpt2TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Overflow-detecting accumulation: __builtin_*_overflow lowers to flag reads
    // (CF/OF) consumed by selects — stresses MedFlags carry/overflow modelling.
    {p+"_ovf",
     t+" "+p+"_ovf("+t+" a){\n"
     "  unsigned acc=(unsigned)a, sat=0;\n"
     "  for(int i=0;i<48;i++){ unsigned x=(unsigned)a*(unsigned)(i+1), s;\n"
     "    if(__builtin_add_overflow(acc,x,&s)){ sat++; s=0xFFFFFFFFu; }\n"
     "    unsigned m; if(__builtin_mul_overflow(s,3u,&m)){ sat++; m=s; }\n"
     "    acc=m^(acc>>7); }\n"
     "  return ("+t+")(unsigned long)(acc+sat*2654435761u); }\n",
     {0x51ULL}, "Opt2", 2},

    // Branchless min/max sorting network over 6 lanes — CMOV / csel chains the
    // flag pass must keep paired with the right comparison.
    {p+"_sortnet",
     t+" "+p+"_sortnet("+t+" a){\n"
     "  int v[6]; for(int i=0;i<6;i++) v[i]=(int)((unsigned)a*(unsigned)(i*7+1)+i*13);\n"
     "  static const int net[12][2]={{0,1},{2,3},{4,5},{0,2},{1,4},{3,5},\n"
     "    {0,1},{2,3},{4,5},{1,2},{3,4},{2,3}};\n"
     "  for(int k=0;k<12;k++){ int x=net[k][0],y=net[k][1];\n"
     "    int lo=v[x]<v[y]?v[x]:v[y], hi=v[x]<v[y]?v[y]:v[x]; v[x]=lo; v[y]=hi; }\n"
     "  unsigned h=0; for(int i=0;i<6;i++) h=h*131u+(unsigned)v[i];\n"
     "  return ("+t+")(long)(int)h; }\n",
     {0x9ULL}, "Opt2", 2},

    // Q16.16 fixed-point multiply with rounding — the 32x32->64 widening
    // multiply plus the >>16 round exercises wide-mul / shift sub-register
    // stitching, while the divide stays 32-bit (native idiv/sdiv) so the kernel
    // never reaches a 64-bit division libcall the bare-metal harness can't run.
    {p+"_fixp",
     t+" "+p+"_fixp("+t+" a){\n"
     "  int acc=(int)a|1, base=((int)a<<8)|0x100;\n"
     "  for(int i=1;i<=40;i++){\n"
     "    long long m=((long long)acc*(long long)(base+i*17));\n"
     "    int prod=(int)((m+0x8000)>>16);\n"
     "    int q=prod/(base|1);\n"
     "    acc=(acc*3+q)^(acc>>5); }\n"
     "  return ("+t+")(long)acc; }\n",
     {0x7ULL}, "Opt2", 2},

    // Dead-on-some-paths values: a variable defined in one arm and read only when
    // a later condition holds — stresses MedDCE cross-block liveness.
    {p+"_deadlive",
     t+" "+p+"_deadlive("+t+" a){\n"
     "  unsigned acc=(unsigned)a, carry=0;\n"
     "  for(int i=0;i<50;i++){ unsigned t1=0,t2=0; int has=0;\n"
     "    if((acc&3u)==0){ t1=acc*2654435761u; has=1; }\n"
     "    else if((acc&3u)==1){ t2=acc^0xA5A5A5A5u; has=2; }\n"
     "    else { acc=(acc<<1)|(acc>>31); }\n"
     "    if(has==1) acc+=t1+carry; else if(has==2) acc^=t2; \n"
     "    carry=(acc>>28)&0xF; acc+=(unsigned)i; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x3ULL}, "Opt2", 2},

    // Constant-fold + overflow wrap stress: chains of constant ops the
    // propagation pass folds, mixed with runtime values so it must mask widths.
    {p+"_constfold",
     t+" "+p+"_constfold("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<60;i++){\n"
     "    unsigned k=(0x80000000u+0x80000001u)+(unsigned)i;\n"
     "    unsigned m=(0xFFFFFFFFu*3u)^(0x0F0F0F0Fu+0xF0F0F0F0u);\n"
     "    acc=(acc+k)*5u; acc^=m; acc=(acc<<3)|(acc>>29); acc-=k; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x2ULL}, "Opt2", 2},

    // Mixed-width sub-register reuse: 8/16/32-bit views of the same value
    // recombined — stresses LowToMed partial-write zero/sign extension.
    {p+"_subreg",
     t+" "+p+"_subreg("+t+" a){\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<56;i++){\n"
     "    unsigned char b=(unsigned char)(acc+(unsigned)i);\n"
     "    unsigned short w=(unsigned short)(acc>>(i&15));\n"
     "    signed char sb=(signed char)b; short sw=(short)w;\n"
     "    acc=((unsigned)(int)sb)+((unsigned)(int)sw)*131u+(unsigned)b*7u+(unsigned)w; \n"
     "    acc^=(acc<<11); acc+=(unsigned)(b^(w&0xFF)); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0xABULL}, "Opt2", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOpt2TC("x64opt2", "long");
static const std::vector<RoundTripTC> kX86 = makeOpt2TC("x86opt2", "int");
static const std::vector<RoundTripTC> kA64 = makeOpt2TC("a64opt2", "long");
static const std::vector<RoundTripTC> kARM = makeOpt2TC("armopt2", "int");

INSTANTIATE_TEST_SUITE_P(Opt2, X64Opt2RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt2, X86Opt2RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt2, A64Opt2RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt2, ARM32Opt2RT, ::testing::ValuesIn(kARM), rtTCName);
