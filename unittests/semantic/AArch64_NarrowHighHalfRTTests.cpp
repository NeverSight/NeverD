//===- AArch64_NarrowHighHalfRTTests.cpp - SQXTN2/UQXTN2/SQXTUN2 ---*- C++ -*=//
//
// Roundtrip probes for the "high-half" saturating-narrow forms SQXTN2/UQXTN2/
// SQXTUN2 (and the XTN2 control).  The base op writes the narrowed lanes to the
// low 64 bits; the "2" form writes them to the UPPER 64 bits and PRESERVES the
// low half (the previous narrow result).  The lift handled only the base form
// (COPY Dst, narrowed) for all three saturating variants, so the "2" form
// clobbered the low half and put its result in the wrong half — a half-vector
// drop the clang auto-vectorizer hits whenever a 16-lane clamp+narrow is split
// into sqxtun + sqxtun2.  Source lanes are derived from the argument at runtime
// (not constant arrays, which clang -O0 would route through a literal pool) and
// chosen to exercise both saturation bounds.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NarrowHighHalfRT : public SemanticRoundTripFixture,
                                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NarrowHighHalfRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kNarrowHighHalf = {
  // SQXTN2: signed saturating narrow 16->8 into the high half (clamp [-128,127]).
  {"sqxtn2_h8b",
   "long sqxtn2_h8b(long a){\n"
   "  short A[8],B[8]; signed char R[16];\n"
   "  for(int i=0;i<8;i++){ A[i]=(short)(a*(i+1)-400); B[i]=(short)(a*3+i*90-300); }\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.8h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.8h},[%2]\\n\\t\"\n"
   "    \"sqxtn v0.8b, v1.8h\\n\\t\"\n"
   "    \"sqxtn2 v0.16b, v2.8h\\n\\t\"\n"
   "    \"st1 {v0.16b},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned h=0; for(int i=0;i<16;i++) h=h*131u+(unsigned char)R[i];\n"
   "  return (long)h;\n"
   "}\n",
   {321}, "NarrowHighHalf", 0, ""},

  // UQXTN2: unsigned saturating narrow 16->8 into the high half (clamp [0,255]).
  {"uqxtn2_h8b",
   "long uqxtn2_h8b(long a){\n"
   "  unsigned short A[8],B[8]; unsigned char R[16];\n"
   "  for(int i=0;i<8;i++){ A[i]=(unsigned short)(a*(i+1)+50); B[i]=(unsigned short)(a*5+i*120); }\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.8h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.8h},[%2]\\n\\t\"\n"
   "    \"uqxtn v0.8b, v1.8h\\n\\t\"\n"
   "    \"uqxtn2 v0.16b, v2.8h\\n\\t\"\n"
   "    \"st1 {v0.16b},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned h=0; for(int i=0;i<16;i++) h=h*131u+R[i];\n"
   "  return (long)h;\n"
   "}\n",
   {777}, "NarrowHighHalf", 0, ""},

  // SQXTUN2: signed->unsigned saturating narrow 16->8 into the high half.
  {"sqxtun2_h8b",
   "long sqxtun2_h8b(long a){\n"
   "  short A[8],B[8]; unsigned char R[16];\n"
   "  for(int i=0;i<8;i++){ A[i]=(short)(a*(i+1)-200); B[i]=(short)(a*3+i*70-150); }\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.8h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.8h},[%2]\\n\\t\"\n"
   "    \"sqxtun v0.8b, v1.8h\\n\\t\"\n"
   "    \"sqxtun2 v0.16b, v2.8h\\n\\t\"\n"
   "    \"st1 {v0.16b},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned h=0; for(int i=0;i<16;i++) h=h*131u+R[i];\n"
   "  return (long)h;\n"
   "}\n",
   {555}, "NarrowHighHalf", 0, ""},

  // SQXTN2 32->16: signed saturating narrow 4S into the high half (4H).
  {"sqxtn2_s4h",
   "long sqxtn2_s4h(long a){\n"
   "  int A[4],B[4]; short R[8];\n"
   "  for(int i=0;i<4;i++){ A[i]=(int)(a*(i+1)-100000); B[i]=(int)(a*7+i*40000); }\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.4s},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.4s},[%2]\\n\\t\"\n"
   "    \"sqxtn v0.4h, v1.4s\\n\\t\"\n"
   "    \"sqxtn2 v0.8h, v2.4s\\n\\t\"\n"
   "    \"st1 {v0.8h},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned h=0; for(int i=0;i<8;i++) h=h*131u+(unsigned short)R[i];\n"
   "  return (long)h;\n"
   "}\n",
   {12345}, "NarrowHighHalf", 0, ""},

  // XTN2 control (no saturation): must stay correct (already handled).
  {"xtn2_h8b_ctrl",
   "long xtn2_h8b_ctrl(long a){\n"
   "  short A[8],B[8]; unsigned char R[16];\n"
   "  for(int i=0;i<8;i++){ A[i]=(short)(a*(i+1)); B[i]=(short)(a*3+i*7); }\n"
   "  __asm__ volatile(\n"
   "    \"ld1 {v1.8h},[%1]\\n\\t\"\n"
   "    \"ld1 {v2.8h},[%2]\\n\\t\"\n"
   "    \"xtn v0.8b, v1.8h\\n\\t\"\n"
   "    \"xtn2 v0.16b, v2.8h\\n\\t\"\n"
   "    \"st1 {v0.16b},[%0]\\n\\t\"\n"
   "    :: \"r\"(R),\"r\"(A),\"r\"(B): \"v0\",\"v1\",\"v2\",\"memory\");\n"
   "  unsigned h=0; for(int i=0;i<16;i++) h=h*131u+R[i];\n"
   "  return (long)h;\n"
   "}\n",
   {888}, "NarrowHighHalf", 0, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(NarrowHighHalf, AArch64NarrowHighHalfRT,
                         ::testing::ValuesIn(kNarrowHighHalf), rtTCName);
