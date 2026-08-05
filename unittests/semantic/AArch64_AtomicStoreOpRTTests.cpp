//===- AArch64_AtomicStoreOpRTTests.cpp - LSE atomic store-op roundtrip --===//
//
// Roundtrip probes for the AArch64 FEAT_LSE atomic *store* aliases
// (STADD/STCLR/STSET/STEOR/STSMAX/STSMIN/STUMAX/STUMIN), which write the
// result back to memory and discard the old value (WZR destination).
//
// Capstone decodes them as the matching LD* instruction id but with
// op_count==2 (Src, [Xn]) and no destination register.  The lifter's load-op
// handlers bailed out on `op_count < 3`, so every store-form atomic was a
// silent no-op — memory was never updated.  Each probe returns the resulting
// memory value; a no-op leaves it unchanged (RED), the correct RMW changes it.
//
// Signed vs unsigned is cross-checked with values where the signed answer, the
// unsigned answer and the unchanged value all differ.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicStoreOpRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicStoreOpRT, Verify) { roundTripAArch64(GetParam()); }

// Fields: Category, OptLevel, ExtraFlags, NoOpt, ClangTargetOverride, UcCpuModel
#define A64LSE "AtomicStoreOp", 1, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off

static const std::vector<RoundTripTC> kA64StoreOp = {

  // ===== Bitwise / add (X form) =====
  {"stadd_x",
   "long stadd_x(long a){ long m=10;"
   " __asm__ volatile(\"stadd %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return m; }\n",
   {5}, A64LSE},

  {"stclr_x",
   "long stclr_x(long a){ unsigned long m=0xFF;"
   " __asm__ volatile(\"stclr %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)m; }\n",
   {0x0F}, A64LSE},

  {"stset_x",
   "long stset_x(long a){ unsigned long m=0xF0;"
   " __asm__ volatile(\"stset %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)m; }\n",
   {0x0F}, A64LSE},

  {"steor_x",
   "long steor_x(long a){ unsigned long m=0xFF;"
   " __asm__ volatile(\"steor %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)m; }\n",
   {0x0F}, A64LSE},

  // ===== Signed/unsigned max/min (X form) — distinguishing values =====
  // smax(-3,5)=5 (unsigned max would keep -3).
  {"stsmax_x",
   "long stsmax_x(long a){ long m=-3;"
   " __asm__ volatile(\"stsmax %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return m; }\n",
   {5}, A64LSE},

  // smin(5,-3)=-3 (unsigned min would keep 5).
  {"stsmin_x",
   "long stsmin_x(long a){ long m=5;"
   " __asm__ volatile(\"stsmin %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return m; }\n",
   {(uint64_t)-3}, A64LSE},

  // umax(5,-3)=0xFFFF...FD (signed max would keep 5).
  {"stumax_x",
   "long stumax_x(long a){ unsigned long m=5;"
   " __asm__ volatile(\"stumax %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)m; }\n",
   {(uint64_t)-3}, A64LSE},

  // umin(-3,5)=5 (signed min would keep -3).
  {"stumin_x",
   "long stumin_x(long a){ unsigned long m=0xFFFFFFFFFFFFFFFDUL;"
   " __asm__ volatile(\"stumin %x[s],[%[p]]\"::[s]\"r\"(a),[p]\"r\"(&m):\"memory\");"
   " return (long)m; }\n",
   {5}, A64LSE},

  // ===== W (32-bit) form =====
  {"stadd_w",
   "long stadd_w(long a){ int m=10;"
   " __asm__ volatile(\"stadd %w[s],[%[p]]\"::[s]\"r\"((int)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)m; }\n",
   {5}, A64LSE},

  {"stumax_w",
   "long stumax_w(long a){ unsigned m=5;"
   " __asm__ volatile(\"stumax %w[s],[%[p]]\"::[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)m; }\n",
   {0xFFFFFFFFu}, A64LSE},

  // ===== Byte / halfword forms =====
  {"staddb",
   "long staddb(long a){ unsigned char m=10;"
   " __asm__ volatile(\"staddb %w[s],[%[p]]\"::[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)(unsigned)m; }\n",
   {5}, A64LSE},

  {"stsmaxh",
   "long stsmaxh(long a){ short m=5;"
   " __asm__ volatile(\"stsmaxh %w[s],[%[p]]\"::[s]\"r\"((unsigned)a),[p]\"r\"(&m):\"memory\");"
   " return (long)m; }\n",
   {16}, A64LSE},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicStoreOp, A64AtomicStoreOpRT,
                         ::testing::ValuesIn(kA64StoreOp), rtTCName);
