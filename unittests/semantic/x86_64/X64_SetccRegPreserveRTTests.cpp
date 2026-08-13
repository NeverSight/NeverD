//===- X64_SetccRegPreserveRTTests.cpp - SETcc upper-byte preserve -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// `SETcc r/m8` writes a single BYTE (0 or 1) to its destination.  For a
// register destination it targets the 8-bit sub-register (AL/BL/.../R15B) and
// MUST leave the enclosing 16/32/64-bit register's other bytes UNTOUCHED — an
// 8-bit register write does NOT zero the upper bits (unlike a 32-bit write,
// which zeroes bits 63:32 of the 64-bit register).
//
// Every existing SETcc test (X64_SetccCmovRTTests) feeds the lifter the
// *compiler-generated* idiom `return a==b ? 1 : 0;`, which emits
// `xor eax,eax; setcc al` (or `setcc al; movzx eax,al`) — i.e. the upper bits
// are ALWAYS already zeroed by a separate instruction, or the byte is
// immediately zero-extended.  That makes a SETcc handler that *wrongly clobbers
// the upper bytes* completely invisible: the classic "weak-test masking" blind
// spot this project keeps hitting (#326/#344/#345 ...).
//
// These probes seed the destination register's full 64 bits with a non-zero,
// upper-rich value, run `setcc` on its LOW BYTE only, then fold the WHOLE
// register into the return value.  Ground truth is taken from the original
// program under Unicorn (which preserves the upper 56 bits), so the lifted
// result diverges if the handler zero-extends the byte into the 32-bit view.
//
// NOTE: because the fixture compares original-Unicorn vs lifted-Unicorn, the
// probes are robust to AT&T operand-order / condition-polarity confusion — the
// upper bits are non-zero regardless of whether the low byte ends up 0 or 1, so
// a handler that drops them ALWAYS diverges.  Inputs are still chosen to mix
// low-byte 0 and 1 outcomes so the value path is exercised too.
//
// All forms are baseline x86-64 and native on the default Unicorn CPU; this is
// a lift-correctness round (register path: BUG; memory path: guardrail).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SetccRegPreserveRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SetccRegPreserveRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

// cmp %2,%1 (= a - b) sets flags; setMN writes ONLY the low byte of r (whose
// upper 56 bits hold a's upper bits); the full register is returned.
#define SETCC_KEEP(MN) \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  unsigned long r = a;\n" \
  "  __asm__ volatile(\"cmp %2, %1\\n\\t" MN " %b0\"\n" \
  "                   : \"+r\"(r) : \"r\"(a), \"r\"(b) : \"cc\");\n" \
  "  return r;\n}\n"

// Memory destination (the already-correct storeToMem path): seed an 8-byte
// stack slot, write ONE byte via setcc, fold all 8 bytes — guards that the
// adjacent 7 bytes are preserved.
#define SETCC_MEM_KEEP(MN) \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  union { unsigned long u; unsigned char c[8]; } v;\n" \
  "  v.u = a;\n" \
  "  __asm__ volatile(\"cmp %2, %1\\n\\t" MN " %0\"\n" \
  "                   : \"=m\"(v.c[0]) : \"r\"(a), \"r\"(b) : \"cc\");\n" \
  "  return v.u;\n}\n"

static const unsigned long long SEED = 0x1122334455667788ULL;

static const std::vector<RoundTripTC> kX64 = {
  // ===== Register destination: upper bytes MUST be preserved. =====
  // a != b  -> low byte 1, upper = SEED upper.
  {"setne_keep_one",  SETCC_KEEP("setne"), {SEED, 0xFFULL}, "SetccKeep"},
  // a == b  -> setne low byte 0, upper preserved (ret = SEED & ~0xFF).
  {"setne_keep_zero", SETCC_KEEP("setne"), {SEED, SEED},    "SetccKeep"},
  {"sete_keep_one",   SETCC_KEEP("sete"),  {SEED, SEED},    "SetccKeep"},
  {"sete_keep_zero",  SETCC_KEEP("sete"),  {SEED, 0xFFULL}, "SetccKeep"},

  // Unsigned: a < b (setb), a > b (seta), a <= b (setbe).
  {"setb_keep",  SETCC_KEEP("setb"),  {SEED, 0xFFFFFFFFFFFFFFFFULL}, "SetccKeep"},
  {"seta_keep",  SETCC_KEEP("seta"),  {SEED, 0x0ULL},                "SetccKeep"},
  {"setbe_keep", SETCC_KEEP("setbe"), {SEED, SEED},                  "SetccKeep"},

  // Signed: a < b (setl), a > b (setg), a <= b (setle), a >= b (setge).
  {"setl_keep",  SETCC_KEEP("setl"),  {SEED, 0x7FFFFFFFFFFFFFFFULL}, "SetccKeep"},
  {"setg_keep",  SETCC_KEEP("setg"),  {SEED, 0x0ULL},                "SetccKeep"},
  {"setle_keep", SETCC_KEEP("setle"), {SEED, SEED},                  "SetccKeep"},
  {"setge_keep", SETCC_KEEP("setge"), {SEED, SEED},                  "SetccKeep"},

  // Sign / not-sign of (a - b).
  {"sets_keep",  SETCC_KEEP("sets"),  {SEED, 0x2000000000000000ULL}, "SetccKeep"},
  {"setns_keep", SETCC_KEEP("setns"), {SEED, 0x0ULL},                "SetccKeep"},

  // Overflow / not-overflow of (a - b).  0x7F..F - (-1) overflows signed.
  {"seto_keep",  SETCC_KEEP("seto"),  {0x7122334455667788ULL, 0x8000000000000000ULL}, "SetccKeep"},

  // Parity of low byte of (a - b).
  {"setp_keep",  SETCC_KEEP("setp"),  {SEED, 0x1ULL}, "SetccKeep"},

  // ===== Memory destination guardrail: adjacent bytes preserved. =====
  {"setne_mem_keep", SETCC_MEM_KEEP("setne"), {SEED, 0xFFULL}, "SetccKeep"},
  {"sete_mem_keep",  SETCC_MEM_KEEP("sete"),  {SEED, SEED},    "SetccKeep"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(SetccKeep, X64SetccRegPreserveRT,
                         ::testing::ValuesIn(kX64), rtTCName);
