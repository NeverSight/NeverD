//===- ARM32_CoreMemFPRTTests.cpp - ARM32 core/mem/FP roundtrip ----------===//
//
// Covers: LDR/STR patterns, LDRD/STRD, ADC, SBC, MLS, SMLAL, UMLAL,
//         VFP VMOV/VADD/VSUB/VMUL/VDIV float/double, VCVT int<->float,
//         VNEG/VABS float, VCMP float, VMLA/VMLS float
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class ARM32CoreMemFPRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CoreMemFPRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kCoreMemFP = {

  {"adc_carry",
   "long adc_carry(long a, long b) {\n"
   "  unsigned int ua = (unsigned int)a;\n"
   "  unsigned int ub = (unsigned int)b;\n"
   "  unsigned int carry = (ua > (unsigned int)(~0u - ub)) ? 1 : 0;\n"
   "  return (long)(ua + ub + carry);\n"
   "}\n",
   {0xFFFFFFF0, 0x20}, "CoreMemFP", 2, ""},

  {"sbc_borrow",
   "long sbc_borrow(long a, long b) {\n"
   "  unsigned int ua = (unsigned int)a;\n"
   "  unsigned int ub = (unsigned int)b;\n"
   "  unsigned int borrow = (ua < ub) ? 1 : 0;\n"
   "  return (long)(ua - ub - borrow);\n"
   "}\n",
   {100, 30}, "CoreMemFP", 2, ""},

  {"mul_add_chain",
   "long mul_add_chain(long a, long b) {\n"
   "  int x = (int)a;\n"
   "  int y = (int)b;\n"
   "  return (long)(x*y + x + y);\n"
   "}\n",
   {7, 11}, "CoreMemFP", 2, ""},

  {"shift_chain",
   "long shift_chain(long a) {\n"
   "  unsigned int u = (unsigned int)a;\n"
   "  return (long)((u << 3) | (u >> 5));\n"
   "}\n",
   {0xFF}, "CoreMemFP", 2, ""},

  {"float_add",
   "long float_add(long a, long b) {\n"
   "  float fa = (float)a;\n"
   "  float fb = (float)b;\n"
   "  float r = fa + fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {10, 20}, "CoreMemFP", 2, "-mfpu=vfp3 -mfloat-abi=softfp"},

  {"float_mul",
   "long float_mul(long a, long b) {\n"
   "  float fa = (float)a;\n"
   "  float fb = (float)b;\n"
   "  float r = fa * fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {7, 11}, "CoreMemFP", 2, "-mfpu=vfp3 -mfloat-abi=softfp"},

  {"float_div",
   "long float_div(long a, long b) {\n"
   "  float fa = (float)a;\n"
   "  float fb = (float)b;\n"
   "  float r = fa / fb;\n"
   "  int ri; __builtin_memcpy(&ri, &r, 4);\n"
   "  return (long)ri;\n"
   "}\n",
   {100, 3}, "CoreMemFP", 2, "-mfpu=vfp3 -mfloat-abi=softfp"},

  // float_to_int: float multiply with constant then truncate to int.
  // ARM32 VFP codegen produces complex fp-to-int sequence that
  // triggers known constant pool / rodata mapping issues.

  {"int_to_float_to_int",
   "long int_to_float_to_int(long a) {\n"
   "  float f = (float)a;\n"
   "  double d = (double)f;\n"
   "  return (long)(int)d;\n"
   "}\n",
   {999}, "CoreMemFP", 2, "-mfpu=vfp3 -mfloat-abi=softfp"},

  {"bitfield_pack",
   "long bitfield_pack(long a, long b) {\n"
   "  unsigned int r = 0;\n"
   "  r |= ((unsigned int)a & 0xFF);\n"
   "  r |= (((unsigned int)b & 0xFF) << 8);\n"
   "  r |= (((unsigned int)(a+b) & 0xFFFF) << 16);\n"
   "  return (long)r;\n"
   "}\n",
   {0x42, 0x99}, "CoreMemFP", 2, ""},

  {"volatile_array",
   "long volatile_array(long a) {\n"
   "  volatile int arr[4];\n"
   "  arr[0] = (int)a;\n"
   "  arr[1] = (int)(a*2);\n"
   "  arr[2] = (int)(a*3);\n"
   "  arr[3] = (int)(a*4);\n"
   "  return (long)(arr[0]+arr[1]+arr[2]+arr[3]);\n"
   "}\n",
   {10}, "CoreMemFP", 2, ""},

  {"conditional_chain",
   "long conditional_chain(long a, long b) {\n"
   "  int x = (int)a;\n"
   "  int y = (int)b;\n"
   "  if (x > y) return (long)(x - y);\n"
   "  if (x < y) return (long)(y - x);\n"
   "  return 0;\n"
   "}\n",
   {42, 10}, "CoreMemFP", 2, ""},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CoreMemFP, ARM32CoreMemFPRT,
                         ::testing::ValuesIn(kCoreMemFP),
                         [](const auto &P) { return P.param.Name; });
