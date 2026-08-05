//===- X86_ParityFlagRTTests.cpp - parity flag + LAHF differential -*-C++-*-==//
//
// The x86 parity flag (PF) and LAHF flag-packing had no value-sweeping coverage:
// the PUSHF/POPF probes only pin fixed flag combinations (PF always 1, low byte
// 0x00) and never exercise odd parity or the data-dependent POPCOUNT path that
// NeverD lifts PF to (parity of the result's low byte).  PF is also special-cased
// in the MedFlags folder (it maps to no high-level CondCode, so a PF condition is
// never folded into an integer compare) -- these probes confirm that genuine PF
// chain survives the optimizer + recompile for arbitrary data on x86-64 and i386.
//
// Each probe LCG-sweeps many values through an inline-asm flag idiom clang never
// emits from plain C (setp/setnp/jp/jnp/cmovp, and LAHF), with the NeverD
// optimizer ON, comparing native vs lifted:
//   - setp/setnp        - PF read straight to a byte
//   - jp/jnp            - PF drives a branch
//   - cmovp             - PF drives a conditional move (SELECT fold path)
//   - lahf after add/sub - SF:ZF:AF:PF:CF packed into AH in one shot (every
//                          arithmetic flag is differentially checked per value)
// LAHF is only taken after ADD/SUB/CMP, where AF is architecturally defined
// (logic ops leave AF undefined, so they are read only through PF via setp).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ParityFlagRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ParityFlagRT, Verify) { roundTripX64(GetParam()); }
class X86ParityFlagRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86ParityFlagRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // setp: PF (even parity of low byte) read straight into a byte, swept.
  {"pf_setp",
   "unsigned long f(unsigned long a){ unsigned long acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245UL+12345UL;\n"
   "    unsigned long v=a^(a>>29); unsigned long p;\n"
   "    __asm__ volatile(\"testq %1,%1\\n\\tsetp %b0\\n\\tmovzbl %b0,%k0\"\n"
   "      :\"=&r\"(p):\"r\"(v):\"cc\");\n"
   "    acc=acc*31+p; }\n"
   "  return acc; }\n",
   {0x9E3779B97F4A7C15ULL}, "ParityFlag", 0},

  // setnp after xor (PF defined; AF undefined for logic, so only PF is read).
  {"pf_setnp_xor",
   "unsigned long f(unsigned long a){ unsigned long acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245UL+12345UL;\n"
   "    unsigned long x=a, y=(a>>17); unsigned long p;\n"
   "    __asm__ volatile(\"xorq %2,%1\\n\\tsetnp %b0\\n\\tmovzbl %b0,%k0\"\n"
   "      :\"=&r\"(p),\"+r\"(x):\"r\"(y):\"cc\");\n"
   "    acc=acc*31+p; }\n"
   "  return acc; }\n",
   {0x123456789ABCDEFULL}, "ParityFlag", 0},

  // jp/jnp: PF drives a branch selecting one of two values.
  {"pf_jp",
   "unsigned long f(unsigned long a){ unsigned long acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245UL+12345UL;\n"
   "    unsigned long x=a, y=(a>>13)|1; unsigned long p;\n"
   "    __asm__ volatile(\"addq %2,%1\\n\\tmovl $7,%k0\\n\\tjnp 1f\\n\\t\"\n"
   "                     \"movl $9,%k0\\n\\t1:\"\n"
   "      :\"=&r\"(p),\"+r\"(x):\"r\"(y):\"cc\");\n"
   "    acc=acc*31+p; }\n"
   "  return acc; }\n",
   {0xCAFEBABEDEADBEEFULL}, "ParityFlag", 0},

  // cmovp: PF drives a conditional move (exercises the SELECT fold path).
  {"pf_cmovp",
   "unsigned long f(unsigned long a){ unsigned long acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245UL+12345UL;\n"
   "    unsigned long v=a^(a>>31); unsigned long r0=0xAA00+i, r1=0xBB00+i;\n"
   "    __asm__ volatile(\"testq %1,%1\\n\\tcmovpq %2,%0\"\n"
   "      :\"+r\"(r0):\"r\"(v),\"r\"(r1):\"cc\");\n"
   "    acc=acc*31+r0; }\n"
   "  return acc; }\n",
   {0x0F1E2D3C4B5A6978ULL}, "ParityFlag", 0},

  // lahf after add: SF:ZF:AF:PF:CF packed into AH (all defined), swept.
  {"lahf_add",
   "unsigned long f(unsigned long a){ unsigned long acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245UL+12345UL;\n"
   "    unsigned long x=a, y=(a>>19)|0x10001; unsigned long ah;\n"
   "    __asm__ volatile(\"addq %2,%1\\n\\tlahf\\n\\tmovzbl %%ah,%k0\"\n"
   "      :\"=&r\"(ah),\"+r\"(x):\"r\"(y):\"rax\",\"cc\");\n"
   "    acc=acc*131+(ah&0xFF); }\n"
   "  return acc; }\n",
   {0x8000000000000001ULL}, "ParityFlag", 0},

  // lahf after sub (borrow chain): SF:ZF:AF:PF:CF packed into AH, swept.
  {"lahf_sub",
   "unsigned long f(unsigned long a){ unsigned long acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245UL+12345UL;\n"
   "    unsigned long x=a, y=(a>>23)|7; unsigned long ah;\n"
   "    __asm__ volatile(\"subq %2,%1\\n\\tlahf\\n\\tmovzbl %%ah,%k0\"\n"
   "      :\"=&r\"(ah),\"+r\"(x):\"r\"(y):\"rax\",\"cc\");\n"
   "    acc=acc*131+(ah&0xFF); }\n"
   "  return acc; }\n",
   {0x7FFFFFFFFFFFFFFFULL}, "ParityFlag", 0},
};

