//===- AArch64_IndexExtendRTTests.cpp - extended register memory offsets -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for AArch64 register-offset addressing with an *extended*
// index register: `LDR/STR Wt|Xt, [Xn, Wm, sxtw #s]` / `[Xn, Wm, uxtw #s]`.
//
// The 32-bit signed/unsigned index (a C `int`/`unsigned` array subscript) is
// extended to 64 bits by the SXTW/UXTW operator and then scaled by the LSL
// amount.  capstone 6 surfaces the scaled-extend form with BOTH shift.type==LSL
// (the scale) AND ext==SXTW/UXTW, and the lifter's index handling checked the
// LSL shift *first* — so it read the whole 64-bit X register and shifted it,
// silently dropping the sign/zero extension.  For a NEGATIVE 32-bit index whose
// register high half is zero (the value did not come from a 64-bit op), the
// real `sxtw` makes a negative 64-bit offset while the buggy lift produced a
// huge positive one → wrong address / unmapped access.
//
// The bug was masked because every prior memory-index probe used a non-negative
// index (or a 64-bit `lsl` index), so sxtw vs plain-64-bit read agreed.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64IndexExtendRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64IndexExtendRT, Verify) { roundTripAArch64(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define A64IDX "IndexExtend", 1, "", false, "", -1

// clang-format off

