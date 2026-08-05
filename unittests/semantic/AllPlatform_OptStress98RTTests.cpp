//===- AllPlatform_OptStress98RTTests.cpp - overlap/unaligned forwarding -===//
//
// OptStress97 showed the lifter's memory model survives aligned-view union
// punning, packed-field RMW, half/byte partial stores and a byte copy + one
// unaligned 4-byte load.  This probe turns the screws on the same axis with the
// shapes most likely to break a store-to-load model that only handles exact or
// fully-covering matches:
//
//   * u8    - an UNALIGNED 8-byte __builtin_memcpy load AND store at runtime
//             offsets.  On i386/ARM32 each splits into two unaligned 4-byte
//             accesses + recombine, so the pair model and the unaligned lowering
//             are both exercised at once.
//   * ovlap - PARTIAL-overlap forwarding: a u32 stored at offset d, then a u32
//             loaded at d+(0..3) so the load straddles the store by 1..3 bytes.
//             A forward that assumed full coverage (or no overlap) diverges.
//   * mixw  - a dense MIXED-WIDTH overlapping chain on one region: u16 store ->
//             u8 store inside it -> u32 load covering both -> u32 store at +1
//             (overlapping) -> u16 load from the middle.  The whole 8/16/32
//             forwarding matrix on aliasing addresses in a single iteration.
//   * agg   - aggregate struct copy `arr[d]=arr[src]` (clang inlines a multi-word
//             load/store) at runtime indices, then field RMW: multi-field memory
//             move modeling.
//
// All integer in / integer out, stack-local (no globals / relocations / rodata),
// LCG-seeded, fully initialised before any read, folded to one integer return.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress98RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress98RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress98RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress98RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress98RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress98RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress98RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress98RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress98TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Unaligned 8-byte load AND store at runtime offsets (two unaligned 4-byte
    // accesses + recombine on 32-bit; pair model + unaligned lowering together).
    {p+"_u8",
     t+" "+p+"_u8("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0; unsigned char buf[32];\n"
     "  for(int i=0;i<32;i++) buf[i]=(unsigned char)(s*(unsigned)(i+1));\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned long long v; __builtin_memcpy(&v,&buf[(s>>3)&7u],8);\n"
     "    v+=(unsigned long long)i*0x100000001ull;\n"
     "    __builtin_memcpy(&buf[(s>>6)&7u],&v,8);\n"
     "    acc+=v; acc^=acc>>11; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xC8u}, "OptStress98", 2},

    // Partial-overlap store-to-load: u32 store at d, u32 load at d+(0..3) so the
    // read straddles the write by 1..3 bytes (full-coverage assumption diverges).
    {p+"_ovlap",
     t+" "+p+"_ovlap("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0; unsigned char buf[32];\n"
     "  for(int i=0;i<32;i++) buf[i]=(unsigned char)(s+(unsigned)i*7u);\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned d=(s>>4)&7u; unsigned win=s^(unsigned)i;\n"
     "    __builtin_memcpy(&buf[d],&win,4);\n"
     "    unsigned r; __builtin_memcpy(&r,&buf[d+((s>>8)&3u)],4);\n"
     "    acc+=(unsigned long long)r; acc^=acc>>7; }\n"
     "  return ("+t+")acc; }\n",
     {0x1Du}, "OptStress98", 2},

    // Dense mixed-width overlapping chain on one region: the 8/16/32 forwarding
    // matrix on aliasing addresses in a single iteration.
    {p+"_mixw",
     t+" "+p+"_mixw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0; unsigned char buf[16];\n"
     "  for(int i=0;i<16;i++) buf[i]=(unsigned char)(s>>(i&7));\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned o=(s>>4)&7u; unsigned short h=(unsigned short)(s>>2);\n"
     "    __builtin_memcpy(&buf[o],&h,2);\n"
     "    buf[o+((s>>6)&1u)]=(unsigned char)(s>>9);\n"
     "    unsigned w; __builtin_memcpy(&w,&buf[o],4); w+=s;\n"
     "    __builtin_memcpy(&buf[o+1],&w,4);\n"
     "    unsigned short h2; __builtin_memcpy(&h2,&buf[o+2],2);\n"
     "    acc+=(unsigned long long)w+h2; acc^=acc>>6; }\n"
     "  return ("+t+")acc; }\n",
     {0x2Eu}, "OptStress98", 2},

    // Aggregate struct copy at runtime indices (inline multi-word load/store) +
    // field RMW: multi-field memory move modeling.
    {p+"_agg",
     "struct "+p+"_AG{ unsigned x; unsigned short y; unsigned char z; };\n"
     +t+" "+p+"_agg("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; struct "+p+"_AG arr[5]; unsigned long long acc=0;\n"
     "  for(int i=0;i<5;i++){ arr[i].x=s*((unsigned)i+1u);\n"
     "    arr[i].y=(unsigned short)(s>>i); arr[i].z=(unsigned char)(s>>(8+i)); }\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned d=(s>>4)%5u, src=(s>>8)%5u;\n"
     "    arr[d]=arr[src];\n"
     "    arr[d].x+=s; arr[d].y^=(unsigned short)i;\n"
     "    acc+=(unsigned long long)arr[d].x+arr[d].y+arr[d].z; acc^=acc>>5; }\n"
     "  return ("+t+")(acc+arr[0].x); }\n",
     {0x3Fu}, "OptStress98", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress98TC("x64o98", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress98TC("x86o98", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress98TC("a64o98", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress98TC("armo98", "int");

INSTANTIATE_TEST_SUITE_P(OptStress98, X64OptStress98RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress98, X86OptStress98RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress98, A64OptStress98RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress98, ARM32OptStress98RT, ::testing::ValuesIn(kARM), rtTCName);
