//===- X64_AVX2Width256RTTests.cpp - element-wise 256-bit (YMM) AVX2 ----*-C++*-=//
//
// x86_64-only probes that force element-wise 256-bit YMM AVX2 instructions via
// GCC vector extensions (vector_size(32)) at -O3 -mavx2, then fold each vector
// to a single integer for bit-exact original-vs-lifted comparison.  These are
// the green guardrail for the #432 256-bit work: the unicorn-fork lane-wise
// execution path (gen_sse_256) and NeverD's 32-byte YMM register model across
// i8/i16/i32/i64 lanes (add/sub/and/or/xor/min/max/abs) reduced via the
// vextracti128 fold tail.
//
// The broader AVX2-256 frontier surfaced by the wider sweep (per-element
// variable shifts VPSLLVD/VPSRLVD/VPSRAVD, cross-lane VPERMQ/VPERM2I128/VPERMD,
// 256-bit VPBROADCAST*, and the vectorized i32/byte horizontal-sum reductions
// VPMULLD+fold / VPSADBW) is root-caused and recorded as a follow-up item in
// the Unicorn unsupported-instructions doc (#432); the element-wise core is fully working here.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVX2Width256RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVX2Width256RT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeWidth256TC() {
  std::string p = "x64w256";
  return {
    // i32 x8: bitwise and/or/xor/andnot -> VPAND/VPOR/VPXOR ymm.
    {p+"_i32logic",
     "long "+p+"_i32logic(long a){\n"
     "  typedef unsigned v8u __attribute__((vector_size(32)));\n"
     "  v8u x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=s;\n"
     "    s=s*1103515245u+12345u; y[i]=s; }\n"
     "  v8u z=((x&y)|(x^y))&(~y|x);\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r^=z[i];\n"
     "  return (long)r; }\n",
     {0x9bULL}, "AVX2W256", 3, "-mavx2"},

    // i32 x8: signed min/max via select -> VPMINSD/VPMAXSD ymm.
    {p+"_i32minmax",
     "long "+p+"_i32minmax(long a){\n"
     "  int x[8],y[8]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(int)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(int)s; }\n"
     "  int r=0;\n"
     "  for(int i=0;i<8;i++){ int mn=x[i]<y[i]?x[i]:y[i]; int mx=x[i]>y[i]?x[i]:y[i];\n"
     "    r+=mx-mn; }\n"
     "  return (long)(unsigned)r; }\n",
     {0x35ULL}, "AVX2W256", 3, "-mavx2"},

    // i64 x4: add/sub/xor -> VPADDQ/VPSUBQ/VPXOR ymm with i64 lanes.
    {p+"_i64addsub",
     "long "+p+"_i64addsub(long a){\n"
     "  typedef long v4l __attribute__((vector_size(32)));\n"
     "  v4l x,y; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul; x[i]=(long)s;\n"
     "    s=s*6364136223846793005ul+1442695040888963407ul; y[i]=(long)s; }\n"
     "  v4l z=(x+y)-(x^y);\n"
     "  long r=0; for(int i=0;i<4;i++) r+=z[i];\n"
     "  return r; }\n",
     {0x77ULL}, "AVX2W256", 3, "-mavx2"},

    // i32 x8: abs of a difference, scalar-reduced -> VPABSD ymm.
    {p+"_i32absdiff",
     "long "+p+"_i32absdiff(long a){\n"
     "  int x[8],y[8]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(int)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(int)s; }\n"
     "  unsigned r=0;\n"
     "  for(int i=0;i<8;i++){ int d=x[i]-y[i]; unsigned u=d<0?(unsigned)(-d):(unsigned)d; r+=u; }\n"
     "  return (long)r; }\n",
     {0x88ULL}, "AVX2W256", 3, "-mavx2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kW256 = makeWidth256TC();
INSTANTIATE_TEST_SUITE_P(AVX2W256, X64AVX2Width256RT,
                         ::testing::ValuesIn(kW256), rtTCName);

// clang-format off
// Probes for the exact 256-bit helpers touched by the ops_sse.h per-lane store
// fix: horizontal add/sub, pack, unpack, pshufb, palignr, mpsadbw at ymm width.
// Each forces the target instruction via a __builtin_ia32_*256 and folds the
// 256-bit result to one integer for bit-exact original-vs-lifted comparison.
static std::vector<RoundTripTC> makeDiffProbeTC() {
  std::string p = "x64w256p";
  return {
    // VPHADDW ymm: per-128-lane horizontal add of 16 words.
    {p+"_phaddw",
     "long "+p+"_phaddw(long a){\n"
     "  typedef short v16h __attribute__((vector_size(32)));\n"
     "  v16h x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; x[i]=(short)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(short)s; }\n"
     "  v16h z=__builtin_ia32_phaddw256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<16;i++) r=r*31u+(unsigned short)z[i];\n"
     "  return (long)r; }\n",
     {0x111ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPHADDD ymm: per-128-lane horizontal add of 8 dwords.
    {p+"_phaddd",
     "long "+p+"_phaddd(long a){\n"
     "  typedef int v8i __attribute__((vector_size(32)));\n"
     "  v8i x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(int)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(int)s; }\n"
     "  v8i z=__builtin_ia32_phaddd256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<8;i++) r=r*31u+(unsigned)z[i];\n"
     "  return (long)r; }\n",
     {0x222ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPHADDSW ymm: saturating per-128-lane horizontal add of 16 words.
    {p+"_phaddsw",
     "long "+p+"_phaddsw(long a){\n"
     "  typedef short v16h __attribute__((vector_size(32)));\n"
     "  v16h x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; x[i]=(short)(s|0x4000);\n"
     "    s=s*1103515245u+12345u; y[i]=(short)(s|0x4000); }\n"
     "  v16h z=__builtin_ia32_phaddsw256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<16;i++) r=r*31u+(unsigned short)z[i];\n"
     "  return (long)r; }\n",
     {0x333ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VHADDPS ymm: per-128-lane horizontal FP add of 8 floats (integer-valued
    // floats keep the adds exact and deterministic).
    {p+"_haddps",
     "long "+p+"_haddps(long a){\n"
     "  typedef float v8f __attribute__((vector_size(32)));\n"
     "  union{unsigned u; float f;} cv; v8f x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(float)((int)(s&0x3ffff)-131072);\n"
     "    s=s*1103515245u+12345u; y[i]=(float)((int)(s&0x3ffff)-131072); }\n"
     "  v8f z=__builtin_ia32_haddps256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<8;i++){ cv.f=z[i]; r=r*31u+cv.u; }\n"
     "  return (long)r; }\n",
     {0x444ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VHADDPD ymm: per-128-lane horizontal FP add of 4 doubles.
    {p+"_haddpd",
     "long "+p+"_haddpd(long a){\n"
     "  typedef double v4d __attribute__((vector_size(32)));\n"
     "  union{unsigned long u; double f;} cv; v4d x,y; unsigned long s=(unsigned long)a|1u;\n"
     "  for(int i=0;i<4;i++){ s=s*6364136223846793005ul+1442695040888963407ul;\n"
     "    x[i]=(double)((long)(s%1000000)-500000);\n"
     "    s=s*6364136223846793005ul+1442695040888963407ul;\n"
     "    y[i]=(double)((long)(s%1000000)-500000); }\n"
     "  v4d z=__builtin_ia32_haddpd256(x,y);\n"
     "  unsigned long r=0; for(int i=0;i<4;i++){ cv.f=z[i]; r=r*1000003ul+cv.u; }\n"
     "  return (long)r; }\n",
     {0x555ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VHSUBPS ymm.
    {p+"_hsubps",
     "long "+p+"_hsubps(long a){\n"
     "  typedef float v8f __attribute__((vector_size(32)));\n"
     "  union{unsigned u; float f;} cv; v8f x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(float)((int)(s&0x3ffff)-131072);\n"
     "    s=s*1103515245u+12345u; y[i]=(float)((int)(s&0x3ffff)-131072); }\n"
     "  v8f z=__builtin_ia32_hsubps256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<8;i++){ cv.f=z[i]; r=r*31u+cv.u; }\n"
     "  return (long)r; }\n",
     {0x666ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VADDSUBPS ymm: full-width per-element alternating add/sub.
    {p+"_addsubps",
     "long "+p+"_addsubps(long a){\n"
     "  typedef float v8f __attribute__((vector_size(32)));\n"
     "  union{unsigned u; float f;} cv; v8f x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(float)((int)(s&0x3ffff)-131072);\n"
     "    s=s*1103515245u+12345u; y[i]=(float)((int)(s&0x3ffff)-131072); }\n"
     "  v8f z=__builtin_ia32_addsubps256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<8;i++){ cv.f=z[i]; r=r*31u+cv.u; }\n"
     "  return (long)r; }\n",
     {0x777ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPACKSSDW ymm: per-128-lane signed dword->word saturate pack.
    {p+"_packssdw",
     "long "+p+"_packssdw(long a){\n"
     "  typedef int v8i __attribute__((vector_size(32)));\n"
     "  typedef short v16h __attribute__((vector_size(32)));\n"
     "  v8i x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(int)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(int)s; }\n"
     "  v16h z=(v16h)__builtin_ia32_packssdw256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<16;i++) r=r*31u+(unsigned short)z[i];\n"
     "  return (long)r; }\n",
     {0x888ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPACKSSWB ymm: per-128-lane signed word->byte saturate pack.
    {p+"_packsswb",
     "long "+p+"_packsswb(long a){\n"
     "  typedef short v16h __attribute__((vector_size(32)));\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  v16h x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; x[i]=(short)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(short)s; }\n"
     "  v32c z=(v32c)__builtin_ia32_packsswb256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<32;i++) r=r*31u+(unsigned char)z[i];\n"
     "  return (long)r; }\n",
     {0x999ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPACKUSWB ymm: per-128-lane unsigned word->byte saturate pack.
    {p+"_packuswb",
     "long "+p+"_packuswb(long a){\n"
     "  typedef short v16h __attribute__((vector_size(32)));\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  v16h x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; x[i]=(short)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(short)s; }\n"
     "  v32c z=(v32c)__builtin_ia32_packuswb256(x,y);\n"
     "  unsigned r=0; for(int i=0;i<32;i++) r=r*31u+(unsigned char)z[i];\n"
     "  return (long)r; }\n",
     {0xaaaULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPSHUFB ymm: per-128-lane byte shuffle by an in-lane index vector.
    {p+"_pshufb",
     "long "+p+"_pshufb(long a){\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  v32c x,idx; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; x[i]=(char)s;\n"
     "    s=s*1103515245u+12345u; idx[i]=(char)s; }\n"
     "  v32c z=(v32c)__builtin_ia32_pshufb256((v32c)x,(v32c)idx);\n"
     "  unsigned r=0; for(int i=0;i<32;i++) r=r*31u+(unsigned char)z[i];\n"
     "  return (long)r; }\n",
     {0xbbbULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPALIGNR ymm: per-128-lane byte align from concat(src1,src2)>>imm.
    {p+"_palignr",
     "long "+p+"_palignr(long a){\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  typedef long long v4q __attribute__((vector_size(32)));\n"
     "  v32c x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; x[i]=(char)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(char)s; }\n"
     "  v32c z=(v32c)__builtin_ia32_palignr256((v4q)x,(v4q)y,5*8);\n"
     "  unsigned r=0; for(int i=0;i<32;i++) r=r*31u+(unsigned char)z[i];\n"
     "  return (long)r; }\n",
     {0xcccULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VMPSADBW ymm: the two 128-bit lanes use DIFFERENT imm sub-fields
    // (low lane imm[2:0]=5, high lane imm[5:3]=4 for imm 0x25).
    {p+"_mpsadbw",
     "long "+p+"_mpsadbw(long a){\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  typedef short v16h __attribute__((vector_size(32)));\n"
     "  v32c x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; x[i]=(char)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(char)s; }\n"
     "  v16h z=(v16h)__builtin_ia32_mpsadbw256((v32c)x,(v32c)y,0x25);\n"
     "  unsigned r=0; for(int i=0;i<16;i++) r=r*31u+(unsigned short)z[i];\n"
     "  return (long)r; }\n",
     {0xdddULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPSADBW ymm: four independent 8-byte sum-of-absolute-differences, one per
    // qword lane (Q0=bytes0..7, Q1=8..15, Q2=16..23, Q3=24..31).  Folding ALL
    // FOUR qwords catches a lift that drops the high 128-bit lane (Q2/Q3).
    {p+"_psadbw",
     "long "+p+"_psadbw(long a){\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  typedef long long v4q __attribute__((vector_size(32)));\n"
     "  v32c x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; x[i]=(char)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(char)s; }\n"
     "  v4q z=(v4q)__builtin_ia32_psadbw256((v32c)x,(v32c)y);\n"
     "  unsigned long r=0; for(int i=0;i<4;i++) r=r*1000003ul+(unsigned long)z[i];\n"
     "  return (long)r; }\n",
     {0x1010ULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VPUNPCKHBW / VPUNPCKLBW ymm: per-128-lane byte interleave.
    {p+"_unpckbw",
     "long "+p+"_unpckbw(long a){\n"
     "  typedef char v32c __attribute__((vector_size(32)));\n"
     "  v32c x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ s=s*1103515245u+12345u; x[i]=(char)s;\n"
     "    s=s*1103515245u+12345u; y[i]=(char)s; }\n"
     "  v32c lo=__builtin_shufflevector(x,y,0,32,1,33,2,34,3,35,4,36,5,37,6,38,7,39,\n"
     "     16,48,17,49,18,50,19,51,20,52,21,53,22,54,23,55);\n"
     "  v32c hi=__builtin_shufflevector(x,y,8,40,9,41,10,42,11,43,12,44,13,45,14,46,15,47,\n"
     "     24,56,25,57,26,58,27,59,28,60,29,61,30,62,31,63);\n"
     "  v32c z=lo^hi;\n"
     "  unsigned r=0; for(int i=0;i<32;i++) r=r*31u+(unsigned char)z[i];\n"
     "  return (long)r; }\n",
     {0xeeeULL}, "AVX2W256Probe", 3, "-mavx2"},

    // VSHUFPS ymm: per-128-lane 4-dword shuffle by imm.
    {p+"_shufps",
     "long "+p+"_shufps(long a){\n"
     "  typedef float v8f __attribute__((vector_size(32)));\n"
     "  union{unsigned u; float f;} cv; v8f x,y; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<8;i++){ s=s*1103515245u+12345u; x[i]=(float)((int)(s&0x3ffff)-131072);\n"
     "    s=s*1103515245u+12345u; y[i]=(float)((int)(s&0x3ffff)-131072); }\n"
     "  v8f z=__builtin_shufflevector(x,y,2,0,9,11, 6,4,13,15);\n"
     "  unsigned r=0; for(int i=0;i<8;i++){ cv.f=z[i]; r=r*31u+cv.u; }\n"
     "  return (long)r; }\n",
     {0xfffULL}, "AVX2W256Probe", 3, "-mavx2"},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kW256Probe = makeDiffProbeTC();
INSTANTIATE_TEST_SUITE_P(AVX2W256Probe, X64AVX2Width256RT,
                         ::testing::ValuesIn(kW256Probe), rtTCName);
