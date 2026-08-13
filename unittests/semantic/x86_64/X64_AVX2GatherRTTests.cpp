//===- X64_AVX2GatherRTTests.cpp - AVX2 VSIB gather lift roundtrip ---*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86_64 probes that force clang to emit real AVX2 VSIB gather instructions
// (VPGATHER{DD,DQ,QD,QQ} and VGATHER{DPS,DPD,QPS,QPD}) via the gather builtins
// rather than the scalar-reload fallback an auto-vectorized loop may pick.
//
// These expose that NeverD lifted gather as an opaque INTRINSIC stub (the
// destination received an undefined intrinsic value, never the gathered data)
// and that the unicorn fork only executed 128-bit (VEX.128) gather — see
// the Unicorn unsupported-instructions doc.  Both the native per-lane gather lift and the
// 256-bit (VEX.256) unicorn gen_sse_256 gather are exercised here.
//
// Each kernel builds a table on the stack, gathers through a runtime index
// vector, and folds the result to one integer return compared native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVX2GatherRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVX2GatherRT, Verify) { roundTripX64(GetParam()); }

// Vector type aliases every kernel shares (no system headers under -nostdlib).
static const char *kVecTypes =
    "typedef int __v4si __attribute__((__vector_size__(16)));\n"
    "typedef int __v8si __attribute__((__vector_size__(32)));\n"
    "typedef long long __v2di __attribute__((__vector_size__(16)));\n"
    "typedef long long __v4di __attribute__((__vector_size__(32)));\n"
    "typedef float __v4sf __attribute__((__vector_size__(16)));\n"
    "typedef double __v4df __attribute__((__vector_size__(32)));\n";

static std::string src(const std::string &Body) {
  return std::string(kVecTypes) + Body;
}

