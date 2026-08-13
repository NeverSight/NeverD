//===- AArch64_ExtendRegFlagsRTTests.cpp - W-form extended-reg flags ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 ADD/SUB/CMP/CMN with an EXTENDED register operand
// (`cmp w1, w2, sxtb`) in the 32-bit (W) form.  The extended operand is always
// materialised as a 64-bit sign/zero-extended value, but a W-form operation and
// its NZCV flags are 32-bit; the wider operand then forced the 32-bit first
// operand to zero-extend, so the C/V flags were computed at 64 bits instead of
// 32 -> wrong C for a signed-extended operand and wrong V whenever the 32-bit
// first operand had bit31 set.  These probes fold C (cset hs) / V (cset vs) into
// the return value to expose it; X-form and no-extend controls must stay green.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64ExtendRegFlagsRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ExtendRegFlagsRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // cmp w,w,sxtb -> C: w1=0xFFFFFFFF, sxtb(0xFF)=0xFFFFFFFF -> equal, no borrow,
  // C=1.  64-bit-wide compare (bug) sees 0xFFFFFFFF <u 0xFFFF..FF -> C=0.
  {"cmp_w_sxtb_c",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"cmp %w1,%w2,sxtb\\n\\tcset %w0,hs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)r; }\n",
   {0xFFFFFFFFULL, 0xFF}, "ExtendRegFlags"},

  // cmp w,w,sxth -> C: w1=0xFFFFFFFF, sxth(0xFFFF)=0xFFFFFFFF -> equal, C=1.
  {"cmp_w_sxth_c",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"cmp %w1,%w2,sxth\\n\\tcset %w0,hs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)r; }\n",
   {0xFFFFFFFFULL, 0xFFFF}, "ExtendRegFlags"},

  // cmp w,w,uxtb -> V: w1=0x80000000 (INT_MIN), uxtb(1)=1 -> INT_MIN-1 signed
  // overflow, V=1.  64-bit compare zero-extends w1 to a positive value -> V=0.
  {"cmp_w_uxtb_v",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"cmp %w1,%w2,uxtb\\n\\tcset %w0,vs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)r; }\n",
   {0x80000000ULL, 1}, "ExtendRegFlags"},

  // adds w,w,w,sxtb -> C: 0xFFFFFFFF + sxtb(1) = 0x1_00000000, 32-bit carry C=1.
  {"adds_w_sxtb_c",
   "long f(long a,long b){ unsigned c; unsigned res;\n"
   "  __asm__(\"adds %w0,%w2,%w3,sxtb\\n\\tcset %w1,hs\""
   ":\"=&r\"(res),\"=&r\"(c):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)c; }\n",
   {0xFFFFFFFFULL, 1}, "ExtendRegFlags"},

  // subs w,w,w,sxtb -> V: 0x80000000 - sxtb(1) = INT_MIN-1, signed overflow V=1.
  {"subs_w_sxtb_v",
   "long f(long a,long b){ unsigned v; unsigned res;\n"
   "  __asm__(\"subs %w0,%w2,%w3,sxtb\\n\\tcset %w1,vs\""
   ":\"=&r\"(res),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)v; }\n",
   {0x80000000ULL, 1}, "ExtendRegFlags"},

  // cmn w,w,sxtb (= adds XZR) -> C: 0xFFFFFFFF + sxtb(1) = carry, C=1.
  {"cmn_w_sxtb_c",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"cmn %w1,%w2,sxtb\\n\\tcset %w0,hs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)r; }\n",
   {0xFFFFFFFFULL, 1}, "ExtendRegFlags"},

  // Result correctness (already width-truncated, must stay green): add with sxtb.
  {"add_w_sxtb_res",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"add %w0,%w1,%w2,sxtb\":\"=r\"(r):\"r\"(a),\"r\"(b));\n"
   "  return (long)r; }\n",
   {1000, 0xFF}, "ExtendRegFlags"},

  // Control: X-form cmp x,x,sxtb is a genuine 64-bit compare (no bug).
  {"cmp_x_sxtb_c_ctl",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"cmp %1,%w2,sxtb\\n\\tcset %w0,hs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)r; }\n",
   {0xFFFFFFFFFFFFFFFFULL, 0xFF}, "ExtendRegFlags"},

  // Control: plain 32-bit cmp (no extend) must stay green.
  {"cmp_w_plain_c_ctl",
   "long f(long a,long b){ unsigned r;\n"
   "  __asm__(\"cmp %w1,%w2\\n\\tcset %w0,hs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");\n"
   "  return (long)r; }\n",
   {0xFFFFFFFFULL, 0xFFFFFFFFULL}, "ExtendRegFlags"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ExtendRegFlags, A64ExtendRegFlagsRT,
                         ::testing::ValuesIn(kA64),
                         [](const auto &I) { return I.param.Name; });
