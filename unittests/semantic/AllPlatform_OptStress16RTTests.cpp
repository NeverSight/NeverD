//===- AllPlatform_OptStress16RTTests.cpp - mem-aliasing probes -*-C++*-=//
//
// Memory-based sub-register / width-aliasing roundtrip probes.  Earlier rounds
// stressed register sub-register aliasing; these route the width mismatch
// *through memory* (store-to-load forwarding at a different width), which
// exercises NeverD's LowToMed memory model and load/store forwarding rather
// than the register SSA path:
//
//   * memsubword - store a 32-bit word to a stack slot, reload its bytes/halves
//                  (memory narrowing of a wide store).
//   * splithl    - store a 64-bit value, reload the two 32-bit halves (lane
//                  split through memory).
//   * packbytes  - assemble a word from four byte stores then reload it whole
//                  (memory widening of narrow stores).
//   * slotreuse  - one stack slot reused at 8/16/32-bit widths across a loop
//                  (overlapping defs at the same address).
//   * ptralias   - write via a wide pointer, read via a narrow pointer to the
//                  same buffer (pointer-cast aliasing).
//   * bswapmem   - byte-reverse a word by shuffling its bytes through memory.
//
// Each kernel uses a tiny (<=64-byte) stack buffer, is integer-only, folds to a
// single integer return and lowers to no runtime helper, so all four targets
// are checked native vs lifted at -O1 (keeps the stores from being fully
// scalarized away while staying inside the Unicorn stack mapping).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress16RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress16RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress16RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress16RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress16RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress16RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress16RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress16RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress16TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Store a 32-bit word, reload its bytes/halves (memory narrowing).
    {p+"_memsubword",
     t+" "+p+"_memsubword("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0; volatile unsigned buf;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u; buf=x;\n"
     "    unsigned char *b=(unsigned char*)&buf; unsigned short *s=(unsigned short*)&buf;\n"
     "    h=h*131u+b[0]+b[1]*3u+b[2]*5u+b[3]*7u+s[0]+s[1]*2u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress16", 1},

    // Store a 64-bit value, reload the two 32-bit halves (lane split via memory).
    {p+"_splithl",
     t+" "+p+"_splithl("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0; volatile unsigned long long buf;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    buf=((unsigned long long)x<<32)|(x^0x9e3779b9u);\n"
     "    unsigned *w=(unsigned*)&buf; h=h*131u+w[0]+w[1]*3u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress16", 1},

    // Assemble a word from four byte stores then reload it whole (widening).
    {p+"_packbytes",
     t+" "+p+"_packbytes("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0; volatile unsigned char buf[4];\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    buf[0]=(unsigned char)x; buf[1]=(unsigned char)(x>>8);\n"
     "    buf[2]=(unsigned char)(x>>16); buf[3]=(unsigned char)(x>>24);\n"
     "    unsigned w=buf[0]|(buf[1]<<8)|(buf[2]<<16)|((unsigned)buf[3]<<24);\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress16", 1},

    // One stack slot reused at 8/16/32-bit widths across the loop.
    {p+"_slotreuse",
     t+" "+p+"_slotreuse("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0; volatile unsigned slot;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    slot=x; unsigned a32=slot;\n"
     "    *(volatile unsigned short*)&slot=(unsigned short)(x>>5); unsigned a16=slot;\n"
     "    *(volatile unsigned char*)&slot=(unsigned char)(x>>11); unsigned a8=slot;\n"
     "    h=h*131u+a32+a16*3u+a8*5u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress16", 1},

    // Write via a wide pointer, read via a narrow pointer to the same buffer.
    {p+"_ptralias",
     t+" "+p+"_ptralias("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0; volatile unsigned buf[2];\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    buf[0]=x; buf[1]=x*2654435761u;\n"
     "    volatile unsigned char *bp=(volatile unsigned char*)buf;\n"
     "    unsigned acc=0; for(int k=0;k<8;k++) acc=acc*31u+bp[k];\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress16", 1},

    // Byte-reverse a word by shuffling its bytes through memory.
    {p+"_bswapmem",
     t+" "+p+"_bswapmem("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0; volatile unsigned char in[4], out[4];\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    in[0]=(unsigned char)x; in[1]=(unsigned char)(x>>8);\n"
     "    in[2]=(unsigned char)(x>>16); in[3]=(unsigned char)(x>>24);\n"
     "    out[0]=in[3]; out[1]=in[2]; out[2]=in[1]; out[3]=in[0];\n"
     "    unsigned w=out[0]|(out[1]<<8)|(out[2]<<16)|((unsigned)out[3]<<24);\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress16", 1},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress16TC("x64o16", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress16TC("x86o16", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress16TC("a64o16", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress16TC("armo16", "int");

INSTANTIATE_TEST_SUITE_P(OptStress16, X64OptStress16RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress16, X86OptStress16RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress16, A64OptStress16RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress16, ARM32OptStress16RT, ::testing::ValuesIn(kARM), rtTCName);
