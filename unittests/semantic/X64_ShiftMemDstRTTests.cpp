//===- X64_ShiftMemDstRTTests.cpp - SHLD/SHRD/RCL/RCR mem dest --*- C++ -*-===//
//
// x86 SHLD/SHRD/RCL/RCR all have a `r/m` (memory) DESTINATION form:
//
//   SHLD m, r, cnt   m = (m << cnt) | (r >> (bits-cnt))     ; write back to m
//   SHRD m, r, cnt   m = (m >> cnt) | (r << (bits-cnt))     ; write back to m
//   RCL  m, cnt      rotate m left  through CF               ; write back to m
//   RCR  m, cnt      rotate m right through CF               ; write back to m
//
// The single shifts/rotates (SHL/SHR/SAR/ROL/ROR) and INC/DEC/NEG/NOT all guard
// a memory destination with `MemDst ? makeTemp : operandWrite` + an explicit
// storeToMem().  SHLD/SHRD/RCL/RCR did NOT: they emitted the result straight
// into operandWrite(operands[0]), which for a memory operand is a discarded
// `ram(0)` placeholder — so `shld [mem],r,cnt` (and the SHRD/RCL/RCR siblings)
// computed the right value, set flags correctly, then SILENTLY DROPPED the
// memory write-back.  The destination cell was left unchanged.
//
// This is the same class of bug already fixed for XCHG [mem],reg and the
// MOVHPS/MOVLPS partial stores — the double-shift / rotate-through-carry memory
// forms were simply never given roundtrip coverage (the existing
// X64_DoubleShiftFlagsRTTests only ever uses a REGISTER destination "+r").
//
// Each probe seeds a stack cell, runs the instruction with a memory destination,
// and folds the post-op cell into the return value.  With the dropped store the
// recompiled function returns the ORIGINAL cell (RED); the fixed lifter writes
// the rotated/shifted value back and matches the native run (GREEN).  Register-
// destination controls guard against an over-broad fix.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ShiftMemDstRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ShiftMemDstRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== SHLD, memory destination (RED: write-back dropped). =====
  // 64-bit, immediate count.
  {"shld_mem_imm64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long mem=a; unsigned long src=a*0xABCDEFu+0x12345u;\n"
   "  __asm__ volatile(\"shldq $7, %1, %0\":\"+m\"(mem):\"r\"(src):\"cc\");\n"
   "  return mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  // 64-bit, CL count (same write-back path, exercises the variable-count form).
  {"shld_mem_cl64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long mem=a; unsigned long src=a*0x9E3779B9u+1u;\n"
   "  __asm__ volatile(\"shldq %%cl, %1, %0\"\n"
   "    :\"+m\"(mem):\"r\"(src),\"c\"(11u):\"cc\");\n"
   "  return mem;}\n",
   {0x0F1E2D3C4B5A6978ULL}, "ShiftMemDst"},

  // 32-bit, immediate count.
  {"shld_mem_imm32",
   "unsigned long f(unsigned long a){\n"
   "  unsigned int mem=(unsigned)a; unsigned int src=(unsigned)(a>>13)*0x85u+3u;\n"
   "  __asm__ volatile(\"shldl $5, %1, %0\":\"+m\"(mem):\"r\"(src):\"cc\");\n"
   "  return (unsigned long)mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  // ===== SHRD, memory destination (RED: write-back dropped). =====
  {"shrd_mem_imm64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long mem=a; unsigned long src=a*0xC2B2AE35u+7u;\n"
   "  __asm__ volatile(\"shrdq $9, %1, %0\":\"+m\"(mem):\"r\"(src):\"cc\");\n"
   "  return mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  {"shrd_mem_cl64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long mem=a; unsigned long src=a*0x27D4EB2Fu+5u;\n"
   "  __asm__ volatile(\"shrdq %%cl, %1, %0\"\n"
   "    :\"+m\"(mem):\"r\"(src),\"c\"(13u):\"cc\");\n"
   "  return mem;}\n",
   {0x0F1E2D3C4B5A6978ULL}, "ShiftMemDst"},

  {"shrd_mem_imm32",
   "unsigned long f(unsigned long a){\n"
   "  unsigned int mem=(unsigned)a; unsigned int src=(unsigned)(a>>7)*0x9Du+1u;\n"
   "  __asm__ volatile(\"shrdl $6, %1, %0\":\"+m\"(mem):\"r\"(src):\"cc\");\n"
   "  return (unsigned long)mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  // ===== RCL, memory destination (RED: write-back dropped). =====
  // CF cleared first so the rotate-through-carry result is deterministic.
  {"rcl_mem_imm64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long mem=a;\n"
   "  __asm__ volatile(\"clc\\n\\trclq $5, %0\":\"+m\"(mem)::\"cc\");\n"
   "  return mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  {"rcl_mem_imm32",
   "unsigned long f(unsigned long a){\n"
   "  unsigned int mem=(unsigned)a;\n"
   "  __asm__ volatile(\"clc\\n\\trcll $3, %0\":\"+m\"(mem)::\"cc\");\n"
   "  return (unsigned long)mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  // ===== RCR, memory destination (RED: write-back dropped). =====
  // CF set first (stc) so the high bit rotated in is a known 1.
  {"rcr_mem_imm64",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long mem=a;\n"
   "  __asm__ volatile(\"stc\\n\\trcrq $5, %0\":\"+m\"(mem)::\"cc\");\n"
   "  return mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  {"rcr_mem_imm32",
   "unsigned long f(unsigned long a){\n"
   "  unsigned int mem=(unsigned)a;\n"
   "  __asm__ volatile(\"stc\\n\\trcrl $3, %0\":\"+m\"(mem)::\"cc\");\n"
   "  return (unsigned long)mem;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  // ===== Register-destination controls (already correct; guard the fix). =====
  {"shld_reg_ctl",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long d=a; unsigned long src=a*0x100000001u+9u;\n"
   "  __asm__ volatile(\"shldq $7, %1, %0\":\"+r\"(d):\"r\"(src):\"cc\");\n"
   "  return d;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},

  {"rcr_reg_ctl",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long d=a;\n"
   "  __asm__ volatile(\"stc\\n\\trcrq $5, %0\":\"+r\"(d)::\"cc\");\n"
   "  return d;}\n",
   {0x123456789ABCDEF0ULL}, "ShiftMemDst"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftMemDst, X64ShiftMemDstRT,
                         ::testing::ValuesIn(kX64), rtTCName);
