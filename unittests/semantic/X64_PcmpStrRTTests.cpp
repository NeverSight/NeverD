//===- X64_PcmpStrRTTests.cpp - SSE4.2 PCMPxSTRx roundtrip ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probes for the SSE4.2 packed string-compare instructions
// PCMPISTRI/PCMPESTRI (index -> ECX) and PCMPISTRM/PCMPESTRM (mask -> XMM0).
//
// These were previously lifted as a non-modelling placeholder: an opaque
// 0-operand intrinsic was emitted and RCX was overwritten with an
// uninitialised temp — the index/mask result and every status flag
// (CF/ZF/SF/OF) came out wrong, and PCMPESTRx's explicit lengths (EAX/EDX)
// were ignored entirely.  Despite the file name `X64_SSE42StringRTTests.cpp`
// the instructions had no roundtrip coverage at all (that file only exercised
// POPCNT/packed-int patterns), so the placeholder went unnoticed.
//
// The fix maps the index/mask result to the real LLVM x86.sse42.pcmp*str*
// intrinsic and derives CF/ZF/SF/OF from the dedicated per-flag intrinsics
// (the comparison clears AF/PF), so the recompiled code lowers back to
// pcmp*str* + setcc and matches hardware bit-for-bit.
//
// The operands are built from the function arguments with vector extensions
// (a plain movq, so no rodata constant pool is needed) and the SSE4.2 string
// builtins (__builtin_ia32_pcmp*str*128) are used directly to avoid the
// <nmmintrin.h> -> <stdlib.h> include chain under the freestanding cross
// target.  Each probe folds the index/mask/flags into the return value so the
// original-vs-recompiled comparison genuinely depends on the modelled result.
// PCMPxSTRx is native on the default (Haswell) Unicorn CPU, so this is a pure
// lift fix.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PcmpStrRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PcmpStrRT, Verify) { roundTripX64(GetParam()); }

// Common prologue: vector typedefs + two operands built from the args.
//   v16 = 16 packed bytes, v2 = 2 packed i64; `(v16)(v2){a,0}` is a movq.
#define PCMP_PRO                                                                \
  "typedef char v16 __attribute__((vector_size(16)));"                         \
  "typedef long long v2 __attribute__((vector_size(16)));"

