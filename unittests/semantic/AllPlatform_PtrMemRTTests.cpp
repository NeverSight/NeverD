//===- AllPlatform_PtrMemRTTests.cpp - pointer / memory idioms ---*-C++*-=//
//
// High-yield roundtrip probing of pointer- and memory-heavy code, the area
// recent rounds kept finding addressing bugs in (#390 post-index, #397 constant-
// pool induction pointers, #394 sub-register memory writes).  These idioms are
// distinct from the struct/array suites: post/pre-increment pointer walks, raw
// pointer differences steering control flow, in-place reverse and the
// three-reverse rotation (overlapping reads/writes), pointer-based selection
// sort, union byte/word reinterpretation, and a 2D access with a runtime row
// stride.  Each kernel returns a value-dependent hash compiled at -O2; the
// roundtrip compares native vs lifted execution across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PtrRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PtrRT, Verify) { roundTripX64(GetParam()); }
class X86PtrRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PtrRT, Verify) { roundTripX86(GetParam()); }
class A64PtrRT : public SemanticRoundTripFixture,
                 public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PtrRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32PtrRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PtrRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makePtrTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Post/pre-increment pointer walks: write the buffer through `*w++`, read it
    // back through `*--r`, exercising address auto-update separate from the value.
    {p+"_walk",
     t+" "+p+"_walk("+t+" a){\n"
     "  unsigned buf[32]; unsigned *w=buf;\n"
     "  for(int i=0;i<32;i++) *w++ = (unsigned)a*(unsigned)(i+1) ^ (unsigned)i;\n"
     "  unsigned h=0; unsigned *r=buf+32;\n"
     "  while(r>buf) h = h*31u + *--r;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1357ULL}, "Ptr", 2},

    // Raw pointer difference and comparison steering control flow: two cursors
    // move at different rates and their gap drives the accumulation.
    {p+"_diff",
     t+" "+p+"_diff("+t+" a){\n"
     "  unsigned buf[40]; for(int i=0;i<40;i++) buf[i]=(unsigned)a+(unsigned)i*7u;\n"
     "  unsigned *lo=buf, *hi=buf+39; unsigned h=0;\n"
     "  while(lo<hi){\n"
     "    long d=hi-lo; h += (unsigned)d*131u + *lo + *hi;\n"
     "    if(*lo<*hi){ lo++; } else { hi--; }\n"
     "    h ^= (unsigned)(hi-lo); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9ULL}, "Ptr", 2},

    // In-place reverse with the classic i++/j-- swap (overlapping endpoints).
    {p+"_rev",
     t+" "+p+"_rev("+t+" a){\n"
     "  unsigned v[24]; for(int i=0;i<24;i++) v[i]=(unsigned)a*2654435761u + (unsigned)i;\n"
     "  int i=0,j=23; while(i<j){ unsigned tmp=v[i]; v[i]=v[j]; v[j]=tmp; i++; j--; }\n"
     "  unsigned h=0; for(int k=0;k<24;k++) h=h*31u+v[k];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x2468ULL}, "Ptr", 2},

    // Array rotation by the three-reverse trick — three overlapping in-place
    // reverses whose boundaries depend on a runtime shift.
    {p+"_rot",
     t+" "+p+"_rot("+t+" a){\n"
     "  unsigned v[20]; for(int i=0;i<20;i++) v[i]=(unsigned)a+(unsigned)i*131u;\n"
     "  unsigned s=((unsigned)a%19u)+1u;\n"
     "  unsigned i,j;\n"
     "  for(i=0,j=s-1;i<j;i++,j--){ unsigned t1=v[i];v[i]=v[j];v[j]=t1; }\n"
     "  for(i=s,j=19;i<j;i++,j--){ unsigned t1=v[i];v[i]=v[j];v[j]=t1; }\n"
     "  for(i=0,j=19;i<j;i++,j--){ unsigned t1=v[i];v[i]=v[j];v[j]=t1; }\n"
     "  unsigned h=0; for(unsigned k=0;k<20;k++) h=h*131u+v[k];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x55ULL}, "Ptr", 2},

    // Pointer-based selection sort: scan with a moving pointer, track the min by
    // address, swap through dereferenced pointers.
    {p+"_sort",
     t+" "+p+"_sort("+t+" a){\n"
     "  unsigned v[16]; for(int i=0;i<16;i++) v[i]=(unsigned)a*(unsigned)(i*7+1) ^ (unsigned)(i*13);\n"
     "  for(unsigned *s=v;s<v+16;s++){ unsigned *m=s;\n"
     "    for(unsigned *q=s+1;q<v+16;q++) if(*q<*m) m=q;\n"
     "    unsigned t1=*s; *s=*m; *m=t1; }\n"
     "  unsigned h=0; for(int i=0;i<16;i++) h=h*31u+v[i];\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x99ULL}, "Ptr", 2},

    // Union byte/halfword/word reinterpretation: write a word, read back its
    // bytes and halfwords, recombine — memory-based type punning.
    {p+"_pun",
     t+" "+p+"_pun("+t+" a){\n"
     "  union { unsigned u; unsigned short h[2]; unsigned char b[4]; } x;\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<48;i++){\n"
     "    x.u = acc ^ ((unsigned)i*0x9E3779B9u);\n"
     "    unsigned r = (unsigned)x.b[0] + (unsigned)x.b[1]*131u\n"
     "               + (unsigned)x.b[2]*7u + (unsigned)x.b[3]*17u\n"
     "               + (unsigned)x.h[0]*3u + (unsigned)x.h[1]*5u;\n"
     "    acc = (acc*1664525u+1013904223u) ^ r; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0xA5ULL}, "Ptr", 2},

    // 2D access with a runtime row stride: index a flat buffer as a matrix whose
    // width is derived at runtime, walking rows and columns.
    {p+"_grid",
     t+" "+p+"_grid("+t+" a){\n"
     "  unsigned buf[64]; for(int i=0;i<64;i++) buf[i]=(unsigned)a+(unsigned)i;\n"
     "  unsigned w=((unsigned)a&3u)+4u; unsigned rows=64u/w;\n"
     "  unsigned h=0;\n"
     "  for(unsigned r=0;r<rows;r++)\n"
     "    for(unsigned c=0;c<w;c++){\n"
     "      unsigned v=buf[r*w+c]; h += v*(c+1u) ^ (r*131u); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x77ULL}, "Ptr", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makePtrTC("x64ptr", "long");
static const std::vector<RoundTripTC> kX86 = makePtrTC("x86ptr", "int");
static const std::vector<RoundTripTC> kA64 = makePtrTC("a64ptr", "long");
static const std::vector<RoundTripTC> kARM = makePtrTC("armptr", "int");

INSTANTIATE_TEST_SUITE_P(Ptr, X64PtrRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ptr, X86PtrRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ptr, A64PtrRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Ptr, ARM32PtrRT, ::testing::ValuesIn(kARM), rtTCName);
