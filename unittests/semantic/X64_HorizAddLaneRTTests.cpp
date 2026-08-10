//===- X64_HorizAddLaneRTTests.cpp - PHADD/PHSUB lane semantics -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 horizontal add/sub (PHADDW/D, PHSUBW/D, PHADDSW/PHSUBSW) reduce ADJACENT
// element pairs, routing src1's reductions to the low half of each 128-bit
// result lane and src2's to the high half — INDEPENDENTLY PER 128-bit lane.
//
// The lifter built the whole result as "all src1 pairs, then all src2 pairs"
// (one BuildHalf(A) + BuildHalf(B) + a single CONCAT).  That is correct for the
// 64-bit MMX and 128-bit xmm forms (a single lane) but WRONG for 256-bit ymm
// (VEX) AVX2 forms: the high 128-bit lane's results land in src2's region
// instead of interleaving each lane's A/B pairs:
//
//   correct ymm: lane0=[A0+A1,A2+A3,B0+B1,B2+B3] lane1=[A4+A5,A6+A7,B4+B5,B6+B7]
//   buggy   ymm: [A0+A1,A2+A3,A4+A5,A6+A7, B0+B1,B2+B3,B4+B5,B6+B7]
//
// Fix: build each 128-bit lane separately (low half from src1's pairs of that
// lane, high half from src2's), so the same loop handles MMX/xmm (NumLanes==1)
// and ymm (NumLanes==2).
//
// The float horizontal forms HADDPS/HADDPD/HSUBPS/HSUBPD and the per-element
// ADDSUBPS/ADDSUBPD shared the identical "only the low 128 bits" defect (their
// lifters hard-coded the 4-float / 2-double layout), so the 256-bit ymm forms
// were truncated/mis-routed too; they get the same per-128-lane (HADD/HSUB) or
// full-width per-element (ADDSUB) rebuild.
//
// The 256-bit forms CANNOT be roundtripped here: the bundled Unicorn fork does
// not decode 256-bit AVX/AVX2 (even a bare `vpaddd ymm` / `vhaddps ymm` is
// UC_ERR_INSN_INVALID), so the *original* program can't be emulated (cf. #341
// maskmov / #342 vpermil).  These 128-bit probes exercise the shared per-lane
// reduction + assembly path the 256-bit fix reuses (128-bit == NumLanes==1), and
// guard against the refactor regressing the (already-correct) narrow forms —
// including the saturating ±overflow arms of PHADDSW/PHSUBSW.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64HorizAddLaneRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64HorizAddLaneRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
#define V8H "typedef short v8h __attribute__((vector_size(16)));\n"
#define V4I "typedef int v4i __attribute__((vector_size(16)));\n"
#define V4F "typedef float v4f __attribute__((vector_size(16)));\n"
#define V2D "typedef double v2d __attribute__((vector_size(16)));\n"

