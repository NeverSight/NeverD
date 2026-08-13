//===- AArch64_NativeDecodeParitySweepTests.cpp - randomized and real binary parity -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AArch64_NativeDecodeParityTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::a64_parity_test;

// begin() resets only the header of the reused cs_aarch64 and leaves the
// 16-slot operands[] array untouched (each decode fully writes operands[0,
// op_count)).  Decode a long, deterministic stream of every covered class into
// ONE poisoned, reused buffer and prove each still lifts identically to a fresh
// Capstone decode — i.e. no stale operand from a prior word ever leaks past the
// current word's op_count.  A regression that dropped the per-class operand
// init (or read past op_count) fails here while the fresh-buffer sweeps pass.
TEST_F(A64NativeParity, ReusedBufferNoStaleOperandLeak) {
  cs_insn NI{};
  cs_detail ND{};
  const va_t A = 0x1230000;
  std::mt19937 Rng(0xC0FFEEu);
  int Handled = 0;
  std::string First;
  // Interleave low- and high-op-count classes so a leaked slot from a 4-operand
  // decode (bitfield/extr/madd) would corrupt a following 2-operand decode.
  const uint32_t Seeds[] = {
      0x8B000000u, // add  x0, x0, x0            (3 ops)
      0xF9400000u, // ldr  x0, [x0]              (2 ops)
      0x93407C00u, // sxtw x0, w0                (2 ops, extend alias)
      0x9B007C00u, // mul  x0, x0, x0            (3 ops, madd alias)
      0x13007C00u, // extr/asr family            (varies)
      0xD65F03C0u, // ret                        (0 ops)
      0x14000000u, // b .                        (1 op)
      0xB3400000u, // bfxil/bfi                  (4 ops)
      0xD2800000u, // movz x0, #0                (2 ops)
      0x1A800400u, // csel/cinc family           (2-3 ops)
      0x8B220000u, // add x0, x0, w0, uxtb       (3 ops, extended reg)
      0x9B220000u, // smaddl/smull               (3-4 ops, widening)
      0x1E204000u, // fmov s0, s0                (2 ops, fp reg)
      0xB2400000u, // orr/mov (bitmask)          (2-3 ops)
  };
  for (int Iter = 0; Iter < 40000; ++Iter) {
    uint32_t Base = Seeds[Iter % (sizeof(Seeds) / sizeof(Seeds[0]))];
    // Perturb register/immediate fields but keep the class selector bits.
    uint32_t W = Base ^ (Rng() & 0x001FFFE0u);
    va_t Va = A + static_cast<va_t>(Iter) * 4;
    cs_insn Probe{};
    cs_detail PD{};
    if (a64native::tryDecode(W, Va, Probe, PD))
      ++Handled;
    std::string D = checkOneReuse(O, NI, ND, W, Va);
    if (!D.empty() && First.empty())
      First = D;
    EXPECT_TRUE(D.empty()) << D;
    if (!First.empty())
      break;
  }
  EXPECT_GT(Handled, 0);
}

// Any missed boundary anywhere in the 32-bit space surfaces as a mismatch or a
// strict-subset violation.  Fixed seed = reproducible.
TEST_F(A64NativeParity, RandomizedSweep) {
  std::mt19937 Rng(0x5EED1234u);
  const va_t Base = 0x40000000;
  constexpr int kWords = 400000;
  int Handled = 0, Mismatch = 0;
  std::string First;
  for (int I = 0; I < kWords; ++I) {
    uint32_t W = Rng();
    va_t A = Base + static_cast<va_t>(I) * 4;
    cs_insn NI{};
    cs_detail ND{};
    if (a64native::tryDecode(W, A, NI, ND))
      ++Handled;
    std::string D = checkOne(O, W, A);
    if (!D.empty()) {
      if (First.empty())
        First = D;
      ++Mismatch;
    }
  }
  EXPECT_EQ(Mismatch, 0) << "first: " << First;
  // Sanity: the random space should hit the covered classes many times, so a
  // zero here would mean the fast path silently stopped matching anything.
  EXPECT_GT(Handled, 0);
}

// Optional real-code corpus: NEVERD_PARITY_BIN=<raw arm64 bytes>.
TEST_F(A64NativeParity, OptionalRealBinaryWalk) {
  const char *Path = std::getenv("NEVERD_PARITY_BIN");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_PARITY_BIN to a raw arm64 code file to enable";
  std::ifstream F(Path, std::ios::binary);
  ASSERT_TRUE(F.good()) << "cannot open " << Path;
  std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(F)),
                             std::istreambuf_iterator<char>());
  ASSERT_GE(Bytes.size(), 4u);
  const va_t Base = 0x100000000ull;
  int Handled = 0, Mismatch = 0;
  std::string First;
  for (size_t Off = 0; Off + 4 <= Bytes.size(); Off += 4) {
    uint32_t W = static_cast<uint32_t>(Bytes[Off]) |
                 (static_cast<uint32_t>(Bytes[Off + 1]) << 8) |
                 (static_cast<uint32_t>(Bytes[Off + 2]) << 16) |
                 (static_cast<uint32_t>(Bytes[Off + 3]) << 24);
    va_t A = Base + Off;
    cs_insn NI{};
    cs_detail ND{};
    if (a64native::tryDecode(W, A, NI, ND))
      ++Handled;
    std::string D = checkOne(O, W, A);
    if (!D.empty()) {
      if (First.empty())
        First = D;
      ++Mismatch;
    }
  }
  EXPECT_EQ(Mismatch, 0) << "first: " << First;
  EXPECT_GT(Handled, 0) << "no covered instructions in corpus?";
}

} // namespace
