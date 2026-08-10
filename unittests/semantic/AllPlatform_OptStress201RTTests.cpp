//===- AllPlatform_OptStress201RTTests.cpp - threaded dispatch / switch ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Control-flow recovery guardrails beyond the dense masked switch: a computed-
// goto threaded dispatch (a distinct reloc-bounded jump-table recovery path with
// no comparison guard), a switch whose cases share targets, and a dense 8-way
// switch.  Each fold depends only on the rodata bytes + control flow (never an
// absolute VA), so the roundtrip comparison is meaningful on all four targets.
//
//   * cgoto    - a computed-goto (`goto *lab[op]`) bytecode interpreter; the
//                label table carries code-pointer relocations and is recovered as
//                a threaded dispatch rather than a guarded switch.
//   * dupcase  - a switch where several labels share one body (case 0,3 -> A;
//                1,4 -> B), exercising duplicate jump-table targets.
//   * dense8   - a dense 8-way switch (forces a real jump table on every target).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress201RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress201RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress201RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress201RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress201RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress201RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress201RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress201RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress201TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Computed-goto threaded-dispatch bytecode interpreter.
    {p+"_cgoto",
     "static const unsigned char "+p+"_pg[32]={\n"
     "1,5,2,3,0,2,1,7,3,4,0,9,2,1,1,6,3,8,0,5,2,2,1,4,3,3,0,7,2,6,1,0};\n"
     +t+" "+p+"_cgoto("+t+" a){\n"
     "  static const void *const lab[5]={&&OP0,&&OP1,&&OP2,&&OP3,&&OPH};\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int pc=0, steps=0;\n"
     "    DISPATCH: { unsigned op="+p+"_pg[pc]; if(op>3u)op=4u; goto *lab[op]; }\n"
     "    OP0: acc+=(unsigned)"+p+"_pg[pc+1]*131u; pc+=2; goto CONT;\n"
     "    OP1: acc^=(unsigned)"+p+"_pg[pc+1]<<3; pc+=2; goto CONT;\n"
     "    OP2: acc=(acc>>3)|(acc<<29); pc+=1; goto CONT;\n"
     "    OP3: acc-=(unsigned)"+p+"_pg[pc+1]; pc+=2; goto CONT;\n"
     "    OPH: acc=acc*31u+7u; pc+=1; goto CONT;\n"
     "    CONT: acc^=acc>>11; steps++;\n"
     "      if(steps<16 && pc<30) goto DISPATCH;\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x51u}, "OptStress201", 2},

    // Switch with several labels sharing one body (duplicate jump-table targets).
    {p+"_dupcase",
     "static const unsigned char "+p+"_dc[28]={\n"
     "0,3,1,4,2,5,0,1,3,2,4,5,1,0,2,3,5,4,0,2,1,3,4,0,5,1,2,3};\n"
     +t+" "+p+"_dupcase("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<28;k++){ unsigned u=(unsigned)"+p+"_dc[k];\n"
     "      switch(u){\n"
     "        case 0: case 3: acc+=u*131u; break;\n"
     "        case 1: case 4: acc^=u<<5; break;\n"
     "        case 2: case 5: acc-=u*7u; break;\n"
     "        default: acc=(acc>>4)|(acc<<28); break; }\n"
     "      out=out*31u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0x52u}, "OptStress201", 2},

    // Dense 8-way switch (forces a real jump table on every target).
    {p+"_dense8",
     "static const unsigned char "+p+"_d8[32]={\n"
     "0,5,2,7,1,6,3,4,7,2,5,0,3,6,1,4,2,0,7,5,1,3,6,4,0,2,4,6,1,3,5,7};\n"
     +t+" "+p+"_dense8("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int k=0;k<32;k++){ unsigned u=(unsigned)"+p+"_d8[k];\n"
     "      switch(u&7u){\n"
     "        case 0: acc+=u*131u; break;\n"
     "        case 1: acc^=u<<3; break;\n"
     "        case 2: acc-=u; break;\n"
     "        case 3: acc=(acc<<5)|(acc>>27); break;\n"
     "        case 4: acc+=u*7u; break;\n"
     "        case 5: acc^=u<<6; break;\n"
     "        case 6: acc=(acc>>2)|(acc<<30); break;\n"
     "        default: acc-=u*5u; break; }\n"
     "      out=out*31u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0x53u}, "OptStress201", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress201TC("x64o201", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress201TC("x86o201", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress201TC("a64o201", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress201TC("armo201", "int");

INSTANTIATE_TEST_SUITE_P(OptStress201, X64OptStress201RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress201, X86OptStress201RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress201, A64OptStress201RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress201, ARM32OptStress201RT, ::testing::ValuesIn(kARM), rtTCName);
