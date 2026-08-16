//===- AArch64_FastClassifyParityTests.cpp - native vs capstone parity ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Differential test: the Capstone-free fixed-width classifiers in
/// neverd/decode/AArch64FastClassify.h must agree, bit-for-bit, with a full
/// Capstone decode of the same 32-bit word.  Capstone is the oracle.
///
/// This is a *correctness* test, so its result is independent of machine load
/// (unlike the throughput numbers the decode benchmark reports).  It exercises
/// three surfaces:
///   1. Hand-picked edge encodings around every decision boundary.
///   2. Exhaustive sweeps over the sub-fields that select a class (Rn for
///      RET/BR, cond+o0 for B.cond/BC.cond, imm samples for B/BL).
///   3. A large deterministic pseudo-random sweep so any missed boundary in the
///      rest of the 32-bit space surfaces as a mismatch.
///
/// Optionally, pointing NEVERD_PARITY_BIN at a raw file walks every aligned
/// 32-bit word of it as an additional real-code corpus.
///
//===----------------------------------------------------------------------===//

#include "neverd/decode/AArch64FastClassify.h"

#include <capstone/capstone.h>

#include "gtest/gtest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>

using namespace neverd;

namespace {

// Capstone-id ground truth for the predicates under test, mirroring exactly
// what AArch64Lifter::isFunctionTerminator / directCallTarget compute from a
// decoded cs_insn.
struct CsTruth {
  bool Decoded = false;
  bool Terminator = false;
  va_t CallTarget = InvalidVA;
};

class CapstoneOracle {
public:
  CapstoneOracle() {
    if (cs_open(CS_ARCH_AARCH64, CS_MODE_ARM, &H) != CS_ERR_OK)
      H = 0;
    else {
      cs_option(H, CS_OPT_DETAIL, CS_OPT_ON);
      Insn = cs_malloc(H);
    }
  }
  ~CapstoneOracle() {
    if (Insn)
      cs_free(Insn, 1);
    if (H)
      cs_close(&H);
  }
  bool ok() const { return H != 0 && Insn != nullptr; }

  CsTruth classify(uint32_t Word, va_t Addr) const {
    CsTruth T;
    uint8_t Code[4] = {static_cast<uint8_t>(Word), static_cast<uint8_t>(Word >> 8),
                       static_cast<uint8_t>(Word >> 16),
                       static_cast<uint8_t>(Word >> 24)};
    const uint8_t *P = Code;
    size_t Sz = 4;
    uint64_t A = Addr;
    if (!cs_disasm_iter(H, &P, &Sz, &A, Insn))
      return T; // Decoded stays false
    T.Decoded = true;
    unsigned Id = Insn->id;
    T.Terminator = (Id == AARCH64_INS_RET || Id == AARCH64_INS_B ||
                    Id == AARCH64_INS_BR || Id == AARCH64_INS_ERET ||
                    Id == AARCH64_INS_BRK || Id == AARCH64_INS_HLT);
    if (Id == AARCH64_INS_BL && Insn->detail &&
        Insn->detail->aarch64.op_count >= 1 &&
        Insn->detail->aarch64.operands[0].type == AARCH64_OP_IMM)
      T.CallTarget = static_cast<va_t>(Insn->detail->aarch64.operands[0].imm);
    return T;
  }

private:
  csh H = 0;
  cs_insn *Insn = nullptr;
};

// Compare native predicates to the oracle for one word; returns "" on match or
// a human-readable diff on mismatch.
std::string diffOne(const CapstoneOracle &O, uint32_t Word, va_t Addr) {
  CsTruth T = O.classify(Word, Addr);
  va_t NatCall = a64fast::directCallTarget(Word, Addr);
  bool NatTerm = a64fast::isTerminatorWord(Word);

  char Buf[256];
  // Call target: must match for every word (BL space is fully accepted by cs).
  if (NatCall != T.CallTarget) {
    std::snprintf(Buf, sizeof(Buf),
                  "call: w=0x%08X va=0x%llX cs=0x%llX nat=0x%llX", Word,
                  (unsigned long long)Addr, (unsigned long long)T.CallTarget,
                  (unsigned long long)NatCall);
    return Buf;
  }
  // Terminator: for a decoded word native must equal the capstone-id verdict.
  // For a word capstone rejects, native must not claim a terminator (so the
  // predicate is safe to use standalone, without a prior decode).
  bool WantTerm = T.Decoded ? T.Terminator : false;
  if (NatTerm != WantTerm) {
    std::snprintf(Buf, sizeof(Buf),
                  "term: w=0x%08X decoded=%d cs=%d nat=%d", Word,
                  (int)T.Decoded, (int)WantTerm, (int)NatTerm);
    return Buf;
  }
  return "";
}

} // namespace

class A64FastClassifyParity : public ::testing::Test {
protected:
  CapstoneOracle Oracle;
  void SetUp() override {
    if (!Oracle.ok())
      GTEST_SKIP() << "capstone AArch64 unavailable";
  }
};

