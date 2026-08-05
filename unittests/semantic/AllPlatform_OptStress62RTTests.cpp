//===- AllPlatform_OptStress62RTTests.cpp - mixed-width promote -*-C++*-=//
//
// Mixed 8/16/32/64-bit integer promotion, narrowing stores and partial-
// register writes — the sub-register-aliasing surface that has produced some
// of the worst miscompiles here (e.g. x86 "write RAX, read AL" folding to 0,
// AArch64 W/X view desync).  Each kernel deliberately writes a wide value then
// reads a narrow alias, narrows into a byte/halfword array and re-widens with
// both signed and unsigned extension so a broken sub-register alias surfaces
// as a return mismatch.
//
//   * subregbyte - write 32/64-bit, read back low byte/halfword (alias read).
//   * narrowarr  - uint8_t/uint16_t array store then signed/unsigned reload.
//   * promchain  - s8->s16->s32->s64 / u8->u64 promotion chains.
//   * wrapwidth  - intentional unsigned wrap at 8/16/32 width boundaries.
//   * mulnarrow  - widening multiply then narrow the result back down.
//   * carrylimb  - multiprecision add/sub via 16-bit limbs (carry across).
//
// All integer, fold to one return, no float / 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress62RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress62RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress62RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress62RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress62RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress62RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress62RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress62RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress62TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Write 32/64-bit, read back low byte/halfword (alias read).
    {p+"_subregbyte",
     t+" "+p+"_subregbyte("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned w=s*2654435761u + h;\n"
     "    unsigned char lb=(unsigned char)w; unsigned short lh=(unsigned short)w;\n"
     "    unsigned char hb=(unsigned char)(w>>8);\n"
     "    signed char sb=(signed char)w; short sh=(short)(w>>16);\n"
     "    h=h*131u+lb+lh*3u+hb*5u+(unsigned)(sb+128)+(unsigned)(sh+32768); h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0xA1u}, "OptStress62", 2},

    // uint8_t/uint16_t array store then signed/unsigned reload.
    {p+"_narrowarr",
     t+" "+p+"_narrowarr("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<120;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned char b[24]; signed char sb[24]; unsigned short w[24];\n"
     "    for(int i=0;i<24;i++){ s=s*1103515245u+12345u;\n"
     "      b[i]=(unsigned char)(s>>9); sb[i]=(signed char)(s>>17); w[i]=(unsigned short)(s>>3); }\n"
     "    unsigned acc=0;\n"
     "    for(int i=0;i<24;i++) acc+= (unsigned)b[i] + (unsigned)(int)sb[i] + (unsigned)w[i];\n"
     "    h=h*131u+acc; h^=h>>12; }\n"
     "  return ("+t+")h; }\n",
     {0xA2u}, "OptStress62", 2},

    // s8->s16->s32->s64 / u8->u64 promotion chains.
    {p+"_promchain",
     t+" "+p+"_promchain("+t+" a){\n"
     "  unsigned long long h=0; unsigned s=(unsigned)a;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    signed char c=(signed char)s; short sh=c; int w=sh; long long q=w;\n"
     "    unsigned char uc=(unsigned char)(s>>8); unsigned long long uq=uc;\n"
     "    q = q*3 - (long long)uq; q ^= (q>>20);\n"
     "    int n=(int)(s>>3); short tn=(short)n; signed char bn=(signed char)tn;\n"
     "    h += (unsigned long long)q + (unsigned long long)(bn+128); h^=h>>23; }\n"
     "  h^=h>>32; return ("+t+")h; }\n",
     {0xA3u}, "OptStress62", 2},

    // Intentional unsigned wrap at 8/16/32 width boundaries.
    {p+"_wrapwidth",
     t+" "+p+"_wrapwidth("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0; unsigned char c8=0; unsigned short c16=0; unsigned c32=0;\n"
     "  for(int i=0;i<400;i++){ s=s*1103515245u+12345u;\n"
     "    c8 = (unsigned char)(c8 + (s>>5));\n"
     "    c16 = (unsigned short)(c16*3u + (s>>7));\n"
     "    c32 = c32*2654435761u + s;\n"
     "    h=h*131u+c8+c16+(c32>>24); h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0xA4u}, "OptStress62", 2},

    // Widening multiply then narrow the result back down.
    {p+"_mulnarrow",
     t+" "+p+"_mulnarrow("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned short x=(unsigned short)s, y=(unsigned short)(s>>11);\n"
     "    unsigned p32=(unsigned)x*(unsigned)y;\n"
     "    unsigned short pn=(unsigned short)(p32>>3);\n"
     "    signed char sa=(signed char)s, sbb=(signed char)(s>>13);\n"
     "    int sp=(int)sa*(int)sbb; unsigned char spn=(unsigned char)sp;\n"
     "    unsigned long long w=(unsigned long long)s*0x9E3779B1ull; unsigned wn=(unsigned)(w>>20);\n"
     "    h=h*131u+pn+spn+(wn&0xffff); h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0xA5u}, "OptStress62", 2},

    // Multiprecision add/sub via 16-bit limbs (carry across limbs).
    {p+"_carrylimb",
     t+" "+p+"_carrylimb("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<150;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned short A[6],B[6],C[6];\n"
     "    for(int i=0;i<6;i++){ s=s*1103515245u+12345u; A[i]=(unsigned short)(s>>8); B[i]=(unsigned short)(s>>15); }\n"
     "    unsigned carry=0;\n"
     "    for(int i=0;i<6;i++){ unsigned t2=(unsigned)A[i]+(unsigned)B[i]+carry; C[i]=(unsigned short)t2; carry=t2>>16; }\n"
     "    unsigned borrow=0;\n"
     "    for(int i=0;i<6;i++){ int d=(int)A[i]-(int)B[i]-(int)borrow; if(d<0){d+=0x10000; borrow=1;} else borrow=0; C[i]^=(unsigned short)d; }\n"
     "    unsigned acc=carry+borrow;\n"
     "    for(int i=0;i<6;i++) acc=acc*131u+C[i];\n"
     "    h=h*1311u+acc; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0xA6u}, "OptStress62", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress62TC("x64o62", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress62TC("x86o62", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress62TC("a64o62", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress62TC("armo62", "int");

INSTANTIATE_TEST_SUITE_P(OptStress62, X64OptStress62RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress62, X86OptStress62RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress62, A64OptStress62RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress62, ARM32OptStress62RT, ::testing::ValuesIn(kARM), rtTCName);
