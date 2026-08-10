//===- AllPlatform_OptStress99RTTests.cpp - jump-table + rodata walk -*-C++-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Regression guard for #487: a `switch` lowered to a JUMP TABLE whose entries
// land in CodePtrRelocSlots, sharing a rodata run with a `static const` array
// that is then WALKED with a pc pointer (`prog[pc]`, clang -O2 turns the index
// walk into a base+stride pointer).  #483's addrInCodePtrMirrorRun gate treated
// the whole run as a code-pointer mirror because of the jump-table slots, which
// left the data-array base raw on i386 (the walked `inttoptr(p)` then read an
// unmapped original VA — exactly the OptStress55 `x86o55_regvm` failure).  The
// fix excludes recovered jump-table slots from that gate; these probes pin it
// across all four targets so the regression cannot silently return.
//
//   * swvm    - a 16-way bytecode VM (dense switch -> jump table) walking a
//               rodata program two bytes at a time.
//   * jtab    - an 8-way op switch PLUS a separate rodata lookup table read by a
//               runtime index, both globals in the same read-only run.
//   * revwalk - a 6-way switch walking a rodata bytecode BACKWARD (negative
//               stride pointer), the reverse of the strroll/regvm forward walk.
//
// All integer in / integer out, file-scope const tables (rodata + relocations),
// LCG-seeded, folded to one integer return; no float / 64-bit divide / libcall.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress99RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress99RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress99RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress99RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress99RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress99RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress99RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress99RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress99TC(const char *prefix, const char *T,
                                                 bool IncludeRevwalk) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> TCs = {
    // 16-way bytecode VM: dense switch (jump table) + pc-walked rodata program.
    {p+"_swvm",
     "static const unsigned char "+p+"_prog[64]={\n"
     "1,5, 0,7, 2,4, 3,9, 4,3, 5,6, 6,8, 7,2, 8,15, 9,11, 10,5, 11,3,\n"
     "12,9, 13,7, 14,1, 0,6, 3,11, 1,4, 2,8, 4,5, 5,1, 7,0, 9,2, 11,6,\n"
     "13,4, 6,3, 8,1, 10,7, 12,2, 14,5, 0,9, 2,3};\n"
     +t+" "+p+"_swvm("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<40;it++){ s=s*1103515245u+12345u; unsigned acc=s; int pc=0;\n"
     "    while(pc+1<64){ unsigned char op="+p+"_prog[pc]; unsigned imm="+p+"_prog[pc+1]; pc+=2;\n"
     "      switch(op&15){\n"
     "        case 0: acc+=imm; break;\n"
     "        case 1: acc-=imm; break;\n"
     "        case 2: acc^=imm; break;\n"
     "        case 3: acc=(acc<<(imm&31u))|(acc>>((32u-(imm&31u))&31u)); break;\n"
     "        case 4: acc*=(imm|1u); break;\n"
     "        case 5: acc|=imm; break;\n"
     "        case 6: acc&=(imm|0x80u); break;\n"
     "        case 7: acc+=acc>>3; break;\n"
     "        case 8: acc^=acc<<5; break;\n"
     "        case 9: acc-=imm*3u; break;\n"
     "        case 10: acc=~acc; break;\n"
     "        case 11: acc+=(acc<<7)^imm; break;\n"
     "        case 12: acc>>=(imm&7u); break;\n"
     "        case 13: acc<<=(imm&7u); break;\n"
     "        case 14: acc^=0xA5A5A5A5u; break;\n"
     "        default: acc+=imm^0xFFu; break; }\n"
     "      out=out*131u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0xF0u}, "OptStress99", 2},

    // 8-way op switch (jump table) + a separate rodata lookup table read by index.
    {p+"_jtab",
     "static const unsigned char "+p+"_ops[42]={\n"
     "0,3,1, 4,2,0, 8,1,3, 12,0,2, 1,3,1, 5,2,3, 9,1,0, 13,3,2,\n"
     "2,0,1, 6,2,0, 10,1,3, 14,0,2, 3,3,1, 7,2,3};\n"
     "static const unsigned "+p+"_tbl[16]={\n"
     "9u,7u,5u,3u,11u,13u,2u,17u,19u,23u,29u,31u,37u,41u,43u,47u};\n"
     +t+" "+p+"_jtab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<48;it++){ s=s*1103515245u+12345u; unsigned acc=s; int pc=0;\n"
     "    while(pc+2<42){ unsigned char op="+p+"_ops[pc]; unsigned d="+p+"_ops[pc+1], x="+p+"_ops[pc+2]; pc+=3;\n"
     "      unsigned k="+p+"_tbl[(acc>>3)&15u];\n"
     "      switch(op&7){\n"
     "        case 0: acc+=k*d; break;\n"
     "        case 1: acc^=k+x; break;\n"
     "        case 2: acc=(acc<<(d&31u))|(acc>>((32u-(d&31u))&31u)); break;\n"
     "        case 3: acc-=k^x; break;\n"
     "        case 4: acc*=(k|1u); break;\n"
     "        case 5: acc|=k<<(x&7u); break;\n"
     "        case 6: acc&=~(k<<(d&7u)); break;\n"
     "        default: acc+="+p+"_tbl[(x+d)&15u]; break; }\n"
     "      out=out*131u+acc; } }\n"
     "  return ("+t+")(out+"+p+"_tbl[3]+"+p+"_tbl[12]); }\n",
     {0x0Fu}, "OptStress99", 2},
  };

  // revwalk walks a rodata array BACKWARD; clang materializes the start pointer
  // as an INTERIOR address (`&bc[35]`) that on i386/ARM32 PIC the lifter emits as
  // a standalone 1-byte rodata global disconnected from the array's segment
  // global, so the negative-stride walk reads before it (unmapped).  That is the
  // deferred "unify rodata interior-pointer addressing model" limitation (the
  // strroll i386/ARM32 PIC class, docs #477/#487) — independent of the #487
  // jump-table-run gate this file guards (raw original VA would also be unmapped),
  // so it runs only on the 64-bit targets where the interior pointer resolves.
  if (IncludeRevwalk)
    TCs.push_back(
    {p+"_revwalk",
     "static const unsigned char "+p+"_bc[36]={\n"
     "2,1, 0,5, 3,2, 1,7, 4,3, 5,0, 2,6, 0,9, 3,1, 1,4, 4,2, 5,3,\n"
     "2,8, 0,1, 3,5, 1,2, 4,7, 5,4};\n"
     +t+" "+p+"_revwalk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, out=0;\n"
     "  for(int it=0;it<44;it++){ s=s*1103515245u+12345u; unsigned acc=s; int pc=34;\n"
     "    while(pc>=0){ unsigned char op="+p+"_bc[pc]; unsigned imm="+p+"_bc[pc+1]; pc-=2;\n"
     "      switch(op){\n"
     "        case 0: acc+=imm*131u; break;\n"
     "        case 1: acc^=(imm<<3)|imm; break;\n"
     "        case 2: acc=(acc>>(imm&15u))|(acc<<((32u-(imm&15u))&31u)); break;\n"
     "        case 3: acc-=imm; break;\n"
     "        case 4: acc*=(imm|3u); break;\n"
     "        default: acc&=~(unsigned)imm; break; }\n"
     "      out=out*31u+acc; } }\n"
     "  return ("+t+")out; }\n",
     {0x99u}, "OptStress99", 2});
  return TCs;
}
// clang-format on

// revwalk (backward interior-pointer walk) — x86-64/i386/AArch64 anchor the
// interior `&bc[last]` pointer to the contiguous rodata run global so the
// negative-stride walk stays in bounds.  ARM32's PC-relative literal-pool base
// is symbolized separately (see kARM below).
static const std::vector<RoundTripTC> kX64 = makeOptStress99TC("x64o99", "long", true);
static const std::vector<RoundTripTC> kX86 = makeOptStress99TC("x86o99", "int", true);
static const std::vector<RoundTripTC> kA64 = makeOptStress99TC("a64o99", "long", true);
static const std::vector<RoundTripTC> kARM = makeOptStress99TC("armo99", "int", true);

INSTANTIATE_TEST_SUITE_P(OptStress99, X64OptStress99RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress99, X86OptStress99RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress99, A64OptStress99RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress99, ARM32OptStress99RT, ::testing::ValuesIn(kARM), rtTCName);
