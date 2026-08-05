//===- AllPlatform_OptStress295RTTests.cpp - bitfield/extract probe =======//
//
// -O2 integer kernels stressing bitfield extract/insert, deposit/scatter and
// sub-word pack/unpack codegen paths:
//
//   * bfextract - repeated bitfield extract at varying offsets.
//   * bfinsert  - bitfield insert into accumulator word.
//   * deposit   - deposit contiguous bits at computed offset.
//   * scatter8  - scatter 8-bit values into a 32-bit word by lane index.
//   * gather8   - gather 8-bit lanes from a packed word.
//   * signext   - sign-extend from 8/16-bit subfields.
//
// All bit widths and offsets are compile-time or masked in range, and there is
// no division -- so native and lifted builds agree bit-for-bit.  Four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress295RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress295RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress295RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress295RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress295RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress295RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress295RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress295RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress295TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // repeated bitfield extract at varying offsets.
    {p+"_bfextract",
     t+" "+p+"_bfextract("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned off=(h&15u)*2u; unsigned w=(h>>off)&0xFFu;\n"
     "    acc=acc*131u+w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress295", 2},

    // bitfield insert into accumulator word.
    {p+"_bfinsert",
     t+" "+p+"_bfinsert("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned off=(h&7u)*4u; unsigned val=h&0xFu;\n"
     "    acc=(acc&~(0xFu<<off))|(val<<off); acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress295", 2},

    // deposit contiguous bits at computed offset.
    {p+"_deposit",
     t+" "+p+"_deposit("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned off=h&31u; unsigned bits=(h>>8)&0xFFu;\n"
     "    acc=(acc&~(0xFFu<<off))|(bits<<off); acc^=h; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress295", 2},

    // scatter 8-bit values into a 32-bit word by lane index.
    {p+"_scatter8",
     t+" "+p+"_scatter8("+t+" a){ unsigned h=(unsigned)a; unsigned word=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned lane=i&3u; unsigned val=(h>>5)&0xFFu;\n"
     "    word=(word&~(0xFFu<<(lane*8u)))|(val<<(lane*8u)); }\n"
     "  return ("+t+")word; }\n",
     {0x45678u}, "OptStress295", 2},

    // gather 8-bit lanes from a packed word.
    {p+"_gather8",
     t+" "+p+"_gather8("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned lane=(h&3u); unsigned b=(h>>(lane*8u))&0xFFu;\n"
     "    acc=acc*131u+b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress295", 2},

    // sign-extend from 8/16-bit subfields.
    {p+"_signext",
     t+" "+p+"_signext("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    signed char c=(signed char)(h&0xFFu);\n"
     "    short s=(short)(h&0xFFFFu);\n"
     "    acc=acc*131u+(unsigned)(int)c+(unsigned)(int)s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress295", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress295TC("x64o295", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress295TC("x86o295", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress295TC("a64o295", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress295TC("armo295", "int");

INSTANTIATE_TEST_SUITE_P(OptStress295, X64OptStress295RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress295, X86OptStress295RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress295, A64OptStress295RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress295, ARM32OptStress295RT, ::testing::ValuesIn(kARM), rtTCName);
