//===- AArch64_CondCompareRTTests.cpp - CCMP / CCMN flag setting -*- C++ -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 conditional compare:
//
//   CCMP <Xn>, <Xm|#imm>, #<nzcv>, <cond>
//   CCMN <Xn>, <Xm|#imm>, #<nzcv>, <cond>
//
//     if <cond> holds : NZCV = flags(Xn - Xm)   (CCMP, a subtract)
//                       NZCV = flags(Xn + Xm)   (CCMN, an add)
//     else            : NZCV = the 4-bit #<nzcv> immediate  (verbatim)
//
// This is the building block of the AArch64 branchless compound-condition idiom
// (`cmp; ccmp; cset` for `a && b` / `a || b`).  The ONLY existing roundtrip
// coverage drives CCMP through *C-level* compound conditions (AArch64_FlagChain
// `a>0 && b>0`, CondLoop, FlagFoldEdge2) — i.e. whatever idiom the compiler
// happens to emit.  None of that exercises, head-on:
//
//   * the FALSE arm where NZCV is loaded from the #nzcv immediate — and whether
//     each of the four immediate bits lands on the RIGHT flag.  The lifter
//     decodes N=(imm>>3), Z=(imm>>2), C=(imm>>1), V=(imm&0) and emits four
//     independent SELECTs into reg(N/Z/C/V); a single transposed bit or a
//     mis-ordered SELECT would silently corrupt one flag — invisible to a test
//     that only reads the *combined* boolean result of a compound condition.
//   * the immediate second-operand form `ccmp Xn,#imm5,...` vs the register form.
//   * CCMN (the add/INT_CARRY/INT_SOVF path) at all — the C idiom almost never
//     emits ccmn, so its carry/overflow flag computation was never roundtripped.
//   * a spread of <cond> codes evaluated *inside* ccmp's own true/false select.
//
// Each probe folds the post-`ccmp`/`ccmn` N,Z,C,V (read back individually with
// `cset mi/eq/hs/vs`, which do not disturb the flags) into the return value, so
// the harness — which only compares X0 — actually observes every flag bit.  All
// operands come from runtime args so clang cannot constant-fold the asm.  ccmp/
// ccmn are base ARMv8-A and native on the default Unicorn arm64 CPU (the
// FlagChain C-idiom probes already execute them), so this is a pure lift-coverage
// round — no lifter/emitter/Unicorn change.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CondCompareRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CondCompareRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {

  // ===== FALSE arm: NZCV := #nzcv immediate, bit-by-bit mapping. =====
  // `cmp a,a` forces Z=1, so `ne` is false and the #nzcv immediate is taken.
  // The returned nibble is (N<<3)|(Z<<2)|(C<<1)|V, which must equal #nzcv.
  // A swapped immediate bit or mis-targeted flag SELECT shows up as a mismatch.
  {"ccmp_false_imm_n",     // 0b1000 -> N only
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x8,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x123456789ABCDEF0ULL}, "CondCompare"},

  {"ccmp_false_imm_z",     // 0b0100 -> Z only
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x4,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x0F1E2D3C4B5A6978ULL}, "CondCompare"},

  {"ccmp_false_imm_c",     // 0b0010 -> C only
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x2,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x1122334455667788ULL}, "CondCompare"},

  {"ccmp_false_imm_v",     // 0b0001 -> V only
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x1,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0xDEADBEEFCAFEF00DULL}, "CondCompare"},

  {"ccmp_false_imm_all",   // 0b1111 -> all set
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0xf,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x8000000000000001ULL}, "CondCompare"},

  {"ccmp_false_imm_none",  // 0b0000 -> all clear
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x0,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x00000000FFFFFFFFULL}, "CondCompare"},

  {"ccmp_false_imm_nc",    // 0b1010 -> N+C
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0xa,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x5555AAAA5555AAAAULL}, "CondCompare"},

  {"ccmp_false_imm_zv",    // 0b0101 -> Z+V
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x5,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0xAAAA5555AAAA5555ULL}, "CondCompare"},

  // ===== TRUE arm: NZCV := flags(Xn - Xm).  cmp a,a -> eq true. =====
  // b - c borrows (0 - 1): N=1, Z=0, C=0 (borrow), V=0.
  {"ccmp_true_borrow",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[b],%[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0, 1}, "CondCompare"},

  // INT_MIN - 1: signed overflow -> N=0, Z=0, C=1, V=1.
  {"ccmp_true_overflow",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[b],%[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0x8000000000000000ULL, 1}, "CondCompare"},

  // Equal operands: Z=1, C=1, N=0, V=0.
  {"ccmp_true_equal",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[b],%[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0x1234, 0x1234}, "CondCompare"},

  // ===== Immediate second-operand form: ccmp Xn,#imm5,#nzcv,cond. =====
  // TRUE arm: b - 5 with b=5 -> Z=1,C=1.
  {"ccmp_immop_true",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[b],#5,#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x99, 5}, "CondCompare"},

  // FALSE arm with immediate operand: ne false -> #0x6 = 0b0110 (Z+C).
  {"ccmp_immop_false",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmp %[b],#5,#0x6,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x99, 5}, "CondCompare"},

  // ===== CCMN (add): NZCV := flags(Xn + Xm). =====
  // -1 + 1 = 0 with carry out: C=1, Z=1, N=0, V=0.
  {"ccmn_true_carry",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmn %[b],%[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0xFFFFFFFFFFFFFFFFULL, 1}, "CondCompare"},

  // INT_MAX + 1: signed overflow -> N=1, Z=0, C=0, V=1.
  {"ccmn_true_overflow",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmn %[b],%[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0x7FFFFFFFFFFFFFFFULL, 1}, "CondCompare"},

  // CCMN immediate operand: -1 + 1 = 0 -> C=1, Z=1.
  {"ccmn_immop_true",
   "unsigned long f(unsigned long a,unsigned long b){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmn %[b],#1,#0x0,eq\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x99, 0xFFFFFFFFFFFFFFFFULL}, "CondCompare"},

  // CCMN FALSE arm: ne false -> #0x3 = 0b0011 (C+V).
  {"ccmn_false_imm",
   "unsigned long f(unsigned long a){unsigned long n,z,c,v;"
   "__asm__ volatile(\"cmp %[a],%[a]\\n\\t\"\n"
   "  \"ccmn %[a],%[a],#0x3,ne\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[c],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[c]\"=r\"(c),[v]\"=r\"(v):[a]\"r\"(a):\"cc\");"
   "return (n<<3)|(z<<2)|(c<<1)|v;}\n",
   {0x0123456789ABCDEFULL}, "CondCompare"},

  // ===== 32-bit (Wn) forms exercise the 4-byte compare width. =====
  // 32-bit borrow: 0 - 1 -> N=1 (bit31), C=0.
  {"ccmp_w_true_borrow",
   "unsigned f(unsigned a,unsigned b,unsigned c){unsigned n,z,cf,v;"
   "__asm__ volatile(\"cmp %w[a],%w[a]\\n\\t\"\n"
   "  \"ccmp %w[b],%w[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %w[n],mi\\n\\tcset %w[z],eq\\n\\tcset %w[cf],hs\\n\\tcset %w[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0, 1}, "CondCompare"},

  // 32-bit add carry: 0xFFFFFFFF + 1 wraps to 0 -> C=1, Z=1.
  {"ccmn_w_true_carry",
   "unsigned f(unsigned a,unsigned b,unsigned c){unsigned n,z,cf,v;"
   "__asm__ volatile(\"cmp %w[a],%w[a]\\n\\t\"\n"
   "  \"ccmn %w[b],%w[c],#0x0,eq\\n\\t\"\n"
   "  \"cset %w[n],mi\\n\\tcset %w[z],eq\\n\\tcset %w[cf],hs\\n\\tcset %w[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 0xFFFFFFFFu, 1}, "CondCompare"},

  // ===== <cond> evaluation inside ccmp's own true/false select. =====
  // GE true (cmp 9,3 -> N==V): ccmp computes a-a -> Z=1,C=1 -> 0b0100.
  {"ccmp_cond_ge_true",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[b],%[c]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0xf,ge\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 9, 3}, "CondCompare"},

  // HI false (cmp 3,9 -> C=0): immediate #0x5 taken -> 0b0101.
  {"ccmp_cond_hi_false",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[b],%[c]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0x5,hi\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 3, 9}, "CondCompare"},

  // LE true (cmp 3,9 -> N!=V): ccmp computes a-a -> Z=1,C=1 -> 0b0100.
  {"ccmp_cond_le_true",
   "unsigned long f(unsigned long a,unsigned long b,unsigned long c){unsigned long n,z,cf,v;"
   "__asm__ volatile(\"cmp %[b],%[c]\\n\\t\"\n"
   "  \"ccmp %[a],%[a],#0xf,le\\n\\t\"\n"
   "  \"cset %[n],mi\\n\\tcset %[z],eq\\n\\tcset %[cf],hs\\n\\tcset %[v],vs\\n\\t\"\n"
   "  :[n]\"=r\"(n),[z]\"=r\"(z),[cf]\"=r\"(cf),[v]\"=r\"(v)"
   "  :[a]\"r\"(a),[b]\"r\"(b),[c]\"r\"(c):\"cc\");"
   "return (n<<3)|(z<<2)|(cf<<1)|v;}\n",
   {0x99, 3, 9}, "CondCompare"},

  // ===== The real branchless compound-condition idiom: cmp; ccmp; cset. =====
  // (a >= lo) && (a <= hi) range check, lifted as cmp/ccmp/cset.  Inside.
  {"ccmp_range_inside",
   "unsigned long f(unsigned long a,unsigned long lo,unsigned long hi){unsigned long r;"
   "__asm__ volatile(\"cmp %[a],%[lo]\\n\\t\"\n"
   "  \"ccmp %[a],%[hi],#0x0,hs\\n\\t\"\n"   // if a>=lo then compare a,hi else NZCV=0
   "  \"cset %[r],ls\\n\\t\"\n"               // ls (a<=hi) true only when both hold
   "  :[r]\"=r\"(r):[a]\"r\"(a),[lo]\"r\"(lo),[hi]\"r\"(hi):\"cc\");"
   "return r;}\n",
   {50, 10, 100}, "CondCompare"},

  // Below the low bound: first cmp fails hs -> ccmp loads #0 -> ls false.
  {"ccmp_range_below",
   "unsigned long f(unsigned long a,unsigned long lo,unsigned long hi){unsigned long r;"
   "__asm__ volatile(\"cmp %[a],%[lo]\\n\\t\"\n"
   "  \"ccmp %[a],%[hi],#0x0,hs\\n\\t\"\n"
   "  \"cset %[r],ls\\n\\t\"\n"
   "  :[r]\"=r\"(r):[a]\"r\"(a),[lo]\"r\"(lo),[hi]\"r\"(hi):\"cc\");"
   "return r;}\n",
   {5, 10, 100}, "CondCompare"},

  // Above the high bound: hs holds, real compare a,hi -> ls false.
  {"ccmp_range_above",
   "unsigned long f(unsigned long a,unsigned long lo,unsigned long hi){unsigned long r;"
   "__asm__ volatile(\"cmp %[a],%[lo]\\n\\t\"\n"
   "  \"ccmp %[a],%[hi],#0x0,hs\\n\\t\"\n"
   "  \"cset %[r],ls\\n\\t\"\n"
   "  :[r]\"=r\"(r):[a]\"r\"(a),[lo]\"r\"(lo),[hi]\"r\"(hi):\"cc\");"
   "return r;}\n",
   {200, 10, 100}, "CondCompare"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(CondCmp, A64CondCompareRT,
                         ::testing::ValuesIn(kA64), rtTCName);
