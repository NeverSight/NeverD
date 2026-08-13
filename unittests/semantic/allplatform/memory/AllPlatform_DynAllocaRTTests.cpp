//===- AllPlatform_DynAllocaRTTests.cpp - dynamic frame stressors -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Harder dynamic-stack-frame roundtrip probes than AllPlatform_VLAFrameRTTests,
// stressing the emitter's real-dynamic-`alloca` lowering of a runtime `sub sp`:
// a 2-D VLA `m[n][k]` (runtime row stride), a VLA passed by pointer to a
// noinline helper (the allocation address must survive as a call argument), a
// VLA plus a second `alloca` taken only on a runtime path (conditional dynamic
// frame), a VLA addressed by scattered runtime indices (not a sweep), two
// sequential VLAs where the first feeds the second, recursion where every frame
// allocates its own VLA, and an explicit `__builtin_alloca`.  All integer so no
// ARM32 soft-float libcall appears; each returns a value-dependent hash compiled
// at -O2, compared native vs lifted across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DynAllocaRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DynAllocaRT, Verify) { roundTripX64(GetParam()); }
class X86DynAllocaRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86DynAllocaRT, Verify) { roundTripX86(GetParam()); }
class A64DynAllocaRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64DynAllocaRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32DynAllocaRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DynAllocaRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeDynAllocaTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2-D VLA m[n][k]: the row stride k is a runtime value, so m[i][j] is
    // base + (i*k + j)*4 — a runtime-strided index into the dynamic region.
    {p+"_md2d",
     t+" "+p+"_md2d("+t+" a){\n"
     "  unsigned n=((unsigned)a&3u)+2u, k=(((unsigned)a>>4)&3u)+2u;\n"
     "  unsigned m[n][k]; unsigned h=0;\n"
     "  for(unsigned i=0;i<n;i++) for(unsigned j=0;j<k;j++) m[i][j]=i*k+j+(unsigned)a;\n"
     "  for(unsigned i=0;i<n;i++) for(unsigned j=0;j<k;j++) h=h*31u+m[i][j];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x35ULL}, "DynAlloca", 2},

    // VLA passed by pointer to a noinline helper: the allocation base must be
    // materialised as a real call argument (a pointer, not a stale frame disp).
    {p+"_helper",
     "static unsigned "+p+"_h(unsigned*,unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_helper("+t+" a){\n"
     "  unsigned n=((unsigned)a&7u)+2u;\n"
     "  unsigned b[n];\n"
     "  for(unsigned i=0;i<n;i++) b[i]=i*(unsigned)a+1u;\n"
     "  return ("+t+")(unsigned long)"+p+"_h(b,n); }\n"
     "static unsigned "+p+"_h(unsigned*p,unsigned n){\n"
     "  unsigned h=0; for(unsigned i=0;i<n;i++) h=h*131u+p[i]; return h; }\n",
     {0x53ULL}, "DynAlloca", 2},

    // VLA plus a second alloca taken only on a runtime path: the dynamic frame
    // is adjusted conditionally, so the second region must not corrupt the first.
    {p+"_cond",
     t+" "+p+"_cond("+t+" a){\n"
     "  unsigned n=((unsigned)a&7u)+2u; unsigned x[n]; unsigned h=0;\n"
     "  for(unsigned i=0;i<n;i++) x[i]=i*(unsigned)a+1u;\n"
     "  if((unsigned)a & 8u){\n"
     "    unsigned m=(((unsigned)a>>4)&7u)+2u; unsigned y[m];\n"
     "    for(unsigned i=0;i<m;i++) y[i]=(i^(unsigned)a)*2654435761u;\n"
     "    for(unsigned i=0;i<m;i++) h=h*131u+y[i]; }\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+x[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9CULL}, "DynAlloca", 2},

    // VLA addressed by scattered runtime indices (write/read jump around the
    // region by a hashed index, not a linear sweep).
    {p+"_scatter",
     t+" "+p+"_scatter("+t+" a){\n"
     "  unsigned n=((unsigned)a&15u)+4u; unsigned b[n];\n"
     "  for(unsigned i=0;i<n;i++) b[i]=i+1u;\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int t=0;t<80;t++){\n"
     "    unsigned wi=(x>>3)%n, ri=(x>>9)%n;\n"
     "    b[wi]=b[wi]*31u+(unsigned)t; unsigned v=b[ri];\n"
     "    h=h*131u+v+wi*7u+ri; x=x*1664525u+1013904223u^v; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABULL}, "DynAlloca", 2},

    // Two sequential VLAs where the first feeds the second: distinct dynamic
    // regions chained through a reduction.
    {p+"_twostage",
     t+" "+p+"_twostage("+t+" a){\n"
     "  unsigned n=((unsigned)a&7u)+2u; unsigned u[n];\n"
     "  for(unsigned i=0;i<n;i++) u[i]=i*(unsigned)a+7u;\n"
     "  unsigned m=(u[0]&7u)+2u; unsigned v[m]; unsigned h=0;\n"
     "  for(unsigned i=0;i<m;i++) v[i]=u[i%n]*2654435761u+i;\n"
     "  for(unsigned i=0;i<m;i++) h=h*131u+v[i];\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+u[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x71ULL}, "DynAlloca", 2},

    // Recursion where every frame allocates its own VLA: each invocation's
    // dynamic alloca must be independent (per-frame), not shared.
    {p+"_recur",
     "static unsigned "+p+"_r(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_recur("+t+" a){ return ("+t+")(unsigned long)"+p+"_r((unsigned)a); }\n"
     "static unsigned "+p+"_r(unsigned a){\n"
     "  unsigned n=(a&3u)+2u; unsigned b[n]; unsigned h=a;\n"
     "  for(unsigned i=0;i<n;i++) b[i]=i*a+1u;\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+b[i];\n"
     "  if(a>4u) h+="+p+"_r(a>>2);\n"
     "  return h; }\n",
     {0x5AULL}, "DynAlloca", 2},

    // Explicit __builtin_alloca: a runtime byte count, addressed as a word array.
    {p+"_explicit",
     t+" "+p+"_explicit("+t+" a){\n"
     "  unsigned n=((unsigned)a&7u)+2u;\n"
     "  unsigned *b=(unsigned*)__builtin_alloca(n*4u);\n"
     "  unsigned h=0;\n"
     "  for(unsigned i=0;i<n;i++) b[i]=(i^(unsigned)a)*2654435761u;\n"
     "  for(unsigned i=0;i<n;i++) h=h*31u+b[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x2DULL}, "DynAlloca", 2},
  };
}
// clang-format on

// Earlier these probes surfaced three pre-existing bugs in subsystems other than
// the dynamic frame, each now fixed and exercised on every target:
//   * `_recur` — a forwarder `f(a){return g(a);}` left its incoming register
//     argument unread, so parameter detection dropped it (recoverCallAbi now
//     recovers a forwarded live-in bounded by the callee arity).
//   * `_helper` — a pointer parameter first read in a non-entry block whose
//     register is reused folded to 0 (buildSsa now computes live-ins by a sound
//     backward liveness, so a sibling/return block's write no longer masks it).
//   * `_md2d` — a 2-D VLA's first element was addressed from the old stack
//     pointer (`old_sp - size`) and resolved to the fixed frame; the emitter now
//     maps such re-derivations back to the dynamic alloca.
static const std::vector<RoundTripTC> kX64 = makeDynAllocaTC("x64da", "long");
static const std::vector<RoundTripTC> kX86 = makeDynAllocaTC("x86da", "int");
static const std::vector<RoundTripTC> kA64 = makeDynAllocaTC("a64da", "long");
static const std::vector<RoundTripTC> kARM = makeDynAllocaTC("armda", "int");

INSTANTIATE_TEST_SUITE_P(DynAlloca, X64DynAllocaRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(DynAlloca, X86DynAllocaRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(DynAlloca, A64DynAllocaRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(DynAlloca, ARM32DynAllocaRT, ::testing::ValuesIn(kARM), rtTCName);
