//===- AllPlatform_OptStress17RTTests.cpp - complex-algo probes -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Whole-algorithm roundtrip probes that combine control flow, memory and
// arithmetic in one kernel — historically the most productive bug surface
// (VectorAlgo / ComplexAlgo / HashAlgo rounds) because they exercise the
// *interaction* of NeverD's MedIR passes rather than one idiom at a time:
//
//   * vm        - a tiny stack-machine interpreter (switch dispatch + an operand
//                 stack in memory + arithmetic/flags).
//   * bnmul     - schoolbook 4-limb x 4-limb bignum multiply (nested loops +
//                 carry propagation across a memory accumulator).
//   * rc4       - RC4 KSA + a short PRGA over a 32-byte state (byte memory,
//                 modular index, in-place swaps).
//   * levensht  - Levenshtein edit distance of two short strings (a small DP
//                 row in memory + min cascades).
//   * crc32bit  - bit-by-bit CRC-32 (no table; shift/xor/branch loop).
//   * murmur    - a full MurmurHash3-x86-32 over a small buffer (rotate/mul/xor
//                 mixing + tail switch).
//
// Every kernel uses a small (<=64-byte) stack buffer, is integer-only, folds to
// a single integer return and lowers to no runtime helper, so all four targets
// are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress17RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress17RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress17RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress17RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress17RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress17RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress17RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress17RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress17TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Tiny stack-machine interpreter (switch dispatch + operand stack).
    {p+"_vm",
     t+" "+p+"_vm("+t+" a){\n"
     "  unsigned seed=(unsigned)a|1u; unsigned char prog[24]; int pc=0;\n"
     "  for(int i=0;i<24;i++){ seed=seed*1103515245u+12345u; prog[i]=(unsigned char)(seed>>17); }\n"
     "  long st[8]; int sp=0; long acc=0;\n"
     "  for(pc=0;pc<24;pc++){ int op=prog[pc]&7;\n"
     "    switch(op){\n"
     "     case 0: if(sp<8) st[sp++]=prog[pc]; break;\n"
     "     case 1: if(sp>0) acc+=st[--sp]; break;\n"
     "     case 2: if(sp>0) acc-=st[--sp]; break;\n"
     "     case 3: if(sp>0) acc^=st[--sp]<<1; break;\n"
     "     case 4: acc=(acc<<1)|(acc>>31); break;\n"
     "     case 5: if(sp>1){ long x=st[--sp],y=st[--sp]; st[sp++]=x*y; } break;\n"
     "     case 6: acc+=(sp>0)?st[sp-1]:0; break;\n"
     "     default: acc=acc*31+op; }\n"
     "    acc^=(acc>>13); }\n"
     "  return ("+t+")(unsigned)(acc+sp); }\n",
     {0x4cULL}, "OptStress17", 2},

    // Schoolbook 4-limb x 4-limb bignum multiply with carry propagation.
    {p+"_bnmul",
     t+" "+p+"_bnmul("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned x[4],y[4],r[8]={0};\n"
     "  for(int i=0;i<4;i++){ s=s*1103515245u+12345u; x[i]=s>>9;\n"
     "    s=s*1103515245u+12345u; y[i]=s>>11; }\n"
     "  for(int i=0;i<4;i++){ unsigned long long c=0;\n"
     "    for(int j=0;j<4;j++){ unsigned long long pr=(unsigned long long)x[i]*y[j]\n"
     "        +r[i+j]+c; r[i+j]=(unsigned)pr; c=pr>>32; }\n"
     "    r[i+4]=(unsigned)c; }\n"
     "  unsigned h=0; for(int k=0;k<8;k++) h=h*131u+r[k];\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress17", 2},

    // RC4 KSA + short PRGA over a 32-byte state.
    {p+"_rc4",
     t+" "+p+"_rc4("+t+" a){\n"
     "  unsigned seed=(unsigned)a|1u; unsigned char S[32], key[8];\n"
     "  for(int i=0;i<8;i++){ seed=seed*1103515245u+12345u; key[i]=(unsigned char)(seed>>16); }\n"
     "  for(int i=0;i<32;i++) S[i]=(unsigned char)i;\n"
     "  int j=0; for(int i=0;i<32;i++){ j=(j+S[i]+key[i&7])&31;\n"
     "    unsigned char tmp=S[i]; S[i]=S[j]; S[j]=tmp; }\n"
     "  int x=0,y=0; unsigned h=0;\n"
     "  for(int k=0;k<32;k++){ x=(x+1)&31; y=(y+S[x])&31;\n"
     "    unsigned char tmp=S[x]; S[x]=S[y]; S[y]=tmp;\n"
     "    h=h*131u+S[(S[x]+S[y])&31]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress17", 2},

    // Levenshtein edit distance of two short strings (small DP row + min).
    {p+"_levensht",
     t+" "+p+"_levensht("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char A[7],B[7];\n"
     "  for(int i=0;i<7;i++){ s=s*1103515245u+12345u; A[i]=(unsigned char)('a'+((s>>20)%5));\n"
     "    s=s*1103515245u+12345u; B[i]=(unsigned char)('a'+((s>>20)%5)); }\n"
     "  int row[8]; for(int j=0;j<=7;j++) row[j]=j;\n"
     "  for(int i=1;i<=7;i++){ int prev=row[0]; row[0]=i;\n"
     "    for(int j=1;j<=7;j++){ int cur=row[j];\n"
     "      int cost=(A[i-1]==B[j-1])?0:1;\n"
     "      int del=row[j]+1, ins=row[j-1]+1, sub=prev+cost;\n"
     "      int m=del<ins?del:ins; m=m<sub?m:sub; row[j]=m; prev=cur; } }\n"
     "  return ("+t+")(unsigned)row[7]; }\n",
     {0x35ULL}, "OptStress17", 2},

    // Bit-by-bit CRC-32 (no table; shift/xor/branch loop).
    {p+"_crc32bit",
     t+" "+p+"_crc32bit("+t+" a){\n"
     "  unsigned seed=(unsigned)a|1u; unsigned char buf[20];\n"
     "  for(int i=0;i<20;i++){ seed=seed*1103515245u+12345u; buf[i]=(unsigned char)(seed>>15); }\n"
     "  unsigned crc=0xffffffffu;\n"
     "  for(int i=0;i<20;i++){ crc^=buf[i];\n"
     "    for(int b=0;b<8;b++){ unsigned m=-(crc&1u); crc=(crc>>1)^(0xedb88320u&m); } }\n"
     "  return ("+t+")(unsigned)(crc^0xffffffffu); }\n",
     {0x6dULL}, "OptStress17", 2},

    // Full MurmurHash3-x86-32 over a small buffer.
    {p+"_murmur",
     t+" "+p+"_murmur("+t+" a){\n"
     "  unsigned seed=(unsigned)a|1u; unsigned char buf[18];\n"
     "  for(int i=0;i<18;i++){ seed=seed*1103515245u+12345u; buf[i]=(unsigned char)(seed>>19); }\n"
     "  unsigned h=0x9747b28cu; int len=18; int nb=len/4;\n"
     "  for(int i=0;i<nb;i++){\n"
     "    unsigned k=buf[i*4]|(buf[i*4+1]<<8)|(buf[i*4+2]<<16)|((unsigned)buf[i*4+3]<<24);\n"
     "    k*=0xcc9e2d51u; k=(k<<15)|(k>>17); k*=0x1b873593u;\n"
     "    h^=k; h=(h<<13)|(h>>19); h=h*5u+0xe6546b64u; }\n"
     "  unsigned k1=0; int tail=nb*4;\n"
     "  switch(len&3){\n"
     "    case 3: k1^=(unsigned)buf[tail+2]<<16; /*fallthrough*/\n"
     "    case 2: k1^=(unsigned)buf[tail+1]<<8; /*fallthrough*/\n"
     "    case 1: k1^=buf[tail]; k1*=0xcc9e2d51u; k1=(k1<<15)|(k1>>17); k1*=0x1b873593u; h^=k1; }\n"
     "  h^=(unsigned)len; h^=h>>16; h*=0x85ebca6bu; h^=h>>13; h*=0xc2b2ae35u; h^=h>>16;\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress17", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress17TC("x64o17", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress17TC("x86o17", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress17TC("a64o17", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress17TC("armo17", "int");

INSTANTIATE_TEST_SUITE_P(OptStress17, X64OptStress17RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress17, X86OptStress17RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress17, A64OptStress17RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress17, ARM32OptStress17RT, ::testing::ValuesIn(kARM), rtTCName);
