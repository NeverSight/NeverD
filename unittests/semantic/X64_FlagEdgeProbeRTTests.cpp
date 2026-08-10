//===- X64_FlagEdgeProbeRTTests.cpp - x86 flag edge probes ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The roundtrip harness only compares return values, so flag computations are
// invisible unless folded into the result.  These probes drive flag-producing
// instructions at their edge cases (overflow boundaries, zero source, count =
// last-bit) and fold the relevant flag(s) into the return value via setcc, so
// any divergence in NeverD's flag modelling shows up as a value mismatch.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FlagEdgeRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FlagEdgeRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== IMUL overflow flags (OF=CF set when product is truncated). =====
  {"imul3_ovf",
   "long f(long a){int x=(int)a;unsigned char of;"
   "__asm__ volatile(\"imull $4,%2,%0\\n\\tseto %1\":\"=r\"(x),\"=r\"(of):\"r\"(x):\"cc\");"
   "return ((unsigned long)of<<32)|(unsigned)x;}\n",
   {0x40000000ULL}, "FlagEdge"},

  {"imul3_noovf",
   "long f(long a){int x=(int)a;unsigned char of;"
   "__asm__ volatile(\"imull $4,%2,%0\\n\\tseto %1\":\"=r\"(x),\"=r\"(of):\"r\"(x):\"cc\");"
   "return ((unsigned long)of<<32)|(unsigned)x;}\n",
   {100}, "FlagEdge"},

  {"imul2_ovf",
   "long f(long a){int x=(int)a;unsigned char of;"
   "__asm__ volatile(\"imull %2,%0\\n\\tseto %1\":\"=r\"(x),\"=r\"(of):\"r\"(4):\"cc\");"
   "return ((unsigned long)of<<32)|(unsigned)x;}\n",
   {0x40000000ULL}, "FlagEdge"},

  {"imul64_ovf",
   "long f(long a){long x=a;unsigned char of;"
   "__asm__ volatile(\"imulq %2,%0\\n\\tseto %1\":\"=r\"(x),\"=r\"(of):\"r\"(4L):\"cc\");"
   "return (of?2:0);}\n",
   {0x4000000000000000ULL}, "FlagEdge"},

  // ===== MUL one-operand: CF/OF set when high half is non-zero. =====
  {"mul32_hi_cf",
   "long f(long a){unsigned x=(unsigned)a,hi;unsigned char cf;"
   "__asm__ volatile(\"mull %3\\n\\tsetc %1\":\"=a\"(x),\"=r\"(cf),\"=d\"(hi):\"r\"(2u),\"0\"(x):\"cc\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x80000000ULL}, "FlagEdge"},

  {"mul32_nohi",
   "long f(long a){unsigned x=(unsigned)a,hi;unsigned char cf;"
   "__asm__ volatile(\"mull %3\\n\\tsetc %1\":\"=a\"(x),\"=r\"(cf),\"=d\"(hi):\"r\"(2u),\"0\"(x):\"cc\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x100}, "FlagEdge"},

  // ===== NEG: CF = (operand != 0). =====
  {"neg_cf_nonzero",
   "long f(long a){int x=(int)a;unsigned char cf;"
   "__asm__ volatile(\"negl %0\\n\\tsetc %1\":\"+r\"(x),\"=r\"(cf)::\"cc\");"
   "return ((unsigned long)cf<<32)|(unsigned)x;}\n",
   {5}, "FlagEdge"},

  {"neg_cf_zero",
   "long f(long a){int x=(int)a;unsigned char cf;"
   "__asm__ volatile(\"negl %0\\n\\tsetc %1\":\"+r\"(x),\"=r\"(cf)::\"cc\");"
   "return ((unsigned long)cf<<32)|(unsigned)x;}\n",
   {0}, "FlagEdge"},

  // ===== INC/DEC overflow at signed boundary. =====
  {"inc_of",
   "long f(long a){int x=(int)a;unsigned char of;"
   "__asm__ volatile(\"incl %0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|(unsigned)x;}\n",
   {0x7FFFFFFFULL}, "FlagEdge"},

  {"dec_of",
   "long f(long a){int x=(int)a;unsigned char of;"
   "__asm__ volatile(\"decl %0\\n\\tseto %1\":\"+r\"(x),\"=r\"(of)::\"cc\");"
   "return ((unsigned long)of<<32)|(unsigned)x;}\n",
   {0x80000000ULL}, "FlagEdge"},

  // ===== SHLD/SHRD carry flag = last bit shifted out. =====
  {"shld_cf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char cf;"
   "__asm__ volatile(\"shldl $4,%2,%0\\n\\tsetc %1\":\"+r\"(x),\"=r\"(cf):\"r\"(y):\"cc\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x12345678ULL, 0x9ABCDEF0ULL}, "FlagEdge"},

  {"shrd_cf",
   "long f(long a,long b){unsigned x=(unsigned)a,y=(unsigned)b;unsigned char cf;"
   "__asm__ volatile(\"shrdl $4,%2,%0\\n\\tsetc %1\":\"+r\"(x),\"=r\"(cf):\"r\"(y):\"cc\");"
   "return ((unsigned long)cf<<32)|x;}\n",
   {0x12345678ULL, 0x9ABCDEF0ULL}, "FlagEdge"},

  // ===== BSF/BSR: ZF=1 when source is zero (dest undefined, so return ZF only). =====
  {"bsf_zero_zf",
   "long f(long a){long r=a;unsigned char zf;"
   "__asm__ volatile(\"bsfq %2,%0\\n\\tsetz %1\":\"=r\"(r),\"=r\"(zf):\"r\"(a):\"cc\");"
   "(void)r;return zf;}\n",
   {0}, "FlagEdge"},

  {"bsf_nonzero",
   "long f(long a){long r=0;unsigned char zf;"
   "__asm__ volatile(\"bsfq %2,%0\\n\\tsetz %1\":\"=r\"(r),\"=r\"(zf):\"r\"(a):\"cc\");"
   "return ((unsigned long)zf<<40)|r;}\n",
   {0x100}, "FlagEdge"},

  {"bsr_nonzero",
   "long f(long a){long r=0;unsigned char zf;"
   "__asm__ volatile(\"bsrq %2,%0\\n\\tsetz %1\":\"=r\"(r),\"=r\"(zf):\"r\"(a):\"cc\");"
   "return ((unsigned long)zf<<40)|r;}\n",
   {0x100}, "FlagEdge"},

  // ===== LZCNT/TZCNT: CF=1 when source is zero, ZF=1 when result is zero. =====
  {"lzcnt_zero_cf",
   "long f(long a){unsigned r=0;unsigned char cf;"
   "__asm__ volatile(\"lzcntl %2,%0\\n\\tsetc %1\":\"=r\"(r),\"=r\"(cf):\"r\"((unsigned)a):\"cc\");"
   "return ((unsigned long)cf<<32)|r;}\n",
   {0}, "FlagEdge", 0, "-mlzcnt"},

  {"tzcnt_zero_cf",
   "long f(long a){unsigned r=0;unsigned char cf;"
   "__asm__ volatile(\"tzcntl %2,%0\\n\\tsetc %1\":\"=r\"(r),\"=r\"(cf):\"r\"((unsigned)a):\"cc\");"
   "return ((unsigned long)cf<<32)|r;}\n",
   {0}, "FlagEdge", 0, "-mbmi"},

  {"lzcnt_zf",
   "long f(long a){unsigned r=0;unsigned char zf;"
   "__asm__ volatile(\"lzcntl %2,%0\\n\\tsetz %1\":\"=r\"(r),\"=r\"(zf):\"r\"((unsigned)a):\"cc\");"
   "return ((unsigned long)zf<<32)|r;}\n",
   {0x80000000ULL}, "FlagEdge", 0, "-mlzcnt"},

  // ===== ADCX/ADOX: 128-bit add maintaining two independent carry chains. =====
  // sum = {hi:lo} + {bh:bl}; capture both output limbs.
  {"adcx_carry",
   "long f(long a,long b){unsigned long lo=a,hi=0,bl=b,cf_in;"
   "__asm__ volatile(\"stc\\n\\tadcxq %2,%0\\n\\tsetc %b1\\n\\tmovzbq %b1,%1\""
   ":\"+r\"(lo),\"=r\"(cf_in):\"r\"(bl):\"cc\");"
   "return lo+ (cf_in<<1);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 0}, "FlagEdge", 0, "-madx"},

  {"adox_carry",
   "long f(long a,long b){unsigned long lo=a,bl=b,of_out;"
   "__asm__ volatile(\"pushq %%rax\\n\\tmovl $-1,%%eax\\n\\taddl %%eax,%%eax\\n\\t"
   "adoxq %2,%0\\n\\tseto %b1\\n\\tmovzbq %b1,%1\\n\\tpopq %%rax\""
   ":\"+r\"(lo),\"=r\"(of_out):\"r\"(bl):\"cc\",\"rax\");"
   "return lo+(of_out<<1);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1}, "FlagEdge", 0, "-madx"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(FlagEdge, X64FlagEdgeRT, ::testing::ValuesIn(kX64),
                         rtTCName);
