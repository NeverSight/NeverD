//===- AllPlatform_RareInsnRTTests.cpp - tricky/rare instructions ---*-C++-*-=//
//
// #397 high-yield probing of instructions clang rarely emits from plain C, so
// they are exercised with inline asm: x86 BMI2 PEXT/PDEP (parallel bit gather/
// scatter), x86 PSADBW/PMADDWD, AArch64 TBL/TBX (vector table lookup) and the
// cross-lane reductions ADDV/UADDLV, ARM32 VTBL/VTBX.  Results are folded into a
// scalar so any lift divergence flips the return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RareRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RareRT, Verify) { roundTripX64(GetParam()); }
class A64RareRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RareRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32RareRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RareRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // PEXT: parallel bit extract under a mask.
  {"x64_pext",
   "long x64_pext(long a){ unsigned long r,m=0xF0F0F0F0F0F0F0F0UL;\n"
   "  __asm__(\"pext %2,%1,%0\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"(m));\n"
   "  return (long)r; }\n",
   {0x0123456789ABCDEFULL}, "Rare", 0, "-mbmi2"},
  // PDEP: parallel bit deposit under a mask.
  {"x64_pdep",
   "long x64_pdep(long a){ unsigned long r,m=0x5555555555555555UL;\n"
   "  __asm__(\"pdep %2,%1,%0\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"(m));\n"
   "  return (long)r; }\n",
   {0xDEADBEEFCAFEF00DULL}, "Rare", 0, "-mbmi2"},
  // PSADBW: sum of absolute differences of 8 byte pairs.
  {"x64_psadbw",
   "long x64_psadbw(long a){ unsigned char x[16],y[16]; unsigned long lo;\n"
   "  for(int i=0;i<16;i++){x[i]=(unsigned char)(a+i*7);y[i]=(unsigned char)(a*3-i*5);}\n"
   "  __asm__(\"movdqu (%1),%%xmm0\\n\\tmovdqu (%2),%%xmm1\\n\\tpsadbw %%xmm1,%%xmm0\\n\\tmovq %%xmm0,%0\"\n"
   "    :\"=r\"(lo):\"r\"(x),\"r\"(y):\"xmm0\",\"xmm1\");\n"
   "  return (long)lo; }\n",
   {0x42ULL}, "Rare", 0, "-msse2"},
  // PMADDWD: multiply 16-bit pairs and add adjacent into 32-bit.
  {"x64_pmaddwd",
   "long x64_pmaddwd(long a){ short x[8],y[8]; unsigned long lo;\n"
   "  for(int i=0;i<8;i++){x[i]=(short)(a+i*111);y[i]=(short)(a*2-i*97);}\n"
   "  __asm__(\"movdqu (%1),%%xmm0\\n\\tmovdqu (%2),%%xmm1\\n\\tpmaddwd %%xmm1,%%xmm0\\n\\tmovq %%xmm0,%0\"\n"
   "    :\"=r\"(lo):\"r\"(x),\"r\"(y):\"xmm0\",\"xmm1\");\n"
   "  return (long)lo; }\n",
   {0x7ULL}, "Rare", 0, "-msse2"},
};

