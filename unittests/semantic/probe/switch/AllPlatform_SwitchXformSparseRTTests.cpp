//===- AllPlatform_SwitchXformSparseRTTests.cpp - sparse switch -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Sparse / signed / non-contiguous half of the switch transformation probing
// suite (see AllPlatform_SwitchXformRTTests.cpp for the shared rationale and
// the roundtrip protocol).  Every shape here defeats the plain base-0 dense
// table the sibling TUs' shapes produce, so it exercises the biased-index and
// comparison-tree lowerings instead:
//   * signed case labels with a negative minimum (subtract-min bias off a
//     negative base, not a low-bits mask);
//   * sparse labels below the jump-table density threshold, which clang lowers
//     to a binary-search comparison tree (the HighIfChainToSwitch recovery
//     path, distinct from JumpTableResolver);
//   * two far-apart dense clusters — two biased jump tables in one function;
//   * non-contiguous labels leaving default-filled holes in a dense table
//     (the kept-slot / EntryIndices sparse-label path);
//   * a loop-carried PHI selector (the index is a back-edge PHI, not a value
//     freshly masked in the dispatch block);
//   * a signed sparse span crossing zero (a tree of signed compares).
//
//===----------------------------------------------------------------------===//

#include "SwitchXformRTFixture.h"

