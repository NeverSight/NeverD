//===- AllPlatform_RevBitfieldRTTests.cpp - rev/rbit/bitfield guards ------===//
//
// Guardrail coverage for byte/bit reversal (REV16/REV32/RBIT, ARM REV/REVSH),
// bitfield extract/insert (SBFX/UBFX/BFI/BFXIL/UBFIZ, ARM BFC), and a few x86
// 16-bit operand-size guards (IMULW flags/result, BTS register form).  All
// confirmed correct (roundtrip compares original-Unicorn vs lifted-Unicorn).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64RevBitfieldRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RevBitfieldRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RevBitfieldRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RevBitfieldRT, Verify) { roundTripARM32(GetParam()); }

class X64RevBitfieldRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RevBitfieldRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  {"rev16_x", "long f(long a){long r;__asm__(\"rev16 %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0102030405060708ULL}, "RevBitfield"},
  {"rev32_x", "long f(long a){long r;__asm__(\"rev32 %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0102030405060708ULL}, "RevBitfield"},
  {"rbit_x", "long f(long a){long r;__asm__(\"rbit %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0123456789ABCDEFULL}, "RevBitfield"},
  {"rev16_w", "long f(long a){unsigned r;__asm__(\"rev16 %w0,%w1\":\"=r\"(r):\"r\"(a));return (long)r;}\n",
   {0x11223344ULL}, "RevBitfield"},
  {"sbfx_x", "long f(long a){long r;__asm__(\"sbfx %0,%1,#4,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0000000000000F70ULL}, "RevBitfield"},
  {"ubfx_x", "long f(long a){long r;__asm__(\"ubfx %0,%1,#4,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0000000000000F70ULL}, "RevBitfield"},
  {"bfxil_x", "long f(long a,long b){long r=b;__asm__(\"bfxil %0,%1,#4,#8\":\"+r\"(r):\"r\"(a));return r;}\n",
   {0xFFFFFFFFFFFFFF70ULL, 0x1122334455667788ULL}, "RevBitfield"},
  {"bfi_x", "long f(long a,long b){long r=b;__asm__(\"bfi %0,%1,#8,#8\":\"+r\"(r):\"r\"(a));return r;}\n",
   {0x00000000000000ABULL, 0x1122334455667788ULL}, "RevBitfield"},
  {"ubfiz_w", "long f(long a){unsigned r;__asm__(\"ubfiz %w0,%w1,#4,#8\":\"=r\"(r):\"r\"(a));return (long)r;}\n",
   {0x000000FFULL}, "RevBitfield"},
};

static const std::vector<RoundTripTC> kARM32 = {
  {"rev", "long f(long a){long r;__asm__(\"rev %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x11223344ULL}, "RevBitfield"},
  {"rev16", "long f(long a){long r;__asm__(\"rev16 %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x11223344ULL}, "RevBitfield"},
  {"revsh", "long f(long a){long r;__asm__(\"revsh %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0000FF80ULL}, "RevBitfield"},
  {"rbit", "long f(long a){long r;__asm__(\"rbit %0,%1\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x0123ABCDULL}, "RevBitfield"},
  {"sbfx", "long f(long a){long r;__asm__(\"sbfx %0,%1,#4,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x00000F70ULL}, "RevBitfield"},
  {"ubfx", "long f(long a){long r;__asm__(\"ubfx %0,%1,#4,#8\":\"=r\"(r):\"r\"(a));return r;}\n",
   {0x00000F70ULL}, "RevBitfield"},
  {"bfi", "long f(long a,long b){long r=b;__asm__(\"bfi %0,%1,#8,#8\":\"+r\"(r):\"r\"(a));return r;}\n",
   {0x000000ABULL, 0x55667788ULL}, "RevBitfield"},
  {"bfc", "long f(long a){long r=a;__asm__(\"bfc %0,#8,#8\":\"+r\"(r));return r;}\n",
   {0xFFFFFFFFULL}, "RevBitfield"},
};

static const std::vector<RoundTripTC> kX64 = {
  // 16-bit imul CF: 0x4000 * 0x4 = 0x10000 overflows 16 bits -> CF=1.
  {"imulw_cf",
   "long f(long a,long b){unsigned short x=(unsigned short)a,y=(unsigned short)b;"
   "unsigned char c;__asm__(\"imulw %2,%0\\n\\tsetc %1\""
   ":\"+r\"(x),\"=&q\"(c):\"r\"(y):\"cc\");return (long)c;}\n",
   {0x4000, 4}, "RevBitfield"},
  // 16-bit imul result low 16.
  {"imulw_res",
   "long f(long a,long b){unsigned short x=(unsigned short)a,y=(unsigned short)b;"
   "__asm__(\"imulw %1,%0\":\"+r\"(x):\"r\"(y):\"cc\");"
   "return (long)(unsigned short)x;}\n",
   {0x1234, 3}, "RevBitfield"},
  // bts register form: set bit 5 of eax, fold result + old CF into return value.
  {"bts_reg",
   "long f(long a){unsigned x=(unsigned)a;unsigned char c;"
   "__asm__(\"btsl $5,%0\\n\\tsetc %1\":\"+r\"(x),\"=&q\"(c)::\"cc\");"
   "return (long)((unsigned long)x<<1|c);}\n",
   {0x00000020ULL}, "RevBitfield"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RevBitfield, A64RevBitfieldRT,
                         ::testing::ValuesIn(kA64),
                         [](const auto &I) { return I.param.Name; });
INSTANTIATE_TEST_SUITE_P(RevBitfield, ARM32RevBitfieldRT,
                         ::testing::ValuesIn(kARM32),
                         [](const auto &I) { return I.param.Name; });
INSTANTIATE_TEST_SUITE_P(RevBitfield, X64RevBitfieldRT,
                         ::testing::ValuesIn(kX64),
                         [](const auto &I) { return I.param.Name; });
