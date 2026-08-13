//===- X64_MovbeEndianRTTests.cpp - MOVBE byte-swap load/store --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 MOVBE moves a 16/32/64-bit value between a GPR and memory while reversing
// its byte order (the on-the-fly big-endian conversion compilers emit for
// `__builtin_bswap`+load/store, network byte order, file formats, ...):
//
//   movbe Gy, My     reg = byteswap(mem)        ; load form
//   movbe My, Gy     mem = byteswap(reg)        ; store form
//
// MOVBE always has exactly one memory operand (there is no reg-reg form), so the
// lifter must (a) byte-swap at the correct OPERAND width and (b) for the store
// form emit an explicit STORE — the same `ram(0)` write-back trap that bit
// SHLD/SHRD/XCHG mem destinations.  The load form additionally has to obey x86
// partial-register rules: a 32-bit `movbe r32,m32` ZEROES bits [63:32] of the
// destination, while a 16-bit `movbe r16,m16` PRESERVES bits [63:16].
//
// Despite being a real, compiler-emitted instruction, MOVBE had ZERO roundtrip
// coverage (grep finds it only in the lifter switch, never a test).  These
// probes exercise both directions across all three widths and pin the two
// partial-register corners that a width-blind COPY would silently break.
//
// Probes fold only the loaded / stored VALUE (seeded from the argument), never
// an absolute stack address, so the oracle (original-Unicorn vs lifted-Unicorn)
// is address-independent.  MOVBE is in qemu64's CPUID_EXT_MOVBE, so it decodes
// natively on the default Unicorn x86-64 CPU.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MovbeEndianRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MovbeEndianRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== Load form: reg = byteswap(mem). =====
  // 32-bit: load 4 bytes, reverse them.
  {"movbe_load32",
   "unsigned long f(unsigned long a){\n"
   "  volatile unsigned m=(unsigned)a;\n"
   "  unsigned r;\n"
   "  __asm__ volatile(\"movbe %1, %0\":\"=r\"(r):\"m\"(m):);\n"
   "  return r;}\n",
   {0x123456789ABCDEF0ULL}, "MovbeEndian", 0, "-mmovbe"},

  // 64-bit: load 8 bytes, reverse them.
  {"movbe_load64",
   "unsigned long f(unsigned long a){\n"
   "  volatile unsigned long m=a*0x0102030405060708ULL+0x1122334455667788ULL;\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"movbe %1, %0\":\"=r\"(r):\"m\"(m):);\n"
   "  return r;}\n",
   {0x0BADC0DECAFEF00DULL}, "MovbeEndian", 0, "-mmovbe"},

  // 16-bit: load 2 bytes, reverse them.  Destination's upper bits PRESERVED.
  // r seeded with 0xCAFE in bits [31:16]; movbe %w0 writes only [15:0], so the
  // recompiled run must keep 0xCAFE there (a width-blind COPY that clobbers the
  // upper half is RED).
  {"movbe_load16_preserve",
   "unsigned long f(unsigned long a){\n"
   "  volatile unsigned short m=(unsigned short)a;\n"
   "  unsigned r=0xCAFE0000u;\n"
   "  __asm__ volatile(\"movbe %1, %w0\":\"+r\"(r):\"m\"(m):);\n"
   "  return r;}\n",
   {0x000000000000ABCDULL}, "MovbeEndian", 0, "-mmovbe"},

  // 32-bit zero-extend: movbe r32,m32 must ZERO bits [63:32].  Destination is
  // pre-loaded with all ones; the full 64-bit register is returned.  If the lift
  // fails to clear the upper half the recompiled value carries 0xFFFFFFFF there.
  {"movbe_load32_zeroupper",
   "unsigned long f(unsigned long a){\n"
   "  volatile unsigned m=(unsigned)a;\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"movq $-1, %0\\n\\tmovbe %1, %k0\"\n"
   "    :\"=&r\"(r):\"m\"(m):);\n"
   "  return r;}\n",
   {0x00000000DEADBEEFULL}, "MovbeEndian", 0, "-mmovbe"},

  // Indexed addressing [base + index*4]: exercises the memory-operand address
  // computation (base + scaled index), not just a plain [rbp-disp] slot.
  {"movbe_load32_indexed",
   "unsigned long f(unsigned long a){\n"
   "  volatile unsigned buf[4];\n"
   "  buf[0]=0x10111213u; buf[1]=0x20212223u;\n"
   "  buf[2]=0x30313233u+(unsigned)a; buf[3]=0x40414243u;\n"
   "  unsigned r; unsigned long i=2;\n"
   "  __asm__ volatile(\"movbe (%1,%2,4), %0\"\n"
   "    :\"=r\"(r):\"r\"(buf),\"r\"(i):\"memory\");\n"
   "  return r;}\n",
   {0x0000000000BEEF01ULL}, "MovbeEndian", 0, "-mmovbe"},

  // ===== Store form: mem = byteswap(reg).  RED if the STORE is dropped. =====
  // 32-bit store.
  {"movbe_store32",
   "unsigned long f(unsigned long a){\n"
   "  unsigned v=(unsigned)a;\n"
   "  volatile unsigned m=0;\n"
   "  __asm__ volatile(\"movbe %1, %0\":\"=m\"(m):\"r\"(v):);\n"
   "  return m;}\n",
   {0x123456789ABCDEF0ULL}, "MovbeEndian", 0, "-mmovbe"},

  // 64-bit store.
  {"movbe_store64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long v=a*0x0102030405060708ULL+0x1122334455667788ULL;\n"
   "  volatile unsigned long m=0;\n"
   "  __asm__ volatile(\"movbe %1, %0\":\"=m\"(m):\"r\"(v):);\n"
   "  return m;}\n",
   {0x0BADC0DECAFEF00DULL}, "MovbeEndian", 0, "-mmovbe"},

  // 16-bit store: only 2 bytes are reversed and written; the surrounding cell
  // (initialized to 0) bounds the write width.
  {"movbe_store16",
   "unsigned long f(unsigned long a){\n"
   "  unsigned short v=(unsigned short)a;\n"
   "  volatile unsigned short m=0;\n"
   "  __asm__ volatile(\"movbe %w1, %0\":\"=m\"(m):\"r\"(v):);\n"
   "  return (unsigned long)m;}\n",
   {0x000000000000ABCDULL}, "MovbeEndian", 0, "-mmovbe"},

  // ===== Round-trip identity: store then load back must recover the value. ====
  // byteswap(byteswap(x)) == x, exercising both directions in one function.
  {"movbe_store_then_load64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long v=a*0x9E3779B97F4A7C15ULL+0x2545F4914F6CDD1DULL;\n"
   "  volatile unsigned long m=0; unsigned long r;\n"
   "  __asm__ volatile(\"movbe %2, %1\\n\\tmovbe %1, %0\"\n"
   "    :\"=r\"(r),\"+m\"(m):\"r\"(v):);\n"
   "  return r;}\n",
   {0x0123456789ABCDEFULL}, "MovbeEndian", 0, "-mmovbe"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(MovbeEndian, X64MovbeEndianRT,
                         ::testing::ValuesIn(kX64), rtTCName);
