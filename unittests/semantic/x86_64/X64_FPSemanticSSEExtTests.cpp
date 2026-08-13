//===- X64_FPSemanticSSEExtTests.cpp - x64 packed SSE tests -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Migrated from scripts/lift_verifier.py — SSE integer arithmetic and shifts,
// SSE4, shuffle, compare, bitwise, SSE3 horizontal, unpack and PCMP suites.
//
// Split out of X64_FPSemanticTests.cpp, which holds the TEST_P pattern that
// these INSTANTIATE_TEST_SUITE_P calls attach to.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

// clang-format off

// ============================================================================
// SSE Integer Arithmetic: PADDW, PADDD, PADDQ, PSUBW, PSUBD, PMULLW, etc.
// ============================================================================
static const std::vector<SemTC> kX64SSEIntArith = {
  {"paddw",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; paddw xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEIntArith",
   {{DATA_BASE, cat({packU16(1), packU16(2), packU16(3), packU16(4), packU16(5), packU16(6), packU16(7), packU16(8),
                     packU16(10), packU16(20), packU16(30), packU16(40), packU16(50), packU16(60), packU16(70), packU16(80)})}}},
  {"paddd",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; paddd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEIntArith",
   {{DATA_BASE, cat({packI32(1), packI32(2), packI32(3), packI32(4),
                     packI32(10), packI32(20), packI32(30), packI32(40)})}}},
  {"paddq",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; paddq xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEIntArith",
   {{DATA_BASE, cat({packU64(100), packU64(200), packU64(10), packU64(20)})}}},
  {"psubw",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; psubw xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEIntArith",
   {{DATA_BASE, cat({packU16(10), packU16(20), packU16(30), packU16(40), packU16(50), packU16(60), packU16(70), packU16(80),
                     packU16(1), packU16(2), packU16(3), packU16(4), packU16(5), packU16(6), packU16(7), packU16(8)})}}},
  {"psubd",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; psubd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEIntArith",
   {{DATA_BASE, cat({packI32(10), packI32(20), packI32(30), packI32(40),
                     packI32(1), packI32(2), packI32(3), packI32(4)})}}},
  {"pmullw",        "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pmullw xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEIntArith",
   {{DATA_BASE, cat({packU16(2), packU16(3), packU16(4), packU16(5), packU16(6), packU16(7), packU16(8), packU16(9),
                     packU16(10), packU16(10), packU16(10), packU16(10), packU16(10), packU16(10), packU16(10), packU16(10)})}}},
};

