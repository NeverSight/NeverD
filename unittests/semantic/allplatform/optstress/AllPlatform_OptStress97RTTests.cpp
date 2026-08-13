//===- AllPlatform_OptStress97RTTests.cpp - sub-word memory aliasing -*-C++-==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The recent arc (#473-#486, OptStress74-96) drove the "constant pool mapping"
// (global-address symbolization) and the lone-sign-flag DNA.  This probe attacks
// a different axis the OptStress series under-hits: the lifter's MEMORY MODEL —
// store-to-load forwarding across mismatched widths, sub-word partial overlap,
// unaligned access, and byte-order — all in plain C at -O2 so clang's NATIVE
// per-target sub-word / unaligned lowering (x86 direct unaligned, ARM32 ldr with
// SCTLR.A=0, AArch64 unaligned) is what gets lifted.  StackMixedWidth has one
// case per arch and X64_MemForward is x86-only inline asm; nothing carries a
// sub-word aliasing stream through a loop on ALL FOUR targets.
//
//   * punw  - a union {u64; u32[2]; u16[4]; u8[8]} written wide then read through
//             every narrower view at RUNTIME indices (forces the object to stay
//             in memory), plus a sub-byte RMW the following wide read must see:
//             store64 -> load32/16/8 -> load8^store8 -> load64 forwarding.
//   * pack1 - a #pragma pack(1) struct array whose u32 `val` sits at byte offset
//             1 (unaligned); a runtime-indexed read-modify-write hammers the
//             unaligned 32/16-bit load+store lowering each iteration.
//   * ovl   - overlapping partial stores (one u16 half + one u8 that may land
//             inside it) merged into a wide u32 read: order- and endian-sensitive
//             partial-store forwarding.
//   * bcpy  - a hand-rolled overlapping byte copy (memmove-shaped) followed by an
//             unaligned 4-byte __builtin_memcpy load: byte-addressed memory plus
//             an unaligned wide load, no libcall.
//
// All integer in / integer out, stack-local (no globals / relocations / rodata),
// LCG-seeded, folded to one integer return.  A single wrong byte offset, stale
// forward, or endian flip diverges from the native run.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress97RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress97RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress97RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress97RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress97RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress97RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress97RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress97RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress97TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Union punning: wide store, narrow reads at runtime views, sub-byte RMW the
    // following wide read must observe (store64 -> load32/16/8 -> load8^store8 ->
    // load64 store-to-load forwarding across the full overlap matrix).
    {p+"_punw",
     "union "+p+"_U{ unsigned long long u; unsigned w[2]; unsigned short h[4]; unsigned char b[8]; };\n"
     +t+" "+p+"_punw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0; union "+p+"_U v;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u;\n"
     "    v.u=((unsigned long long)s<<32)|(unsigned long long)(s^0xABCDu);\n"
     "    unsigned w=v.w[(s>>3)&1u];\n"
     "    unsigned short h=v.h[(s>>5)&3u];\n"
     "    unsigned char b=v.b[(s>>7)&7u];\n"
     "    v.b[(s>>9)&7u]^=(unsigned char)s;\n"
     "    acc+=(unsigned long long)w+(unsigned long long)h+(unsigned long long)b+v.u;\n"
     "    acc^=acc>>13; }\n"
     "  return ("+t+")(acc^(acc>>32)); }\n",
     {0xA7u}, "OptStress97", 2},

    // Packed struct: u32 `val` at byte offset 1 (unaligned); runtime-indexed RMW
    // exercises the unaligned 32/16-bit load+store lowering every iteration.
    {p+"_pack1",
     "#pragma pack(push,1)\n"
     "struct "+p+"_PS{ unsigned char tag; unsigned val; unsigned short aux; };\n"
     "#pragma pack(pop)\n"
     +t+" "+p+"_pack1("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; struct "+p+"_PS arr[6]; unsigned long long acc=0;\n"
     "  for(int i=0;i<6;i++){ arr[i].tag=(unsigned char)(s+(unsigned)i);\n"
     "    arr[i].val=s*((unsigned)i+1u); arr[i].aux=(unsigned short)(s>>i); }\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned k=(s>>4)%6u;\n"
     "    arr[k].val+=s^arr[(k+1u)%6u].val;\n"
     "    arr[k].aux+=(unsigned short)(arr[k].val>>3);\n"
     "    acc+=(unsigned long long)arr[k].val+arr[k].aux+arr[k].tag; acc^=acc>>7; }\n"
     "  return ("+t+")(acc+arr[0].val+arr[5].aux); }\n",
     {0x35u}, "OptStress97", 2},

    // Overlapping partial stores (a u16 half + a u8 possibly inside it) merged
    // into a wide u32 read: order- and endian-sensitive partial-store forwarding.
    {p+"_ovl",
     "union "+p+"_V{ unsigned u; unsigned short h[2]; unsigned char b[4]; };\n"
     +t+" "+p+"_ovl("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0; union "+p+"_V v; v.u=s;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    v.h[(s>>4)&1u]=(unsigned short)(s>>8);\n"
     "    v.b[(s>>6)&3u]=(unsigned char)(s>>3);\n"
     "    acc+=(unsigned long long)v.u; acc^=acc>>5; v.u+=s; }\n"
     "  return ("+t+")acc; }\n",
     {0x5Cu}, "OptStress97", 2},

    // Hand-rolled overlapping byte copy (memmove-shaped) + an unaligned 4-byte
    // __builtin_memcpy load: byte-addressed memory plus an unaligned wide load.
    {p+"_bcpy",
     t+" "+p+"_bcpy("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned long long acc=0; unsigned char buf[16];\n"
     "  for(int i=0;i<16;i++) buf[i]=(unsigned char)(s>>(i&7));\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    int d=(int)((s>>4)&7u), so=(int)((s>>7)&7u), n=(int)((s>>10)&7u)+1;\n"
     "    for(int j=0;j<n;j++) buf[(d+j)&15]=buf[(so+j)&15];\n"
     "    unsigned w; __builtin_memcpy(&w,&buf[(s>>2)&3u],4);\n"
     "    acc+=(unsigned long long)w; acc^=acc>>6; }\n"
     "  return ("+t+")acc; }\n",
     {0x6Eu}, "OptStress97", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress97TC("x64o97", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress97TC("x86o97", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress97TC("a64o97", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress97TC("armo97", "int");

INSTANTIATE_TEST_SUITE_P(OptStress97, X64OptStress97RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress97, X86OptStress97RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress97, A64OptStress97RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress97, ARM32OptStress97RT, ::testing::ValuesIn(kARM), rtTCName);
