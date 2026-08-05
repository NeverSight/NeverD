//===- AllPlatform_VectorAlgo29RTTests.cpp - autovectorized kernels --*-C++-*-=//
//
// #397 high-yield probing: kernels clang -O2 auto-vectorizes (scan, argmax,
// saturating widening MAC, Horner, threshold sum, dot product, bit reversal)
// over arrays derived from the runtime seed (no rodata constants — focus on the
// SIMD lowering and reduction lift, not the constant-pool path).  All four
// targets; the result is hashed so any per-lane / reduction / saturation
// divergence flips the return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VA29RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VA29RT, Verify) { roundTripX64(GetParam()); }
class X86VA29RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86VA29RT, Verify) { roundTripX86(GetParam()); }
class A64VA29RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VA29RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32VA29RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VA29RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeVA29TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Prefix sum then reduce — loop-carried scan, vectorizes to a tree.
    {p+"_scan",
     t+" "+p+"_scan("+t+" a){\n"
     "  unsigned v[32], acc=0, h=2166136261u;\n"
     "  for(int i=0;i<32;i++) v[i]=(unsigned)a*2654435761u + (unsigned)i*40503u;\n"
     "  for(int i=0;i<32;i++){ acc+=v[i]; h=(h^acc)*16777619u; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1234ULL}, "VA29", 2},

    // Argmax with index — vectorizes to a max-reduce + index select.
    {p+"_argmax",
     t+" "+p+"_argmax("+t+" a){\n"
     "  int v[40]; int best=-2147483647-1, bi=0;\n"
     "  for(int i=0;i<40;i++) v[i]=(int)((unsigned)a*1103515245u+(unsigned)i*12345u);\n"
     "  for(int i=0;i<40;i++){ if(v[i]>best){best=v[i];bi=i;} }\n"
     "  return ("+t+")(long)(best ^ (bi*0x9E3779B1)); }\n",
     {0x55ULL}, "VA29", 2},

    // Saturating widening multiply-accumulate (i16*i16 -> i32, clamp).
    {p+"_satmac",
     t+" "+p+"_satmac("+t+" a){\n"
     "  short x[32], y[32]; long long acc=0;\n"
     "  for(int i=0;i<32;i++){ x[i]=(short)((unsigned)a+ i*7); y[i]=(short)((unsigned)a*3 - i*5); }\n"
     "  for(int i=0;i<32;i++){ acc+=(long long)x[i]*(long long)y[i];\n"
     "    if(acc>1000000000) acc=1000000000; if(acc<-1000000000) acc=-1000000000; }\n"
     "  return ("+t+")(long)acc; }\n",
     {0x9ULL}, "VA29", 2},

    // Threshold sum + count — branchy reduce that vectorizes to a masked add.
    {p+"_thresh",
     t+" "+p+"_thresh("+t+" a){\n"
     "  unsigned v[48], sum=0, cnt=0;\n"
     "  for(int i=0;i<48;i++) v[i]=(unsigned)a*2246822519u + (unsigned)i*668265263u;\n"
     "  for(int i=0;i<48;i++){ if(v[i]&0x80000000u){ sum+=v[i]; cnt++; } }\n"
     "  return ("+t+")(unsigned long)(sum*31u+cnt); }\n",
     {0x3ULL}, "VA29", 2},

    // Horner polynomial over the array (FMA-friendly float reduce).
    {p+"_horner",
     t+" "+p+"_horner("+t+" a){\n"
     "  float c[16]; float acc=0.f;\n"
     "  for(int i=0;i<16;i++) c[i]=(float)((int)((unsigned)a + (unsigned)i*7u) % 17 - 8);\n"
     "  float xx=1.5f;\n"
     "  for(int i=0;i<16;i++) acc=acc*xx + c[i];\n"
     "  unsigned u; __builtin_memcpy(&u,&acc,4);\n"
     "  return ("+t+")(unsigned long)u; }\n",
     {0x21ULL}, "VA29", 2},

    // Widening dot product (unsigned 8-bit -> 32-bit), classic SAD/dot shape.
    {p+"_dot8",
     t+" "+p+"_dot8("+t+" a){\n"
     "  unsigned char x[64], y[64]; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ x[i]=(unsigned char)((unsigned)a+i*3u); y[i]=(unsigned char)((unsigned)a*5u - i*2u); }\n"
     "  for(int i=0;i<64;i++) acc += (unsigned)x[i]*(unsigned)y[i];\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x7ULL}, "VA29", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeVA29TC("x64v29", "long");
static const std::vector<RoundTripTC> kX86 = makeVA29TC("x86v29", "int");
static const std::vector<RoundTripTC> kA64 = makeVA29TC("a64v29", "long");
static const std::vector<RoundTripTC> kARM = makeVA29TC("armv29", "int");

INSTANTIATE_TEST_SUITE_P(VectorAlgo29, X64VA29RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo29, X86VA29RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo29, A64VA29RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo29, ARM32VA29RT, ::testing::ValuesIn(kARM), rtTCName);
