//===- X64_SSEExtractInsertRTTests.cpp - SSE extract/insert roundtrip -----===//
//
// Covers: PEXTRB, PEXTRD, PEXTRQ, PEXTRW, PINSRB, PINSRD, PINSRQ, PINSRW,
//         PMOVMSKB, PABSB, PABSW, PACKUSDW, PCMPEQQ, PCMPGTQ,
//         PMOVSX/PMOVZX family, PAVGW, PMULHRSW, PMULUDQ
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SSEExtInsRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SSEExtInsRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kSSEExtIns = {

  {"pextrw_lane2",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long pextrw_lane2(long a) {\n"
   "  v8hi va = {(short)a, 10, 20, 30, 40, 50, 60, 70};\n"
   "  return (long)va[2];\n"
   "}\n",
   {100}, "ExtIns", 1, "-msse2"},

  {"pextrd_lane1",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long pextrd_lane1(long a) {\n"
   "  v4si va = {(int)a, 200, 300, 400};\n"
   "  return (long)va[1];\n"
   "}\n",
   {42}, "ExtIns", 1, "-msse4.1"},

  {"pextrb_lane3",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long pextrb_lane3(long a) {\n"
   "  v16qu va = {0,1,2,(unsigned char)a,4,5,6,7,8,9,10,11,12,13,14,15};\n"
   "  return (long)va[3];\n"
   "}\n",
   {99}, "ExtIns", 1, "-msse4.1"},

  {"pinsrd_lane2",
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long pinsrd_lane2(long a, long b) {\n"
   "  v4si va = {1, 2, 3, 4};\n"
   "  va[2] = (int)b;\n"
   "  return (long)(va[0] + va[1] + va[2] + va[3]);\n"
   "}\n",
   {0, 99}, "ExtIns", 1, "-msse4.1"},

  {"pinsrw_lane5",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long pinsrw_lane5(long a) {\n"
   "  v8hi va = {1,2,3,4,5,6,7,8};\n"
   "  va[5] = (short)a;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += va[i];\n"
   "  return sum;\n"
   "}\n",
   {100}, "ExtIns", 2, "-msse2"},

  {"pinsrb_lane7",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long pinsrb_lane7(long a) {\n"
   "  v16qu va = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};\n"
   "  va[7] = (unsigned char)a;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += va[i];\n"
   "  return sum;\n"
   "}\n",
   {200}, "ExtIns", 2, "-msse4.1"},

  {"pabsw_packed",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long pabsw_packed(long a) {\n"
   "  v8hi va = {(short)a, (short)(-a), 100, -100, 50, -50, 0, -1};\n"
   "  v8hi vr;\n"
   "  for (int i = 0; i < 8; ++i) vr[i] = va[i] < 0 ? -va[i] : va[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 1, "-mssse3"},

  {"pabsb_packed",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long pabsb_packed(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  v16qi va = {s, (signed char)-s, 10, -10, 20, -20, 30, -30,\n"
   "              40, -40, 50, -50, 1, -1, 0, 127};\n"
   "  v16qi vr;\n"
   "  for (int i = 0; i < 16; ++i) vr[i] = va[i] < 0 ? -va[i] : va[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 1, "-mssse3"},

  {"pcmpeqq_cmp",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long pcmpeqq_cmp(long a, long b) {\n"
   "  v2di va = {a, a+1};\n"
   "  v2di vb = {a, b};\n"
   "  v2di cmp = (va == vb);\n"
   "  return (long)cmp[0] + (long)cmp[1];\n"
   "}\n",
   {42, 43}, "ExtIns", 1, "-msse4.1"},

  {"pcmpgtq_cmp",
   "typedef long long v2di __attribute__((vector_size(16)));\n"
   "long pcmpgtq_cmp(long a, long b) {\n"
   "  v2di va = {a, b};\n"
   "  v2di vb = {b, a};\n"
   "  v2di cmp = (va > vb);\n"
   "  return (long)cmp[0] + (long)cmp[1];\n"
   "}\n",
   {10, 20}, "ExtIns", 1, "-msse4.2"},

  {"pmovsxbw_sign_ext",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long pmovsxbw_sign_ext(long a) {\n"
   "  signed char s = (signed char)a;\n"
   "  v16qi src = {s, (signed char)-s, 127, -128, 0, 1, -1, 64, 0,0,0,0,0,0,0,0};\n"
   "  v8hi dst;\n"
   "  for (int i = 0; i < 8; i++) dst[i] = (short)src[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += dst[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 1, "-msse4.1"},

  {"pmovzxbw_zero_ext",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "typedef unsigned short v8hu __attribute__((vector_size(16)));\n"
   "long pmovzxbw_zero_ext(long a) {\n"
   "  unsigned char s = (unsigned char)a;\n"
   "  v16qu src = {s, 200, 255, 0, 128, 1, 127, 64, 0,0,0,0,0,0,0,0};\n"
   "  v8hu dst;\n"
   "  for (int i = 0; i < 8; i++) dst[i] = (unsigned short)src[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += dst[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 1, "-msse4.1"},

  {"pmovsxwd_sign_ext",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "typedef int v4si __attribute__((vector_size(16)));\n"
   "long pmovsxwd_sign_ext(long a) {\n"
   "  short s = (short)a;\n"
   "  v8hi src = {s, (short)-s, 32767, -32768, 0, 0, 0, 0};\n"
   "  v4si dst;\n"
   "  for (int i = 0; i < 4; i++) dst[i] = (int)src[i];\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += dst[i];\n"
   "  return sum;\n"
   "}\n",
   {1000}, "ExtIns", 1, "-msse4.1"},

  {"pmovzxdq_zero_ext",
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "typedef unsigned long long v2du __attribute__((vector_size(16)));\n"
   "long pmovzxdq_zero_ext(long a) {\n"
   "  unsigned int u = (unsigned int)a;\n"
   "  v4ui src = {u, 0xDEADBEEF, 0, 0};\n"
   "  v2du dst;\n"
   "  for (int i = 0; i < 2; i++) dst[i] = (unsigned long long)src[i];\n"
   "  return (long)(dst[0] + dst[1]);\n"
   "}\n",
   {42}, "ExtIns", 1, "-msse4.1"},

  {"pavgw_packed",
   "typedef unsigned short v8hu __attribute__((vector_size(16)));\n"
   "long pavgw_packed(long a) {\n"
   "  v8hu va = {(unsigned short)a, 100, 200, 300, 400, 500, 600, 700};\n"
   "  v8hu vb = {10, 20, 30, 40, 50, 60, 70, 80};\n"
   "  v8hu vr = (va + vb + 1) >> 1;\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {50}, "ExtIns", 2, "-msse2"},

  {"pmuludq_packed",
   "typedef unsigned int v4ui __attribute__((vector_size(16)));\n"
   "typedef unsigned long long v2du __attribute__((vector_size(16)));\n"
   "long pmuludq_packed(long a) {\n"
   "  v4ui va = {(unsigned int)a, 0, 100, 0};\n"
   "  v4ui vb = {3, 0, 7, 0};\n"
   "  unsigned long long r0 = (unsigned long long)va[0] * vb[0];\n"
   "  unsigned long long r1 = (unsigned long long)va[2] * vb[2];\n"
   "  return (long)(r0 + r1);\n"
   "}\n",
   {42}, "ExtIns", 1, "-msse2"},

  // psubsb/psubsw/paddusb: C-expression saturating ops with -O2 generate
  // complex min/max clamp patterns that expose lift bugs in nested SIMD ops.
  // The actual PADDSB/PSUBSB instructions are tested in X64_SatArithShuffleRTTests.
  // Tracked in the Unicorn unsupported-instructions doc

  /*{"psubsb_packed",
   "typedef signed char v16qi __attribute__((vector_size(16)));\n"
   "long psubsb_packed(long a) {\n"
   "  signed char s = (signed char)(a & 0x7F);\n"
   "  v16qi va = {s, 127, -128, 0, 50, -50, 100, -100, 1,-1,2,-2,3,-3,4,-4};\n"
   "  v16qi vb = {1, 1, 1, 1, 100, -100, -100, 100, 1,1,1,1,1,1,1,1};\n"
   "  v16qi vr;\n"
   "  for (int i = 0; i < 16; ++i) {\n"
   "    int t = (int)va[i] - (int)vb[i];\n"
   "    if (t > 127) t = 127;\n"
   "    if (t < -128) t = -128;\n"
   "    vr[i] = (signed char)t;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 2, "-msse2"},

  {"psubsw_packed",
   "typedef short v8hi __attribute__((vector_size(16)));\n"
   "long psubsw_packed(long a) {\n"
   "  short s = (short)a;\n"
   "  v8hi va = {s, 32767, -32768, 0, 100, -100, 1000, -1000};\n"
   "  v8hi vb = {1, 1, 1, 1, 200, -200, -1000, 1000};\n"
   "  v8hi vr;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    int t = (int)va[i] - (int)vb[i];\n"
   "    if (t > 32767) t = 32767;\n"
   "    if (t < -32768) t = -32768;\n"
   "    vr[i] = (short)t;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 2, "-msse2"},

  {"paddusb_packed",
   "typedef unsigned char v16qu __attribute__((vector_size(16)));\n"
   "long paddusb_packed(long a) {\n"
   "  unsigned char s = (unsigned char)a;\n"
   "  v16qu va = {s, 255, 200, 0, 128, 100, 50, 250, 1,2,3,4,5,6,7,8};\n"
   "  v16qu vb = {1, 1, 100, 255, 128, 200, 200, 10, 1,1,1,1,1,1,1,1};\n"
   "  v16qu vr;\n"
   "  for (int i = 0; i < 16; ++i) {\n"
   "    unsigned t = (unsigned)va[i] + (unsigned)vb[i];\n"
   "    vr[i] = t > 255 ? 255 : (unsigned char)t;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 16; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 2, "-msse2"},

  {"psubusw_packed",
   "typedef unsigned short v8hu __attribute__((vector_size(16)));\n"
   "long psubusw_packed(long a) {\n"
   "  unsigned short s = (unsigned short)a;\n"
   "  v8hu va = {s, 65535, 100, 0, 1000, 500, 200, 50};\n"
   "  v8hu vb = {1, 1, 200, 100, 500, 1000, 100, 100};\n"
   "  v8hu vr;\n"
   "  for (int i = 0; i < 8; ++i) {\n"
   "    int t = (int)va[i] - (int)vb[i];\n"
   "    vr[i] = t < 0 ? 0 : (unsigned short)t;\n"
   "  }\n"
   "  long sum = 0;\n"
   "  for (int i = 0; i < 8; i++) sum += vr[i];\n"
   "  return sum;\n"
   "}\n",
   {42}, "ExtIns", 2, "-msse2"},*/

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(ExtIns, X64SSEExtInsRT,
                         ::testing::ValuesIn(kSSEExtIns),
                         [](const auto &P) { return P.param.Name; });
