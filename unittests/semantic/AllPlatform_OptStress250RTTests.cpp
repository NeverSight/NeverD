//===- AllPlatform_OptStress250RTTests.cpp - writable globals at -O0 =====//
//
// Read-modify-write of WRITABLE globals (.data / .bss) at -O0, the writable
// counterpart to the #507 rodata probes.  Writable data resolves through a
// different emitter path (tryResolveWritableData / the writable-run embed) than
// rodata, and the i386/ARM32 -O0 PIC base (`call .+0;pop;add GOTPC` GOT base
// spilled to the stack, globals via `disp@GOTOFF(%base,%idx,s)`) reaches it the
// same way the rodata bugs did.  Each function folds the final global state into
// the integer return so a mis-symbolized base (stale VA / double base) shows up.
//
//   * ghist    - histogram into a .bss array (load-modify-store).
//   * gacc     - .data array seeded then RMW-accumulated.
//   * gstructrw- writable struct array, per-field RMW.
//   * gptrrw   - writable global walked by an induction pointer with RMW.
//   * gmixrd   - read a const table, accumulate into a .data array.
//   * g2drw    - 2D writable array RMW by computed row/col.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress250RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress250RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress250RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress250RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress250RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress250RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress250RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress250RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress250TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Histogram into a .bss array (load-modify-store by computed index).
    {p+"_ghist",
     "static unsigned HG[16];\n"
     +t+" "+p+"_ghist("+t+" a){ unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<16;i++) HG[i]=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; HG[(h>>5)&15u]+=(h&7u)+1u; }\n"
     "  unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+HG[i];\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress250", 0},

    // .data array seeded then RMW-accumulated.
    {p+"_gacc",
     "static unsigned AC[8]={11u,22u,33u,44u,55u,66u,77u,88u};\n"
     +t+" "+p+"_gacc("+t+" a){ unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned k=(h>>6)&7u;\n"
     "    AC[k]=AC[k]*131u+(h&0xffffu)+(unsigned)i; }\n"
     "  unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+AC[i];\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress250", 0},

    // Writable struct array, per-field RMW (mixed widths).
    {p+"_gstructrw",
     "static struct S{ unsigned char b; short s; int w; } SR[5];\n"
     +t+" "+p+"_gstructrw("+t+" a){ unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<5;i++){ SR[i].b=0; SR[i].s=0; SR[i].w=0; }\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; int k=(int)((h>>8)%5u);\n"
     "    SR[k].b=(unsigned char)(SR[k].b+(h&0xffu));\n"
     "    SR[k].s=(short)(SR[k].s+(short)(h>>16));\n"
     "    SR[k].w=SR[k].w+(int)(h&0xffffu); }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<5;i++) acc=acc*131u+(unsigned)SR[i].b+(unsigned)(int)SR[i].s+(unsigned)SR[i].w;\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress250", 0},

    // Writable global walked by an induction pointer with RMW.
    {p+"_gptrrw",
     "static unsigned PW[8];\n"
     +t+" "+p+"_gptrrw("+t+" a){ unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<8;i++) PW[i]=(unsigned)(i*7+1);\n"
     "  for(int rep=0;rep<24;rep++){ h=h*1103515245u+12345u;\n"
     "    unsigned *q=PW; for(int i=0;i<8;i++){ *q=(*q)*131u+h+(unsigned)i; q++; } }\n"
     "  unsigned acc=0; for(int i=0;i<8;i++) acc=acc*131u+PW[i];\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress250", 0},

    // Read a const table, accumulate into a .data array.
    {p+"_gmixrd",
     "static const unsigned KT[6]={2654435761u,40503u,2246822519u,3266489917u,668265263u,374761393u};\n"
     "static unsigned DT[6]={1u,2u,3u,4u,5u,6u};\n"
     +t+" "+p+"_gmixrd("+t+" a){ unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<180;i++){ h=h*1103515245u+12345u; unsigned k=(h>>4)%6u;\n"
     "    DT[k]=DT[k]*31u+KT[k]+(h&0xffu); }\n"
     "  unsigned acc=0; for(int i=0;i<6;i++) acc=acc*131u+DT[i];\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress250", 0},

    // 2D writable array RMW by computed row/col.
    {p+"_g2drw",
     "static unsigned MM[4][4];\n"
     +t+" "+p+"_g2drw("+t+" a){ unsigned h=(unsigned)a;\n"
     "  for(int r=0;r<4;r++) for(int c=0;c<4;c++) MM[r][c]=(unsigned)(r*4+c);\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=(h>>5)&3u, c=(h>>9)&3u; MM[r][c]=MM[r][c]*131u+(h&0xffu); }\n"
     "  unsigned acc=0; for(int r=0;r<4;r++) for(int c=0;c<4;c++) acc=acc*131u+MM[r][c];\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress250", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress250TC("x64o250", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress250TC("x86o250", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress250TC("a64o250", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress250TC("armo250", "int");

INSTANTIATE_TEST_SUITE_P(OptStress250, X64OptStress250RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress250, X86OptStress250RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress250, A64OptStress250RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress250, ARM32OptStress250RT, ::testing::ValuesIn(kARM), rtTCName);
