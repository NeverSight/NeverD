//===- AllPlatform_OptStress160RTTests.cpp - rotated search / peak / Kadane =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through a plain base+index copy into a stack buffer and folds a result that
// depends only on the bytes + control flow (never an absolute VA), so nothing
// touches the deferred i386/ARM32 PIC rodata *interior*-pointer model
// (#477/#487); every probe runs on all four targets.
//
//   * rotsearch - binary search in a runtime-rotated sorted rodata array: the
//                 half containing the key is chosen from which side is ordered.
//                 Pins a rotated-array search (distinct from the plain bounded
//                 binary search in #144).
//   * peakfind  - local-peak location by slope-following binary search over a
//                 rodata array.  Pins a gradient-descent halving search (distinct
//                 from the value-equality search above).
//   * kadane    - maximum-subarray sum of a signed rodata stream via Kadane's
//                 running best/current.  Pins a reset-on-drop prefix scan
//                 (distinct from any plain prefix sum or sort).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress160RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress160RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress160RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress160RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress160RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress160RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress160RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress160RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress160TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // binary search in a runtime-rotated sorted rodata array.
    {p+"_rotsearch",
     "static const unsigned char "+p+"_rs[24]={5,11,18,24,31,37,44,50,57,63,70,76,83,89,96,102,109,115,122,128,135,141,148,154};\n"
     +t+" "+p+"_rotsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned base[24]; for(int i=0;i<24;i++) base[i]=(unsigned)"+p+"_rs[i];\n"
     "    unsigned rot=s%24u; unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=base[(i+rot)%24u];\n"
     "    for(int q=0;q<16;q++){ unsigned key=((s>>(q&7))&0x7Fu)+(unsigned)q; int lo=0,hi=23,found=-1;\n"
     "      while(lo<=hi){ int mid=(lo+hi)/2; if(arr[mid]==key){ found=mid; break; }\n"
     "        if(arr[lo]<=arr[mid]){ if(arr[lo]<=key && key<arr[mid]) hi=mid-1; else lo=mid+1; }\n"
     "        else { if(arr[mid]<key && key<=arr[hi]) lo=mid+1; else hi=mid-1; } }\n"
     "      acc=acc*131u+(unsigned)(found+1)+key; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x24u}, "OptStress160", 2},

    // local-peak location by slope-following binary search over rodata.
    {p+"_peakfind",
     "static const unsigned char "+p+"_pf[24]={2,5,9,14,20,27,33,40,46,51,55,58,54,49,43,36,30,25,19,13,8,4,3,1};\n"
     +t+" "+p+"_peakfind("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=(unsigned)"+p+"_pf[i]^((s>>(i&7))&3u);\n"
     "    int lo=0,hi=23; while(lo<hi){ int mid=(lo+hi)/2; if(arr[mid]<arr[mid+1]) lo=mid+1; else hi=mid; }\n"
     "    acc=acc*131u+(unsigned)lo+arr[lo]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x35u}, "OptStress160", 2},

    // maximum-subarray sum of a signed rodata stream (Kadane).
    {p+"_kadane",
     "static const unsigned char "+p+"_kd[28]={5,12,3,9,1,14,7,2,11,6,4,15,8,10,13,0,9,5,12,3,7,2,8,6,11,1,14,4};\n"
     +t+" "+p+"_kadane("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; int best=-1000000,cur=0;\n"
     "    for(int i=0;i<28;i++){ int v=(int)((unsigned)"+p+"_kd[i]^((s>>(i&7))&7u))-8;\n"
     "      cur=cur+v; if(cur<v) cur=v; if(cur>best) best=cur; acc=acc*131u+(unsigned)(cur+1000); }\n"
     "    acc=acc*131u+(unsigned)(best+1000); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x66u}, "OptStress160", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress160TC("x64o160", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress160TC("x86o160", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress160TC("a64o160", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress160TC("armo160", "int");

INSTANTIATE_TEST_SUITE_P(OptStress160, X64OptStress160RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress160, X86OptStress160RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress160, A64OptStress160RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress160, ARM32OptStress160RT, ::testing::ValuesIn(kARM), rtTCName);
