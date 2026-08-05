//===- AllPlatform_OptStress287RTTests.cpp - memory/struct/array probe ====//
//
// -O2 kernels that drive their state through stack memory rather than registers
// only, exercising the load/store, addressing-mode and sub-word memory-access
// paths: strided array reduction, mixed-width struct field RMW, unaligned
// multi-width loads (via __builtin_memcpy), index-computed gather, indexed
// scatter/accumulate (histogram), and an in-place byte-buffer shift.
//
//   * arrsum    - i32 array fill + strided index reduction (load/store).
//   * structrmw - {u8,u16,u32} struct per-field read-modify-write.
//   * mixwidth  - unaligned 32/16-bit loads aliasing a byte buffer (memcpy).
//   * ptrchase  - data-dependent gather via a computed index chain.
//   * histo     - indexed scatter + accumulate into 16 bins, then reduce.
//   * memshift  - in-place byte-buffer rotate (load/store memmove loop).
//
// All buffers are fixed-size stack arrays, all indices masked in range, all
// punning via unsigned char / memcpy so native and lifted agree bit-for-bit.
// No division.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress287RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress287RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress287RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress287RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress287RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress287RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress287RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress287RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress287TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // i32 array fill + strided index reduction (load/store).
    {p+"_arrsum",
     t+" "+p+"_arrsum("+t+" a){ unsigned buf[64]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u; buf[i]=h; }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ acc+=buf[i]; acc^=buf[(i*7)&63]; acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress287", 2},

    // {u8,u16,u32} struct per-field read-modify-write.
    {p+"_structrmw",
     t+" "+p+"_structrmw("+t+" a){ struct S{ unsigned char b; unsigned short w; unsigned int d; } s={0,0,0};\n"
     "  unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    s.b=(unsigned char)(s.b+(unsigned char)h); s.w=(unsigned short)(s.w^(unsigned short)(h>>3)); s.d=s.d*131u+h;\n"
     "    acc+=(unsigned)s.b+(unsigned)s.w+s.d+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress287", 2},

    // unaligned 32/16-bit loads aliasing a byte buffer (memcpy).
    {p+"_mixwidth",
     t+" "+p+"_mixwidth("+t+" a){ unsigned char buf[32]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<32;i++){ h=h*1103515245u+12345u; buf[i]=(unsigned char)(h>>5); }\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<28;i++){ unsigned w; __builtin_memcpy(&w,&buf[i],4);\n"
     "    unsigned short s; __builtin_memcpy(&s,&buf[i+1],2);\n"
     "    acc=acc*131u+w+s+buf[i]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress287", 2},

    // data-dependent gather via a computed index chain.
    {p+"_ptrchase",
     t+" "+p+"_ptrchase("+t+" a){ unsigned idx[64]; unsigned val[64]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u; idx[i]=h&63u; val[i]=h; }\n"
     "  unsigned acc=0; unsigned cur=0;\n"
     "  for(int i=0;i<128;i++){ cur=idx[cur&63]; acc=acc*131u+val[cur]+cur; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress287", 2},

    // indexed scatter + accumulate into 16 bins, then reduce.
    {p+"_histo",
     t+" "+p+"_histo("+t+" a){ unsigned bins[16]={0}; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned k=(h>>7)&15u; bins[k]+=(h&0xFFu)+1u; }\n"
     "  unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+bins[i];\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress287", 2},

    // in-place byte-buffer rotate (load/store memmove loop).
    {p+"_memshift",
     t+" "+p+"_memshift("+t+" a){ unsigned char buf[40]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u; buf[i]=(unsigned char)(h>>9); }\n"
     "  unsigned acc=0;\n"
     "  for(int r=0;r<20;r++){ unsigned char first=buf[0];\n"
     "    for(int i=0;i<39;i++) buf[i]=buf[i+1];\n"
     "    buf[39]=(unsigned char)(first^(unsigned char)r);\n"
     "    acc=acc*131u+buf[(r*3)&39]+(unsigned)r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress287", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress287TC("x64o287", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress287TC("x86o287", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress287TC("a64o287", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress287TC("armo287", "int");

INSTANTIATE_TEST_SUITE_P(OptStress287, X64OptStress287RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress287, X86OptStress287RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress287, A64OptStress287RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress287, ARM32OptStress287RT, ::testing::ValuesIn(kARM), rtTCName);
