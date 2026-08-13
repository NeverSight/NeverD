//===- X64_FPSemanticTests.cpp - x64 FPU/SSE/AVX tests ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Migrated from scripts/lift_verifier.py — x87 FPU, scalar SSE/AVX and the
// basic packed SSE/AVX suites. The packed-integer, SSE4, shuffle, compare and
// PCMP suites live in X64_FPSemanticSSEExtTests.cpp.
//
//===----------------------------------------------------------------------===//

#include "UnicornSemanticFixture.h"

TEST_P(X64Semantic, FPVerify) { runX64(GetParam()); }

// clang-format off

// ============================================================================
// x87 FPU
// ============================================================================
static const std::vector<SemTC> kX64FPU = {
  {"fld_fst",       "fld qword ptr [rsi]; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPU",
   {{DATA_BASE, packF64(3.14)}}},
  {"fild_fistp",    "fild dword ptr [rsi]; fistp dword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPU",
   {{DATA_BASE, packI32(42)}}},
  {"fadd_mem",      "fld qword ptr [rsi]; fld qword ptr [rdi]; faddp",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 8}}, {}, "FPU",
   {{DATA_BASE, cat({packF64(1.0), packF64(2.0)})}}},
  {"fsub_mem",      "fld qword ptr [rsi]; fld qword ptr [rdi]; fsubp",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 8}}, {}, "FPU",
   {{DATA_BASE, cat({packF64(10.0), packF64(3.0)})}}},
  {"fmul_mem",      "fld qword ptr [rsi]; fld qword ptr [rdi]; fmulp",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 8}}, {}, "FPU",
   {{DATA_BASE, cat({packF64(2.0), packF64(3.0)})}}},
  {"fdiv_mem",      "fld qword ptr [rsi]; fld qword ptr [rdi]; fdivp",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 8}}, {}, "FPU",
   {{DATA_BASE, cat({packF64(10.0), packF64(2.0)})}}},
  {"fchs",          "fld qword ptr [rsi]; fchs; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPU",
   {{DATA_BASE, packF64(5.0)}}},
  {"fabs",          "fld qword ptr [rsi]; fabs; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPU",
   {{DATA_BASE, packF64(-5.0)}}},
  {"fxch",          "fld qword ptr [rsi]; fld qword ptr [rdi]; fxch; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 8}}, {}, "FPU",
   {{DATA_BASE, cat({packF64(1.0), packF64(2.0)})}}},
  {"fnstcw_fldcw",  "fnstcw [rsi]; fldcw [rsi]",
   {{"rsi", DATA_BASE}}, {}, "FPU",
   {{DATA_BASE, cat({packU16(0x037F), zeros(6)})}}},
};

// ============================================================================
// FPU Extra
// ============================================================================
static const std::vector<SemTC> kX64FPUExtra = {
  {"fld_fst_double","fld qword ptr [rsi]; fst qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPUExtra",
   {{DATA_BASE, cat({packF64(3.14), zeros(8)})}}},
  {"fild_int32",    "fild dword ptr [rsi]; fistp dword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPUExtra",
   {{DATA_BASE, cat({packI32(42), zeros(12)})}}},
  {"fabs_extra",    "fld qword ptr [rsi]; fabs; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPUExtra",
   {{DATA_BASE, cat({packF64(-5.0), zeros(8)})}}},
  {"fchs_extra",    "fld qword ptr [rsi]; fchs; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPUExtra",
   {{DATA_BASE, cat({packF64(3.0), zeros(8)})}}},
  {"fsqrt",         "fld qword ptr [rsi]; fsqrt; fstp qword ptr [rdi]",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "FPUExtra",
   {{DATA_BASE, cat({packF64(16.0), zeros(8)})}}},
  {"fldz",          "fldz; fstp qword ptr [rdi]",
   {{"rdi", DATA_BASE}}, {}, "FPUExtra", {}},
  {"fld1",          "fld1; fstp qword ptr [rdi]",
   {{"rdi", DATA_BASE}}, {}, "FPUExtra", {}},
};

