//===- X64_MemForwardRTTests.cpp - store-to-load forwarding ------*- C++ -*-===//
//
// Stress the MedIR memory model / store-to-load forwarding across mismatched
// access sizes: a wide store followed by a narrower load at an offset, several
// narrow stores feeding a wide load, and a narrow store under a wider load
// (which must NOT be forwarded as if it covered the whole region).  The exact
// store/load stream is pinned with inline asm so clang cannot fold it away.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MemForwardRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MemForwardRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // Store 8 bytes, load the byte at offset 3.  a=0x1122334455667788 -> 0x55.
  {"store_q_load_b3",
   "long f(long a){unsigned long buf;unsigned char r;"
   "__asm__ volatile(\"movq %1,(%2)\\n\\tmovzbl 3(%2),%k0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(&buf):\"memory\");return (long)r;}\n",
   {0x1122334455667788ULL}, "MemForward", 0},

  // Store 8 bytes, load the word at offset 2.  -> 0x4455.
  {"store_q_load_w2",
   "long f(long a){unsigned long buf;unsigned short r;"
   "__asm__ volatile(\"movq %1,(%2)\\n\\tmovzwl 2(%2),%k0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(&buf):\"memory\");return (long)r;}\n",
   {0x1122334455667788ULL}, "MemForward", 0},

  // Store 8 bytes, load the dword at offset 4.  -> 0x11223344.
  {"store_q_load_d4",
   "long f(long a){unsigned long buf;unsigned int r;"
   "__asm__ volatile(\"movq %1,(%2)\\n\\tmovl 4(%2),%k0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(&buf):\"memory\");return (long)r;}\n",
   {0x1122334455667788ULL}, "MemForward", 0},

  // Two dword stores feeding one qword load.  lo=0xDEADBEEF, hi=0xCAFEBABE.
  {"store_dd_load_q",
   "long f(long a,long b){unsigned long buf;unsigned long r;"
   "__asm__ volatile(\"movl %k1,(%3)\\n\\tmovl %k2,4(%3)\\n\\tmovq (%3),%0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(b),\"r\"(&buf):\"memory\");return (long)r;}\n",
   {0xDEADBEEFULL, 0xCAFEBABEULL}, "MemForward", 0},

  // Wide store, then overwrite the low byte, then wide load (partial overwrite
  // must merge, not drop either store).
  {"store_q_over_b0_load_q",
   "long f(long a,long b){unsigned long buf;unsigned long r;"
   "__asm__ volatile(\"movq %1,(%3)\\n\\tmovb %b2,(%3)\\n\\tmovq (%3),%0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(b),\"r\"(&buf):\"memory\");return (long)r;}\n",
   {0x1122334455667788ULL, 0xAA}, "MemForward", 0},

  // Narrow store under a wider load: buf is pre-seeded with a known qword, then
  // only byte 0 is overwritten; the qword load must see seed[63:8] | newbyte.
  {"store_b_under_load_q",
   "long f(long a){unsigned long buf;unsigned long r;unsigned long seed=0x99AABBCCDDEEFF00ULL;"
   "__asm__ volatile(\"movq %2,(%3)\\n\\tmovb %b1,(%3)\\n\\tmovq (%3),%0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(seed),\"r\"(&buf):\"memory\");return (long)r;}\n",
   {0x42}, "MemForward", 0},

  // Store qword at +0 and a byte at +8, load qword at +4 (straddles both).
  {"store_straddle_load",
   "long f(long a,long b){unsigned char buf[16];unsigned long r;"
   "__asm__ volatile(\"movq %1,(%3)\\n\\tmovb %b2,8(%3)\\n\\tmovq 4(%3),%0\""
   ":\"=&r\"(r):\"r\"(a),\"r\"(b),\"r\"(buf):\"memory\");return (long)r;}\n",
   {0x1122334455667788ULL, 0x77}, "MemForward", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(MemForward, X64MemForwardRT, ::testing::ValuesIn(kX64),
                         rtTCName);
