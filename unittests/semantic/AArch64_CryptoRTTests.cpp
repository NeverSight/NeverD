//===- AArch64_CryptoRTTests.cpp - AES/SHA1/SHA256 roundtrip ---------------===//
//
// Covers the AArch64 crypto extension (FEAT_AES / FEAT_SHA1 / FEAT_SHA2):
//   AESE, AESD, AESMC, AESIMC,
//   SHA1C, SHA1P, SHA1M, SHA1H, SHA1SU0, SHA1SU1,
//   SHA256H, SHA256H2, SHA256SU0, SHA256SU1.
//
// These previously lifted to a placeholder x86 intrinsic with no AArch64
// emitter handler, folding the whole function to `ret 0`.  Each now maps to the
// real LLVM AArch64 crypto intrinsic.  Unicorn's default MAX CPU enables the
// crypto extensions, so the roundtrip is bit-exact.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64CryptoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64CryptoRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64Crypto = {

  // ===== AESE — single AES encrypt round (SubBytes∘ShiftRows∘AddRoundKey) =====
  {"aese",
   "#include <arm_neon.h>\n"
   "unsigned long aese(unsigned long a, unsigned long b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u64((uint64x2_t){a, b});\n"
   "  uint8x16_t k = vreinterpretq_u8_u64((uint64x2_t){b ^ 0x55ULL, a});\n"
   "  uint64x2_t o = vreinterpretq_u64_u8(vaeseq_u8(d, k));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== AESD — single AES decrypt round =====
  {"aesd",
   "#include <arm_neon.h>\n"
   "unsigned long aesd(unsigned long a, unsigned long b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u64((uint64x2_t){a, b});\n"
   "  uint8x16_t k = vreinterpretq_u8_u64((uint64x2_t){a, b ^ 0xA5ULL});\n"
   "  uint64x2_t o = vreinterpretq_u64_u8(vaesdq_u8(d, k));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== AESMC — AES MixColumns =====
  {"aesmc",
   "#include <arm_neon.h>\n"
   "unsigned long aesmc(unsigned long a, unsigned long b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u64((uint64x2_t){a, b});\n"
   "  uint64x2_t o = vreinterpretq_u64_u8(vaesmcq_u8(d));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0xDEADBEEFCAFEBABEULL, 0x8BADF00DC0FFEE11ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== AESIMC — AES Inverse MixColumns =====
  {"aesimc",
   "#include <arm_neon.h>\n"
   "unsigned long aesimc(unsigned long a, unsigned long b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u64((uint64x2_t){a, b});\n"
   "  uint64x2_t o = vreinterpretq_u64_u8(vaesimcq_u8(d));\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== Full AES encrypt round (aese + aesmc, the common idiom) =====
  {"aes_round",
   "#include <arm_neon.h>\n"
   "unsigned long aes_round(unsigned long a, unsigned long b) {\n"
   "  uint8x16_t st = vreinterpretq_u8_u64((uint64x2_t){a, b});\n"
   "  uint8x16_t key = vreinterpretq_u8_u64(\n"
   "      (uint64x2_t){0x0f0e0d0c0b0a0908ULL, 0x0706050403020100ULL});\n"
   "  st = vaesmcq_u8(vaeseq_u8(st, key));\n"
   "  uint64x2_t o = vreinterpretq_u64_u8(st);\n"
   "  return o[0] ^ o[1];\n"
   "}\n",
   {0x3243F6A8885A308DULL, 0x313198A2E0370734ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA1H — SHA1 fixed rotate of hash_e =====
  {"sha1h",
   "#include <arm_neon.h>\n"
   "unsigned long sha1h(unsigned long a) {\n"
   "  return (unsigned long)vsha1h_u32((unsigned int)a);\n"
   "}\n",
   {0x67452301ULL}, "Crypto", 1, "-march=armv8-a+crypto"},

  // ===== SHA1C — SHA1 hash update (choose) =====
  {"sha1c",
   "#include <arm_neon.h>\n"
   "unsigned long sha1c(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t abcd = {(unsigned)a, (unsigned)(a>>32), (unsigned)b,\n"
   "                     (unsigned)(b>>32)};\n"
   "  uint32_t e = (unsigned)(a ^ b);\n"
   "  uint32x4_t wk = {0x5A827999u, 0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu};\n"
   "  uint32x4_t r = vsha1cq_u32(abcd, e, wk);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 16);\n"
   "}\n",
   {0xEFCDAB8967452301ULL, 0xC3D2E1F0A1B2C3D4ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA1P — SHA1 hash update (parity) =====
  {"sha1p",
   "#include <arm_neon.h>\n"
   "unsigned long sha1p(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t abcd = {(unsigned)a, (unsigned)(a>>32), (unsigned)b,\n"
   "                     (unsigned)(b>>32)};\n"
   "  uint32_t e = (unsigned)(a + b);\n"
   "  uint32x4_t wk = {0x6ED9EBA1u, 0x11111111u, 0x22222222u, 0x33333333u};\n"
   "  uint32x4_t r = vsha1pq_u32(abcd, e, wk);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 16);\n"
   "}\n",
   {0x1111222233334444ULL, 0x5555666677778888ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA1M — SHA1 hash update (majority) =====
  {"sha1m",
   "#include <arm_neon.h>\n"
   "unsigned long sha1m(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t abcd = {(unsigned)a, (unsigned)(a>>32), (unsigned)b,\n"
   "                     (unsigned)(b>>32)};\n"
   "  uint32_t e = (unsigned)(a - b);\n"
   "  uint32x4_t wk = {0x8F1BBCDCu, 0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu};\n"
   "  uint32x4_t r = vsha1mq_u32(abcd, e, wk);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 16);\n"
   "}\n",
   {0xCAFEBABEDEADBEEFULL, 0x0123456789ABCDEFULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA1SU0 — SHA1 schedule update 0 =====
  {"sha1su0",
   "#include <arm_neon.h>\n"
   "unsigned long sha1su0(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t w0 = {(unsigned)a, (unsigned)(a>>32), 0x11u, 0x22u};\n"
   "  uint32x4_t w4 = {(unsigned)b, (unsigned)(b>>32), 0x33u, 0x44u};\n"
   "  uint32x4_t w8 = {0xAAu, 0xBBu, 0xCCu, 0xDDu};\n"
   "  uint32x4_t r = vsha1su0q_u32(w0, w4, w8);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 8);\n"
   "}\n",
   {0xDEADBEEF12345678ULL, 0xCAFEBABE87654321ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA1SU1 — SHA1 schedule update 1 =====
  {"sha1su1",
   "#include <arm_neon.h>\n"
   "unsigned long sha1su1(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t tw = {(unsigned)a, (unsigned)(a>>32), (unsigned)b,\n"
   "                   (unsigned)(b>>32)};\n"
   "  uint32x4_t w12 = {0x10u, 0x20u, 0x30u, 0x40u};\n"
   "  uint32x4_t r = vsha1su1q_u32(tw, w12);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 8);\n"
   "}\n",
   {0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA256H — SHA256 hash update (part 1) =====
  {"sha256h",
   "#include <arm_neon.h>\n"
   "unsigned long sha256h(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t abcd = {(unsigned)a, (unsigned)(a>>32), 0x6A09E667u,\n"
   "                     0xBB67AE85u};\n"
   "  uint32x4_t efgh = {(unsigned)b, (unsigned)(b>>32), 0x3C6EF372u,\n"
   "                     0xA54FF53Au};\n"
   "  uint32x4_t wk = {0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u};\n"
   "  uint32x4_t r = vsha256hq_u32(abcd, efgh, wk);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 16);\n"
   "}\n",
   {0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA256H2 — SHA256 hash update (part 2) =====
  {"sha256h2",
   "#include <arm_neon.h>\n"
   "unsigned long sha256h2(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t abcd = {(unsigned)a, (unsigned)(a>>32), 0x1F83D9ABu,\n"
   "                     0x5BE0CD19u};\n"
   "  uint32x4_t efgh = {(unsigned)b, (unsigned)(b>>32), 0x6A09E667u,\n"
   "                     0xBB67AE85u};\n"
   "  uint32x4_t wk = {0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u};\n"
   "  uint32x4_t r = vsha256h2q_u32(abcd, efgh, wk);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 16);\n"
   "}\n",
   {0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA256SU0 — SHA256 schedule update 0 =====
  {"sha256su0",
   "#include <arm_neon.h>\n"
   "unsigned long sha256su0(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t w0 = {(unsigned)a, (unsigned)(a>>32), (unsigned)b,\n"
   "                   (unsigned)(b>>32)};\n"
   "  uint32x4_t w4 = {0xDEADu, 0xBEEFu, 0xCAFEu, 0xBABEu};\n"
   "  uint32x4_t r = vsha256su0q_u32(w0, w4);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 8);\n"
   "}\n",
   {0x428A2F9871374491ULL, 0xB5C0FBCFE9B5DBA5ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

  // ===== SHA256SU1 — SHA256 schedule update 1 =====
  {"sha256su1",
   "#include <arm_neon.h>\n"
   "unsigned long sha256su1(unsigned long a, unsigned long b) {\n"
   "  uint32x4_t tw = {(unsigned)a, (unsigned)(a>>32), (unsigned)b,\n"
   "                   (unsigned)(b>>32)};\n"
   "  uint32x4_t w8 = {0x11u, 0x22u, 0x33u, 0x44u};\n"
   "  uint32x4_t w12 = {0x55u, 0x66u, 0x77u, 0x88u};\n"
   "  uint32x4_t r = vsha256su1q_u32(tw, w8, w12);\n"
   "  return (unsigned long)r[0] ^ ((unsigned long)r[1] << 32) ^\n"
   "         (unsigned long)r[2] ^ ((unsigned long)r[3] << 8);\n"
   "}\n",
   {0x3243F6A8885A308DULL, 0x313198A2E0370734ULL}, "Crypto", 1,
   "-march=armv8-a+crypto"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Crypto, AArch64CryptoRT,
                         ::testing::ValuesIn(kA64Crypto),
                         [](const auto &P) { return P.param.Name; });
