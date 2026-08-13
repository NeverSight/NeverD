//===- AllPlatform_OptStress64RTTests.cpp - register-pressure mega -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// High register-pressure "mega" kernels that interleave many live values of
// mixed width so the optimizer is forced to spill/reload and reuse sub-
// registers heavily — the exact conditions under which SSA/liveness and
// sub-register-alias tracking miscompiles surface.  Each function combines
// several feature axes in one body (nested loops, switch dispatch, a runtime-
// indexed stack array, 64-bit and narrow arithmetic, conditional accumulation)
// rather than isolating one, to flush interaction bugs the per-feature probes
// miss.
//
//   * manyacc  - 16 interleaved mixed-width accumulators (forces spilling).
//   * vmstate  - bytecode VM with 8 registers + switch dispatch + memory.
//   * megamix  - 64-bit hash + byte ring + switch + conditional in one loop.
//   * shuffle  - permutation/transpose of a stack matrix, index-heavy.
//   * statem   - multi-variable state machine over a rodata program.
//   * crcroll  - CRC table + rolling polynomial hash interleaved.
//
// All integer, computed array fills, fold to one return, no float / 64-bit
// divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress64RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress64RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress64RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress64RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress64RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress64RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress64RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress64RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress64TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 16 interleaved mixed-width accumulators (forces register spilling).
    {p+"_manyacc",
     t+" "+p+"_manyacc("+t+" a){\n"
     "  unsigned s=(unsigned)a;\n"
     "  unsigned a0=1,a1=2,a2=3,a3=4; unsigned char b0=5,b1=6,b2=7,b3=8;\n"
     "  unsigned short c0=9,c1=10,c2=11,c3=12; unsigned long long d0=13,d1=14,d2=15,d3=16;\n"
     "  for(int i=0;i<400;i++){ s=s*1103515245u+12345u;\n"
     "    a0+=s; a1^=a0; a2+=a1*3u; a3=(a3<<1)|(a3>>31); a3+=a2;\n"
     "    b0+=(unsigned char)s; b1^=b0; b2+=(unsigned char)(b1+s); b3=(unsigned char)(b3*3u);\n"
     "    c0+=(unsigned short)s; c1^=c0; c2+=(unsigned short)(c1*5u); c3=(unsigned short)(c3+a0);\n"
     "    d0+=s; d1^=d0; d2+=d1*0x9E3779B1ull; d3=(d3<<7)|(d3>>57); d3+=d2;\n"
     "    a0^=b3+c2; d0+=(unsigned long long)c3+b1; }\n"
     "  unsigned long long h=a0+a1+a2+a3+b0+b1+b2+b3+c0+c1+c2+c3+d0+d1+d2+d3;\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0xC1u}, "OptStress64", 2},

    // Bytecode VM with 8 registers + switch dispatch + memory.
    {p+"_vmstate",
     "static const unsigned char PROG[40]={"
     "1,0,7, 1,1,3, 2,2,0,1, 4,3,2, 1,4,9, 6,5,4,3, 2,6,5,2, 7,7,6, "
     "3,0,7, 5,1,0, 6,2,1,4, 0,3,2};\n"
     +t+" "+p+"_vmstate("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<150;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned R[8]={s&7u,(s>>3)&7u,(s>>6)&7u,(s>>9)&7u,(s>>12)&7u,(s>>15)&7u,(s>>18)&7u,(s>>21)&7u};\n"
     "    unsigned mem[8]={0}; int pc=0;\n"
     "    for(int step=0;step<24 && pc<38;step++){ unsigned op=PROG[pc]&7u;\n"
     "      switch(op){\n"
     "        case 0: R[PROG[pc+1]&7u]=PROG[pc+2]; pc+=3; break;\n"
     "        case 1: R[PROG[pc+1]&7u]+=PROG[pc+2]; pc+=3; break;\n"
     "        case 2: R[PROG[pc+1]&7u]=R[PROG[pc+2]&7u]+R[PROG[pc+3]&7u]; pc+=4; break;\n"
     "        case 3: R[PROG[pc+1]&7u]^=R[PROG[pc+2]&7u]; pc+=3; break;\n"
     "        case 4: mem[R[PROG[pc+1]&7u]&7u]=R[PROG[pc+2]&7u]; pc+=3; break;\n"
     "        case 5: R[PROG[pc+1]&7u]=mem[R[PROG[pc+2]&7u]&7u]; pc+=3; break;\n"
     "        case 6: R[PROG[pc+1]&7u]=R[PROG[pc+2]&7u]*R[PROG[pc+3]&7u]; pc+=4; break;\n"
     "        default: R[PROG[pc+1]&7u]=(R[PROG[pc+2]&7u]<<1); pc+=3; break; } }\n"
     "    for(int i=0;i<8;i++) h=h*131u+R[i]+mem[i]; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0xC2u}, "OptStress64", 2},

    // 64-bit hash + byte ring + switch + conditional in one loop.
    {p+"_megamix",
     t+" "+p+"_megamix("+t+" a){\n"
     "  unsigned long long h=0; unsigned s=(unsigned)a; unsigned char ring[16]={0}; int ri=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    ring[ri&15]=(unsigned char)(s>>9); int prev=ring[(ri-3)&15];\n"
     "    unsigned long long k=((unsigned long long)s<<32)|(s^0x55555555u);\n"
     "    switch((s>>4)&3u){\n"
     "      case 0: k*=0xff51afd7ed558ccdull; k^=k>>33; break;\n"
     "      case 1: k=(k<<27)|(k>>37); k+=(unsigned)prev; break;\n"
     "      case 2: k^=(unsigned long long)ring[ri&15]*131u; k*=5ull; break;\n"
     "      default: k-=h; k^=k>>29; break; }\n"
     "    if((int)k<0) h-=k; else h+=k;\n"
     "    h ^= (h<<17)|(h>>47); ri++; }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0xC3u}, "OptStress64", 2},

    // Permutation / transpose of a stack matrix, index-heavy.
    {p+"_shuffle",
     "static const unsigned char PERM[16]={5,11,2,14,7,0,9,3,12,6,1,15,8,4,13,10};\n"
     +t+" "+p+"_shuffle("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<150;it++){ unsigned m[16];\n"
     "    for(int i=0;i<16;i++){ s=s*1103515245u+12345u; m[i]=(s>>8)&0xffff; }\n"
     "    unsigned t2[16]; for(int i=0;i<16;i++) t2[PERM[i]]=m[i];\n"
     "    unsigned tr[16]; for(int r=0;r<4;r++) for(int c=0;c<4;c++) tr[c*4+r]=t2[r*4+c];\n"
     "    for(int i=0;i<16;i++) h=h*131u+tr[i]+(unsigned)(i*tr[(i*7)&15]); h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0xC4u}, "OptStress64", 2},

    // Multi-variable state machine over a rodata program.
    {p+"_statem",
     "static const unsigned char SM[24]={0,3,12,1, 1,5,7,2, 2,9,1,0, "
     "0,2,8,3, 3,6,4,1, 1,1,11,0};\n"
     +t+" "+p+"_statem("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<200;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned st=0, x=s, y=0, z=1; int guard=0;\n"
     "    for(int step=0; step<32 && guard<24; step++){\n"
     "      unsigned base=(st%6u)*4u;\n"
     "      unsigned ns=SM[base], add=SM[base+1], mul=SM[base+2], nx=SM[base+3];\n"
     "      y=y+add*(x&0xff); z=z*mul + (x>>3); x=(x>>1)^(x<<3);\n"
     "      st = (x&1u)? ns : nx; guard++; if(st==0 && step>4) break; }\n"
     "    h=h*131u+y+z; h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0xC5u}, "OptStress64", 2},

    // CRC table + rolling polynomial hash interleaved.
    {p+"_crcroll",
     t+" "+p+"_crcroll("+t+" a){\n"
     "  unsigned s=(unsigned)a, crc=0xffffffffu, roll=0, h=0;\n"
     "  unsigned tbl[16];\n"
     "  for(int i=0;i<16;i++){ unsigned c=i; for(int k=0;k<8;k++) c=(c>>1)^(0xEDB88320u&(0u-(c&1u))); tbl[i]=c; }\n"
     "  for(int i=0;i<400;i++){ s=s*1103515245u+12345u; unsigned byte=(s>>11)&0xff;\n"
     "    crc=(crc>>4)^tbl[(crc^byte)&0xf]; crc=(crc>>4)^tbl[(crc^(byte>>4))&0xf];\n"
     "    roll=roll*131u+byte; roll^=roll>>15;\n"
     "    h += (crc&1u)? roll : (crc>>1); h=(h<<3)|(h>>29); }\n"
     "  return ("+t+")(h^crc^roll); }\n",
     {0xC6u}, "OptStress64", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress64TC("x64o64", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress64TC("x86o64", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress64TC("a64o64", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress64TC("armo64", "int");

INSTANTIATE_TEST_SUITE_P(OptStress64, X64OptStress64RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress64, X86OptStress64RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress64, A64OptStress64RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress64, ARM32OptStress64RT, ::testing::ValuesIn(kARM), rtTCName);