// ============================================================================
// SSE scalar
// ============================================================================
static const std::vector<SemTC> kX64SSE = {
  {"movss_load",    "movss xmm0, [rsi]; movss [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(3.14f), zeros(12)})}}},
  {"movsd_load",    "movsd xmm0, [rsi]; movsd [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF64(2.718), zeros(8)})}}},
  {"addss",         "movss xmm0, [rsi]; movss xmm1, [rdi]; addss xmm0, xmm1; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(1.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"subss",         "movss xmm0, [rsi]; movss xmm1, [rdi]; subss xmm0, xmm1; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(5.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"mulss",         "movss xmm0, [rsi]; movss xmm1, [rdi]; mulss xmm0, xmm1; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(3.0f), zeros(12), packF32(4.0f), zeros(12)})}}},
  {"divss",         "movss xmm0, [rsi]; movss xmm1, [rdi]; divss xmm0, xmm1; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(10.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"addsd",         "movsd xmm0, [rsi]; movsd xmm1, [rdi]; addsd xmm0, xmm1; movsd [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF64(1.0), zeros(8), packF64(2.0), zeros(8)})}}},
  {"xorps_zero",    "xorps xmm0, xmm0; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(99.0f), zeros(12)})}}},
  {"pxor_zero",     "pxor xmm0, xmm0; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "SSE",
   {{DATA_BASE, fill(16, 0xFF)}}},
  {"cvtsi2ss",      "cvtsi2ss xmm0, rax; movss [rsi], xmm0",
   {{"rax", 42}, {"rsi", DATA_BASE}}, {}, "SSE",
   {{DATA_BASE, zeros(16)}}},
  {"cvtss2si",      "movss xmm0, [rsi]; cvtss2si rax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSE",
   {{DATA_BASE, cat({packF32(42.0f), zeros(12)})}}},
  {"cvtsi2sd",      "cvtsi2sd xmm0, rax; movsd [rsi], xmm0",
   {{"rax", 42}, {"rsi", DATA_BASE}}, {}, "SSE",
   {{DATA_BASE, zeros(16)}}},
  {"cvtsd2si",      "movsd xmm0, [rsi]; cvtsd2si rax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSE",
   {{DATA_BASE, cat({packF64(42.0), zeros(8)})}}},
  {"ucomiss_flags", "movss xmm0, [rsi]; movss xmm1, [rdi]; ucomiss xmm0, xmm1; setz al",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {"rax"}, "SSE",
   {{DATA_BASE, cat({packF32(3.0f), zeros(12), packF32(3.0f), zeros(12)})}}},
  {"minss",         "movss xmm0, [rsi]; movss xmm1, [rdi]; minss xmm0, xmm1; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(5.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"maxss",         "movss xmm0, [rsi]; movss xmm1, [rdi]; maxss xmm0, xmm1; movss [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(5.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"sqrtss",        "movss xmm0, [rsi]; sqrtss xmm0, xmm0; movss [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSE",
   {{DATA_BASE, cat({packF32(16.0f), zeros(12)})}}},
  {"movd_xmm",      "movd xmm0, eax; movd ebx, xmm0",
   {{"rax", 0xDEADBEEF}}, {"rbx"}, "SSE", {}},
  {"movq_xmm",      "movq xmm0, rax; movq rbx, xmm0",
   {{"rax", 0xDEADBEEF12345678ULL}}, {"rbx"}, "SSE", {}},
};

// ============================================================================
// SSE Convert: CVTDQ2PS, CVTPS2DQ, COMISS, COMISD, etc.
// ============================================================================
static const std::vector<SemTC> kX64SSEConv = {
  {"cvtdq2ps",      "movdqu xmm0, [rsi]; cvtdq2ps xmm1, xmm0; movaps [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEConv",
   {{DATA_BASE, cat({packI32(1), packI32(2), packI32(3), packI32(4)})}}},
  {"cvtps2dq",      "movaps xmm0, [rsi]; cvtps2dq xmm1, xmm0; movdqu [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEConv",
   {{DATA_BASE, cat({packF32(1.5f), packF32(2.5f), packF32(3.5f), packF32(4.5f)})}}},
  {"cvttps2dq",     "movaps xmm0, [rsi]; cvttps2dq xmm1, xmm0; movdqu [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEConv",
   {{DATA_BASE, cat({packF32(1.9f), packF32(2.9f), packF32(3.1f), packF32(4.1f)})}}},
  {"cvttss2si",     "movss xmm0, [rsi]; cvttss2si rax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEConv",
   {{DATA_BASE, cat({packF32(42.9f), zeros(12)})}}},
  {"cvttsd2si",     "movsd xmm0, [rsi]; cvttsd2si rax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEConv",
   {{DATA_BASE, cat({packF64(42.9), zeros(8)})}}},
  {"comiss_lt",     "movss xmm0, [rsi]; movss xmm1, [rdi]; comiss xmm0, xmm1; setb al",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {"rax"}, "SSEConv",
   {{DATA_BASE, cat({packF32(1.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"comiss_eq",     "movss xmm0, [rsi]; movss xmm1, [rdi]; comiss xmm0, xmm1; setz al",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {"rax"}, "SSEConv",
   {{DATA_BASE, cat({packF32(3.0f), zeros(12), packF32(3.0f), zeros(12)})}}},
  {"comisd_lt",     "movsd xmm0, [rsi]; movsd xmm1, [rdi]; comisd xmm0, xmm1; setb al",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {"rax"}, "SSEConv",
   {{DATA_BASE, cat({packF64(1.0), zeros(8), packF64(2.0), zeros(8)})}}},
};

// ============================================================================
// AVX scalar
// ============================================================================
static const std::vector<SemTC> kX64AVX = {
  {"vmovss_load",   "vmovss xmm0, [rsi]; vmovss [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(2.5f), zeros(12)})}}},
  {"vmovsd_load",   "vmovsd xmm0, [rsi]; vmovsd [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "AVX",
   {{DATA_BASE, cat({packF64(1.23), zeros(8)})}}},
  {"vaddss",        "vmovss xmm0, [rsi]; vmovss xmm1, [rdi]; vaddss xmm2, xmm0, xmm1; vmovss [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(1.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"vsubss",        "vmovss xmm0, [rsi]; vmovss xmm1, [rdi]; vsubss xmm2, xmm0, xmm1; vmovss [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(5.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"vmulss",        "vmovss xmm0, [rsi]; vmovss xmm1, [rdi]; vmulss xmm2, xmm0, xmm1; vmovss [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(3.0f), zeros(12), packF32(4.0f), zeros(12)})}}},
  {"vdivss",        "vmovss xmm0, [rsi]; vmovss xmm1, [rdi]; vdivss xmm2, xmm0, xmm1; vmovss [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(10.0f), zeros(12), packF32(2.0f), zeros(12)})}}},
  {"vaddsd",        "vmovsd xmm0, [rsi]; vmovsd xmm1, [rdi]; vaddsd xmm2, xmm0, xmm1; vmovsd [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF64(1.0), zeros(8), packF64(2.0), zeros(8)})}}},
  {"vxorps_zero",   "vxorps xmm0, xmm0, xmm0; vmovss [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(99.0f), zeros(12)})}}},
  {"vpxor_zero",    "vpxor xmm0, xmm0, xmm0; vmovdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}}, {}, "AVX",
   {{DATA_BASE, fill(16, 0xFF)}}},
  {"vcvtsi2ss",     "vcvtsi2ss xmm0, xmm0, rax; vmovss [rsi], xmm0",
   {{"rax", 42}, {"rsi", DATA_BASE}}, {}, "AVX",
   {{DATA_BASE, zeros(16)}}},
  {"vcvtss2si",     "vmovss xmm0, [rsi]; vcvtss2si rax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "AVX",
   {{DATA_BASE, cat({packF32(42.0f), zeros(12)})}}},
  {"vsqrtss",       "vmovss xmm0, [rsi]; vsqrtss xmm1, xmm0, xmm0; vmovss [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF32(25.0f), zeros(12)})}}},
  {"vsqrtsd",       "vmovsd xmm0, [rsi]; vsqrtsd xmm1, xmm0, xmm0; vmovsd [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVX",
   {{DATA_BASE, cat({packF64(49.0), zeros(8)})}}},
  {"vucomiss",      "vmovss xmm0, [rsi]; vmovss xmm1, [rdi]; vucomiss xmm0, xmm1; setz al",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {"rax"}, "AVX",
   {{DATA_BASE, cat({packF32(3.0f), zeros(12), packF32(3.0f), zeros(12)})}}},
};

// ============================================================================
// SSE packed int: PADDB, PSUBB, PCMPEQD, PADDD, PADDQ, PSUBD, PSHUFD, etc.
// ============================================================================
static const std::vector<SemTC> kX64SSEInt = {
  {"paddb",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; paddb xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEInt",
   {{DATA_BASE, cat({fill(16, 0x01), fill(16, 0x02)})}}},
  {"psubb",         "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; psubb xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEInt",
   {{DATA_BASE, cat({fill(16, 0x05), fill(16, 0x02)})}}},
  {"pcmpeqd_eq",    "movdqu xmm0, [rsi]; movdqu xmm1, [rdi]; pcmpeqd xmm0, xmm1; movdqu [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEInt",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4),
                     packU32(1), packU32(0), packU32(3), packU32(0)})}}},
  {"pmovmskb",      "movdqu xmm0, [rsi]; pmovmskb eax, xmm0",
   {{"rsi", DATA_BASE}}, {"rax"}, "SSEInt",
   {{DATA_BASE, {0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00,
                 0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00}}}},
  {"pshufd",        "movdqu xmm0, [rsi]; pshufd xmm1, xmm0, 0x1B; movdqu [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "SSEInt",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4)})}}},
  {"movaps_store",  "movaps xmm0, [rsi]; movaps [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEInt",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f)})}}},
  {"movdqu_load",   "movdqu xmm0, [rsi]; movdqu [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEInt",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4)})}}},
};

// ============================================================================
// SSE packed FP: ADDPS, SUBPS, MULPS, DIVPS, ADDPD, SUBPD
// ============================================================================
static const std::vector<SemTC> kX64SSEPacked = {
  {"addps_packed",  "movaps xmm0, [rsi]; movaps xmm1, [rdi]; addps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEPacked",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(10.0f), packF32(20.0f), packF32(30.0f), packF32(40.0f)})}}},
  {"subps_packed",  "movaps xmm0, [rsi]; movaps xmm1, [rdi]; subps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEPacked",
   {{DATA_BASE, cat({packF32(10.0f), packF32(20.0f), packF32(30.0f), packF32(40.0f),
                     zeros(16),
                     packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f)})}}},
  {"mulps_packed",  "movaps xmm0, [rsi]; movaps xmm1, [rdi]; mulps xmm0, xmm1; movaps [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEPacked",
   {{DATA_BASE, cat({packF32(2.0f), packF32(3.0f), packF32(4.0f), packF32(5.0f),
                     zeros(16),
                     packF32(10.0f), packF32(10.0f), packF32(10.0f), packF32(10.0f)})}}},
  {"addpd_packed",  "movapd xmm0, [rsi]; movapd xmm1, [rdi]; addpd xmm0, xmm1; movapd [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEPacked",
   {{DATA_BASE, cat({packF64(1.0), packF64(2.0), zeros(16), packF64(10.0), packF64(20.0)})}}},
  {"subpd_packed",  "movapd xmm0, [rsi]; movapd xmm1, [rdi]; subpd xmm0, xmm1; movapd [rsi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "SSEPacked",
   {{DATA_BASE, cat({packF64(10.0), packF64(20.0), zeros(16), packF64(1.0), packF64(2.0)})}}},
};

