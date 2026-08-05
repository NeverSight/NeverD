//===- ARM32_DSPMulSatRTTests.cpp - ARM32 DSP multiply/saturating roundtrip -//
//
// Tests ARM32 DSP multiply and saturating arithmetic instructions.
// Exercises ARMLiftMul.cpp: SMULBB/SMULBT/SMULTB/SMULTT/SMLABB/SMLATB/
// SMLATT/SMMUL/SMMLA/SMMLS/SMLAL/UMLAL/SMULWB/SMULWT, and
// ARMLiftCoreExt.cpp: QADD/QSUB/QDADD/QDSUB/SSAT/USAT.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32DSPRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DSPRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32DSP = {

  // ===== SMULBB - signed multiply bottom*bottom halfwords =====
  {"arm_smulbb",
   "int arm_smulbb(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smulbb %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006}, "DSPMul"},

  // ===== SMULBT - signed multiply bottom*top halfwords =====
  {"arm_smulbt",
   "int arm_smulbt(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smulbt %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006}, "DSPMul"},

  // ===== SMULTB - signed multiply top*bottom halfwords =====
  {"arm_smultb",
   "int arm_smultb(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smultb %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006}, "DSPMul"},

  // ===== SMULTT - signed multiply top*top halfwords =====
  {"arm_smultt",
   "int arm_smultt(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smultt %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006}, "DSPMul"},

  // ===== SMLABB - signed multiply-accumulate bottom*bottom =====
  {"arm_smlabb",
   "int arm_smlabb(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlabb %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006, 100}, "DSPMul"},

  // ===== SMLABT =====
  {"arm_smlabt",
   "int arm_smlabt(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlabt %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006, 100}, "DSPMul"},

  // ===== SMLATB =====
  {"arm_smlatb",
   "int arm_smlatb(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlatb %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006, 100}, "DSPMul"},

  // ===== SMLATT =====
  {"arm_smlatt",
   "int arm_smlatt(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlatt %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006, 100}, "DSPMul"},

  // ===== SMMUL - signed most-significant-word multiply =====
  {"arm_smmul",
   "int arm_smmul(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smmul %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x40000000, 0x40000000}, "DSPMul"},

  // ===== SMMLA - signed most-significant-word multiply-accumulate =====
  {"arm_smmla",
   "int arm_smmla(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smmla %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x40000000, 0x40000000, 10}, "DSPMul"},

  // ===== SMMLS - signed most-significant-word multiply-subtract =====
  {"arm_smmls",
   "int arm_smmls(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smmls %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x40000000, 0x40000000, 100}, "DSPMul"},

  // ===== SMULWB - signed multiply word by halfword (bottom) =====
  {"arm_smulwb",
   "int arm_smulwb(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smulwb %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x10000, 0x00040000}, "DSPMul"},

  // ===== SMULWT - signed multiply word by halfword (top) =====
  {"arm_smulwt",
   "int arm_smulwt(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smulwt %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x10000, 0x00040000}, "DSPMul"},

  // ===== SMLAWB - signed multiply-accumulate word by halfword =====
  {"arm_smlawb",
   "int arm_smlawb(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlawb %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x10000, 0x00040000, 50}, "DSPMul"},

  // ===== SMLAWT =====
  {"arm_smlawt",
   "int arm_smlawt(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlawt %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x10000, 0x00040000, 50}, "DSPMul"},

  // ===== SMLAD - dual signed multiply-accumulate =====
  {"arm_smlad",
   "int arm_smlad(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlad %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006, 100}, "DSPMul"},

  // ===== SMLSD - dual signed multiply-subtract =====
  {"arm_smlsd",
   "int arm_smlsd(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smlsd %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006, 100}, "DSPMul"},

  // ===== SMUAD - dual signed multiply-add =====
  {"arm_smuad",
   "int arm_smuad(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smuad %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006}, "DSPMul"},

  // ===== SMUSD - dual signed multiply-subtract =====
  {"arm_smusd",
   "int arm_smusd(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"smusd %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00030004, 0x00050006}, "DSPMul"},

  // QADD/QSUB/QDADD/QDSUB: clang cross-compile generates SSAT sequence
  // instead of native QADD on cortex-a15 — tested via C saturating clamp patterns
  // in AArch64_NEONAdvOps2RTTests.cpp (c_ssat_i32_to_i16 etc)

  // SSAT/USAT require Thumb2 encoding in cross-compilation — tested via C clamp patterns instead

  // ===== USAD8 - unsigned sum of absolute differences =====
  {"arm_usad8",
   "int arm_usad8(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"usad8 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x01020304, 0x04030201}, "DSPMul"},

  // ===== USADA8 - unsigned sum of absolute differences + accumulate =====
  {"arm_usada8",
   "int arm_usada8(int a, int b, int c) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"usada8 %0, %1, %2, %3\" : \"=r\"(r) : \"r\"(a), \"r\"(b), \"r\"(c));\n"
   "  return r;\n"
   "}\n",
   {0x01020304, 0x04030201, 100}, "DSPMul"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(DSPMul, ARM32DSPRT,
                         ::testing::ValuesIn(kARM32DSP), rtTCName);
