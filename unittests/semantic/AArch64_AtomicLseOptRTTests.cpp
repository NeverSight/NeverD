//===- AArch64_AtomicLseOptRTTests.cpp - LSE atomics under the optimizer -===//
//
// Optimizer-driven C11-atomic round-trip probes that force the AArch64 FEAT_LSE
// *single-instruction* lowering by compiling at -O2 with -march=armv8.1-a.  This
// is the realistic compiler output for `__atomic_*` builtins on an LSE target,
// and is distinct from two existing families:
//
//   * AllPlatform_OptStress298/299 ran the same C11 builtins at the *baseline*
//     march, so AArch64 lowered them to ldxr/stxr LL-SC loops (not LSE).
//   * AArch64_Atomic{CAS,SwapAlias,MinMax,StoreOp} exercise individual LSE
//     opcodes via inline asm, with no optimizer-driven old-value dataflow.
//
// Here clang lowers the builtins to ldaddal / ldsetal / ldclral (via mvn) /
// ldeoral / swpal / casal — and, crucially, the sub-word ldaddalb / ldsetalh /
// casalb forms whose returned OLD value is zero-extended and then consumed
// across loop PHIs and branches.  The atomic target is a file-scope global so
// the address escapes and the optimizer cannot demote the atomic to a plain op.
//
// Single-threaded, so every CAS succeeds on the first try and all results are
// deterministic.  The original side runs the real LSE instructions on the MAX
// Unicorn CPU; the lifted side lowers each to plain load/op/store and needs no
// LSE feature.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64AtomicLseOptRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64AtomicLseOptRT, Verify) { roundTripAArch64(GetParam()); }

// Fields after Args: Category, OptLevel, ExtraFlags, NoOpt, Triple, UcCpu
#define A64LSE "AtomicLseOpt", 2, "-march=armv8.1-a", false, "", UC_CPU_ARM64_MAX

