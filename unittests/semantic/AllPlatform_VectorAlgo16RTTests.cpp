//===- AllPlatform_VectorAlgo16RTTests.cpp - FP convert/compare -*- C++ -*-===//
//
// Sixteenth batch of clang -O2 algorithm probes.  Focuses on floating-point
// conversion and comparison corners distinct from VectorAlgo8/10: unsigned
// f32<->u32 conversion (fcvtzu/ucvtf), f64<->i64, negative-value min/max/abs,
// copysign, conditional select on FP and exact f32 division.  Values stay small
// integers held exactly in IEEE so original/recompiled fold bit-identically.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo16RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo16RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo16RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo16RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo16RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo16RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec16TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned u32 -> f32 -> u32 round trip (ucvtf + fcvtzu).
    {p+"_u2f2u",
     t+" "+p+"_u2f2u("+t+" a) {\n"
     "  unsigned v[48]; unsigned s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(unsigned)(a*(i+1)) & 0xFFFFF;\n"
     "  for (int i=0;i<48;i++){ float f=(float)v[i]; s += (unsigned)(f*0.25f); }\n"
     "  return (int)s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo16", opt, fl},

    // Signed i32 -> f32 with negatives, scaled, back to int (scvtf/fcvtzs).
    {p+"_s2f2s",
     t+" "+p+"_s2f2s("+t+" a) {\n"
     "  int v[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(int)(a*(i+1)) - (i*5000);\n"
     "  for (int i=0;i<48;i++){ float f=(float)v[i]*0.5f; s += (int)f; }\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo16", opt, fl},

    // f64 arithmetic via i32<->f64 (vcvt; ARM32 i64<->f64 is a __aeabi library
    // call with no hardware instruction, so keep the conversion 32-bit).
    {p+"_f64conv",
     t+" "+p+"_f64conv("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i=0;i<32;i++) v[i]=(int)(a*(i+1)) & 0xFFFFF;\n"
     "  for (int i=0;i<32;i++){ double d=(double)v[i]*1.5 + 2.0; s += (int)d; }\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo16", opt, fl},

    // f32 min/max over negatives (ensures FP-aware compare, not integer).
    {p+"_fminmaxneg",
     t+" "+p+"_fminmaxneg("+t+" a) {\n"
     "  float v[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(float)((int)(a*(i+1)) - (i*333));\n"
     "  float mn=v[0], mx=v[0];\n"
     "  for (int i=1;i<48;i++){ if (v[i]<mn)mn=v[i]; if (v[i]>mx)mx=v[i]; }\n"
     "  s = (int)mx - (int)mn;\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo16", opt, fl},

    // copysign: apply sign of one array to magnitude of another.
    {p+"_copysign",
     t+" "+p+"_copysign("+t+" a) {\n"
     "  float v[48], w[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(float)((int)(a*(i+1))&0xFFF); w[i]=(float)((int)(a*(i+3))-(i*100)); }\n"
     "  for (int i=0;i<48;i++){ float r=__builtin_copysignf(v[i], w[i]); s += (int)r; }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo16", opt, fl},

    // FP conditional select: r = (v<w) ? v*2 : w*2 (fcmp + select).
    {p+"_fsel",
     t+" "+p+"_fsel("+t+" a) {\n"
     "  float v[48], w[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(float)((int)(a*(i+1))&0xFFFF); w[i]=(float)((int)(a*(i+5))&0xFFFF); }\n"
     "  for (int i=0;i<48;i++){ float r=(v[i]<w[i])?(v[i]*2.0f):(w[i]*2.0f); s += (int)r; }\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo16", opt, fl},

    // Exact f32 division (power-of-two divisors stay exact).
    {p+"_fdiv2",
     t+" "+p+"_fdiv2("+t+" a) {\n"
     "  float v[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(float)((int)(a*(i+1))&0xFFFFF);\n"
     "  for (int i=0;i<48;i++){ float r=v[i]/4.0f; s += (int)r; }\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo16", opt, fl},

    // f32 abs sum over negatives (vbsl/vneg or fabs); values stay in-range so
    // the float->int conversion is exact and never saturates.
    {p+"_fabsneg",
     t+" "+p+"_fabsneg("+t+" a) {\n"
     "  float v[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(float)(((int)(a*(i+1)) & 0x1FFFF) - 0x10000);\n"
     "  for (int i=0;i<48;i++){ float r=v[i]<0?-v[i]:v[i]; s += (int)r; }\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo16", opt, fl},

    // Negate-conditional: subtract or add based on sign of a companion.
    {p+"_fnegcond",
     t+" "+p+"_fnegcond("+t+" a) {\n"
     "  float v[48], w[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(float)((int)(a*(i+1))&0x3FFF); w[i]=(float)((int)(a*(i+2))-(i*70)); }\n"
     "  for (int i=0;i<48;i++){ float r=(w[i]<0.0f)?(v[i]-w[i]):(v[i]+w[i]); s += (int)r; }\n"
     "  return s;\n"
     "}\n",
     {0x99AABBCULL}, "VectorAlgo16", opt, fl},

    // Mixed f32 multiply-add chain reduced to int.
    {p+"_fmac",
     t+" "+p+"_fmac("+t+" a) {\n"
     "  float v[48], w[48]; float acc = 0.0f;\n"
     "  for (int i=0;i<48;i++){ v[i]=(float)((int)(a*(i+1))&0x7FF); w[i]=(float)((int)(a*(i+3))&0x3F); }\n"
     "  for (int i=0;i<48;i++) acc += v[i]*w[i];\n"
     "  return (int)acc;\n"
     "}\n",
     {0xAABBCCDULL}, "VectorAlgo16", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec16 =
    makeVec16TC("x64v16", "long", 2, "-msse4.2 -fno-math-errno -fno-trapping-math");
static const std::vector<RoundTripTC> kA64Vec16 =
    makeVec16TC("a64v16", "long", 2, "-fno-math-errno -fno-trapping-math");
static const std::vector<RoundTripTC> kARM32Vec16 =
    makeVec16TC("armv16v", "int", 2, "-fno-math-errno -fno-trapping-math");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo16, X64VectorAlgo16RT,
                         ::testing::ValuesIn(kX64Vec16), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo16, A64VectorAlgo16RT,
                         ::testing::ValuesIn(kA64Vec16), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo16, ARM32VectorAlgo16RT,
                         ::testing::ValuesIn(kARM32Vec16), rtTCName);
