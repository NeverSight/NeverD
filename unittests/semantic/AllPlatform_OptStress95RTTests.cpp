//===- AllPlatform_OptStress95RTTests.cpp - variable rotate / funnel -*-C++-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Variable-count rotates and funnel shifts are a poison minefield: the UB-safe C
// idiom `(x << n) | (x >> (W - n))` has a `x >> W` term when n==0 (a shift by the
// full width — LLVM poison if the count ever folds to a constant W).  clang -O2
// lowers the idiom to a single `rol`/`ror` (x86), `ror`/`extr` (AArch64), or
// `ror`/`mov ...,ror` (ARM); the lifter of those instructions must reproduce the
// n==0 identity exactly (rotate by 0 = unchanged), the same shift-by-bitwidth
// hazard the RCL/RCR and SHLD/SHRD fixes already chase, here on the plain rotate
// and the `__builtin_rotateleft/right` funnel.
//
// Each probe drives the rotate amount from a runtime arg (invisible to the
// optimizer) across a loop that hits amount 0 and the wrap boundary every cycle,
// folding every intermediate into the result so a single wrong rotate diverges
// from the native run.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress95RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress95RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress95RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress95RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress95RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress95RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress95RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress95RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress95TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 32-bit rotate-left by a runtime amount that sweeps 0..31 (hits the n==0
    // identity every 32 steps).  UB-safe idiom -> clang emits `rol`.
    {p+"_rotl32",
     "static unsigned rl32(unsigned x,unsigned n){ n&=31u;\n"
     "  return (x<<n)|(x>>((32u-n)&31u)); }\n"
     +t+" "+p+"_rotl32("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(unsigned i=0;i<128;i++){ x=rl32(x,i); h=h*131u+x; }\n"
     "  return ("+t+")h; }\n",
     {0x9E3779B9u}, "OptStress95", 2},

    // 32-bit rotate-right by a runtime amount sweeping 0..31.
    {p+"_rotr32",
     "static unsigned rr32(unsigned x,unsigned n){ n&=31u;\n"
     "  return (x>>n)|(x<<((32u-n)&31u)); }\n"
     +t+" "+p+"_rotr32("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(unsigned i=0;i<128;i++){ x=rr32(x,i); h=h*131u+x; }\n"
     "  return ("+t+")h; }\n",
     {0x12345678u}, "OptStress95", 2},

    // __builtin funnel/rotate intrinsics (clang lowers to the native rotate too).
    {p+"_brotl32",
     "static unsigned brl(unsigned x,unsigned n){ return __builtin_rotateleft32(x,n); }\n"
     +t+" "+p+"_brotl32("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(unsigned i=0;i<96;i++){ x=brl(x,i); h=(h^x)*16777619u; }\n"
     "  return ("+t+")h; }\n",
     {0xCAFEBABEu}, "OptStress95", 2},

    // 8-bit and 16-bit rotates (sub-word rotate amount masking is per-width).
    {p+"_rotl8",
     "static unsigned char rl8(unsigned char x,unsigned n){ n&=7u;\n"
     "  return (unsigned char)((x<<n)|(x>>((8u-n)&7u))); }\n"
     +t+" "+p+"_rotl8("+t+" a){\n"
     "  unsigned char x=(unsigned char)a|1u; unsigned h=0;\n"
     "  for(unsigned i=0;i<64;i++){ x=rl8(x,i); h=h*131u+x; }\n"
     "  return ("+t+")h; }\n",
     {0xB7u}, "OptStress95", 2},

    {p+"_rotr16",
     "static unsigned short rr16(unsigned short x,unsigned n){ n&=15u;\n"
     "  return (unsigned short)((x>>n)|(x<<((16u-n)&15u))); }\n"
     +t+" "+p+"_rotr16("+t+" a){\n"
     "  unsigned short x=(unsigned short)a|1u; unsigned h=0;\n"
     "  for(unsigned i=0;i<80;i++){ x=rr16(x,i); h=h*131u+x; }\n"
     "  return ("+t+")h; }\n",
     {0x8001u}, "OptStress95", 2},

    // Funnel shift across TWO words (true double-shift, not a rotate): the lo/hi
    // pair shifts by a runtime amount sweeping 0..31, exercising shld/shrd or the
    // AArch64/ARM extr/funnel lowering at the 0 and 32 boundaries.
    {p+"_funnel32",
     "static unsigned fn(unsigned hi,unsigned lo,unsigned n){ n&=31u;\n"
     "  if(n==0) return hi;\n"
     "  return (hi<<n)|(lo>>(32u-n)); }\n"
     +t+" "+p+"_funnel32("+t+" a){\n"
     "  unsigned hi=(unsigned)a|1u, lo=hi*2654435761u+1u, h=0;\n"
     "  for(unsigned i=0;i<128;i++){ unsigned r=fn(hi,lo,i);\n"
     "    h=h*131u+r; lo=hi; hi=r; }\n"
     "  return ("+t+")h; }\n",
     {0x0BADF00Du}, "OptStress95", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress95TC("x64o95", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress95TC("x86o95", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress95TC("a64o95", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress95TC("armo95", "int");

INSTANTIATE_TEST_SUITE_P(OptStress95, X64OptStress95RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress95, X86OptStress95RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress95, A64OptStress95RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress95, ARM32OptStress95RT, ::testing::ValuesIn(kARM), rtTCName);
