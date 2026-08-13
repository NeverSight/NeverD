//===- X64_IncDecOverflowRTTests.cpp - INC/DEC OF snapshot -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the OF (overflow) flag of INC / DEC on sub-register
// (byte/word) destinations.  For a register operand DstR (read) and DstW
// (write) alias the same register, and LowToMed's sub-register handling
// redirects a *later* read of DstR to the post-update value.  INC/DEC compute
// OF as INT_SOVF/INT_SBOR(DstR, 1) AFTER the INT_ADD/INT_SUB has written
// the register, so OF was taken from the post-increment value:
//
//   incb 0x7F -> 0x80: real OF=1 (127+1 overflows signed byte), but
//     SCARRY(0x80, 1) = -128+1 = no overflow -> 0.
//
// The fix snapshots the source (the `PreVal` temp added for AF in the AF round)
// before the write and feeds it to the OF computation too.  AF already used the
// snapshot; this extends it to OF (INC/DEC) and CF/OF (NEG).
//
// Probes capture OF with SETO; the boundary values (INT_MAX/INT_MIN at each
// width) are exactly where the post-vs-pre value flips the overflow result.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64IncDecOverflowRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64IncDecOverflowRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // incb 0x7F -> 0x80: signed byte overflow, OF=1.  (old: OF from 0x80 -> 0)
  {"inc_of_byte",
   "long f(long a){unsigned int o;"
   "__asm__ volatile(\"movb %b1,%%al\\n\\tincb %%al\\n\\tseto %b0\""
   ":\"=&r\"(o):\"r\"(a):\"al\",\"cc\");return (long)(o&1);}\n",
   {0x7FULL}, "IncDecOvf", 0},

  // decb 0x80 -> 0x7F: signed byte underflow, OF=1.  (old: OF from 0x7F -> 0)
  {"dec_of_byte",
   "long f(long a){unsigned int o;"
   "__asm__ volatile(\"movb %b1,%%al\\n\\tdecb %%al\\n\\tseto %b0\""
   ":\"=&r\"(o):\"r\"(a):\"al\",\"cc\");return (long)(o&1);}\n",
   {0x80ULL}, "IncDecOvf", 0},

  // incw 0x7FFF -> 0x8000: signed word overflow, OF=1.  (old: OF from 0x8000 -> 0)
  {"inc_of_word",
   "long f(long a){unsigned int o;"
   "__asm__ volatile(\"movw %w1,%%ax\\n\\tincw %%ax\\n\\tseto %b0\""
   ":\"=&r\"(o):\"r\"(a):\"ax\",\"cc\");return (long)(o&1);}\n",
   {0x7FFFULL}, "IncDecOvf", 0},

  // Control: incb 0x05 -> 0x06, no overflow, OF=0 (same with/without fix).
  {"inc_of_clear",
   "long f(long a){unsigned int o;"
   "__asm__ volatile(\"movb %b1,%%al\\n\\tincb %%al\\n\\tseto %b0\""
   ":\"=&r\"(o):\"r\"(a):\"al\",\"cc\");return (long)(o&1);}\n",
   {0x05ULL}, "IncDecOvf", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(IncDecOvf, X64IncDecOverflowRT,
                         ::testing::ValuesIn(kX64), rtTCName);
