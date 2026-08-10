//===- AllPlatform_MemBuiltinLiveRTTests.cpp - live external mem* ABI =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip equivalence for functions that emit *live* external
// memcpy/memset/memmove calls (clang lowers a runtime-size __builtin_mem* to a
// real libcall, not an inline loop).  These are the calls behind §15.2
// arm32-memset-note: a relocatable object names the callee by a branch
// relocation whose displacement target is a placeholder 0, so recoverCallAbi
// must NOT borrow the VA-0 intra-module function's arity (ARM32/AArch64 `bl`),
// and on i386 the cdecl stack-argument order must survive (the call passes every
// argument on the stack, no ECX/EDX regparm).  A freestanding mem* helper is
// linked into BOTH the original and recompiled images (LinkMemBuiltins, ON by
// default), so each side genuinely executes the copy/set and the return value
// (an accumulation over the touched bytes) differs the moment an argument is
// dropped or reordered.
//
//   * memtrip   - memset + memcpy + overlapping memmove, runtime size (-O2).
//   * memtrip0  - the same combined shape at -O0 (the clean `call mem*` form).
//   * memwide   - larger buffers + larger runtime size (-O2).
//   * memovlp   - memcpy + a TIGHT overlapping memmove(dst+1, dst, n-1) (-O2).
//   * memchain0 - memset -> memcpy(from memset'd) -> memmove chain (-O0).
//
// All buffers are fixed-size stack arrays, all sizes derived from the masked
// argument and bounded well within the buffer, all bytes unsigned so native and
// lifted agree bit-for-bit.  All four targets x {ELF default}.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64MemBuiltinLiveRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64MemBuiltinLiveRT, Verify) { roundTripX64(GetParam()); }
class X86MemBuiltinLiveRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MemBuiltinLiveRT, Verify) { roundTripX86(GetParam()); }
class A64MemBuiltinLiveRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64MemBuiltinLiveRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32MemBuiltinLiveRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32MemBuiltinLiveRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeMemBuiltinLiveTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // memset + memcpy + overlapping memmove, runtime size, -O2.
    {p+"_memtrip",
     t+" "+p+"_memtrip("+t+" a){ unsigned char src[96],dst[96],tmp[96]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u; src[i]=(unsigned char)(h>>9); }\n"
     "  unsigned n=64u+(h&31u);\n"
     "  __builtin_memset(tmp,(int)((h&0x3Fu)+1u),n);\n"
     "  __builtin_memcpy(dst,src,n);\n"
     "  __builtin_memmove(dst+5,dst,n-5);\n"
     "  unsigned acc=0; for(unsigned i=0;i<n;i++){ acc=acc*131u+dst[i]+tmp[i]+i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "MemBuiltinLive", 2},

    // The same combined shape at -O0 (the clean `call mem*` form, as clang emits
    // for an -O0 computed-goto table copy).
    {p+"_memtrip0",
     t+" "+p+"_memtrip0("+t+" a){ unsigned char src[96],dst[96],tmp[96]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u; src[i]=(unsigned char)(h>>9); }\n"
     "  unsigned n=64u+(h&31u);\n"
     "  __builtin_memset(tmp,(int)((h&0x3Fu)+1u),n);\n"
     "  __builtin_memcpy(dst,src,n);\n"
     "  __builtin_memmove(dst+5,dst,n-5);\n"
     "  unsigned acc=0; for(unsigned i=0;i<n;i++){ acc=acc*131u+dst[i]+tmp[i]+i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "MemBuiltinLive", 0},

    // Larger buffers + larger runtime size (-O2): stresses the size argument
    // (third stack/register argument) at a value the optimizer cannot bound.
    {p+"_memwide",
     t+" "+p+"_memwide("+t+" a){ unsigned char src[256],dst[256],tmp[256]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<256;i++){ h=h*1103515245u+12345u; src[i]=(unsigned char)(h>>7); }\n"
     "  unsigned n=192u+(h&63u);\n"
     "  __builtin_memset(tmp,(int)((h>>3)&0xFFu),n);\n"
     "  __builtin_memcpy(dst,src,n);\n"
     "  __builtin_memmove(dst+9,dst,n-9);\n"
     "  unsigned acc=0; for(unsigned i=0;i<n;i++){ acc=acc*131u+dst[i]+tmp[i]+i*3u; }\n"
     "  return ("+t+")acc; }\n",
     {0x2BCDEu}, "MemBuiltinLive", 2},

    // memcpy + a TIGHT overlapping memmove(dst+1, dst, n-1) (-O2): the
    // byte-at-a-time backward copy in the freestanding helper makes a dropped
    // length or swapped src/dst diverge immediately.
    {p+"_memovlp",
     t+" "+p+"_memovlp("+t+" a){ unsigned char src[128],dst[128]; unsigned h=(unsigned)a;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; src[i]=(unsigned char)(h>>13); }\n"
     "  unsigned n=96u+(h&31u);\n"
     "  __builtin_memcpy(dst,src,n);\n"
     "  __builtin_memmove(dst+1,dst,n-1);\n"
     "  unsigned acc=0; for(unsigned i=0;i<n;i++){ acc=acc*131u+dst[i]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x3CAFEu}, "MemBuiltinLive", 2},

    // memset -> memcpy(from the memset'd buffer) -> memmove chain (-O0): the
    // result of one mem* feeds the next, so every call must execute correctly.
    {p+"_memchain0",
     t+" "+p+"_memchain0("+t+" a){ unsigned char a0[112],b0[112],c0[112]; unsigned h=(unsigned)a;\n"
     "  unsigned n=80u+(h&31u);\n"
     "  __builtin_memset(a0,(int)((h&0x7Fu)|1u),n);\n"
     "  for(unsigned i=0;i<n;i++) a0[i]=(unsigned char)(a0[i]+(unsigned char)(i*7u));\n"
     "  __builtin_memcpy(b0,a0,n);\n"
     "  __builtin_memmove(c0,b0,n);\n"
     "  unsigned acc=0; for(unsigned i=0;i<n;i++){ acc=acc*131u+a0[i]+b0[i]+c0[i]+i; }\n"
     "  return ("+t+")acc; }\n",
     {0x4D00Du}, "MemBuiltinLive", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeMemBuiltinLiveTC("x64mbl", "long");
static const std::vector<RoundTripTC> kX86 = makeMemBuiltinLiveTC("x86mbl", "int");
static const std::vector<RoundTripTC> kA64 = makeMemBuiltinLiveTC("a64mbl", "long");
static const std::vector<RoundTripTC> kARM = makeMemBuiltinLiveTC("armmbl", "int");

INSTANTIATE_TEST_SUITE_P(MemBuiltinLive, X64MemBuiltinLiveRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemBuiltinLive, X86MemBuiltinLiveRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemBuiltinLive, A64MemBuiltinLiveRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(MemBuiltinLive, ARM32MemBuiltinLiveRT, ::testing::ValuesIn(kARM), rtTCName);
