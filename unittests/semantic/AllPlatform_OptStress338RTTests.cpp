//===- AllPlatform_OptStress338RTTests.cpp - writable ptr × large table -===//
//
// Hardens #536 (writable store/return VALUE symbolization, gated on
// WritableRelocDataAddrs) in forms OptStress75-87 never reached: a struct RETURN
// whose pointer field addresses a writable global on 32-bit register-pair targets
// (i386 EDX:EAX / ARM32 R1:R0), a 2D writable pointer table, and a writable
// pointer returned-then-threaded through an i64 accumulator.
//
//   * _structret: a `noinline` helper returns `struct{int* p; int n;}` where p =
//                 &G[k]; the caller dereferences the returned pointer field.  The
//                 pointer field rides the multi-register struct return (the
//                 field-symbolization path #536 gated), p in the low return
//                 register on every target.
//   * _2dptr    : an array of pointers each addressing a distinct global row;
//                 double runtime index (`row[r][c]`) so both the pointer load and
//                 the element access must resolve to the right global run.
//   * _selret64 : a helper returns one of two global arrays (cond ? A : B), the
//                 caller indexes it and threads a loop-carried u64 accumulator so
//                 the selected base flows through i64-pair liveness on 32-bit.
//   * _ptab512  : a 512-entry writable `int*` table filled with `(i&1)?A:B`,
//                 which clang auto-vectorizes into a SIMD `pshufd` broadcast of
//                 &A/&B packed into i128 stores.  The taken addresses enter a
//                 VECTOR lane, so getVar symbolizes them via the relocation set
//                 (symbolizesWritableRelocPtr) — the lane carries a relocatable
//                 @G that survives relinking instead of the stale original VA.
//
// libcall-free on i386/ARM32: 32-bit mul/add/xor/shift and i64 add/xor/CONSTANT
// shift only (no i64 divide / variable shift / aggregate memcpy).  Deterministic
// LCG seed; folds to one integer return so a dropped/mis-symbolized pointer
// surfaces.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress338RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress338RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress338RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress338RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress338RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress338RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress338RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress338RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress338TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // noinline returns struct{int* p; int n;} with p=&G[k]; caller derefs p.
    {p+"_structret",
     "static int "+p+"_G[32]={0};\n"
     "struct "+p+"_pr { int *p; int n; };\n"
     "static struct "+p+"_pr "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_structret("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<31;i++) "+p+"_G[i]=(int)(i*2654435761u);\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"_pr r="+p+"_mk(s>>5); *r.p+=(int)(s>>11)+r.n;\n"
     "    sum+=*r.p; sum^=sum>>6; }\n"
     "  return ("+t+")(sum+"+p+"_G[0]+"+p+"_G[31]); }\n"
     "static struct "+p+"_pr "+p+"_mk(unsigned k){\n"
     "  struct "+p+"_pr r; r.p=&"+p+"_G[k&31u]; r.n=(int)(k&7u); return r; }\n",
     {0xC2u}, "OptStress338", 2, "-O2"},

    // Array of pointers to distinct global rows; double runtime index.
    {p+"_2dptr",
     "static int "+p+"_r0[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_r1[8]={2,7,1,8,2,8,1,8};\n"
     "static int "+p+"_r2[8]={1,4,1,4,2,1,3,5};\n"
     "static int "+p+"_r3[8]={9,2,6,5,3,5,8,9};\n"
     "static int *"+p+"_rows[4];\n"
     +t+" "+p+"_2dptr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  "+p+"_rows[0]="+p+"_r0; "+p+"_rows[1]="+p+"_r1;\n"
     "  "+p+"_rows[2]="+p+"_r2; "+p+"_rows[3]="+p+"_r3;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned r=(s>>5)&3u, c=(s>>9)&7u; "+p+"_rows[r][c]+=(int)(s>>13)+i;\n"
     "    sum+="+p+"_rows[(s>>17)&3u][(s>>21)&7u]; sum^=sum>>7; }\n"
     "  int acc=sum; for(int q=0;q<4;q++) for(int c=0;c<8;c++) acc=acc*131+"+p+"_rows[q][c];\n"
     "  return ("+t+")acc; }\n",
     {0xC3u}, "OptStress338", 2, "-O2"},

    // noinline returns cond?A:B; caller indexes it threading a u64 accumulator.
    {p+"_selret64",
     "static int "+p+"_P[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_Q[8]={2,7,1,8,2,8,1,8};\n"
     "static int* "+p+"_pq(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_selret64("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=(unsigned)a|1ull;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    int *q="+p+"_pq(s); q[(s>>5)&7u]^=(int)(s>>13)+i;\n"
     "    acc+=(unsigned long long)(unsigned)q[(s>>9)&7u]<<((i&1)?32:0);\n"
     "    acc^=acc>>29; }\n"
     "  return ("+t+")((acc ^ (acc>>32)) + "+p+"_P[0] + "+p+"_Q[7]); }\n"
     "static int* "+p+"_pq(unsigned s){ return (s&1u)?"+p+"_P:"+p+"_Q; }\n",
     {0xC4u}, "OptStress338", 2, "-O2"},

    // 512-entry writable int* table filled with (i&1)?A:B — clang -O2 vectorizes
    // the fill into a pshufd broadcast of &A/&B packed into i128 stores, so the
    // taken addresses enter a SIMD lane.  A dropped/mis-symbolized lane pointer
    // dereferences a stale VA after relinking and surfaces as a value mismatch.
    {p+"_ptab512",
     "static int "+p+"_A[8]={3,1,4,1,5,9,2,6};\n"
     "static int "+p+"_B[8]={2,7,1,8,2,8,1,8};\n"
     "static int *"+p+"_tab[512];\n"
     +t+" "+p+"_ptab512("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int sum=0;\n"
     "  for(int i=0;i<512;i++) "+p+"_tab[i]=(i&1)?"+p+"_A:"+p+"_B;\n"
     "  for(int i=0;i<512;i++){ s=s*1103515245u+12345u;\n"
     "    int *p="+p+"_tab[(s>>7)&511u]; sum+=p[(s>>17)&7u]; sum^=sum>>5; }\n"
     "  return ("+t+")(sum+"+p+"_A[0]+"+p+"_B[7]); }\n",
     {0xC5u}, "OptStress338", 2, "-O2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress338TC("x64o338", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress338TC("x86o338", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress338TC("a64o338", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress338TC("armo338", "int");

INSTANTIATE_TEST_SUITE_P(OptStress338, X64OptStress338RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress338, X86OptStress338RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress338, A64OptStress338RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress338, ARM32OptStress338RT, ::testing::ValuesIn(kARM), rtTCName);
