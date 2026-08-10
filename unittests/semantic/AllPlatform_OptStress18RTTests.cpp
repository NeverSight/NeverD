//===- AllPlatform_OptStress18RTTests.cpp - memory-layout probes -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Memory-layout / address-computation roundtrip probes orthogonal to the
// arithmetic-heavy OptStress1-17 rounds.  These stress NeverD's memory SSA,
// stack-slot reconstruction and the lift of clang's address-generation code
// (misaligned access lowering, 2-D index arithmetic, struct field offsets):
//
//   * packed_rw  - a __attribute__((packed)) struct whose int/short/char fields
//                  land on misaligned offsets (clang emits unaligned word
//                  load/store sequences the lifter must reproduce faithfully).
//   * mixed2d    - a 2-D byte matrix walked by row and by column (i*W+j index
//                  arithmetic feeding a memory address).
//   * soa_walk   - struct-of-arrays (separate x/y/z arrays) accessed in lockstep
//                  (three independent induction pointers in one loop body).
//   * node_walk  - an array-backed linked list (each node holds a next index);
//                  pointer chasing through a runtime-dependent index chain.
//   * slidingw   - read a 32-bit word from every byte offset of a buffer
//                  (deliberately misaligned overlapping loads).
//   * gatherstr  - gather bytes from a buffer through a runtime stride/permutation.
//
// Every kernel uses a small (<=64-byte) stack buffer, is integer-only, folds to
// a single integer return and lowers to no runtime helper, so all four targets
// are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress18RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress18RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress18RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress18RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress18RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress18RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress18RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress18RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress18TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Packed struct: misaligned int/short/char fields (unaligned access lowering).
    {p+"_packed_rw",
     "struct __attribute__((packed)) Pk{ unsigned char c; unsigned w; "
     "unsigned short h; int s; };\n"
     +t+" "+p+"_packed_rw("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned hsh=0; struct Pk pk;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u;\n"
     "    pk.c=(unsigned char)x; pk.w=x^0x9e3779b9u;\n"
     "    pk.h=(unsigned short)(x>>11); pk.s=(int)x>>3;\n"
     "    hsh=hsh*131u+pk.c+pk.w+pk.h+(unsigned)pk.s; }\n"
     "  return ("+t+")(unsigned)hsh; }\n",
     {0x4cULL}, "OptStress18", 2},

    // 2-D byte matrix walked by row then by column (i*W+j addressing).
    {p+"_mixed2d",
     t+" "+p+"_mixed2d("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned char m[6][6]; unsigned h=0;\n"
     "  for(int i=0;i<6;i++) for(int j=0;j<6;j++){ x=x*1103515245u+12345u;\n"
     "    m[i][j]=(unsigned char)(x>>13); }\n"
     "  for(int i=0;i<6;i++){ unsigned rs=0; for(int j=0;j<6;j++) rs+=m[i][j];\n"
     "    h=h*131u+rs; }\n"
     "  for(int j=0;j<6;j++){ unsigned cs=0; for(int i=0;i<6;i++) cs+=m[i][j]*(unsigned)(i+1);\n"
     "    h=h*131u+cs; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress18", 2},

    // Struct-of-arrays accessed in lockstep (three induction pointers).
    {p+"_soa_walk",
     t+" "+p+"_soa_walk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int xs[12],ys[12],zs[12]; unsigned h=0;\n"
     "  for(int i=0;i<12;i++){ s=s*1103515245u+12345u; xs[i]=(int)s>>20;\n"
     "    s=s*1103515245u+12345u; ys[i]=(int)s>>20;\n"
     "    s=s*1103515245u+12345u; zs[i]=(int)s>>20; }\n"
     "  long acc=0;\n"
     "  for(int i=0;i<12;i++){ acc+=(long)xs[i]*ys[i]-(long)zs[i]*xs[i]+ys[i]; }\n"
     "  for(int i=0;i<11;i++){ int dx=xs[i+1]-xs[i],dy=ys[i+1]-ys[i];\n"
     "    h=h*131u+(unsigned)(dx*dx+dy*dy); }\n"
     "  return ("+t+")(unsigned)(h+(unsigned)acc); }\n",
     {0xa7ULL}, "OptStress18", 2},

    // Array-backed linked list: chase a runtime next-index chain.
    {p+"_node_walk",
     t+" "+p+"_node_walk("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int nxt[16]; int val[16]; unsigned h=0;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; val[i]=(int)(s>>8);\n"
     "    nxt[i]=(int)((s>>20)&15); }\n"
     "  int cur=(int)(s&15); int steps=0; unsigned acc=0;\n"
     "  while(steps<40){ acc=acc*131u+(unsigned)val[cur]; cur=nxt[cur];\n"
     "    if(cur==0 && steps>4) break; steps++; }\n"
     "  h=acc*7u+(unsigned)steps;\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress18", 2},

    // Read a 32-bit word from every byte offset (overlapping misaligned loads).
    {p+"_slidingw",
     t+" "+p+"_slidingw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char buf[24]; unsigned h=0;\n"
     "  for(int i=0;i<24;i++){ s=s*1103515245u+12345u; buf[i]=(unsigned char)(s>>16); }\n"
     "  for(int i=0;i<=24-4;i++){\n"
     "    unsigned w=(unsigned)buf[i]|((unsigned)buf[i+1]<<8)\n"
     "      |((unsigned)buf[i+2]<<16)|((unsigned)buf[i+3]<<24);\n"
     "    w=(w<<7)|(w>>25); h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress18", 2},

    // Gather bytes through a runtime stride / permutation index.
    {p+"_gatherstr",
     t+" "+p+"_gatherstr("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char buf[32]; unsigned h=0;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; buf[i]=(unsigned char)(s>>9); }\n"
     "  unsigned stride=((s>>3)&7u)|1u; int idx=0;\n"
     "  for(int k=0;k<32;k++){ idx=(idx+(int)stride)&31; unsigned b=buf[idx];\n"
     "    h=h*131u+b; buf[(idx*3+1)&31]=(unsigned char)(b^k); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress18", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress18TC("x64o18", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress18TC("x86o18", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress18TC("a64o18", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress18TC("armo18", "int");

INSTANTIATE_TEST_SUITE_P(OptStress18, X64OptStress18RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress18, X86OptStress18RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress18, A64OptStress18RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress18, ARM32OptStress18RT, ::testing::ValuesIn(kARM), rtTCName);
