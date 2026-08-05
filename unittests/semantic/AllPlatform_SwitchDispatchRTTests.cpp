//===- AllPlatform_SwitchDispatchRTTests.cpp - jump-table edge shapes -*-C++*-=//
//
// #398 follow-up to the x86/x64 jump-table recovery fix.  Beyond the dense /
// sparse / nested shapes in AllPlatform_SwitchTableRTTests, these stress the
// table-index normalization the resolver must recover precisely:
//
//   * a switch whose cases start at a non-zero base (index = k - base),
//   * a signed switch spanning negative case values,
//   * a wide 32-way dense table,
//   * a char-class lexer dispatch (the canonical real-world jump table), and
//   * a multi-state machine that builds several tables in one function.
//
// Each keeps the dispatch inside a loop so every arm is exercised and the
// induction variables keep the table live.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SwDispRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SwDispRT, Verify) { roundTripX64(GetParam()); }
class X86SwDispRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SwDispRT, Verify) { roundTripX86(GetParam()); }
class A64SwDispRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SwDispRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SwDispRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SwDispRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSwDispTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Cases start at a non-zero base: index = k - 100, so the resolver must
    // recover the normalization base and still map every arm correctly.
    {p+"_base",
     t+" "+p+"_base("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned k=100u+(((unsigned)a+(unsigned)i)%12u); unsigned r;\n"
     "    switch(k){\n"
     "      case 100: r=(unsigned)a*3u+1u; break;  case 101: r=(unsigned)a^0x55u; break;\n"
     "      case 102: r=(unsigned)a<<2; break;     case 103: r=(unsigned)a>>1; break;\n"
     "      case 104: r=(unsigned)a+7u; break;     case 105: r=(unsigned)a*7u; break;\n"
     "      case 106: r=~(unsigned)a; break;       case 107: r=(unsigned)a&0xFF00u; break;\n"
     "      case 108: r=(unsigned)a|0x0Fu; break;  case 109: r=(unsigned)a*(unsigned)a; break;\n"
     "      case 110: r=(unsigned)a-13u; break;    default: r=0xABCDu; break; }\n"
     "    acc=acc*31u+r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ULL}, "SwDisp", 2},

    // Signed switch spanning negative case values (index = k + 4).
    {p+"_signed",
     t+" "+p+"_signed("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ int k=(int)(((unsigned)a+(unsigned)i)&7u)-4; unsigned r;\n"
     "    switch(k){\n"
     "      case -4: r=(unsigned)a*5u; break;   case -3: r=(unsigned)a^0x33u; break;\n"
     "      case -2: r=(unsigned)a+9u; break;   case -1: r=(unsigned)a<<3; break;\n"
     "      case 0:  r=~(unsigned)a; break;     case 1:  r=(unsigned)a>>2; break;\n"
     "      case 2:  r=(unsigned)a*(unsigned)a; break; case 3: r=(unsigned)a|0x07u; break;\n"
     "      default: r=0x1234u; break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x5ULL}, "SwDisp", 2},

    // Wide 32-way dense table.
    {p+"_wide32",
     t+" "+p+"_wide32("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned k=((unsigned)a+(unsigned)i)&31u; unsigned r;\n"
     "    switch(k){\n"
     "      case 0:r=(unsigned)a+1u;break;   case 1:r=(unsigned)a+2u;break;   case 2:r=(unsigned)a*3u;break;\n"
     "      case 3:r=(unsigned)a^4u;break;   case 4:r=(unsigned)a|5u;break;   case 5:r=(unsigned)a&6u;break;\n"
     "      case 6:r=(unsigned)a<<1;break;   case 7:r=(unsigned)a>>1;break;   case 8:r=~(unsigned)a;break;\n"
     "      case 9:r=(unsigned)a*9u;break;   case 10:r=(unsigned)a+10u;break; case 11:r=(unsigned)a^11u;break;\n"
     "      case 12:r=(unsigned)a*12u;break; case 13:r=(unsigned)a-13u;break; case 14:r=(unsigned)a|14u;break;\n"
     "      case 15:r=(unsigned)a&15u;break; case 16:r=(unsigned)a<<2;break;  case 17:r=(unsigned)a>>2;break;\n"
     "      case 18:r=(unsigned)a*18u;break; case 19:r=(unsigned)a^19u;break; case 20:r=(unsigned)a+20u;break;\n"
     "      case 21:r=(unsigned)a*21u;break; case 22:r=(unsigned)a|22u;break; case 23:r=(unsigned)a&23u;break;\n"
     "      case 24:r=(unsigned)a<<3;break;  case 25:r=(unsigned)a>>3;break;  case 26:r=(unsigned)a*26u;break;\n"
     "      case 27:r=(unsigned)a^27u;break; case 28:r=(unsigned)a+28u;break; case 29:r=(unsigned)a-29u;break;\n"
     "      case 30:r=(unsigned)a*30u;break; default:r=(unsigned)a^0x9E37u;break; }\n"
     "    acc=acc*31u+r+(acc>>11); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x13ULL}, "SwDisp", 2},

    // Char-class lexer dispatch — the canonical real-world jump table.
    {p+"_lex",
     t+" "+p+"_lex("+t+" a){\n"
     "  static const char s[24]=\"a1+B_ 9*z/Q(%7]gH-=tW^0\";\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<24;i++){ char c=s[i]; unsigned r;\n"
     "    switch(c){\n"
     "      case 'a': case 'e': case 'i': case 'o': case 'u': r=1u; break;\n"
     "      case '0': case '1': case '2': case '3': case '4':\n"
     "      case '5': case '6': case '7': case '8': case '9': r=2u+(unsigned)(c-'0'); break;\n"
     "      case '+': case '-': case '*': case '/': r=100u+(unsigned)c; break;\n"
     "      case ' ': case '\\t': case '\\n': r=200u; break;\n"
     "      default: r=((unsigned)c>='A'&&(unsigned)c<='Z')?300u+(unsigned)c:999u; break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x41ULL}, "SwDisp", 2},

    // State machine: the dispatch value is itself produced by the previous arm,
    // so several tables / a tight dispatch loop are exercised together.
    {p+"_state",
     t+" "+p+"_state("+t+" a){\n"
     "  unsigned acc=(unsigned)a; unsigned st=(unsigned)a&7u;\n"
     "  for(int i=0;i<48;i++){\n"
     "    switch(st){\n"
     "      case 0: acc+= (unsigned)a*3u; st=(acc>>2)&7u; break;\n"
     "      case 1: acc^= 0xA5A5u;        st=(acc+1u)&7u; break;\n"
     "      case 2: acc=(acc<<3)|(acc>>29); st=(acc^2u)&7u; break;\n"
     "      case 3: acc-= (unsigned)a+7u; st=(acc>>5)&7u; break;\n"
     "      case 4: acc*= 2654435761u;    st=(acc>>9)&7u; break;\n"
     "      case 5: acc+= ~(unsigned)a;   st=(acc+5u)&7u; break;\n"
     "      case 6: acc&= 0x0FFFFFFFu;    st=(acc>>3)&7u; break;\n"
     "      default: acc+= (unsigned)i;   st=(acc^7u)&7u; break; }\n"
     "    acc+=(unsigned)i; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x2ULL}, "SwDisp", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeSwDispTC("x64swd", "long");
static const std::vector<RoundTripTC> kX86 = makeSwDispTC("x86swd", "int");
static const std::vector<RoundTripTC> kA64 = makeSwDispTC("a64swd", "long");
static const std::vector<RoundTripTC> kARM = makeSwDispTC("armswd", "int");

INSTANTIATE_TEST_SUITE_P(SwDisp, X64SwDispRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwDisp, X86SwDispRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwDisp, A64SwDispRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwDisp, ARM32SwDispRT, ::testing::ValuesIn(kARM), rtTCName);
