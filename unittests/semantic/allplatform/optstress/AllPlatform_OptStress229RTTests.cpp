//===- AllPlatform_OptStress229RTTests.cpp - local buffers + pointers ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Breadth probes for stack-local memory: a function allocates a local array /
// struct and accesses it through indices and pointers at several widths.  This
// stresses frame-relative addressing (#158), sub-word load/store, and pointer
// arithmetic together rather than register-only dataflow.
//
//   * arrsum   - fill a local u32 array then sum it (i32 store/load).
//   * bytebuf  - fill a local u8 buffer, read back widened, checksum (i8).
//   * revbuf   - fill then reverse a local array in place (two-pointer swap).
//   * structmix- local struct of {u8,u16,u32} written/read at field offsets.
//   * idxgather- gather from a local table with computed indices.
//   * histbin  - small histogram: load-modify-store into a local count array.
//
// Buffers are filled with explicit loops (no array initializers) so no
// memset/memcpy libcall is emitted under -nostdlib.  Integer in / integer out,
// LCG-seeded, folded to one integer return.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress229RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress229RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress229RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress229RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress229RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress229RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress229RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress229RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress229TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fill a local u32 array then sum it (i32 store then load).
    {p+"_arrsum",
     t+" "+p+"_arrsum("+t+" a){ unsigned h=(unsigned)a; unsigned buf[32];\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u; buf[i]=h^(unsigned)i; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<32;i++){ acc=acc*131u+buf[(i*7+5)&31]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress229", 2},

    // Fill a local u8 buffer, read back widened, checksum.
    {p+"_bytebuf",
     t+" "+p+"_bytebuf("+t+" a){ unsigned h=(unsigned)a; unsigned char b[64];\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u; b[i]=(unsigned char)(h>>(i&7)); }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ acc=acc*131u+(unsigned)b[i]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress229", 2},

    // Fill then reverse a local array in place (two-pointer swap).
    {p+"_revbuf",
     t+" "+p+"_revbuf("+t+" a){ unsigned h=(unsigned)a; unsigned buf[24];\n"
     "  for(int i=0;i<24;i++){ h=h*1103515245u+12345u; buf[i]=h; }\n"
     "  int lo=0, hi=23;\n"
     "  while(lo<hi){ unsigned tmp=buf[lo]; buf[lo]=buf[hi]; buf[hi]=tmp; lo++; hi--; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<24;i++){ acc=acc*131u+buf[i]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress229", 2},

    // Local struct of {u8,u16,u32} written/read at field offsets.
    {p+"_structmix",
     "typedef struct{unsigned char c; unsigned short s; unsigned w;}"+p+"_S;\n"
     +t+" "+p+"_structmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_S st; st.c=(unsigned char)h; st.s=(unsigned short)(h>>5); st.w=h>>1;\n"
     "    if(h&1u) st.c=(unsigned char)(st.c+st.s); else st.s=(unsigned short)(st.s^st.w);\n"
     "    acc=acc*131u+(unsigned)st.c+(unsigned)st.s+st.w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress229", 2},

    // Gather from a local table with computed indices.
    {p+"_idxgather",
     t+" "+p+"_idxgather("+t+" a){ unsigned h=(unsigned)a; unsigned tab[32];\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u; tab[i]=h; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned idx=(h>>11)&31u; acc=acc*131u+tab[idx]+(unsigned)i;\n"
     "    tab[(h>>3)&31u]=acc; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress229", 2},

    // Small histogram: load-modify-store into a local count array.
    {p+"_histbin",
     t+" "+p+"_histbin("+t+" a){ unsigned h=(unsigned)a; unsigned cnt[16];\n"
     "  for(int i=0;i<16;i++) cnt[i]=0;\n"
     "  for(int i=0;i<256;i++){ h=h*1103515245u+12345u; cnt[h&15u]+=((h>>4)&7u)+1u; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<16;i++){ acc=acc*131u+cnt[i]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress229", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress229TC("x64o229", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress229TC("x86o229", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress229TC("a64o229", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress229TC("armo229", "int");

INSTANTIATE_TEST_SUITE_P(OptStress229, X64OptStress229RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress229, X86OptStress229RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress229, A64OptStress229RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress229, ARM32OptStress229RT, ::testing::ValuesIn(kARM), rtTCName);
