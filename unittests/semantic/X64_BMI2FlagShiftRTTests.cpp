//===- X64_BMI2FlagShiftRTTests.cpp - BMI2 flag-less shifts -------*- C++ -*-=//
//
// x86 BMI2 `RORX`/`SARX`/`SHLX`/`SHRX` compute a rotate/shift WITHOUT touching
// EFLAGS (unlike the legacy `ROR`/`SAR`/`SHL`/`SHR`, which set CF/OF/SF/ZF/PF).
// The lifter emits the value op with NO flag updates, so a CF set beforehand
// must survive the op.  Existing coverage (X64_AutoRoundTrip / IntegerSemantic)
// only ever consumes the shift RESULT, never checks that the flags are left
// intact — a classic weak-test gap.  Each probe sets CF=1 with `stc`, runs the
// BMI2 op, then `setc` reads CF back: a lifter that wrongly routed these through
// the flag-setting legacy shift path would clear CF (RED).  The folded return
// also carries the shifted value, so the shift/rotate semantics are checked too.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BMI2FlagShiftRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BMI2FlagShiftRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // SHRX (logical right), 64-bit: CF preserved across the op.
  {"shrx_q_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, cnt=5, dst, cf;\n"
   "  __asm__ volatile(\"stc\\n\\tshrxq %3,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src),\"r\"(cnt):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x123456789ABCDEF0ULL}, "BMI2FlagShift", 1, "-mbmi2"},

  // SHLX (logical left), 64-bit.
  {"shlx_q_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, cnt=7, dst, cf;\n"
   "  __asm__ volatile(\"stc\\n\\tshlxq %3,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src),\"r\"(cnt):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x0F1E2D3C4B5A6978ULL}, "BMI2FlagShift", 1, "-mbmi2"},

  // SARX (arithmetic right), 64-bit, negative input keeps sign.
  {"sarx_q_cf",
   "long f(long a){\n"
   "  long src=a, dst; unsigned long cf; long cnt=4;\n"
   "  __asm__ volatile(\"stc\\n\\tsarxq %3,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src),\"r\"(cnt):\"cc\");\n"
   "  return dst*4+(long)(cf&1);}\n",
   {0xFFFFFFFFFFFFFF00ULL}, "BMI2FlagShift", 1, "-mbmi2"},

  // RORX (rotate right, immediate count), 64-bit.
  {"rorx_q_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"stc\\n\\trorxq $13,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x123456789ABCDEF0ULL}, "BMI2FlagShift", 1, "-mbmi2"},

  // 32-bit forms exercise the 5-bit count mask + zero-extension.
  {"shrx_d_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned src=(unsigned)a, cnt=3, dst; unsigned long cf;\n"
   "  __asm__ volatile(\"stc\\n\\tshrxl %3,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src),\"r\"(cnt):\"cc\");\n"
   "  return (unsigned long)dst*4+(cf&1);}\n",
   {0x00000000DEADBEEFULL}, "BMI2FlagShift", 1, "-mbmi2"},

  {"rorx_d_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned src=(unsigned)a, dst; unsigned long cf;\n"
   "  __asm__ volatile(\"stc\\n\\trorxl $11,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return (unsigned long)dst*4+(cf&1);}\n",
   {0x00000000CAFEF00DULL}, "BMI2FlagShift", 1, "-mbmi2"},

  // Control: CF cleared beforehand stays cleared (guards an over-eager "always
  // set CF" mis-lift).
  {"shrx_q_nocf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, cnt=9, dst, cf;\n"
   "  __asm__ volatile(\"clc\\n\\tshrxq %3,%2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src),\"r\"(cnt):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x123456789ABCDEF0ULL}, "BMI2FlagShift", 1, "-mbmi2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BMI2FlagShift, X64BMI2FlagShiftRT,
                         ::testing::ValuesIn(kX64), rtTCName);