// ============================================================================
// SSE Integer Shift: PSLLW, PSLLD, PSLLQ, PSRLW, PSRLD, PSRLQ, PSRAW, PSRAD,
//                    PSLLDQ, PSRLDQ
// ============================================================================
static const std::vector<SemTC> kX64SSEIntShift = {
  {"psllw",         "movdqu xmm0, [rsi]; psllw xmm0, 2; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, cat({packU16(1), packU16(2), packU16(4), packU16(8), packU16(16), packU16(32), packU16(64), packU16(128)})}}},
  {"pslld",         "movdqu xmm0, [rsi]; pslld xmm0, 4; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, cat({packU32(1), packU32(0xFF), packU32(0x1000), packU32(0xABCD)})}}},
  {"psllq",         "movdqu xmm0, [rsi]; psllq xmm0, 8; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, cat({packU64(1), packU64(0xFF)})}}},
  {"psrlw",         "movdqu xmm0, [rsi]; psrlw xmm0, 2; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, cat({packU16(4), packU16(8), packU16(16), packU16(32), packU16(64), packU16(128), packU16(256), packU16(512)})}}},
  {"psrld",         "movdqu xmm0, [rsi]; psrld xmm0, 4; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, cat({packU32(0xF0), packU32(0xFF0), packU32(0xFFF0), packU32(0xFFFF0)})}}},
  {"psrlq",         "movdqu xmm0, [rsi]; psrlq xmm0, 8; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, cat({packU64(0xFF00), packU64(0xFFFF00)})}}},
  {"pslldq",        "movdqu xmm0, [rsi]; pslldq xmm0, 2; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}}}},
  {"psrldq",        "movdqu xmm0, [rsi]; psrldq xmm0, 2; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSEIntShift",
   {{DATA_BASE, {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}}}},
};

// ============================================================================
// SSE4: PMINSD, PMAXSD, PMINUD, PMAXUD, PMULLD, PBLENDW, PTEST, PMOVSXxx
// ============================================================================
static const std::vector<SemTC> kX64SSE4 = {
  {"pminsd",        "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pminsd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE4",
   {{DATA_BASE, cat({packI32(5), packI32(-3), packI32(10), packI32(0),
                     packI32(3), packI32(-5), packI32(20), packI32(-1)})}}},
  {"pmaxsd",        "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pmaxsd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE4",
   {{DATA_BASE, cat({packI32(5), packI32(-3), packI32(10), packI32(0),
                     packI32(3), packI32(-5), packI32(20), packI32(-1)})}}},
  {"pminud",        "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pminud xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE4",
   {{DATA_BASE, cat({packU32(5), packU32(100), packU32(10), packU32(0),
                     packU32(3), packU32(50), packU32(20), packU32(1)})}}},
  {"pmaxud",        "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pmaxud xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE4",
   {{DATA_BASE, cat({packU32(5), packU32(100), packU32(10), packU32(0),
                     packU32(3), packU32(50), packU32(20), packU32(1)})}}},
  {"pmulld",        "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pmulld xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE4",
   {{DATA_BASE, cat({packI32(2), packI32(3), packI32(4), packI32(5),
                     packI32(10), packI32(20), packI32(30), packI32(40)})}}},
  {"pblendw",       "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pblendw xmm0, xmm1, 0xAA; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE4",
   {{DATA_BASE, cat({packU16(1), packU16(2), packU16(3), packU16(4), packU16(5), packU16(6), packU16(7), packU16(8),
                     packU16(11), packU16(12), packU16(13), packU16(14), packU16(15), packU16(16), packU16(17), packU16(18)})}}},
};

// ============================================================================
// SSE Shuffle: SHUFPS, SHUFPD, PSHUFB, PALIGNR, PEXTRB, PEXTRD, PINSRB, PINSRD
// ============================================================================
static const std::vector<SemTC> kX64SSEShuffle = {
  {"shufps",        "movaps xmm0, [rsi]; movaps xmm1, [rdi]; shufps xmm0, xmm1, 0x1B; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEShuffle",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(5.0f), packF32(6.0f), packF32(7.0f), packF32(8.0f)})}}},
  {"shufpd",        "movapd xmm0, [rsi]; movapd xmm1, [rdi]; shufpd xmm0, xmm1, 1; movapd [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEShuffle",
   {{DATA_BASE, cat({packF64(1.0), packF64(2.0), zeros(16), packF64(3.0), packF64(4.0)})}}},
  {"unpcklps",      "movaps xmm0, [rsi]; movaps xmm1, [rdi]; unpcklps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEShuffle",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(5.0f), packF32(6.0f), packF32(7.0f), packF32(8.0f)})}}},
  {"unpckhps",      "movaps xmm0, [rsi]; movaps xmm1, [rdi]; unpckhps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEShuffle",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(5.0f), packF32(6.0f), packF32(7.0f), packF32(8.0f)})}}},
  {"movhlps",       "movaps xmm0, [rsi]; movaps xmm1, [rdi]; movhlps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEShuffle",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(5.0f), packF32(6.0f), packF32(7.0f), packF32(8.0f)})}}},
  {"movlhps",       "movaps xmm0, [rsi]; movaps xmm1, [rdi]; movlhps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEShuffle",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(5.0f), packF32(6.0f), packF32(7.0f), packF32(8.0f)})}}},
  {"pextrb",        "movdqu xmm0, [rsi]; pextrb eax, xmm0, 5",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEShuffle",
   {{DATA_BASE, {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}}}},
  {"pextrd",        "movdqu xmm0, [rsi]; pextrd eax, xmm0, 2",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEShuffle",
   {{DATA_BASE, cat({packU32(0x11), packU32(0x22), packU32(0x33), packU32(0x44)})}}},
  {"pinsrb",        "movdqu xmm0, [rsi]; pinsrb xmm0, eax, 3; movdqu [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}, {"rax", 0xFF}}, {}, "SSEShuffle",
   {{DATA_BASE, zeros(16)}}},
  {"pinsrd",        "movdqu xmm0, [rsi]; pinsrd xmm0, eax, 1; movdqu [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}, {"rax", 0xDEADBEEF}}, {}, "SSEShuffle",
   {{DATA_BASE, zeros(16)}}},
};

