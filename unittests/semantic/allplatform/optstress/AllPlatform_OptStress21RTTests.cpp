//===- AllPlatform_OptStress21RTTests.cpp - stack-mem optimizer -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress 1-20 hammer the register/flag MedIR passes; this round turns the
// same fold-to-0 / dropped-write scrutiny onto stack-resident memory.  Every
// kernel keeps a small array or struct on the stack and indexes it with a
// data-dependent subscript, which defeats SROA/scalarization and forces real
// loads and stores through NeverD's recovered frame.  The recovery must map
// every computed access back to the same logical slot and preserve store-to-load
// forwarding, sub-byte/half stores aliasing wider loads, and read-modify-write
// ordering exactly:
//
//   * idxgather  - gather two computed slots, scatter to a third, sub-byte read
//                  of a freshly written element (load/store forwarding at a
//                  computed offset + signed-char narrowing of stack memory).
//   * byteweave  - assemble a 32-bit word from four adjacent bytes of a stack
//                  buffer, then store a byte back (sub-register store aliasing a
//                  wider load at a runtime index).
//   * histrmw    - histogram read-modify-write at a scrambled index (the RMW
//                  ordering DCE/propagation must not reorder or drop).
//   * revstride  - reverse walk with index wrap and a sub-half read of each
//                  rewritten element (negative-direction store/load chain).
//   * structfield- local struct array, per-field mixed-width access at a runtime
//                  index (field-offset arithmetic into a stack object).
//   * swapperm   - in-place swap permutation: two loads + two stores to computed
//                  slots per step (aliasing when the two indices coincide).
//
// All integer, fold to one integer return, no float / 64-bit-divide helper and
// no memset-able zero-init (arrays seed from the LCG) so the original stays a
// self-contained native function.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress21RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress21RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress21RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress21RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress21RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress21RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress21RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress21RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress21TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Gather two computed slots, scatter to a third, sub-byte read of a slot.
    {p+"_idxgather",
     t+" "+p+"_idxgather("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int g[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; g[i]=(int)(s>>4)-(int)(s<<1); }\n"
     "  int h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)&15u, k=(s>>12)&15u;\n"
     "    int v=g[j]+g[k];\n"
     "    g[(j+k)&15u]=v;\n"
     "    h=h*31+v-(int)(signed char)g[j]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress21", 2},

    // Assemble a 32-bit word from four adjacent stack bytes, store one back.
    {p+"_byteweave",
     t+" "+p+"_byteweave("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[32];\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; b[i]=(unsigned char)(s>>9); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&28u;\n"
     "    unsigned w=(unsigned)b[j]|((unsigned)b[j+1]<<8)|((unsigned)b[j+2]<<16)|((unsigned)b[j+3]<<24);\n"
     "    b[(s>>3)&31u]=(unsigned char)(w>>((j&3u)*8u));\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")h; }\n",
     {0x9bULL}, "OptStress21", 2},

    // Histogram read-modify-write at a scrambled index.
    {p+"_histrmw",
     t+" "+p+"_histrmw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int c[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; c[i]=(int)(s>>13); }\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>7)&15u; c[j]+=(int)(s>>10)+1; c[j]^=(int)(s<<2); }\n"
     "  int h=0; for(int i=0;i<16;i++) h=h*31+c[i];\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress21", 2},

    // Reverse walk with index wrap, sub-half read of each rewritten element.
    {p+"_revstride",
     t+" "+p+"_revstride("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int v[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; v[i]=(int)s; }\n"
     "  int h=0;\n"
     "  for(int i=15;i>=0;i--){ int prev=v[(i+1)&15]; v[i]=v[i]-prev+(i*7);\n"
     "    h=h*31+v[i]-(int)(short)v[i]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x55ULL}, "OptStress21", 2},

    // Local struct array, per-field mixed-width access at a runtime index.
    {p+"_structfield",
     t+" "+p+"_structfield("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  struct S { int x; short y; unsigned char z; signed char w; } arr[8];\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u;\n"
     "    arr[i].x=(int)s; arr[i].y=(short)(s>>3);\n"
     "    arr[i].z=(unsigned char)(s>>11); arr[i].w=(signed char)(s>>19); }\n"
     "  int h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; unsigned j=(s>>6)&7u, k=(s>>10)&7u;\n"
     "    arr[j].x += (int)arr[k].w; arr[j].y ^= (short)arr[k].z;\n"
     "    h=h*31+arr[j].x+(int)arr[j].y-(int)arr[j].z+(int)arr[j].w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress21", 2},

    // In-place swap permutation: two loads + two stores to computed slots.
    {p+"_swapperm",
     t+" "+p+"_swapperm("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int v[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; v[i]=(int)(s>>4); }\n"
     "  int h=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u, k=(s>>11)&15u;\n"
     "    int tmp=v[j]; v[j]=v[k]+i; v[k]=tmp-(int)(unsigned char)tmp;\n"
     "    h+=v[j]^v[k]; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress21", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress21TC("x64o21", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress21TC("x86o21", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress21TC("a64o21", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress21TC("armo21", "int");

INSTANTIATE_TEST_SUITE_P(OptStress21, X64OptStress21RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress21, X86OptStress21RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress21, A64OptStress21RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress21, ARM32OptStress21RT, ::testing::ValuesIn(kARM), rtTCName);
