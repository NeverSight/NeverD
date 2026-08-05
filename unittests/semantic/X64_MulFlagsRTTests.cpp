//===- X64_MulFlagsRTTests.cpp - one-operand MUL/IMUL CF/OF ------*- C++ -*-=//
//
// x86 one-operand `MUL`/`IMUL` (the widening forms: AX, DX:AX, EDX:EAX,
// RDX:RAX) define ONLY CF and OF; SF/ZF/AF/PF are architecturally undefined.
// The defined rule differs between the unsigned and signed forms:
//
//   MUL  r/m :  CF = OF = (high half != 0)
//                 -> the full product does not fit in the low half (unsigned)
//   IMUL r/m :  CF = OF = (high half != SignExtend(low half))
//                 -> the full product does not fit in the low half (signed)
//
// Two traps live here.  (1) The unsigned vs signed CF rule: a lifter that
// reuses "high != 0" for IMUL is wrong whenever the product is negative (its
// high half is all-ones, not zero), and a lifter that reuses "high !=
// sext(low)" for MUL is wrong for large unsigned products whose low half has
// its top bit set.  (2) For the 8-bit form the product lands in AX (no DX), so
// the "high half" is AH, not DX — an off-by-register split mis-reads it.
//
// The widening value splits (AX / DX:AX) are pinned by
// X64_PartialRegMulDivRTTests, but NOTHING reads CF/OF after these, and nothing
// drives the 64-bit one-operand form whose 128-bit product is the only path
// that materializes RDX:RAX from a true i128 multiply.  Each probe seeds the
// fit/overflow boundary, captures CF (or OF via seto) with setcc, and folds it
// together with the low and high product halves so a wrong CF polarity, a
// signed/unsigned flag-rule swap, an off-by-register high half, or a botched
// 128-bit product all diverge from the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MulFlagsRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MulFlagsRT, Verify) { roundTripX64(GetParam()); }

// 8-bit: AX = AL * src; "high half" is AH (= AX>>8).  Read the whole AX plus
// the captured flag.  MN = mul/imul, SCC = setc/seto.
#define MUL8_FN(MN, SCC) \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  unsigned int ax=(unsigned)(a&0xFFu), fl;\n" \
  "  __asm__ volatile(\"" MN "b %b2\\n\\t" SCC " %b1\\n\\t\"\n" \
  "    :\"+a\"(ax),\"=&q\"(fl):\"q\"((unsigned)(b&0xFFu)):\"cc\");\n" \
  "  return (unsigned long)(ax&0xFFFFu)*8u+((unsigned long)(fl&1u)*2u);}\n"

// 16-bit: DX:AX = AX * src.  Fold AX, DX and the flag into disjoint fields.
#define MUL16_FN(MN, SCC) \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  unsigned int ax=(unsigned)(a&0xFFFFu), dx, fl;\n" \
  "  __asm__ volatile(\"" MN "w %w3\\n\\t" SCC " %b2\\n\\t\"\n" \
  "    :\"+a\"(ax),\"=d\"(dx),\"=&q\"(fl):\"r\"((unsigned)(b&0xFFFFu)):\"cc\");\n" \
  "  return (unsigned long)(ax&0xFFFFu)|((unsigned long)(dx&0xFFFFu)<<16)\n" \
  "       |((unsigned long)(fl&1u)<<40);}\n"

// 32-bit: EDX:EAX = EAX * src.  Multiplicative fold (every component matters).
#define MUL32_FN(MN, SCC) \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  unsigned int eax=(unsigned)a, edx, fl;\n" \
  "  __asm__ volatile(\"" MN "l %3\\n\\t" SCC " %b2\\n\\t\"\n" \
  "    :\"+a\"(eax),\"=d\"(edx),\"=&q\"(fl):\"r\"((unsigned)b):\"cc\");\n" \
  "  return (unsigned long)eax*0xD1B54A32D192ED03ULL\n" \
  "       +(unsigned long)edx*0x9E3779B97F4A7C15ULL\n" \
  "       +(unsigned long)(fl&1u)*0xC2B2AE3D27D4EB4FULL;}\n"

