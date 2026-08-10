//===- ARM32_CryptoRTTests.cpp - ARMv8 AArch32 AES/SHA roundtrip -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers the ARMv8 AArch32 crypto extension:
//   AESE, AESD, AESMC, AESIMC,
//   SHA1C, SHA1P, SHA1M, SHA1H, SHA1SU0, SHA1SU1,
//   SHA256H, SHA256H2, SHA256SU0, SHA256SU1.
//
// These previously lifted to a placeholder x86 intrinsic with no ARM emitter
// handler, folding the whole function to `ret 0`.  Each now maps to the real
// LLVM ARM NEON crypto intrinsic (codegen adds +v8,+fp-armv8,+aes,+sha2).
//
// The default ARM32 fixture pins cortex-a15 (ARMv7, no crypto), so these tests
// compile with an ARMv8 AArch32 target and run under Unicorn's MAX CPU model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32CryptoRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CryptoRT, Verify) { roundTripARM32(GetParam()); }

// ARMv8 AArch32 crypto needs an ARMv8 target + the MAX CPU for emulation.
#define A32CRYPTO "Crypto", 1, \
  "-march=armv8-a+crypto -mfpu=crypto-neon-fp-armv8", false, \
  "armv8-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kArm32Crypto = {

  // ===== AESE — single AES encrypt round =====
  {"aese",
   "#include <arm_neon.h>\n"
   "unsigned aese(unsigned a, unsigned b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u32((uint32x4_t){a, b, a ^ b, a + b});\n"
   "  /* Avoid PC-relative literal pool so lift keeps the key vector. */\n"
   "  uint8x16_t k = vreinterpretq_u8_u32(\n"
   "      (uint32x4_t){b, a, (b ^ a) + 0x55u, (a ^ b) + 0xFFu});\n"
   "  uint32x4_t o = vreinterpretq_u32_u8(vaeseq_u8(d, k));\n"
   "  return o[0] ^ o[1] ^ o[2] ^ o[3];\n"
   "}\n",
   {0x89ABCDEFu, 0x76543210u}, A32CRYPTO},

  // ===== AESD — single AES decrypt round =====
  {"aesd",
   "#include <arm_neon.h>\n"
   "unsigned aesd(unsigned a, unsigned b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u32((uint32x4_t){a, b, a + b, a ^ b});\n"
   "  uint8x16_t k = vreinterpretq_u8_u32((uint32x4_t){a, b, 0xA5u, 0u});\n"
   "  uint32x4_t o = vreinterpretq_u32_u8(vaesdq_u8(d, k));\n"
   "  return o[0] ^ o[1] ^ o[2] ^ o[3];\n"
   "}\n",
   {0x55667788u, 0x99AABBCCu}, A32CRYPTO},

  // ===== AESMC — AES MixColumns =====
  {"aesmc",
   "#include <arm_neon.h>\n"
   "unsigned aesmc(unsigned a, unsigned b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u32((uint32x4_t){a, b, ~a, ~b});\n"
   "  uint32x4_t o = vreinterpretq_u32_u8(vaesmcq_u8(d));\n"
   "  return o[0] ^ o[1] ^ o[2] ^ o[3];\n"
   "}\n",
   {0xCAFEBABEu, 0xC0FFEE11u}, A32CRYPTO},

  // ===== AESIMC — AES Inverse MixColumns =====
  {"aesimc",
   "#include <arm_neon.h>\n"
   "unsigned aesimc(unsigned a, unsigned b) {\n"
   "  uint8x16_t d = vreinterpretq_u8_u32((uint32x4_t){a, b, a ^ 0x5Au, b ^ 0xA5u});\n"
   "  uint32x4_t o = vreinterpretq_u32_u8(vaesimcq_u8(d));\n"
   "  return o[0] ^ o[1] ^ o[2] ^ o[3];\n"
   "}\n",
   {0x44556677u, 0x8899AABBu}, A32CRYPTO},

  // ===== Full AES encrypt round (aese + aesmc) =====
  {"aes_round",
   "#include <arm_neon.h>\n"
   "unsigned aes_round(unsigned a, unsigned b) {\n"
   "  uint8x16_t st = vreinterpretq_u8_u32((uint32x4_t){a, b, a ^ b, a + b});\n"
   "  uint8x16_t key = vreinterpretq_u8_u32(\n"
   "      (uint32x4_t){0x0b0a0908u, 0x0f0e0d0cu, 0x03020100u, 0x07060504u});\n"
   "  st = vaesmcq_u8(vaeseq_u8(st, key));\n"
   "  uint32x4_t o = vreinterpretq_u32_u8(st);\n"
   "  return o[0] ^ o[1] ^ o[2] ^ o[3];\n"
   "}\n",
   {0x885A308Du, 0xE0370734u}, A32CRYPTO},

  // ===== SHA1H — SHA1 fixed rotate of hash_e =====
  {"sha1h",
   "#include <arm_neon.h>\n"
   "unsigned sha1h(unsigned a) {\n"
   "  return vsha1h_u32(a);\n"
   "}\n",
   {0x67452301u}, A32CRYPTO},

  // ===== SHA1C — SHA1 hash update (choose) =====
  {"sha1c",
   "#include <arm_neon.h>\n"
   "unsigned sha1c(unsigned a, unsigned b) {\n"
   "  uint32x4_t abcd = {a, b, a ^ b, a + b};\n"
   "  uint32_t e = a - b;\n"
   "  uint32x4_t wk = {(a ^ b) | 1u, a, b, a + b};\n"
   "  uint32x4_t r = vsha1cq_u32(abcd, e, wk);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0x67452301u, 0xEFCDAB89u}, A32CRYPTO},

  // ===== SHA1P — SHA1 hash update (parity) =====
  {"sha1p",
   "#include <arm_neon.h>\n"
   "unsigned sha1p(unsigned a, unsigned b) {\n"
   "  uint32x4_t abcd = {a, b, a ^ b, a + b};\n"
   "  uint32_t e = a + b;\n"
   "  uint32x4_t wk = {a + b, a ^ b, a | 1u, b | 1u};\n"
   "  uint32x4_t r = vsha1pq_u32(abcd, e, wk);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0x11112222u, 0x55556666u}, A32CRYPTO},

  // ===== SHA1M — SHA1 hash update (majority) =====
  {"sha1m",
   "#include <arm_neon.h>\n"
   "unsigned sha1m(unsigned a, unsigned b) {\n"
   "  uint32x4_t abcd = {a, b, a ^ b, a + b};\n"
   "  uint32_t e = a ^ b;\n"
   "  uint32x4_t wk = {a ^ b, a + b, b ^ a, (a & b) | 1u};\n"
   "  uint32x4_t r = vsha1mq_u32(abcd, e, wk);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0xDEADBEEFu, 0x01234567u}, A32CRYPTO},

  // ===== SHA1SU0 — SHA1 schedule update 0 =====
  {"sha1su0",
   "#include <arm_neon.h>\n"
   "unsigned sha1su0(unsigned a, unsigned b) {\n"
   "  uint32x4_t w0 = {a, b, 0x11u, 0x22u};\n"
   "  uint32x4_t w4 = {b, a, 0x33u, 0x44u};\n"
   "  uint32x4_t w8 = {0xAAu, 0xBBu, 0xCCu, 0xDEu};\n"
   "  uint32x4_t r = vsha1su0q_u32(w0, w4, w8);\n"
   "  /* Weighted (not plain XOR) so symmetric inputs can't cancel lanes. */\n"
   "  return r[0] + r[1]*3u + r[2]*5u + r[3]*7u;\n"
   "}\n",
   {0x12345678u, 0x87654321u}, A32CRYPTO},

  // ===== SHA1SU1 — SHA1 schedule update 1 =====
  {"sha1su1",
   "#include <arm_neon.h>\n"
   "unsigned sha1su1(unsigned a, unsigned b) {\n"
   "  uint32x4_t tw = {a, b, a ^ b, a + b};\n"
   "  uint32x4_t w12 = {0x10u, 0x20u, 0x30u, 0x40u};\n"
   "  uint32x4_t r = vsha1su1q_u32(tw, w12);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0x90ABCDEFu, 0x87654321u}, A32CRYPTO},

  // ===== SHA256H — SHA256 hash update (part 1) =====
  {"sha256h",
   "#include <arm_neon.h>\n"
   "unsigned sha256h(unsigned a, unsigned b) {\n"
   "  uint32x4_t abcd = {a, b, a ^ b, a + b};\n"
   "  uint32x4_t efgh = {b, a, a | 1u, b | 1u};\n"
   "  uint32x4_t wk = {(a ^ b) + 1u, a + b, b, a};\n"
   "  uint32x4_t r = vsha256hq_u32(abcd, efgh, wk);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0xADE682D1u, 0x2B3E6C1Fu}, A32CRYPTO},

  // ===== SHA256H2 — SHA256 hash update (part 2) =====
  {"sha256h2",
   "#include <arm_neon.h>\n"
   "unsigned sha256h2(unsigned a, unsigned b) {\n"
   "  uint32x4_t abcd = {a, b, b ^ a, a + b};\n"
   "  uint32x4_t efgh = {b, a, a ^ 1u, b ^ 1u};\n"
   "  uint32x4_t wk = {a + b, (a ^ b) | 1u, b, a};\n"
   "  uint32x4_t r = vsha256h2q_u32(abcd, efgh, wk);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0xF3BCC908u, 0x84CAA73Bu}, A32CRYPTO},

  // ===== SHA256SU0 — SHA256 schedule update 0 =====
  {"sha256su0",
   "#include <arm_neon.h>\n"
   "unsigned sha256su0(unsigned a, unsigned b) {\n"
   "  uint32x4_t w0 = {a, b, a ^ b, a + b};\n"
   "  uint32x4_t w4 = {b, a, a ^ b, a + b};\n"
   "  uint32x4_t r = vsha256su0q_u32(w0, w4);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0x71374491u, 0xB5C0FBCFu}, A32CRYPTO},

  // ===== SHA256SU1 — SHA256 schedule update 1 =====
  {"sha256su1",
   "#include <arm_neon.h>\n"
   "unsigned sha256su1(unsigned a, unsigned b) {\n"
   "  uint32x4_t tw = {a, b, a ^ b, a + b};\n"
   "  uint32x4_t w8 = {a, b, a ^ 1u, b ^ 1u};\n"
   "  uint32x4_t w12 = {b, a, a + b, a ^ b};\n"
   "  uint32x4_t r = vsha256su1q_u32(tw, w8, w12);\n"
   "  return r[0] ^ r[1] ^ r[2] ^ r[3];\n"
   "}\n",
   {0x885A308Du, 0xE0370734u}, A32CRYPTO},

};

// clang-format on

#undef A32CRYPTO

INSTANTIATE_TEST_SUITE_P(Crypto, ARM32CryptoRT,
                         ::testing::ValuesIn(kArm32Crypto),
                         [](const auto &P) { return P.param.Name; });
