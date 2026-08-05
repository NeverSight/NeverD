//===- X86_32_IntRoundTripTests.cpp - i386 integer roundtrip ----*- C++ -*-===//
//
// First roundtrip coverage for the 32-bit x86 (i386) target.  NeverD supports
// Arch::X86 across the loader / lifter / LowToMed / codegen (triple
// i386-unknown-linux-gnu), but every prior roundtrip suite exercised only
// x86-64, AArch64, and ARM32 — leaving the i386 path (cdecl stack arguments,
// 32-bit pointers, the i386 instruction encodings clang selects) unverified.
//
// These kernels are deliberately integer-only (no globals, no float, no 64-bit
// result) so they validate the cdecl harness and the core i386 ALU/branch/loop
// lift end-to-end before wider coverage builds on top.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86IntRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86IntRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86Int = {
  // Plain add into a 32-bit register (sub-register width handling on i386).
  {"x86_add",
   "int x86_add(int a){ return a + 0x12345678; }\n",
   {0x1111ULL}, "X86Int", 2, ""},

  // Mixed arithmetic + byte mask + arithmetic-shift chain.
  {"x86_arith",
   "int x86_arith(int a){ int b=a*3+7; b^=b>>5; b-=a&0xFF; return b*5-1; }\n",
   {0x2468ULL}, "X86Int", 2, ""},

  // Loop with a loop-carried accumulator and imul.
  {"x86_loopsum",
   "int x86_loopsum(int a){ int acc=0; for(int i=0;i<100;i++) acc+=a*(i+1); return acc; }\n",
   {0x37ULL}, "X86Int", 2, ""},

  // Kernighan popcount folded into the result.
  {"x86_popcnt",
   "int x86_popcnt(int a){ unsigned x=(unsigned)a; int n=0; while(x){ x&=x-1; n++; } return n*1000003+a; }\n",
   {0xF0F0A5A5ULL}, "X86Int", 2, ""},

  // Branchy min/max/abs cascade (setcc/cmov + conditional branches).
  {"x86_cond",
   "int x86_cond(int a){ int b=a*7-3, c=a^0x55, r=0;\n"
   "  r += (a<b)?b:a; r -= (c>a)?c:a; r += (a<0)?-a:a; return r; }\n",
   {0x91ULL}, "X86Int", 2, ""},

  // Shifts and a rotate idiom.
  {"x86_shift",
   "int x86_shift(int a){ unsigned u=(unsigned)a;\n"
   "  unsigned r=(u<<3)|(u>>29); r^=(unsigned)(a>>2); return (int)(r+(u<<11)); }\n",
   {0xC3D4ULL}, "X86Int", 2, ""},

  // Native i386 signed division and remainder (idiv, no library call).
  {"x86_divrem",
   "int x86_divrem(int a){ int q=a/7, r=a%7; return q*100+r; }\n",
   {0x4D2ULL}, "X86Int", 2, ""},

  // Unsigned division/modulo (div).
  {"x86_udivrem",
   "int x86_udivrem(int a){ unsigned u=(unsigned)a; return (int)((u/13u)*7u+(u%13u)); }\n",
   {0xABCDEULL}, "X86Int", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86Int, X86IntRT, ::testing::ValuesIn(kX86Int),
                         rtTCName);
