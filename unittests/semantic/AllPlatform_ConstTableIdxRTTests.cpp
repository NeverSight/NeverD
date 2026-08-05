//===- AllPlatform_ConstTableIdxRTTests.cpp - rodata table walks --*-C++*-=//
//
// Roundtrip probes for runtime-indexed read-only (`.rodata`) constant tables in
// *rolled* loops — the addressing shape that surfaced #411 (ARM32 keeps the
// table base in a loop-carried induction pointer derived from a PC-relative
// literal pool, advanced by a runtime offset; the emitter must map every access
// back to the embedded global instead of dereferencing a stale original-VA
// literal).  Each kernel forces clang -O2 to keep the table in .rodata and index
// it with a runtime value across a trip count too large to unroll: a 1-D word
// table, a 2-D table with runtime row/col (`base + i*stride + j`), a struct
// table (`tab[i].field`, element stride + field displacement), a reverse /
// negative-stride walk, a byte sbox (sub-word loads), two tables cross-indexed
// in one function, and a nested loop whose inner index is a runtime product.
// All integer (no ARM32 soft-float libcall), each folds into a value-dependent
// hash, compared native vs lifted at -O2 across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CTblRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CTblRT, Verify) { roundTripX64(GetParam()); }
class X86CTblRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CTblRT, Verify) { roundTripX86(GetParam()); }
class A64CTblRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CTblRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CTblRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CTblRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCTblTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 1-D word table indexed by a runtime hash across 128 iterations (rolled).
    {p+"_t1d",
     "static const unsigned "+p+"_T1[16]={0x9E3779B9u,0x85EBCA6Bu,0xC2B2AE35u,\n"
     "  0x27D4EB2Fu,0x165667B1u,0xD3A2646Cu,0xFD7046C5u,0xB55A4F09u,1u,2u,3u,5u,\n"
     "  8u,13u,21u,34u};\n"
     +t+" "+p+"_t1d("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u,h=0;\n"
     "  for(int i=0;i<128;i++){ unsigned idx=(x>>3)&15u;\n"
     "    h=h*131u+"+p+"_T1[idx]+idx;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x41ULL}, "CTbl", 2},

    // 2-D table: runtime row and column => base + (i*cols + j)*4.
    {p+"_t2d",
     "static const unsigned "+p+"_T2[6][5]={\n"
     "  {11u,22u,33u,44u,55u},{66u,77u,88u,99u,111u},{121u,131u,141u,151u,161u},\n"
     "  {2u,4u,8u,16u,32u},{3u,9u,27u,81u,243u},{5u,25u,125u,625u,3125u}};\n"
     +t+" "+p+"_t2d("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u,h=0;\n"
     "  for(int k=0;k<120;k++){ unsigned i=(x>>4)%6u, j=(x>>9)%5u;\n"
     "    h=h*131u+"+p+"_T2[i][j]+i*7u+j;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x53ULL}, "CTbl", 2},

    // Struct table: element stride 8 + field displacement 0/4.
    {p+"_tstruct",
     "struct "+p+"S{unsigned a,b;};\n"
     "static const struct "+p+"S "+p+"_TS[12]={\n"
     "  {1u,2u},{3u,4u},{5u,6u},{7u,8u},{9u,10u},{11u,12u},{13u,14u},{15u,16u},\n"
     "  {17u,18u},{19u,20u},{21u,22u},{23u,24u}};\n"
     +t+" "+p+"_tstruct("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u,h=0;\n"
     "  for(int k=0;k<120;k++){ unsigned i=(x>>5)%12u;\n"
     "    h=h*131u+"+p+"_TS[i].a*7u+"+p+"_TS[i].b+i;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x67ULL}, "CTbl", 2},

    // Reverse / descending walk: index decreases, exercising a negative stride.
    {p+"_trev",
     "static const unsigned "+p+"_TR[16]={100u,101u,102u,103u,104u,105u,106u,\n"
     "  107u,108u,109u,110u,111u,112u,113u,114u,115u};\n"
     +t+" "+p+"_trev("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u,h=0;\n"
     "  for(int k=0;k<128;k++){ unsigned idx=15u-((x>>2)&15u);\n"
     "    h=h*131u+"+p+"_TR[idx]+idx;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x71ULL}, "CTbl", 2},

    // Byte sbox: sub-word (8-bit) loads zero-extended, indexed at runtime.
    {p+"_tbyte",
     "static const unsigned char "+p+"_TB[32]={\n"
     "  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,\n"
     "  0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,\n"
     "  0x9c,0xa4,0x72,0xc0};\n"
     +t+" "+p+"_tbyte("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u,h=0;\n"
     "  for(int i=0;i<160;i++){ unsigned idx=(x>>1)&31u;\n"
     "    h=h*131u+(unsigned)"+p+"_TB[idx]+idx;\n"
     "    x=(x*1664525u+1013904223u)^h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x29ULL}, "CTbl", 2},

    // Two distinct tables cross-indexed in one function.
    {p+"_t2tab",
     "static const unsigned "+p+"_TA[8]={3u,1u,4u,1u,5u,9u,2u,6u};\n"
     "static const unsigned "+p+"_TC[8]={2u,7u,1u,8u,2u,8u,1u,8u};\n"
     +t+" "+p+"_t2tab("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u,h=0;\n"
     "  for(int i=0;i<128;i++){ unsigned ia=(x>>3)&7u, ic=(x>>11)&7u;\n"
     "    h=h*131u+"+p+"_TA[ia]*5u+"+p+"_TC[ic]+ia+ic;\n"
     "    x=(x*1664525u+1013904223u)^("+p+"_TA[ic]^"+p+"_TC[ia]); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9CULL}, "CTbl", 2},

    // Nested loops whose inner index is a runtime product (i*j) into the table.
    {p+"_tnest",
     "static const unsigned "+p+"_TN[20]={0u,1u,1u,2u,3u,5u,8u,13u,21u,34u,55u,\n"
     "  89u,144u,233u,377u,610u,987u,1597u,2584u,4181u};\n"
     +t+" "+p+"_tnest("+t+" a){\n"
     "  unsigned h=(unsigned)a|1u;\n"
     "  for(int i=1;i<12;i++) for(int j=1;j<12;j++){\n"
     "    unsigned idx=((unsigned)(i*j)+(h>>4))%20u;\n"
     "    h=h*131u+"+p+"_TN[idx]+(unsigned)(i+j); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABULL}, "CTbl", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCTblTC("x64ctbl", "long");
static const std::vector<RoundTripTC> kX86 = makeCTblTC("x86ctbl", "int");
static const std::vector<RoundTripTC> kA64 = makeCTblTC("a64ctbl", "long");
static const std::vector<RoundTripTC> kARM = makeCTblTC("armctbl", "int");

INSTANTIATE_TEST_SUITE_P(CTbl, X64CTblRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CTbl, X86CTblRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CTbl, A64CTblRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CTbl, ARM32CTblRT, ::testing::ValuesIn(kARM), rtTCName);
