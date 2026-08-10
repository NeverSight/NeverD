//===- X86_FlagPreserveRTTests.cpp - x86 partial-flag preservation -*-C++-*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Probes the self-written MedFlags attribution against the x86 partial-flag
// quirks that an SSA flag model can mis-track: INC/DEC update OF/SF/ZF/AF/PF but
// PRESERVE CF, and a flag consumer (ADC / SETcc / SBB-mask) reached across such
// a CF-preserving op must take CF from the *earlier* CF writer, not the INC/DEC
// in between.  Likewise OF/SF/ZF must come from the *last* writer when several
// ops touch them.  Each kernel forces an exact instruction order via a single
// inline-asm block (clang never inserts flag clobbers inside one asm block), so
// any wrong flag-def attribution in MedFlags diverges native-vs-lifted.  Run on
// x86-64 and i386 (the less-mature i386 lift path) with the optimizer ON.
//
//   * cf_thru_inc - ADD sets CF, INC preserves it, ADC consumes it.
//   * cf_thru_dec - ADD sets CF, DEC preserves it, ADC consumes it.
//   * cf_thru_mov - ADD sets CF, MOV/LEA (flag-free) between, ADC consumes it.
//   * sbb_thru_inc- SUB sets CF(borrow), INC preserves it, SBB reg,reg = -CF.
//   * of_last_wins- ADD sets OF, AND clears OF, SETO reads the cleared OF.
//   * multi_setcc - one CMP feeds SETB/SETA/SETL with no intervening flag write.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FlagPreserveRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FlagPreserveRT, Verify) { roundTripX64(GetParam()); }
class X86FlagPreserveRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FlagPreserveRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
// x86-64 kernels use 64-bit operands; the inline-asm forces the exact order.
static const std::vector<RoundTripTC> kX64 = {
  // ADD sets CF, INC preserves CF, ADC consumes CF.  a=~0,b=1 -> CF=1 -> r=1.
  {"cf_thru_inc",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){\n"
   "  unsigned long r=a, cc=c;\n"
   "  __asm__ volatile(\"addq %2,%0\\n\\tincq %1\\n\\tadcq $0,%0\"\n"
   "    :\"+r\"(r),\"+r\"(cc):\"r\"(b):\"cc\");\n"
   "  return r*31u+cc; }\n",
   {0xFFFFFFFFFFFFFFFFULL, 1ULL, 100ULL}, "FlagPreserve"},

  // ADD sets CF, DEC preserves CF, ADC consumes CF.
  {"cf_thru_dec",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){\n"
   "  unsigned long r=a, cc=c;\n"
   "  __asm__ volatile(\"addq %2,%0\\n\\tdecq %1\\n\\tadcq $0,%0\"\n"
   "    :\"+r\"(r),\"+r\"(cc):\"r\"(b):\"cc\");\n"
   "  return r*31u+cc; }\n",
   {0xFFFFFFFFFFFFFFF0ULL, 0x20ULL, 100ULL}, "FlagPreserve"},

  // ADD sets CF, flag-free MOV/LEA between, ADC consumes CF.
  {"cf_thru_mov",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){\n"
   "  unsigned long r=a, cc=c, t;\n"
   "  __asm__ volatile(\"addq %3,%0\\n\\tmovq %0,%2\\n\\tleaq 7(%1),%1\\n\\tadcq $0,%0\"\n"
   "    :\"+r\"(r),\"+r\"(cc),\"=&r\"(t):\"r\"(b):\"cc\");\n"
   "  return r*31u+cc+t; }\n",
   {0xFFFFFFFFFFFFFFFFULL, 2ULL, 100ULL}, "FlagPreserve"},

  // SUB sets CF (borrow), INC preserves it, SBB reg,reg materializes -CF (mask).
  {"sbb_thru_inc",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){\n"
   "  unsigned long r=a, cc=c, m;\n"
   "  __asm__ volatile(\"subq %3,%0\\n\\tincq %1\\n\\tsbbq %2,%2\"\n"
   "    :\"+r\"(r),\"+r\"(cc),\"=r\"(m):\"r\"(b):\"cc\");\n"
   "  return (m&0xABCDu)+r+cc; }\n",
   {1ULL, 5ULL, 100ULL}, "FlagPreserve"},

  // ADD sets OF, AND clears OF, SETO must read the *cleared* OF (AND, last writer).
  {"of_last_wins",
   "unsigned long f(unsigned long a,unsigned long b){\n"
   "  unsigned long r=a; unsigned char o;\n"
   "  __asm__ volatile(\"addq %2,%0\\n\\tandq $0xff,%0\\n\\tseto %1\"\n"
   "    :\"+r\"(r),\"=r\"(o):\"r\"(b):\"cc\");\n"
   "  return r + (unsigned long)o*1000u; }\n",
   {0x7FFFFFFFFFFFFFFFULL, 1ULL}, "FlagPreserve"},

  // One CMP feeds three SETcc with no intervening flag write (multi-consumer).
  {"multi_setcc",
   "unsigned long f(unsigned long a,unsigned long b){\n"
   "  unsigned char lb,ab,ll;\n"
   "  __asm__ volatile(\"cmpq %4,%3\\n\\tsetb %0\\n\\tseta %1\\n\\tsetl %2\"\n"
   "    :\"=r\"(lb),\"=r\"(ab),\"=r\"(ll):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (unsigned long)lb + ab*10u + ll*100u; }\n",
   {(uint64_t)(int64_t)-3, 5ULL}, "FlagPreserve"},
};