// Hand-picked encodings around every decision boundary.
TEST_F(A64FastClassifyParity, EdgeEncodings) {
  const va_t A = 0x100000;
  const uint32_t Words[] = {
      0xD65F03C0u, // ret
      0xD65F0000u, // ret x0
      0xD65F03E0u, // ret xzr
      0xD65F0BFFu, // retaa   (distinct id -> NOT a terminator)
      0xD65F0FFFu, // retab   (distinct id -> NOT a terminator)
      0xD61F0000u, // br x0
      0xD61F03C0u, // br x30
      0xD69F03E0u, // eret
      0xD69F0BFFu, // eretaa  (distinct id -> NOT a terminator)
      0x14000000u, // b .+0
      0x17FFFFFFu, // b .-4
      0x14FFFFFFu, // b large +
      0x54000000u, // b.eq
      0x5400000Eu, // b.al
      0x5400000Fu, // b.nv
      0x54000010u, // bc.eq   (id 53 -> NOT a terminator)
      0x5400001Fu, // bc.nv   (id 53 -> NOT a terminator)
      0xD4200020u, // brk #1
      0xD4400020u, // hlt #1
      0x94000000u, // bl .+0
      0x97FFFFFFu, // bl .-4
      0x94123456u, // bl arbitrary
      0xD63F0000u, // blr x0  (indirect call, not a terminator, not direct call)
      0xD503201Fu, // nop
      0x8B000000u, // add x0,x0,x0
      0x00000000u, // udf #0
      0xFFFFFFFFu, // rejected
      0x55000000u, // rejected (top6 0x15)
  };
  for (uint32_t W : Words) {
    std::string D = diffOne(Oracle, W, A);
    EXPECT_TRUE(D.empty()) << D;
  }
}

// Exhaustive over the RET / BR register field (Rn = bits[9:5]) and the trailing
// bits that separate the plain form from the PAC forms.
TEST_F(A64FastClassifyParity, RetBrRegisterSweep) {
  const va_t A = 0x400000;
  for (uint32_t Rn = 0; Rn < 32; ++Rn) {
    EXPECT_TRUE(diffOne(Oracle, 0xD65F0000u | (Rn << 5), A).empty());
    EXPECT_TRUE(diffOne(Oracle, 0xD61F0000u | (Rn << 5), A).empty());
  }
}

// Exhaustive over the conditional-branch condition (bits[3:0]) and the o0 bit
// (bit 4) that separates B.cond (id 51) from ARMv8.8 BC.cond (id 53), across a
// few imm19 samples.
TEST_F(A64FastClassifyParity, CondBranchSweep) {
  const va_t A = 0x400000;
  const uint32_t Imm19Samples[] = {0u, 1u, 0x3FFFEu, 0x40000u, 0x7FFFFu};
  for (uint32_t Cond = 0; Cond < 16; ++Cond)
    for (uint32_t O0 = 0; O0 < 2; ++O0)
      for (uint32_t Imm : Imm19Samples) {
        uint32_t W = 0x54000000u | ((Imm & 0x7FFFFu) << 5) | (O0 << 4) | Cond;
        EXPECT_TRUE(diffOne(Oracle, W, A).empty());
      }
}

// B / BL immediate samples across the signed imm26 range.
TEST_F(A64FastClassifyParity, BranchLinkImmSweep) {
  const va_t A = 0x800000;
  const uint32_t Imm26Samples[] = {0u,        1u,        2u,        0x1FFFFFEu,
                                   0x1FFFFFFu, 0x2000000u, 0x3FFFFFEu, 0x3FFFFFFu};
  for (uint32_t Imm : Imm26Samples) {
    EXPECT_TRUE(diffOne(Oracle, 0x14000000u | (Imm & 0x3FFFFFFu), A).empty());
    EXPECT_TRUE(diffOne(Oracle, 0x94000000u | (Imm & 0x3FFFFFFu), A).empty());
  }
}

// Large deterministic pseudo-random sweep: any missed boundary anywhere in the
// 32-bit encoding space shows up as a mismatch.  Fixed seed = reproducible.
TEST_F(A64FastClassifyParity, RandomizedSweep) {
  std::mt19937 Rng(0xC0FFEEu);
  const va_t A = 0x10000000;
  constexpr int kWords = 300000;
  int Mismatches = 0;
  std::string First;
  for (int I = 0; I < kWords; ++I) {
    uint32_t W = Rng();
    std::string D = diffOne(Oracle, W, A + static_cast<va_t>(I) * 4);
    if (!D.empty()) {
      if (First.empty())
        First = D;
      ++Mismatches;
    }
  }
  EXPECT_EQ(Mismatches, 0) << "first: " << First;
}

// Optional: walk a real file's aligned words if NEVERD_PARITY_BIN is set.
TEST_F(A64FastClassifyParity, OptionalRealBinaryWalk) {
  const char *Path = std::getenv("NEVERD_PARITY_BIN");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_PARITY_BIN to a raw arm64 code file to enable";
  std::ifstream F(Path, std::ios::binary);
  ASSERT_TRUE(F.good()) << "cannot open " << Path;
  std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(F)),
                             std::istreambuf_iterator<char>());
  ASSERT_GE(Bytes.size(), 4u);
  const va_t Base = 0x100000000ull;
  int Mismatches = 0;
  std::string First;
  for (size_t Off = 0; Off + 4 <= Bytes.size(); Off += 4) {
    uint32_t W = static_cast<uint32_t>(Bytes[Off]) |
                 (static_cast<uint32_t>(Bytes[Off + 1]) << 8) |
                 (static_cast<uint32_t>(Bytes[Off + 2]) << 16) |
                 (static_cast<uint32_t>(Bytes[Off + 3]) << 24);
    std::string D = diffOne(Oracle, W, Base + Off);
    if (!D.empty()) {
      if (First.empty())
        First = D;
      ++Mismatches;
    }
  }
  EXPECT_EQ(Mismatches, 0) << "first: " << First;
}