// clang-format off
// Third batch: shapes that force a NON-dense, non-base-0 lowering — the earlier
// batches all dispatch on the low bits of a value (a base-0 dense table).  Here
// the switch value is signed with a negative minimum (subtract-min bias with a
// negative base), sparse (clang builds a binary-search comparison tree, not a
// jump table — the HighIfChainToSwitch recovery path, distinct from
// JumpTableResolver), or split into two far-apart dense clusters (two biased
// jump tables in one function).  Each index is kept inside the case set so the
// original's default (present, never taken) UB path never diverges the runs.
static std::vector<RoundTripTC> makeSwXform3TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Signed switch with negative case labels, k in [-4,3].  clang biases the
    // dense range by +4 (an add, not the plain low-bits mask of the earlier
    // shapes) before indexing, so recovery must trace a signed subtract-min with
    // a NEGATIVE minimum — a case the index/bound logic never sees at base 0.
    {p+"_signeg",
     t+" "+p+"_signeg("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    int k=(int)(acc & 7u) - 4;\n"
     "    switch(k){\n"
     "    case -4: acc+=0x9E3779B9u; break; case -3: acc^=acc<<13; break;\n"
     "    case -2: acc*=2654435761u; break; case -1: acc-=acc>>5; break;\n"
     "    case 0:  acc+=0x85EBCA6Bu; break; case 1:  acc^=acc>>17; break;\n"
     "    case 2:  acc=(acc<<7)|(acc>>25); break; case 3: acc+=0xA5A5u; break;\n"
     "    default: acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1357ULL}, "SwXform3", Opt},

    // Sparse switch: 8 labels spread across [3,35003] (density ~ 1/4375).  Below
    // the jump-table density threshold, clang lowers this to a binary-search
    // comparison tree of INT_EQUAL / range compares — exercising if-chain-to-
    // switch recovery and plain compare lifting rather than an INDIR_BR table.
    {p+"_sparse",
     t+" "+p+"_sparse("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=(acc & 7u)*5000u + 3u;\n"
     "    switch(k){\n"
     "    case 3:     acc+=0x9E3779B9u; break; case 5003:  acc^=acc<<13; break;\n"
     "    case 10003: acc*=2654435761u; break; case 15003: acc-=acc>>5; break;\n"
     "    case 20003: acc+=0x85EBCA6Bu; break; case 25003: acc^=acc>>17; break;\n"
     "    case 30003: acc=(acc<<7)|(acc>>25); break;\n"
     "    case 35003: acc+=0xA5A5u; break;\n"
     "    default:    acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x2468ULL}, "SwXform3", Opt},

    // Two dense clusters far apart: {0,1,2,3} and {1004,1005,1006,1007}.  clang
    // splits on the cluster then emits a jump table per cluster, the second
    // biased by 1004 — so the function carries two INDIR_BR tables with
    // distinct non-zero bases, each keyed by its own dispatch address.
    {p+"_cluster",
     t+" "+p+"_cluster("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned sel=acc & 7u;\n"
     "    unsigned k=(sel<4u)?sel:(1000u+sel);\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E37u; break;    case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;         case 3: acc-=acc>>3; break;\n"
     "    case 1004: acc+=0x1111u; break; case 1005: acc^=0x2222u; break;\n"
     "    case 1006: acc+=acc<<2; break;  case 1007: acc^=0xA5u; break;\n"
     "    default: acc^=0xBEEFu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ABCULL}, "SwXform3", Opt},
  };
}

// Sixth batch: dispatch shapes whose index is not a plain mask of the running
// value — non-contiguous case labels (a dense table with default-filled holes,
// exercising the kept-slot / EntryIndices sparse-label path), a loop-carried
// PHI selector (the index is a back-edge PHI, not a fresh mask), and a signed
// sparse switch spanning negatives (a binary-search tree of signed compares).
static std::vector<RoundTripTC> makeSwXform6TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Non-contiguous case labels {0,3,6,9,12}: clang builds a dense table over
    // [0,12] with the 8 holes routed to default.  The index always hits a real
    // case, so default is never taken, but the recovered table must fill the
    // holes with the default edge rather than mis-labelling or truncating.
    {p+"_gaps",
     t+" "+p+"_gaps("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=(acc % 5u)*3u;\n"
     "    switch(k){\n"
     "    case 0:  acc+=0x9E3779B9u; break; case 3:  acc^=acc<<13; break;\n"
     "    case 6:  acc*=2654435761u; break; case 9:  acc-=acc>>5; break;\n"
     "    case 12: acc+=0x85EBCA6Bu; break; default: acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x4242ULL}, "SwXform6", Opt},

    // Loop-carried PHI selector: `sel` is dispatched on and then updated for the
    // next iteration, so the switch index is a back-edge PHI value (not a fresh
    // mask of acc) — the index recovery must anchor on the PHI, not assume the
    // masked value is freshly computed in the dispatch block.
    {p+"_loopcarry",
     t+" "+p+"_loopcarry("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u; unsigned sel=(acc&7u)+1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(sel & 7u){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=acc>>3; break;\n"
     "    case 4: acc+=0x1111u; break; case 5: acc^=0x2222u; break;\n"
     "    case 6: acc+=acc<<2; break;  default: acc^=0xA5u; break;\n"
     "    }\n"
     "    sel=sel*5u+acc;\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1515ULL}, "SwXform6", Opt},

    // Signed sparse switch spanning negatives {-100,-50,0,50,100}: below the
    // jump-table density threshold and signed, so clang builds a binary-search
    // tree of SIGNED comparisons — exercising signed compare lifting and the
    // if-chain-to-switch recovery over a negative-through-positive span.
    {p+"_negrange",
     t+" "+p+"_negrange("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    int k=(int)(acc % 5u)*50 - 100;\n"
     "    switch(k){\n"
     "    case -100: acc+=0x9E3779B9u; break; case -50: acc^=acc<<13; break;\n"
     "    case 0:    acc*=2654435761u; break; case 50:  acc-=acc>>5; break;\n"
     "    case 100:  acc+=0x85EBCA6Bu; break; default:  acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9090ULL}, "SwXform6", Opt},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64C =
    makeSwXform3TC("x64swz", "long", 2);
static const std::vector<RoundTripTC> kX86C =
    makeSwXform3TC("x86swz", "int", 2);
static const std::vector<RoundTripTC> kA64C =
    makeSwXform3TC("a64swz", "long", 2);
static const std::vector<RoundTripTC> kARMC =
    makeSwXform3TC("armswz", "int", 2);
static const std::vector<RoundTripTC> kX64C0 =
    makeSwXform3TC("x64swz0", "long", 0);
static const std::vector<RoundTripTC> kX86C0 =
    makeSwXform3TC("x86swz0", "int", 0);
static const std::vector<RoundTripTC> kA64C0 =
    makeSwXform3TC("a64swz0", "long", 0);
static const std::vector<RoundTripTC> kARMC0 =
    makeSwXform3TC("armswz0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform3, X64SwXformRT, ::testing::ValuesIn(kX64C),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3, X86SwXformRT, ::testing::ValuesIn(kX86C),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3, A64SwXformRT, ::testing::ValuesIn(kA64C),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3, ARM32SwXformRT, ::testing::ValuesIn(kARMC),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, X64SwXformRT, ::testing::ValuesIn(kX64C0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, X86SwXformRT, ::testing::ValuesIn(kX86C0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, A64SwXformRT, ::testing::ValuesIn(kA64C0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, ARM32SwXformRT,
                         ::testing::ValuesIn(kARMC0), rtTCName);

static const std::vector<RoundTripTC> kX64F =
    makeSwXform6TC("x64swu", "long", 2);
static const std::vector<RoundTripTC> kX86F =
    makeSwXform6TC("x86swu", "int", 2);
static const std::vector<RoundTripTC> kA64F =
    makeSwXform6TC("a64swu", "long", 2);
static const std::vector<RoundTripTC> kARMF =
    makeSwXform6TC("armswu", "int", 2);
static const std::vector<RoundTripTC> kX64F0 =
    makeSwXform6TC("x64swu0", "long", 0);
static const std::vector<RoundTripTC> kX86F0 =
    makeSwXform6TC("x86swu0", "int", 0);
static const std::vector<RoundTripTC> kA64F0 =
    makeSwXform6TC("a64swu0", "long", 0);
static const std::vector<RoundTripTC> kARMF0 =
    makeSwXform6TC("armswu0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform6, X64SwXformRT, ::testing::ValuesIn(kX64F),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6, X86SwXformRT, ::testing::ValuesIn(kX86F),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6, A64SwXformRT, ::testing::ValuesIn(kA64F),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6, ARM32SwXformRT, ::testing::ValuesIn(kARMF),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, X64SwXformRT, ::testing::ValuesIn(kX64F0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, X86SwXformRT, ::testing::ValuesIn(kX86F0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, A64SwXformRT, ::testing::ValuesIn(kA64F0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, ARM32SwXformRT,
                         ::testing::ValuesIn(kARMF0), rtTCName);