// clang-format off
static const std::vector<RoundTripTC> kA64LseOpt = {

  // __atomic_fetch_add (word) -> ldaddal; old folded into a hash accumulator.
  {"lse_add",
   "static unsigned g_at;\n"
   "long lse_add(long a){ unsigned h=(unsigned)a; g_at=h; unsigned acc=0;\n"
   "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned old=__atomic_fetch_add(&g_at,(h>>8)&0xFFu,__ATOMIC_SEQ_CST);\n"
   "    acc=acc*131u+old; }\n"
   "  return (long)(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
   {0x12345}, A64LSE},

  // fetch_or / fetch_and / fetch_xor -> ldsetal / mvn+ldclral / ldeoral.
  {"lse_bits",
   "static unsigned g_at;\n"
   "long lse_bits(long a){ unsigned h=(unsigned)a; g_at=h|1u; unsigned acc=0;\n"
   "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned o1=__atomic_fetch_or(&g_at,(h>>5)&0xFFu,__ATOMIC_SEQ_CST);\n"
   "    unsigned o2=__atomic_fetch_and(&g_at,~((h>>13)&0xFu),__ATOMIC_SEQ_CST);\n"
   "    unsigned o3=__atomic_fetch_xor(&g_at,(h>>17)&0x55u,__ATOMIC_SEQ_CST);\n"
   "    acc=acc*131u+(o1^o2^o3); }\n"
   "  return (long)(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
   {0x23456}, A64LSE},

  // __atomic_exchange_n -> swpal; old value chained into next swapped-in value.
  {"lse_xchg",
   "static unsigned g_at;\n"
   "long lse_xchg(long a){ unsigned h=(unsigned)a; g_at=h; unsigned acc=0;\n"
   "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned old=__atomic_exchange_n(&g_at,h^acc,__ATOMIC_SEQ_CST);\n"
   "    acc=acc*131u+old; }\n"
   "  return (long)(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
   {0x34567}, A64LSE},

  // __atomic_compare_exchange_n running-max -> casal (lock-free, succeeds 1st try).
  {"lse_cas",
   "static unsigned g_at;\n"
   "long lse_cas(long a){ unsigned h=(unsigned)a; g_at=0; unsigned acc=0;\n"
   "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u; unsigned v=(h>>3)&0xFFFFu;\n"
   "    unsigned cur=__atomic_load_n(&g_at,__ATOMIC_RELAXED);\n"
   "    while(v>cur){\n"
   "      if(__atomic_compare_exchange_n(&g_at,&cur,v,0,\n"
   "          __ATOMIC_SEQ_CST,__ATOMIC_RELAXED)) break; }\n"
   "    acc=acc*131u+cur; }\n"
   "  return (long)(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
   {0x45678}, A64LSE},

  // __atomic_fetch_sub -> neg+ldaddal from a saturated start.
  {"lse_sub",
   "static unsigned g_at;\n"
   "long lse_sub(long a){ unsigned h=(unsigned)a; g_at=0xFFFFFFFFu; unsigned acc=0;\n"
   "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned old=__atomic_fetch_sub(&g_at,(h>>9)&0x7Fu,__ATOMIC_SEQ_CST);\n"
   "    acc=acc*131u+(old>>1); }\n"
   "  return (long)(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
   {0x56789}, A64LSE},

  // 8-bit fetch_add -> ldaddalb; returned old BYTE zero-extended and consumed.
  {"lse_addb",
   "static unsigned char g_b;\n"
   "long lse_addb(long a){ unsigned h=(unsigned)a; g_b=(unsigned char)h; unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned char old=__atomic_fetch_add(&g_b,(unsigned char)(h>>11),__ATOMIC_SEQ_CST);\n"
   "    acc=acc*131u+(unsigned)old; }\n"
   "  return (long)(acc + (unsigned)__atomic_load_n(&g_b,__ATOMIC_SEQ_CST)); }\n",
   {0x6789A}, A64LSE},

  // 16-bit fetch_or / fetch_and -> ldsetalh / mvn+ldclralh; old halfword consumed.
  {"lse_orh",
   "static unsigned short g_h;\n"
   "long lse_orh(long a){ unsigned h=(unsigned)a; g_h=(unsigned short)(h|1u); unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned short o1=__atomic_fetch_or(&g_h,(unsigned short)((h>>4)&0xFFu),__ATOMIC_SEQ_CST);\n"
   "    unsigned short o2=__atomic_fetch_and(&g_h,(unsigned short)~((h>>15)&0x7u),__ATOMIC_SEQ_CST);\n"
   "    acc=acc*131u+((unsigned)o1^(unsigned)o2); }\n"
   "  return (long)(acc + (unsigned)__atomic_load_n(&g_h,__ATOMIC_SEQ_CST)); }\n",
   {0x789AB}, A64LSE},

  // 8-bit exchange + compare_exchange -> swpalb / casalb; old byte across CFG.
  {"lse_casb",
   "static unsigned char g_b;\n"
   "long lse_casb(long a){ unsigned h=(unsigned)a; g_b=(unsigned char)h; unsigned acc=0;\n"
   "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned char want=(unsigned char)(h>>20);\n"
   "    unsigned char cur=__atomic_load_n(&g_b,__ATOMIC_RELAXED);\n"
   "    unsigned char nv=(unsigned char)(cur+want+1u);\n"
   "    if(__atomic_compare_exchange_n(&g_b,&cur,nv,0,\n"
   "        __ATOMIC_SEQ_CST,__ATOMIC_RELAXED))\n"
   "      acc=acc*131u+(unsigned)cur;\n"
   "    else acc=acc*131u+(unsigned)(cur^0x5Au);\n"
   "    unsigned char old=__atomic_exchange_n(&g_b,(unsigned char)(acc>>3),__ATOMIC_SEQ_CST);\n"
   "    acc+=(unsigned)old; }\n"
   "  return (long)(acc + (unsigned)__atomic_load_n(&g_b,__ATOMIC_SEQ_CST)); }\n",
   {0x89ABC}, A64LSE},

  // 64-bit fetch_add (x-form ldaddal) + 64-bit fetch_or; full-width old value.
  {"lse_add64",
   "static unsigned long g_q;\n"
   "long lse_add64(long a){ unsigned long h=(unsigned long)a|1; g_q=h; unsigned long acc=0;\n"
   "  for(int i=0;i<64;i++){ h=h*6364136223846793005UL+1442695040888963407UL;\n"
   "    unsigned long o1=__atomic_fetch_add(&g_q,(h>>11)&0xFFFFFFu,__ATOMIC_SEQ_CST);\n"
   "    unsigned long o2=__atomic_fetch_or(&g_q,(h>>33)&0xFFu,__ATOMIC_SEQ_CST);\n"
   "    acc=acc*1099511628211UL+(o1^o2); }\n"
   "  return (long)(acc + __atomic_load_n(&g_q,__ATOMIC_SEQ_CST)); }\n",
   {0x9ABCD}, A64LSE},

  // fetch_add then branch on the old value into distinct sub-word writes.
  {"lse_branch",
   "static unsigned g_at;\n"
   "long lse_branch(long a){ unsigned h=(unsigned)a; g_at=h; unsigned acc=0;\n"
   "  for(int i=0;i<72;i++){ h=h*1103515245u+12345u;\n"
   "    unsigned old=__atomic_fetch_add(&g_at,(h>>7)&0x3Fu,__ATOMIC_SEQ_CST);\n"
   "    unsigned o;\n"
   "    if((old&3u)==0u) o=(unsigned char)(old>>1);\n"
   "    else if((old&3u)==1u) o=(unsigned short)(old*3u);\n"
   "    else o=old^0x9E37u;\n"
   "    acc=acc*131u+o+(unsigned)i; }\n"
   "  return (long)(acc + __atomic_load_n(&g_at,__ATOMIC_SEQ_CST)); }\n",
   {0xABCDE}, A64LSE},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicLseOpt, A64AtomicLseOptRT,
                         ::testing::ValuesIn(kA64LseOpt), rtTCName);
