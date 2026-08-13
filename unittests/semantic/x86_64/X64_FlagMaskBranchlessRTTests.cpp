//===- X64_FlagMaskBranchlessRTTests.cpp - flag-as-value idioms --*- C++ -*-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Existing flag tests read a flag into a setcc/jcc/cmov (a *condition*).  A
// different optimizer path materializes a flag as a numeric *value* and feeds
// it into arithmetic — the branchless idioms compilers love:
//   * `sbb same,same`  -> 0/-1 mask of CF (used to AND-select branchlessly)
//   * `adc reg,reg`    -> 2*reg + CF
//   * `setcc` byte fed straight into add/mul (x += (a<b))
//   * one CMP feeding *several different* setcc (stresses markFlagChainDead's
//     surviving-use guard: folding one consumer must not kill flags another
//     reads).
// These exercise the SSA flag value-flow plus the MedFlags fold + DCE +
// propagation interplay.  Every probe folds the produced value into the return
// so any dropped/mis-versioned flag diverges from the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FlagMaskRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FlagMaskRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== sbb same,same : CF -> 0/-1 mask. =====
  // cmp a,b sets CF when a<b (unsigned); sbb r,r = -CF.  a<b -> mask=-1.
  {"sbb_mask_lt",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"cmpq %2,%1\\n\\tsbbq %0,%0\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return m;}\n",
   {5, 9}, "FlagMask"},
  {"sbb_mask_ge",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"cmpq %2,%1\\n\\tsbbq %0,%0\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return m;}\n",
   {9, 5}, "FlagMask"},
  // Branchless select: r = (a<b) ? a : b  via mask = -(a<b); (a&m)|(b&~m).
  {"sbb_mask_select",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m;"
   "__asm__ volatile(\"cmpq %2,%1\\n\\tsbbq %0,%0\":\"=r\"(m):\"r\"(a),\"r\"(b):\"cc\");"
   "return (a&m)|(b&~m);}\n",
   {5, 9}, "FlagMask"},
  // CF from a shift (bit0) drives the mask.
  {"sbb_mask_shr",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long m,x=a;"
   "__asm__ volatile(\"shrq $1,%1\\n\\tsbbq %0,%0\":\"=r\"(m),\"+r\"(x)::\"cc\");"
   "return m^x;}\n",
   {0xF, 0}, "FlagMask"},

  // ===== adc reg,reg : 2*reg + CF. =====
  {"adc_double_cf1",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a;"
   "__asm__ volatile(\"stc\\n\\tadcq %0,%0\":\"+r\"(x)::\"cc\");"
   "return x;}\n",
   {0x40, 0}, "FlagMask"},
  {"adc_double_cf0",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long x=a;"
   "__asm__ volatile(\"clc\\n\\tadcq %0,%0\":\"+r\"(x)::\"cc\");"
   "return x;}\n",
   {0x40, 0}, "FlagMask"},

  // ===== setcc byte -> arithmetic (branchless increment / accumulate). =====
  // r = base + (a<b) ; clang lowers to setb + add or adc.
  {"setb_add",
   "unsigned long f(unsigned long a,unsigned long b){"
   "return 1000ULL+(a<b);}\n",
   {5, 9}, "FlagMask", 1},
  {"setcc_branchless_acc",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long acc=0;"
   "for(unsigned long i=0;i<64;i++){acc+=((a>>i)&1)?b:0;}return acc;}\n",
   {0xA5A5A5A5ULL, 7}, "FlagMask", 2},

  // ===== one CMP -> several different setcc (surviving-use guard). =====
  // sete/setne/setl/setg/setb/seta all read the SAME cmp's flags; folding one
  // must not kill ZF/SF/OF/CF that the others still read.
  {"multi_setcc_eq",
   "unsigned long f(long a,long b){unsigned long e,n,l,g,bl,ab;"
   "__asm__ volatile(\"cmpq %7,%6\\n\\tsete %b0\\n\\tsetne %b1\\n\\tsetl %b2\\n\\t\"\n"
   "  \"setg %b3\\n\\tsetb %b4\\n\\tseta %b5\"\n"
   "  :\"=&q\"(e),\"=&q\"(n),\"=&q\"(l),\"=&q\"(g),\"=&q\"(bl),\"=&q\"(ab)\n"
   "  :\"r\"(a),\"r\"(b):\"cc\");"
   "return (e&1)|((n&1)<<1)|((l&1)<<2)|((g&1)<<3)|((bl&1)<<4)|((ab&1)<<5);}\n",
   {7, 7}, "FlagMask"},
  {"multi_setcc_lt",
   "unsigned long f(long a,long b){unsigned long e,n,l,g,bl,ab;"
   "__asm__ volatile(\"cmpq %7,%6\\n\\tsete %b0\\n\\tsetne %b1\\n\\tsetl %b2\\n\\t\"\n"
   "  \"setg %b3\\n\\tsetb %b4\\n\\tseta %b5\"\n"
   "  :\"=&q\"(e),\"=&q\"(n),\"=&q\"(l),\"=&q\"(g),\"=&q\"(bl),\"=&q\"(ab)\n"
   "  :\"r\"(a),\"r\"(b):\"cc\");"
   "return (e&1)|((n&1)<<1)|((l&1)<<2)|((g&1)<<3)|((bl&1)<<4)|((ab&1)<<5);}\n",
   {(uint64_t)(int64_t)-3, 5}, "FlagMask"},
  {"multi_setcc_signov",
   "unsigned long f(long a,long b){unsigned long e,n,l,g,bl,ab;"
   "__asm__ volatile(\"cmpq %7,%6\\n\\tsete %b0\\n\\tsetne %b1\\n\\tsetl %b2\\n\\t\"\n"
   "  \"setg %b3\\n\\tsetb %b4\\n\\tseta %b5\"\n"
   "  :\"=&q\"(e),\"=&q\"(n),\"=&q\"(l),\"=&q\"(g),\"=&q\"(bl),\"=&q\"(ab)\n"
   "  :\"r\"(a),\"r\"(b):\"cc\");"
   "return (e&1)|((n&1)<<1)|((l&1)<<2)|((g&1)<<3)|((bl&1)<<4)|((ab&1)<<5);}\n",
   {(uint64_t)(int64_t)0x8000000000000000ULL, 1}, "FlagMask"},

  // ===== 16-bit partial write, read parent at wider width (upper preserved). =====
  // movw into low 16 must preserve the upper 16/48 bits of the parent.
  {"movw_read32",
   "unsigned long f(unsigned long a,unsigned long b){unsigned r=(unsigned)a;"
   "unsigned short v=(unsigned short)b;"
   "__asm__ volatile(\"movw %w1,%w0\":\"+r\"(r):\"r\"(v));"
   "return r;}\n",
   {0xAAAA1111ULL, 0xBBBB2222ULL}, "FlagMask"},
  {"movw_read64",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long r=a;"
   "unsigned short v=(unsigned short)b;"
   "__asm__ volatile(\"movw %w1,%w0\":\"+r\"(r):\"r\"(v));"
   "return r;}\n",
   {0x1122334455660000ULL, 0x99AA}, "FlagMask"},
  // addw into low 16 (carry stays within the 16-bit field; upper preserved).
  {"addw_read64",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long r=a;"
   "unsigned short v=(unsigned short)b;"
   "__asm__ volatile(\"addw %w1,%w0\":\"+r\"(r):\"r\"(v):\"cc\");"
   "return r;}\n",
   {0x11223344FFFF0001ULL, 0xFFFF}, "FlagMask"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FlagMask, X64FlagMaskRT, ::testing::ValuesIn(kX64),
                         rtTCName);
