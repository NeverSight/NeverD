//===- AllPlatform_OptStress57RTTests.cpp - algo kernels II -----*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A second batch of whole-program kernels covering data/memory shapes distinct
// from #464/#465 (VM / FSM / sort / CRC / path / string-walk): 2D matrix math,
// bit-array manipulation, dynamic-programming tables, shift-register sequences,
// rodata-kernel convolution, and run-length packing.  These drive nested-loop
// MAC chains, runtime-indexed bit/byte arrays, and rodata-kernel loads through
// the optimizer to flush any remaining width / addressing / CFG miscompiles.
//
//   * matmul   - 4x4 integer matrix multiply (nested MAC, 2D stack arrays).
//   * bitset   - bit-array set/test/popcount over a runtime index stream.
//   * editdist - Levenshtein distance with a 1D rolling DP row.
//   * lfsr     - Galois LFSR sequence mixed into a hash (shift + conditional xor).
//   * convolve - 1D convolution of a computed signal with a rodata kernel.
//   * runlen   - run-length pack a rodata buffer into a stack array, then fold.
//
// All integer, arrays filled with computed values (never memset/memcpy), fold to
// one return, no float / 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress57RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress57RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress57RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress57RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress57RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress57RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress57RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress57RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress57TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 4x4 integer matrix multiply: nested multiply-accumulate over 2D arrays.
    {p+"_matmul",
     t+" "+p+"_matmul("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<30;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned A[4][4],B[4][4],C[4][4];\n"
     "    for(int i=0;i<4;i++)for(int j=0;j<4;j++){ s=s*1103515245u+12345u;\n"
     "      A[i][j]=(s>>16)&0xff; s=s*1103515245u+12345u; B[i][j]=(s>>16)&0xff; }\n"
     "    for(int i=0;i<4;i++)for(int j=0;j<4;j++){ unsigned acc=0;\n"
     "      for(int k=0;k<4;k++) acc+=A[i][k]*B[k][j]; C[i][j]=acc; }\n"
     "    for(int i=0;i<4;i++)for(int j=0;j<4;j++) h=h*131u+C[i][j];\n"
     "    h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x51u}, "OptStress57", 2},

    // Bit-array set/test/popcount over a runtime index stream.
    {p+"_bitset",
     t+" "+p+"_bitset("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<40;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned bits[8];\n"
     "    for(int i=0;i<8;i++){ s=s*1103515245u+12345u; bits[i]=s; }\n"
     "    unsigned pop=0;\n"
     "    for(int q=0;q<64;q++){ s=s*1103515245u+12345u; unsigned b=(s>>5)&255u;\n"
     "      if((s>>4)&1u) bits[b>>5]|=1u<<(b&31u); else bits[b>>5]&=~(1u<<(b&31u));\n"
     "      if(bits[b>>5]&(1u<<(b&31u))) pop++; }\n"
     "    for(int i=0;i<8;i++){ unsigned v=bits[i];\n"
     "      v=v-((v>>1)&0x55555555u); v=(v&0x33333333u)+((v>>2)&0x33333333u);\n"
     "      v=(v+(v>>4))&0x0f0f0f0fu; pop+=(v*0x01010101u)>>24; }\n"
     "    h=h*131u+pop; }\n"
     "  return ("+t+")h; }\n",
     {0x52u}, "OptStress57", 2},

    // Levenshtein edit distance with a 1D rolling DP row.
    {p+"_editdist",
     "static const char SA[]=\"kitten_sequence_alpha\";\n"
     "static const char SB[]=\"sitting_sequins_alfa\";\n"
     +t+" "+p+"_editdist("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  const int LA=(int)(sizeof(SA)-1), LB=(int)(sizeof(SB)-1);\n"
     "  for(int it=0;it<40;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned off=(s>>7)&3u; unsigned row[24];\n"
     "    for(int j=0;j<=LB;j++) row[j]=(unsigned)j;\n"
     "    for(int i=1;i<=LA;i++){ unsigned prev=row[0]; row[0]=(unsigned)i;\n"
     "      for(int j=1;j<=LB;j++){ unsigned cur=row[j];\n"
     "        unsigned cost=((SA[i-1]+off)==(unsigned)SB[j-1])?0u:1u;\n"
     "        unsigned m=prev+cost; unsigned d=row[j-1]+1u; if(d<m)m=d;\n"
     "        d=row[j]+1u; if(d<m)m=d; row[j]=m; prev=cur; } }\n"
     "    h=h*131u+row[LB]+off; }\n"
     "  return ("+t+")h; }\n",
     {0x53u}, "OptStress57", 2},

    // Galois LFSR sequence mixed into a hash (shift + conditional xor).
    {p+"_lfsr",
     t+" "+p+"_lfsr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int it=0;it<300;it++){ unsigned lsb=s&1u; s>>=1; if(lsb) s^=0xb4bcd35cu;\n"
     "    unsigned t2=s; t2^=t2<<13; t2^=t2>>17; t2^=t2<<5;\n"
     "    h=h*131u+(t2&0xffffu)+(lsb<<20); h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x54u}, "OptStress57", 2},

    // 1D convolution of a computed signal with a rodata kernel (signed taps).
    {p+"_convolve",
     "static const int K[7]={-1,3,-5,11,-5,3,-1};\n"
     +t+" "+p+"_convolve("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<60;it++){ s=s*1103515245u+12345u;\n"
     "    int sig[24];\n"
     "    for(int i=0;i<24;i++){ s=s*1103515245u+12345u; sig[i]=(int)((s>>20)&0x3f)-32; }\n"
     "    for(int i=3;i<21;i++){ int acc=0;\n"
     "      for(int k=0;k<7;k++) acc+=sig[i-3+k]*K[k];\n"
     "      h=h*131u+(unsigned)acc; }\n"
     "    h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x55u}, "OptStress57", 2},

    // Run-length pack a rodata buffer into a stack array, then fold the packs.
    {p+"_runlen",
     "static const unsigned char D[48]={"
     "1,1,1,2,2,3,3,3,3,4,5,5,5,6,6,6,7,7,8,8,8,8,8,9,"
     "1,2,2,2,3,4,4,5,5,5,5,6,7,7,7,8,9,9,9,9,1,1,2};\n"
     +t+" "+p+"_runlen("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<120;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned start=(s>>5)%24u; unsigned packs[48]; int np=0;\n"
     "    unsigned char cur=D[start]; unsigned run=1;\n"
     "    for(unsigned i=start+1;i<48 && np<46;i++){\n"
     "      if(D[i]==cur) run++;\n"
     "      else { packs[np++]=((unsigned)cur<<8)|run; cur=D[i]; run=1; } }\n"
     "    packs[np++]=((unsigned)cur<<8)|run;\n"
     "    unsigned acc=0; for(int i=0;i<np;i++) acc=acc*31u+packs[i];\n"
     "    h=h*131u+acc; h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0x56u}, "OptStress57", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress57TC("x64o57", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress57TC("x86o57", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress57TC("a64o57", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress57TC("armo57", "int");

INSTANTIATE_TEST_SUITE_P(OptStress57, X64OptStress57RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress57, X86OptStress57RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress57, A64OptStress57RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress57, ARM32OptStress57RT, ::testing::ValuesIn(kARM), rtTCName);