static const std::vector<RoundTripTC> kA64 = {
  // TBL: 16-byte table lookup with out-of-range -> 0.
  {"a64_tbl",
   "long a64_tbl(long a){ unsigned char tbl[16],idx[16],out[16];\n"
   "  for(int i=0;i<16;i++){tbl[i]=(unsigned char)(a+i*7);idx[i]=(unsigned char)((a+i*3)&0x1f);}\n"
   "  __asm__(\"ld1 {v1.16b},[%1]\\n\\tld1 {v2.16b},[%2]\\n\\ttbl v0.16b,{v1.16b},v2.16b\\n\\tst1 {v0.16b},[%0]\"\n"
   "    ::\"r\"(out),\"r\"(tbl),\"r\"(idx):\"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  long h=0; for(int i=0;i<16;i++) h=h*31+out[i]; return h; }\n",
   {0x33ULL}, "Rare", 0},
  // TBX: like TBL but out-of-range keeps the destination byte.
  {"a64_tbx",
   "long a64_tbx(long a){ unsigned char tbl[16],idx[16],out[16];\n"
   "  for(int i=0;i<16;i++){tbl[i]=(unsigned char)(a+i*5);idx[i]=(unsigned char)((a+i*3)&0x1f);out[i]=(unsigned char)(0xA0+i);}\n"
   "  __asm__(\"ld1 {v0.16b},[%0]\\n\\tld1 {v1.16b},[%1]\\n\\tld1 {v2.16b},[%2]\\n\\ttbx v0.16b,{v1.16b},v2.16b\\n\\tst1 {v0.16b},[%0]\"\n"
   "    ::\"r\"(out),\"r\"(tbl),\"r\"(idx):\"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  long h=0; for(int i=0;i<16;i++) h=h*31+out[i]; return h; }\n",
   {0x44ULL}, "Rare", 0},
  // ADDV: full-vector horizontal add reduce.
  {"a64_addv",
   "long a64_addv(long a){ unsigned x[4],r;\n"
   "  for(int i=0;i<4;i++) x[i]=(unsigned)(a+i*0x9E3779B1u);\n"
   "  __asm__(\"ld1 {v0.4s},[%1]\\n\\taddv s1,v0.4s\\n\\tfmov %w0,s1\":\"=r\"(r):\"r\"(x):\"v0\",\"v1\");\n"
   "  return (long)(unsigned long)r; }\n",
   {0x9ULL}, "Rare", 0},
  // UADDLV: widening horizontal add of 16 bytes -> halfword.
  {"a64_uaddlv",
   "long a64_uaddlv(long a){ unsigned char x[16]; unsigned r;\n"
   "  for(int i=0;i<16;i++) x[i]=(unsigned char)(a+i*13);\n"
   "  __asm__(\"ld1 {v0.16b},[%1]\\n\\tuaddlv h1,v0.16b\\n\\tfmov %w0,s1\":\"=r\"(r):\"r\"(x):\"v0\",\"v1\");\n"
   "  return (long)(unsigned long)(r&0xFFFF); }\n",
   {0x5ULL}, "Rare", 0},
};

static const std::vector<RoundTripTC> kARM = {
  // VTBL.8: single-register table lookup.
  {"arm_vtbl",
   "int arm_vtbl(int a){ unsigned char tbl[8],idx[8],out[8];\n"
   "  for(int i=0;i<8;i++){tbl[i]=(unsigned char)(a+i*7);idx[i]=(unsigned char)((a+i*3)&0xf);}\n"
   "  __asm__(\"vld1.8 {d1},[%1]\\n\\tvld1.8 {d2},[%2]\\n\\tvtbl.8 d0,{d1},d2\\n\\tvst1.8 {d0},[%0]\"\n"
   "    ::\"r\"(out),\"r\"(tbl),\"r\"(idx):\"d0\",\"d1\",\"d2\",\"memory\");\n"
   "  int h=0; for(int i=0;i<8;i++) h=h*31+out[i]; return h; }\n",
   {0x33ULL}, "Rare", 0},
  // VTBX.8: table lookup keeping destination on out-of-range index.
  {"arm_vtbx",
   "int arm_vtbx(int a){ unsigned char tbl[8],idx[8],out[8];\n"
   "  for(int i=0;i<8;i++){tbl[i]=(unsigned char)(a+i*5);idx[i]=(unsigned char)((a+i*3)&0xf);out[i]=(unsigned char)(0xB0+i);}\n"
   "  __asm__(\"vld1.8 {d0},[%0]\\n\\tvld1.8 {d1},[%1]\\n\\tvld1.8 {d2},[%2]\\n\\tvtbx.8 d0,{d1},d2\\n\\tvst1.8 {d0},[%0]\"\n"
   "    ::\"r\"(out),\"r\"(tbl),\"r\"(idx):\"d0\",\"d1\",\"d2\",\"memory\");\n"
   "  int h=0; for(int i=0;i<8;i++) h=h*31+out[i]; return h; }\n",
   {0x44ULL}, "Rare", 0},
  // VPADD: pairwise add reduce.
  {"arm_vpadd",
   "int arm_vpadd(int a){ unsigned x[2],r;\n"
   "  x[0]=(unsigned)a; x[1]=(unsigned)(a*2654435761u);\n"
   "  __asm__(\"vld1.32 {d0},[%1]\\n\\tvpadd.i32 d1,d0,d0\\n\\tvmov %0,s2\":\"=r\"(r):\"r\"(x):\"d0\",\"d1\");\n"
   "  return (int)r; }\n",
   {0x9ULL}, "Rare", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Rare, X64RareRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rare, A64RareRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Rare, ARM32RareRT, ::testing::ValuesIn(kARM), rtTCName);
