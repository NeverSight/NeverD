//===- X86_32_PartialRegMulDivRTTests.cpp - i386 partial reg + mul/div -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// i386 (32-bit) counterpart to X64_PartialRegMulDivRTTests.  The 64-bit file
// only reaches the x64 lift path, where a 32-bit register write zero-extends at
// the *lift* level.  i386 has no such quirk, so EAX writes flow through
// LowToMed (Phase A/B2) instead — the same code the cross-block SUBBYTES fix
// touched.  These probes drive instruction forms clang never emits from plain
// C, so a wrong SUBBYTES/merge or a dropped EDX:EAX half diverges from the
// original:
//
//   * `mul/imul r/m32` -> EDX:EAX = EAX * src   (full 64-bit product split)
//   * `div/idiv r/m32` -> EAX = EDX:EAX / src, EDX = rem
//   * `mul/imul r/m8`  -> AX = AL * src; `r/m16` -> DX:AX
//   * `div/idiv r/m8`  -> AL quot / AH rem; `r/m16` -> AX quot / DX rem
//   * 16/8-bit and AH (byte offset 1) writes MERGE into EAX (no zero-extend)
//   * a u64 mul loop whose low word lands in EAX via SUBBYTES, read post-loop
//
// Each probe folds the post-instruction register state into the 32-bit return
// (EAX) so the harness actually observes the behavior.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86PartialRegMulDivRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PartialRegMulDivRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86 = {

  // ===== 32-bit one-operand MUL/IMUL: EDX:EAX = EAX * src. =====
  // A lost high word (EDX) or stale low word (EAX) flips the folded return.
  {"mull_edxeax",
   "unsigned mull_edxeax(unsigned a){\n"
   "  unsigned x=a, y=a*2654435761u+1u, hi, lo;\n"
   "  __asm__ volatile(\"mull %3\":\"=a\"(lo),\"=d\"(hi):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  return hi ^ lo;}\n",
   {0x9E3779B9ULL}, "X86PartMulDiv"},

  {"imull_edxeax",
   "int imull_edxeax(int a){\n"
   "  int x=a, y=a*7-123456789;\n"
   "  unsigned hi, lo;\n"
   "  __asm__ volatile(\"imull %3\":\"=a\"(lo),\"=d\"(hi):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  return (int)(hi ^ lo);}\n",
   {0x01234567ULL}, "X86PartMulDiv"},

  // ===== 32-bit one-operand DIV/IDIV: EDX:EAX / src -> EAX quot, EDX rem. =====
  // hi(EDX) < src keeps the quotient inside 32 bits (no #DE).
  {"divl_eaxedx",
   "unsigned divl_eaxedx(unsigned a){\n"
   "  unsigned den=a|0x80000000u;\n"
   "  unsigned hi=(a>>1)&0x7FFFFFFFu;\n"
   "  unsigned lo=a*2654435761u, q, r;\n"
   "  __asm__ volatile(\"divl %4\":\"=a\"(q),\"=d\"(r):\"a\"(lo),\"d\"(hi),\"r\"(den):\"cc\");\n"
   "  return q ^ (r*31u);}\n",
   {0x0BADF00DULL}, "X86PartMulDiv"},

  // Signed: dividend = sext64(a); den=(a>>4)|1 never forces an INT_MIN/-1 trap.
  {"idivl_eaxedx",
   "int idivl_eaxedx(int a){\n"
   "  int lo=a, hi=a>>31, den=(a>>4)|1, q, r;\n"
   "  __asm__ volatile(\"idivl %4\":\"=a\"(q),\"=d\"(r):\"a\"(lo),\"d\"(hi),\"r\"(den):\"cc\");\n"
   "  return q ^ (r*31);}\n",
   {0xFFFFFF85ULL}, "X86PartMulDiv"},

  // mul then div by the same operand: quotient must recover x, remainder 0.
  {"muldiv_recover",
   "unsigned muldiv_recover(unsigned a){\n"
   "  unsigned x=a|1u, y=(a>>3)|1u, hi, lo, q, r;\n"
   "  __asm__ volatile(\"mull %3\":\"=a\"(lo),\"=d\"(hi):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  __asm__ volatile(\"divl %4\":\"=a\"(q),\"=d\"(r):\"a\"(lo),\"d\"(hi),\"r\"(y):\"cc\");\n"
   "  return q ^ (r+1u);}\n",
   {0xC0FFEE11ULL}, "X86PartMulDiv"},

  // ===== One-operand 8/16-bit MUL: AX and DX:AX through the i386 path. =====
  {"mulb_ax",
   "unsigned mulb_ax(unsigned a){\n"
   "  unsigned char x=(unsigned char)a, y=(unsigned char)(a>>8);\n"
   "  unsigned short ax;\n"
   "  __asm__ volatile(\"mulb %2\":\"=a\"(ax):\"a\"(x),\"q\"(y):\"cc\");\n"
   "  return ax;}\n",
   {0x0000C37DULL}, "X86PartMulDiv"},

  {"mulw_dxax",
   "unsigned mulw_dxax(unsigned a){\n"
   "  unsigned short x=(unsigned short)a, y=(unsigned short)(a>>16);\n"
   "  unsigned short dx, ax;\n"
   "  __asm__ volatile(\"mulw %3\":\"=a\"(ax),\"=d\"(dx):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  return ((unsigned)dx<<16)|ax;}\n",
   {0xABCD1234ULL}, "X86PartMulDiv"},

  {"imulw_dxax",
   "unsigned imulw_dxax(unsigned a){\n"
   "  short x=(short)a, y=(short)(a>>16);\n"
   "  unsigned short dx, ax;\n"
   "  __asm__ volatile(\"imulw %3\":\"=a\"(ax),\"=d\"(dx):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  return ((unsigned)(unsigned short)dx<<16)|(unsigned short)ax;}\n",
   {0xF1238ED5ULL}, "X86PartMulDiv"},

  // ===== One-operand 8/16-bit DIV: AL/AH and AX/DX splits. =====
  {"divb_al_ah",
   "unsigned divb_al_ah(unsigned a){\n"
   "  unsigned short num=(unsigned char)a;\n"
   "  unsigned char den=(unsigned char)((a>>8)|1);\n"
   "  unsigned short res;\n"
   "  __asm__ volatile(\"divb %2\":\"=a\"(res):\"a\"(num),\"q\"(den):\"cc\");\n"
   "  return res;}\n",
   {0x0000FB07ULL}, "X86PartMulDiv"},

  {"idivb_al_ah",
   "unsigned idivb_al_ah(unsigned a){\n"
   "  short num=(signed char)a;\n"
   "  signed char den=(signed char)((a>>8)|1);\n"
   "  unsigned short res;\n"
   "  __asm__ volatile(\"idivb %2\":\"=a\"(res):\"a\"(num),\"q\"(den):\"cc\");\n"
   "  return (unsigned short)res;}\n",
   {0x0000F19BULL}, "X86PartMulDiv"},

  {"divw_ax_dx",
   "unsigned divw_ax_dx(unsigned a){\n"
   "  unsigned short den=(unsigned short)((a>>16)|0x8000u);\n"
   "  unsigned short dxv=(unsigned short)((a>>1)&0x7FFFu);\n"
   "  unsigned short axv=(unsigned short)a;\n"
   "  unsigned short q,r;\n"
   "  __asm__ volatile(\"divw %4\":\"=a\"(q),\"=d\"(r):\"a\"(axv),\"d\"(dxv),\"r\"(den):\"cc\");\n"
   "  return ((unsigned)r<<16)|q;}\n",
   {0x1357ABCDULL}, "X86PartMulDiv"},

  // ===== Sub-register write MERGE into EAX (no zero-extend on i386). =====
  {"movw_merge32",
   "unsigned movw_merge32(unsigned a){\n"
   "  unsigned r=a;\n"
   "  __asm__ volatile(\"movw $0x1234, %w0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAABBCCDDULL}, "X86PartMulDiv"},

  {"movb_merge32",
   "unsigned movb_merge32(unsigned a){\n"
   "  unsigned r=a;\n"
   "  __asm__ volatile(\"movb $0x99, %b0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAABBCCDDULL}, "X86PartMulDiv"},

  // High byte (AH/BH/CH/DH) lives at byte offset 1.
  {"movb_high_merge32",
   "unsigned movb_high_merge32(unsigned a){\n"
   "  unsigned r=a;\n"
   "  __asm__ volatile(\"movb $0x77, %h0\":\"+Q\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAABBCCDDULL}, "X86PartMulDiv"},

  // 16-bit add wraps inside AX; carry must NOT bleed into bit 16.
  {"addw_wrap32",
   "unsigned addw_wrap32(unsigned a){\n"
   "  unsigned r=a;\n"
   "  __asm__ volatile(\"addw $0x20, %w0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0x1122FFF0ULL}, "X86PartMulDiv"},

  // High-byte arithmetic: add %ah,%al then read full EAX.
  {"add_ah_al32",
   "unsigned add_ah_al32(unsigned a){\n"
   "  unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\taddb %%ah,%%al\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"cc\");\n"
   "  return r;}\n",
   {0xCCCC34F0ULL}, "X86PartMulDiv"},

  // xchg %ah,%al swaps byte 0 and byte 1, upper 16 preserved.
  {"xchg_ah_al32",
   "unsigned xchg_ah_al32(unsigned a){\n"
   "  unsigned r;\n"
   "  __asm__ volatile(\"movl %1,%%eax\\n\\txchg %%ah,%%al\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"eax\",\"cc\");\n"
   "  return r;}\n",
   {0xCCCC12ABULL}, "X86PartMulDiv"},

  // ===== u64 mul loop: low word lands in EAX via SUBBYTES, read post-loop. ====
  // Directly guards the cross-block parent-register sync the Phase A fix
  // restored — clang lowers u64*u32 to i386 mull sequences here.
  {"ll_mul_loop",
   "unsigned ll_mul_loop(unsigned a){\n"
   "  unsigned long long h=a?a:1u;\n"
   "  for(int i=0;i<13;i++){ h*=131ull; h^=h>>17; h+=0x9E3779B97F4A7C15ull; }\n"
   "  return (unsigned)(h ^ (h>>32));}\n",
   {0x00012345ULL}, "X86PartMulDiv", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86PartMulDiv, X86PartialRegMulDivRT,
                         ::testing::ValuesIn(kX86), rtTCName);
