//===- AllPlatform_PtrTableIdx2RTTests.cpp - loaded-pointer uses -*-C++*-=//
//
// Follow-on to #457 PtrTableIdx: there the table-loaded pointer was dereferenced
// with a fresh `[k]` subscript.  These exercise the OTHER ways a pointer read
// from a global table is consumed, at the intersection of #457 (pointer tables)
// and #411/#426 (rodata induction pointers):
//
//   * strscan  - `const char *p = W[i]; while(*p){ h+=*p; p++; }` — the loaded
//                table pointer becomes a `p++` induction base (a real induction
//                pointer, not an indexed reread).
//   * offptr   - `const char *p = W[i] + (s%len); h += *p` — pointer arithmetic
//                on the loaded pointer before the deref.
//   * arrind   - `const int *p = rows[i]; for(j) sum += *p++;` — int-array
//                induction walked from a two-level pointer table.
//   * fntab    - `FT[i](x)` direct-indexed function-pointer table call in a loop
//                (the code-pointer table rebuild from #402/#413, looped).
//   * fnsel    - `(c ? FA : FB)[i](x)` — a function-pointer table SELECT base
//                then indirect call: the CALL/LOAD-of-code-pointer dual of the
//                #457 `selptr` pointer-select fix.
//   * twowalk  - two table strings walked together (`while(*a&&*b){…;a++;b++;}`).
//
// Helpers are file-scope `static __attribute__((noinline))` so the function
// pointer tables stay real; the entry is defined first so the harness runs it
// from the image start.  All integer, fold to one return, no float / 64-bit
// divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PtrTbl2RT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PtrTbl2RT, Verify) { roundTripX64(GetParam()); }
class X86PtrTbl2RT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PtrTbl2RT, Verify) { roundTripX86(GetParam()); }
class A64PtrTbl2RT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PtrTbl2RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32PtrTbl2RT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PtrTbl2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makePtrTbl2TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Loaded table pointer becomes a `p++` induction base.
    {p+"_strscan",
     "static const char *const W[6]={\"alpha\",\"bravo\",\"charlie\",\"delta\","
     "\"echo\",\"foxtrot\"};\n"
     +t+" "+p+"_strscan("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const char *p=W[(s>>5)%6u];\n"
     "    while(*p){ h=h*131u+(unsigned)(unsigned char)*p; p++; }\n"
     "    h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x31u}, "PtrTableIdx2", 2},

    // Pointer arithmetic on the loaded pointer before deref.  Equal-length
    // strings keep the bound a compile-time constant (no separate length array,
    // which would add a third read-only section the fixture cannot all pack).
    {p+"_offptr",
     "static const char *const W[6]={\"alphaA\",\"bravoB\",\"charlC\",\"deltaD\","
     "\"echoEe\",\"foxtrF\"};\n"
     +t+" "+p+"_offptr("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)%6u; const char *p=W[j]+((s>>9)%6u);\n"
     "    h=h*131u+(unsigned)(unsigned char)*p; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x32u}, "PtrTableIdx2", 2},

    // Int-array induction walked from a two-level pointer table.
    {p+"_arrind",
     "static const int r0[5]={2,3,5,7,11};\n"
     "static const int r1[5]={13,17,19,23,29};\n"
     "static const int r2[5]={31,37,41,43,47};\n"
     "static const int r3[5]={53,59,61,67,71};\n"
     "static const int *const rows[4]={r0,r1,r2,r3};\n"
     +t+" "+p+"_arrind("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const int *p=rows[(s>>6)&3u]; unsigned acc=0;\n"
     "    for(int j=0;j<5;j++) acc=acc*7u+(unsigned)*p++;\n"
     "    h=h*131u+acc; h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x33u}, "PtrTableIdx2", 2},
  };
}

