//===- X64_RepStringBackwardRTTests.cpp - DF=1 REP string ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Backward (DF=1, `std`) REP MOVS/STOS.  The lifter previously modelled only
// the forward (DF=0) REP form -- it unconditionally advanced RSI/RDI *up* and
// emitted a forward `rep movs/stos`, so a `std; rep movsb` over overlapping
// buffers (the backward memmove idiom) copied the wrong direction and the final
// pointer landed on the wrong side.  These probes use overlapping ranges (movs)
// and a final-pointer check (stos) so forward-vs-backward is observable, forcing
// the roundtrip to honor the direction flag.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RepBwdRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RepBwdRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // std; rep movsb over an overlapping range (dst above src): backward copy
  // propagates correctly, a forward copy would smear byte 0 across the range.
  {"rep_movsb_bwd",
   "long f(long a){ unsigned char b[24];\n"
   "  for(int i=0;i<24;i++) b[i]=(unsigned char)(a+i);\n"
   "  unsigned char *ps=b+15,*pd=b+19; unsigned long n=16;\n"
   "  __asm__ volatile(\"std; rep movsb; cld\"\n"
   "    :\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");\n"
   "  int acc=0; for(int i=0;i<24;i++) acc=acc*3+b[i];\n"
   "  return acc + (int)(ps-b)*7 + (int)(pd-b)*13; }\n",
   {0x41}, "RepBwd", 0},

  // std; rep movsq backward over overlapping qwords.
  {"rep_movsq_bwd",
   "long f(long a){ unsigned long b[8];\n"
   "  for(int i=0;i<8;i++) b[i]=(unsigned long)a*(i+1)+i;\n"
   "  unsigned long *ps=b+5,*pd=b+6; unsigned long n=5;\n"
   "  __asm__ volatile(\"std; rep movsq; cld\"\n"
   "    :\"+S\"(ps),\"+D\"(pd),\"+c\"(n)::\"memory\",\"cc\");\n"
   "  unsigned long acc=0; for(int i=0;i<8;i++) acc=acc*131+b[i];\n"
   "  return (long)(acc ^ (acc>>32)) + (int)(pd-b); }\n",
   {0x1000003ULL}, "RepBwd", 0},

  // std; rep stosb backward: fill value is direction-independent but the final
  // RDI and which 8 of 16 bytes are written depend on the direction.
  {"rep_stosb_bwd",
   "long f(long a){ unsigned char b[16]; for(int i=0;i<16;i++) b[i]=0x11;\n"
   "  unsigned char *d=b+15; unsigned long n=8;\n"
   "  __asm__ volatile(\"std; rep stosb; cld\"\n"
   "    :\"+D\"(d),\"+c\"(n):\"a\"((unsigned char)a):\"memory\",\"cc\");\n"
   "  int acc=0; for(int i=0;i<16;i++) acc=acc*3+b[i];\n"
   "  return acc + (int)(d-b)*1000; }\n",
   {0xA5}, "RepBwd", 0},

  // std; rep stosd backward (dword fill, final pointer check).
  {"rep_stosd_bwd",
   "long f(long a){ unsigned int b[8]; for(int i=0;i<8;i++) b[i]=0x33333333u;\n"
   "  unsigned int *d=b+7; unsigned long n=5;\n"
   "  __asm__ volatile(\"std; rep stosl; cld\"\n"
   "    :\"+D\"(d),\"+c\"(n):\"a\"((unsigned int)a):\"memory\",\"cc\");\n"
   "  unsigned long acc=0; for(int i=0;i<8;i++) acc=acc*131+b[i];\n"
   "  return (long)(acc ^ (acc>>32)) + (int)(d-b)*10; }\n",
   {0xDEADBEEFULL}, "RepBwd", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RepBwd, X64RepBwdRT, ::testing::ValuesIn(kX64),
                         rtTCName);
