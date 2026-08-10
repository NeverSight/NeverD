//===- AllPlatform_OptStress40RTTests.cpp - jump-table-in-loop --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Integer-only analogues of the x87 #452 `forcepeel` shape, isolating the two
// JumpTableResolver fixes from the open x87-TOP item so they are validated on
// every platform.  The kernels drive a `switch` inside a peeled and/or rotated
// loop whose dispatch table clang places in rodata next to another table or
// whose PIC base is materialised in the loop preheader:
//
//   * peel8   - an explicit `first` flag clang peels; the peeled copy and the
//               steady body each get a `switch(u%8)` table (`and $7`), placed
//               back-to-back -> exercises the mask over-read clamp and (on the
//               ARM32 inline `.text` table, whose dead odd cases point past the
//               function) the sparse-table case-label index recovery.
//   * twosw   - two masked switches in one loop body -> two adjacent tables.
//   * looptab - a rotated loop whose switch table base (PIC `lea tab(%rip)` /
//               ARM `adr`) is set in the preheader, after an early guard branch
//               -> exercises the cross-block foldRegConstant base recovery.
//   * guardpeel - peel triggered by a statically-false first-iteration branch.
//   * mod10   - `switch(u%10)` (non-power-of-two, magic-division index) in a
//               peeled loop -> confirms the mask clamp leaves modulo tables alone.
//
// Integer-only, single integer return, bounded, no 64-bit divide; all four
// targets at -O2, native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress40RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress40RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress40RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress40RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress40RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress40RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress40RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress40RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress40TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // peel8: explicit `first` flag peels the first iteration; both the peeled
    // copy and the steady loop body dispatch through a `switch(u%8)` (and $7).
    {p+"_peel8",
     t+" "+p+"_peel8("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u, x=1u, y=2u, h=0; int first=1;\n"
     "  for(int i=0;i<240;i++){ unsigned v=(u%53u);\n"
     "    if(first){ x+=5u; first=0; }\n"
     "    switch(u%8u){\n"
     "      case 0: x=x*3u+v; break;   case 1: y=y-v; break;\n"
     "      case 2: x=x+y*4u; break;   case 3: y=y*7u-v; break;\n"
     "      case 4: x=x+v; break;      case 5: x=(x>>1)+(y>>1); break;\n"
     "      case 6: y=y+(x>>3); break; default: x=x+y; break; }\n"
     "    h=h*131u+(x>y);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(h^x^(y<<1)); }\n",
     {0x17ULL}, "OptStress40", 2},

    // twosw: two masked switches in one loop body -> two adjacent rodata tables.
    {p+"_twosw",
     t+" "+p+"_twosw("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u, x=3u, y=5u, h=0;\n"
     "  for(int i=0;i<200;i++){ unsigned v=(u%29u);\n"
     "    switch(u%8u){\n"
     "      case 0: x+=v; break;       case 1: x-=v; break;\n"
     "      case 2: x*=3u; break;      case 3: x^=v; break;\n"
     "      case 4: x+=y; break;       case 5: x=(x>>1)+1u; break;\n"
     "      case 6: x+=v<<2; break;    default: x+=1u; break; }\n"
     "    switch((u>>4)%8u){\n"
     "      case 0: y+=x; break;       case 1: y-=v; break;\n"
     "      case 2: y*=5u; break;      case 3: y^=x; break;\n"
     "      case 4: y+=v; break;       case 5: y=(y>>1)+x; break;\n"
     "      case 6: y+=3u; break;      default: y+=v+1u; break; }\n"
     "    h=h*131u+(x^y);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(h^x^y); }\n",
     {0x3AULL}, "OptStress40", 2},

    // looptab: a rotated while-loop whose switch table base sits in the preheader
    // after an early `n==0` guard branch.
    {p+"_looptab",
     t+" "+p+"_looptab("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; int n=(int)((u%200u)+40u);\n"
     "  unsigned x=9u, y=1u, h=0;\n"
     "  if(n==0) return ("+t+")0;\n"
     "  while(n-->0){ unsigned v=(u%37u);\n"
     "    switch(u%8u){\n"
     "      case 0: x+=v; break;       case 1: y+=v; break;\n"
     "      case 2: x=x*3u+1u; break;  case 3: y=y*3u+1u; break;\n"
     "      case 4: x^=y; break;       case 5: y^=x; break;\n"
     "      case 6: x+=y>>1; break;    default: y+=x>>1; break; }\n"
     "    h=h*131u+(x+y);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(h^x^y); }\n",
     {0x53ULL}, "OptStress40", 2},

    // guardpeel: a statically-false first-iteration branch (acc>1e9 for acc=4)
    // is clang's classic peel trigger.
    {p+"_guardpeel",
     t+" "+p+"_guardpeel("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u, acc=4u, h=0;\n"
     "  for(int i=0;i<220;i++){ unsigned v=(u%31u);\n"
     "    if(acc>1000000000u){ acc-=1000000000u; h+=1u; }\n"
     "    switch(u%8u){\n"
     "      case 0: acc=acc*3u+v; break; case 1: acc-=v; break;\n"
     "      case 2: acc+=v<<3; break;    case 3: acc^=0x55u; break;\n"
     "      case 4: acc+=v; break;       case 5: acc=(acc>>1)+v; break;\n"
     "      case 6: acc+=11u; break;     default: acc=acc+v+1u; break; }\n"
     "    h=h*131u+acc;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(h^acc); }\n",
     {0x61ULL}, "OptStress40", 2},

    // peel16: a 16-way `switch(u&15)` over odd-only cases (1,3,..,15) in a
    // peeled loop.  Odd-only cases make clang normalise the index to
    // `(u&15)-1`; the peeled copy (u known odd) drops the `cmp` bound, so the
    // index-defining `sub idx,1` (and, on the steady body, the index register
    // reused as the loop counter `subs idx,idx,1`) must not be mistaken for a
    // range guard that collapses the unguarded peeled table.  On ARM32 the dead
    // even slots also point past the function (sparse inline `.text` table).
    {p+"_peel16",
     t+" "+p+"_peel16("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u, acc=2u, h=0; int first=1;\n"
     "  for(int i=0;i<220;i++){ unsigned v=(u%23u);\n"
     "    if(first){ acc+=9u; first=0; }\n"
     "    switch(u&15u){\n"
     "      case 1: acc+=v; break;     case 3: acc-=v; break;\n"
     "      case 5: acc*=3u; break;    case 7: acc^=v; break;\n"
     "      case 9: acc+=v<<1; break;  case 11: acc=(acc>>1)+v; break;\n"
     "      case 13: acc+=7u; break;   case 15: acc-=v>>1; break;\n"
     "      default: acc+=v+1u; break; }\n"
     "    h=h*131u+acc;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(h^acc); }\n",
     {0x29ULL}, "OptStress40", 2},

    // mod10: a non-power-of-two modulo switch in a peeled loop -> magic-division
    // index, confirming the power-of-two mask clamp does not disturb it.
    {p+"_mod10",
     t+" "+p+"_mod10("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u, acc=2u, h=0; int first=1;\n"
     "  for(int i=0;i<220;i++){ unsigned v=(u%23u);\n"
     "    if(first){ acc+=9u; first=0; }\n"
     "    switch(u%10u){\n"
     "      case 0: acc+=v; break;     case 1: acc-=v; break;\n"
     "      case 2: acc*=3u; break;    case 3: acc^=v; break;\n"
     "      case 4: acc+=v<<1; break;  case 5: acc=(acc>>1)+v; break;\n"
     "      case 6: acc+=7u; break;    case 7: acc-=v>>1; break;\n"
     "      case 8: acc*=2u; break;    default: acc+=v+1u; break; }\n"
     "    h=h*131u+acc;\n"
     "    u=u*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)(h^acc); }\n",
     {0x72ULL}, "OptStress40", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress40TC("x64o40", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress40TC("x86o40", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress40TC("a64o40", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress40TC("armo40", "int");

INSTANTIATE_TEST_SUITE_P(OptStress40, X64OptStress40RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress40, X86OptStress40RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress40, A64OptStress40RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress40, ARM32OptStress40RT, ::testing::ValuesIn(kARM), rtTCName);
