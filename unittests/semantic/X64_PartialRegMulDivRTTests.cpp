//===- X64_PartialRegMulDivRTTests.cpp - partial reg + mul/div -*- C++ -*-===//
//
// x86-64 sub-register write *merge* semantics, the one-operand 8/16-bit
// MUL/IMUL/DIV/IDIV forms (DX:AX / AX split), and the high-byte registers
// (AH/BH/CH/DH).  The existing X64_Word16OpRTTests only ever drives 16/8-bit
// arithmetic through *C-level* expressions, so the compiler keeps everything in
// full registers and these instruction-level edge cases were never reached:
//
//   * Writing AX/AL/AH must MERGE into RAX (preserve the untouched bytes), not
//     zero the upper bits — only a 32-bit (EAX) write zeroes RAX[63:32].
//   * `mul/imul r/m8`  -> AX = AL * src           (result in AX, not DL:AL)
//   * `mul/imul r/m16` -> DX:AX = AX * src         (high half in DX)
//   * `div/idiv r/m8`  -> AL = AX / src, AH = rem  (dividend AX, not DX:AX)
//   * `div/idiv r/m16` -> AX = DX:AX / src, DX rem
//   * AH/BH/CH/DH live at byte offset 1 of their parent (`add %ah,%al` etc.).
//
// Each probe folds the post-instruction register state into the return value so
// the harness (which only compares RAX) actually observes the behavior.  A lift
// bug in the sub-register merge, the AX/DX:AX split, or the high-byte offset
// shows up as a return-value mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PartialRegMulDivRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PartialRegMulDivRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== Sub-register write MERGE (preserve untouched bytes). =====
  // movw writes only the low 16 bits; RAX[63:16] must be preserved.
  {"movw_merge",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r=a;\n"
   "  __asm__ volatile(\"movw $0x1234, %w0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCCDDDDULL}, "PartialRegMulDiv"},

  // movb writes only the low 8 bits; RAX[63:8] preserved.
  {"movb_merge",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r=a;\n"
   "  __asm__ volatile(\"movb $0x99, %b0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCCDDDDULL}, "PartialRegMulDiv"},

  // movb to the HIGH byte (AH/BH/CH/DH): only byte 1 changes.
  {"movb_high_merge",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r=a;\n"
   "  __asm__ volatile(\"movb $0x77, %h0\":\"+Q\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCCDDDDULL}, "PartialRegMulDiv"},

  // addw wraps within 16 bits; carry must NOT bleed into RAX[16] and the upper
  // 48 bits stay put.
  {"addw_wrap_merge",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r=a;\n"
   "  __asm__ volatile(\"addw $1, %w0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0x111122223333FFFFULL}, "PartialRegMulDiv"},

  // negw of the low 16 bits; upper preserved.
  {"negw_merge",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r=a;\n"
   "  __asm__ volatile(\"negw %w0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCC0001ULL}, "PartialRegMulDiv"},

  // notb of the low byte; upper preserved.
  {"notb_merge",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r=a;\n"
   "  __asm__ volatile(\"notb %b0\":\"+r\"(r)::\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCCDD00ULL}, "PartialRegMulDiv"},

  // 32-bit write (bswap %eax) zeroes RAX[63:32].
  {"bswap32_zeroupper",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\tbswap %%eax\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"cc\");\n"
   "  return r;}\n",
   {0xDEADBEEF11223344ULL}, "PartialRegMulDiv"},

  // ===== High-byte register arithmetic (byte offset 1). =====
  // add %ah,%al : (a & ~0xFF) | ((al + ah) & 0xFF)
  {"add_ah_al",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\taddb %%ah,%%al\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCC34F0ULL}, "PartialRegMulDiv"},

  // xchg %ah,%al : swap byte0 and byte1 of RAX, upper preserved.
  {"xchg_ah_al",
   "unsigned long f(unsigned long a){\n"
   "  unsigned long r;\n"
   "  __asm__ volatile(\"movq %1,%%rax\\n\\txchg %%ah,%%al\\n\\tmovq %%rax,%0\"\n"
   "    :\"=r\"(r):\"r\"(a):\"rax\",\"cc\");\n"
   "  return r;}\n",
   {0xAAAABBBBCCCC12ABULL}, "PartialRegMulDiv"},

  // ===== One-operand 8-bit MUL/IMUL: result in AX. =====
  {"mulb_ax",
   "unsigned long f(unsigned long a){\n"
   "  unsigned char x=(unsigned char)a, y=(unsigned char)(a>>8);\n"
   "  unsigned short ax;\n"
   "  __asm__ volatile(\"mulb %2\":\"=a\"(ax):\"a\"(x),\"q\"(y):\"cc\");\n"
   "  return ax;}\n",
   {0x000000000000C37DULL}, "PartialRegMulDiv"},

  {"imulb_ax",
   "unsigned long f(unsigned long a){\n"
   "  signed char x=(signed char)a, y=(signed char)(a>>8);\n"
   "  unsigned short ax;\n"
   "  __asm__ volatile(\"imulb %2\":\"=a\"(ax):\"a\"(x),\"q\"(y):\"cc\");\n"
   "  return (unsigned short)ax;}\n",
   {0x000000000000B9E6ULL}, "PartialRegMulDiv"},

  // ===== One-operand 16-bit MUL/IMUL: DX:AX split. =====
  {"mulw_dxax",
   "unsigned long f(unsigned long a){\n"
   "  unsigned short x=(unsigned short)a, y=(unsigned short)(a>>16);\n"
   "  unsigned short dx, ax;\n"
   "  __asm__ volatile(\"mulw %3\":\"=a\"(ax),\"=d\"(dx):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  return ((unsigned long)dx<<16)|ax;}\n",
   {0x00000000ABCD1234ULL}, "PartialRegMulDiv"},

  {"imulw_dxax",
   "unsigned long f(unsigned long a){\n"
   "  short x=(short)a, y=(short)(a>>16);\n"
   "  unsigned short dx, ax;\n"
   "  __asm__ volatile(\"imulw %3\":\"=a\"(ax),\"=d\"(dx):\"a\"(x),\"r\"(y):\"cc\");\n"
   "  return ((unsigned long)(unsigned short)dx<<16)|(unsigned short)ax;}\n",
   {0x00000000F1238ED5ULL}, "PartialRegMulDiv"},

  // ===== One-operand 8-bit DIV/IDIV: AX / src -> AL quot, AH rem. =====
  // Keep the dividend < 256 (AH=0) so the quotient always fits in AL.
  {"divb_al_ah",
   "unsigned long f(unsigned long a){\n"
   "  unsigned short num=(unsigned char)a;\n"
   "  unsigned char den=(unsigned char)((a>>8)|1);\n"
   "  unsigned short res;\n"
   "  __asm__ volatile(\"divb %2\":\"=a\"(res):\"a\"(num),\"q\"(den):\"cc\");\n"
   "  return res;}\n",
   {0x000000000000FB07ULL}, "PartialRegMulDiv"},

  // Signed: dividend = sext(int8) in [-128,127], divisor nonzero.
  {"idivb_al_ah",
   "unsigned long f(unsigned long a){\n"
   "  short num=(signed char)a;\n"
   "  signed char den=(signed char)((a>>8)|1);\n"
   "  unsigned short res;\n"
   "  __asm__ volatile(\"idivb %2\":\"=a\"(res):\"a\"(num),\"q\"(den):\"cc\");\n"
   "  return (unsigned short)res;}\n",
   {0x000000000000F19BULL}, "PartialRegMulDiv"},

  // ===== One-operand 16-bit DIV/IDIV: DX:AX / src -> AX quot, DX rem. =====
  // Force DX < divisor so the quotient fits in 16 bits.
  {"divw_ax_dx",
   "unsigned long f(unsigned long a){\n"
   "  unsigned short den=(unsigned short)((a>>32)|0x8000u);\n"
   "  unsigned short dxv=(unsigned short)((a>>16)&0x7FFFu);\n"
   "  unsigned short axv=(unsigned short)a;\n"
   "  unsigned short q,r;\n"
   "  __asm__ volatile(\"divw %4\":\"=a\"(q),\"=d\"(r)\n"
   "    :\"a\"(axv),\"d\"(dxv),\"r\"(den):\"cc\");\n"
   "  return ((unsigned long)r<<16)|q;}\n",
   {0x00001357ABCD2468ULL}, "PartialRegMulDiv"},

  // Signed: dividend = sext(int16), divisor nonzero.
  {"idivw_ax_dx",
   "unsigned long f(unsigned long a){\n"
   "  short axv=(short)a;\n"
   "  short dxv=(short)(axv>>15);\n"
   "  short den=(short)((a>>16)|1);\n"
   "  short q,r;\n"
   "  __asm__ volatile(\"idivw %4\":\"=a\"(q),\"=d\"(r)\n"
   "    :\"a\"(axv),\"d\"(dxv),\"r\"(den):\"cc\");\n"
   "  return ((unsigned long)(unsigned short)r<<16)|(unsigned short)q;}\n",
   {0x000000003E8FB1C5ULL}, "PartialRegMulDiv"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PartialRegMulDiv, X64PartialRegMulDivRT,
                         ::testing::ValuesIn(kX64), rtTCName);
