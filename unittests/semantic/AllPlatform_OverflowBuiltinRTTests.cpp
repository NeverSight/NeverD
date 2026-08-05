//===- AllPlatform_OverflowBuiltinRTTests.cpp - overflow idioms -*- C++ -*-===//
//
// clang -O2 `__builtin_{add,sub,mul}_overflow` kernels across all four targets.
// These read the overflow/carry flag produced by a genuine ADD/SUB/IMUL (not a
// CMP), then branch or select on it -- exactly the "flag from arithmetic, not a
// subtraction" path the MedFlags optimizer must leave unfolded so the emitter
// lowers INT_SOVF/INT_CARRY faithfully (a prior class of bug silently dropped
// OF and miscompiled `add;jo` / `cmn;bvs` signed-overflow idioms).  Signed and
// unsigned add/sub/mul are covered; everything is 32-bit so no 64-bit overflow
// libcall (__mulodi4) is emitted on i386/ARM32.  Each folds to an exact int.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OvfRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OvfRT, Verify) { roundTripX64(GetParam()); }

class X86OvfRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OvfRT, Verify) { roundTripX86(GetParam()); }

class A64OvfRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OvfRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32OvfRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OvfRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOvf(const char *prefix) {
  std::string p = prefix;
  return {
    // Signed add overflow: add;seto/jo reading OF after a real ADD.
    {p+"_sadd",
     "int "+p+"_sadd(int a){ int s=0;\n"
     "  for(int i=0;i<200;i++){ int x=a*(i+1)+i*131, y=(a^i)-i*7; int r;\n"
     "    int ov=__builtin_add_overflow(x,y,&r);\n"
     "    s += ov ? ((x>>1)-(y>>1)) : r; }\n"
     "  return s; }\n",
     {0x1234567ULL}, "Overflow", 2, ""},

    // Unsigned add overflow (carry): add;setc reading CF after a real ADD.
    {p+"_uadd",
     "int "+p+"_uadd(int a){ unsigned s=0;\n"
     "  for(int i=0;i<200;i++){ unsigned x=(unsigned)(a*(i+1))*2654435761u,\n"
     "      y=(unsigned)(a+i*7)*40503u; unsigned r;\n"
     "    int c=__builtin_add_overflow(x,y,&r);\n"
     "    s += c ? (x^y) : r; s=s*131u+(unsigned)c; }\n"
     "  return (int)s; }\n",
     {0x2233445ULL}, "Overflow", 2, ""},

    // Signed subtract overflow: sub;jo reading OF after a real SUB.
    {p+"_ssub",
     "int "+p+"_ssub(int a){ int s=0;\n"
     "  for(int i=0;i<200;i++){ int x=a*(i+3)-i*97, y=(a<<1)^(i*131); int r;\n"
     "    int ov=__builtin_sub_overflow(x,y,&r);\n"
     "    s += ov ? (x+y) : r; }\n"
     "  return s; }\n",
     {0x3344556ULL}, "Overflow", 2, ""},

    // Signed multiply overflow: imul;jo (x86) / smull+cmp asr (ARM) /
    // smulh-compare (AArch64).
    {p+"_smul",
     "int "+p+"_smul(int a){ int s=0;\n"
     "  for(int i=1;i<=200;i++){ int x=a*i+i, y=(a^(i*131))-i; int r;\n"
     "    int ov=__builtin_mul_overflow(x,y,&r);\n"
     "    s += ov ? (x-y) : r; s ^= (s<<1); }\n"
     "  return s; }\n",
     {0x4455667ULL}, "Overflow", 2, ""},

    // Unsigned multiply overflow: mul;setc (x86) / umulh!=0 (AArch64) /
    // umull high!=0 (ARM).
    {p+"_umul",
     "int "+p+"_umul(int a){ unsigned s=1;\n"
     "  for(int i=1;i<=200;i++){ unsigned x=(unsigned)(a+i)*2654435761u,\n"
     "      y=(unsigned)(a*i)|1u; unsigned r;\n"
     "    int c=__builtin_mul_overflow(x,y,&r);\n"
     "    s += c ? (x>>3)+(y>>5) : r; }\n"
     "  return (int)s; }\n",
     {0x5566778ULL}, "Overflow", 2, ""},

    // Chained overflow checks: the OF/CF of several ops live simultaneously and
    // feed nested conditionals -- stresses the flag-liveness/no-fold bookkeeping.
    {p+"_chain",
     "int "+p+"_chain(int a){ int s=0;\n"
     "  for(int i=0;i<160;i++){ int x=a+i, y=a*i, z=a-i*3; int r1,r2,r3;\n"
     "    int o1=__builtin_add_overflow(x,y,&r1);\n"
     "    int o2=__builtin_sub_overflow(r1,z,&r2);\n"
     "    int o3=__builtin_mul_overflow(x,z,&r3);\n"
     "    s += (o1?1:r1) + (o2?2:r2) + (o3?4:(r3&0xFF)); }\n"
     "  return s; }\n",
     {0x6677889ULL}, "Overflow", 2, ""},
  };
}

static const std::vector<RoundTripTC> kX64Ovf   = makeOvf("x64ovf");
static const std::vector<RoundTripTC> kX86Ovf   = makeOvf("x86ovf");
static const std::vector<RoundTripTC> kA64Ovf   = makeOvf("a64ovf");
static const std::vector<RoundTripTC> kARM32Ovf = makeOvf("armovf");
// clang-format on

INSTANTIATE_TEST_SUITE_P(Overflow, X64OvfRT, ::testing::ValuesIn(kX64Ovf),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Overflow, X86OvfRT, ::testing::ValuesIn(kX86Ovf),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Overflow, A64OvfRT, ::testing::ValuesIn(kA64Ovf),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(Overflow, ARM32OvfRT, ::testing::ValuesIn(kARM32Ovf),
                         rtTCName);
