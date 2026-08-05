//===- AllPlatform_OptStress125RTTests.cpp - raster / NFA / DP-set shapes ---=//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * raster - triangle rasterization inside-test via signed 2D edge functions
//              (`(x1-x0)*(py-y0)-(y1-y0)*(px-x0)`) for rodata test points against
//              a rodata triangle.  Pins three signed cross-product half-plane
//              tests (distinct from the unsigned image stencils).
//   * nfa    - Thompson NFA simulation as a state bitset over a rodata input: the
//              active-set transition `next |= trans[state*4+sym]` ORs the move
//              masks of all active states.  Pins a set-valued state recurrence
//              (distinct from the single-state DFA walk).
//   * subset - 0/1 subset-sum reachability DP over rodata values: the classic
//              backward sweep `reach[s]|=reach[s-v]` that forbids item reuse.
//              Pins a reverse-iterated boolean DP table over rodata weights.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress125RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress125RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress125RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress125RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress125RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress125RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress125RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress125RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress125TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // triangle rasterization inside-test via signed 2D edge functions.
    {p+"_raster",
     "static const unsigned char "+p+"_tri[6]={10,8,55,20,30,60};\n"
     "static const unsigned char "+p+"_pts[48]={\n"
     "20,25,40,30,15,50,35,15, 50,45,25,55,12,40,48,22, 30,35,18,28,45,52,22,18,\n"
     "38,48,28,12,52,38,16,33, 42,20,33,58,24,44,46,28, 14,36,50,30,27,49,39,17};\n"
     +t+" "+p+"_raster("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int x0="+p+"_tri[0], y0="+p+"_tri[1], x1="+p+"_tri[2], y1="+p+"_tri[3], x2="+p+"_tri[4], y2="+p+"_tri[5];\n"
     "    unsigned inside=0u;\n"
     "    for(int i=0;i+1<48;i+=2){ int px=(int)"+p+"_pts[i]+(int)((s>>(i&7))&1u), py=(int)"+p+"_pts[i+1];\n"
     "      int e0=(x1-x0)*(py-y0)-(y1-y0)*(px-x0);\n"
     "      int e1=(x2-x1)*(py-y1)-(y2-y1)*(px-x1);\n"
     "      int e2=(x0-x2)*(py-y2)-(y0-y2)*(px-x2);\n"
     "      unsigned in=((e0>=0&&e1>=0&&e2>=0)||(e0<=0&&e1<=0&&e2<=0))?1u:0u;\n"
     "      inside+=in; acc=acc*131u+in+(unsigned)(e0&0xFF); }\n"
     "    acc=acc*131u+inside; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Au}, "OptStress125", 2},

    // Thompson NFA simulation as a state bitset over a rodata input.
    {p+"_nfa",
     "static const unsigned char "+p+"_trans[32]={\n"
     "0x02,0x05,0x01,0x12, 0x10,0x20,0x44,0x09, 0x04,0x40,0x11,0x82, 0x28,0x14,0x03,0x50,\n"
     "0x80,0x41,0x22,0x0c, 0x60,0x09,0x90,0x14, 0x03,0x48,0x21,0x84, 0x18,0x42,0x05,0x30};\n"
     "static const unsigned char "+p+"_in[40]={\n"
     "1,3,0,2,1,2,3,0, 2,1,3,0,1,2,0,3, 1,0,2,3,2,1,0,3, 0,2,1,3,1,0,2,3, 3,1,2,0,1,3,0,2};\n"
     +t+" "+p+"_nfa("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned states=1u;\n"
     "    for(int i=0;i<40;i++){ unsigned sym=("+p+"_in[i]^(s>>(i&7)))&3u; unsigned ns=0u;\n"
     "      for(int st=0;st<8;st++) if(states&(1u<<st)) ns|="+p+"_trans[st*4+sym];\n"
     "      states=ns&0xFFu; if(states==0u) states=1u; acc=acc*131u+states; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Eu}, "OptStress125", 2},

    // 0/1 subset-sum reachability DP over rodata values (backward sweep).
    {p+"_subset",
     "static const unsigned char "+p+"_vals[16]={13,7,21,4,9,30,2,18, 25,11,6,28,3,15,22,8};\n"
     +t+" "+p+"_subset("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned target=64u+((s>>4)&31u);\n"
     "    unsigned char reach[256]; for(int i=0;i<256;i++) reach[i]=0; reach[0]=1;\n"
     "    for(int i=0;i<16;i++){ unsigned v=("+p+"_vals[i]&31u)+1u;\n"
     "      for(int sm=255;sm>=(int)v;sm--) if(reach[sm-(int)v]) reach[sm]=1; }\n"
     "    unsigned cnt=0u; for(int i=0;i<256;i++) if(reach[i]) cnt++;\n"
     "    acc=acc*131u+cnt+(reach[target&255u]?7u:0u); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x55u}, "OptStress125", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress125TC("x64o125", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress125TC("x86o125", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress125TC("a64o125", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress125TC("armo125", "int");

INSTANTIATE_TEST_SUITE_P(OptStress125, X64OptStress125RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress125, X86OptStress125RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress125, A64OptStress125RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress125, ARM32OptStress125RT, ::testing::ValuesIn(kARM), rtTCName);