static const std::vector<RoundTripTC> kX86 = {
  // i386 dual: 32-bit registers, args on the stack, EBX reserved for PIC.
  {"pf_setp",
   "unsigned f(unsigned a){ unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245u+12345u;\n"
   "    unsigned v=a^(a>>13); unsigned p;\n"
   "    __asm__ volatile(\"testl %1,%1\\n\\tsetp %b0\\n\\tmovzbl %b0,%0\"\n"
   "      :\"=&q\"(p):\"r\"(v):\"cc\");\n"
   "    acc=acc*31+p; }\n"
   "  return acc; }\n",
   {0x9E3779B9u}, "ParityFlag32", 0},

  {"pf_setnp_xor",
   "unsigned f(unsigned a){ unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245u+12345u;\n"
   "    unsigned x=a, y=(a>>7); unsigned p;\n"
   "    __asm__ volatile(\"xorl %2,%1\\n\\tsetnp %b0\\n\\tmovzbl %b0,%0\"\n"
   "      :\"=&q\"(p),\"+r\"(x):\"r\"(y):\"cc\");\n"
   "    acc=acc*31+p; }\n"
   "  return acc; }\n",
   {0x1357ACEFu}, "ParityFlag32", 0},

  {"pf_jp",
   "unsigned f(unsigned a){ unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245u+12345u;\n"
   "    unsigned x=a, y=(a>>5)|1; unsigned p;\n"
   "    __asm__ volatile(\"addl %2,%1\\n\\tmovl $7,%0\\n\\tjnp 1f\\n\\t\"\n"
   "                     \"movl $9,%0\\n\\t1:\"\n"
   "      :\"=&r\"(p),\"+r\"(x):\"r\"(y):\"cc\");\n"
   "    acc=acc*31+p; }\n"
   "  return acc; }\n",
   {0xCAFEBABEu}, "ParityFlag32", 0},

  {"pf_cmovp",
   "unsigned f(unsigned a){ unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245u+12345u;\n"
   "    unsigned v=a^(a>>11); unsigned r0=0xAA00+i, r1=0xBB00+i;\n"
   "    __asm__ volatile(\"testl %1,%1\\n\\tcmovpl %2,%0\"\n"
   "      :\"+r\"(r0):\"r\"(v),\"r\"(r1):\"cc\");\n"
   "    acc=acc*31+r0; }\n"
   "  return acc; }\n",
   {0x0F1E2D3Cu}, "ParityFlag32", 0},

  {"lahf_add",
   "unsigned f(unsigned a){ unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245u+12345u;\n"
   "    unsigned x=a, y=(a>>9)|0x101; unsigned ah;\n"
   "    __asm__ volatile(\"addl %2,%1\\n\\tlahf\\n\\tmovzbl %%ah,%0\"\n"
   "      :\"=&q\"(ah),\"+r\"(x):\"r\"(y):\"eax\",\"cc\");\n"
   "    acc=acc*131+(ah&0xFF); }\n"
   "  return acc; }\n",
   {0x80000001u}, "ParityFlag32", 0},

  {"lahf_sub",
   "unsigned f(unsigned a){ unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ a=a*1103515245u+12345u;\n"
   "    unsigned x=a, y=(a>>15)|7; unsigned ah;\n"
   "    __asm__ volatile(\"subl %2,%1\\n\\tlahf\\n\\tmovzbl %%ah,%0\"\n"
   "      :\"=&q\"(ah),\"+r\"(x):\"r\"(y):\"eax\",\"cc\");\n"
   "    acc=acc*131+(ah&0xFF); }\n"
   "  return acc; }\n",
   {0x7FFFFFFFu}, "ParityFlag32", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ParityFlag, X64ParityFlagRT, ::testing::ValuesIn(kX64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(ParityFlag, X86ParityFlagRT, ::testing::ValuesIn(kX86),
                         rtTCName);
