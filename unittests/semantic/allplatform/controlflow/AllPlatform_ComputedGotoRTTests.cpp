//===- AllPlatform_ComputedGotoRTTests.cpp - computed-goto dispatch -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High-yield roundtrip probing of computed goto (GCC/clang label-as-value
// `goto *p`), the threaded-interpreter / state-machine dispatch shape.  clang
// lowers it to a memory-indirect branch through a `.data.rel.ro` array of
// absolute code-pointer relocations — distinct from a `switch`, which uses a
// PIC self-relative `.rodata` table.  That table has no comparison guard to
// bound it and (for 4-byte targets) is otherwise indistinguishable from a PIC
// offset table, so it exercises the loader's absolute-reloc application and the
// jump-table resolver's reloc-run bounding for memory- and register-indirect
// branches alike.  Each dispatch index is derived from a running value seeded
// by the argument so the optimizer cannot constant-fold the interpreter away.
// All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CGotoRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CGotoRT, Verify) { roundTripX64(GetParam()); }
class X86CGotoRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CGotoRT, Verify) { roundTripX86(GetParam()); }
class A64CGotoRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CGotoRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CGotoRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CGotoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCGotoTC(const char *prefix, const char *T,
                                            int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> Cases = {
    // 4-way computed-goto dispatch; next label index derived from the running
    // accumulator so the dispatch sequence is data-dependent (no constant fold).
    {p+"_disp4",
     t+" "+p+"_disp4("+t+" a){\n"
     "  static const void *tab[4]={&&L0,&&L1,&&L2,&&L3};\n"
     "  unsigned acc=(unsigned)a; int pc=0;\n"
     "  for(int i=0;i<60;i++){ goto *tab[pc&3];\n"
     "  L0: acc+=0x9E3779B9u; pc=(int)(acc&3); continue;\n"
     "  L1: acc^=(acc<<13)|(acc>>19); pc=(int)((acc>>7)&3); continue;\n"
     "  L2: acc*=2654435761u; pc=(int)((acc>>11)&3); continue;\n"
     "  L3: acc-=(acc>>5); pc=(int)((acc>>3)&3); continue; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x51ULL}, "CGoto", Opt},

    // 6-way threaded VM: a fixed bytecode whose operand stream is perturbed by
    // the argument, dispatched through a label table (multiple INDIR_BR copies
    // after -O2 tail-duplication, all sharing the same table base).
    {p+"_vm6",
     t+" "+p+"_vm6("+t+" a){\n"
     "  static const void *ops[6]={&&ADD,&&XOR,&&MUL,&&ROT,&&SUB,&&NXT};\n"
     "  unsigned acc=(unsigned)a|1u; int pc=0;\n"
     "  for(int i=0;i<72;i++){ int op=(int)((acc^(unsigned)i)%6u); goto *ops[op];\n"
     "  ADD: acc+=0x85EBCA6Bu; goto done;\n"
     "  XOR: acc^=acc>>15; goto done;\n"
     "  MUL: acc*=0xC2B2AE35u; goto done;\n"
     "  ROT: acc=(acc<<11)|(acc>>21); goto done;\n"
     "  SUB: acc-=(acc<<3); goto done;\n"
     "  NXT: acc+=0x9E3779B1u; goto done;\n"
     "  done: acc^=(unsigned)i*2246822519u; }\n"
     "  return ("+t+")(unsigned long)(acc^(acc>>16)); }\n",
     {0x1234ULL}, "CGoto", Opt},

    // State machine: the next state is selected from the running value, so the
    // edge sequence is fully dynamic.
    {p+"_state",
     t+" "+p+"_state("+t+" a){\n"
     "  static const void *st[5]={&&S0,&&S1,&&S2,&&S3,&&S4};\n"
     "  unsigned acc=(unsigned)a; unsigned h=0x811C9DC5u; int s=0;\n"
     "  for(int i=0;i<80;i++){ goto *st[s];\n"
     "  S0: h=(h^(acc&0xFF))*16777619u; s=(int)(h%5u); continue;\n"
     "  S1: h=(h+(acc>>8))*2654435761u; s=(int)((h>>3)%5u); continue;\n"
     "  S2: h^=(h<<7)^(acc>>3); s=(int)((h>>5)%5u); continue;\n"
     "  S3: h=(h>>11)|(h<<21); h+=acc; s=(int)((h>>1)%5u); continue;\n"
     "  S4: h-=(h>>6)+acc; s=(int)(h%5u); acc+=0x9E3779B9u; continue; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xABCDULL}, "CGoto", Opt},

    // Character-class dispatch over a derived byte stream (lexer shape).
    {p+"_lex",
     t+" "+p+"_lex("+t+" a){\n"
     "  static const void *cls[4]={&&DIG,&&ALP,&&SPC,&&OTH};\n"
     "  unsigned acc=(unsigned)a; unsigned tok=0;\n"
     "  for(int i=0;i<96;i++){ unsigned c=((acc>>(i&15))^(unsigned)(i*131))&0xFF;\n"
     "    int k = c<64?0 : c<128?1 : c<192?2 : 3; goto *cls[k];\n"
     "  DIG: tok=tok*10u+(c&7u); acc+=c; continue;\n"
     "  ALP: tok=(tok<<4)^(c|0x20u); acc^=c<<1; continue;\n"
     "  SPC: tok+=0x9E37u; acc=(acc<<1)|(acc>>31); continue;\n"
     "  OTH: tok^=c*2654435761u; acc-=c; continue; }\n"
     "  return ("+t+")(unsigned long)(tok^acc); }\n",
     {0x77ULL}, "CGoto", Opt},

    // Wide 12-way dispatch — exercises the reloc-run bound well past the small
    // cases and the larger label table.
    {p+"_wide12",
     t+" "+p+"_wide12("+t+" a){\n"
     "  static const void *t12[12]={&&W0,&&W1,&&W2,&&W3,&&W4,&&W5,\n"
     "                              &&W6,&&W7,&&W8,&&W9,&&W10,&&W11};\n"
     "  unsigned acc=(unsigned)a|1u; int pc=0;\n"
     "  for(int i=0;i<96;i++){ goto *t12[pc];\n"
     "  W0: acc+=1u; goto nx; W1: acc^=acc<<3; goto nx;\n"
     "  W2: acc*=3u; goto nx; W3: acc-=acc>>2; goto nx;\n"
     "  W4: acc+=0x9E37u; goto nx; W5: acc^=acc>>7; goto nx;\n"
     "  W6: acc=(acc<<5)|(acc>>27); goto nx; W7: acc+=acc<<1; goto nx;\n"
     "  W8: acc^=0xA5A5u; goto nx; W9: acc*=2654435761u; goto nx;\n"
     "  W10: acc-=0x61C8u; goto nx; W11: acc+=acc>>11; goto nx;\n"
     "  nx: pc=(int)(acc%12u); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x7ULL}, "CGoto", Opt},

    // Jump-threaded arithmetic: each block computes the next target index,
    // mixing add/sub/mul/shift paths driven by the accumulator's low bits.
    {p+"_thread",
     t+" "+p+"_thread("+t+" a){\n"
     "  static const void *tg[4]={&&T0,&&T1,&&T2,&&T3};\n"
     "  unsigned acc=(unsigned)a^0xDEADBEEFu; unsigned n=0;\n"
     "  goto *tg[acc&3];\n"
     "  T0: acc=acc*1664525u+1013904223u; n++; if(n>=64) goto fin; goto *tg[(acc>>2)&3];\n"
     "  T1: acc^=acc>>17; acc*=0xED5AD4BBu; n++; if(n>=64) goto fin; goto *tg[(acc>>5)&3];\n"
     "  T2: acc=(acc<<13)|(acc>>19); acc+=0x165667B1u; n++; if(n>=64) goto fin; goto *tg[(acc>>9)&3];\n"
     "  T3: acc-=(acc<<7); acc^=acc>>11; n++; if(n>=64) goto fin; goto *tg[(acc>>1)&3];\n"
     "  fin: return ("+t+")(unsigned long)(acc^n); }\n",
     {0x2468ULL}, "CGoto", Opt},
  };
  if (p == "x64cg")
    for (RoundTripTC &TC : Cases)
      if (TC.Name == "x64cg_vm6" || TC.Name == "x64cg_thread")
        TC.RecoveredSwitch = RecoveredSwitchExpectation::Required;
  return Cases;
}

// Mixed variable/CONSTANT-index dispatch: some goto-sites use a literal index
// (`goto *tab[2]`), which clang -O0 folds to a load from a constant table-slot
// address (no scaled index).  All sites funnel into one shared dispatch block,
// so its predecessors carry a mix of variable indices and constant-address
// loads.  Recovering the constant predecessors (constIndexFromTableLoad) is what
// keeps this from bailing to a loud trap; the other cases (all variable-index)
// never exercise it.  -O0 only: at -O2 clang hoists/folds the constant sites
// into a different (non-shared-dispatch) shape.
static std::vector<RoundTripTC> makeCGotoConstTC(const char *prefix,
                                                 const char *T) {
  std::string p = prefix, t = T;
  return {
    {p+"_kconst",
     t+" "+p+"_kconst("+t+" a){\n"
     "  static const void *tab[4]={&&K0,&&K1,&&K2,&&K3};\n"
     "  unsigned acc=(unsigned)a; int n=0;\n"
     "  goto *tab[acc&3];\n"
     "  K0: acc+=0x9E3779B9u;        if(++n>=40) goto fin; goto *tab[2];\n"
     "  K1: acc^=acc>>13;            if(++n>=40) goto fin; goto *tab[(acc>>4)&3];\n"
     "  K2: acc=acc*2654435761u+1u;  if(++n>=40) goto fin; goto *tab[0];\n"
     "  K3: acc-=acc>>5;             if(++n>=40) goto fin; goto *tab[(acc>>2)&3];\n"
     "  fin: return ("+t+")(unsigned long)(acc^(unsigned)n); }\n",
     {0x51ULL}, "CGoto", 0},
  };
}
// Local (non-`static`) label table: the computed-goto table is a stack-local
// array, so clang materialises it on the stack at -O0 by copying the read-only
// initializer run (which holds the absolute code-pointer rebases) into a frame
// slot, then dispatches `ldr xT,[sp+slot, idx, scale]`.  The table-base register
// folds to a stack address rather than a data segment, so the jump-table
// resolver must trace the slot back to its constant init source
// (resolveStackMaterializedTableSource) to recover the targets.  Covered on all
// four targets at small (<=4-entry) shapes whose initializer clang inlines as a
// direct copy: AArch64 (adrp/add __const base), x86-64 (RIP-relative scalar
// copy, frame offset in the dispatch load disp), i386 (PIC GOTOFF GOT-base-0),
// ARM32 (PC-relative literal-pool base + scaled index folded into the frame
// base).  Larger tables (clang memcpy/staged init on x86) remain a documented
// gap (localtab-cgoto).
static std::vector<RoundTripTC> makeCGotoLocalTC(const char *prefix,
                                                 const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> Cases = {
    {p+"_loc4",
     t+" "+p+"_loc4("+t+" a){\n"
     "  const void *tab[4]={&&L0,&&L1,&&L2,&&L3};\n"
     "  unsigned acc=(unsigned)a; int pc=0;\n"
     "  for(int i=0;i<60;i++){ goto *tab[pc&3];\n"
     "  L0: acc+=0x9E3779B9u; pc=(int)(acc&3); continue;\n"
     "  L1: acc^=(acc<<13)|(acc>>19); pc=(int)((acc>>7)&3); continue;\n"
     "  L2: acc*=2654435761u; pc=(int)((acc>>11)&3); continue;\n"
     "  L3: acc-=(acc>>5); pc=(int)((acc>>3)&3); continue; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x51ULL}, "CGoto", 0},

    // 8-entry local table: clang -O0 materialises a >=5-entry local label table
    // not store-by-store but with a single `memcpy(slot, __const_run, size)` of
    // the read-only initializer run.  constIndexFromTableLoad / the stack-table
    // resolver must recognise the memcpy table-init (named by a call-site
    // relocation, the relocatable object leaves its constant target at 0) to
    // recover the dispatch into a switch; the freestanding mem* helper the
    // fixture links makes both the original (real computed-goto) and recompiled
    // (switch) images runnable under Unicorn.
    {p+"_loc8",
     t+" "+p+"_loc8("+t+" a){\n"
     "  const void *tab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned acc=(unsigned)a; int pc=0;\n"
     "  for(int i=0;i<80;i++){ goto *tab[pc&7];\n"
     "  L0: acc+=0x9E3779B9u; pc=(int)(acc&7); continue;\n"
     "  L1: acc^=(acc<<13)|(acc>>19); pc=(int)((acc>>7)&7); continue;\n"
     "  L2: acc*=2654435761u; pc=(int)((acc>>11)&7); continue;\n"
     "  L3: acc-=(acc>>5); pc=(int)((acc>>3)&7); continue;\n"
     "  L4: acc+=0x85EBCA6Bu; pc=(int)((acc>>2)&7); continue;\n"
     "  L5: acc^=acc>>15; pc=(int)((acc>>1)&7); continue;\n"
     "  L6: acc=(acc<<5)|(acc>>27); pc=(int)((acc>>9)&7); continue;\n"
     "  L7: acc+=acc<<1; pc=(int)((acc>>4)&7); continue; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x51ULL}, "CGoto", 0, /*ExtraFlags=*/"", /*NoOpt=*/false,
     /*ClangTargetOverride=*/"", /*UcCpuModel=*/-1, /*LinkMemBuiltins=*/true},
  };
  for (RoundTripTC &TC : Cases)
    TC.RecoveredSwitch = RecoveredSwitchExpectation::Required;
  return Cases;
}

// A lexical initializer is not an all-path reaching-memory proof.  The test
// executes the fully initialized arm, while the feasible sibling deliberately
// leaves the runtime table undefined.  Recovering a static switch would erase
// that unsafe arm; the frame certificate must therefore fail closed.
static std::vector<RoundTripTC> makeCGotoFrameRejectTC(const char *prefix,
                                                       const char *T) {
  std::string p = prefix, t = T;
  RoundTripTC SiblingOnly{
      p + "_frame_sibling_init",
      t + " " + p + "_frame_sibling_init(" + t + " a){\n"
      "  if(!(((unsigned)a)&1u)) goto Dispatch;\n"
      "  const void *tab[4]={&&L0,&&L1,&&L2,&&L3};\n"
      "  Dispatch:;\n"
      "  unsigned pc=((unsigned)a>>1)&3u; goto *tab[pc];\n"
      "  L0:return (" + t + ")11; L1:return (" + t + ")22;\n"
      "  L2:return (" + t + ")33; L3:return (" + t + ")44; }\n",
      {1ULL}, "CGotoFrameReject", 0};
  SiblingOnly.RecoveredSwitch = RecoveredSwitchExpectation::Forbidden;
  return {std::move(SiblingOnly)};
}
// clang-format on

static const std::vector<RoundTripTC> kX64FrameReject =
    makeCGotoFrameRejectTC("x64cgr", "long");
INSTANTIATE_TEST_SUITE_P(CGotoFrameReject, X64CGotoRT,
                         ::testing::ValuesIn(kX64FrameReject), rtTCName);

#ifndef NEVERD_COMPUTED_GOTO_STRUCTURAL_ONLY
static const std::vector<RoundTripTC> kX64 = makeCGotoTC("x64cg", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeCGotoTC("x86cg", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeCGotoTC("a64cg", "long", 2);
static const std::vector<RoundTripTC> kARM = makeCGotoTC("armcg", "int", 2);

INSTANTIATE_TEST_SUITE_P(CGoto, X64CGotoRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGoto, X86CGotoRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGoto, A64CGotoRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGoto, ARM32CGotoRT, ::testing::ValuesIn(kARM), rtTCName);

// -O0 variants exercise the shared/decoupled computed-goto dispatch clang emits
// without optimization: each `goto *p` spills the table-loaded target to a
// common indirect-goto slot and branches to one shared dispatch block (`ldr
// xT,[slot]; br xT`), so the table load + index sit in *predecessor* goto-site
// blocks.  This is the P10-note shape the jump-table resolver (cross-block table
// recovery) and emitter (single-predecessor index in the dominating block;
// multi-site shared dispatch routed through a common index slot) handle.
// The -O0 sets also carry the mixed variable/constant-index dispatch case on
// all four targets.  A constant goto-site folds to a load from a constant
// table-slot address; constIndexFromTableLoad recovers the index across the
// three legal base models, distinguished structurally with no mis-routing risk:
// absolute base (AArch64 adrp / x86-64 RIP), model-zero PIC/GOT base register
// (i386 get_pc_thunk), and a PC-relative literal-pool base whose displacement is
// read from the image (ARM32).  Only the model that yields an in-range, aligned
// index is accepted; an ambiguous/unresolved site still bails to the loud trap.
static const auto appendConst = [](std::vector<RoundTripTC> v,
                                   std::vector<RoundTripTC> c) {
  v.insert(v.end(), c.begin(), c.end());
  return v;
};
// The stack-materialised local table is covered on all four targets: AArch64
// copies the small-table __const initializer run with a direct register copy;
// x86-64/i386 -O0 copy it scalar-by-scalar (`mov (%rip),%r; mov %r,-k(%rbp)`)
// with the table-base frame offset riding in the dispatch load displacement;
// ARM32 -O0 loads the run through a PC-relative literal-pool base and folds the
// scaled dispatch index into the frame base register.  resolveStackMaterialized-
// TableSource handles all three lowerings (constant RIP base, i386 GOTOFF
// GOT-base-0, ARM base+scaled-index reuse).
static const std::vector<RoundTripTC> kX64O0 =
    appendConst(appendConst(makeCGotoTC("x64cgo0", "long", 0),
                            makeCGotoConstTC("x64cgo0", "long")),
                makeCGotoLocalTC("x64cgo0", "long"));
static const std::vector<RoundTripTC> kX86O0 =
    appendConst(appendConst(makeCGotoTC("x86cgo0", "int", 0),
                            makeCGotoConstTC("x86cgo0", "int")),
                makeCGotoLocalTC("x86cgo0", "int"));
static const std::vector<RoundTripTC> kA64O0 =
    appendConst(appendConst(makeCGotoTC("a64cgo0", "long", 0),
                            makeCGotoConstTC("a64cgo0", "long")),
                makeCGotoLocalTC("a64cgo0", "long"));
static const std::vector<RoundTripTC> kARMO0 =
    appendConst(appendConst(makeCGotoTC("armcgo0", "int", 0),
                            makeCGotoConstTC("armcgo0", "int")),
                makeCGotoLocalTC("armcgo0", "int"));

INSTANTIATE_TEST_SUITE_P(CGotoO0, X64CGotoRT, ::testing::ValuesIn(kX64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGotoO0, X86CGotoRT, ::testing::ValuesIn(kX86O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGotoO0, A64CGotoRT, ::testing::ValuesIn(kA64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGotoO0, ARM32CGotoRT, ::testing::ValuesIn(kARMO0), rtTCName);
#else
// The standalone CI target is intentionally limited to the eight local-table
// cases that carry the structural switch assertion.  The monolithic semantic
// suite still instantiates the complete optimized and -O0 computed-goto
// matrix; duplicating that whole matrix here would add dozens of compile/link/
// Unicorn roundtrips without strengthening this platform-specific gate.
static const std::vector<RoundTripTC> kX64Local =
    makeCGotoLocalTC("x64cgo0", "long");
static const std::vector<RoundTripTC> kX86Local =
    makeCGotoLocalTC("x86cgo0", "int");
static const std::vector<RoundTripTC> kA64Local =
    makeCGotoLocalTC("a64cgo0", "long");
static const std::vector<RoundTripTC> kARMLocal =
    makeCGotoLocalTC("armcgo0", "int");

INSTANTIATE_TEST_SUITE_P(CGotoLocal, X64CGotoRT,
                         ::testing::ValuesIn(kX64Local), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGotoLocal, X86CGotoRT,
                         ::testing::ValuesIn(kX86Local), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGotoLocal, A64CGotoRT,
                         ::testing::ValuesIn(kA64Local), rtTCName);
INSTANTIATE_TEST_SUITE_P(CGotoLocal, ARM32CGotoRT,
                         ::testing::ValuesIn(kARMLocal), rtTCName);
#endif