// ============================================================================
// AVX packed integer: VPADDD, VPSUBD, VPAND, VPOR, VPCMPEQD, VPSLLD, VPSRLD
// ============================================================================
static const std::vector<SemTC> kX64AVXInt = {
  {"vpaddd",        "vmovdqu xmm0, [rsi]; vmovdqu xmm1, [rdi]; vpaddd xmm2, xmm0, xmm1; vmovdqu [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4),
                     packU32(10), packU32(20), packU32(30), packU32(40)})}}},
  {"vpsubd",        "vmovdqu xmm0, [rsi]; vmovdqu xmm1, [rdi]; vpsubd xmm2, xmm0, xmm1; vmovdqu [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(10), packU32(20), packU32(30), packU32(40),
                     packU32(1), packU32(2), packU32(3), packU32(4)})}}},
  {"vpand",         "vmovdqu xmm0, [rsi]; vmovdqu xmm1, [rdi]; vpand xmm2, xmm0, xmm1; vmovdqu [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(0xFF), packU32(0xF0), packU32(0x0F), packU32(0x00),
                     packU32(0x0F), packU32(0x0F), packU32(0xFF), packU32(0xFF)})}}},
  {"vpor",          "vmovdqu xmm0, [rsi]; vmovdqu xmm1, [rdi]; vpor xmm2, xmm0, xmm1; vmovdqu [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(0xF0), packU32(0), packU32(0x0F), packU32(0),
                     packU32(0x0F), packU32(0x0F), packU32(0), packU32(0x0F)})}}},
  {"vpcmpeqd",      "vmovdqu xmm0, [rsi]; vmovdqu xmm1, [rdi]; vpcmpeqd xmm2, xmm0, xmm1; vmovdqu [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4),
                     packU32(1), packU32(0), packU32(3), packU32(0)})}}},
  {"vpslld",        "vmovdqu xmm0, [rsi]; vpslld xmm1, xmm0, 4; vmovdqu [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4)})}}},
  {"vpsrld",        "vmovdqu xmm0, [rsi]; vpsrld xmm1, xmm0, 4; vmovdqu [rdi], xmm1",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXInt",
   {{DATA_BASE, cat({packU32(16), packU32(32), packU32(64), packU32(128)})}}},
};

