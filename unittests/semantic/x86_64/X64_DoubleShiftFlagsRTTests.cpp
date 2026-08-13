//===- X64_DoubleShiftFlagsRTTests.cpp - SHLD/SHRD flags -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 double-precision shifts SHLD/SHRD follow the same flag rules as the
// single shifts that #312 fixed, but were not covered there: a zero (post-mask)
// count leaves ALL flags unchanged, and the OF flag is defined only for a 1-bit
// shift (set on a sign change of the result).  The lift unconditionally wrote
// CF / ZF / SF / PF and never wrote OF, so these probes fold the post-op flags
// into the return value with setcc to expose both gaps.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DoubleShiftFlagsRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DoubleShiftFlagsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // count==0: CF must be preserved (stc sets it; shld cl=0 must keep CF=1).
  {"shld_cnt0_keep_cf",
   "long f(long a){unsigned r;int d=(int)a;"
   "__asm__ volatile(\"stc\\n\\tshldl %%cl,%2,%1\\n\\tsetc %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(0x12345678),\"c\"(0):\"cc\");"
   "return (long)(unsigned char)r;}\n",
   {0xF0F0F0F0ULL}, "DblShift"},
  // count==0: ZF must be preserved (a prior cmp sets ZF=1; shld keeps it).
  {"shld_cnt0_keep_zf",
   "long f(long a){unsigned r;int d=(int)a;"
   "__asm__ volatile(\"xorl %%eax,%%eax\\n\\tcmpl %%eax,%%eax\\n\\t"
   "shldl %%cl,%2,%1\\n\\tsetz %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(0x12345678),\"c\"(0):\"cc\",\"eax\");"
   "return (long)(unsigned char)r;}\n",
   {0xF0F0F0F0ULL}, "DblShift"},
  // shrd count==0: CF preserved.
  {"shrd_cnt0_keep_cf",
   "long f(long a){unsigned r;int d=(int)a;"
   "__asm__ volatile(\"stc\\n\\tshrdl %%cl,%2,%1\\n\\tsetc %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(0x12345678),\"c\"(0):\"cc\");"
   "return (long)(unsigned char)r;}\n",
   {0xF0F0F0F0ULL}, "DblShift"},
  // shld $1 sets OF on a sign change: 0x40000000 << 1 -> 0x80000000 (bit31 flips).
  {"shld_1_of_set",
   "long f(long a){unsigned r;int d=0x40000000;"
   "__asm__ volatile(\"shldl $1,%2,%1\\n\\tseto %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(0):\"cc\");"
   "return (long)(unsigned char)r;}\n",
   {0}, "DblShift"},
  // shld $1 clears OF when no sign change: 0x10000000 << 1 -> 0x20000000.
  {"shld_1_of_clear",
   "long f(long a){unsigned r;int d=0x10000000;"
   "__asm__ volatile(\"shldl $1,%2,%1\\n\\tseto %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(0):\"cc\");"
   "return (long)(unsigned char)r;}\n",
   {0}, "DblShift"},
  // shrd $1 OF = MSB change of result vs source-of-MSB.
  {"shrd_1_of_set",
   "long f(long a){unsigned r;int d=1;"
   "__asm__ volatile(\"shrdl $1,%2,%1\\n\\tseto %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(1):\"cc\");"
   "return (long)(unsigned char)r;}\n",
   {0}, "DblShift"},
  // Control: nonzero count CF is the last bit shifted out (already correct).
  {"shld_cnt_cf_ctrl",
   "long f(long a){unsigned r;int d=(int)a;"
   "__asm__ volatile(\"shldl $4,%2,%1\\n\\tsetc %b0\""
   ":\"=&q\"(r),\"+r\"(d):\"r\"(0):\"cc\");"
   "return ((long)(unsigned)d<<1)|(unsigned char)r;}\n",
   {0x08000000ULL}, "DblShift"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(DblShift, X64DoubleShiftFlagsRT,
                         ::testing::ValuesIn(kX64), rtTCName);
