//===- AArch64_SHA512SM34RTTests.cpp - SHA512/SM3/SM4 roundtrip -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers the AArch64 ARMv8.2 crypto extensions:
//   FEAT_SHA512: SHA512H, SHA512H2, SHA512SU0, SHA512SU1
//   FEAT_SM3:    SM3PARTW1, SM3PARTW2, SM3SS1, SM3TT1A/1B/2A/2B
//   FEAT_SM4:    SM4E, SM4EKEY
//
// These previously lifted to a generic ShaGeneric/AesGeneric placeholder that
// merely copied one source, folding the result to garbage.  Each now maps to
// the real LLVM AArch64 crypto intrinsic (llvm.aarch64.crypto.sha512h, ...) so
// codegen emits the actual instruction.  Unicorn's default MAX CPU enables all
// three extensions (id_aa64isar0: SHA2=2, SM3=1, SM4=1), so the roundtrip is
// bit-exact.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64SHA512SM34RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64SHA512SM34RT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64Sha512Sm34 = {

  // ===== SHA512H — SHA512 hash update part 1 (3-source, destructive) =====
  {"sha512h",
   "#include <arm_neon.h>\n"
   "unsigned long sha512h(unsigned long a, unsigned long b) {\n"
   "  uint64x2_t x = (uint64x2_t){a, b};\n"
   "  uint64x2_t y = (uint64x2_t){b ^ 0x55ULL, a + 1};\n"
   "  uint64x2_t z = (uint64x2_t){a ^ 0xA5A5ULL, b + 3};\n"
   "  uint64x2_t o = vsha512hq_u64(x, y, z);\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8.2-a+sha3"},

  // ===== SHA512H2 — SHA512 hash update part 2 =====
  {"sha512h2",
   "#include <arm_neon.h>\n"
   "unsigned long sha512h2(unsigned long a, unsigned long b) {\n"
   "  uint64x2_t x = (uint64x2_t){a, b};\n"
   "  uint64x2_t y = (uint64x2_t){b, a ^ 0x33ULL};\n"
   "  uint64x2_t z = (uint64x2_t){a + 7, b ^ 0x99ULL};\n"
   "  uint64x2_t o = vsha512h2q_u64(x, y, z);\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL}, "Crypto", 1,
   "-march=armv8.2-a+sha3"},

  // ===== SHA512SU0 — SHA512 schedule update 0 (2-source, destructive) =====
  {"sha512su0",
   "#include <arm_neon.h>\n"
   "unsigned long sha512su0(unsigned long a, unsigned long b) {\n"
   "  uint64x2_t x = (uint64x2_t){a, b};\n"
   "  uint64x2_t y = (uint64x2_t){b ^ 0x5AULL, a + 9};\n"
   "  uint64x2_t o = vsha512su0q_u64(x, y);\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0xCAFEBABEDEADBEEFULL, 0x8BADF00DC0FFEE11ULL}, "Crypto", 1,
   "-march=armv8.2-a+sha3"},

  // ===== SHA512SU1 — SHA512 schedule update 1 (3-source, destructive) =====
  {"sha512su1",
   "#include <arm_neon.h>\n"
   "unsigned long sha512su1(unsigned long a, unsigned long b) {\n"
   "  uint64x2_t x = (uint64x2_t){a, b};\n"
   "  uint64x2_t y = (uint64x2_t){b + 1, a ^ 0x77ULL};\n"
   "  uint64x2_t z = (uint64x2_t){a ^ 0x11ULL, b + 5};\n"
   "  uint64x2_t o = vsha512su1q_u64(x, y, z);\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL}, "Crypto", 1,
   "-march=armv8.2-a+sha3"},

  // ===== SHA512 full update idiom (su0 + su1 + h + h2 chained) =====
  {"sha512_chain",
   "#include <arm_neon.h>\n"
   "unsigned long sha512_chain(unsigned long a, unsigned long b) {\n"
   "  uint64x2_t w0 = (uint64x2_t){a, b};\n"
   "  uint64x2_t w1 = (uint64x2_t){b ^ a, a + b};\n"
   "  uint64x2_t s = vsha512su0q_u64(w0, w1);\n"
   "  s = vsha512su1q_u64(s, w1, w0);\n"
   "  uint64x2_t h = vsha512hq_u64(w0, w1, s);\n"
   "  h = vsha512h2q_u64(h, w0, s);\n"
   "  return h[0] ^ h[1];\n"
   "}\n",
   {0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL}, "Crypto", 1,
   "-march=armv8.2-a+sha3"},

  // ===== SM3PARTW1 — SM3 message expansion part 1 (3-source, destructive) =====
  {"sm3partw1",
   "#include <arm_neon.h>\n"
   "unsigned long sm3partw1(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b ^ 0x55ULL, a});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a + 1, b + 2});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3partw1q_u32(x, y, z));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM3PARTW2 — SM3 message expansion part 2 =====
  {"sm3partw2",
   "#include <arm_neon.h>\n"
   "unsigned long sm3partw2(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b, a ^ 0x33ULL});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a + 7, b ^ 0x99ULL});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3partw2q_u32(x, y, z));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM3SS1 — SM3 round constant rotate (4-register, non-destructive) =====
  {"sm3ss1",
   "#include <arm_neon.h>\n"
   "unsigned long sm3ss1(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b, a});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a ^ b, a + b});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3ss1q_u32(x, y, z));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM3TT1A — SM3 compression T1, lane index 0 (destructive + imm) =====
  {"sm3tt1a",
   "#include <arm_neon.h>\n"
   "unsigned long sm3tt1a(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b ^ a, a + 1});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a + 2, b + 3});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3tt1aq_u32(x, y, z, 0));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM3TT1B — SM3 compression T1 variant, lane index 2 =====
  {"sm3tt1b",
   "#include <arm_neon.h>\n"
   "unsigned long sm3tt1b(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b, a ^ 0x5ULL});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a + 4, b + 6});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3tt1bq_u32(x, y, z, 2));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM3TT2A — SM3 compression T2, lane index 1 =====
  {"sm3tt2a",
   "#include <arm_neon.h>\n"
   "unsigned long sm3tt2a(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b + 1, a});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a ^ 0x9ULL, b + 2});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3tt2aq_u32(x, y, z, 1));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0xCAFEBABEDEADBEEFULL, 0x8BADF00DC0FFEE11ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM3TT2B — SM3 compression T2 variant, lane index 3 =====
  {"sm3tt2b",
   "#include <arm_neon.h>\n"
   "unsigned long sm3tt2b(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t y = vreinterpretq_u32_u64((uint64x2_t){b, a + 8});\n"
   "  uint32x4_t z = vreinterpretq_u32_u64((uint64x2_t){a + 3, b ^ 0xFULL});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm3tt2bq_u32(x, y, z, 3));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM4E — SM4 encryption round (2-source, destructive) =====
  {"sm4e",
   "#include <arm_neon.h>\n"
   "unsigned long sm4e(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t k = vreinterpretq_u32_u64((uint64x2_t){b ^ 0x55ULL, a + 1});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm4eq_u32(x, k));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM4EKEY — SM4 key expansion (2-source, non-destructive) =====
  {"sm4ekey",
   "#include <arm_neon.h>\n"
   "unsigned long sm4ekey(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t x = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t k = vreinterpretq_u32_u64((uint64x2_t){b, a ^ 0xA5ULL});\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm4ekeyq_u32(x, k));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},

  // ===== SM4 round idiom (key expand then encrypt) =====
  {"sm4_round",
   "#include <arm_neon.h>\n"
   "unsigned long sm4_round(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t st = vreinterpretq_u32_u64((uint64x2_t){a, b});\n"
   "  uint32x4_t ck = vreinterpretq_u32_u64((uint64x2_t){a ^ b, a + b});\n"
   "  uint32x4_t rk = vsm4ekeyq_u32(st, ck);\n"
   "  uint64x2_t o = vreinterpretq_u64_u32(vsm4eq_u32(st, rk));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8.2-a+sm4"},
};

INSTANTIATE_TEST_SUITE_P(SHA512SM34, AArch64SHA512SM34RT,
                         ::testing::ValuesIn(kA64Sha512Sm34), rtTCName);

// clang-format on
