//===- AllPlatform_SwitchModuloRTTests.cpp - modulo-indexed switch ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probing of `switch(x % N)` for a non-power-of-two N — clang computes
// `x % N` with a magic-reciprocal multiply and dispatches the dense 0..N-1 cases
// through a jump table with no `cmp` range guard (the remainder already bounds
// the index).  Recovering the entry count therefore needs the modulus, obtained
// from either the entry relocations or the remainder arithmetic:
//
//   * x86-64 / i386 and small-N AArch64 lower to a 4-byte PC-relative `.rodata`
//     word table whose entries carry code relocations; the relocation run bounds
//     it (#403).
//   * AArch64 byte/halfword compact tables (larger case counts) and ARM32 inline
//     `.text` word tables carry no entry relocations; their entry count is read
//     from the `quotient * N` back-multiply of the remainder (#404).
//
// Each kernel returns a value-dependent hash compiled at -O2; the roundtrip
// compares native vs lifted execution.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SwModRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SwModRT, Verify) { roundTripX64(GetParam()); }
class X86SwModRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SwModRT, Verify) { roundTripX86(GetParam()); }
class A64SwModRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SwModRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SwModRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SwModRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// Build a `switch(acc % N)` loop with N distinct case bodies so the dispatch
// table really has N entries (every case 0..N-1 is reachable).
static RoundTripTC mkSwModTC(const std::string &p, const char *T, unsigned N) {
  static const char *kOps[] = {
    "acc+=0x9E3779B9u;",        "acc^=acc<<13;",
    "acc*=2654435761u;",        "acc-=acc>>5;",
    "acc=(acc<<7)|(acc>>25);",  "acc+=0x85EBCA6Bu;",
    "acc^=acc>>17;",            "acc*=0xC2B2AE35u;",
    "acc-=acc<<3;",             "acc+=0x27D4EB2Fu;",
    "acc^=acc>>11;",            "acc=(acc<<5)|(acc>>27);",
    "acc+=0x165667B1u;",        "acc^=acc<<7;",
    "acc*=0x9E3779B1u;",        "acc-=acc>>13;",
    "acc+=0xCC9E2D51u;",        "acc^=acc>>15;",
    "acc*=0x1B873593u;",        "acc-=acc<<11;",
  };
  std::string t = T, ns = std::to_string(N);
  std::string body =
    t+" "+p+"_m"+ns+"("+t+" a){\n"
    "  unsigned acc=(unsigned)a|1u;\n"
    "  for(int i=0;i<100;i++){\n"
    "    switch(acc % "+ns+"u){\n";
  for (unsigned k = 0; k < N && k < 20; ++k)
    body += "    case " + std::to_string(k) + ": " + kOps[k] + " break;\n";
  body +=
    "    }\n"
    "    acc+=(unsigned)i*131u;\n"
    "  }\n"
    "  return ("+t+")(unsigned long)acc; }\n";
  return RoundTripTC{p+"_m"+ns, body, {0x1234ULL}, "SwMod", 2};
}
// clang-format on

// x86-64 / i386 lower every non-power-of-two modulo switch to the relocation-
// bounded PC-relative word table.
static std::vector<RoundTripTC> makeWordTableTC(const char *p, const char *T) {
  return {mkSwModTC(p, T, 5), mkSwModTC(p, T, 7), mkSwModTC(p, T, 9)};
}
// AArch64 keeps a small modulo switch on the PC-relative word table but moves to
// a byte/halfword compact table once the case count grows; cover both.
static std::vector<RoundTripTC> makeA64TC(const char *p, const char *T) {
  return {mkSwModTC(p, T, 5),  mkSwModTC(p, T, 7),  mkSwModTC(p, T, 9),
          mkSwModTC(p, T, 13), mkSwModTC(p, T, 17), mkSwModTC(p, T, 19)};
}
// ARM32 dispatches a modulo switch through an inline `.text` word table whose
// entry count comes from the remainder back-multiply.
static std::vector<RoundTripTC> makeARMTC(const char *p, const char *T) {
  return {mkSwModTC(p, T, 5),  mkSwModTC(p, T, 7),
          mkSwModTC(p, T, 9),  mkSwModTC(p, T, 13)};
}

static const std::vector<RoundTripTC> kX64 = makeWordTableTC("x64swm", "long");
static const std::vector<RoundTripTC> kX86 = makeWordTableTC("x86swm", "int");
static const std::vector<RoundTripTC> kA64 = makeA64TC("a64swm", "long");
static const std::vector<RoundTripTC> kARM = makeARMTC("armswm", "int");

INSTANTIATE_TEST_SUITE_P(SwMod, X64SwModRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwMod, X86SwModRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwMod, A64SwModRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwMod, ARM32SwModRT, ::testing::ValuesIn(kARM), rtTCName);
