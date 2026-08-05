//===- X64_BitTestRTTests.cpp - BT/BTS/BTR/BTC bit-offset semantics -*- C++ -*-===//
//
// x86 BT/BTS/BTR/BTC have two distinct bit-offset semantics the lifter must
// honour:
//   * register bit base: the offset is taken modulo the operand size (16/32/64),
//     so `bt eax, 33` tests bit 1.
//   * memory bit base:   the offset is a signed bit index into memory, so
//     `bt (mem), 40` tests bit 0 of byte 5 — it can reach BEYOND the addressed
//     operand word.
//
// The probes pin the instruction stream with inline asm and use offsets >= the
// operand width so a naive `base >> idx` (which saturates / stays in-word) is
// observably wrong.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitTestRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitTestRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // Register BT, index 33 -> masks to bit 1.  x has bit 1 set -> CF=1.
  {"bt_reg_mask32",
   "long f(long a,long b){unsigned int x=(unsigned int)a;unsigned int n=(unsigned int)b;"
   "unsigned long cf=0;"
   "__asm__ volatile(\"btl %[n],%[x]\\n\\tsetc %b[c]\""
   ":[c]\"+r\"(cf):[x]\"r\"(x),[n]\"r\"(n):\"cc\");return (long)(cf&1);}\n",
   {0x2, 33}, "BitTest", 0},

  // Register BT 64-bit, index 65 -> masks to bit 1.  x has bit 1 set -> CF=1.
  {"bt_reg_mask64",
   "long f(long a,long b){unsigned long x=(unsigned long)a;unsigned long n=(unsigned long)b;"
   "unsigned long cf=0;"
   "__asm__ volatile(\"btq %[n],%[x]\\n\\tsetc %b[c]\""
   ":[c]\"+r\"(cf):[x]\"r\"(x),[n]\"r\"(n):\"cc\");return (long)(cf&1);}\n",
   {0x2, 65}, "BitTest", 0},

  // Register BTS, index 34 -> masks to bit 2; result must have bit 2 set.
  {"bts_reg_mask32",
   "long f(long a,long b){unsigned int x=(unsigned int)a;unsigned int n=(unsigned int)b;"
   "__asm__ volatile(\"btsl %[n],%[x]\":[x]\"+r\"(x):[n]\"r\"(n):\"cc\");"
   "return (long)x;}\n",
   {0x1, 34}, "BitTest", 0},

  // Register BTR, index 32 -> masks to bit 0; result must clear bit 0.
  {"btr_reg_mask32",
   "long f(long a,long b){unsigned int x=(unsigned int)a;unsigned int n=(unsigned int)b;"
   "__asm__ volatile(\"btrl %[n],%[x]\":[x]\"+r\"(x):[n]\"r\"(n):\"cc\");"
   "return (long)x;}\n",
   {0xF, 32}, "BitTest", 0},

  // Register BTC, index 33 -> masks to bit 1; toggles bit 1.
  {"btc_reg_mask32",
   "long f(long a,long b){unsigned int x=(unsigned int)a;unsigned int n=(unsigned int)b;"
   "__asm__ volatile(\"btcl %[n],%[x]\":[x]\"+r\"(x):[n]\"r\"(n):\"cc\");"
   "return (long)x;}\n",
   {0x0, 33}, "BitTest", 0},

  // Memory BT, index 33 -> byte 4 bit 1.  arr[1] (bytes 4-7) low byte has bit 1
  // set -> CF=1.  A naive in-word shift of arr[0] would read 0.
  {"bt_mem_large",
   "long f(long a){volatile unsigned int arr[4]={0,0,0,0};arr[1]=(unsigned int)a;"
   "unsigned long cf=0;int n=33;"
   "__asm__ volatile(\"btl %[n],%[m]\\n\\tsetc %b[c]\""
   ":[c]\"+r\"(cf):[m]\"m\"(arr[0]),[n]\"r\"(n):\"cc\",\"memory\");"
   "return (long)(cf&1);}\n",
   {0x2}, "BitTest", 0},

  // Memory BTS, index 40 -> set bit 0 of byte 5 (arr[1] byte 1).
  {"bts_mem_large",
   "long f(long a){volatile unsigned int arr[4]={0,0,0,0};arr[0]=(unsigned int)a;"
   "int n=40;"
   "__asm__ volatile(\"btsl %[n],%[m]\":[m]\"+m\"(arr[0]):[n]\"r\"(n):\"cc\");"
   "return (long)arr[1];}\n",
   {0x12345678}, "BitTest", 0},

  // Memory BTR, index 41 -> clear bit 1 of byte 5 (arr[1] byte 1).
  {"btr_mem_large",
   "long f(long a){volatile unsigned int arr[4]={0,0,0,0};arr[1]=(unsigned int)a;"
   "int n=41;"
   "__asm__ volatile(\"btrl %[n],%[m]\":[m]\"+m\"(arr[0]):[n]\"r\"(n):\"cc\");"
   "return (long)arr[1];}\n",
   {0xFFFFFFFFu}, "BitTest", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BitTest, X64BitTestRT, ::testing::ValuesIn(kX64),
                         rtTCName);
