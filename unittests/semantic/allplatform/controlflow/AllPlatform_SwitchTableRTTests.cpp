//===- AllPlatform_SwitchTableRTTests.cpp - jump-table switch lowering -*-C++-*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// #397 high-yield probing of dense `switch` statements clang -O2 lowers to a
// rodata jump table + indirect branch — an indexed rodata access whose entries
// are *code* addresses, so it stresses both indirect-branch CFG recovery and the
// constant-pool redirection in one shape.  A nested / fall-through switch and a
// computed-goto-like dense dispatch loop add CFG complexity.
//
// Instantiated for all four platforms.  #398 closed the previously-documented
// x86/x64 gap: the generic JumpTableResolver mis-attributed shift carry-flag
// helper temps as an index normalization base (dropping x64 dense cases) and did
// not recognize the i386 PIC GOTOFF table shape (`disp(%ecx,%idx,4)` with the
// GOT base spilled to the stack), so i386 recovered no indirect branch at all.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SwRT : public SemanticRoundTripFixture,
               public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SwRT, Verify) { roundTripX64(GetParam()); }
class X86SwRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SwRT, Verify) { roundTripX86(GetParam()); }
class A64SwRT : public SemanticRoundTripFixture,
                public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SwRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SwRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SwRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSwTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Dense 16-way switch driven over a loop so every arm is taken.
    {p+"_dense",
     t+" "+p+"_dense("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<32;i++){ unsigned k=((unsigned)a+(unsigned)i)&15u; unsigned r;\n"
     "    switch(k){\n"
     "      case 0: r=(unsigned)a*3u+1u; break;   case 1: r=(unsigned)a^0x55u; break;\n"
     "      case 2: r=(unsigned)a<<2; break;       case 3: r=(unsigned)a>>1; break;\n"
     "      case 4: r=(unsigned)a+100u; break;     case 5: r=(unsigned)a*7u; break;\n"
     "      case 6: r=~(unsigned)a; break;         case 7: r=(unsigned)a&0xFF00u; break;\n"
     "      case 8: r=(unsigned)a|0x0Fu; break;    case 9: r=(unsigned)a*(unsigned)a; break;\n"
     "      case 10: r=(unsigned)a-13u; break;     case 11: r=((unsigned)a<<8)|((unsigned)a>>8); break;\n"
     "      case 12: r=(unsigned)a^0xAAAAu; break; case 13: r=(unsigned)a+(unsigned)i; break;\n"
     "      case 14: r=(unsigned)a*0x9E3779B1u; break; default: r=0x12345678u; break; }\n"
     "    acc=acc*31u+r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x7ULL}, "SwTable", 2},

    // Sparse switch with fall-through clusters (clang mixes table + compares).
    {p+"_sparse",
     t+" "+p+"_sparse("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned k=((unsigned)a*2654435761u+(unsigned)i)%100u; unsigned r=1u;\n"
     "    switch(k){\n"
     "      case 3: case 5: case 7: r=(unsigned)a+k; break;\n"
     "      case 11: case 13: r=(unsigned)a*k; break;\n"
     "      case 50: r=(unsigned)a^0xDEADu; break;\n"
     "      case 97: case 98: case 99: r=~(unsigned)a + k; break;\n"
     "      default: r=(unsigned)a>>(k&7); break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x3ULL}, "SwTable", 2},

    // Nested switch — two jump tables, outer arm selects inner dispatch.
    {p+"_nested",
     t+" "+p+"_nested("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<24;i++){ unsigned o=((unsigned)a+(unsigned)i)&3u, k=((unsigned)a>>2)+(unsigned)i; unsigned r=0u;\n"
     "    switch(o){\n"
     "      case 0: switch(k&3u){case 0:r=k+1;break;case 1:r=k*2;break;case 2:r=k^7u;break;default:r=k-1;}break;\n"
     "      case 1: r=k*3u+1u; break;\n"
     "      case 2: switch(k&3u){case 0:r=k|8u;break;case 1:r=k&0xFu;break;default:r=k<<1;}break;\n"
     "      default: r=~k; break; }\n"
     "    acc=acc*31u+r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ULL}, "SwTable", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeSwTC("x64sw", "long");
static const std::vector<RoundTripTC> kX86 = makeSwTC("x86sw", "int");
static const std::vector<RoundTripTC> kA64 = makeSwTC("a64sw", "long");
static const std::vector<RoundTripTC> kARM = makeSwTC("armsw", "int");

INSTANTIATE_TEST_SUITE_P(SwTable, X64SwRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwTable, X86SwRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwTable, A64SwRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwTable, ARM32SwRT, ::testing::ValuesIn(kARM), rtTCName);
