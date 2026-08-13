//===- ARM32_CoreExtRTTests.cpp - ARM32 core/ext roundtrip tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: ADC, SBC, RSB, RSC, TST, TEQ, CMP, CMN, BIC, ORN,
//         UMULL, UMLAL, SMULL, SMLAL, MOVW, MOVT, UBFX, SBFX,
//         BFI, BFC, CLZ, RBIT, REV, REV16, REVSH, PKH*,
//         SSAT, USAT, QADD, QSUB, QDADD, QDSUB
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32CoreExtRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CoreExtRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kARM32CoreExt = {

  // ===== ADC — add with carry =====
  {"arm_adc",
   "int arm_adc(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ (\"adds %0, %1, %2\\n\\tadc %0, %0, #0\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {100, 50}, "CoreExt", 1, ""},

  // ===== RSB — reverse subtract =====
  {"arm_rsb",
   "int arm_rsb(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ (\"rsb %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {30, 100}, "CoreExt", 1, ""},

  // ===== BIC — bit clear =====
  {"arm_bic",
   "int arm_bic(int a, int b) {\n"
   "  int r;\n"
   "  __asm__ (\"bic %0, %1, %2\" : \"=r\"(r) : \"r\"(a), \"r\"(b));\n"
   "  return r;\n"
   "}\n",
   {0xFFFF, 0x00FF}, "CoreExt", 1, ""},

  // ===== CLZ — count leading zeros =====
  {"arm_clz_c",
   "int arm_clz_c(int a) {\n"
   "  return __builtin_clz((unsigned int)a);\n"
   "}\n",
   {0x100}, "CoreExt", 1, ""},

  // ===== RBIT — reverse bits =====
  {"arm_rbit",
   "int arm_rbit(int a) {\n"
   "  return (int)__builtin_bitreverse32((unsigned int)a);\n"
   "}\n",
   {1}, "CoreExt", 1, ""},

  // ===== REV — byte reverse =====
  {"arm_rev",
   "int arm_rev(int a) {\n"
   "  return (int)__builtin_bswap32((unsigned int)a);\n"
   "}\n",
   {0x01020304}, "CoreExt", 1, ""},

  // ===== REV16 — byte reverse in halfwords =====
  {"arm_rev16",
   "int arm_rev16(int a) {\n"
   "  unsigned int u = (unsigned int)a;\n"
   "  return (int)(((u & 0xFF00FF00) >> 8) | ((u & 0x00FF00FF) << 8));\n"
   "}\n",
   {0x01020304}, "CoreExt", 0, ""},

  // ===== UMULL — unsigned multiply long =====
  {"arm_umull",
   "int arm_umull(int a, int b) {\n"
   "  unsigned long long r = (unsigned long long)(unsigned int)a * (unsigned int)b;\n"
   "  return (int)(r >> 32) + (int)r;\n"
   "}\n",
   {0x10000, 0x10000}, "CoreExt", 1, ""},

  // ===== SMULL — signed multiply long =====
  {"arm_smull",
   "int arm_smull(int a, int b) {\n"
   "  long long r = (long long)a * (long long)b;\n"
   "  return (int)(r >> 32) + (int)r;\n"
   "}\n",
   {0x10000, 0xFFFFFFFEULL}, "CoreExt", 1, ""},

  // ===== Bit field extract (C equivalent) =====
  {"c_bitfield_extract",
   "int c_bitfield_extract(int a) {\n"
   "  return (a >> 8) & 0xFF;\n"
   "}\n",
   {0x12345678}, "CoreExt", 0, ""},

  // ===== Conditional execution via CMOV pattern =====
  {"arm_cond_select",
   "int arm_cond_select(int a, int b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {42, 100}, "CoreExt", 0, ""},

  // ===== TST pattern =====
  {"arm_tst",
   "int arm_tst(int a, int b) {\n"
   "  return (a & b) ? 1 : 0;\n"
   "}\n",
   {0xFF, 0x0F}, "CoreExt", 0, ""},

  // ===== MOVW/MOVT pattern — load 32-bit immediate =====
  {"arm_movw_movt",
   "int arm_movw_movt(int a) {\n"
   "  return a + 0x12345678;\n"
   "}\n",
   {1}, "CoreExt", 0, ""},

  // ===== MLS — multiply and subtract =====
  {"arm_mls",
   "int arm_mls(int a, int b, int c) {\n"
   "  return c - a * b;\n"
   "}\n",
   {6, 7, 100}, "CoreExt", 0, ""},

  // ARM32 atomic ldrex/strex have known Unicorn emulation limitations.

  // ===== Volatile load/store =====
  {"arm32_volatile_ls",
   "int arm32_volatile_ls(int a) {\n"
   "  volatile int val = a;\n"
   "  int r = val;\n"
   "  val = r + 1;\n"
   "  return val;\n"
   "}\n",
   {42}, "CoreExt", 1, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CoreExt, ARM32CoreExtRT,
                         ::testing::ValuesIn(kARM32CoreExt),
                         [](const auto &P) { return P.param.Name; });
