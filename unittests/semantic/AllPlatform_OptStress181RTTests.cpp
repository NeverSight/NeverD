//===- AllPlatform_OptStress181RTTests.cpp - sieve / digital-root / Collatz =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * sieve     - Sieve of Eratosthenes up to a rodata-seeded bound: cross out
//                 composites by additive striding, then count and sum primes.
//                 Pins a striding mark-and-sweep (distinct from every per-element
//                 scan; the marking index advances by addition, no divide).
//   * digitroot - digital root by repeated base-10 digit-sum until one digit,
//                 counting the reductions.  Pins a nested digit-extraction loop
//                 (distinct from the single-pass Luhn/atoi digit shapes).
//   * collatz   - Collatz trajectory length and peak: halve when even, 3n+1 when
//                 odd, until 1.  Pins a data-driven branch recurrence (distinct
//                 from the Josephus ring #175 and modpow #116).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress181RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress181RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress181RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress181RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress181RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress181RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress181RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress181RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress181TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sieve of Eratosthenes up to a rodata-seeded bound; count + sum primes.
    {p+"_sieve",
     "static const unsigned char "+p+"_sv[8]={37,12,58,4,29,61,7,44};\n"
     +t+" "+p+"_sieve("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned N=40u+(((unsigned)"+p+"_sv[s&7u]^(s&15u))%24u);\n"
     "    unsigned comp[64]; for(unsigned i=0;i<64u;i++) comp[i]=0u;\n"
     "    unsigned primes=0u, psum=0u;\n"
     "    for(unsigned i=2u;i<N;i++){ if(!comp[i]){ primes++; psum+=i; for(unsigned j=i*i;j<N;j+=i) comp[j]=1u; } }\n"
     "    acc=acc*131u+primes*1000u+psum+N; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Bu}, "OptStress181", 2},

    // digital root by repeated base-10 digit-sum, counting reductions.
    {p+"_digitroot",
     "static const unsigned char "+p+"_dr[8]={211,97,143,38,176,52,9,250};\n"
     +t+" "+p+"_digitroot("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=((unsigned)"+p+"_dr[s&7u]^(s&255u))*131u+(s&0xFFFFu); unsigned steps=0u;\n"
     "    while(n>=10u){ unsigned t=0u; while(n>0u){ t+=n%10u; n/=10u; } n=t; steps++; }\n"
     "    acc=acc*131u+n*100u+steps; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Cu}, "OptStress181", 2},

    // Collatz trajectory length and peak.
    {p+"_collatz",
     "static const unsigned char "+p+"_cz[8]={211,97,143,38,176,52,9,250};\n"
     +t+" "+p+"_collatz("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=(((unsigned)"+p+"_cz[s&7u]^(s&255u))%9999u)+1u; unsigned steps=0u, peak=n;\n"
     "    while(n!=1u && steps<1000u){ if(n&1u) n=3u*n+1u; else n>>=1; if(n>peak) peak=n; steps++; }\n"
     "    acc=acc*131u+steps*131u+peak; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Du}, "OptStress181", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress181TC("x64o181", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress181TC("x86o181", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress181TC("a64o181", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress181TC("armo181", "int");

INSTANTIATE_TEST_SUITE_P(OptStress181, X64OptStress181RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress181, X86OptStress181RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress181, A64OptStress181RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress181, ARM32OptStress181RT, ::testing::ValuesIn(kARM), rtTCName);
