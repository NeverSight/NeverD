//===- AArch64_GPRSubRegAliasRTTests.cpp - GP sub-register alias stress ====//
//
// AArch64 dual of X86_PartialRegAliasRTTests, for the general-purpose register
// sub-register aliasing class that historically bit the optimizer (a narrow
// write folded wrong against a wide read).  AArch64 has no merging byte/half
// sub-registers like x86 AL/AH; instead the trap is:
//
//   - a 32-bit (Wn) write ZERO-extends into Xn -- the upper 32 bits MUST clear
//   - UXTB/UXTH zero-extend, SXTB/SXTH/SXTW sign-extend a sub-field
//   - BFI/BFXIL MERGE a field while PRESERVING the surrounding destination bits
//   - 32-bit flag-setting ALU (adds/subs Wn) computes flags from the 32-bit
//     result yet the register result still zero-extends
//
// Each probe drives the pattern in a loop-carried accumulation so the value
// flows through the NeverD optimizer (ON) every iteration -- a single-shot
// op may not trip a folding pass, a carried one does.  Hard-coded scratch
// registers x9-x12 (caller-saved) with explicit clobbers force the exact
// sub-register stream clang never emits from plain C.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64GPRSubRegRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64GPRSubRegRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // 32-bit add into Wn zero-extends Xn: upper 32 bits of x9 must clear.
  {"w_add_zext",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tadd w9,w9,#0x21\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\",\"cc\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0xAABBCCDDEEFF1122ULL}, "GPRSubReg", 2},

  // Plain 32-bit mov into Wn zero-extends Xn.
  {"w_mov_zext",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tmov w9,#0x9ABC\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x1122334455667788ULL}, "GPRSubReg", 2},

  // UXTB: zero-extend the low byte (clears everything above bit 7).
  {"uxtb_wide",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tuxtb w9,w9\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0xDEADBEEF12345678ULL}, "GPRSubReg", 2},

  // SXTB: sign-extend the low byte all the way to 64 bits.
  {"sxtb_wide",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tsxtb x9,w9\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x11223344556680F0ULL}, "GPRSubReg", 2},

  // SXTH: sign-extend the low halfword to 64 bits.
  {"sxth_wide",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tsxth x9,w9\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x1122334455668F70ULL}, "GPRSubReg", 2},

  // SXTW: sign-extend the low 32 bits to 64 (the W->X sign-extend form).
  {"sxtw_wide",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tsxtw x9,w9\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x11223344F5667788ULL}, "GPRSubReg", 2},

  // BFI: insert an 8-bit field at bit 16, PRESERVE all other destination bits.
  {"bfi_merge",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tmov x10,#0xA5\\n\\tbfi x9,x10,#16,#8\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\",\"x10\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0xFEDCBA9876543210ULL}, "GPRSubReg", 2},

  // 32-bit adds sets flags from the 32-bit result; carry-out captured by cset,
  // and the Wn result still zero-extends Xn.
  {"w_adds_carry",
   "long f(long a){ unsigned long acc=(unsigned long)a,r,c;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%2\\n\\tmov w10,#0x40\\n\\tadds w9,w9,w10\\n\\t"
   "cset x11,cs\\n\\tmov %0,x9\\n\\tmov %1,x11\"\n"
   "      :\"=r\"(r),\"=r\"(c):\"r\"(acc):\"x9\",\"x10\",\"x11\",\"cc\");\n"
   "    acc=acc*131u+r+c*7u+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x99887766FFFFFFE0ULL}, "GPRSubReg", 2},

  // Conditional negate on a 32-bit signed compare (cneg), result widened.
  {"w_cneg_mi",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tsubs w10,w9,#5\\n\\tcneg x11,x9,mi\\n\\tmov %0,x11\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\",\"x10\",\"x11\",\"cc\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x0102030405060708ULL}, "GPRSubReg", 2},

  // UBFX extract then a wide op: extract bits [20:8] (12 bits) zero-extended.
  {"ubfx_then_wide",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\tubfx x10,x9,#8,#12\\n\\tadd x10,x10,x9\\n\\tmov %0,x10\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\",\"x10\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0x1111222233334444ULL}, "GPRSubReg", 2},

  // W-write zero-extend then a 64-bit read that depends on the cleared upper.
  {"w_then_x_dep",
   "long f(long a){ unsigned long acc=(unsigned long)a,r;\n"
   "  for(int i=0;i<64;i++){\n"
   "    __asm__ volatile(\"mov x9,%1\\n\\torr w9,w9,#0xF\\n\\teor x9,x9,%1\\n\\tmov %0,x9\"\n"
   "      :\"=r\"(r):\"r\"(acc):\"x9\");\n"
   "    acc=acc*131u+r+(unsigned)i; }\n"
   "  return (long)acc; }\n",
   {0xCAFEBABEDEADF00DULL}, "GPRSubReg", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(GPRSubReg, A64GPRSubRegRT,
                         ::testing::ValuesIn(kA64), rtTCName);