// ============================================================================
// SSE Compare: CMPEQPS, CMPLTPS, MINPS, MAXPS, SQRTPS, etc.
// ============================================================================
static const std::vector<SemTC> kX64SSECmp = {
  {"cmpeqps",       "movaps xmm0, [rsi]; movaps xmm1, [rdi]; cmpeqps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSECmp",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(1.0f), packF32(99.0f), packF32(3.0f), packF32(99.0f)})}}},
  {"cmpltps",       "movaps xmm0, [rsi]; movaps xmm1, [rdi]; cmpltps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSECmp",
   {{DATA_BASE, cat({packF32(1.0f), packF32(5.0f), packF32(3.0f), packF32(10.0f),
                     zeros(16),
                     packF32(2.0f), packF32(2.0f), packF32(4.0f), packF32(4.0f)})}}},
  {"minps",         "movaps xmm0, [rsi]; movaps xmm1, [rdi]; minps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSECmp",
   {{DATA_BASE, cat({packF32(1.0f), packF32(5.0f), packF32(3.0f), packF32(10.0f),
                     zeros(16),
                     packF32(2.0f), packF32(2.0f), packF32(4.0f), packF32(4.0f)})}}},
  {"maxps",         "movaps xmm0, [rsi]; movaps xmm1, [rdi]; maxps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSECmp",
   {{DATA_BASE, cat({packF32(1.0f), packF32(5.0f), packF32(3.0f), packF32(10.0f),
                     zeros(16),
                     packF32(2.0f), packF32(2.0f), packF32(4.0f), packF32(4.0f)})}}},
  {"sqrtps",        "movaps xmm0, [rsi]; sqrtps xmm1, xmm0; movaps [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSECmp",
   {{DATA_BASE, cat({packF32(4.0f), packF32(9.0f), packF32(16.0f), packF32(25.0f)})}}},
  {"sqrtpd",        "movapd xmm0, [rsi]; sqrtpd xmm1, xmm0; movapd [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSECmp",
   {{DATA_BASE, cat({packF64(4.0), packF64(9.0)})}}},
};

// ============================================================================
// SSE Bitwise: ANDPS, ORPS, XORPS, ANDNPS, MOVMSKPS, MOVD, MOVQ
// ============================================================================
static const std::vector<SemTC> kX64SSEBitwise = {
  {"andps",         "movaps xmm0, [rsi]; movaps xmm1, [rdi]; andps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEBitwise",
   {{DATA_BASE, cat({packU32(0xFFFFFFFF), packU32(0), packU32(0xF0F0F0F0), packU32(0x0F0F0F0F),
                     zeros(16),
                     packU32(0x0F0F0F0F), packU32(0xFFFFFFFF), packU32(0x0F0F0F0F), packU32(0xF0F0F0F0)})}}},
  {"orps",          "movaps xmm0, [rsi]; movaps xmm1, [rdi]; orps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEBitwise",
   {{DATA_BASE, cat({packU32(0xF0), packU32(0), packU32(0x0F), packU32(0),
                     zeros(16),
                     packU32(0x0F), packU32(0x0F), packU32(0), packU32(0x0F)})}}},
  {"andnps",        "movaps xmm0, [rsi]; movaps xmm1, [rdi]; andnps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEBitwise",
   {{DATA_BASE, cat({packU32(0xFF), packU32(0), packU32(0xFF00), packU32(0),
                     zeros(16),
                     packU32(0xFFFF), packU32(0xFFFF), packU32(0xFFFF), packU32(0xFFFF)})}}},
  {"movmskps",      "movaps xmm0, [rsi]; movmskps eax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEBitwise",
   {{DATA_BASE, cat({packU32(0x80000000), packU32(0), packU32(0x80000000), packU32(0)})}}},
  {"movmskpd",      "movapd xmm0, [rsi]; movmskpd eax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEBitwise",
   {{DATA_BASE, cat({packU64(0x8000000000000000ULL), packU64(0)})}}},
};

