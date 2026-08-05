//===- ARM32_BitfieldCarryRTTests.cpp - carry alias + bitfield -*- C++ -*-===//
//
// RSC (reverse subtract with carry) reads its source operands for the C/V
// flags; when the destination aliases the minuend (rscs rD,rN,rD) the source
// must be snapshotted before the result write.  These probes drive that form
// (folding the carry-out) plus the bitfield (BFI/BFC/SBFX/UBFX), pack
// (PKHBT/PKHTB) and reverse (REV/REV16/REVSH/RBIT) families.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32BitfieldCarryRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BitfieldCarryRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kARM32 = {
  // ===== RSCS with dst aliasing Rm (the minuend), carry-in 0. =====
  // b = b - a - !C = 1 - 0 - 1 = 0, carry-out must be 1.
  {"rscs_alias",
   "int f(int u){unsigned b=1,scratch,cf=0,a=0;"
   "__asm__ volatile(\"adds %1,%4,%4\\n\\trscs %0,%3,%0\\n\\tmovcs %2,#1\\n\\tmovcc %2,#0\""
   ":\"+r\"(b),\"=&r\"(scratch),\"=r\"(cf):\"r\"(a),\"r\"(0u):\"cc\");"
   "return b+(cf<<1);}\n",
   {0}, "BitfieldCarry"},

  // ===== ADCS / SBCS 64-bit chains (dst aliases src1). =====
  {"adcs_alias",
   "int f(int u){unsigned lo=0,hi=0xFFFFFFFFu,cf;"
   "__asm__ volatile(\"adds %0,%0,%3\\n\\tadcs %1,%1,%4\\n\\tmovcs %2,#1\\n\\tmovcc %2,#0\""
   ":\"+r\"(lo),\"+r\"(hi),\"=r\"(cf):\"r\"(0u),\"r\"(1u):\"cc\");"
   "return hi+(cf<<1)+lo;}\n",
   {0}, "BitfieldCarry"},

  {"sbcs_alias",
   "int f(int u){unsigned lo=0,hi=0,cf;"
   "__asm__ volatile(\"subs %0,%0,%3\\n\\tsbcs %1,%1,%4\\n\\tmovcs %2,#1\\n\\tmovcc %2,#0\""
   ":\"+r\"(lo),\"+r\"(hi),\"=r\"(cf):\"r\"(1u),\"r\"(0u):\"cc\");"
   "return hi+(cf<<1)+lo;}\n",
   {0}, "BitfieldCarry"},

  // ===== BFI / BFC. =====
  {"bfi",
   "int f(int a,int b){unsigned r=(unsigned)a;"
   "__asm__ volatile(\"bfi %0,%1,#8,#8\":\"+r\"(r):\"r\"(b));return r;}\n",
   {0xFFFFFFFFu, 0xAB}, "BitfieldCarry"},

  {"bfc",
   "int f(int a){unsigned r=(unsigned)a;"
   "__asm__ volatile(\"bfc %0,#12,#10\":\"+r\"(r));return r;}\n",
   {0xFFFFFFFFu}, "BitfieldCarry"},

  // ===== SBFX (signed) / UBFX (unsigned) extract. =====
  {"sbfx_neg",
   "int f(int a){int r;"
   "__asm__ volatile(\"sbfx %0,%1,#8,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0000FF00}, "BitfieldCarry"},

  {"ubfx",
   "int f(int a){unsigned r;"
   "__asm__ volatile(\"ubfx %0,%1,#4,#12\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0xDEADBEEF}, "BitfieldCarry"},

  // ===== PKHBT / PKHTB pack halfword. =====
  {"pkhbt",
   "int f(int a,int b){unsigned r;"
   "__asm__ volatile(\"pkhbt %0,%1,%2,lsl #4\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {0x0000ABCD, 0x12345000}, "BitfieldCarry"},

  {"pkhtb",
   "int f(int a,int b){unsigned r;"
   "__asm__ volatile(\"pkhtb %0,%1,%2,asr #4\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {0xABCD0000, 0x00012345}, "BitfieldCarry"},

  // ===== REV / REV16 / REVSH / RBIT / CLZ. =====
  {"rev",
   "int f(int a){unsigned r;"
   "__asm__ volatile(\"rev %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x12345678}, "BitfieldCarry"},

  {"rev16",
   "int f(int a){unsigned r;"
   "__asm__ volatile(\"rev16 %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x12345678}, "BitfieldCarry"},

  {"revsh",
   "int f(int a){int r;"
   "__asm__ volatile(\"revsh %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x00008099}, "BitfieldCarry"},

  {"rbit",
   "int f(int a){unsigned r;"
   "__asm__ volatile(\"rbit %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x12345678}, "BitfieldCarry"},

  {"clz",
   "int f(int a){unsigned r;"
   "__asm__ volatile(\"clz %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x00010000}, "BitfieldCarry"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BitfieldCarry, ARM32BitfieldCarryRT,
                         ::testing::ValuesIn(kARM32), rtTCName);
