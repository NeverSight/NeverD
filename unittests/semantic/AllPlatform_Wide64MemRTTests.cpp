//===- AllPlatform_Wide64MemRTTests.cpp - i64 values in memory --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// 64-bit integer values living in *memory arrays*, complementing Wide64Opt
// (register-only kernels) and Wide64CallAbi (call boundaries).  On the 32-bit
// targets (i386 / ARM32) each i64 occupies a register *pair*, so loading,
// storing, comparing and recombining the two halves through memory exercises
// the LowToMed sub-register pair tracking and the partial-write merge over a
// two-word value read back from the stack — a path the in-register kernels do
// not reach.  Kernels use only i64 ops that legalize inline on 32-bit (add /
// sub / and / or / xor / not / neg, *constant* shifts and rotates, signed /
// unsigned compare and select, 32x32->64 widening multiply); they avoid a
// variable i64 shift or i64 divide (the only i64 ops needing __ashldi3 /
// __divdi3, which the bare-metal harness cannot resolve).  Each folds to a
// single integer return, compiled -O2, native vs lifted on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Wide64MemRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Wide64MemRT, Verify) { roundTripX64(GetParam()); }
class X86Wide64MemRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Wide64MemRT, Verify) { roundTripX86(GetParam()); }
class A64Wide64MemRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Wide64MemRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Wide64MemRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Wide64MemRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeW64Mem(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fill an i64 array via LCG, then reduce with an xor/add mix reading both
    // halves of each loaded pair.
    {p+"_sum",
     t+" "+p+"_sum("+t+" a){\n"
     "  unsigned long long v[32]; unsigned long long s=(unsigned long long)a|1ull;\n"
     "  for(int i=0;i<32;i++){ s=s*6364136223846793005ull+1442695040888963407ull; v[i]=s^(s<<13); }\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<32;i++){ acc+=v[i]; acc^=acc>>17; acc+=(v[i]<<7); }\n"
     "  return ("+t+")(unsigned long long)(acc^(acc>>32)); }\n",
     {0x41ULL}, "Wide64Mem", 2},

    // Running signed i64 min/max over a memory array (two-word compare + select
    // with each candidate reloaded from the stack).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  long long v[32]; unsigned long long s=(unsigned long long)a|1ull;\n"
     "  for(int i=0;i<32;i++){ s=s*6364136223846793005ull+1442695040888963407ull; v[i]=(long long)s; }\n"
     "  long long mn=v[0], mx=v[0]; unsigned long long h=0;\n"
     "  for(int i=1;i<32;i++){ if(v[i]<mn)mn=v[i]; if(v[i]>mx)mx=v[i];\n"
     "    h=h*131u+(unsigned long long)mn+(unsigned long long)mx; }\n"
     "  unsigned long long span=(unsigned long long)mx-(unsigned long long)mn;\n"
     "  return ("+t+")(unsigned long long)((h^span)^((h^span)>>32)); }\n",
     {0x53ULL}, "Wide64Mem", 2},

    // In-place i64 array reverse, then a paired fold — pure i64 load/store moves.
    {p+"_rev",
     t+" "+p+"_rev("+t+" a){\n"
     "  unsigned long long v[24]; unsigned long long s=(unsigned long long)a|1ull;\n"
     "  for(int i=0;i<24;i++){ s=s*6364136223846793005ull+1442695040888963407ull; v[i]=s; }\n"
     "  for(int i=0,j=23;i<j;i++,j--){ unsigned long long tmp=v[i]; v[i]=v[j]; v[j]=tmp; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<24;i++){ acc=acc*1099511628211ull ^ v[i]; }\n"
     "  return ("+t+")(unsigned long long)(acc^(acc>>32)); }\n",
     {0x6dULL}, "Wide64Mem", 2},

    // Insertion-sort a small i64 array (signed) then hash it: many i64 memory
    // compares, shifts of elements, and stores.  (Held to 8 elements: at >=16
    // clang -O2 fully unrolls into a stack pointer-array sorting network whose
    // frame-pointer recovery has a separate scale-triggered defect tracked in
    // the Unicorn unsupported-instructions doc — out of scope for this i64-memory probe.)
    {p+"_sort",
     t+" "+p+"_sort("+t+" a){\n"
     "  long long v[8]; unsigned long long s=(unsigned long long)a|1ull;\n"
     "  for(int i=0;i<8;i++){ s=s*6364136223846793005ull+1442695040888963407ull; v[i]=(long long)(s^(s>>23)); }\n"
     "  for(int i=1;i<8;i++){ long long key=v[i]; int j=i-1;\n"
     "    while(j>=0 && v[j]>key){ v[j+1]=v[j]; j--; } v[j+1]=key; }\n"
     "  unsigned long long h=0;\n"
     "  for(int i=0;i<8;i++){ h=h*131u+(unsigned long long)v[i]; }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0xb2ULL}, "Wide64Mem", 2},

    // Two i32 packed into an i64 stored to memory, reloaded as a pair, halves
    // recombined: stresses sub-register reads of a two-word memory value.
    {p+"_pack",
     t+" "+p+"_pack("+t+" a){\n"
     "  unsigned long long m[16]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; unsigned lo=s, hi=s^0x9e3779b9u;\n"
     "    m[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long h=0;\n"
     "  for(int i=0;i<16;i++){ unsigned lo=(unsigned)m[i], hi=(unsigned)(m[i]>>32);\n"
     "    h+=(unsigned long long)lo*hi; h^=m[i]; h=(h<<1)|(h>>63); }\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x18ULL}, "Wide64Mem", 2},

    // i64 memory accumulator RMW: read-modify-write a single 64-bit slot each
    // iteration with constant rotate + 32x32->64 widening multiply.
    {p+"_rmw",
     t+" "+p+"_rmw("+t+" a){\n"
     "  unsigned long long acc[2]={0,~0ull}; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long pr=(unsigned long long)s*(unsigned long long)(s^0x5a5a5a5au);\n"
     "    acc[i&1]+=pr; acc[i&1]^=acc[i&1]>>29; acc[i&1]=(acc[i&1]<<1)|(acc[i&1]>>63); }\n"
     "  unsigned long long h=acc[0]^acc[1];\n"
     "  return ("+t+")(unsigned long long)(h^(h>>32)); }\n",
     {0x2fULL}, "Wide64Mem", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeW64Mem("x64w64m", "long");
static const std::vector<RoundTripTC> kX86 = makeW64Mem("x86w64m", "int");
static const std::vector<RoundTripTC> kA64 = makeW64Mem("a64w64m", "long");
static const std::vector<RoundTripTC> kARM = makeW64Mem("armw64m", "int");

INSTANTIATE_TEST_SUITE_P(Wide64Mem, X64Wide64MemRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Mem, X86Wide64MemRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Mem, A64Wide64MemRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Mem, ARM32Wide64MemRT, ::testing::ValuesIn(kARM), rtTCName);
