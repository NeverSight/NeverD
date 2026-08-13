//===- ARM32_SatArithRTTests.cpp - ARM32 saturating arithmetic roundtrip --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip tests for ARM32 saturating / parallel DSP arithmetic forced via
// inline asm: QADD/QSUB/QDADD/QDSUB, the parallel (16/8-bit lane) add/sub
// family (QADD16/UQADD16/QADD8/QSUB16/QASX/QSAX, halving SHADD16/UHADD16),
// and SSAT/USAT/SSAT16/USAT16.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32SatRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SatRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32Sat = {
  // ===== QADD — saturating signed 32-bit add =====
  {"arm_qadd",
   "int arm_qadd(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qadd %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x7FFFFF00u, 0x00010000u}, "Sat"},

  // ===== QSUB — saturating signed 32-bit subtract =====
  {"arm_qsub",
   "int arm_qsub(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qsub %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x80000000u, 0x00010000u}, "Sat"},

  // ===== QDADD — Rd = sat(Rm + sat(2*Rn)) =====
  {"arm_qdadd",
   "int arm_qdadd(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qdadd %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x70000000u, 0x10000000u}, "Sat"},

  // ===== QDSUB — Rd = sat(Rm - sat(2*Rn)) =====
  {"arm_qdsub",
   "int arm_qdsub(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qdsub %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x80000000u, 0x10000000u}, "Sat"},

  // ===== QADD16 — parallel saturating signed 16-bit add =====
  {"arm_qadd16",
   "int arm_qadd16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qadd16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x7FFF0001u, 0x00050002u}, "Sat"},

  // ===== UQADD16 — parallel saturating unsigned 16-bit add =====
  {"arm_uqadd16",
   "int arm_uqadd16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"uqadd16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFFFF0001u, 0x00050002u}, "Sat"},

  // ===== QADD8 — parallel saturating signed 8-bit add =====
  {"arm_qadd8",
   "int arm_qadd8(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qadd8 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x7F010203u, 0x40010203u}, "Sat"},

  // ===== QSUB16 — parallel saturating signed 16-bit subtract =====
  {"arm_qsub16",
   "int arm_qsub16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qsub16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x80000001u, 0x00010002u}, "Sat"},

  // ===== QASX — saturating exchange add/sub =====
  {"arm_qasx",
   "int arm_qasx(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qasx %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x7FFF0001u, 0x00027FFFu}, "Sat"},

  // ===== SHADD16 — signed halving 16-bit add =====
  {"arm_shadd16",
   "int arm_shadd16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"shadd16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x7FFF0003u, 0x7FFF0005u}, "Sat"},

  // ===== UHADD16 — unsigned halving 16-bit add =====
  {"arm_uhadd16",
   "int arm_uhadd16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"uhadd16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFFFF0003u, 0xFFFF0005u}, "Sat"},

  // ===== SSAT — saturate signed to N bits =====
  {"arm_ssat",
   "int arm_ssat(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"ssat %0, #16, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x00012345u}, "Sat"},

  // ===== USAT — saturate unsigned to N bits =====
  {"arm_usat",
   "int arm_usat(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"usat %0, #12, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x00012345u}, "Sat"},

  // ===== SSAT16 — saturate two signed 16-bit lanes to N bits =====
  {"arm_ssat16",
   "int arm_ssat16(int a) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"ssat16 %0, #12, %1\" : \"=r\"(r) : \"r\"(a));\n"
   "  return r;\n"
   "}\n",
   {0x7FFF8000u}, "Sat"},

  // ===== QSUB8 — parallel saturating signed 8-bit subtract (4 lanes) =====
  {"arm_qsub8",
   "int arm_qsub8(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qsub8 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x80017FFEu, 0x7F010203u}, "Sat"},

  // ===== UQSUB8 — parallel saturating unsigned 8-bit subtract =====
  {"arm_uqsub8",
   "int arm_uqsub8(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"uqsub8 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x01050310u, 0x100502FFu}, "Sat"},

  // ===== QSAX — saturating subtract/add with exchange =====
  {"arm_qsax",
   "int arm_qsax(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"qsax %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x7FFF8000u, 0x7FFF0001u}, "Sat"},

  // ===== UADD8 — parallel (modulo) unsigned 8-bit add (4 lanes, plain) =====
  {"arm_uadd8",
   "int arm_uadd8(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"uadd8 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFF804020u, 0x02807030u}, "Sat"},

  // ===== SSUB16 — parallel (modulo) signed 16-bit subtract =====
  {"arm_ssub16",
   "int arm_ssub16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"ssub16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00010002u, 0x00030005u}, "Sat"},

  // ===== SHSUB16 — signed halving 16-bit subtract (negative diff) =====
  {"arm_shsub16",
   "int arm_shsub16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"shsub16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00010002u, 0x00050008u}, "Sat"},

  // ===== UHSUB16 — unsigned halving 16-bit subtract (negative diff path) =====
  {"arm_uhsub16",
   "int arm_uhsub16(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"uhsub16 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00010002u, 0x00050008u}, "Sat"},

  // ===== SHADD8 — signed halving 8-bit add (negative lanes) =====
  {"arm_shadd8",
   "int arm_shadd8(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"shadd8 %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x80FF7F01u, 0x80017F02u}, "Sat"},

  // ===== SASX — signed (modulo) add/sub with exchange =====
  {"arm_sasx",
   "int arm_sasx(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ volatile (\"sasx %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0x00100020u, 0x00050003u}, "Sat"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Sat, ARM32SatRT,
                         ::testing::ValuesIn(kARM32Sat), rtTCName);
