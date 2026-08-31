//===- AllPlatform_SwitchXformTableRTTests.cpp - table extents --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Table-extent / index-provenance half of the switch transformation probing
// suite (see AllPlatform_SwitchXformRTTests.cpp for the shared rationale and
// the roundtrip protocol).  These shapes push on the resolver's recursion and
// register-aliasing seams and on the emitter's table extent, rather than on
// which lowering clang picks:
//   * deeply nested switches whose levels all dispatch on slices of the SAME
//     accumulator, so every table index aliases one live value;
//   * large dense tables sized around the compact-table width boundaries — on
//     AArch64 the byte (ldrb) table below the threshold and the halfword
//     (ldrh, lsl #1) table above it, and on ARM32 the TBB -> TBH crossover;
//   * a shifted mid-bit index feeding a large table;
//   * data-dependent chained dispatch, where the second table's index is the
//     result the first switch produced;
//   * a fall-through chain whose arms share blocks (no break);
//   * an index loaded from a read-only global array (a memory operand, not a
//     masked register);
//   * a nested pair at -O0 where both indices spill to the frame and reload
//     into shared registers.
//
//===----------------------------------------------------------------------===//

#include "SwitchXformRTFixture.h"

// clang-format off
// Fourth batch: shapes that stress the resolver's recursion / register-aliasing
// and the emitter's table extent — the three prior fixes in this suite were a
// nested-switch shared-register bound alias, a deep esp-chain recursion, and a
// narrow-constant APInt assertion, so these deliberately push on those seams:
//   * deepnest  — three switch levels nested in one arm, every level dispatching
//     on a slice of the SAME running accumulator (so all three table indices
//     alias one value across the block — the hostile case for the shared-reg
//     index-bound logic refined for two levels);
//   * biglut    — a ~140-arm dense jump table (case bodies cycle through eight
//     distinct op kinds so clang keeps a CODE jump table, not a rodata value
//     lookup), forcing a large table whose extent bound must include every
//     entry; on ARM32 >128 entries overflow a TBB byte table into a TBH
//     halfword table (a distinct compact-table path);
//   * chained   — switch #1 produces a value that becomes switch #2's selector
//     in the same iteration, so two dispatches are data-dependent (the index of
//     the second table is the loaded result of the first).
static std::vector<RoundTripTC> makeSwXform4TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;

  // Build a dense N-arm switch whose case bodies cycle through eight genuinely
  // distinct operation kinds (add / xor-shift / mul / sub-shift / rotate / etc.)
  // keyed by the arm index, so clang lowers it to a real code jump table rather
  // than a rodata value table.  The selector is reduced mod N so every index is
  // in range and the (present) default is never taken.
  const int N = 140;
  std::string big;
  big += t + " " + p + "_biglut(" + t + " a){\n";
  big += "  unsigned acc=(unsigned)a|1u;\n";
  big += "  for(int i=0;i<64;i++){\n";
  big += "    unsigned k=acc % " + std::to_string(N) + "u;\n";
  big += "    switch(k){\n";
  for (int j = 0; j < N; ++j) {
    std::string c = std::to_string(j);
    std::string body;
    switch (j % 8) {
    case 0: body = "acc+=0x9E3779B9u+" + c + "u;"; break;
    case 1: body = "acc^=acc<<((" + c + "u&7u)+1u);"; break;
    case 2: body = "acc*=(2u*" + c + "u+3u);"; break;
    case 3: body = "acc-=acc>>((" + c + "u&7u)+1u);"; break;
    case 4: body = "acc+=0x85EBCA6Bu^" + c + "u;"; break;
    case 5: body = "acc^=acc>>((" + c + "u&7u)+3u);"; break;
    case 6: body = "acc=(acc<<5)|(acc>>27);acc+=" + c + "u;"; break;
    default: body = "acc+=(" + c + "u*2654435761u);"; break;
    }
    big += "    case " + c + ": " + body + " break;\n";
  }
  big += "    default: acc^=0xDEADu; break;\n";
  big += "    }\n";
  big += "    acc+=(unsigned)i*131u;\n";
  big += "  }\n";
  big += "  return (" + t + ")(unsigned long)acc; }\n";

  std::vector<RoundTripTC> Cases{
    // Three levels of switch nested in a single arm, each dispatching on a
    // different bit-slice of the same accumulator — all three table indices
    // alias one value live across the dispatch block.
    {p+"_deepnest",
     t+" "+p+"_deepnest("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(acc & 3u){\n"
     "    case 0: acc+=0x9E37u; break;\n"
     "    case 1: acc^=acc<<7; break;\n"
     "    case 2:\n"
     "      switch((acc>>2)&3u){\n"
     "      case 0: acc+=1u; break;\n"
     "      case 1:\n"
     "        switch((acc>>4)&3u){\n"
     "        case 0: acc*=3u; break;      case 1: acc-=acc>>4; break;\n"
     "        case 2: acc+=0x1234u; break; default: acc^=0xFFu; break;\n"
     "        }\n"
     "        break;\n"
     "      case 2: acc^=0x55u; break; default: acc-=0x777u; break;\n"
     "      }\n"
     "      break;\n"
     "    default: acc=acc*31u+0x1234u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1BADULL}, "SwXform4", Opt},

    {p+"_biglut", big, {0x33CCULL}, "SwXform4", Opt},

    // Data-dependent chained dispatch: switch #1 assigns a selector `t`, and
    // switch #2 dispatches on `t & 7` in the same iteration — the second table's
    // index is the result the first switch produced.
    {p+"_chained",
     t+" "+p+"_chained("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned t;\n"
     "    switch(acc & 7u){\n"
     "    case 0: t=0x9E37u; break; case 1: t=acc<<3; break;\n"
     "    case 2: t=acc*3u; break;  case 3: t=acc>>2; break;\n"
     "    case 4: t=acc+7u; break;  case 5: t=acc^0x55u; break;\n"
     "    case 6: t=acc-9u; break;  default: t=0xA5u; break;\n"
     "    }\n"
     "    switch(t & 7u){\n"
     "    case 0: acc^=0x11u; break; case 1: acc+=0x33u; break;\n"
     "    case 2: acc-=0x55u; break; case 3: acc^=0x77u; break;\n"
     "    case 4: acc+=0x99u; break; case 5: acc^=0xBBu; break;\n"
     "    case 6: acc-=0xDDu; break; default: acc+=0xFFu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x77E1ULL}, "SwXform4", Opt},
  };
  // The i386 -O0 shape previously exhausted the consumer-audit budget.
  // The x86-64 O2 shape has two dispatches over the same 140-slot table and
  // previously oscillated their proposal ranks; the O0 source object has one.
  // Keep the round-trip checks and require every physical consumer so an
  // opaque branch cannot become an accidental semantic pass.
  const bool RequireBigLUTSwitch =
      (p == "x86sww0" && Opt == 0) || (p == "x64sww" && Opt == 2) ||
      (p == "x64sww0" && Opt == 0);
  if (RequireBigLUTSwitch)
    for (RoundTripTC &TC : Cases)
      if (TC.Name == p + "_biglut") {
        TC.RecoveredSwitch = RecoveredSwitchExpectation::Required;
        TC.MinimumRecoveredSwitchCount = p == "x64sww" ? 2 : 1;
      }
  return Cases;
}

// Fifth batch: shapes complementary to biglut — a mid-size dense table (~64
// arms) that on AArch64 selects the BYTE compact table (`ldrb [base,idx]`, the
// unscaled-index sibling of biglut's halfword `ldrh [base,idx,lsl #1]`), a
// fall-through chain where cases share blocks (no break), and a signed 64-bit
// sparse switch (a binary-search tree over 64-bit signed labels).
static std::vector<RoundTripTC> makeSwXform5TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;

  // ~64-arm dense switch with small case bodies so the per-arm offset stays
  // under a byte: on AArch64 this selects the byte offset table (ldrb), the
  // path biglut's halfword table does not exercise.
  const int N = 64;
  std::string mid;
  mid += t + " " + p + "_midbyte(" + t + " a){\n";
  mid += "  unsigned acc=(unsigned)a|1u;\n";
  mid += "  for(int i=0;i<64;i++){\n";
  mid += "    unsigned k=acc % " + std::to_string(N) + "u;\n";
  mid += "    switch(k){\n";
  for (int j = 0; j < N; ++j) {
    std::string c = std::to_string(j);
    std::string body;
    switch (j % 4) {
    case 0: body = "acc+=" + c + "u;"; break;
    case 1: body = "acc^=" + c + "u;"; break;
    case 2: body = "acc-=" + c + "u;"; break;
    default: body = "acc+=" + std::to_string(j * 7 + 1) + "u;"; break;
    }
    mid += "    case " + c + ": " + body + " break;\n";
  }
  mid += "    default: acc^=0xDEADu; break;\n";
  mid += "    }\n";
  mid += "    acc+=(unsigned)i*131u;\n";
  mid += "  }\n";
  mid += "  return (" + t + ")(unsigned long)acc; }\n";

  return {
    {p+"_midbyte", mid, {0x5151ULL}, "SwXform5", Opt},

    // Fall-through chain: consecutive cases with no `break` fall into the next,
    // so the case blocks are shared/chained (like Duff's device but without the
    // loop) — the structuring must not duplicate or drop the shared tails.
    {p+"_fallthru",
     t+" "+p+"_fallthru("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned s=0; unsigned k=acc & 7u;\n"
     "    switch(k){\n"
     "    case 0: s+=0x11u;\n"
     "    case 1: s+=0x22u;\n"
     "    case 2: s+=0x33u;\n"
     "    case 3: s+=0x44u; break;\n"
     "    case 4: s+=0x55u;\n"
     "    case 5: s+=0x66u;\n"
     "    default: s+=0x77u;\n"
     "    }\n"
     "    acc=acc*31u + s + (unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x6262ULL}, "SwXform5", Opt},

    // Index loaded from a read-only global array indexed by the running value,
    // so the switch selector is a memory load (not a masked register) — the
    // index-tracing must follow the load, not stop at a fresh register.
    {p+"_memidx",
     "static const unsigned char "+p+"_mtab[16]="
     "{3,1,4,1,5,2,6,5,3,0,7,4,2,6,0,7};\n"+
     t+" "+p+"_memidx("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k="+p+"_mtab[acc & 15u];\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=acc>>3; break;\n"
     "    case 4: acc+=0x1111u; break; case 5: acc^=0x2222u; break;\n"
     "    case 6: acc+=acc<<2; break;  default: acc^=0xA5u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x7373ULL}, "SwXform5", Opt},
  };
}

// Seventh batch: index-provenance shapes that combine the two just-fixed seams.
// bigbyte is a ~120-arm dense table sized so AArch64 keeps a BYTE compact table
// yet close enough to the halfword boundary to stress the width choice; wideidx
// dispatches on a mid-bit slice `(v>>5)&0x7f` into a 128-arm table (a shifted
// index feeding a large table); spillnest is a two-level nested switch at every
// opt level so both the outer and inner index spill/reload and share registers
// (Bug ② scenario, one level deeper).
static std::vector<RoundTripTC> makeSwXform7TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;

  auto denseBody = [](int j) -> std::string {
    std::string c = std::to_string(j);
    switch (j % 6) {
    case 0: return "acc+=" + c + "u;";
    case 1: return "acc^=(" + c + "u<<1);";
    case 2: return "acc-=" + c + "u;";
    case 3: return "acc+=(acc>>2)+" + c + "u;";
    case 4: return "acc^=(acc<<1)^" + c + "u;";
    default: return "acc+=(" + std::to_string(j * 3 + 1) + "u);";
    }
  };

  // ~120-arm dense table: small case bodies keep AArch64 on the byte compact
  // table, just under the >127-arm halfword threshold biglut crossed.
  const int NB = 120;
  std::string bb;
  bb += t + " " + p + "_bigbyte(" + t + " a){\n";
  bb += "  unsigned acc=(unsigned)a|1u;\n";
  bb += "  for(int i=0;i<64;i++){\n";
  bb += "    unsigned k=acc % " + std::to_string(NB) + "u;\n";
  bb += "    switch(k){\n";
  for (int j = 0; j < NB; ++j)
    bb += "    case " + std::to_string(j) + ": " + denseBody(j) + " break;\n";
  bb += "    default: acc^=0xDEADu; break;\n    }\n";
  bb += "    acc+=(unsigned)i*131u;\n  }\n";
  bb += "  return (" + t + ")(unsigned long)acc; }\n";

  // 128-arm table dispatched on a mid-bit slice (v>>5)&0x7f — a shifted index
  // feeding a large table (index recovery must trace the shift + mask).
  const int NW = 128;
  std::string wi;
  wi += t + " " + p + "_wideidx(" + t + " a){\n";
  wi += "  unsigned acc=(unsigned)a|1u;\n";
  wi += "  for(int i=0;i<64;i++){\n";
  wi += "    unsigned k=(acc>>5) & 0x7Fu;\n";
  wi += "    switch(k){\n";
  for (int j = 0; j < NW; ++j)
    wi += "    case " + std::to_string(j) + ": " + denseBody(j) + " break;\n";
  wi += "    default: acc^=0xBEEFu; break;\n    }\n";
  wi += "    acc+=(unsigned)i*131u;\n  }\n";
  wi += "  return (" + t + ")(unsigned long)acc; }\n";

  return {
    {p+"_bigbyte", bb, {0x1234ULL}, "SwXform7", Opt},
    {p+"_wideidx", wi, {0x5678ULL}, "SwXform7", Opt},

    // Two-level nested switch, both dispatching on slices of the same value, so
    // at -O0 both indices spill to the frame and reload into shared registers —
    // the Bug ② scenario extended to a nested pair.
    {p+"_spillnest",
     t+" "+p+"_spillnest("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(acc & 7u){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=acc>>3; break;\n"
     "    case 4:\n"
     "      switch((acc>>3) & 7u){\n"
     "      case 0: acc+=0x11u; break; case 1: acc+=0x33u; break;\n"
     "      case 2: acc-=0x55u; break; case 3: acc^=0x77u; break;\n"
     "      case 4: acc+=0x99u; break; case 5: acc^=0xBBu; break;\n"
     "      case 6: acc-=0xDDu; break; default: acc+=0xFFu; break;\n"
     "      }\n"
     "      break;\n"
     "    case 5: acc^=0x2222u; break; case 6: acc+=acc<<2; break;\n"
     "    default: acc^=0xA5u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x4321ULL}, "SwXform7", Opt},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64D =
    makeSwXform4TC("x64sww", "long", 2);
static const std::vector<RoundTripTC> kX86D =
    makeSwXform4TC("x86sww", "int", 2);
static const std::vector<RoundTripTC> kA64D =
    makeSwXform4TC("a64sww", "long", 2);
static const std::vector<RoundTripTC> kARMD =
    makeSwXform4TC("armsww", "int", 2);
static const std::vector<RoundTripTC> kX64D0 =
    makeSwXform4TC("x64sww0", "long", 0);
static const std::vector<RoundTripTC> kX86D0 =
    makeSwXform4TC("x86sww0", "int", 0);
static const std::vector<RoundTripTC> kA64D0 =
    makeSwXform4TC("a64sww0", "long", 0);
static const std::vector<RoundTripTC> kARMD0 =
    makeSwXform4TC("armsww0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform4, X64SwXformRT, ::testing::ValuesIn(kX64D),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4, X86SwXformRT, ::testing::ValuesIn(kX86D),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4, A64SwXformRT, ::testing::ValuesIn(kA64D),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4, ARM32SwXformRT, ::testing::ValuesIn(kARMD),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, X64SwXformRT, ::testing::ValuesIn(kX64D0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, X86SwXformRT, ::testing::ValuesIn(kX86D0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, A64SwXformRT, ::testing::ValuesIn(kA64D0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, ARM32SwXformRT,
                         ::testing::ValuesIn(kARMD0), rtTCName);

static const std::vector<RoundTripTC> kX64E =
    makeSwXform5TC("x64swv", "long", 2);
static const std::vector<RoundTripTC> kX86E =
    makeSwXform5TC("x86swv", "int", 2);
static const std::vector<RoundTripTC> kA64E =
    makeSwXform5TC("a64swv", "long", 2);
static const std::vector<RoundTripTC> kARME =
    makeSwXform5TC("armswv", "int", 2);
static const std::vector<RoundTripTC> kX64E0 =
    makeSwXform5TC("x64swv0", "long", 0);
static const std::vector<RoundTripTC> kX86E0 =
    makeSwXform5TC("x86swv0", "int", 0);
static const std::vector<RoundTripTC> kA64E0 =
    makeSwXform5TC("a64swv0", "long", 0);
static const std::vector<RoundTripTC> kARME0 =
    makeSwXform5TC("armswv0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform5, X64SwXformRT, ::testing::ValuesIn(kX64E),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5, X86SwXformRT, ::testing::ValuesIn(kX86E),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5, A64SwXformRT, ::testing::ValuesIn(kA64E),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5, ARM32SwXformRT, ::testing::ValuesIn(kARME),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, X64SwXformRT, ::testing::ValuesIn(kX64E0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, X86SwXformRT, ::testing::ValuesIn(kX86E0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, A64SwXformRT, ::testing::ValuesIn(kA64E0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, ARM32SwXformRT,
                         ::testing::ValuesIn(kARME0), rtTCName);

static const std::vector<RoundTripTC> kX64G =
    makeSwXform7TC("x64swt", "long", 2);
static const std::vector<RoundTripTC> kX86G =
    makeSwXform7TC("x86swt", "int", 2);
static const std::vector<RoundTripTC> kA64G =
    makeSwXform7TC("a64swt", "long", 2);
static const std::vector<RoundTripTC> kARMG =
    makeSwXform7TC("armswt", "int", 2);
static const std::vector<RoundTripTC> kX64G0 =
    makeSwXform7TC("x64swt0", "long", 0);
static const std::vector<RoundTripTC> kX86G0 =
    makeSwXform7TC("x86swt0", "int", 0);
static const std::vector<RoundTripTC> kA64G0 =
    makeSwXform7TC("a64swt0", "long", 0);
static const std::vector<RoundTripTC> kARMG0 =
    makeSwXform7TC("armswt0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform7, X64SwXformRT, ::testing::ValuesIn(kX64G),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7, X86SwXformRT, ::testing::ValuesIn(kX86G),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7, A64SwXformRT, ::testing::ValuesIn(kA64G),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7, ARM32SwXformRT, ::testing::ValuesIn(kARMG),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, X64SwXformRT, ::testing::ValuesIn(kX64G0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, X86SwXformRT, ::testing::ValuesIn(kX86G0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, A64SwXformRT, ::testing::ValuesIn(kA64G0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, ARM32SwXformRT,
                         ::testing::ValuesIn(kARMG0), rtTCName);