// clang-format off
static std::vector<RoundTripTC> makeGatherTC() {
  std::string p = "x64gat";
  return {
    // VPGATHERDD xmm — 4 dword gather, dword indices.
    {p+"_dd128",
     src("unsigned long "+p+"_dd128(unsigned long a){\n"
     "  unsigned t[64]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; t[i]=x; }\n"
     "  __v4si vi={(int)(a&63),(int)((a>>3)&63),(int)((a>>6)&63),(int)((a>>9)&63)};\n"
     "  __v4si m={-1,-1,-1,-1}; __v4si s={0,0,0,0};\n"
     "  __v4si g=__builtin_ia32_gatherd_d(s,(const int*)t,vi,m,4);\n"
     "  return (unsigned long)(unsigned)((unsigned)g[0]+(unsigned)g[1]+(unsigned)g[2]+(unsigned)g[3]); }\n"),
     {0x1234ULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERDD ymm — 8 dword gather, dword indices (256-bit).
    {p+"_dd256",
     src("unsigned long "+p+"_dd256(unsigned long a){\n"
     "  unsigned t[64]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; t[i]=x; }\n"
     "  __v8si vi; for(int i=0;i<8;i++) vi[i]=(int)((a>>(i*2))&63);\n"
     "  __v8si m={-1,-1,-1,-1,-1,-1,-1,-1}; __v8si s={0,0,0,0,0,0,0,0};\n"
     "  __v8si g=__builtin_ia32_gatherd_d256(s,(const int*)t,vi,m,4);\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r+=(unsigned)g[i];\n"
     "  return (unsigned long)(unsigned)r; }\n"),
     {0xACE12345ULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERQQ xmm — 2 qword gather, qword indices.
    {p+"_qq128",
     src("unsigned long "+p+"_qq128(unsigned long a){\n"
     "  unsigned long long t[64]; unsigned long long x=a|1ull;\n"
     "  for(int i=0;i<64;i++){ x=x*6364136223846793005ull+1442695040888963407ull; t[i]=x; }\n"
     "  __v2di vi={(long long)(a&63),(long long)((a>>6)&63)};\n"
     "  __v2di m={-1,-1}; __v2di s={0,0};\n"
     "  __v2di g=__builtin_ia32_gatherq_q(s,(const long long*)t,vi,m,8);\n"
     "  return (unsigned long)((unsigned long long)g[0]+(unsigned long long)g[1]); }\n"),
     {0x9876ULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERQQ ymm — 4 qword gather, qword indices (256-bit).
    {p+"_qq256",
     src("unsigned long "+p+"_qq256(unsigned long a){\n"
     "  unsigned long long t[64]; unsigned long long x=a|1ull;\n"
     "  for(int i=0;i<64;i++){ x=x*6364136223846793005ull+1442695040888963407ull; t[i]=x; }\n"
     "  __v4di vi; for(int i=0;i<4;i++) vi[i]=(long long)((a>>(i*4))&63);\n"
     "  __v4di m={-1,-1,-1,-1}; __v4di s={0,0,0,0};\n"
     "  __v4di g=__builtin_ia32_gatherq_q256(s,(const long long*)t,vi,m,8);\n"
     "  unsigned long long r=0; for(int i=0;i<4;i++) r+=(unsigned long long)g[i];\n"
     "  return (unsigned long)r; }\n"),
     {0xBEEF99ULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERDQ xmm — 2 qword gather, dword indices.
    {p+"_dq128",
     src("unsigned long "+p+"_dq128(unsigned long a){\n"
     "  unsigned long long t[64]; unsigned long long x=a|1ull;\n"
     "  for(int i=0;i<64;i++){ x=x*6364136223846793005ull+1442695040888963407ull; t[i]=x; }\n"
     "  __v4si vi={(int)(a&63),(int)((a>>6)&63),0,0};\n"
     "  __v2di m={-1,-1}; __v2di s={0,0};\n"
     "  __v2di g=__builtin_ia32_gatherd_q(s,(const long long*)t,vi,m,8);\n"
     "  return (unsigned long)((unsigned long long)g[0]+(unsigned long long)g[1]); }\n"),
     {0x55AAULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERDQ ymm — 4 qword gather, dword indices (256-bit dst, xmm index).
    {p+"_dq256",
     src("unsigned long "+p+"_dq256(unsigned long a){\n"
     "  unsigned long long t[64]; unsigned long long x=a|1ull;\n"
     "  for(int i=0;i<64;i++){ x=x*6364136223846793005ull+1442695040888963407ull; t[i]=x; }\n"
     "  __v4si vi={(int)(a&63),(int)((a>>4)&63),(int)((a>>8)&63),(int)((a>>12)&63)};\n"
     "  __v4di m={-1,-1,-1,-1}; __v4di s={0,0,0,0};\n"
     "  __v4di g=__builtin_ia32_gatherd_q256(s,(const long long*)t,vi,m,8);\n"
     "  unsigned long long r=0; for(int i=0;i<4;i++) r+=(unsigned long long)g[i];\n"
     "  return (unsigned long)r; }\n"),
     {0x13579ULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERQD xmm — 2 dword gather, qword indices.
    {p+"_qd128",
     src("unsigned long "+p+"_qd128(unsigned long a){\n"
     "  unsigned t[64]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; t[i]=x; }\n"
     "  __v2di vi={(long long)(a&63),(long long)((a>>6)&63)};\n"
     "  __v4si m={-1,-1,0,0}; __v4si s={0,0,0,0};\n"
     "  __v4si g=__builtin_ia32_gatherq_d(s,(const int*)t,vi,m,4);\n"
     "  return (unsigned long)(unsigned)((unsigned)g[0]+(unsigned)g[1]); }\n"),
     {0x2468ULL}, "AVX2Gather", 2, "-mavx2"},

    // VPGATHERQD with ymm qword indices — 4 dword gather to xmm dst (256-bit).
    {p+"_qd256",
     src("unsigned long "+p+"_qd256(unsigned long a){\n"
     "  unsigned t[64]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; t[i]=x; }\n"
     "  __v4di vi; for(int i=0;i<4;i++) vi[i]=(long long)((a>>(i*4))&63);\n"
     "  __v4si m={-1,-1,-1,-1}; __v4si s={0,0,0,0};\n"
     "  __v4si g=__builtin_ia32_gatherq_d256(s,(const int*)t,vi,m,4);\n"
     "  return (unsigned long)(unsigned)((unsigned)g[0]+(unsigned)g[1]+(unsigned)g[2]+(unsigned)g[3]); }\n"),
     {0x97531ULL}, "AVX2Gather", 2, "-mavx2"},

    // VGATHERDPS xmm — 4 single-precision gather (bit-identical to VPGATHERDD).
    {p+"_ps128",
     src("unsigned long "+p+"_ps128(unsigned long a){\n"
     "  unsigned t[64]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; t[i]=x; }\n"
     "  __v4si vi={(int)(a&63),(int)((a>>3)&63),(int)((a>>6)&63),(int)((a>>9)&63)};\n"
     "  __v4sf m=(__v4sf){0,0,0,0}-(__v4sf){0,0,0,0}; __v4sf s={0,0,0,0};\n"
     "  __v4si mi={-1,-1,-1,-1}; __builtin_memcpy(&m,&mi,16);\n"
     "  __v4sf g=__builtin_ia32_gatherd_ps(s,(const float*)t,vi,m,4);\n"
     "  __v4si gi; __builtin_memcpy(&gi,&g,16);\n"
     "  return (unsigned long)(unsigned)((unsigned)gi[0]+(unsigned)gi[1]+(unsigned)gi[2]+(unsigned)gi[3]); }\n"),
     {0x4321ULL}, "AVX2Gather", 2, "-mavx2"},

    // VGATHERDPD ymm — 4 double-precision gather (bit-identical to VPGATHERDQ).
    {p+"_pd256",
     src("unsigned long "+p+"_pd256(unsigned long a){\n"
     "  unsigned long long t[64]; unsigned long long x=a|1ull;\n"
     "  for(int i=0;i<64;i++){ x=x*6364136223846793005ull+1442695040888963407ull; t[i]=x; }\n"
     "  __v4si vi={(int)(a&63),(int)((a>>4)&63),(int)((a>>8)&63),(int)((a>>12)&63)};\n"
     "  __v4df m; __v4di mi={-1,-1,-1,-1}; __builtin_memcpy(&m,&mi,32);\n"
     "  __v4df s={0,0,0,0};\n"
     "  __v4df g=__builtin_ia32_gatherd_pd256(s,(const double*)t,vi,m,8);\n"
     "  __v4di gi; __builtin_memcpy(&gi,&g,32);\n"
     "  unsigned long long r=0; for(int i=0;i<4;i++) r+=(unsigned long long)gi[i];\n"
     "  return (unsigned long)r; }\n"),
     {0x8642ULL}, "AVX2Gather", 2, "-mavx2"},

    // Partial mask — lanes 1,3 keep the src value (sign bit clear), 0,2 gather.
    {p+"_mask",
     src("unsigned long "+p+"_mask(unsigned long a){\n"
     "  unsigned t[64]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ x=x*1103515245u+12345u; t[i]=x; }\n"
     "  __v4si vi={(int)(a&63),(int)((a>>3)&63),(int)((a>>6)&63),(int)((a>>9)&63)};\n"
     "  __v4si m={-1,0,-1,0}; __v4si s={111,222,333,444};\n"
     "  __v4si g=__builtin_ia32_gatherd_d(s,(const int*)t,vi,m,4);\n"
     "  return (unsigned long)(unsigned)((unsigned)g[0]+(unsigned)g[1]+(unsigned)g[2]+(unsigned)g[3]); }\n"),
     {0x1111ULL}, "AVX2Gather", 2, "-mavx2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kGather = makeGatherTC();
INSTANTIATE_TEST_SUITE_P(AVX2Gather, X64AVX2GatherRT,
                         ::testing::ValuesIn(kGather), rtTCName);