// 64-bit: RDX:RAX = RAX * src — the only path that needs a real i128 product.
#define MUL64_FN(MN, SCC) \
  "unsigned long f(unsigned long a, unsigned long b){\n" \
  "  unsigned long rax=a, rdx, fl;\n" \
  "  __asm__ volatile(\"" MN "q %3\\n\\t" SCC " %b2\\n\\t\"\n" \
  "    :\"+a\"(rax),\"=d\"(rdx),\"=&q\"(fl):\"r\"(b):\"cc\");\n" \
  "  return rax*0xD1B54A32D192ED03ULL+rdx*0x9E3779B97F4A7C15ULL\n" \
  "       +(fl&1UL)*0xC2B2AE3D27D4EB4FULL;}\n"

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== MUL (unsigned): CF = OF = (high half != 0). =====
  // 8-bit fit: 10*12=120 -> AH=0 -> CF=0.
  {"mul8_fit",   MUL8_FN("mul","setc"),  {10, 12},   "MulFlags"},
  // 8-bit overflow: 100*3=300=0x12C -> AH=1 -> CF=1.
  {"mul8_ovf",   MUL8_FN("mul","setc"),  {100, 3},   "MulFlags"},
  // 16-bit fit: 0x100*0x10=0x1000 -> DX=0 -> CF=0.
  {"mul16_fit",  MUL16_FN("mul","setc"), {0x100, 0x10},   "MulFlags"},
  // 16-bit overflow: 0x1234*0x1000 -> DX=0x123 -> CF=1.
  {"mul16_ovf",  MUL16_FN("mul","setc"), {0x1234, 0x1000}, "MulFlags"},
  // 32-bit fit: 0x10000*0x10=0x100000 -> EDX=0 -> CF=0.
  {"mul32_fit",  MUL32_FN("mul","setc"), {0x10000, 0x10},  "MulFlags"},
  // 32-bit overflow: 0xFFFFFFFF*0x10 -> EDX=0xF -> CF=1.
  {"mul32_ovf",  MUL32_FN("mul","setc"), {0xFFFFFFFFULL, 0x10}, "MulFlags"},
  // 32-bit overflow, OF read with seto (must equal CF).
  {"mul32_of",   MUL32_FN("mul","seto"), {0xFFFFFFFFULL, 0x10}, "MulFlags"},
  // 64-bit fit: 2^32 * 2 = 2^33 -> RDX=0 -> CF=0.
  {"mul64_fit",  MUL64_FN("mul","setc"), {0x100000000ULL, 2}, "MulFlags"},
  // 64-bit overflow: 2^32 * 2^32 = 2^64 -> RAX=0, RDX=1 -> CF=1.
  {"mul64_ovf",  MUL64_FN("mul","setc"), {0x100000000ULL, 0x100000000ULL}, "MulFlags"},
  // 64-bit overflow, OF read with seto.
  {"mul64_of",   MUL64_FN("mul","seto"), {0x100000000ULL, 0x100000000ULL}, "MulFlags"},

  // ===== IMUL (signed): CF = OF = (high half != SignExtend(low half)). =====
  // 8-bit fit: 10*12=120 (<=127) -> CF=0.
  {"imul8_fit",  MUL8_FN("imul","setc"), {10, 12},   "MulFlags"},
  // 8-bit overflow: 100*2=200 (>127) -> AL=0xC8=-56, sext!=AX -> CF=1.
  {"imul8_ovf",  MUL8_FN("imul","setc"), {100, 2},   "MulFlags"},
  // 8-bit negative-product fit: (-5)*6=-30 fits a signed byte -> CF=0.  (A
  // "high != 0" mis-rule would wrongly flag this since AH=0xFF here.)
  {"imul8_negfit", MUL8_FN("imul","setc"), {(uint64_t)(int64_t)-5, 6}, "MulFlags"},
  // 16-bit fit: 0x100*0x10=0x1000 (<32768) -> CF=0.
  {"imul16_fit", MUL16_FN("imul","setc"), {0x100, 0x10}, "MulFlags"},
  // 16-bit overflow: 0x4000*4=0x10000 (>32767) -> CF=1.
  {"imul16_ovf", MUL16_FN("imul","setc"), {0x4000, 4}, "MulFlags"},
  // 32-bit sign fit: (-1)*5=-5 -> EDX=0xFFFFFFFF=sext(EAX) -> CF=0.
  {"imul32_negfit", MUL32_FN("imul","setc"), {0xFFFFFFFFULL, 5}, "MulFlags"},
  // 32-bit overflow: 2^30 * 4 = 2^32 -> CF=1.
  {"imul32_ovf", MUL32_FN("imul","setc"), {0x40000000ULL, 4}, "MulFlags"},
  // 64-bit sign fit: (-1)*5=-5 -> RDX=all-ones=sext(RAX) -> CF=0.
  {"imul64_negfit", MUL64_FN("imul","setc"), {0xFFFFFFFFFFFFFFFFULL, 5}, "MulFlags"},
  // 64-bit overflow: 2^62 * 4 = 2^64 -> CF=1.
  {"imul64_ovf", MUL64_FN("imul","setc"), {0x4000000000000000ULL, 4}, "MulFlags"},
  // 64-bit overflow, OF read with seto.
  {"imul64_of",  MUL64_FN("imul","seto"), {0x4000000000000000ULL, 4}, "MulFlags"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(MulFlags, X64MulFlagsRT,
                         ::testing::ValuesIn(kX64), rtTCName);
