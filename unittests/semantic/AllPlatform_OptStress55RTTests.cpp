//===- AllPlatform_OptStress55RTTests.cpp - mini-program kernels -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Whole-program stress: complex single functions that combine switch dispatch
// (lowered to jump tables), a runtime-indexed memory array, and nested loops in
// one body — the interaction surface where CFG / jump-table / stack-frame
// modeling meet, distinct from the per-feature OptStress probes.  Unlike
// #ComputedGoto (label-as-value `goto *`), these drive `switch` jump tables plus
// a real memory stack/register file.
//
//   * bcvm     - stack bytecode VM: 12-op switch dispatch over a rodata program,
//                runtime-sp-indexed value stack.
//   * regvm    - register-file VM: 8-way op switch over a rodata code stream,
//                runtime-indexed register array.
//   * tokenize - finite-state tokenizer walking a rodata string, 3-state switch.
//   * insort   - insertion sort of a computed array (memory moves + compares).
//   * crc8     - bit-by-bit CRC-8 over a rodata message (nested loop + branch).
//   * pathmin  - Bellman-Ford relaxation over a rodata edge list (array relax).
//
// All integer, computed array fills (never memset/memcpy), fold to one return,
// no float / 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress55RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress55RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress55RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress55RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress55RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress55RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress55RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress55RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress55TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Stack bytecode VM: 12-op switch dispatch + runtime-sp value stack.
    {p+"_bcvm",
     "static const unsigned char prog[34]={"
     "0,5, 0,7, 1, 4, 3, 0,3, 6, 8,2, 0,15, 11, 5, 2, 0,9, 3, 7, 10, 9,1,"
     "1, 6, 3, 11, 0xFF, 0xFF};\n"
     +t+" "+p+"_bcvm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int iter=0;iter<40;iter++){ s=s*1103515245u+12345u;\n"
     "    unsigned st[34]; int sp=0; st[sp++]=s; st[sp++]=s>>7; int pc=0;\n"
     "    while(pc<32 && prog[pc]!=0xFF){ unsigned char op=prog[pc++];\n"
     "      switch(op){\n"
     "        case 0: if(sp<33) st[sp++]=prog[pc++]; else pc++; break;\n"
     "        case 1: if(sp>=2){ st[sp-2]+=st[sp-1]; sp--; } break;\n"
     "        case 2: if(sp>=2){ st[sp-2]-=st[sp-1]; sp--; } break;\n"
     "        case 3: if(sp>=2){ st[sp-2]*=st[sp-1]; sp--; } break;\n"
     "        case 4: if(sp>=1&&sp<33){ st[sp]=st[sp-1]; sp++; } break;\n"
     "        case 5: if(sp>=2){ unsigned tt=st[sp-1]; st[sp-1]=st[sp-2]; st[sp-2]=tt; } break;\n"
     "        case 6: if(sp>=2){ st[sp-2]^=st[sp-1]; sp--; } break;\n"
     "        case 7: if(sp>=1){ st[sp-1]=0u-st[sp-1]; } break;\n"
     "        case 8: if(sp>=1){ st[sp-1]<<=(prog[pc++]&31); } else pc++; break;\n"
     "        case 9: if(sp>=1){ st[sp-1]>>=(prog[pc++]&31); } else pc++; break;\n"
     "        case 10: if(sp>=2){ st[sp-2]|=st[sp-1]; sp--; } break;\n"
     "        case 11: if(sp>=2){ st[sp-2]&=st[sp-1]; sp--; } break;\n"
     "        default: break; } }\n"
     "    out=out*131u+(sp>0?st[sp-1]:0u); }\n"
     "  return ("+t+")out; }\n",
     {0x11u}, "OptStress55", 2},

    // Register-file VM: 8-way op switch + runtime-indexed register array.
    {p+"_regvm",
     "static const unsigned char code[48]={"
     "0,1,2, 9,3,0, 18,2,1, 27,0,3, 36,1,2, 5,4,5, 14,6,7, 23,5,1,"
     "32,7,2, 1,3,6, 10,0,4, 19,2,5, 28,6,1, 37,4,0, 6,1,7, 15,3,2};\n"
     +t+" "+p+"_regvm("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int iter=0;iter<40;iter++){ s=s*1103515245u+12345u;\n"
     "    unsigned r[8];\n"
     "    for(int k=0;k<8;k++){ s=s*1103515245u+12345u; r[k]=s>>8; }\n"
     "    int pc=0;\n"
     "    while(pc+2<48){ unsigned char op=code[pc]; int d=code[pc+1]&7, x=code[pc+2]&7; pc+=3;\n"
     "      switch(op&7){\n"
     "        case 0: r[d]+=r[x]; break;\n"
     "        case 1: r[d]^=r[x]; break;\n"
     "        case 2: r[d]=(r[d]<<(x+1))|(r[d]>>(31-x)); break;\n"
     "        case 3: r[d]-=r[x]; break;\n"
     "        case 4: r[d]*=(r[x]|1u); break;\n"
     "        case 5: r[d]|=r[x]; break;\n"
     "        case 6: r[d]&=r[x]; break;\n"
     "        default: r[d]=r[x]+(unsigned)(op>>3); break; } }\n"
     "    for(int k=0;k<8;k++) out=out*131u+r[k]; }\n"
     "  return ("+t+")out; }\n",
     {0x12u}, "OptStress55", 2},

    // Finite-state tokenizer walking a rodata string, 3-state switch.
    {p+"_tokenize",
     "static const char txt[]=\"the_quick123 brown,FOX==42; jumps_over+the.lazy(dog)*7\";\n"
     +t+" "+p+"_tokenize("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0; const int N=(int)(sizeof(txt)-1);\n"
     "  for(int iter=0;iter<60;iter++){ s=s*1103515245u+12345u;\n"
     "    int state=0; unsigned tok=0; int start=(int)((s>>5)%(unsigned)N);\n"
     "    for(int i=0;i<N;i++){ char c=txt[(start+i)%N];\n"
     "      int cls=(c>='0'&&c<='9')?1:(((c>='a'&&c<='z')||(c>='A'&&c<='Z'))?2:3);\n"
     "      switch(state){\n"
     "        case 0: if(cls==2){state=1;tok=(unsigned char)c;}\n"
     "                else if(cls==1){state=2;tok=(unsigned)(c-'0');}\n"
     "                else h=h*131u+(unsigned char)c; break;\n"
     "        case 1: if(cls==2||cls==1) tok=tok*31u+(unsigned char)c;\n"
     "                else { h=h*1000003u+tok; state=0; } break;\n"
     "        default: if(cls==1) tok=tok*10u+(unsigned)(c-'0');\n"
     "                 else { h=h*131u+tok*7u; state=0; } break; } }\n"
     "    h^=h>>13; h+=tok; }\n"
     "  return ("+t+")h; }\n",
     {0x13u}, "OptStress55", 2},

    // Insertion sort of a computed array: memory moves + unsigned compares.
    {p+"_insort",
     t+" "+p+"_insort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int iter=0;iter<50;iter++){ s=s*1103515245u+12345u;\n"
     "    unsigned arr[16];\n"
     "    for(int i=0;i<16;i++){ s=s*1103515245u+12345u; arr[i]=s>>10; }\n"
     "    for(int i=1;i<16;i++){ unsigned key=arr[i]; int j=i-1;\n"
     "      while(j>=0 && arr[j]>key){ arr[j+1]=arr[j]; j--; } arr[j+1]=key; }\n"
     "    for(int i=0;i<16;i++) out=out*131u+arr[i]+(unsigned)i; }\n"
     "  return ("+t+")out; }\n",
     {0x14u}, "OptStress55", 2},

    // Bit-by-bit CRC-8 over a rodata message: nested loop + per-bit branch.
    {p+"_crc8",
     "static const unsigned char msg[40]={"
     "0x9e,0x37,0x79,0xb9,0x7f,0x4a,0x7c,0x15,0xf3,0x9c,0xc0,0x60,0x5c,0xed,"
     "0xc8,0x34,0x10,0x42,0xbd,0x1d,0x6e,0xf7,0x42,0x8c,0x1b,0x5a,0x71,"
     "0x6b,0x2f,0x83,0x91,0x0d,0xa6,0x55,0xeb,0x2c,0x9a,0x44,0xfe,0xc1};\n"
     +t+" "+p+"_crc8("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int iter=0;iter<50;iter++){ s=s*1103515245u+12345u;\n"
     "    unsigned crc=(s>>3)&0xffu;\n"
     "    for(int i=0;i<40;i++){ crc^=msg[(i+(s>>9))%40];\n"
     "      for(int b=0;b<8;b++) crc=(crc&0x80u)?(((crc<<1)^0x07u)&0xffu):((crc<<1)&0xffu); }\n"
     "    out=out*131u+crc; }\n"
     "  return ("+t+")out; }\n",
     {0x15u}, "OptStress55", 2},

    // Bellman-Ford relaxation over a rodata edge list: array relax + compares.
    {p+"_pathmin",
     "static const unsigned char edges[30]={"
     "0,1,4, 0,2,1, 2,1,2, 1,3,1, 2,3,5, 3,4,3, 0,4,9, 4,5,2, 1,5,7, 3,5,4};\n"
     +t+" "+p+"_pathmin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int iter=0;iter<40;iter++){ s=s*1103515245u+12345u;\n"
     "    unsigned dist[6];\n"
     "    for(int i=0;i<6;i++) dist[i]=100000u+(s&7u);\n"
     "    dist[0]=(s>>5)&3u;\n"
     "    for(int r=0;r<6;r++) for(int e=0;e<10;e++){\n"
     "      unsigned f=edges[e*3], to=edges[e*3+1], w=edges[e*3+2];\n"
     "      if(dist[f]+w<dist[to]) dist[to]=dist[f]+w; }\n"
     "    for(int i=0;i<6;i++) out=out*131u+dist[i]; }\n"
     "  return ("+t+")out; }\n",
     {0x16u}, "OptStress55", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress55TC("x64o55", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress55TC("x86o55", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress55TC("a64o55", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress55TC("armo55", "int");

INSTANTIATE_TEST_SUITE_P(OptStress55, X64OptStress55RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress55, X86OptStress55RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress55, A64OptStress55RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress55, ARM32OptStress55RT, ::testing::ValuesIn(kARM), rtTCName);