// i386 kernels: 32-bit operands, same flag-preservation quirks, cdecl ABI.
static const std::vector<RoundTripTC> kX86 = {
  {"cf_thru_inc",
   "unsigned f(unsigned a,unsigned b,unsigned c){\n"
   "  unsigned r=a, cc=c;\n"
   "  __asm__ volatile(\"addl %2,%0\\n\\tincl %1\\n\\tadcl $0,%0\"\n"
   "    :\"+r\"(r),\"+r\"(cc):\"r\"(b):\"cc\");\n"
   "  return r*31u+cc; }\n",
   {0xFFFFFFFFULL, 1ULL, 100ULL}, "FlagPreserve32"},

  {"cf_thru_dec",
   "unsigned f(unsigned a,unsigned b,unsigned c){\n"
   "  unsigned r=a, cc=c;\n"
   "  __asm__ volatile(\"addl %2,%0\\n\\tdecl %1\\n\\tadcl $0,%0\"\n"
   "    :\"+r\"(r),\"+r\"(cc):\"r\"(b):\"cc\");\n"
   "  return r*31u+cc; }\n",
   {0xFFFFFFF0ULL, 0x20ULL, 100ULL}, "FlagPreserve32"},

  {"sbb_thru_inc",
   "unsigned f(unsigned a,unsigned b,unsigned c){\n"
   "  unsigned r=a, cc=c, m;\n"
   "  __asm__ volatile(\"subl %3,%0\\n\\tincl %1\\n\\tsbbl %2,%2\"\n"
   "    :\"+r\"(r),\"+r\"(cc),\"=r\"(m):\"r\"(b):\"cc\");\n"
   "  return (m&0xABCDu)+r+cc; }\n",
   {1ULL, 5ULL, 100ULL}, "FlagPreserve32"},

  {"of_last_wins",
   "unsigned f(unsigned a,unsigned b){\n"
   "  unsigned r=a; unsigned char o;\n"
   "  __asm__ volatile(\"addl %2,%0\\n\\tandl $0xff,%0\\n\\tseto %1\"\n"
   "    :\"+r\"(r),\"=r\"(o):\"r\"(b):\"cc\");\n"
   "  return r + (unsigned)o*1000u; }\n",
   {0x7FFFFFFFULL, 1ULL}, "FlagPreserve32"},

  {"multi_setcc",
   "unsigned f(unsigned a,unsigned b){\n"
   "  unsigned char lb,ab,ll;\n"
   "  __asm__ volatile(\"cmpl %4,%3\\n\\tsetb %0\\n\\tseta %1\\n\\tsetl %2\"\n"
   "    :\"=r\"(lb),\"=r\"(ab),\"=r\"(ll):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (unsigned)lb + ab*10u + ll*100u; }\n",
   {(uint64_t)(uint32_t)-3, 5ULL}, "FlagPreserve32"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FlagPreserve, X64FlagPreserveRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FlagPreserve, X86FlagPreserveRT,
                         ::testing::ValuesIn(kX86), rtTCName);
