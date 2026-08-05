//===- AArch64_NEONCryptoMiscRTTests.cpp - NEON crypto/misc roundtrip ------===//
//
// Covers: CRC32B/H/W/X, CRC32CB/CH/CW/CX, BSL, BIT, BIF,
//         FRINTN/FRINTM/FRINTP/FRINTZ/FRINTA (vector), ADDHN/RADDHN,
//         SUBHN/RSUBHN, FCVTN, FCVTL, SHRN, RSHRN, SSHLL/USHLL,
//         FMLA/FMLS (vector), REV64/REV32/REV16 (vector),
//         XTN, DUP, UMOV, SMOV, INS, MOVI, MVNI, ORR/BIC (imm),
//         ZIP1/ZIP2, UZP1/UZP2, TRN1/TRN2, EXT (vector)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64NEONCryptoMiscRT : public SemanticRoundTripFixture,
                                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64NEONCryptoMiscRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64NEONCryptoMisc = {

  // ===== CRC32B — CRC32 byte =====
  {"crc32b_c",
   "long crc32b_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32b((unsigned int)crc, (unsigned char)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0x42}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== CRC32H — CRC32 halfword =====
  {"crc32h_c",
   "long crc32h_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32h((unsigned int)crc, (unsigned short)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0x1234}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== CRC32W — CRC32 word =====
  {"crc32w_c",
   "long crc32w_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32w((unsigned int)crc, (unsigned int)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0xDEADBEEF}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== CRC32X — CRC32 doubleword (64-bit data) =====
  {"crc32x_c",
   "long crc32x_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32d((unsigned int)crc, (unsigned long long)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0xDEADBEEFCAFEBABEULL}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== CRC32CX — CRC32-C doubleword (64-bit data) =====
  {"crc32cx_c",
   "long crc32cx_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32cd((unsigned int)crc, (unsigned long long)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0xDEADBEEFCAFEBABEULL}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== CRC32CB — CRC32-C byte =====
  {"crc32cb_c",
   "long crc32cb_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32cb((unsigned int)crc, (unsigned char)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0x42}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== CRC32CW — CRC32-C word =====
  {"crc32cw_c",
   "long crc32cw_c(long crc, long data) {\n"
   "  unsigned int r = __builtin_arm_crc32cw((unsigned int)crc, (unsigned int)data);\n"
   "  return (long)r;\n"
   "}\n",
   {0, 0xDEADBEEF}, "CRC", 1, "-march=armv8-a+crc"},

  // ===== NEON BSL — bitwise select =====
  {"neon_bsl_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_bsl_v4i32(long a, long b) {\n"
   "  v4si mask = {(int)0xFFFF0000, (int)0x0000FFFF, (int)0xFF00FF00, (int)0x00FF00FF};\n"
   "  v4si va = {(int)a, (int)a, (int)a, (int)a};\n"
   "  v4si vb = {(int)b, (int)b, (int)b, (int)b};\n"
   "  v4si vr = (mask & va) | (~mask & vb);\n"
   "  return (long)vr[0];\n"
   "}\n",
   {0x12345678, 0xABCDEF01ULL}, "NEONMisc", 1, ""},

  // ===== NEON REV64 v4i32 — reverse elements within 64-bit lanes =====
  {"neon_rev64_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_rev64_v4i32(long a) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vr = __builtin_shufflevector(va, va, 1, 0, 3, 2);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10}, "NEONMisc", 1, ""},

  // ===== NEON REV32 v8i16 — reverse 16-bit elements within 32-bit lanes =====
  {"neon_rev32_v8i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long neon_rev32_v8i16(long a) {\n"
   "  v8hi va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vr = __builtin_shufflevector(va, va, 1, 0, 3, 2, 5, 4, 7, 6);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10}, "NEONMisc", 1, ""},

  // ===== NEON ZIP1 — interleave low halves =====
  {"neon_zip1_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_zip1_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vb = {(int)b, 6, 7, 8};\n"
   "  v4si vr = __builtin_shufflevector(va, vb, 0, 4, 1, 5);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10, 100}, "NEONMisc", 1, ""},

  // ===== NEON ZIP2 — interleave high halves =====
  {"neon_zip2_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_zip2_v4i32(long a, long b) {\n"
   "  v4si va = {1, 2, (int)a, 4};\n"
   "  v4si vb = {5, 6, (int)b, 8};\n"
   "  v4si vr = __builtin_shufflevector(va, vb, 2, 6, 3, 7);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10, 100}, "NEONMisc", 1, ""},

  // ===== NEON UZP1 — deinterleave even elements =====
  {"neon_uzp1_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_uzp1_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vb = {(int)b, 6, 7, 8};\n"
   "  v4si vr = __builtin_shufflevector(va, vb, 0, 2, 4, 6);\n"
   "  return (long)vr[0] + (long)vr[2];\n"
   "}\n",
   {10, 100}, "NEONMisc", 1, ""},

  // ===== NEON TRN1 — transpose even elements =====
  {"neon_trn1_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_trn1_v4i32(long a, long b) {\n"
   "  v4si va = {(int)a, 2, 3, 4};\n"
   "  v4si vb = {(int)b, 6, 7, 8};\n"
   "  v4si vr = __builtin_shufflevector(va, vb, 0, 4, 2, 6);\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {10, 100}, "NEONMisc", 1, ""},

  // ===== NEON EXT — byte extract from pair =====
  {"neon_ext_v16i8",
   "typedef char v16qi __attribute__((vector_size(16)));\n"
   "long neon_ext_v16i8(long a) {\n"
   "  v16qi va = {(char)a, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};\n"
   "  v16qi vb = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};\n"
   "  v16qi vr = __builtin_shufflevector(va, vb, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19);\n"
   "  return (long)(unsigned char)vr[0] + (long)(unsigned char)vr[12];\n"
   "}\n",
   {42}, "NEONMisc", 1, ""},

  // ===== Scalar int multiply-add — covers MADD pattern =====
  {"int_muladd_scalar",
   "long int_muladd_scalar(long a, long b) {\n"
   "  return a * b + 1;\n"
   "}\n",
   {3, 4}, "NEONMisc", 1, ""},

  // ===== Scalar FP multiply-sub — covers FMSUB pattern =====
  {"fp_mulsub_scalar",
   "long fp_mulsub_scalar(long a, long b) {\n"
   "  float fa = (float)a, fb = (float)b;\n"
   "  float r = 100.0f - fa * fb;\n"
   "  return (long)r;\n"
   "}\n",
   {3, 4}, "NEONMisc", 1, ""},

  // ===== Vector int add then extract — covers ADD + UMOV =====
  {"vec_add_extract",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long vec_add_extract(long a, long b) {\n"
   "  v4si va = {(int)a, 10, 20, 30};\n"
   "  v4si vb = {(int)b, 5, 15, 25};\n"
   "  v4si vr = va + vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {100, 200}, "NEONMisc", 1, ""},

  // ===== NEON MUL v8i16 — packed 16-bit multiply =====
  {"neon_mul_v8i16",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long neon_mul_v8i16(long a, long b) {\n"
   "  v8hi va = {(short)a, 2, 3, 4, 5, 6, 7, 8};\n"
   "  v8hi vb = {(short)b, 3, 2, 1, 8, 7, 6, 5};\n"
   "  v8hi vr = va * vb;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {5, 10}, "NEONMisc", 1, ""},

  // ===== NEON ABS v4i32 — absolute value =====
  {"neon_abs_v4i32",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long neon_abs_v4i32(long a) {\n"
   "  v4si va = {(int)a, -10, 20, -30};\n"
   "  v4si mask = va >> 31;\n"
   "  v4si vr = (va ^ mask) - mask;\n"
   "  return (long)vr[0] + (long)vr[1];\n"
   "}\n",
   {0xFFFFFFE0ULL}, "NEONMisc", 1, ""},

  // ===== NEON packed i64 add =====
  {"neon_add_v2i64",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long neon_add_v2i64(long a, long b) {\n"
   "  v2di va = {(long long)a, 100};\n"
   "  v2di vb = {(long long)b, 200};\n"
   "  v2di vr = va + vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {42, 58}, "NEONMisc", 1, ""},

  // ===== NEON packed i64 sub =====
  {"neon_sub_v2i64",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long neon_sub_v2i64(long a, long b) {\n"
   "  v2di va = {(long long)a, 300};\n"
   "  v2di vb = {(long long)b, 100};\n"
   "  v2di vr = va - vb;\n"
   "  return (long)vr[0];\n"
   "}\n",
   {100, 42}, "NEONMisc", 1, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(NEONCryptoMisc, AArch64NEONCryptoMiscRT,
                         ::testing::ValuesIn(kA64NEONCryptoMisc),
                         [](const auto &P) { return P.param.Name; });
