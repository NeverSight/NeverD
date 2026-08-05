//===- AArch64_OrderedWidthRTTests.cpp - ordered/unpriv byte/half width --===//
//
// Roundtrip probes for AArch64 ordered (LDAPR/LDLAR/STLLR, FEAT_LRCPC/LOR),
// unscaled (LDAPUR/STLUR, FEAT_LRCPC2) and unprivileged (LDTR/STTR) byte/
// halfword load/store width.
//
// The zero-extending byte/half loads (LDAPRB/H, LDLARB/H, LDAPURB/H, LDTRB/H)
// and the byte/half stores (STLLRB/H, STLUR/B/H, STTRB/H) issued
// `LOAD Dst, {EA}` / `STORE EA, Src` at the full register width (4/8 bytes)
// instead of 1/2 bytes — so byte/half loads read 3/2 extra bytes and byte/half
// stores wrote 3/2 extra bytes.  (The sign-extending siblings LD*SB/SH/SW
// correctly derive the access width; the zero-extending ones were missed —
// the AArch64 analogue of the ARM32 acquire/release width bug.)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64OrderedWidthRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OrderedWidthRT, Verify) { roundTripAArch64(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define A64V84 "OrderedWidth", 1, "-march=armv8.4-a", false, "", UC_CPU_ARM64_MAX

// clang-format off

static const std::vector<RoundTripTC> kA64OrderedWidth = {

  // Zero-extending byte/half loads.
  {"ldaprb",
   "long ldaprb(long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldaprb %w0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A64V84},

  {"ldaprh",
   "long ldaprh(long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldaprh %w0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A64V84},

  {"ldtrb",
   "long ldtrb(long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldtrb %w0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A64V84},

  {"ldtrh",
   "long ldtrh(long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldtrh %w0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A64V84},

  {"ldapurb",
   "long ldapurb(long a){ unsigned char m[4]={0x11,0x22,0x33,0x44}; unsigned r;"
   " __asm__ volatile(\"ldapurb %w0,[%1]\":\"=r\"(r):\"r\"(m):\"memory\"); return r; }\n",
   {0}, A64V84},

  // Byte/half stores (pointer arg into the DATA region -> plain memory, not a
  // stack slot).
  {"sttrb",
   "long sttrb(long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"sttrb %w1,[%0]\"::\"r\"(p),\"r\"((unsigned)a):\"memory\"); return (long)(unsigned)*p; }\n",
   {0xAABBCCDD, 0x500000}, A64V84},

  {"stlurb",
   "long stlurb(long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"stlurb %w1,[%0]\"::\"r\"(p),\"r\"((unsigned)a):\"memory\"); return (long)(unsigned)*p; }\n",
   {0xAABBCCDD, 0x500000}, A64V84},

  {"stlurh",
   "long stlurh(long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"stlurh %w1,[%0]\"::\"r\"(p),\"r\"((unsigned)a):\"memory\"); return (long)(unsigned)*p; }\n",
   {0xAABBCCDD, 0x500000}, A64V84},

  {"stlur",
   "long stlur(long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"stlur %w1,[%0]\"::\"r\"(p),\"r\"((unsigned)a):\"memory\"); return (long)(unsigned)*p; }\n",
   {0xAABBCCDD, 0x500000}, A64V84},

  {"sttrh",
   "long sttrh(long a, unsigned *p){ *p=0x11223344;"
   " __asm__ volatile(\"sttrh %w1,[%0]\"::\"r\"(p),\"r\"((unsigned)a):\"memory\"); return (long)(unsigned)*p; }\n",
   {0xAABBCCDD, 0x500000}, A64V84},

  // Controls: word forms already correct.
  {"ldapr_ctl",
   "long ldapr_ctl(long a){ unsigned m=0xDEADBEEF; unsigned r;"
   " __asm__ volatile(\"ldapr %w0,[%1]\":\"=r\"(r):\"r\"(&m):\"memory\"); return r; }\n",
   {0}, A64V84},

  {"ldtr_ctl",
   "long ldtr_ctl(long a){ unsigned m=0xCAFEF00D; unsigned r;"
   " __asm__ volatile(\"ldtr %w0,[%1]\":\"=r\"(r):\"r\"(&m):\"memory\"); return r; }\n",
   {0}, A64V84},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(OrderedWidth, A64OrderedWidthRT,
                         ::testing::ValuesIn(kA64OrderedWidth), rtTCName);
