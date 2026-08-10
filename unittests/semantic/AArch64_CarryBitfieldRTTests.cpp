//===- AArch64_CarryBitfieldRTTests.cpp - carry alias + bitfield -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// ADCS/SBCS read their source operands for the C/V flags; when the destination
// aliases a source (adcs xD,xD,xM, the multi-precision-add idiom) the source
// must be snapshotted before the result write or the flag reads the post-write
// value.  These probes drive that aliasing form (folding the carry-out) plus
// edge cases of the bitfield-move aliases (UBFX/SBFX/BFI/BFXIL/SBFIZ/UBFIZ),
// EXTR rotate amounts and the bit/byte-reverse family.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CarryBitfieldRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CarryBitfieldRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // ===== ADCS with dst aliasing src1, carry-in 0, A+B overflows (C1 path). ====
  // lo=0+0 (C=0), then adcs hi=~0 + 1 + 0 -> 0, carry-out must be 1.
  {"adcs_alias_c1",
   "long f(long u){unsigned long lo=0,hi=0xFFFFFFFFFFFFFFFFUL,cf;"
   "__asm__ volatile(\"adds %0,%0,%3\\n\\tadcs %1,%1,%4\\n\\tcset %2,cs\""
   ":\"+r\"(lo),\"+r\"(hi),\"=r\"(cf):\"r\"(0UL),\"r\"(1UL):\"cc\");"
   "return hi+(cf<<1)+lo;}\n",
   {0}, "CarryBitfield"},

  // ===== ADCS dst aliasing src1, carry-in 1 propagating (C2 path). =====
  {"adcs_alias_c2",
   "long f(long u){unsigned long lo=0xFFFFFFFFFFFFFFFFUL,hi=0xFFFFFFFFFFFFFFFFUL,cf;"
   "__asm__ volatile(\"adds %0,%0,%3\\n\\tadcs %1,%1,%4\\n\\tcset %2,cs\""
   ":\"+r\"(lo),\"+r\"(hi),\"=r\"(cf):\"r\"(1UL),\"r\"(0UL):\"cc\");"
   "return hi+(cf<<1)+lo;}\n",
   {0}, "CarryBitfield"},

  // ===== ADCS overflow flag (V) with dst aliasing src1. =====
  {"adcs_alias_v",
   "long f(long u){long lo=0,hi=0x7FFFFFFFFFFFFFFFL,vf;"
   "__asm__ volatile(\"adds %0,%0,%3\\n\\tadcs %1,%1,%4\\n\\tcset %2,vs\""
   ":\"+r\"(lo),\"+r\"(hi),\"=r\"(vf):\"r\"(0L),\"r\"(1L):\"cc\");"
   "return hi+(vf<<1)+lo;}\n",
   {0}, "CarryBitfield"},

  // ===== SBCS dst aliasing src1: 128-bit subtract borrow chain. =====
  {"sbcs_alias",
   "long f(long u){unsigned long lo=0,hi=0,cf;"
   "__asm__ volatile(\"subs %0,%0,%3\\n\\tsbcs %1,%1,%4\\n\\tcset %2,cs\""
   ":\"+r\"(lo),\"+r\"(hi),\"=r\"(cf):\"r\"(1UL),\"r\"(0UL):\"cc\");"
   "return hi+(cf<<1)+lo;}\n",
   {0}, "CarryBitfield"},

  // ===== NGCS (negate with carry, sets flags) alias of SBCS. =====
  {"ngcs",
   "long f(long a){long r,cf;"
   "__asm__ volatile(\"subs xzr,xzr,xzr\\n\\tngcs %0,%2\\n\\tcset %1,cs\""
   ":\"=r\"(r),\"=r\"(cf):\"r\"(a):\"cc\");"
   "return r+(cf<<1);}\n",
   {5}, "CarryBitfield"},

  // ===== UBFX: extract bits [8:23] (width 16, lsb 8). =====
  {"ubfx",
   "long f(long a){long r;"
   "__asm__ volatile(\"ubfx %0,%1,#8,#16\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0xDEADBEEFCAFE1234ULL}, "CarryBitfield"},

  // ===== SBFX: signed extract, sign bit set. =====
  {"sbfx_neg",
   "long f(long a){long r;"
   "__asm__ volatile(\"sbfx %0,%1,#8,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0000FF00ULL}, "CarryBitfield"},

  // ===== BFI: insert 8 bits at position 16, preserving the rest of dst. =====
  {"bfi",
   "long f(long a,long b){long r=a;"
   "__asm__ volatile(\"bfi %0,%2,#16,#8\":\"+r\"(r):\"r\"(b));return r;}\n",
   {0xFFFFFFFFFFFFFFFFULL, 0xAB}, "CarryBitfield"},

  // ===== BFXIL: insert-low from src bits [4:11] into dst[0:7]. =====
  {"bfxil",
   "long f(long a,long b){long r=a;"
   "__asm__ volatile(\"bfxil %0,%2,#4,#8\":\"+r\"(r):\"r\"(b));return r;}\n",
   {0xFFFFFFFFFFFFFF00ULL, 0x0000ABC0ULL}, "CarryBitfield"},

  // ===== SBFIZ: signed insert at zero, sign-extend the field. =====
  {"sbfiz",
   "long f(long a){long r;"
   "__asm__ volatile(\"sbfiz %0,%1,#20,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x80ULL}, "CarryBitfield"},

  // ===== UBFIZ: unsigned insert at zero. =====
  {"ubfiz",
   "long f(long a){long r;"
   "__asm__ volatile(\"ubfiz %0,%1,#20,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0xFFULL}, "CarryBitfield"},

  // ===== EXTR: rotate-by-extract from a register pair. =====
  {"extr",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"extr %0,%1,%2,#20\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL}, "CarryBitfield"},

  // ===== EXTR alias ROR by immediate. =====
  {"ror_imm",
   "long f(long a){long r;"
   "__asm__ volatile(\"ror %0,%1,#13\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x123456789ABCDEF0ULL}, "CarryBitfield"},

  // ===== RBIT / REV16 / REV32 / CLS edge values. =====
  {"rbit",
   "long f(long a){long r;"
   "__asm__ volatile(\"rbit %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x123456789ABCDEF0ULL}, "CarryBitfield"},

  {"rev16",
   "long f(long a){long r;"
   "__asm__ volatile(\"rev16 %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x123456789ABCDEF0ULL}, "CarryBitfield"},

  {"rev32",
   "long f(long a){long r;"
   "__asm__ volatile(\"rev32 %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x123456789ABCDEF0ULL}, "CarryBitfield"},

  {"cls_neg",
   "long f(long a){long r;"
   "__asm__ volatile(\"cls %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0xFFFFFF0000000000ULL}, "CarryBitfield"},

  // ===== Conditional select family with same source registers. =====
  {"csneg",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsneg %0,%1,%2,gt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {5, 9}, "CarryBitfield"},

  {"csinv",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsinv %0,%1,%2,lt\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {5, 9}, "CarryBitfield"},

  {"cneg",
   "long f(long a){long r;"
   "__asm__ volatile(\"cmp %1,#0\\n\\tcneg %0,%1,ge\":\"=r\"(r):\"r\"(a):\"cc\");"
   "return r;}\n",
   {7}, "CarryBitfield"},

  {"csetm",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcsetm %0,eq\":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");"
   "return r;}\n",
   {4, 4}, "CarryBitfield"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CarryBitfield, A64CarryBitfieldRT,
                         ::testing::ValuesIn(kA64), rtTCName);
