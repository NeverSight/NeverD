//===- AllPlatform_OptStress254RTTests.cpp - switch / jump tables at -O0 ==//
//
// Switch dispatch, computed goto, and jump-table forms at -O0 — the code-pointer
// counterpart of the #507 / OptStress250-251 -O0 global probes.  At -O0 i386 and
// ARM32 reach the PIC jump table through the GOT base spilled to the stack
// (`call .+0;pop;add GOTPC` / `ldr;add pc`), the same spilled-base path that hid
// the #507 GOTOFF bugs, but here for code-pointer tables rather than data.
//
//   * dsw     - dense switch (contiguous labels), per-case accumulation.
//   * ssw     - sparse switch (non-contiguous labels).
//   * dupsw   - switch with several cases sharing one body (duplicate targets).
//   * nsw     - nested switch (table inside a table).
//   * swdat   - switch dispatch combined with a rodata lookup table.
//
// Computed goto (`goto *lab[op]`) at -O0 is intentionally NOT exercised here: at
// -O0 clang copies the .data.rel.ro label-address table into a stack array and
// the dispatch reads the stack copy through an extra spill slot, so recovering
// the indirect branch needs multi-block stack store-to-load forwarding plus
// stack-table -> source-data-table tracing.  The -O2 computed-goto form (which
// reads the label table directly) is already covered by OptStress201 `cgoto`.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress254RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress254RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress254RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress254RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress254RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress254RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress254RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress254RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress254TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Dense switch (0..7), distinct body per case.
    {p+"_dsw",
     t+" "+p+"_dsw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h&7u; unsigned r;\n"
     "    switch(v){ case 0:r=h+1u;break; case 1:r=h^0xffu;break; case 2:r=h*3u;break;\n"
     "      case 3:r=h>>2;break; case 4:r=h+0x1234u;break; case 5:r=~h;break;\n"
     "      case 6:r=h*5u+1u;break; default:r=h-7u;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress254", 0},

    // Sparse switch (non-contiguous labels).
    {p+"_ssw",
     t+" "+p+"_ssw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h%200u; unsigned r;\n"
     "    switch(v){ case 3:r=h+1u;break; case 17:r=h^0x55u;break; case 42:r=h*3u;break;\n"
     "      case 88:r=h>>3;break; case 130:r=h+9u;break; case 199:r=~h;break;\n"
     "      default:r=h+v;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress254", 0},

    // Switch with several cases sharing one body (duplicate jump-table targets).
    {p+"_dupsw",
     t+" "+p+"_dupsw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h&15u; unsigned r;\n"
     "    switch(v){ case 0: case 1: case 2: r=h+1u;break;\n"
     "      case 3: case 4: case 5: r=h*3u;break;\n"
     "      case 6: case 7: case 8: r=h>>2;break;\n"
     "      default: r=~h;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress254", 0},

    // Nested switch (outer selects a group, inner selects within it).
    {p+"_nsw",
     t+" "+p+"_nsw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned g=(h>>2)&3u, s=(h>>6)&3u; unsigned r=0;\n"
     "    switch(g){\n"
     "      case 0: switch(s){case 0:r=h+1u;break;case 1:r=h^2u;break;default:r=h+3u;break;} break;\n"
     "      case 1: switch(s){case 0:r=h*3u;break;case 2:r=h>>1;break;default:r=h+5u;break;} break;\n"
     "      case 2: r=h*7u; break;\n"
     "      default: switch(s){case 3:r=~h;break;default:r=h+s;break;} break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress254", 0},

    // Switch dispatch combined with a rodata lookup table.
    {p+"_swdat",
     "static const unsigned WT[8]={2654435761u,40503u,2246822519u,3266489917u,668265263u,374761393u,3332679571u,2147483647u};\n"
     +t+" "+p+"_swdat("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned v=h&7u; unsigned r;\n"
     "    switch(v){ case 0:r=WT[h&7u];break; case 1:r=WT[(h>>3)&7u]+1u;break;\n"
     "      case 2:r=WT[(h>>6)&7u]*3u;break; case 3:r=h+WT[0];break;\n"
     "      default:r=h^WT[v]; break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress254", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress254TC("x64o254", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress254TC("x86o254", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress254TC("a64o254", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress254TC("armo254", "int");

INSTANTIATE_TEST_SUITE_P(OptStress254, X64OptStress254RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress254, X86OptStress254RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress254, A64OptStress254RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress254, ARM32OptStress254RT, ::testing::ValuesIn(kARM), rtTCName);
