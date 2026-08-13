//===- AllPlatform_OptStress144RTTests.cpp - binsearch / heap / insort =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * binsrch - binary search over a sorted rodata table: a lo/hi/mid window
//               halves the live interval until the key is found or the range
//               collapses.  Pins a divide-and-conquer logarithmic probe
//               (distinct from any linear table scan or hashed lookup).
//   * heapsd  - max-heap construction by sift-down over a rodata-seeded stack
//               array: each root sinks past the larger of its two children via
//               implicit parent/child index arithmetic (2i+1 / 2i+2).  Pins a
//               binary-heap restructure (distinct from the pivot partition in
//               the #124 quickselect and from any adjacent-swap pass).
//   * insort  - insertion sort with element shifting over a rodata-seeded array:
//               each new key walks left, sliding larger neighbours up one slot,
//               then drops into place.  Pins a shift-insert ordering pass
//               (distinct from the Dutch-flag three-way partition in #141).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress144RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress144RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress144RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress144RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress144RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress144RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress144RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress144RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress144TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // binary search over a sorted rodata table (lo/hi/mid halving window).
    {p+"_binsrch",
     "static const unsigned char "+p+"_tab[32]={\n"
     "2,5,9,13,18,22,27,31, 36,40,45,49,54,58,63,67, 72,76,81,85,90,94,99,103, 108,112,117,121,126,130,135,139};\n"
     +t+" "+p+"_binsrch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<48;q++){ unsigned key=(((s>>(q&7))&0xFFu)+(unsigned)q*3u)&0xFFu;\n"
     "      int lo=0, hi=31, found=-1, steps=0;\n"
     "      while(lo<=hi){ int mid=(lo+hi)>>1; unsigned v=(unsigned)"+p+"_tab[mid]; steps++;\n"
     "        if(v==key){ found=mid; break; }\n"
     "        else if(v<key) lo=mid+1; else hi=mid-1; }\n"
     "      acc=acc*131u+(unsigned)(found+1)+(unsigned)steps*7u; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x14u}, "OptStress144", 2},

    // max-heap build by sift-down over a rodata-seeded stack array.
    {p+"_heapsd",
     "static const unsigned char "+p+"_arr[24]={\n"
     "37,12,180,5,99,46,213,8, 71,150,23,64,131,2,97,55, 188,30,118,77,14,200,41,160};\n"
     +t+" "+p+"_heapsd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned h[24];\n"
     "    for(int i=0;i<24;i++) h[i]=(unsigned)"+p+"_arr[i]^((s>>(i&7))&3u);\n"
     "    for(int start=11;start>=0;start--){ int root=start;\n"
     "      while(root*2+1<24){ int child=root*2+1;\n"
     "        if(child+1<24 && h[child+1]>h[child]) child++;\n"
     "        if(h[root]<h[child]){ unsigned tmp=h[root]; h[root]=h[child]; h[child]=tmp; root=child; acc=acc*131u+h[child]; }\n"
     "        else break; } }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+h[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x29u}, "OptStress144", 2},

    // insertion sort (shift-insert) over a rodata-seeded stack array.
    {p+"_insort",
     "static const unsigned char "+p+"_keys[20]={\n"
     "44,9,127,3,88,21,200,15, 67,140,30,5,110,72,18,155, 96,40,123,1};\n"
     +t+" "+p+"_insort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned k[20];\n"
     "    for(int i=0;i<20;i++) k[i]=(unsigned)"+p+"_keys[i]^((s>>(i&7))&7u);\n"
     "    for(int i=1;i<20;i++){ unsigned key=k[i]; int j=i-1;\n"
     "      while(j>=0 && k[j]>key){ k[j+1]=k[j]; j--; acc=acc*131u+k[j+1]; }\n"
     "      k[j+1]=key; acc=acc*131u+(unsigned)(j+1); }\n"
     "    for(int i=0;i<20;i++) acc=acc*131u+k[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Au}, "OptStress144", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress144TC("x64o144", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress144TC("x86o144", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress144TC("a64o144", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress144TC("armo144", "int");

INSTANTIATE_TEST_SUITE_P(OptStress144, X64OptStress144RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress144, X86OptStress144RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress144, A64OptStress144RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress144, ARM32OptStress144RT, ::testing::ValuesIn(kARM), rtTCName);