// ============================================================================
// AVX packed FP: VADDPS, VSUBPS, VMULPS, etc.
// ============================================================================
static const std::vector<SemTC> kX64AVXPacked = {
  {"vaddps_packed", "vmovaps xmm0, [rsi]; vmovaps xmm1, [rdi]; vaddps xmm2, xmm0, xmm1; vmovaps [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "AVXPacked",
   {{DATA_BASE, cat({packF32(1.0f), packF32(2.0f), packF32(3.0f), packF32(4.0f),
                     zeros(16),
                     packF32(10.0f), packF32(20.0f), packF32(30.0f), packF32(40.0f)})}}},
  {"vsubsd",        "vmovsd xmm0, [rsi]; vmovsd xmm1, [rdi]; vsubsd xmm2, xmm0, xmm1; vmovsd [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXPacked",
   {{DATA_BASE, cat({packF64(10.0), zeros(8), packF64(3.0), zeros(8)})}}},
  {"vmulsd",        "vmovsd xmm0, [rsi]; vmovsd xmm1, [rdi]; vmulsd xmm2, xmm0, xmm1; vmovsd [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXPacked",
   {{DATA_BASE, cat({packF64(2.0), zeros(8), packF64(3.0), zeros(8)})}}},
  {"vdivsd",        "vmovsd xmm0, [rsi]; vmovsd xmm1, [rdi]; vdivsd xmm2, xmm0, xmm1; vmovsd [rsi], xmm2",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 16}}, {}, "AVXPacked",
   {{DATA_BASE, cat({packF64(10.0), zeros(8), packF64(2.0), zeros(8)})}}},
  {"vmovdqu_load",  "vmovdqu xmm0, [rsi]; vmovdqu [rdi], xmm0",
   {{"rsi", DATA_BASE}, {"rdi", DATA_BASE + 32}}, {}, "AVXPacked",
   {{DATA_BASE, cat({packU32(1), packU32(2), packU32(3), packU32(4)})}}},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(FPU, X64Semantic, ::testing::ValuesIn(kX64FPU),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(FPUExtra, X64Semantic,
                         ::testing::ValuesIn(kX64FPUExtra), semTCName);
INSTANTIATE_TEST_SUITE_P(SSE, X64Semantic, ::testing::ValuesIn(kX64SSE),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(SSEConv, X64Semantic, ::testing::ValuesIn(kX64SSEConv),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(AVX, X64Semantic, ::testing::ValuesIn(kX64AVX),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(SSEInt, X64Semantic, ::testing::ValuesIn(kX64SSEInt),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(SSEPacked, X64Semantic,
                         ::testing::ValuesIn(kX64SSEPacked), semTCName);
INSTANTIATE_TEST_SUITE_P(AVXInt, X64Semantic, ::testing::ValuesIn(kX64AVXInt),
                         semTCName);
INSTANTIATE_TEST_SUITE_P(AVXPacked, X64Semantic,
                         ::testing::ValuesIn(kX64AVXPacked), semTCName);
