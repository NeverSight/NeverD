//===- AllPlatform_OptStress49RTTests.cpp - unaligned wide mem -*-C++*-=//
//
// OptStress21 assembles wide words from a stack buffer byte-by-byte with
// explicit shifts; these probes instead use `__builtin_memcpy` of 2/4/8 bytes,
// which clang -O2 lowers to a SINGLE unaligned wide load/store at a runtime
// offset (movl/movq, ldur, unaligned ldr/str) — a different lift path that
// must model a multi-byte LOAD/STORE at an arbitrary frame offset, clang's
// load/store coalescing (several adjacent byte stores fused into one wide
// store), and store-to-load forwarding across mismatched widths:
//
//   * ucomb   - overlapping unaligned 4-byte loads at a data-dependent offset
//               (load combining), carried hash defeats vectorization.
//   * wtail   - 4-byte body + a 1/2/3-byte tail read with narrower memcpy
//               (mixed-width tail load, the murmur/FNV finalizer shape).
//   * b2w     - individual byte stores then a wide 4-byte read-back
//               (store coalescing + store-to-load forwarding wide<-narrow).
//   * wstore  - wide 4-byte store at a runtime offset then per-byte read-back
//               (wide store aliasing narrower loads).
//   * u64un   - unaligned 8-byte load/store (one ldr/str on 64-bit targets,
//               a split 32-bit pair on i386/ARM32); const-only 64-bit ops so
//               no __udivdi3/var-shift helper is emitted.
//   * swapw   - 4-byte load, byte-swap via the shift/or idiom, store, reload
//               (bswap recognition through stack memory).
//
// All integer, arrays seed from the LCG (no memset zero-init), fold to one
// integer return, no float / 64-bit divide / variable 64-bit shift helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress49RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress49RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress49RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress49RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress49RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress49RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress49RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress49RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress49TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Overlapping unaligned 4-byte loads at a data-dependent offset.
    {p+"_ucomb",
     t+" "+p+"_ucomb("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[40];\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; b[i]=(unsigned char)(s>>9); }\n"
     "  unsigned h=2166136261u;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)%37u; unsigned w;\n"
     "    __builtin_memcpy(&w,b+j,4);\n"
     "    h=(h^w)*16777619u; h=(h<<7)|(h>>25); }\n"
     "  return ("+t+")h; }\n",
     {0x4cULL}, "OptStress49", 2},

    // 4-byte body + 1/2/3-byte tail read with narrower memcpy (mixed-width).
    {p+"_wtail",
     t+" "+p+"_wtail("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[48];\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u; b[i]=(unsigned char)(s>>13); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=((s>>4)%11u)+1u, j=(s>>8)%32u, k=0; unsigned acc=h;\n"
     "    while(k+4u<=n){ unsigned w; __builtin_memcpy(&w,b+j+k,4); acc=acc*131u+w; k+=4u; }\n"
     "    if(k+2u<=n){ unsigned short w; __builtin_memcpy(&w,b+j+k,2); acc=acc*131u+w; k+=2u; }\n"
     "    if(k<n){ acc=acc*131u+b[j+k]; }\n"
     "    h=acc^(acc>>15); }\n"
     "  return ("+t+")h; }\n",
     {0x9bULL}, "OptStress49", 2},

    // Individual byte stores then a wide 4-byte read-back (store coalescing).
    {p+"_b2w",
     t+" "+p+"_b2w("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[36];\n"
     "  for(int i=0;i<36;i++) b[i]=0;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)%32u;\n"
     "    b[j]=(unsigned char)s; b[j+1]=(unsigned char)(s>>8);\n"
     "    b[j+2]=(unsigned char)(s>>16); b[j+3]=(unsigned char)(s>>24);\n"
     "    unsigned w; __builtin_memcpy(&w,b+j,4);\n"
     "    h=h*1000003u+w; }\n"
     "  return ("+t+")h; }\n",
     {0xa7ULL}, "OptStress49", 2},

    // Wide 4-byte store at a runtime offset then per-byte read-back.
    {p+"_wstore",
     t+" "+p+"_wstore("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[36];\n"
     "  for(int i=0;i<36;i++){ s=s*1103515245u+12345u; b[i]=(unsigned char)(s>>17); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)%32u, w=s^(s<<11);\n"
     "    __builtin_memcpy(b+j,&w,4);\n"
     "    h+=(unsigned)b[j]+((unsigned)b[j+1]<<8)+((unsigned)b[j+2]<<16)+((unsigned)b[j+3]<<24);\n"
     "    h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x55ULL}, "OptStress49", 2},

    // Unaligned 8-byte load/store (one ldr/str on 64-bit, 32-bit pair on
    // 32-bit) with const-only 64-bit ops -> no libcall.
    {p+"_u64un",
     t+" "+p+"_u64un("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[48];\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u; b[i]=(unsigned char)(s>>7); }\n"
     "  unsigned long long h=1469598103934665603ULL;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)%41u; unsigned long long w;\n"
     "    __builtin_memcpy(&w,b+j,8);\n"
     "    h=(h^w)*1099511628211ULL; h^=h>>29;\n"
     "    w+=h; __builtin_memcpy(b+j,&w,8); }\n"
     "  return ("+t+")(unsigned)(h^(h>>32)); }\n",
     {0x6dULL}, "OptStress49", 2},

    // 4-byte load, byte-swap via shift/or idiom, store, reload.
    {p+"_swapw",
     t+" "+p+"_swapw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char b[40];\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u; b[i]=(unsigned char)(s>>5); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)%37u, w;\n"
     "    __builtin_memcpy(&w,b+j,4);\n"
     "    w=((w&0xFFu)<<24)|((w&0xFF00u)<<8)|((w>>8)&0xFF00u)|((w>>24)&0xFFu);\n"
     "    __builtin_memcpy(b+j,&w,4);\n"
     "    unsigned r; __builtin_memcpy(&r,b+j,4);\n"
     "    h=h*131u+r; }\n"
     "  return ("+t+")h; }\n",
     {0x13ULL}, "OptStress49", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress49TC("x64o49", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress49TC("x86o49", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress49TC("a64o49", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress49TC("armo49", "int");

INSTANTIATE_TEST_SUITE_P(OptStress49, X64OptStress49RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress49, X86OptStress49RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress49, A64OptStress49RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress49, ARM32OptStress49RT, ::testing::ValuesIn(kARM), rtTCName);
