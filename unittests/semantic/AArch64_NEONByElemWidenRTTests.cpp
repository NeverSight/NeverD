//===- AArch64_NEONByElemWidenRTTests.cpp - by-element widen mul -*- C++ -*-=//
//
// Roundtrip probes for AArch64 NEON by-element widening multiplies
// (`smull/umull/sqdmull v.Ts, v.Th, vN.h[idx]`).  MUL/MLA/MLS/FMLA/SMLAL/
// SQDMULH already broadcast the selected lane; the widening NON-accumulate
// SMULL/UMULL and SMULL2/UMULL2/SQDMULL/SQDMULL2 handlers did not — they walked
// B[0..N] per lane while operandRead returns just the selected element, so every
// lane but lane 0 multiplied by an out-of-range (zero) read.
//
// clang lowers `vmull_lane_*` to `dup`+`smull` (vector form), so the by-element
// encoding is driven directly via inline asm.  Each kernel loads two known
// vectors, runs the by-element op against a NON-zero lane with distinct B lanes
// (so the buggy per-lane walk diverges on lane 0 too), stores the result, and
// hash-reduces it.  Intrinsic controls confirm the vector form stays green.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64ByElemWidenRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64ByElemWidenRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kByElemWiden = {
  // --- control: vector-form widening multiply (must stay green) ---
  {"vmull_lane_s16_intrin",
   "#include <arm_neon.h>\n"
   "long be_vmull_lane_s16i(long a){\n"
   "  short x=(short)a;\n"
   "  int16x4_t va={x,(short)-x,1000,-1000};\n"
   "  int16x4_t vb={3,7,11,13};\n"
   "  int32x4_t r=vmull_lane_s16(va,vb,1);\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^(unsigned)r[i];\n"
   "  return (long)o;\n"
   "}\n",
   {321}, "ByElemWiden", 1, ""},

  // --- SMULL by-element: v0.4s = v1.4h * v2.h[1] (lane 1 broadcast) ---
  {"smull_h_byelem",
   "long be_smull_h(long a){\n"
   "  short A[4]={(short)a,(short)-a,1000,-1000};\n"
   "  short B[4]={3,7,11,13};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.4h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.4h},[%2]\\n\\t\"\n"
   "    \"smull v0.4s, v1.4h, v2.h[1]\\n\\t\"\n"
   "    \"st1 {v0.4s},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (long)o;\n"
   "}\n",
   {321}, "ByElemWiden", 0, ""},

  // --- UMULL by-element: v0.4s = v1.4h * v2.h[2] (unsigned) ---
  {"umull_h_byelem",
   "long be_umull_h(long a){\n"
   "  unsigned short A[4]={(unsigned short)a,(unsigned short)(a+1),60000,40000};\n"
   "  unsigned short B[4]={3,7,11,13};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.4h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.4h},[%2]\\n\\t\"\n"
   "    \"umull v0.4s, v1.4h, v2.h[2]\\n\\t\"\n"
   "    \"st1 {v0.4s},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (long)o;\n"
   "}\n",
   {777}, "ByElemWiden", 0, ""},

  // --- SMULL by-element s32->s64: v0.2d = v1.2s * v2.s[1] ---
  {"smull_s_byelem",
   "long be_smull_s(long a){\n"
   "  int A[2]={(int)a,(int)-a};\n"
   "  int B[2]={7,13};\n"
   "  unsigned long long R[2];\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.2s},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.2s},[%2]\\n\\t\"\n"
   "    \"smull v0.2d, v1.2s, v2.s[1]\\n\\t\"\n"
   "    \"st1 {v0.2d},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<2;i++) o=o*131u^(unsigned)(R[i]^(R[i]>>32));\n"
   "  return (long)o;\n"
   "}\n",
   {54321}, "ByElemWiden", 0, ""},

  // --- SMULL2 by-element: upper-half source lanes ---
  {"smull2_h_byelem",
   "long be_smull2_h(long a){\n"
   "  short A[8]={1,2,3,4,(short)a,(short)-a,1000,-1000};\n"
   "  short B[4]={3,7,11,13};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.8h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.4h},[%2]\\n\\t\"\n"
   "    \"smull2 v0.4s, v1.8h, v2.h[3]\\n\\t\"\n"
   "    \"st1 {v0.4s},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (long)o;\n"
   "}\n",
   {888}, "ByElemWiden", 0, ""},

  // --- SQDMULL by-element: saturating doubling widening (lane 1 = INT16_MIN) ---
  {"sqdmull_h_byelem",
   "long be_sqdmull_h(long a){\n"
   "  short A[4]={(short)a,(short)-a,32767,-32768};\n"
   "  short B[4]={3,-32768,11,13};\n"
   "  unsigned R[4];\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.4h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.4h},[%2]\\n\\t\"\n"
   "    \"sqdmull v0.4s, v1.4h, v2.h[1]\\n\\t\"\n"
   "    \"st1 {v0.4s},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned o=0; for(int i=0;i<4;i++) o=o*131u^R[i];\n"
   "  return (long)o;\n"
   "}\n",
   {300}, "ByElemWiden", 0, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ByElemWiden, AArch64ByElemWidenRT,
                         ::testing::ValuesIn(kByElemWiden), rtTCName);
