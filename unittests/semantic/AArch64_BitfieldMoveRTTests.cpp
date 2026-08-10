//===- AArch64_BitfieldMoveRTTests.cpp - BFM/UBFM/SBFM family RT -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64's bitfield-move family (the BFM/UBFM/SBFM primitives and their
// architectural aliases BFI/BFXIL, UBFX/UBFIZ, SBFX/SBFIZ, plus LSL/LSR/ASR)
// is alias-heavy and historically bug-prone — the source-less BFC alias was a
// silent no-op until #348-adjacent work (see AArch64_BitfieldClearRTTests).
// Two correctness aspects are classic "weak-test masking" blind spots:
//
//   1. BFI / BFXIL must PRESERVE the destination bits OUTSIDE the inserted
//      field (they are read-modify-write inserts).  If prior coverage only ran
//      them over a zero background, a clobbered preserve-region is invisible.
//      Here the destination is seeded with a non-zero, bit-rich value and the
//      WHOLE register is folded into the return.
//
//   2. SBFX / SBFIZ / ASR must SIGN-extend from the top of the extracted field.
//      With a field whose top bit is 0 the signed and unsigned results are
//      identical, hiding a zero-extend bug.  These probes pick fields whose top
//      bit is 1 so the sign fill is observable (negative results).
//
//   3. The 32-bit (Wd) forms must ZERO bits 63:32 of the X register (every W
//      write does).  Seeding the register non-zero first makes that observable.
//
// All forms are base ARMv8-A and native on the default Unicorn arm64 CPU; the
// handlers are believed correct, so this is a regression-locking guardrail
// round (cf. #345/#346/#326/#288).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64BitfieldMoveRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BitfieldMoveRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

// --- Insert (read-modify-write: preserve bits outside the field). ---
#define BFI_X(LSB,W) \
  "unsigned long f(unsigned long a, unsigned long b){unsigned long r=a;\n" \
  "  __asm__ volatile(\"bfi %0,%1,#" #LSB ",#" #W "\":\"+r\"(r):\"r\"(b));\n" \
  "  return r;}\n"
#define BFI_W(LSB,W) \
  "unsigned long f(unsigned long a, unsigned long b){unsigned long r=a;\n" \
  "  __asm__ volatile(\"bfi %w0,%w1,#" #LSB ",#" #W "\":\"+r\"(r):\"r\"(b));\n" \
  "  return r;}\n"
#define BFXIL_X(LSB,W) \
  "unsigned long f(unsigned long a, unsigned long b){unsigned long r=a;\n" \
  "  __asm__ volatile(\"bfxil %0,%1,#" #LSB ",#" #W "\":\"+r\"(r):\"r\"(b));\n" \
  "  return r;}\n"

// --- Unsigned extract (zero-extend). ---
#define UBFX_X(LSB,W) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"ubfx %0,%1,#" #LSB ",#" #W "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"
#define UBFIZ_X(LSB,W) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"ubfiz %0,%1,#" #LSB ",#" #W "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"

// --- Signed extract (sign-extend from field top). ---
#define SBFX_X(LSB,W) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"sbfx %0,%1,#" #LSB ",#" #W "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"
#define SBFIZ_X(LSB,W) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"sbfiz %0,%1,#" #LSB ",#" #W "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"
#define SBFX_W(LSB,W) \
  "unsigned long f(unsigned long a){unsigned long r;\n" \
  "  __asm__ volatile(\"sbfx %w0,%w1,#" #LSB ",#" #W "\":\"=r\"(r):\"r\"(a));\n" \
  "  return r;}\n"

static const unsigned long long SEED = 0x1122334455667788ULL;
static const unsigned long long ONES = 0xFFFFFFFFFFFFFFFFULL;

static const std::vector<RoundTripTC> kA64 = {
  // ===== BFI: insert field, PRESERVE the rest of the seeded register. =====
  {"bfi_x_lo",   BFI_X(0,16),  {SEED, ONES}, "BFMove"},
  {"bfi_x_mid",  BFI_X(8,16),  {SEED, ONES}, "BFMove"},
  {"bfi_x_high", BFI_X(40,16), {SEED, ONES}, "BFMove"},
  {"bfi_x_1bit", BFI_X(63,1),  {SEED, 0x1ULL}, "BFMove"},
  // Wd form: bits 63:32 of X must be zeroed; in-W field inserted, rest of W kept.
  {"bfi_w_mid",  BFI_W(8,12),  {SEED, ONES}, "BFMove"},

  // ===== BFXIL: insert low field, preserve dst high bits. =====
  {"bfxil_x_lo16",  BFXIL_X(16,16), {SEED, 0xAAAABBBBCCCCDDDDULL}, "BFMove"},
  {"bfxil_x_lo32",  BFXIL_X(0,32),  {SEED, 0xAAAABBBBCCCCDDDDULL}, "BFMove"},

  // ===== UBFX: unsigned extract (zero-extended). =====
  {"ubfx_x_mid",  UBFX_X(8,16),  {0xFFEEDDCCBBAA9988ULL}, "BFMove"},
  {"ubfx_x_top",  UBFX_X(48,16), {0xFFEEDDCCBBAA9988ULL}, "BFMove"},

  // ===== UBFIZ: zero-extend field then shift into position. =====
  {"ubfiz_x",     UBFIZ_X(20,12), {0x0000000000000FFFULL}, "BFMove"},

  // ===== SBFX: signed extract — field top bit set -> NEGATIVE result. =====
  // Field [8+15:8] of 0x...FF80.. has top bit 1 -> sign-extends to all-ones high.
  {"sbfx_x_neg",  SBFX_X(8,16),  {0x0000000000FF8000ULL}, "BFMove"},
  {"sbfx_x_pos",  SBFX_X(8,16),  {0x0000000000123400ULL}, "BFMove"},
  {"sbfx_x_byte", SBFX_X(24,8),  {0x00000000F0000000ULL}, "BFMove"},
  // Wd form: sign-extend within W, then bits 63:32 zeroed.
  {"sbfx_w_neg",  SBFX_W(4,8),   {0x0000000000000F80ULL}, "BFMove"},

  // ===== SBFIZ: sign-extend field then shift into position. =====
  {"sbfiz_x",     SBFIZ_X(16,8), {0x00000000000000C0ULL}, "BFMove"},
  {"sbfiz_x_pos", SBFIZ_X(16,8), {0x0000000000000030ULL}, "BFMove"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BFMove, A64BitfieldMoveRT,
                         ::testing::ValuesIn(kA64), rtTCName);