static const std::vector<RoundTripTC> kA64IndexExtend = {

  // --- RED: negative sxtw index (high 32 bits of the index reg are zero, so
  // sign-extension of the low 32 bits is observably different from reading the
  // whole 64-bit register).  idx passed as 0x00000000_FFFFFFFx. ---

  // ldr w, [base, w_idx, sxtw #2]; idx = -2 -> base[-2] = arr[2] = 0x33333333
  {"ldr_w_sxtw_neg2",
   "long ldr_w_sxtw_neg2(long a0, long idx){"
   " volatile unsigned arr[8]={0x11111111u,0x22222222u,0x33333333u,0x44444444u,"
   "0x55555555u,0x66666666u,0x77777777u,0x88888888u};"
   " unsigned *base=(unsigned*)arr+4; unsigned r;"
   " __asm__ volatile(\"ldr %w0,[%1,%w2,sxtw #2]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 0xFFFFFFFEull}, A64IDX},

  // ldr w, [base, w_idx, sxtw #2]; idx = -4 -> base[-4] = arr[0] = 0x11111111
  {"ldr_w_sxtw_neg4",
   "long ldr_w_sxtw_neg4(long a0, long idx){"
   " volatile unsigned arr[8]={0x11111111u,0x22222222u,0x33333333u,0x44444444u,"
   "0x55555555u,0x66666666u,0x77777777u,0x88888888u};"
   " unsigned *base=(unsigned*)arr+4; unsigned r;"
   " __asm__ volatile(\"ldr %w0,[%1,%w2,sxtw #2]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 0xFFFFFFFCull}, A64IDX},

  // ldr x, [base, w_idx, sxtw #3]; idx = -2 -> base[-2] = arr[2]
  {"ldr_x_sxtw_neg",
   "long ldr_x_sxtw_neg(long a0, long idx){"
   " volatile unsigned long arr[8]={0x1111111111111111ul,0x2222222222222222ul,"
   "0x3333333333333333ul,0x4444444444444444ul,0x5555555555555555ul,"
   "0x6666666666666666ul,0x7777777777777777ul,0x8888888888888888ul};"
   " unsigned long *base=(unsigned long*)arr+4; unsigned long r;"
   " __asm__ volatile(\"ldr %0,[%1,%w2,sxtw #3]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)r; }\n",
   {0, 0xFFFFFFFEull}, A64IDX},

  // ldrb w, [base, w_idx, sxtw]; (scale 0) idx = -3 -> base[-3] = arr[1] = 0x22
  {"ldrb_sxtw_neg",
   "long ldrb_sxtw_neg(long a0, long idx){"
   " volatile unsigned char arr[8]={0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};"
   " unsigned char *base=arr+4; unsigned r;"
   " __asm__ volatile(\"ldrb %w0,[%1,%w2,sxtw]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 0xFFFFFFFDull}, A64IDX},

  // ldrh w, [base, w_idx, sxtw #1]; idx = -2 -> base[-2] = arr[2] = 0x3333
  {"ldrh_sxtw_neg",
   "long ldrh_sxtw_neg(long a0, long idx){"
   " volatile unsigned short arr[8]={0x1111,0x2222,0x3333,0x4444,0x5555,0x6666,"
   "0x7777,0x8888};"
   " unsigned short *base=arr+4; unsigned r;"
   " __asm__ volatile(\"ldrh %w0,[%1,%w2,sxtw #1]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 0xFFFFFFFEull}, A64IDX},

  // ldrsw x, [base, w_idx, sxtw #2]; idx = -1 -> base[-1] = arr[3] (sign-extended)
  {"ldrsw_sxtw_neg",
   "long ldrsw_sxtw_neg(long a0, long idx){"
   " volatile int arr[8]={0x11111111,0x22222222,0x33333333,-1,"
   "0x55555555,0x66666666,0x77777777,0x18888888};"
   " int *base=arr+4; long r;"
   " __asm__ volatile(\"ldrsw %0,[%1,%w2,sxtw #2]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return r; }\n",
   {0, 0xFFFFFFFFull}, A64IDX},

  // str w, [base, w_idx, sxtw #2]; idx = -2 stores into arr[2], read back.
  {"str_w_sxtw_neg",
   "long str_w_sxtw_neg(long val, long idx){"
   " volatile unsigned arr[8]={0,0,0,0,0,0,0,0};"
   " unsigned *base=(unsigned*)arr+4;"
   " __asm__ volatile(\"str %w0,[%1,%w2,sxtw #2]\"::\"r\"((unsigned)val),\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)arr[2]; }\n",
   {0xDEADBEEFull, 0xFFFFFFFEull}, A64IDX},

  // strb w, [base, w_idx, sxtw]; idx = -3 stores into arr[1], read back.
  {"strb_sxtw_neg",
   "long strb_sxtw_neg(long val, long idx){"
   " volatile unsigned char arr[8]={0,0,0,0,0,0,0,0};"
   " unsigned char *base=arr+4;"
   " __asm__ volatile(\"strb %w0,[%1,%w2,sxtw]\"::\"r\"((unsigned)val),\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)arr[1]; }\n",
   {0xA5ull, 0xFFFFFFFDull}, A64IDX},

  // --- Controls: should pass with or without the fix ---

  // uxtw with a positive index (high bits already zero, no sign issue).
  {"ldr_w_uxtw_pos_ctl",
   "long ldr_w_uxtw_pos_ctl(long a0, long idx){"
   " volatile unsigned arr[8]={0x11111111u,0x22222222u,0x33333333u,0x44444444u,"
   "0x55555555u,0x66666666u,0x77777777u,0x88888888u};"
   " unsigned *base=(unsigned*)arr; unsigned r;"
   " __asm__ volatile(\"ldr %w0,[%1,%w2,uxtw #2]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 2}, A64IDX},

  // 64-bit lsl index (no extend) — the path that already worked.
  {"ldr_w_lsl_pos_ctl",
   "long ldr_w_lsl_pos_ctl(long a0, long idx){"
   " volatile unsigned arr[8]={0x11111111u,0x22222222u,0x33333333u,0x44444444u,"
   "0x55555555u,0x66666666u,0x77777777u,0x88888888u};"
   " unsigned *base=(unsigned*)arr; unsigned r;"
   " __asm__ volatile(\"ldr %w0,[%1,%2,lsl #2]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 3}, A64IDX},

  // sxtw with a positive index (sext == zext == 64-bit read; works either way).
  {"ldr_w_sxtw_pos_ctl",
   "long ldr_w_sxtw_pos_ctl(long a0, long idx){"
   " volatile unsigned arr[8]={0x11111111u,0x22222222u,0x33333333u,0x44444444u,"
   "0x55555555u,0x66666666u,0x77777777u,0x88888888u};"
   " unsigned *base=(unsigned*)arr; unsigned r;"
   " __asm__ volatile(\"ldr %w0,[%1,%w2,sxtw #2]\":\"=r\"(r):\"r\"(base),\"r\"(idx):\"memory\");"
   " return (long)(unsigned)r; }\n",
   {0, 5}, A64IDX},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(IndexExtend, A64IndexExtendRT,
                         ::testing::ValuesIn(kA64IndexExtend), rtTCName);
