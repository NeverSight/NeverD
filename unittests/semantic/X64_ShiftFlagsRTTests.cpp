//===- X64_ShiftFlagsRTTests.cpp - x86 shift OF/CF flag probes --*- C++ -*-===//
//
// The roundtrip harness only compares return values, so the flag side effects
// of SHL/SHR/SAR are invisible unless folded into the result.  These probes
// drive 1-bit shifts (where OF is architecturally defined) and fold OF/CF into
// the return value via seto/setc, exposing any gap in NeverD's shift-flag
// modelling.  Intel SDM: for a 1-bit shift, SHL sets OF = MSB(result) ^ CF,
// SHR sets OF = MSB(source), SAR clears OF; for count 0 the flags are
// unchanged; for count > 1 OF is undefined (not probed).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ShiftFlagRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ShiftFlagRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== SHL 1-bit OF = MSB(result) ^ CF (top two source bits differ). =====
  // 0x40000000: bit31=0,bit30=1 differ -> OF=1 (result 0x80000000, CF=0).
  {"shl1_of_set",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"shll $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x40000000ULL}, "ShiftFlag"},
  // 0x80000000: bit31=1,bit30=0 differ -> OF=1 (result 0, CF=1).
  {"shl1_of_set_b",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"shll $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x80000000ULL}, "ShiftFlag"},
  // 0x10000000: bit31=0,bit30=0 same -> OF=0 (control).
  {"shl1_of_clear",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"shll $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x10000000ULL}, "ShiftFlag"},
  // 0xC0000000: bit31=1,bit30=1 same -> OF=0 (result 0x80000000, CF=1) control.
  {"shl1_of_clear_b",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"shll $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0xC0000000ULL}, "ShiftFlag"},
  // 64-bit form.
  {"shlq1_of_set",
   "long f(long a){unsigned long x=(unsigned long)a;unsigned char of;"
   "__asm__ volatile(\"shlq $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return (of?2:0);}\n",
   {0x4000000000000000ULL}, "ShiftFlag"},

  // ===== SHR 1-bit OF = MSB(source). =====
  {"shr1_of_set",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"shrl $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x80000000ULL}, "ShiftFlag"},
  {"shr1_of_clear",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"shrl $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x40000000ULL}, "ShiftFlag"},

  // ===== SAR 1-bit clears OF.  Pre-set OF=1 via an overflowing add so the bug
  // (SAR not touching OF) leaves OF=1 and diverges from the cleared truth. =====
  {"sar1_of_clear",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"movl $0x7fffffff,%%ecx\\n\\tincl %%ecx\\n\\t"
   "sarl $1,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\",\"ecx\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x80000000ULL}, "ShiftFlag"},

  // ===== CF controls (already modelled; guard against regressions). =====
  {"shl1_cf",
   "long f(long a){unsigned x=(unsigned)a;unsigned char cf;"
   "__asm__ volatile(\"shll $1,%0\\n\\tsetc %1\":\"+r\"(x),\"=r\"(cf)::\"cc\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x80000000ULL}, "ShiftFlag"},
  {"shr1_cf",
   "long f(long a){unsigned x=(unsigned)a;unsigned char cf;"
   "__asm__ volatile(\"shrl $1,%0\\n\\tsetc %1\":\"+r\"(x),\"=r\"(cf)::\"cc\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x00000001ULL}, "ShiftFlag"},

  // ===== Count 0: x86 leaves ALL flags unchanged.  A %cl == 0 shift/rotate
  // must preserve the entry flag; `movl $0,%ecx` zeroes the count without
  // touching flags so the pre-set flag is the one observed afterwards. =====
  {"shl_cl0_cf_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char cf;"
   "__asm__ volatile(\"stc\\n\\tmovl $0,%%ecx\\n\\tshll %%cl,%0\\n\\tsetc %1\""
   ":\"+r\"(x),\"=r\"(cf)::\"cc\",\"ecx\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x00000002ULL}, "ShiftFlag"},
  {"shl_cl0_zf_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char zf;"
   "__asm__ volatile(\"xorl %%edx,%%edx\\n\\tmovl $0,%%ecx\\n\\t"
   "shll %%cl,%0\\n\\tsetz %1\":\"+r\"(x),\"=r\"(zf)::\"cc\",\"ecx\",\"edx\");"
   "return ((unsigned long)zf<<32)|x;}\n",
   {0x00000005ULL}, "ShiftFlag"},
  {"shl_cl0_sf_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char sf;"
   "__asm__ volatile(\"movl $-1,%%edx\\n\\ttestl %%edx,%%edx\\n\\tmovl $0,%%ecx"
   "\\n\\tshll %%cl,%0\\n\\tsets %1\":\"+r\"(x),\"=r\"(sf)::\"cc\",\"ecx\",\"edx\");"
   "return ((unsigned long)sf<<32)|x;}\n",
   {0x00000005ULL}, "ShiftFlag"},
  {"shr_cl0_cf_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char cf;"
   "__asm__ volatile(\"stc\\n\\tmovl $0,%%ecx\\n\\tshrl %%cl,%0\\n\\tsetc %1\""
   ":\"+r\"(x),\"=r\"(cf)::\"cc\",\"ecx\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x00000002ULL}, "ShiftFlag"},
  {"rol_cl0_cf_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char cf;"
   "__asm__ volatile(\"stc\\n\\tmovl $0,%%ecx\\n\\troll %%cl,%0\\n\\tsetc %1\""
   ":\"+r\"(x),\"=r\"(cf)::\"cc\",\"ecx\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x00000002ULL}, "ShiftFlag"},
  {"ror_cl0_cf_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char cf;"
   "__asm__ volatile(\"stc\\n\\tmovl $0,%%ecx\\n\\trorl %%cl,%0\\n\\tsetc %1\""
   ":\"+r\"(x),\"=r\"(cf)::\"cc\",\"ecx\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x00000002ULL}, "ShiftFlag"},
  // OF preserved on a zero count (already handled by the count==1 OF guard).
  {"shl_cl0_of_keep",
   "long f(long a){unsigned x=(unsigned)a;unsigned char of;"
   "__asm__ volatile(\"movl $0x7fffffff,%%edx\\n\\tincl %%edx\\n\\tmovl $0,%%ecx"
   "\\n\\tshll %%cl,%0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\",\"ecx\",\"edx\");"
   "return ((unsigned long)of<<32)|x;}\n",
   {0x00000005ULL}, "ShiftFlag"},
  // Nonzero %cl control: the count==0 guard must not disturb a real shift.
  {"shl_cl4_value",
   "long f(long a){unsigned x=(unsigned)a;"
   "__asm__ volatile(\"movl $4,%%ecx\\n\\tshll %%cl,%0\":\"+r\"(x)::\"cc\",\"ecx\");"
   "return x;}\n",
   {0x12345678ULL}, "ShiftFlag"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftFlag, X64ShiftFlagRT, ::testing::ValuesIn(kX64),
                         rtTCName);