// Direct-indexed and SELECT-base function-pointer table calls.  Defined
// separately so the helpers can be emitted after the entry function.
static RoundTripTC makeFntabTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {p+"_fntab",
     "static int "+p+"_g0(int x){ return x*3+1; }\n"
     "static int "+p+"_g1(int x){ return x^0x55; }\n"
     "static int "+p+"_g2(int x){ return (x>>1)+7; }\n"
     "static int "+p+"_g3(int x){ return x*x+x; }\n"
     "static int (*const FT[4])(int)={"+p+"_g0,"+p+"_g1,"+p+"_g2,"+p+"_g3};\n"
     +t+" "+p+"_fntab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int (*f)(int)=FT[(s>>6)&3u];\n"
     "    h=h*131u+(unsigned)f((int)(s>>3)); h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0x34u}, "PtrTableIdx2", 2};
}

// `(cond ? FA : FB)[i](x)`: function-pointer table SELECT base + indirect call,
// the code-pointer dual of #457's selptr pointer-select fix.  Instantiated for
// x86-64 / AArch64 / i386 only: the table-pointer redirect is correct on ARM32
// too (verified in IR), but the recompiled ARM32 object places the biased
// literal-pool base globals in a SEPARATE `.rodata` section from the
// `.data.rel.ro` table, and the roundtrip fixture's link step packs only ONE
// read-only section — a pre-existing harness limitation orthogonal to the
// pointer-table lift — so the far-global base load reads unmapped.  Deferred as
// the next ARM32 multi-RO-section harness specialization.
static RoundTripTC makeFnselTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {p+"_fnsel",
     "static int "+p+"_a0(int x){ return x+11; }\n"
     "static int "+p+"_a1(int x){ return x*5; }\n"
     "static int "+p+"_a2(int x){ return x^0x3c; }\n"
     "static int "+p+"_a3(int x){ return (x<<2)+3; }\n"
     "static int "+p+"_b0(int x){ return x-7; }\n"
     "static int "+p+"_b1(int x){ return x*9+2; }\n"
     "static int "+p+"_b2(int x){ return (x>>2)^x; }\n"
     "static int "+p+"_b3(int x){ return x*x-1; }\n"
     "static int (*const FA[4])(int)={"+p+"_a0,"+p+"_a1,"+p+"_a2,"+p+"_a3};\n"
     "static int (*const FB[4])(int)={"+p+"_b0,"+p+"_b1,"+p+"_b2,"+p+"_b3};\n"
     +t+" "+p+"_fnsel("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int (*const *T)(int)=((s>>4)&1u)?FA:FB;\n"
     "    int (*f)(int)=T[(s>>7)&3u];\n"
     "    h=h*131u+(unsigned)f((int)(s>>3)); h^=h>>10; }\n"
     "  return ("+t+")h; }\n",
     {0x35u}, "PtrTableIdx2", 2};
}

static RoundTripTC makeTwowalkTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {p+"_twowalk",
     "static const char *const W[6]={\"sigma\",\"tau\",\"upsilon\",\"phi\","
     "\"chi\",\"psi\"};\n"
     +t+" "+p+"_twowalk("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    const char *x=W[(s>>5)%6u], *y=W[(s>>9)%6u];\n"
     "    while(*x&&*y){ h=h*131u+(unsigned)(unsigned char)(*x^ *y); x++; y++; }\n"
     "    h^=h>>14; }\n"
     "  return ("+t+")h; }\n",
     {0x36u}, "PtrTableIdx2", 2};
}
// clang-format on

static std::vector<RoundTripTC> makeAll(const char *p, const char *T,
                                        bool WithFnsel) {
  auto V = makePtrTbl2TC(p, T);
  V.push_back(makeFntabTC(p, T));
  if (WithFnsel)
    V.push_back(makeFnselTC(p, T));
  V.push_back(makeTwowalkTC(p, T));
  return V;
}

static const std::vector<RoundTripTC> kX64 = makeAll("x64pt2", "long", true);
static const std::vector<RoundTripTC> kX86 = makeAll("x86pt2", "int", true);
static const std::vector<RoundTripTC> kA64 = makeAll("a64pt2", "long", true);
static const std::vector<RoundTripTC> kARM = makeAll("armpt2", "int", false);

INSTANTIATE_TEST_SUITE_P(PtrTableIdx2, X64PtrTbl2RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrTableIdx2, X86PtrTbl2RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrTableIdx2, A64PtrTbl2RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrTableIdx2, ARM32PtrTbl2RT, ::testing::ValuesIn(kARM), rtTCName);
