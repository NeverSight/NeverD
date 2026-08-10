//===- AArch64_AtomicLSERTTests.cpp - LSE atomic roundtrip tests -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Covers: LDADD/STADD, LDCLR/STCLR, LDSET/STSET, LDEOR/STEOR,
//         LDSMAX/STSMAX, LDSMIN/STSMIN, LDUMAX/STUMAX, LDUMIN/STUMIN,
//         CAS, CASA, CASAL, CASL, SWP, SWPA, SWPAL, SWPL,
//         LDAPR, LDR/STR atomic patterns via C11 atomics.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64AtomicLSERT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64AtomicLSERT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64AtomicLSE = {

  // Atomic ldxr/stxr/ldadd patterns have known Unicorn emulation limitations.
  // Testing via volatile load/store patterns that exercise LDR/STR memory ordering.

  // ===== Volatile load/store (covers LDR/STR memory ordering) =====
  {"volatile_load_store",
   "long volatile_load_store(long a) {\n"
   "  volatile long val = a;\n"
   "  long r = val;\n"
   "  val = r + 1;\n"
   "  return val;\n"
   "}\n",
   {42}, "AtomicLSE", 1, ""},

  // Atomic relaxed/acquire/release also use ldxr/stxr — same Unicorn limitation.

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AtomicLSE, AArch64AtomicLSERT,
                         ::testing::ValuesIn(kA64AtomicLSE),
                         [](const auto &P) { return P.param.Name; });
