//===- AllPlatform_OptStress96RTTests.cpp - vtable / 2D / ptr-chain -*-C++-==//
//
// #473-#485 symbolized global addresses flowing through calls, returns, struct
// fields, arrays and PHI merges.  These push the "constant pool mapping" DNA into
// three shapes those probes never hit:
//
//   * vtab  - the C vtable idiom: a const global table of FUNCTION pointers, an
//             object that stores a pointer to that table, and a DOUBLE-indirect
//             dispatch `o->vt->m(o,..)`.  The const vtable's function-pointer
//             fields are relocations (code-pointer segment), the per-object vt
//             field is a writable-stored global address, and the call target is
//             reached through two loads — a compound of #475 fptab + object vt.
//   * mat2d - a 2D global array `M[i][j]` with BOTH indices runtime: the base
//             must symbolize once and the nested `i*C + j` scaling must stay
//             base-relative (a transpose-style `M[i][j] += M[j][i]` makes a wrong
//             base/stride diverge immediately).
//   * gchain- a STATICALLY-INITIALIZED const pointer chain (`next` fields hold
//             `&Nk` set by the linker, .rodata relocations) walked at runtime via
//             `p = p->next`.  The loaded pointer is a global address that must be
//             re-symbolized at the dereference (the static-init dual of #476's
//             run-time-stored global pointer array).
//
// All integer, file-scope globals, LCG-seeded, fold to one integer return; no
// float / 64-bit divide / libcall.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress96RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress96RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress96RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress96RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress96RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress96RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress96RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress96RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress96TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // C vtable idiom: const fptr table + object vt pointer + double-indirect call.
    {p+"_vtab",
     "struct "+p+"_O; typedef int (*"+p+"_M)(struct "+p+"_O*,int);\n"
     "struct "+p+"_VT{ "+p+"_M m; int tag; };\n"
     "struct "+p+"_O{ const struct "+p+"_VT* vt; int val; };\n"
     "static int "+p+"_addm(struct "+p+"_O*,int) __attribute__((noinline));\n"
     "static int "+p+"_mulm(struct "+p+"_O*,int) __attribute__((noinline));\n"
     "static const struct "+p+"_VT "+p+"_VA={"+p+"_addm,10};\n"
     "static const struct "+p+"_VT "+p+"_VM={"+p+"_mulm,20};\n"
     +t+" "+p+"_vtab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; struct "+p+"_O o[4]; long acc=0;\n"
     "  for(int i=0;i<4;i++){ o[i].vt=(i&1)?&"+p+"_VM:&"+p+"_VA; o[i].val=(int)(s+(unsigned)i); }\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"_O* q=&o[(s>>5)&3u];\n"
     "    acc+=q->vt->m(q,(int)(s>>9))+q->vt->tag; acc^=acc>>6; }\n"
     "  return ("+t+")(acc+o[0].val+o[3].val); }\n"
     "static int "+p+"_addm(struct "+p+"_O*o,int x){ o->val+=x; return o->val; }\n"
     "static int "+p+"_mulm(struct "+p+"_O*o,int x){ o->val*=(x|1); return o->val; }\n",
     {0xD1u}, "OptStress96", 2},

    // 2D global array, both indices runtime, transpose-style cross access.
    {p+"_mat2d",
     "static int "+p+"_M[8][8];\n"
     +t+" "+p+"_mat2d("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<8;i++)for(int j=0;j<8;j++){ s=s*1103515245u+12345u; "+p+"_M[i][j]=(int)(s>>8); }\n"
     "  for(int k=0;k<160;k++){ s=s*1103515245u+12345u;\n"
     "    int i=(int)((s>>4)&7u), j=(int)((s>>8)&7u);\n"
     "    "+p+"_M[i][j]+="+p+"_M[j][i]+(int)(s>>13);\n"
     "    acc+="+p+"_M[i][j]; acc^=acc>>5; }\n"
     "  return ("+t+")(acc+"+p+"_M[0][0]+"+p+"_M[7][7]); }\n",
     {0xD2u}, "OptStress96", 2},

    // Statically-initialized const pointer chain (.rodata relocations) walked at
    // runtime; the loaded `next` is a global address re-symbolized at the deref.
    {p+"_gchain",
     "struct "+p+"_N{ int v; const struct "+p+"_N* next; };\n"
     "static const struct "+p+"_N "+p+"_N4={50,0};\n"
     "static const struct "+p+"_N "+p+"_N3={40,&"+p+"_N4};\n"
     "static const struct "+p+"_N "+p+"_N2={30,&"+p+"_N3};\n"
     "static const struct "+p+"_N "+p+"_N1={20,&"+p+"_N2};\n"
     "static const struct "+p+"_N "+p+"_N0={10,&"+p+"_N1};\n"
     +t+" "+p+"_gchain("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    const struct "+p+"_N* q=&"+p+"_N0; int steps=(int)((s>>4)&7u);\n"
     "    while(steps-->0 && q->next) q=q->next;\n"
     "    acc+=q->v; acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0xD3u}, "OptStress96", 2},

    // Array of structs each holding a static pointer into a shared global table;
    // dispatch selects a struct, follows its pointer, indexes the target.
    {p+"_sptab",
     "static int "+p+"_P[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     "static int "+p+"_Q[16]={2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5};\n"
     "struct "+p+"_R{ const int* base; int bias; };\n"
     "static const struct "+p+"_R "+p+"_RT[4]={{"+p+"_P,1},{"+p+"_Q,2},{"+p+"_Q,3},{"+p+"_P,4}};\n"
     +t+" "+p+"_sptab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long acc=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    const struct "+p+"_R* r=&"+p+"_RT[(s>>4)&3u];\n"
     "    acc+=r->base[(s>>6)&15u]+r->bias; acc^=acc>>6; }\n"
     "  return ("+t+")(acc+"+p+"_P[5]+"+p+"_Q[10]); }\n",
     {0xD4u}, "OptStress96", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress96TC("x64o96", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress96TC("x86o96", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress96TC("a64o96", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress96TC("armo96", "int");

INSTANTIATE_TEST_SUITE_P(OptStress96, X64OptStress96RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress96, X86OptStress96RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress96, A64OptStress96RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress96, ARM32OptStress96RT, ::testing::ValuesIn(kARM), rtTCName);
