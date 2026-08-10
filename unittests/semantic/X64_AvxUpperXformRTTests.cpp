//===- X64_AvxUpperXformRTTests.cpp - AVX2-256 immediate-shuffle guards -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Regression guards for the Unicorn-fork VEX.256 helper bug: the 256-bit
// decoder runs each 128-bit SSE helper twice (once per 128-bit lane) with the
// operand pointers advanced by 16 bytes.  helper_pshufd / helper_shufps /
// helper_shufpd wrote their 128-bit result via `*d = r;` where `r` is a full
// 512-bit ZMMReg with uninitialised upper bytes, so the low-lane call clobbered
// the destination's high 128 bits with garbage — corrupting the high-lane
// source of an in-place `vpshufd ymm0,ymm0` (the exact shape clang emits for
// vectorised `u32 % constant`).  Fixed by storing exactly 128 bits.
//
// Each probe folds ALL eight 32-bit lanes into the return value so any lane
// divergence between the original (native bytes in Unicorn) and the NeverD
// recompile flips it.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AvxUpperRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AvxUpperRT, Verify) { roundTripX64(GetParam()); }

#define FILL8                                                                  \
  "  unsigned x[8],y[8]; unsigned s=(unsigned)a|1u;\n"                         \
  "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s; }\n"               \
  "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; y[i]=s; }\n"
