//===- AllPlatform_OptStress164RTTests.cpp - RLE decode / LZ match / suffix =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index loops (separate arrays for paired data, copies for
// scanned data) and folds a result that depends only on the bytes + control
// flow (never an absolute VA), so nothing touches the deferred i386/ARM32 PIC
// rodata *interior*-pointer model (#477/#487); every probe runs on all four
// targets.
//
//   * rledec   - run-length DECODE: expand rodata (value,count) pairs into a
//                bounded output stream.  Pins a run-expansion writer (distinct
//                from the run-length ENCODER in #147).
//   * lzmatch  - LZ77-style longest-match finder: for each position scan a
//                bounded back-window for the longest prefix match.  Pins a
//                sliding-window back-reference search (distinct from the
//                Boyer-Moore / Rabin-Karp pattern searches elsewhere).
//   * suffrank - naive suffix ranking: rank each suffix by counting how many
//                other suffixes compare lexicographically smaller.  Pins an
//                all-pairs suffix comparison (distinct from the Z-array / KMP
//                prefix-function scans).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress164RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress164RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress164RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress164RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress164RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress164RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress164RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress164RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress164TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // run-length DECODE: expand rodata (value,count) pairs into a stream.
    {p+"_rledec",
     "static const unsigned char "+p+"_rv[12]={5,9,2,7,1,8,3,6,4,0,9,2};\n"
     "static const unsigned char "+p+"_rc[12]={2,1,3,0,2,1,0,3,1,2,0,3};\n"
     +t+" "+p+"_rledec("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s; unsigned ob[48]; int w=0;\n"
     "    for(int i=0;i<12;i++){ unsigned v=(unsigned)"+p+"_rv[i]^((s>>(i&7))&3u); unsigned cnt=((unsigned)"+p+"_rc[i]&3u)+1u;\n"
     "      for(unsigned k=0;k<cnt && w<48;k++){ ob[w++]=v; acc=acc*131u+v; } }\n"
     "    acc=acc*131u+(unsigned)w; for(int i=0;i<w;i++) acc=acc*131u+ob[i]*(unsigned)(i+1); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x28u}, "OptStress164", 2},

    // LZ77-style longest back-window match finder over rodata.
    {p+"_lzmatch",
     "static const unsigned char "+p+"_lz[40]={5,9,12,5,9,12,3,7,14,2,8,11,5,9,12,3,7,14,6,1,15,4,10,13,5,9,12,0,9,5,12,3,7,2,8,6,5,9,12,3};\n"
     +t+" "+p+"_lzmatch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[40]; for(int i=0;i<40;i++) d[i]=(unsigned)"+p+"_lz[i]^((s>>(i&7))&1u);\n"
     "    unsigned totlen=0u,maxlen=0u;\n"
     "    for(int i=1;i<40;i++){ int bestlen=0,bestdist=0; int wstart=(i-16>0)?i-16:0;\n"
     "      for(int j=wstart;j<i;j++){ int len=0; while(i+len<40 && d[j+len]==d[i+len] && len<8) len++;\n"
     "        if(len>bestlen){ bestlen=len; bestdist=i-j; } }\n"
     "      totlen+=(unsigned)bestlen; if((unsigned)bestlen>maxlen) maxlen=(unsigned)bestlen; acc=acc*131u+(unsigned)(bestlen*16+bestdist); }\n"
     "    acc=acc*131u+totlen+maxlen; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x39u}, "OptStress164", 2},

    // naive suffix ranking by all-pairs lexicographic comparison over rodata.
    {p+"_suffrank",
     "static const unsigned char "+p+"_sr[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     +t+" "+p+"_suffrank("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned d[16]; for(int i=0;i<16;i++) d[i]=(unsigned)"+p+"_sr[i]^((s>>(i&7))&3u);\n"
     "    for(int i=0;i<16;i++){ unsigned rank=0u;\n"
     "      for(int j=0;j<16;j++){ if(j==i) continue; int k=0; while(i+k<16 && j+k<16 && d[i+k]==d[j+k]) k++;\n"
     "        int jless; if(j+k>=16 && i+k>=16) jless=0; else if(j+k>=16) jless=1; else if(i+k>=16) jless=0; else jless=(d[j+k]<d[i+k])?1:0;\n"
     "        if(jless) rank++; }\n"
     "      acc=acc*131u+rank; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Au}, "OptStress164", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress164TC("x64o164", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress164TC("x86o164", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress164TC("a64o164", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress164TC("armo164", "int");

INSTANTIATE_TEST_SUITE_P(OptStress164, X64OptStress164RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress164, X86OptStress164RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress164, A64OptStress164RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress164, ARM32OptStress164RT, ::testing::ValuesIn(kARM), rtTCName);
