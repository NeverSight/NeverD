//===- AllPlatform_VectorAlgo9RTTests.cpp - 64-bit int algos ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Ninth batch of clang -O2 algorithm probes.  The first seven batches were
// 32-bit integer and the eighth was float32; this one targets 64-bit INTEGER
// arithmetic.  On ARM32 every i64 value is a register pair, so these exercise
// the carry/borrow chains (adds/adc, subs/sbc), umull+mla 64x64 multiply,
// variable i64 shifts, sbcs-based signed i64 compare and loop-carried i64
// accumulators.  On AArch64/x86 they hit native i64 plus paddq/add v.2d when
// clang vectorizes the array loops.
//
// Each algorithm folds its i64 results down to an exact 32-bit return value so
// the comparison is bit-exact between original and recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo9RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo9RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo9RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo9RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo9RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo9RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// Shared prologue: build two i64 arrays whose high halves are non-trivial so
// carries/borrows actually propagate across the 32-bit boundary on ARM32.
#define VEC9_INIT \
  "  long long b[64], c[64], r[64]; int s = 0;\n" \
  "  for (int i=0;i<64;i++){\n" \
  "    b[i]=((long long)(a*(i+1))<<21) ^ (long long)(unsigned)(i*0x9E3779B1u);\n" \
  "    c[i]=((long long)(a*(i+3))<<18) ^ (long long)(unsigned)(i*0x85EBCA77u);\n" \
  "  }\n"

static std::vector<RoundTripTC> makeVec9TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // i64 add: carry from low to high half (ARM32 adds/adc).
    {p+"_i64add",
     t+" "+p+"_i64add("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) r[i] = b[i] + c[i];\n"
     "  for (int i=0;i<64;i++){ long long x=r[i]; s ^= (int)(x ^ (x>>32)) + i; }\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo9", opt, fl},

    // i64 sub: borrow from high half (ARM32 subs/sbc).
    {p+"_i64sub",
     t+" "+p+"_i64sub("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) r[i] = b[i] - c[i];\n"
     "  for (int i=0;i<64;i++){ long long x=r[i]; s += (int)(x ^ (x>>32)); }\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo9", opt, fl},

    // i64 multiply: 64x64->64 (ARM32 umull + two mla).
    {p+"_i64mul",
     t+" "+p+"_i64mul("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) r[i] = b[i] * c[i];\n"
     "  for (int i=0;i<64;i++){ long long x=r[i]; s ^= (int)(x ^ (x>>32)) + i; }\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo9", opt, fl},

    // i64 variable left shift by (i&63).
    {p+"_i64shl",
     t+" "+p+"_i64shl("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) r[i] = b[i] << (i & 63);\n"
     "  for (int i=0;i<64;i++){ long long x=r[i]; s += (int)(x ^ (x>>32)); }\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo9", opt, fl},

    // i64 variable logical right shift (unsigned) by (i&63).
    {p+"_i64lsr",
     t+" "+p+"_i64lsr("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) r[i] = (long long)((unsigned long long)b[i] >> (i & 63));\n"
     "  for (int i=0;i<64;i++){ long long x=r[i]; s ^= (int)(x ^ (x>>32)); }\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo9", opt, fl},

    // i64 variable arithmetic right shift (signed) by (i&63).
    {p+"_i64asr",
     t+" "+p+"_i64asr("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) r[i] = b[i] >> (i & 63);\n"
     "  for (int i=0;i<64;i++){ long long x=r[i]; s += (int)(x ^ (x>>32)) + i; }\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo9", opt, fl},

    // Signed i64 compare (ARM32 subs/sbcs + conditional).
    {p+"_i64cmp",
     t+" "+p+"_i64cmp("+t+" a) {\n" VEC9_INIT
     "  for (int i=0;i<64;i++) if (b[i] < c[i]) s += i + 1;\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo9", opt, fl},

    // Loop-carried i64 accumulator (multiply-accumulate reduction).
    {p+"_i64acc",
     t+" "+p+"_i64acc("+t+" a) {\n" VEC9_INIT
     "  long long acc = 0;\n"
     "  for (int i=0;i<64;i++) acc += b[i] * (long long)(i + 1);\n"
     "  s = (int)(acc ^ (acc >> 32));\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo9", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec9 =
    makeVec9TC("x64v9", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec9 =
    makeVec9TC("a64v9", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec9 =
    makeVec9TC("armv9v", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo9, X64VectorAlgo9RT,
                         ::testing::ValuesIn(kX64Vec9), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo9, A64VectorAlgo9RT,
                         ::testing::ValuesIn(kA64Vec9), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo9, ARM32VectorAlgo9RT,
                         ::testing::ValuesIn(kARM32Vec9), rtTCName);