// imm8 control byte fields (Intel SDM):
//   [1:0] data format  : 0=ubyte 1=uword 2=sbyte 3=sword
//   [3:2] aggregation  : 0=equal_any 1=ranges 2=equal_each 3=equal_ordered
//   [5:4] polarity     : 0=positive 1=negative 2=masked+ 3=masked-
//   [6]   output select: index LSB/MSB ; mask bit/unit

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // PCMPISTRI, equal-each (ubyte), index -> ECX.  Two 8-byte strings that
  // differ partway, so the returned index is neither 0 nor 16.
  {"pistri_eq_idx",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpistri128(x,y,0x08);}\n",
   {0x1122334455667788LL, 0x112233445566AA88LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRI, equal-any (ubyte): treat x as a char set, find first char of y
  // present in it.  index -> ECX.
  {"pistri_any_idx",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpistri128(x,y,0x00);}\n",
   {0x0000000000003341LL, 0x1122334455667788LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRI, equal-ordered (substring search) — the classic strstr idiom.
  {"pistri_ord_idx",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpistri128(x,y,0x0C);}\n",
   {0x0000000000006655LL, 0x1122334455667788LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRI, uword (16-bit) equal-each: exercises a different data format.
  {"pistri_word_idx",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpistri128(x,y,0x09);}\n",
   {0x1111222233334444LL, 0x1111222299994444LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRI, negative polarity (bit5): inverts the result bits before the
  // index search — verifies the imm is forwarded verbatim.
  {"pistri_neg_idx",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpistri128(x,y,0x18);}\n",
   {0x1122334455667788LL, 0x112233445566AA88LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRI flags: capture CF/ZF/SF/OF together so the roundtrip depends on
  // every status flag the instruction sets.
  {"pistri_flags",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "int c=__builtin_ia32_pcmpistric128(x,y,0x08);"
   "int z=__builtin_ia32_pcmpistriz128(x,y,0x08);"
   "int s=__builtin_ia32_pcmpistris128(x,y,0x08);"
   "int o=__builtin_ia32_pcmpistrio128(x,y,0x08);"
   "return c|(z<<1)|(s<<2)|(o<<3);}\n",
   {0x1122334455667788LL, 0x112233445566AA88LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRI flags where one operand has an embedded null: drives ZF/SF (null
  // reached in operand 2 / operand 1) to set.
  {"pistri_flags_null",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "int c=__builtin_ia32_pcmpistric128(x,y,0x08);"
   "int z=__builtin_ia32_pcmpistriz128(x,y,0x08);"
   "int s=__builtin_ia32_pcmpistris128(x,y,0x08);"
   "int o=__builtin_ia32_pcmpistrio128(x,y,0x08);"
   "return c|(z<<1)|(s<<2)|(o<<3);}\n",
   {0x0000000000333231LL, 0x0000003433323100LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRIA combines CF==0 && ZF==0 in one branch idiom (strchr loop guard).
  {"pistri_after",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpistria128(x,y,0x08);}\n",
   {0x1122334455667788LL, 0x1122334455667788LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRM, bit mask -> XMM0; fold the low 64 bits of the mask.
  {"pistrm_bitmask",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "v16 r=__builtin_ia32_pcmpistrm128(x,y,0x08);"
   "return ((v2)r)[0];}\n",
   {0x1122334455667788LL, 0x112233445566AA88LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPISTRM, unit (byte) mask -> XMM0: each lane is 0x00 or 0xFF.
  {"pistrm_unitmask",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "v16 r=__builtin_ia32_pcmpistrm128(x,y,0x48);"
   "return ((v2)r)[0];}\n",
   {0x1122334455667788LL, 0x112233445566AA88LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPESTRI, explicit lengths in EAX/EDX, index -> ECX.  la=5 truncates the
  // first operand so the explicit-length path is genuinely exercised.
  {"pestri_idx",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpestri128(x,5,y,8,0x08);}\n",
   {0x1122334455667788LL, 0x1122334455667788LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPESTRI flags with explicit lengths: la=3, lb=8 so SF/ZF differ.
  {"pestri_flags",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "int c=__builtin_ia32_pcmpestric128(x,3,y,8,0x08);"
   "int z=__builtin_ia32_pcmpestriz128(x,3,y,8,0x08);"
   "int s=__builtin_ia32_pcmpestris128(x,3,y,8,0x08);"
   "int o=__builtin_ia32_pcmpestrio128(x,3,y,8,0x08);"
   "return c|(z<<1)|(s<<2)|(o<<3);}\n",
   {0x1122334455667788LL, 0x112233445566AA88LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPESTRM, explicit lengths, bit mask -> XMM0.
  {"pestrm_mask",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "v16 r=__builtin_ia32_pcmpestrm128(x,6,y,8,0x08);"
   "return ((v2)r)[0];}\n",
   {0x1122334455667788LL, 0x1122334455667788LL}, "PcmpStr", 1, "-msse4.2"},

  // PCMPESTRI with a length larger than the element count: hardware caps it at
  // 16, exercising EAX/EDX read fidelity (la=20 capped, lb=4).
  {"pestri_idx_caplen",
   PCMP_PRO
   "long f(long a,long b){v16 x=(v16)(v2){a,0},y=(v16)(v2){b,0};"
   "return __builtin_ia32_pcmpestri128(x,20,y,4,0x0C);}\n",
   {0x0000000000006655LL, 0x1122334455667788LL}, "PcmpStr", 1, "-msse4.2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(PcmpStr, X64PcmpStrRT, ::testing::ValuesIn(kX64),
                         rtTCName);
