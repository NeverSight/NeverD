//===- X64_BitCountFlagsRTTests.cpp - LZCNT/TZCNT/POPCNT flags ---*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 `LZCNT`/`TZCNT`/`POPCNT` have flag and zero-input semantics that differ
// sharply from the `BSR`/`BSF` they superficially resemble:
//
//   LZCNT/TZCNT : result = count (NOT a bit index); on a ZERO source the result
//                 is the OPERAND SIZE (32 or 64) and CF is SET; CF = (src==0),
//                 ZF = (result==0), and OF/SF/AF/PF are cleared/ignored.
//   POPCNT      : ZF = (result==0); CF (and OF/SF/AF/PF) are always CLEARED,
//                 even if CF was 1 going in.
//
// (Contrast BSR/BSF, which leave the destination untouched on a zero source and
// set ZF from the SOURCE.)  The lifter lowers LZCNT via `ctlz(x, is_zero=false)`
// — so lzcnt(0)==width rather than poison — and synthesizes TZCNT as
// `popcount(~x & (x-1))`, which yields width for x==0 by construction; flags are
// emitted explicitly (ZF from the result, CF from the source, OF/SF=0).  But the
// pre-existing coverage only ever feeds NONZERO inputs and reads back the count
// value — it never drives the zero-source boundary nor verifies a single flag.
// These probes set/clear CF with `stc`/`clc`, run the op, and fold the count
// together with a `setc`/`setz`-materialized flag so both the boundary value and
// the EFLAGS effect are pinned head-on.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BitCountFlagsRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BitCountFlagsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== LZCNT 64-bit zero source: result=64, CF=1. =====
  // (BSR-style mis-lift would leave dst unchanged and CF derived elsewhere.)
  {"lzcnt_q_zero_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"lzcntq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0}, "BitCountFlags", 1, "-mlzcnt"},

  // LZCNT 64-bit nonzero: 0x100 -> 55 leading zeros, CF=0.
  {"lzcnt_q_nz_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"lzcntq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x100}, "BitCountFlags", 1, "-mlzcnt"},

  // LZCNT result==0 (top bit set) -> ZF=1.  src=0x8000... -> 0 leading zeros.
  {"lzcnt_q_result_zf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"lzcntq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {0x8000000000000000ULL}, "BitCountFlags", 1, "-mlzcnt"},

  // ===== TZCNT 64-bit zero source: result=64, CF=1. =====
  {"tzcnt_q_zero_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"tzcntq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0}, "BitCountFlags", 1, "-mbmi"},

  // TZCNT 64-bit nonzero: 0x100 -> 8 trailing zeros, CF=0.
  {"tzcnt_q_nz_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"tzcntq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0x100}, "BitCountFlags", 1, "-mbmi"},

  // TZCNT result==0 (bit 0 set) -> ZF=1.  src=0xF -> 0 trailing zeros.
  {"tzcnt_q_result_zf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"tzcntq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {0xF}, "BitCountFlags", 1, "-mbmi"},

  // ===== POPCNT zero source: result=0 -> ZF=1. =====
  {"popcnt_q_zero_zf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"popcntq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {0}, "BitCountFlags", 1, "-mpopcnt"},

  // POPCNT always CLEARS CF: pre-set CF with `stc`, run popcnt, read CF -> 0.
  // A mis-lift that leaves CF intact (or routes through a CF-setting path) -> 1.
  {"popcnt_q_clear_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, cf;\n"
   "  __asm__ volatile(\"stc\\n\\tpopcntq %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(cf&1);}\n",
   {0xFF}, "BitCountFlags", 1, "-mpopcnt"},

  // POPCNT nonzero result -> ZF=0 (fold confirms ZF cleared + count value).
  {"popcnt_q_nz_zf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long src=a, dst, zf;\n"
   "  __asm__ volatile(\"popcntq %2,%0\\n\\tsetz %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(zf):\"r\"(src):\"cc\");\n"
   "  return dst*4+(zf&1);}\n",
   {0xDEADBEEFCAFEBABEULL}, "BitCountFlags", 1, "-mpopcnt"},

  // ===== 32-bit operand-size boundary: zero source -> result is 32 (not 64). =====
  {"lzcnt_d_zero_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned src=(unsigned)a, dst; unsigned long cf;\n"
   "  __asm__ volatile(\"lzcntl %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return (unsigned long)dst*4+(cf&1);}\n",
   {0}, "BitCountFlags", 1, "-mlzcnt"},

  {"tzcnt_d_zero_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned src=(unsigned)a, dst; unsigned long cf;\n"
   "  __asm__ volatile(\"tzcntl %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return (unsigned long)dst*4+(cf&1);}\n",
   {0}, "BitCountFlags", 1, "-mbmi"},

  // 32-bit LZCNT nonzero high bit set near top: 0x40000000 -> 1 leading zero.
  {"lzcnt_d_nz_cf",
   "unsigned long f(unsigned long a){\n"
   "  unsigned src=(unsigned)a, dst; unsigned long cf;\n"
   "  __asm__ volatile(\"lzcntl %2,%0\\n\\tsetc %b1\\n\\t\"\n"
   "    :\"=r\"(dst),\"=q\"(cf):\"r\"(src):\"cc\");\n"
   "  return (unsigned long)dst*4+(cf&1);}\n",
   {0x40000000ULL}, "BitCountFlags", 1, "-mlzcnt"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BitCountFlags, X64BitCountFlagsRT,
                         ::testing::ValuesIn(kX64), rtTCName);
