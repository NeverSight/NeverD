//===- ARM32_Crc32RTTests.cpp - ARMv8 AArch32 CRC32 roundtrip -------------===//
//
// Covers the ARMv8-A AArch32 CRC32 extension: CRC32B/H/W and the Castagnoli
// CRC32CB/CH/CW.  Each accumulates a polynomial checksum (Rd = crc(Rn, Rm)),
// not a plain XOR, so it must keep the real instruction end-to-end.
//
// The lifter previously emitted the CRC32 intrinsic with no operands at all
// (no destination, no inputs) and the ARM backend had no handler, so every
// CRC32 call folded the whole function to `ret 0`.  These probes accumulate
// over a derived byte/halfword/word stream and fold the checksum into the
// return value so a dropped instruction is observable.
//
// The default ARM32 fixture pins cortex-a15 (ARMv7, no CRC), so these compile
// with an ARMv8 AArch32 target and run under Unicorn's MAX CPU model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32Crc32RT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Crc32RT, Verify) { roundTripARM32(GetParam()); }

// ARMv8 AArch32 CRC32 needs an ARMv8 target (+crc) + the MAX CPU for emulation.
#define A32CRC "Crc32", 1, "-march=armv8-a+crc", false, \
  "armv8-linux-gnueabihf", UC_CPU_ARM_MAX

// clang-format off

static const std::vector<RoundTripTC> kArm32Crc = {

  // CRC32B — accumulate one byte per step.
  {"crc32b",
   "unsigned crc32b(unsigned a, unsigned b) {\n"
   "  unsigned crc = a;\n"
   "  for (int i = 0; i < 32; i++)\n"
   "    crc = __builtin_arm_crc32b(crc, (unsigned char)(b * (i + 1) + i * 7));\n"
   "  return crc;\n"
   "}\n",
   {0xFFFFFFFFu, 0x89ABCDEFu}, A32CRC},

  // CRC32H — accumulate one halfword per step.
  {"crc32h",
   "unsigned crc32h(unsigned a, unsigned b) {\n"
   "  unsigned crc = a;\n"
   "  for (int i = 0; i < 24; i++)\n"
   "    crc = __builtin_arm_crc32h(crc, (unsigned short)(b * (i + 3) + i * 131));\n"
   "  return crc;\n"
   "}\n",
   {0x12345678u, 0x76543210u}, A32CRC},

  // CRC32W — accumulate one word per step.
  {"crc32w",
   "unsigned crc32w(unsigned a, unsigned b) {\n"
   "  unsigned crc = a;\n"
   "  for (int i = 0; i < 16; i++)\n"
   "    crc = __builtin_arm_crc32w(crc, b * 2654435761u + (unsigned)i * 40503u);\n"
   "  return crc;\n"
   "}\n",
   {0xCAFEBABEu, 0xC0FFEE11u}, A32CRC},

  // CRC32CB — Castagnoli byte.
  {"crc32cb",
   "unsigned crc32cb(unsigned a, unsigned b) {\n"
   "  unsigned crc = a;\n"
   "  for (int i = 0; i < 32; i++)\n"
   "    crc = __builtin_arm_crc32cb(crc, (unsigned char)(b + i * 29));\n"
   "  return crc;\n"
   "}\n",
   {0x00000000u, 0xDEADBEEFu}, A32CRC},

  // CRC32CH — Castagnoli halfword.
  {"crc32ch",
   "unsigned crc32ch(unsigned a, unsigned b) {\n"
   "  unsigned crc = a;\n"
   "  for (int i = 0; i < 24; i++)\n"
   "    crc = __builtin_arm_crc32ch(crc, (unsigned short)(b ^ (i * 0x9E37u)));\n"
   "  return crc;\n"
   "}\n",
   {0xA5A5A5A5u, 0x5A5A5A5Au}, A32CRC},

  // CRC32CW — Castagnoli word.
  {"crc32cw",
   "unsigned crc32cw(unsigned a, unsigned b) {\n"
   "  unsigned crc = a;\n"
   "  for (int i = 0; i < 16; i++)\n"
   "    crc = __builtin_arm_crc32cw(crc, b + (unsigned)i * 0x01000193u);\n"
   "  return crc;\n"
   "}\n",
   {0x1EDC6F41u, 0x12345678u}, A32CRC},

  // Mixed b/h/w + Castagnoli chain so a single dropped variant is caught.
  {"crc32_mixed",
   "unsigned crc32_mixed(unsigned a, unsigned b) {\n"
   "  unsigned c = a;\n"
   "  c = __builtin_arm_crc32b(c, (unsigned char)b);\n"
   "  c = __builtin_arm_crc32h(c, (unsigned short)(b >> 4));\n"
   "  c = __builtin_arm_crc32w(c, b ^ a);\n"
   "  c = __builtin_arm_crc32cb(c, (unsigned char)(b >> 16));\n"
   "  c = __builtin_arm_crc32ch(c, (unsigned short)(a >> 8));\n"
   "  c = __builtin_arm_crc32cw(c, a + b);\n"
   "  return c;\n"
   "}\n",
   {0x9B05688Cu, 0x1F83D9ABu}, A32CRC},
};

// clang-format on

#undef A32CRC

INSTANTIATE_TEST_SUITE_P(Crc32, ARM32Crc32RT, ::testing::ValuesIn(kArm32Crc),
                         rtTCName);
