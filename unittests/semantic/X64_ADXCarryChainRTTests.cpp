//===- X64_ADXCarryChainRTTests.cpp - ADX adcx/adox carry chains -*- C++ -*-===//
//
// x86 ADX (ADCX / ADOX) exist for one reason: multi-precision arithmetic with
// TWO INDEPENDENT carry chains running in parallel.
//
//   ADCX dst, src   dst = dst + src + CF;  CF = carry-out   (OF, SF, ZF, AF,
//                                                            PF all PRESERVED)
//   ADOX dst, src   dst = dst + src + OF;  OF = carry-out   (CF, SF, ZF, AF,
//                                                            PF all PRESERVED)
//
// The defining property is that ADCX touches ONLY CF and ADOX touches ONLY OF,
// so an ADCX chain (carrying through CF) and an ADOX chain (carrying through OF)
// can be interleaved instruction-by-instruction without clobbering each other —
// this is exactly how big-integer multiply (MULX + ADCX/ADOX) accumulates two
// partial-product columns at once.
//
// The lifter models ADCX by writing ONLY reg(CF) and ADOX by writing ONLY
// reg(OF), leaving every other flag untouched.  But the only existing coverage
// (X64_AutoRoundTripTests `adcx`/`adox`) runs a single `clc; adcx` / `clc; adox`
// with operands {10,20} — CF/OF start at 0, never carry out, and nothing checks
// that the other flag survives.  So the entire reason ADX exists (independent,
// interleaved CF and OF carry chains) had ZERO roundtrip coverage — a textbook
// "weak test masking" gap (cf. #346/#345/#326/#288).
//
// These probes drive real carry propagation through multi-limb chains, interleave
// an ADCX (CF) chain with an ADOX (OF) chain to prove they do not interfere, and
// fold a deliberately-set "other" flag (OF for ADCX, CF for ADOX) back into the
// return value to prove ADX preserves it.  All seeds come from the runtime arg so
// clang cannot constant-fold the asm away.  adcx/adox are native on the default
// Unicorn x86-64 CPU (the existing AutoRT probes already execute them), so this is
// a pure lift-coverage round — no lifter/emitter/Unicorn-fork change.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ADXCarryChainRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ADXCarryChainRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // ===== ADCX: 2-limb chain, low limb carries into high (CF chain). =====
  // lo = 0xFFFF... + (s|1) overflows → CF=1 → propagates into hi.
  {"adcx_chain_carry",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long lo=0xFFFFFFFFFFFFFFFFUL, hi=0x100UL;\n"
   "  unsigned long blo=(s|1), bhi=0x3UL;\n"
   "  __asm__ volatile(\"clc\\n\\t\"\n"
   "    \"adcx %[blo], %[lo]\\n\\t\"\n"
   "    \"adcx %[bhi], %[hi]\\n\\t\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi):[blo]\"r\"(blo),[bhi]\"r\"(bhi):\"cc\");\n"
   "  return lo ^ (hi<<1);}\n",
   {0x123456789ABCDEF0ULL}, "ADX"},

  // Control: low limb does NOT carry → high limb gets no +1 from CF.
  {"adcx_chain_nocarry",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long lo=0x1UL, hi=0x100UL;\n"
   "  unsigned long blo=(s&0xFFFF), bhi=0x3UL;\n"
   "  __asm__ volatile(\"clc\\n\\t\"\n"
   "    \"adcx %[blo], %[lo]\\n\\t\"\n"
   "    \"adcx %[bhi], %[hi]\\n\\t\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi):[blo]\"r\"(blo),[bhi]\"r\"(bhi):\"cc\");\n"
   "  return lo ^ (hi<<1);}\n",
   {0x00000000ABCDEF12ULL}, "ADX"},

  // src + CF_in itself overflows (the two-step carry C1||C2 path): with CF=1 and
  // blo=0xFFFF..., blo+CF wraps to 0 carrying out, so lo is unchanged but CF
  // stays 1 and must still propagate into hi.
  {"adcx_double_carry",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long lo=(s|1), hi=0x100UL;\n"
   "  unsigned long blo=0xFFFFFFFFFFFFFFFFUL, bhi=0x7UL;\n"
   "  __asm__ volatile(\"stc\\n\\t\"\n"
   "    \"adcx %[blo], %[lo]\\n\\t\"\n"
   "    \"adcx %[bhi], %[hi]\\n\\t\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi):[blo]\"r\"(blo),[bhi]\"r\"(bhi):\"cc\");\n"
   "  return lo ^ (hi<<1);}\n",
   {0x0F1E2D3C4B5A6978ULL}, "ADX"},

  // ===== ADOX: 2-limb chain, low limb carries into high (OF chain). =====
  // xor clears OF (and CF); adox uses OF as the incoming carry.
  {"adox_chain_carry",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long lo=0xFFFFFFFFFFFFFFFFUL, hi=0x200UL, tmp;\n"
   "  unsigned long blo=(s|1), bhi=0x5UL;\n"
   "  __asm__ volatile(\"xor %[tmp], %[tmp]\\n\\t\"\n"
   "    \"adox %[blo], %[lo]\\n\\t\"\n"
   "    \"adox %[bhi], %[hi]\\n\\t\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi),[tmp]\"=&r\"(tmp)\n"
   "    :[blo]\"r\"(blo),[bhi]\"r\"(bhi):\"cc\");\n"
   "  return lo ^ (hi<<1);}\n",
   {0x123456789ABCDEF0ULL}, "ADX"},

  {"adox_chain_nocarry",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long lo=0x1UL, hi=0x200UL, tmp;\n"
   "  unsigned long blo=(s&0xFFFF), bhi=0x5UL;\n"
   "  __asm__ volatile(\"xor %[tmp], %[tmp]\\n\\t\"\n"
   "    \"adox %[blo], %[lo]\\n\\t\"\n"
   "    \"adox %[bhi], %[hi]\\n\\t\"\n"
   "    :[lo]\"+r\"(lo),[hi]\"+r\"(hi),[tmp]\"=&r\"(tmp)\n"
   "    :[blo]\"r\"(blo),[bhi]\"r\"(bhi):\"cc\");\n"
   "  return lo ^ (hi<<1);}\n",
   {0x00000000ABCDEF12ULL}, "ADX"},

  // ===== Interleaved ADCX(CF) + ADOX(OF): the two chains must stay independent.
  // If the lifter coupled CF/OF (e.g. adcx also wrote OF, or adox clobbered CF),
  // the interleaved high limbs would receive the wrong carry.
  {"adx_interleaved_independent",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long xlo=0xFFFFFFFFFFFFFFFFUL, xhi=0x100UL;\n"
   "  unsigned long ylo=0xFFFFFFFFFFFFFFFFUL, yhi=0x200UL, tmp;\n"
   "  unsigned long axlo=(s|1), axhi=0x3UL, aylo=(s|1), ayhi=0x7UL;\n"
   "  __asm__ volatile(\"xor %[tmp], %[tmp]\\n\\t\"\n"
   "    \"adcx %[axlo], %[xlo]\\n\\t\"\n"
   "    \"adox %[aylo], %[ylo]\\n\\t\"\n"
   "    \"adcx %[axhi], %[xhi]\\n\\t\"\n"
   "    \"adox %[ayhi], %[yhi]\\n\\t\"\n"
   "    :[xlo]\"+r\"(xlo),[xhi]\"+r\"(xhi),[ylo]\"+r\"(ylo),[yhi]\"+r\"(yhi),\n"
   "     [tmp]\"=&r\"(tmp)\n"
   "    :[axlo]\"r\"(axlo),[axhi]\"r\"(axhi),[aylo]\"r\"(aylo),[ayhi]\"r\"(ayhi)\n"
   "    :\"cc\");\n"
   "  return xlo ^ (xhi*3) ^ (ylo*5) ^ (yhi*7);}\n",
   {0x1122334455667788ULL}, "ADX"},

  // Interleaved where only ONE chain carries: proves the carrying chain does not
  // bleed its carry into the other chain's high limb.
  {"adx_interleaved_one_chain",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long xlo=0xFFFFFFFFFFFFFFFFUL, xhi=0x100UL;\n" // CF chain carries
   "  unsigned long ylo=0x1UL, yhi=0x200UL, tmp;\n"           // OF chain no carry
   "  unsigned long axlo=(s|1), axhi=0x3UL, aylo=(s&0xFFFF), ayhi=0x7UL;\n"
   "  __asm__ volatile(\"xor %[tmp], %[tmp]\\n\\t\"\n"
   "    \"adcx %[axlo], %[xlo]\\n\\t\"\n"
   "    \"adox %[aylo], %[ylo]\\n\\t\"\n"
   "    \"adcx %[axhi], %[xhi]\\n\\t\"\n"
   "    \"adox %[ayhi], %[yhi]\\n\\t\"\n"
   "    :[xlo]\"+r\"(xlo),[xhi]\"+r\"(xhi),[ylo]\"+r\"(ylo),[yhi]\"+r\"(yhi),\n"
   "     [tmp]\"=&r\"(tmp)\n"
   "    :[axlo]\"r\"(axlo),[axhi]\"r\"(axhi),[aylo]\"r\"(aylo),[ayhi]\"r\"(ayhi)\n"
   "    :\"cc\");\n"
   "  return xlo ^ (xhi*3) ^ (ylo*5) ^ (yhi*7);}\n",
   {0x00000000DEADBEEFULL}, "ADX"},

  // ===== ADCX preserves OF (set OF=1, run adcx, read OF back via SETO). =====
  // addb 0x7f+1 → AL=0x80, OF=1, CF=0.  A correct adcx leaves OF=1.
  {"adcx_preserve_of_set",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long a=(s|1), b=0x10UL, of;\n"
   "  __asm__ volatile(\"movb $0x7f, %%al\\n\\t addb $1, %%al\\n\\t\"\n"
   "    \"adcx %[b], %[a]\\n\\t\"\n"
   "    \"seto %%al\\n\\t movzbl %%al, %k[of]\\n\\t\"\n"
   "    :[a]\"+r\"(a),[of]\"=r\"(of):[b]\"r\"(b):\"rax\",\"cc\");\n"
   "  return of;}\n",
   {0x123456789ABCDEF0ULL}, "ADX"},

  // ADCX preserves OF=0 (cleared via xor): a correct adcx leaves OF=0.
  {"adcx_preserve_of_clear",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long a=(s|1), b=0xFFFFFFFFFFFFFFFFUL, of, tmp;\n"
   "  __asm__ volatile(\"xor %[tmp], %[tmp]\\n\\t\"\n"   // OF=0, CF=0
   "    \"stc\\n\\t\"                                  \n" // CF=1, OF still 0
   "    \"adcx %[b], %[a]\\n\\t\"\n"                       // carries → CF=1, OF stays 0
   "    \"seto %%al\\n\\t movzbl %%al, %k[of]\\n\\t\"\n"
   "    :[a]\"+r\"(a),[of]\"=r\"(of),[tmp]\"=&r\"(tmp):[b]\"r\"(b):\"rax\",\"cc\");\n"
   "  return of;}\n",
   {0x0F1E2D3C4B5A6978ULL}, "ADX"},

  // ===== ADOX preserves CF (set CF=1, run adox, read CF back via SETC). =====
  // Set OF=1 first (overflow), then stc (CF=1, OF untouched), then adox.
  {"adox_preserve_cf_set",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long a=(s|1), b=0x10UL, cf;\n"
   "  __asm__ volatile(\"movb $0x7f, %%al\\n\\t addb $1, %%al\\n\\t\"\n" // OF=1, CF=0
   "    \"stc\\n\\t\"                                                  \n" // CF=1, OF=1
   "    \"adox %[b], %[a]\\n\\t\"\n"                                       // must keep CF=1
   "    \"setc %%al\\n\\t movzbl %%al, %k[cf]\\n\\t\"\n"
   "    :[a]\"+r\"(a),[cf]\"=r\"(cf):[b]\"r\"(b):\"rax\",\"cc\");\n"
   "  return cf;}\n",
   {0x123456789ABCDEF0ULL}, "ADX"},

  // ADOX preserves CF=0: clear both, run adox that carries out (OF=1), CF stays 0.
  {"adox_preserve_cf_clear",
   "unsigned long f(unsigned long s){\n"
   "  unsigned long a=0xFFFFFFFFFFFFFFFFUL, b=(s|1), cf, tmp;\n"
   "  __asm__ volatile(\"xor %[tmp], %[tmp]\\n\\t\"\n"  // CF=0, OF=0
   "    \"adox %[b], %[a]\\n\\t\"\n"                      // a overflows → OF=1, CF stays 0
   "    \"setc %%al\\n\\t movzbl %%al, %k[cf]\\n\\t\"\n"
   "    :[a]\"+r\"(a),[cf]\"=r\"(cf),[tmp]\"=&r\"(tmp):[b]\"r\"(b):\"rax\",\"cc\");\n"
   "  return cf;}\n",
   {0x0F1E2D3C4B5A6978ULL}, "ADX"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ADX, X64ADXCarryChainRT,
                         ::testing::ValuesIn(kX64), rtTCName);
