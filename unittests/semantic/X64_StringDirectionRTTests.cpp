//===- X64_StringDirectionRTTests.cpp - x86 string DF direction -*- C++ -*-===//
//
// x86 string instructions (LODS/STOS/SCAS/MOVS/CMPS) advance their pointers
// FORWARD when DF=0 and BACKWARD when DF=1.  The lift modelled them as always
// forward; these probes set DF=1 with `std`, run one element, then fold the
// pointer delta (which is address-independent) into the return value so the
// roundtrip harness exposes the ignored direction flag.  DF=0 (`cld`) controls
// must already pass.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StringDirRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StringDirRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // LODS backward: rsi 16 -> 15, before-after = +1.
  {"lodsb_df1",
   "long f(long a){volatile char buf[32];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rsi\\n\\tmovq %%rsi,%0\\n\\t"
   "std\\n\\tlodsb\\n\\tcld\\n\\tmovq %%rsi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rsi\",\"rax\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},
  // LODS forward control: rsi 16 -> 17, before-after = -1.
  {"lodsb_df0",
   "long f(long a){volatile char buf[32];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rsi\\n\\tmovq %%rsi,%0\\n\\t"
   "cld\\n\\tlodsb\\n\\tmovq %%rsi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rsi\",\"rax\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},
  // LODS with no cld/std: relies on the SysV ABI invariant DF=0 (DFLAG live-in
  // must default to 0 / forward, not become a garbage parameter).
  {"lodsb_nocld",
   "long f(long a){volatile char buf[32];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rsi\\n\\tmovq %%rsi,%0\\n\\t"
   "lodsb\\n\\tmovq %%rsi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rsi\",\"rax\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},

  // STOS backward: rdi 16 -> 15.
  {"stosb_df1",
   "long f(long a){volatile char buf[32];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rdi\\n\\tmovq %%rdi,%0\\n\\t"
   "std\\n\\tstosb\\n\\tcld\\n\\tmovq %%rdi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rdi\",\"rax\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},

  // STOS dword backward: rdi delta = +4.
  {"stosd_df1",
   "long f(long a){volatile int buf[16];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rdi\\n\\tmovq %%rdi,%0\\n\\t"
   "std\\n\\tstosl\\n\\tcld\\n\\tmovq %%rdi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rdi\",\"rax\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},

  // SCAS backward: rdi 16 -> 15.
  {"scasb_df1",
   "long f(long a){volatile char buf[32];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rdi\\n\\tmovq %%rdi,%0\\n\\t"
   "std\\n\\tscasb\\n\\tcld\\n\\tmovq %%rdi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rdi\",\"rax\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},

  // MOVS backward: both rsi and rdi step back; check rdi delta = +1.
  {"movsb_df1",
   "long f(long a){volatile char buf[64];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rsi\\n\\tleaq 48(%2),%%rdi\\n\\t"
   "movq %%rdi,%0\\n\\tstd\\n\\tmovsb\\n\\tcld\\n\\tmovq %%rdi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rsi\",\"rdi\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},

  // CMPS backward: both step back; check rsi delta = +1.
  {"cmpsb_df1",
   "long f(long a){volatile char buf[64];unsigned long b,e;"
   "__asm__ volatile(\"leaq 16(%2),%%rsi\\n\\tleaq 48(%2),%%rdi\\n\\t"
   "movq %%rsi,%0\\n\\tstd\\n\\tcmpsb\\n\\tcld\\n\\tmovq %%rsi,%1\""
   ":\"=&r\"(b),\"=&r\"(e):\"r\"(buf):\"rsi\",\"rdi\",\"cc\",\"memory\");"
   "return (long)(b-e);}\n",
   {0}, "StringDir"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(StringDir, X64StringDirRT, ::testing::ValuesIn(kX64),
                         rtTCName);
