//===- AllPlatform_OptStress179RTTests.cpp - kth-select / mode / window-min =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * kthsel    - k-th smallest by histogram order-statistic selection: tally a
//                 16-bucket histogram, then walk cumulative counts to the k-th
//                 rank.  Pins a select-by-counting (distinct from the full
//                 counting sort #177).
//   * mode      - most frequent value via histogram argmax.  Pins a frequency
//                 argmax (distinct from the majority vote #174 which is a
//                 streaming candidate, not a true histogram peak).
//   * windowmin - sliding-window minimum via a monotonic index deque.  Pins a
//                 monotonic-deque window scan (distinct from the two-pointer
//                 rain-water #172 and the Kadane/window peak scans).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress179RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress179RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress179RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress179RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress179RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress179RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress179RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress179RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress179TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // k-th smallest by histogram order-statistic selection.
    {p+"_kthsel",
     "static const unsigned char "+p+"_ks[20]={12,3,15,7,1,9,12,4,8,3,15,0,6,11,2,9,14,5,10,7};\n"
     +t+" "+p+"_kthsel("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[16]; for(int i=0;i<16;i++) cnt[i]=0u;\n"
     "    for(int i=0;i<20;i++){ unsigned v=((unsigned)"+p+"_ks[i]^((s>>(i&7))&3u))&15u; cnt[v]++; }\n"
     "    unsigned k=(s>>4)%20u, seen=0u, kth=0u;\n"
     "    for(unsigned v=0;v<16u;v++){ seen+=cnt[v]; if(seen>k){ kth=v; break; } }\n"
     "    acc=acc*131u+kth*16u+k; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x39u}, "OptStress179", 2},

    // most frequent value via histogram argmax.
    {p+"_mode",
     "static const unsigned char "+p+"_md[24]={5,2,5,9,2,5,1,9,5,2,7,5,9,2,5,3,5,9,2,5,1,5,9,2};\n"
     +t+" "+p+"_mode("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[16]; for(int i=0;i<16;i++) cnt[i]=0u;\n"
     "    for(int i=0;i<24;i++){ unsigned v=((unsigned)"+p+"_md[i]^((s>>(i&7))&3u))&15u; cnt[v]++; }\n"
     "    unsigned best=0u, bestv=0u; for(unsigned v=0;v<16u;v++) if(cnt[v]>best){ best=cnt[v]; bestv=v; }\n"
     "    acc=acc*131u+bestv*32u+best; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Au}, "OptStress179", 2},

    // sliding-window minimum via a monotonic index deque (window K=4).
    {p+"_windowmin",
     "static const unsigned char "+p+"_wm[24]={6,2,9,1,12,4,7,3,11,0,14,5,8,2,10,1,13,4,7,2,9,5,3,8};\n"
     +t+" "+p+"_windowmin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=((unsigned)"+p+"_wm[i]^((s>>(i&7))&3u))&15u;\n"
     "    int dq[24]; int head=0, tail=0; int K=4; unsigned fold=0u;\n"
     "    for(int i=0;i<24;i++){ while(tail>head && arr[dq[tail-1]]>=arr[i]) tail--; dq[tail++]=i;\n"
     "      if(dq[head]<=i-K) head++; if(i>=K-1){ fold=fold*131u+arr[dq[head]]; } }\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Bu}, "OptStress179", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress179TC("x64o179", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress179TC("x86o179", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress179TC("a64o179", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress179TC("armo179", "int");

INSTANTIATE_TEST_SUITE_P(OptStress179, X64OptStress179RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress179, X86OptStress179RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress179, A64OptStress179RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress179, ARM32OptStress179RT, ::testing::ValuesIn(kARM), rtTCName);
