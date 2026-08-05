//===- AllPlatform_OptStress292RTTests.cpp - lookup/table probe ===========//
//
// -O2 integer kernels stressing table lookup, index computation and
// data-dependent gather/scatter codegen paths:
//
//   * lutchain  - chained LUT lookups via computed index.
//   * permute   - in-place array permutation by index table.
//   * rankcnt   - per-value rank/count in a small alphabet (16 bins).
//   * stridemap - strided index map with XOR fold.
//   * runlen    - run-length encode then decode sum.
//   * crcfold   - table-driven CRC-like fold (no HW CRC insn).
//
// All indices are masked in range, all tables are fixed-size stack arrays,
// and there is no division -- so native and lifted builds agree bit-for-bit.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress292RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress292RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress292RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress292RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress292RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress292RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress292RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress292RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress292TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // chained LUT lookups via computed index.
    {p+"_lutchain",
     t+" "+p+"_lutchain("+t+" a){ unsigned tbl[32]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<32;i++) tbl[i]=(unsigned)(a*(i+1)*131u);\n"
     "  unsigned acc=0; unsigned idx=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; idx=(idx+h)&31u; acc=acc*131u+tbl[idx]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress292", 2},

    // in-place array permutation by index table.
    {p+"_permute",
     t+" "+p+"_permute("+t+" a){ unsigned buf[32], idx[32]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<32;i++){ buf[i]=(unsigned)(a*(i+1)); idx[i]=(unsigned)((a*(i+3))&31u); }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<16;r++){ unsigned t=buf[r&31]; buf[r&31]=buf[idx[r&31]&31u]; buf[idx[r&31]&31u]=t;\n"
     "    acc=acc*131u+buf[r&31]+(unsigned)r; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress292", 2},

    // per-value rank/count in a small alphabet (16 bins).
    {p+"_rankcnt",
     t+" "+p+"_rankcnt("+t+" a){ unsigned cnt[16]={0}; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; cnt[(h>>4)&15u]++; }\n"
     "  unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+cnt[i]*((unsigned)i+1u);\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress292", 2},

    // strided index map with XOR fold.
    {p+"_stridemap",
     t+" "+p+"_stridemap("+t+" a){ unsigned src[64]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<64;i++) src[i]=(unsigned)(a*(i+1));\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned k=(i*7+h)&63u; acc=acc*131u+(src[k]^src[(k+13)&63u])+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress292", 2},

    // run-length encode then decode sum.
    {p+"_runlen",
     t+" "+p+"_runlen("+t+" a){ unsigned char seq[80]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u; seq[i]=(unsigned char)((h>>5)&7u); }\n"
     "  unsigned acc=0; unsigned run=1u;\n"
     "  for(int i=1;i<80;i++){ if(seq[i]==seq[i-1]) run++; else { acc=acc*131u+run*(unsigned)seq[i-1]; run=1u; } }\n"
     "  acc=acc*131u+run*(unsigned)seq[79];\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress292", 2},

    // table-driven CRC-like fold (no HW CRC insn).
    {p+"_crcfold",
     t+" "+p+"_crcfold("+t+" a){ unsigned tbl[256]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<256;i++) tbl[i]=(unsigned)(i*0x1DB710641u);\n"
     "  unsigned crc=0xFFFFFFFFu;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b=h&0xFFu; crc=(crc>>8)^tbl[(crc^b)&0xFFu]; }\n"
     "  return ("+t+")crc; }\n",
     {0x6789Au}, "OptStress292", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress292TC("x64o292", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress292TC("x86o292", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress292TC("a64o292", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress292TC("armo292", "int");

INSTANTIATE_TEST_SUITE_P(OptStress292, X64OptStress292RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress292, X86OptStress292RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress292, A64OptStress292RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress292, ARM32OptStress292RT, ::testing::ValuesIn(kARM), rtTCName);
