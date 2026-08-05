//===- X64_BMI2DivEdgeRTTests.cpp - BMI2 + sign-extend + div edges -*-C++-*-==//
//
// Roundtrip probes for the less-exercised x86 bit-manipulation (BMI2) family
// (PDEP/PEXT/BZHI/BEXTR/MULX), the sign-extend-into-DX idioms (CDQ/CQO/CWD)
// that feed signed division, and signed/unsigned division edge cases.  These
// have complex multi-register / parallel-bit semantics that simple ALU probes
// miss.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BMI2DivRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BMI2DivRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== PDEP / PEXT (parallel bit deposit / extract). =====
  {"pdep64",
   "long f(long a,long b){return (long)__builtin_ia32_pdep_di(a,b);}\n",
   {0x00000000000000FFULL, 0x0F0F0F0F0F0F0F0FULL}, "BMI2", 1, "-mbmi2"},
  {"pext64",
   "long f(long a,long b){return (long)__builtin_ia32_pext_di(a,b);}\n",
   {0x123456789ABCDEF0ULL, 0x0F0F0F0F0F0F0F0FULL}, "BMI2", 1, "-mbmi2"},
  {"pdep32",
   "long f(long a,long b){return __builtin_ia32_pdep_si((unsigned)a,(unsigned)b);}\n",
   {0x000000FFULL, 0xAAAAAAAAULL}, "BMI2", 1, "-mbmi2"},
  {"pext32",
   "long f(long a,long b){return __builtin_ia32_pext_si((unsigned)a,(unsigned)b);}\n",
   {0xDEADBEEFULL, 0xF0F0F0F0ULL}, "BMI2", 1, "-mbmi2"},
  {"pdep_all_ones",
   "long f(long a,long b){return (long)__builtin_ia32_pdep_di(a,b);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 0xCCCCCCCCCCCCCCCCULL}, "BMI2", 1, "-mbmi2"},

  // ===== BZHI (zero high bits starting at index). =====
  {"bzhi64",
   "long f(long a,long b){return (long)__builtin_ia32_bzhi_di(a,b);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 20}, "BMI2", 1, "-mbmi2"},
  // index >= operand size: BZHI clears nothing (result = source).  Exposed the
  // Unicorn fork bug where N was clamped to opsize-1 and the top bit cleared.
  {"bzhi64_ge64",
   "long f(long a,long b){return (long)__builtin_ia32_bzhi_di(a,b);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 70}, "BMI2", 1, "-mbmi2"},
  {"bzhi64_eq64",
   "long f(long a,long b){return (long)__builtin_ia32_bzhi_di(a,b);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 64}, "BMI2", 1, "-mbmi2"},
  {"bzhi32_ge32",
   "long f(long a,long b){return __builtin_ia32_bzhi_si((unsigned)a,(unsigned)b);}\n",
   {0xFFFFFFFFULL, 40}, "BMI2", 1, "-mbmi2"},
  // BZHI carry flag: CF = (N >= operand size).  Fold CF into bit 40.  N=63 is
  // the boundary (CF must be 0); the Unicorn fork mis-set CF for N==opsize-1.
  {"bzhi_cf_n63",
   "long f(long a){unsigned long r;unsigned char cf;"
   "__asm__ volatile(\"bzhi %[i],%[s],%[d]\\n\\tsetc %[c]\""
   ":[d]\"=r\"(r),[c]\"=q\"(cf):[s]\"r\"(a),[i]\"r\"(63ULL):\"cc\");"
   "return ((unsigned long)cf<<40)|(r&0xFFFFFFFFFFULL);}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi2"},
  {"bzhi_cf_n70",
   "long f(long a){unsigned long r;unsigned char cf;"
   "__asm__ volatile(\"bzhi %[i],%[s],%[d]\\n\\tsetc %[c]\""
   ":[d]\"=r\"(r),[c]\"=q\"(cf):[s]\"r\"(a),[i]\"r\"(70ULL):\"cc\");"
   "return ((unsigned long)cf<<40)|(r&0xFFFFFFFFFFULL);}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi2"},
  {"bzhi_cf_n20",
   "long f(long a){unsigned long r;unsigned char cf;"
   "__asm__ volatile(\"bzhi %[i],%[s],%[d]\\n\\tsetc %[c]\""
   ":[d]\"=r\"(r),[c]\"=q\"(cf):[s]\"r\"(a),[i]\"r\"(20ULL):\"cc\");"
   "return ((unsigned long)cf<<40)|r;}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi2"},

  // ===== BEXTR (bitfield extract: start in bits 0-7, len in bits 8-15). =====
  {"bextr64",
   "long f(long a){return (long)__builtin_ia32_bextr_u64(a,(8ULL<<8)|4);}\n",
   {0x0123456789ABCDEFULL}, "BMI2", 1, "-mbmi -mbmi2"},
  {"bextr32",
   "long f(long a){return __builtin_ia32_bextr_u32((unsigned)a,(12U<<8)|8);}\n",
   {0xDEADBEEFULL}, "BMI2", 1, "-mbmi -mbmi2"},
  // LEN >= operand size: mask is all-ones (extract from START to top).  Exposed
  // the Unicorn fork bug where LEN was clamped to opsize-1 (top bit dropped).
  {"bextr_len_ge64",
   "long f(long a){return (long)__builtin_ia32_bextr_u64(a,(64ULL<<8)|0);}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi -mbmi2"},
  {"bextr_len70_start8",
   "long f(long a){return (long)__builtin_ia32_bextr_u64(a,(70ULL<<8)|8);}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi -mbmi2"},
  {"bextr_len32_full",
   "long f(long a){return __builtin_ia32_bextr_u32((unsigned)a,(40U<<8)|0);}\n",
   {0xFFFFFFFFULL}, "BMI2", 1, "-mbmi -mbmi2"},
  // START >= operand size: result 0 (all bits shifted out).
  {"bextr_start_ge64",
   "long f(long a){return (long)__builtin_ia32_bextr_u64(a,(8ULL<<8)|70);}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi -mbmi2"},
  // START+LEN past the top: only available high bits extracted.
  {"bextr_start60_len10",
   "long f(long a){return (long)__builtin_ia32_bextr_u64(a,(10ULL<<8)|60);}\n",
   {0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi -mbmi2"},

  // ===== MULX-style 64x64→high (unsigned multiply high half). =====
  {"mulx_hi",
   "long f(long a,long b){__uint128_t p=(__uint128_t)(unsigned long)a*"
   "(unsigned long)b;return (long)(unsigned long)(p>>64);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL}, "BMI2", 1, "-mbmi2"},

  // ===== Sign-extend into DX feeding signed division (cdq/cqo). =====
  {"idiv32_neg",
   "long f(long a,long b){int x=(int)a,y=(int)b;return x/y;}\n",
   {(uint64_t)(int64_t)-1000000, (uint64_t)(int64_t)7}, "BMI2"},
  {"imod32_neg",
   "long f(long a,long b){int x=(int)a,y=(int)b;return x%y;}\n",
   {(uint64_t)(int64_t)-1000000, (uint64_t)(int64_t)7}, "BMI2"},
  {"idiv64_neg",
   "long f(long a,long b){return a/b;}\n",
   {(uint64_t)(int64_t)-1000000000000LL, (uint64_t)(int64_t)-7}, "BMI2"},
  {"imod64_neg",
   "long f(long a,long b){return a%b;}\n",
   {(uint64_t)(int64_t)1000000000000LL, (uint64_t)(int64_t)-7}, "BMI2"},
  {"udiv64",
   "long f(long a,long b){return (long)((unsigned long)a/(unsigned long)b);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 3}, "BMI2"},
  // INT_MIN / -1 traps on hardware; use INT_MIN / large to avoid trap but keep
  // the high-bit dividend.
  {"idiv32_intmin",
   "long f(long a,long b){int x=(int)a,y=(int)b;return x/y;}\n",
   {0x80000000ULL, 3}, "BMI2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(BMI2Div, X64BMI2DivRT, ::testing::ValuesIn(kX64),
                         rtTCName);
