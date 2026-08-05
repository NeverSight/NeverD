//===- X86_32_MultiArgRTTests.cpp - i386 multi-arg cdecl recovery -*- C++ -*-=//
//
// i386 cdecl passes every argument on the stack at [esp_entry + 4 + 4*i].  When
// clang -O2 auto-vectorizes a kernel it reaches those arguments through SSE
// broadcasts (`pshufd $0,off(%esp),%xmm`) rather than scalar `mov`, and after
// codegen pushes callee-saved registers the reconstructed entry-SP no longer
// equals the frame top — so a stack arg recovered through the frame instead of
// a real parameter is read at the wrong offset.  These probes pass 2..6 cdecl
// arguments and use them inside vectorizable loops to exercise multi-argument
// stack-parameter recovery (the broadcast-read path) end to end.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86MultiArgRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86MultiArgRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86MA = {
  // 4 args, each broadcast into a vectorized array build + hash reduce.
  {"ma_vec4",
   "int ma_vec4(int a,int b,int c,int d){ int v[16];\n"
   "  for(int i=0;i<16;i++) v[i]=a*i+b*(i^3)+c*(i&5)-d;\n"
   "  unsigned acc=0; for(int i=0;i<16;i++) acc=acc*131u+(unsigned)v[i];\n"
   "  return (int)acc; }\n",
   {0x11,0x22,0x33,0x44}, "X86MultiArg", 2, ""},

  // 6 args (max SysV count; on i386 all six sit on the stack at +4..+24).
  {"ma_vec6",
   "int ma_vec6(int a,int b,int c,int d,int e,int f){ int v[24];\n"
   "  for(int i=0;i<24;i++) v[i]=a+b*i-c*(i&3)+d*(i^1)+e-(f<<(i&7));\n"
   "  unsigned acc=2166136261u; for(int i=0;i<24;i++){ acc^=(unsigned)v[i]; acc*=16777619u; }\n"
   "  return (int)acc; }\n",
   {0x7,0x9,0xB,0xD,0xF,0x11}, "X86MultiArg", 2, ""},

  // Two args feeding a vectorized 64-bit accumulate (register-pair + broadcast).
  {"ma_wide2",
   "int ma_wide2(int a,int b){ unsigned long long acc=0;\n"
   "  for(int i=0;i<32;i++){ unsigned long long h=(unsigned)(a*i+b); h*=0x9E3779B97F4A7C15ULL; acc+=h; }\n"
   "  return (int)(acc^(acc>>32)); }\n",
   {0x1234,0x5678}, "X86MultiArg", 2, ""},

  // Three args, signed min/max/abs cascade (vectorized pcmpgtd/pblend).
  {"ma_minmax3",
   "int ma_minmax3(int a,int b,int c){ int v[20];\n"
   "  for(int i=0;i<20;i++){ int x=a*(i+1)-b*i+c; v[i]=x<0?-x:x; }\n"
   "  int mx=v[0]; for(int i=1;i<20;i++) if(v[i]>mx) mx=v[i];\n"
   "  unsigned acc=0; for(int i=0;i<20;i++) acc=acc*31u+(unsigned)(v[i]^mx);\n"
   "  return (int)acc; }\n",
   {0x41,0x17,0x9}, "X86MultiArg", 2, ""},

  // Five args, mixed shift/mul (later args at higher stack offsets +20).
  {"ma_mix5",
   "int ma_mix5(int a,int b,int c,int d,int e){ unsigned acc=0;\n"
   "  for(int i=0;i<40;i++){ unsigned t=(unsigned)(a+i)*3u + (unsigned)(b<<(i&7))\n"
   "                          + (unsigned)(c*i) - (unsigned)d + ((unsigned)e>>(i&3));\n"
   "    acc=acc*1000003u + (t^(t>>11)); }\n"
   "  return (int)acc; }\n",
   {0x3,0x5,0x7,0x9,0xB}, "X86MultiArg", 2, ""},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86MultiArg, X86MultiArgRT, ::testing::ValuesIn(kX86MA),
                         rtTCName);
