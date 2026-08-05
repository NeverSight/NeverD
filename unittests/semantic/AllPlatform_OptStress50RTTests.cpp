//===- AllPlatform_OptStress50RTTests.cpp - rodata+stack mix ---*-C++*-=//
//
// The constant-pool redirect must fire for a read-only `.rodata` lookup table
// but NOT for a stack array (whose absolute frame displacement can look like a
// low rodata VA).  These probes interleave a `static const` rodata table with a
// runtime-filled stack array in one function so the redirect's read-only /
// stored-base gating (StoredConstBases) is exercised both ways at once:
//
//   * tblplusarr - rodata table read + stack array read/write per step, summed
//                  (redirect the table, keep the stack array absolute).
//   * tblcopy    - copy the rodata table into a stack array, then index the
//                  stack copy (rodata read source + stack write/read dest).
//   * twotbl     - two rodata tables, one's value indexes the other
//                  (cross-table chaining, both redirected).
//   * gathermix  - gather from the rodata table at a scrambled index into a
//                  stack histogram, fold the histogram (read table / RMW stack).
//   * sboxround  - an sbox-style rodata byte table driving a stack-state round
//                  function (byte sub-word table loads + stack state update).
//   * idxtbl     - rodata index table whose entries select stack-array slots
//                  (rodata-controlled stack permutation).
//
// All integer, arrays seed from the LCG (no memset zero-init the loader would
// drop), fold to one return, no float / 64-bit divide helper.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress50RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress50RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress50RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress50RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress50RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress50RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress50RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress50RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress50TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // rodata table read + stack array read/write per step.
    {p+"_tblplusarr",
     "static const unsigned T[16]={7,3,11,2,13,5,17,9,19,1,23,8,29,4,31,6};\n"
     +t+" "+p+"_tblplusarr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned v[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; v[i]=s>>8; }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; v[j]=v[j]+T[j]; h=h*131u+v[j]+T[(j+1)&15u]; }\n"
     "  return ("+t+")h; }\n",
     {0x51u}, "OptStress50", 2},

    // Copy the rodata table into a stack array, then index the stack copy.
    {p+"_tblcopy",
     "static const unsigned T[16]={101,202,303,404,505,606,707,808,"
     "909,111,222,333,444,555,666,777};\n"
     +t+" "+p+"_tblcopy("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned v[16];\n"
     "  for(int i=0;i<16;i++) v[i]=T[i]+(unsigned)i;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)&15u, k=(s>>10)&15u;\n"
     "    v[j]=(v[j]^v[k])+T[k]; h=h*131u+v[j]; }\n"
     "  return ("+t+")h; }\n",
     {0x52u}, "OptStress50", 2},

    // Two rodata tables, one's value indexes the other.
    {p+"_twotbl",
     "static const unsigned A[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static const unsigned B[16]={11,22,33,44,55,66,77,88,"
     "99,10,20,30,40,50,60,70};\n"
     +t+" "+p+"_twotbl("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; unsigned k=A[j]&15u;\n"
     "    h=h*131u+B[k]+A[(k+1)&15u]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x53u}, "OptStress50", 2},

    // Gather from a rodata table into a stack histogram, fold the histogram.
    {p+"_gathermix",
     "static const unsigned T[16]={2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53};\n"
     +t+" "+p+"_gathermix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned c[16];\n"
     "  for(int i=0;i<16;i++) c[i]=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>7)&15u; c[T[j]&15u]+=T[j]; }\n"
     "  unsigned h=0; for(int i=0;i<16;i++) h=h*131u+c[i];\n"
     "  return ("+t+")h; }\n",
     {0x54u}, "OptStress50", 2},

    // sbox-style rodata byte table driving a stack-state round function.
    {p+"_sboxround",
     "static const unsigned char S[16]={0x6,0xb,0x5,0x4,0x2,0xe,0x7,0xa,"
     "0x9,0xd,0xf,0xc,0x3,0x1,0x0,0x8};\n"
     +t+" "+p+"_sboxround("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char st[8];\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; st[i]=(unsigned char)(s>>9); }\n"
     "  unsigned h=0;\n"
     "  for(int r=0;r<80;r++){\n"
     "    for(int i=0;i<8;i++){ st[i]=(unsigned char)((S[st[i]&15u]<<4)|S[(st[i]>>4)&15u]); }\n"
     "    unsigned char t0=st[0]; for(int i=0;i<7;i++) st[i]=st[i+1]; st[7]=t0;\n"
     "    h=h*131u+st[0]+((unsigned)st[3]<<4); }\n"
     "  return ("+t+")h; }\n",
     {0x55u}, "OptStress50", 2},

    // rodata index table whose entries select stack-array slots.
    {p+"_idxtbl",
     "static const unsigned char P[16]={5,2,9,0,13,7,1,14,"
     "3,11,6,8,15,4,10,12};\n"
     +t+" "+p+"_idxtbl("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned v[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; v[i]=s>>10; }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; unsigned d=P[j];\n"
     "    v[d]=v[d]*3u+v[j]; h=h*131u+v[d]; }\n"
     "  return ("+t+")h; }\n",
     {0x56u}, "OptStress50", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress50TC("x64o50", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress50TC("x86o50", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress50TC("a64o50", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress50TC("armo50", "int");

INSTANTIATE_TEST_SUITE_P(OptStress50, X64OptStress50RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress50, X86OptStress50RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress50, A64OptStress50RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress50, ARM32OptStress50RT, ::testing::ValuesIn(kARM), rtTCName);