#define HASH8(BUF)                                                             \
  "  unsigned r=0; for(int i=0;i<8;i++) r=r*33u+" BUF "[i]; return (long)r;"

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // vpshufd in-place ymm (the magic-division shape) — the original repro.
  {"vpshufd_inplace",
   "long vpshufd_inplace(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpshufd $0xf5,%%ymm0,%%ymm0\\n\\tvmovdqu %%ymm0,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // vpshufd distinct dst — must remain correct.
  {"vpshufd_distinct",
   "long vpshufd_distinct(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpshufd $0x1b,%%ymm0,%%ymm1\\n\\tvmovdqu %%ymm1,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm1\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // vshufps in-place ymm (helper_shufps `*d = r`).
  {"vshufps_inplace",
   "long vshufps_inplace(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvshufps $0x4e,%%ymm1,%%ymm0,%%ymm0\\n\\tvmovdqu %%ymm0,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // vshufpd in-place ymm (helper_shufpd `*d = r`).
  {"vshufpd_inplace",
   "long vshufpd_inplace(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvshufpd $0x5,%%ymm1,%%ymm0,%%ymm0\\n\\tvmovdqu %%ymm0,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // vpalignr in-place ymm (per-128-lane byte align; emitter 256-bit path).
  {"vpalignr_inplace",
   "long vpalignr_inplace(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpalignr $5,%%ymm1,%%ymm0,%%ymm0\\n\\tvmovdqu %%ymm0,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // vpalignr distinct dst, different shift.
  {"vpalignr_distinct",
   "long vpalignr_distinct(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpalignr $11,%%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},

  // 256-bit unpacks (NeverD emitUnpackShuffle was 128-bit-only -> 0).
  {"vpunpckldq256",
   "long vpunpckldq256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpunpckldq %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},
  {"vpunpckhwd256",
   "long vpunpckhwd256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpunpckhwd %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vpunpcklbw256",
   "long vpunpcklbw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpunpcklbw %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vpunpckhqdq256",
   "long vpunpckhqdq256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpunpckhqdq %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},

  // 256-bit vpshufb (NeverD emitPshufb was 128-bit-only -> 0).  Control bytes
  // masked to [0,15] so no lane is zeroed, exercising the real permute.
  {"vpshufb256",
   "long vpshufb256(long a){\n" FILL8
   "  unsigned char ctl[32]; for(int i=0;i<32;i++) ctl[i]=(unsigned char)((x[i&7]>>((i&3)*8))&0x0f);\n"
   "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpshufb %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(ctl):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // 256-bit vpshuflw / vpshufhw (emitImmShuffle bitcast i256->v8i16 crashed).
  {"vpshuflw256",
   "long vpshuflw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpshuflw $0x1b,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vpshufhw256",
   "long vpshufhw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpshufhw $0x1b,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vpshuflw256_inplace",
   "long vpshuflw256_inplace(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpshuflw $0xb1,%%ymm0,%%ymm0\\n\\tvmovdqu %%ymm0,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},

  // 256-bit vmovsldup / vmovshdup / vmovddup (emitMovDup was 128-bit-only).
  {"vmovsldup256",
   "long vmovsldup256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovsldup %%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vmovshdup256",
   "long vmovshdup256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovshdup %%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vmovddup256",
   "long vmovddup256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovddup %%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},

  // 256-bit vpmovmskb r32,ymm (emitPmovmskb was 128-bit-only -> 0).  Result is
  // a GPR mask (32 byte sign bits, 16 per lane), returned directly.
  {"vpmovmskb256",
   "long vpmovmskb256(long a){\n" FILL8 "  unsigned r;\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpmovmskb %%ymm0,%%eax\\n\\tmovl %%eax,%0\"\n"
   "    :\"=r\"(r):\"r\"(x):\"ymm0\",\"eax\",\"memory\");\n  return (long)r; }\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},

  // 256-bit vmpsadbw (emitMpsadbw was 128-bit-only -> 0).  imm 0x2a exercises
  // the differing low(imm[2:0]=2)/high(imm[5:3]=5) 128-bit-lane sub-fields.
  {"vmpsadbw256",
   "long vmpsadbw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvmpsadbw $0x2a,%%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},

  // 256-bit integer PACK (helper_pack* still did `*d = r` -> clobbered the
  // adjacent 128-bit lane when the VEX.256 decoder ran the helper per lane).
  {"vpacksswb256",
   "long vpacksswb256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpacksswb %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vpackuswb256",
   "long vpackuswb256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpackuswb %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vpackssdw256",
   "long vpackssdw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpackssdw %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},
  {"vpackusdw256",
   "long vpackusdw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpackusdw %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // 256-bit FP horizontal add/sub (helper_hadd*/hsub* did `*d = r`, same lane
  // clobber).  Integer PRNG bytes reinterpreted as floats; the byte-exact
  // result still round-trips (same softfloat on both sides).
  {"vhaddps256",
   "long vhaddps256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvhaddps %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vhsubps256",
   "long vhsubps256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvhsubps %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vhaddpd256",
   "long vhaddpd256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvhaddpd %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},
  {"vhsubpd256",
   "long vhsubpd256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvhsubpd %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // Cross-lane permutes: VPERMPD/VPERMQ (imm8, 4 qwords) and VPERMPS (var, 8 dwords).
  {"vpermpd_imm",
   "long vpermpd_imm(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpermpd $0x1b,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vpermq_imm",
   "long vpermq_imm(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvpermq $0x1b,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vpermps_var",
   "long vpermps_var(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpermps %%ymm0,%%ymm1,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},

  // Run-time byte/dword blend (VPBLENDVB /is4 mask, VBLENDVPS).
  {"vpblendvb256",
   "long vpblendvb256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpblendvb %%ymm1,%%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vblendvps256",
   "long vblendvps256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvblendvps %%ymm1,%%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},

  // VROUNDPS/PD ymm (per-lane FP round, imm 2 = round up).
  {"vroundps256",
   "long vroundps256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvroundps $2,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},
  {"vroundpd256",
   "long vroundpd256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvroundpd $2,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x):\"ymm0\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},

  // VPHSUBSW ymm (saturating horizontal sub), VPSIGNW ymm, VPMULDQ ymm (even
  // dword signed 32x32->64) -- all per-128-lane, exercise the generic path.
  {"vphsubsw256",
   "long vphsubsw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvphsubsw %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x5c}, "AvxUpper", 3, "-mavx2"},
  {"vpsignw256",
   "long vpsignw256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpsignw %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0x91}, "AvxUpper", 3, "-mavx2"},
  {"vpmuldq256",
   "long vpmuldq256(long a){\n" FILL8 "  unsigned o[8];\n"
   "  __asm__ volatile(\"vmovdqu (%1),%%ymm0\\n\\tvmovdqu (%2),%%ymm1\\n\\tvpmuldq %%ymm1,%%ymm0,%%ymm2\\n\\tvmovdqu %%ymm2,(%0)\"\n"
   "    ::\"r\"(o),\"r\"(x),\"r\"(y):\"ymm0\",\"ymm1\",\"ymm2\",\"memory\");\n" HASH8("o") "}\n",
   {0xa3}, "AvxUpper", 3, "-mavx2"},

  // The real magic-modulo shape carried forward (clang autovectorised).
  {"magic_modu1000",
   "long magic_modu1000(long a){\n"
   "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
   "  v8u v; unsigned s=(unsigned)a|1u;\n"
   "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; v[i]=s; }\n"
   "  v8u z=v%1000u; unsigned r=0; for(int i=0;i<8;i++) r+=z[i];\n"
   "  return (long)r; }\n",
   {0xb7}, "AvxUpper", 3, "-mavx2"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(AvxUpper, X64AvxUpperRT, ::testing::ValuesIn(kX64),
                         rtTCName);
