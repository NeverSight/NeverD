//===- AllPlatform_OptStress233RTTests.cpp - switch x call x spill =======//
//
// The recent red bugs cluster around switch/jump-table recovery interacting
// with calls and stack spills: #502(5) (ARM32 jump-table base preceded by a
// `bl` never folds), #491 (i386 jump-table target spilled to a stack slot
// drives the wrong selector), #449/#452/#453 (jump-table base via stack slot /
// peeled+rotated loop switch).  This file deliberately forces dense jump
// tables and then crosses them with noinline calls, high register pressure,
// nesting, a 64-bit selector and a computed-goto dispatch with calls.
//
//   * swcall8 - 8 dense cases, EVERY arm calls a noinline helper (base-after-bl).
//   * swspill - switch under high live-register pressure (spilled table base).
//   * nestsw  - switch nested inside a case of an outer switch.
//   * sw64    - selector taken from the high half of a 64-bit value.
//   * cgcall  - computed-goto (`goto *tab[op]`) dispatch with calls in handlers.
//   * swret   - switch arms that return early, mixed with a call.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress233RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress233RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress233RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress233RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress233RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress233RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress233RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress233RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress233TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 8 dense cases, every arm calls a noinline helper (jump-table base after bl).
    {p+"_swcall8",
     "static unsigned "+p+"_h8(unsigned,int) __attribute__((noinline));\n"
     +t+" "+p+"_swcall8("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned r;\n"
     "    switch(h&7u){\n"
     "      case 0: r="+p+"_h8(h,0); break; case 1: r="+p+"_h8(h,1); break;\n"
     "      case 2: r="+p+"_h8(h,2); break; case 3: r="+p+"_h8(h,3); break;\n"
     "      case 4: r="+p+"_h8(h,4); break; case 5: r="+p+"_h8(h,5); break;\n"
     "      case 6: r="+p+"_h8(h,6); break; default: r="+p+"_h8(h,7); }\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static unsigned "+p+"_h8(unsigned v,int k){ return (v>>(k&7))*2654435761u + (unsigned)k; }\n",
     {0x12345u}, "OptStress233", 2},

    // Switch under high live-register pressure (spilled jump-table base).
    {p+"_swspill",
     t+" "+p+"_swspill("+t+" a){ unsigned h=(unsigned)a;\n"
     "  unsigned v0=h^1u,v1=h^2u,v2=h^3u,v3=h^5u,v4=h^7u,v5=h^11u,v6=h^13u,v7=h^17u;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned r=0;\n"
     "    switch((h>>13)&7u){\n"
     "      case 0: r=v0+v7; break; case 1: r=v1*3u-v6; break;\n"
     "      case 2: r=v2^v5; break;  case 3: r=v3+v4*5u; break;\n"
     "      case 4: r=v4-v3; break;  case 5: r=v5^v2; break;\n"
     "      case 6: r=v6+v1; break;  default: r=v7*7u+v0; }\n"
     "    v0+=r; v1^=r; v2+=r>>1; v3^=r<<1; v4+=r; v5^=r; v6+=r; v7^=r;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")(acc+v0+v1+v2+v3+v4+v5+v6+v7); }\n",
     {0x23456u}, "OptStress233", 2},

    // Switch nested inside a case of an outer switch.
    {p+"_nestsw",
     t+" "+p+"_nestsw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned r=0;\n"
     "    switch((h>>3)&3u){\n"
     "      case 0: switch((h>>7)&3u){ case 0:r=1u;break; case 1:r=h;break;\n"
     "              case 2:r=h>>2;break; default:r=h*3u; } break;\n"
     "      case 1: r=h^0x55u; break;\n"
     "      case 2: switch((h>>9)&3u){ case 0:r=h+7u;break; case 1:r=h-3u;break;\n"
     "              case 2:r=h<<1;break; default:r=~h; } break;\n"
     "      default: r=h*2654435761u; }\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress233", 2},

    // Selector taken from the high half of a 64-bit value.
    {p+"_sw64",
     t+" "+p+"_sw64("+t+" a){ unsigned long long h=(unsigned long long)a^0x0F1E2D3C4B5A6978ULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*6364136223846793005ULL+1u;\n"
     "    unsigned sel=(unsigned)(h>>32)&7u; unsigned lo=(unsigned)h; unsigned r;\n"
     "    switch(sel){ case 0:r=lo;break; case 1:r=lo*3u;break; case 2:r=lo^0xffu;break;\n"
     "      case 3:r=lo>>3;break; case 4:r=lo+0x1000u;break; case 5:r=lo*5u;break;\n"
     "      case 6:r=lo^(unsigned)(h>>40);break; default:r=lo+(unsigned)(h>>48); }\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress233", 2},

    // Computed-goto dispatch with calls in handlers.
    {p+"_cgcall",
     "static unsigned "+p+"_cg(unsigned,int) __attribute__((noinline));\n"
     +t+" "+p+"_cgcall("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  static const void* const tab[4]={&&L0,&&L1,&&L2,&&L3};\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned r;\n"
     "    goto *tab[h&3u];\n"
     "    L0: r="+p+"_cg(h,0); goto done;\n"
     "    L1: r="+p+"_cg(h,1)+1u; goto done;\n"
     "    L2: r=h^"+p+"_cg(h,2); goto done;\n"
     "    L3: r="+p+"_cg(h,3)*3u; goto done;\n"
     "    done: acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static unsigned "+p+"_cg(unsigned v,int k){ return (v+(unsigned)k)*2246822519u; }\n",
     {0x56789u}, "OptStress233", 2},

    // Switch arms that return early, mixed with a call.
    {p+"_swret",
     "static unsigned "+p+"_rr(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_swret("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    switch(h&7u){\n"
     "      case 0: if(acc>0x40000000u) return ("+t+")(acc^"+p+"_rr(h)); acc+=h; break;\n"
     "      case 1: case 2: acc^="+p+"_rr(h); break;\n"
     "      case 3: acc+=h>>2; break;\n"
     "      case 7: if((h&0x100u)==0) { acc=acc*3u+"+p+"_rr(h); break; } acc-=h; break;\n"
     "      default: acc+=h*5u; }\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static unsigned "+p+"_rr(unsigned v){ return v*40503u ^ (v>>11); }\n",
     {0x6789Au}, "OptStress233", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress233TC("x64o233", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress233TC("x86o233", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress233TC("a64o233", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress233TC("armo233", "int");

INSTANTIATE_TEST_SUITE_P(OptStress233, X64OptStress233RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress233, X86OptStress233RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress233, A64OptStress233RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress233, ARM32OptStress233RT, ::testing::ValuesIn(kARM), rtTCName);
