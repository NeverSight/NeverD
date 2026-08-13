//===- AllPlatform4_KernelRTTests.cpp - 4-platform control/mem kernels -C++-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Algorithm kernels run across ALL FOUR targets including i386 (most of the big
// VectorAlgo batches skip 32-bit x86).  These stress control flow, memory, and
// scalar arithmetic — string hashing, run-length encode, binary search,
// insertion sort, Fletcher checksum, and a small byte state machine — so the
// i386 cdecl frame, x87/SSE epilogue, and pointer recovery are exercised next
// to the 64-bit and ARM paths.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Plat4KernelRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Plat4KernelRT, Verify) { roundTripX64(GetParam()); }

class X86Plat4KernelRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Plat4KernelRT, Verify) { roundTripX86(GetParam()); }

class A64Plat4KernelRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Plat4KernelRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32Plat4KernelRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Plat4KernelRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeP4TC(const char *prefix, const char *T,
                                         int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // djb2 + fnv1a string hashes fused (multiply/xor loop-carried chains).
    {p+"_strhash",
     t+" "+p+"_strhash("+t+" a){\n"
     "  unsigned char s[64]; for(int i=0;i<64;i++) s[i]=(unsigned char)(a+i*7);\n"
     "  unsigned d=5381u,f=2166136261u;\n"
     "  for(int i=0;i<64;i++){ d=((d<<5)+d)+s[i]; f=(f^s[i])*16777619u; }\n"
     "  return ("+t+")(d^f);\n"
     "}\n",
     {0x1234567ULL}, "Plat4Kernel", opt, fl},

    // Run-length encode then sum the (value,count) stream.
    {p+"_rle",
     t+" "+p+"_rle("+t+" a){\n"
     "  unsigned char s[96]; for(int i=0;i<96;i++) s[i]=(unsigned char)((a+i)>>2);\n"
     "  unsigned h=0; int i=0;\n"
     "  while(i<96){ unsigned char c=s[i]; int run=1;\n"
     "    while(i+run<96 && s[i+run]==c && run<255) run++;\n"
     "    h=h*131u+c*257u+(unsigned)run; i+=run; }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x2233445ULL}, "Plat4Kernel", opt, fl},

    // Binary search over a sorted table for several keys.
    {p+"_bsearch",
     t+" "+p+"_bsearch("+t+" a){\n"
     "  int tab[64]; for(int i=0;i<64;i++) tab[i]=i*i-100;\n"
     "  unsigned h=0;\n"
     "  for(int q=0;q<48;q++){ int key=(int)((a+q*53)%4000)-100;\n"
     "    int lo=0,hi=63,found=-1;\n"
     "    while(lo<=hi){ int mid=(lo+hi)>>1;\n"
     "      if(tab[mid]==key){found=mid;break;} else if(tab[mid]<key) lo=mid+1; else hi=mid-1; }\n"
     "    h=h*131u+(unsigned)(found+1); }\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "Plat4Kernel", opt, fl},

    // Insertion sort of a small array, then weighted sum.
    {p+"_insort",
     t+" "+p+"_insort("+t+" a){\n"
     "  int v[24]; for(int i=0;i<24;i++) v[i]=(int)((a*(i+1))^(i*1103515245u));\n"
     "  for(int i=1;i<24;i++){ int x=v[i],j=i-1;\n"
     "    while(j>=0 && v[j]>x){ v[j+1]=v[j]; j--; } v[j+1]=x; }\n"
     "  unsigned h=0; for(int i=0;i<24;i++) h=h*131u+(unsigned)v[i]*(unsigned)(i+1);\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x4455667ULL}, "Plat4Kernel", opt, fl},

    // Fletcher-16 checksum (two modular accumulators).
    {p+"_fletcher",
     t+" "+p+"_fletcher("+t+" a){\n"
     "  unsigned char s[100]; for(int i=0;i<100;i++) s[i]=(unsigned char)(a*3+i*5);\n"
     "  unsigned s1=0,s2=0;\n"
     "  for(int i=0;i<100;i++){ s1=(s1+s[i])%255u; s2=(s2+s1)%255u; }\n"
     "  return ("+t+")((s2<<8)|s1);\n"
     "}\n",
     {0x5566778ULL}, "Plat4Kernel", opt, fl},

    // Byte state machine (switch over loop-carried state).
    {p+"_fsm",
     t+" "+p+"_fsm("+t+" a){\n"
     "  unsigned char s[120]; for(int i=0;i<120;i++) s[i]=(unsigned char)(a+i*11);\n"
     "  int st=0; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ unsigned c=s[i];\n"
     "    switch(st){\n"
     "      case 0: st=(c&1)?1:2; acc+=c; break;\n"
     "      case 1: st=(c&2)?2:0; acc+=c*3u; break;\n"
     "      default: st=(c&4)?0:1; acc^=c; break; } }\n"
     "  return ("+t+")(acc*131u+(unsigned)st);\n"
     "}\n",
     {0x6677889ULL}, "Plat4Kernel", opt, fl},

    // Nested loop matrix row/col accumulation with conditional.
    {p+"_grid",
     t+" "+p+"_grid("+t+" a){\n"
     "  int m[12][12]; for(int r=0;r<12;r++) for(int c=0;c<12;c++) m[r][c]=(int)(a+r*7+c*3);\n"
     "  int acc=0;\n"
     "  for(int r=0;r<12;r++) for(int c=0;c<12;c++){\n"
     "    int v=m[r][c]; if((r+c)&1) acc+=v; else acc-=v;\n"
     "    if(r>0) acc^=m[r-1][c]&0xFF; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "Plat4Kernel", opt, fl},
  };
}
// clang-format on

// 32-bit modular exponentiation with a 64-bit intermediate: on 64-bit targets
// `% const` lowers to a native magic-number divide, but on i386 the 64-bit
// modulo becomes a __umoddi3 library call the original itself relies on — so it
// is restricted to the 64-bit platforms (the original would otherwise need libc).
static RoundTripTC makeModExp(const char *prefix, const char *T, int opt) {
  std::string p = prefix, t = T;
  return {p+"_modexp",
    t+" "+p+"_modexp("+t+" a){\n"
    "  unsigned base=((unsigned)a|1u)%1000003u, exp=(unsigned)a&0x7F, m=1000003u;\n"
    "  unsigned r=1;\n"
    "  for(int i=0;i<32;i++){ if((exp>>(i&31))&1u) r=(unsigned)(((unsigned long long)r*base)%m);\n"
    "    base=(unsigned)(((unsigned long long)base*base)%m); }\n"
    "  return ("+t+")r;\n"
    "}\n",
    {0x778899AULL}, "Plat4Kernel", opt};
}

static std::vector<RoundTripTC> with64(const char *prefix, const char *T) {
  auto V = makeP4TC(prefix, T, 2, "");
  V.push_back(makeModExp(prefix, T, 2));
  return V;
}

static const std::vector<RoundTripTC> kX64 = with64("x64p4", "long");
static const std::vector<RoundTripTC> kX86 = makeP4TC("x86p4", "int", 2, "");
static const std::vector<RoundTripTC> kA64 = with64("a64p4", "long");
static const std::vector<RoundTripTC> kARM = makeP4TC("armp4", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(Plat4Kernel, X64Plat4KernelRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Plat4Kernel, X86Plat4KernelRT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Plat4Kernel, A64Plat4KernelRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Plat4Kernel, ARM32Plat4KernelRT,
                         ::testing::ValuesIn(kARM), rtTCName);
