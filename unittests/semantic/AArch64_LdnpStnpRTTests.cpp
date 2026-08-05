//===- AArch64_LdnpStnpRTTests.cpp - non-temporal pair load/store --------===//
//
// Roundtrip probes for AArch64 LDNP/STNP (non-temporal pair load/store).
//
// Their handler used `EA = operandRead(operands[2])`, which for a `[Xn, #imm]`
// memory operand DEREFERENCES the pointer (emits a LOAD) and returns the loaded
// value — so the pair access used a *data value* as the address (double
// indirection) and the `#imm` offset was ignored.  (The sibling LDP/STP and
// LDPSW handlers correctly compute base+disp; LDNP/STNP were missed.)
//
// Compilers emit LDNP/STNP for non-temporal/streaming copies at -O2/-O3.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64LdnpStnpRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64LdnpStnpRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64LdnpStnp = {

  // LDNP with offset: r0=buf[2], r1=buf[3].
  {"ldnp_off",
   "long ldnp_off(long a){ long buf[6]; buf[2]=0x100+a; buf[3]=0x200+a;"
   " long r0,r1; __asm__ volatile(\"ldnp %0,%1,[%2,#16]\":\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\");"
   " return r0*7+r1*13; }\n",
   {5}, "LdnpStnp", 1, ""},

  // LDNP no offset (base only).
  {"ldnp_base",
   "long ldnp_base(long a){ long buf[4]; buf[0]=0x300+a; buf[1]=0x400+a;"
   " long r0,r1; __asm__ volatile(\"ldnp %0,%1,[%2]\":\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\");"
   " return r0*7+r1*13; }\n",
   {9}, "LdnpStnp", 1, ""},

  // STNP with offset: store pair, read back.
  {"stnp_off",
   "long stnp_off(long a){ long buf[6]={0,0,0,0,0,0}; long v0=0x100+a, v1=0x200+a;"
   " __asm__ volatile(\"stnp %1,%2,[%0,#16]\"::\"r\"(buf),\"r\"(v0),\"r\"(v1):\"memory\");"
   " return buf[2]*7+buf[3]*13; }\n",
   {5}, "LdnpStnp", 1, ""},

  // STNP 32-bit (w registers) with offset.
  {"stnp_w",
   "long stnp_w(long a){ int buf[6]={0,0,0,0,0,0}; int v0=0x100+(int)a, v1=0x200+(int)a;"
   " __asm__ volatile(\"stnp %w1,%w2,[%0,#8]\"::\"r\"(buf),\"r\"(v0),\"r\"(v1):\"memory\");"
   " return (long)(unsigned)(buf[2]*7+buf[3]*13); }\n",
   {5}, "LdnpStnp", 1, ""},

  // Control: LDP with offset (already correct).
  {"ldp_off_ctl",
   "long ldp_off_ctl(long a){ long buf[6]; buf[2]=0x100+a; buf[3]=0x200+a;"
   " long r0,r1; __asm__ volatile(\"ldp %0,%1,[%2,#16]\":\"=r\"(r0),\"=r\"(r1):\"r\"(buf):\"memory\");"
   " return r0*7+r1*13; }\n",
   {5}, "LdnpStnp", 1, ""},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(LdnpStnp, A64LdnpStnpRT, ::testing::ValuesIn(kA64LdnpStnp),
                         rtTCName);