// ============================================================================
// SSE3 Horizontal: HADDPS, HSUBPS, HADDPD, ADDSUBPS, ADDSUBPD
// ============================================================================
static const std::vector<SemTC> kX64SSEMisc = {
  {"haddps",        "movups xmm0, [rsi]; movups xmm1, [rdi]; haddps xmm0, xmm1; movups [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEMisc",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     packF32(5.0f), packF32(6.0f), packF32(7.0f), packF32(8.0f)})}}},
  {"hsubps",        "movups xmm0, [rsi]; movups xmm1, [rdi]; hsubps xmm0, xmm1; movups [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEMisc",
   {{DATA_BASE, cat({packF32(10.0f), packF32(3.0f), packF32(20.0f), packF32(5.0f),
                     packF32(30.0f), packF32(7.0f), packF32(40.0f), packF32(9.0f)})}}},
  {"haddpd",        "movupd xmm0, [rsi]; movupd xmm1, [rdi]; haddpd xmm0, xmm1; movupd [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEMisc",
   {{DATA_BASE, cat({packF64(1.0), packF64(2.0), packF64(3.0), packF64(4.0)})}}},
  {"addsubps",      "movups xmm0, [rsi]; movups xmm1, [rdi]; addsubps xmm0, xmm1; movups [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEMisc",
   {{DATA_BASE, cat({packF32(10.0f), packF32(20.0f), packF32(30.0f), packF32(40.0f),
                     packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f)})}}},
  {"addsubpd",      "movupd xmm0, [rsi]; movupd xmm1, [rdi]; addsubpd xmm0, xmm1; movupd [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEMisc",
   {{DATA_BASE, cat({packF64(10.0), packF64(20.0), packF64(1.0), packF64(2.0)})}}},
};

// ============================================================================
// SSE Unpack/Interleave
// ============================================================================
static const std::vector<SemTC> kX64SSEUnpack = {
  {"punpcklbw",     "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; punpcklbw xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEUnpack",
   {{DATA_BASE, cat({{1,2,3,4,5,6,7,8,0,0,0,0,0,0,0,0},
                     {11,12,13,14,15,16,17,18,0,0,0,0,0,0,0,0}})}}},
  {"punpckhbw",     "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; punpckhbw xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEUnpack",
   {{DATA_BASE, cat({{0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8},
                     {0,0,0,0,0,0,0,0,11,12,13,14,15,16,17,18}})}}},
  {"punpcklwd",     "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; punpcklwd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEUnpack",
   {{DATA_BASE, cat({packU16(1), packU16(2), packU16(3), packU16(4), zeros(8),
                     packU16(11), packU16(12), packU16(13), packU16(14), zeros(8)})}}},
  {"punpckldq",     "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; punpckldq xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEUnpack",
   {{DATA_BASE, cat({packU32(1), packU32(2), zeros(8),
                     packU32(11), packU32(12), zeros(8)})}}},
  {"punpcklqdq",    "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; punpcklqdq xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEUnpack",
   {{DATA_BASE, cat({packU64(1), zeros(8), packU64(11), zeros(8)})}}},
};

// ============================================================================
// PCmp: PCMPGTB, PCMPGTW, PCMPGTD, PCMPEQB, PCMPEQW
// ============================================================================
static const std::vector<SemTC> kX64PCmp = {
  {"pcmpgtb",       "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pcmpgtb xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "PCmp",
   {{DATA_BASE, cat({{0,10,20,30,40,50,60,70,80,90,100,110,120,130,140,150}, fill(16, 50)})}}},
  {"pcmpgtd",       "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pcmpgtd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "PCmp",
   {{DATA_BASE, cat({packU32(100), packU32(200), packU32(50), packU32(300),
                     packU32(150), packU32(150), packU32(150), packU32(150)})}}},
  {"pcmpeqb",       "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pcmpeqb xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "PCmp",
   {{DATA_BASE, cat({{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                     {0,99,2,99,4,99,6,99,8,99,10,99,12,99,14,99}})}}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(SSEIntArith, X64Semantic,
                         ::testing::ValuesIn(kX64SSEIntArith), semTCName);
INSTANTIATE_TEST_SUITE_P(SSEIntShift, X64Semantic,
                         ::testing::ValuesIn(kX64SSEIntShift), semTCName);
INSTANTIATE_TEST_SUITE_P(SSE4, X64Semantic, ::testing::ValuesIn(kX64SSE4),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(SSEShuffle, X64Semantic,
                         ::testing::ValuesIn(kX64SSEShuffle), semTCName);
INSTANTIATE_TEST_SUITE_P(SSECmp, X64Semantic, ::testing::ValuesIn(kX64SSECmp),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(SSEBitwise, X64Semantic,
                         ::testing::ValuesIn(kX64SSEBitwise), semTCName);
INSTANTIATE_TEST_SUITE_P(SSEMisc, X64Semantic, ::testing::ValuesIn(kX64SSEMisc),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(SSEUnpack, X64Semantic,
                         ::testing::ValuesIn(kX64SSEUnpack), semTCName);
INSTANTIATE_TEST_SUITE_P(PCmp, X64Semantic, ::testing::ValuesIn(kX64PCmp),
                         semTCName);