static const std::vector<RoundTripTC> kX64 = {

  // ===== PHADDD (128-bit): res=[A0+A1,A2+A3,B0+B1,B2+B3]. =====
  {"phaddd_128",
   V4I
   "long f(long a, long b){\n"
   "  v4i va={(int)a,(int)(a>>32),(int)a+5,(int)(a>>16)+7};\n"
   "  v4i vb={(int)b,(int)(b>>32),(int)b+5,(int)(b>>16)+7};\n"
   "  v4i vr=__builtin_ia32_phaddd128(va,vb);\n"
   "  return (unsigned long)((unsigned)vr[0]*7u+(unsigned)vr[1]*13u\n"
   "                        +(unsigned)vr[2]*17u+(unsigned)vr[3]*23u);\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== PHSUBD (128-bit): res=[A0-A1,A2-A3,B0-B1,B2-B3]. =====
  {"phsubd_128",
   V4I
   "long f(long a, long b){\n"
   "  v4i va={(int)a,(int)(a>>32),(int)a+5,(int)(a>>16)+7};\n"
   "  v4i vb={(int)b,(int)(b>>32),(int)b+5,(int)(b>>16)+7};\n"
   "  v4i vr=__builtin_ia32_phsubd128(va,vb);\n"
   "  return (unsigned long)((unsigned)vr[0]*7u+(unsigned)vr[1]*13u\n"
   "                        +(unsigned)vr[2]*17u+(unsigned)vr[3]*23u);\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== PHADDW (128-bit): 8 words, [4 A-pairs, 4 B-pairs]. =====
  {"phaddw_128",
   V8H
   "long f(long a, long b){\n"
   "  v8h va={(short)a,(short)(a>>16),(short)(a>>32),(short)(a>>48),\n"
   "          (short)(a+1),(short)((a>>16)+2),(short)((a>>32)+3),(short)((a>>48)+4)};\n"
   "  v8h vb={(short)b,(short)(b>>16),(short)(b>>32),(short)(b>>48),\n"
   "          (short)(b+1),(short)((b>>16)+2),(short)((b>>32)+3),(short)((b>>48)+4)};\n"
   "  v8h vr=__builtin_ia32_phaddw128(va,vb);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+(unsigned short)vr[i];\n"
   "  return (unsigned long)(unsigned)s;\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== PHSUBW (128-bit). =====
  {"phsubw_128",
   V8H
   "long f(long a, long b){\n"
   "  v8h va={(short)a,(short)(a>>16),(short)(a>>32),(short)(a>>48),\n"
   "          (short)(a+1),(short)((a>>16)+2),(short)((a>>32)+3),(short)((a>>48)+4)};\n"
   "  v8h vb={(short)b,(short)(b>>16),(short)(b>>32),(short)(b>>48),\n"
   "          (short)(b+1),(short)((b>>16)+2),(short)((b>>32)+3),(short)((b>>48)+4)};\n"
   "  v8h vr=__builtin_ia32_phsubw128(va,vb);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+(unsigned short)vr[i];\n"
   "  return (unsigned long)(unsigned)s;\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== PHADDSW (128-bit): saturating; hits +ve and -ve overflow arms. =====
  {"phaddsw_128_sat",
   V8H
   "long f(long a, long b){\n"
   "  v8h va={(short)a,(short)(a>>16),0x7000,0x6000,(short)-0x7000,(short)-0x6000,(short)(a>>32),(short)(a>>48)};\n"
   "  v8h vb={(short)b,(short)(b>>16),0x5000,0x5000,(short)-0x5000,(short)-0x5000,(short)(b>>32),(short)(b>>48)};\n"
   "  v8h vr=__builtin_ia32_phaddsw128(va,vb);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+(unsigned short)vr[i];\n"
   "  return (unsigned long)(unsigned)s;\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== PHSUBSW (128-bit): saturating subtract; overflow on both signs. =====
  {"phsubsw_128_sat",
   V8H
   "long f(long a, long b){\n"
   "  v8h va={(short)a,(short)(a>>16),0x7000,(short)-0x6000,(short)-0x7000,0x6000,(short)(a>>32),(short)(a>>48)};\n"
   "  v8h vb={(short)b,(short)(b>>16),(short)-0x4000,0x4000,0x4000,(short)-0x4000,(short)(b>>32),(short)(b>>48)};\n"
   "  v8h vr=__builtin_ia32_phsubsw128(va,vb);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+(unsigned short)vr[i];\n"
   "  return (unsigned long)(unsigned)s;\n"
   "}\n",
   {0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== Mixed sign / carry edges for the non-saturating word form. =====
  {"phaddw_128_wrap",
   V8H
   "long f(long a, long b){\n"
   "  v8h va={(short)0x7FFF,(short)0x7FFF,(short)0x8000,(short)0x8000,\n"
   "          (short)a,(short)b,(short)(a>>16),(short)(b>>16)};\n"
   "  v8h vb={(short)0xFFFF,(short)1,(short)0x4000,(short)0x4000,\n"
   "          (short)(a>>32),(short)(b>>32),(short)(a>>48),(short)(b>>48)};\n"
   "  v8h vr=__builtin_ia32_phaddw128(va,vb);\n"
   "  int s=0; for(int i=0;i<8;i++) s=s*31+(unsigned short)vr[i];\n"
   "  return (unsigned long)(unsigned)s;\n"
   "}\n",
   {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, "HorizAddLane", 1, "-mssse3"},

  // ===== Horizontal FP add/sub (HADDPS/HADDPD/HSUBPS) — same per-128-lane
  //       routing as PHADD; 128-bit forms are SSE3 and DO roundtrip. =====
  {"haddps_128",
   V4F
   "long f(long a, long b){\n"
   "  float fa,fb; __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va={fa,fa*2+1,fa*3+2,fa-4};\n"
   "  v4f vb={fb,fb*2+1,fb*3+2,fb-4};\n"
   "  v4f vr=__builtin_ia32_haddps(va,vb);\n"
   "  unsigned o0,o1,o2,o3; float t;\n"
   "  t=vr[0];__builtin_memcpy(&o0,&t,4); t=vr[1];__builtin_memcpy(&o1,&t,4);\n"
   "  t=vr[2];__builtin_memcpy(&o2,&t,4); t=vr[3];__builtin_memcpy(&o3,&t,4);\n"
   "  return (unsigned long)(o0*7u+o1*13u+o2*17u+o3*23u);\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "HorizAddLane", 1, "-msse3"},

  {"hsubps_128",
   V4F
   "long f(long a, long b){\n"
   "  float fa,fb; __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va={fa,fa*2+1,fa*3+2,fa-4};\n"
   "  v4f vb={fb,fb*2+1,fb*3+2,fb-4};\n"
   "  v4f vr=__builtin_ia32_hsubps(va,vb);\n"
   "  unsigned o0,o1,o2,o3; float t;\n"
   "  t=vr[0];__builtin_memcpy(&o0,&t,4); t=vr[1];__builtin_memcpy(&o1,&t,4);\n"
   "  t=vr[2];__builtin_memcpy(&o2,&t,4); t=vr[3];__builtin_memcpy(&o3,&t,4);\n"
   "  return (unsigned long)(o0*7u+o1*13u+o2*17u+o3*23u);\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "HorizAddLane", 1, "-msse3"},

  {"haddpd_128",
   V2D
   "long f(long a, long b){\n"
   "  double da,db; __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va={da,da*2+1}; v2d vb={db,db*3-2};\n"
   "  v2d vr=__builtin_ia32_haddpd(va,vb);\n"
   "  unsigned long o0,o1; double t;\n"
   "  t=vr[0];__builtin_memcpy(&o0,&t,8); t=vr[1];__builtin_memcpy(&o1,&t,8);\n"
   "  return o0*1000003ul + o1*99u;\n"
   "}\n",
   {0x4010000000000000ULL, 0x4008000000000000ULL}, "HorizAddLane", 1, "-msse3"},

  {"addsubps_128",
   V4F
   "long f(long a, long b){\n"
   "  float fa,fb; __builtin_memcpy(&fa,&a,4); __builtin_memcpy(&fb,&b,4);\n"
   "  v4f va={fa,fa*2+1,fa*3+2,fa-4};\n"
   "  v4f vb={fb,fb*2+1,fb*3+2,fb-4};\n"
   "  v4f vr=__builtin_ia32_addsubps(va,vb);\n"
   "  unsigned o0,o1,o2,o3; float t;\n"
   "  t=vr[0];__builtin_memcpy(&o0,&t,4); t=vr[1];__builtin_memcpy(&o1,&t,4);\n"
   "  t=vr[2];__builtin_memcpy(&o2,&t,4); t=vr[3];__builtin_memcpy(&o3,&t,4);\n"
   "  return (unsigned long)(o0*7u+o1*13u+o2*17u+o3*23u);\n"
   "}\n",
   {0x40A00000ULL, 0x40400000ULL}, "HorizAddLane", 1, "-msse3"},

  {"addsubpd_128",
   V2D
   "long f(long a, long b){\n"
   "  double da,db; __builtin_memcpy(&da,&a,8); __builtin_memcpy(&db,&b,8);\n"
   "  v2d va={da,da*2+1}; v2d vb={db,db*3-2};\n"
   "  v2d vr=__builtin_ia32_addsubpd(va,vb);\n"
   "  unsigned long o0,o1; double t;\n"
   "  t=vr[0];__builtin_memcpy(&o0,&t,8); t=vr[1];__builtin_memcpy(&o1,&t,8);\n"
   "  return o0*1000003ul + o1*99u;\n"
   "}\n",
   {0x4010000000000000ULL, 0x4008000000000000ULL}, "HorizAddLane", 1, "-msse3"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(HorizAddLane, X64HorizAddLaneRT,
                         ::testing::ValuesIn(kX64), rtTCName);
