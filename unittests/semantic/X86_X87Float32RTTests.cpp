//===- X86_X87Float32RTTests.cpp - x87 32-bit float load/store -*- C++ -*-===//
//
// x87 with a 32-bit `float` memory operand (flds / fsts / fstps / fadds...).
// Every prior x87 suite used only `double` (fldl/fstpl), so the m32fp path was
// unverified: NeverD models the x87 stack as 64-bit, and `FLD m32fp` widened
// the float with FLOAT_INT2FLOAT (reinterpreting the float bits as an integer)
// while `FST/FSTP m32fp` stored the full 64-bit register with no double->float
// rounding.  Both corrupt the value; FADD's memory path already used the right
// FLOAT_FLOAT2FLOAT conversion, so FLD/FST were simply inconsistent.
//
// x87 is x86-family only, so these run on x86-64 and i386.  Each kernel folds
// its float result into the int return; inputs keep results exactly
// representable as float so the 64-bit-vs-80-bit intermediate width never
// matters and native-vs-lifted must agree bit for bit.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87F32RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87F32RT, Verify) { roundTripX64(GetParam()); }

class X86X87F32RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87F32RT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeF32(const char *prefix) {
  std::string p = prefix;
  return {
    // flds + fstps round trip: a normal float bit pattern must pass through
    // unchanged.  The old FLOAT_INT2FLOAT load turned it into garbage.
    {p+"_f32ldst",
     "int "+p+"_f32ldst(int a){ float x; __builtin_memcpy(&x,&a,4);\n"
     "  float r; __asm__ volatile(\"flds %1\\n\\tfstps %0\":\"=m\"(r):\"m\"(x));\n"
     "  int o; __builtin_memcpy(&o,&r,4); return o; }\n",
     {0x42F6E979ULL}, "X87F32", 0, "-mno-sse -mfpmath=387"},

    // fldl (double) + fstps (float): double->float narrowing on store.  The old
    // path stored the low 4 bytes of the 64-bit pattern instead of rounding.
    {p+"_f32narrow",
     "int "+p+"_f32narrow(int a){ double d=(double)(a%2000-1000)*0.5;\n"
     "  float r; __asm__ volatile(\"fldl %1\\n\\tfstps %0\":\"=m\"(r):\"m\"(d));\n"
     "  int o; __builtin_memcpy(&o,&r,4); return o; }\n",
     {0x1F5ULL}, "X87F32", 0, "-mno-sse -mfpmath=387"},

    // flds + fadds + fmuls + fstps: float memory operands through arithmetic.
    {p+"_f32arith",
     "int "+p+"_f32arith(int a){ float x=(float)(a&31), y=(float)((a>>5)&31);\n"
     "  float r; __asm__ volatile(\"flds %1\\n\\tfadds %2\\n\\tfmuls %1\\n\\t\"\n"
     "    \"fstps %0\":\"=m\"(r):\"m\"(x),\"m\"(y));\n"
     "  int o; __builtin_memcpy(&o,&r,4); return o; }\n",
     {0x35AULL}, "X87F32", 0, "-mno-sse -mfpmath=387"},

    // fildl (int->x87) + fstps (->float): integer source, float store.
    {p+"_f32fild",
     "int "+p+"_f32fild(int a){ int v=(a%1000)-500;\n"
     "  float r; __asm__ volatile(\"fildl %1\\n\\tfstps %0\":\"=m\"(r):\"m\"(v));\n"
     "  int o; __builtin_memcpy(&o,&r,4); return o; }\n",
     {0x289ULL}, "X87F32", 0, "-mno-sse -mfpmath=387"},

    // Natural clang x87 float codegen: float constants are loaded with flds from
    // the constant pool, so this hits the FLD m32fp path without inline asm.
    {p+"_f32cnat",
     "int "+p+"_f32cnat(int a){ float x=(float)(a&31); float y=x+0.5f;\n"
     "  return (int)(y*8.0f); }\n",
     {0x3D7ULL}, "X87F32", 0, "-mno-sse -mfpmath=387"},
  };
}

static const std::vector<RoundTripTC> kX64F32 = makeF32("x64");
static const std::vector<RoundTripTC> kX86F32 = makeF32("x86");
// clang-format on

INSTANTIATE_TEST_SUITE_P(X87F32, X64X87F32RT, ::testing::ValuesIn(kX64F32),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(X87F32, X86X87F32RT, ::testing::ValuesIn(kX86F32),
                         rtTCName);
