//===- X64_HighByteRTTests.cpp - AH/BH/CH/DH sub-register aliasing -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The existing sub-register stress tests cover the low-byte (AL, offset 0)
// alias of a wide register.  x86 also has the *high-byte* registers AH/BH/CH/DH
// which alias bits [15:8] (offset +1) of their parent.  Writing AH then reading
// AX/EAX, or byte division (which writes the quotient to AL and the remainder to
// AH), exercises an offset-1 sub-register alias the optimizer must model.  These
// probes pin the exact instruction stream with inline asm (and a couple of pure
// -C byte div/mod forms that clang lowers to `divb` + an AH read).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64HighByteRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64HighByteRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // divb: AX / divisor -> AL=quotient, AH=remainder.  Read both halves of AX.
  // (1000/7=142 fits in AL; 1000%7=6 in AH.)
  {"divb_al_ah",
   "long f(long a,long b){unsigned short dvd=(unsigned short)a;"
   "unsigned char dvs=(unsigned char)b;unsigned short ax;"
   "__asm__ volatile(\"divb %[d]\":\"=a\"(ax):\"a\"(dvd),[d]\"q\"(dvs):\"cc\");"
   "return (long)(ax&0xFF)*256+((ax>>8)&0xFF);}\n",
   {1000, 7}, "HighByte", 0},

  // idivb: signed byte division -> AL=quotient, AH=remainder.
  // (-100/7=-14 fits in signed AL; -100%7=-2 in AH.)
  {"idivb_al_ah",
   "long f(long a,long b){short dvd=(short)a;signed char dvs=(signed char)b;"
   "unsigned short ax;"
   "__asm__ volatile(\"idivb %[d]\":\"=a\"(ax):\"a\"(dvd),[d]\"q\"(dvs):\"cc\");"
   "return (long)(ax&0xFF)*256+((ax>>8)&0xFF);}\n",
   {(uint64_t)(-100), 7}, "HighByte", 0},

  // mulb: AX = AL * r/m8 (unsigned).  Reads AL, writes full AX.
  {"mulb_ax",
   "long f(long a,long b){unsigned char al=(unsigned char)a;"
   "unsigned char m=(unsigned char)b;unsigned short ax;"
   "__asm__ volatile(\"mulb %[m]\":\"=a\"(ax):\"a\"(al),[m]\"q\"(m):\"cc\");"
   "return ax;}\n",
   {200, 200}, "HighByte", 0},

  // imulb: AX = AL * r/m8 (signed).
  {"imulb_ax",
   "long f(long a,long b){signed char al=(signed char)a;"
   "signed char m=(signed char)b;short ax;"
   "__asm__ volatile(\"imulb %[m]\":\"=a\"(ax):\"a\"(al),[m]\"q\"(m):\"cc\");"
   "return (long)ax;}\n",
   {(uint64_t)(-100), 100}, "HighByte", 0},

  // Write AH directly, then read the full AX (offset-1 alias of AX).
  {"ah_write_read_ax",
   "long f(long a){unsigned short ax=0x1234;unsigned char h=(unsigned char)a;"
   "__asm__ volatile(\"movb %[h],%%ah\":\"+a\"(ax):[h]\"q\"(h):);"
   "return ax;}\n",
   {0x56}, "HighByte", 0},

  // Read AH as a source operand: AL = AL + AH.
  {"ah_as_source_add",
   "long f(long a){unsigned short ax=(unsigned short)a;"
   "__asm__ volatile(\"addb %%ah,%%al\":\"+a\"(ax)::\"cc\");"
   "return ax&0xFF;}\n",
   {0x3412}, "HighByte", 0},

  // AH and AL written independently, then AX read as a 16-bit whole.
  {"al_ah_compose_ax",
   "long f(long a,long b){unsigned short ax;"
   "unsigned char lo=(unsigned char)a,hi=(unsigned char)b;"
   "__asm__ volatile(\"movb %[l],%%al\\n\\tmovb %[h],%%ah\""
   ":\"=a\"(ax):[l]\"q\"(lo),[h]\"q\"(hi):);"
   "return ax;}\n",
   {0x78, 0x9A}, "HighByte", 0},

  // EAX written wide, then only AH overwritten; read full EAX (the [31:16] and
  // [7:0] halves must survive while [15:8] changes).
  {"eax_then_ah",
   "long f(long a,long b){unsigned int eax=(unsigned int)a;"
   "unsigned char hi=(unsigned char)b;"
   "__asm__ volatile(\"movb %[h],%%ah\":\"+a\"(eax):[h]\"q\"(hi):);"
   "return eax;}\n",
   {0xDEADBEEFu, 0x55}, "HighByte", 0},

  // Pure-C unsigned byte div/mod: clang lowers to one `divb` and reads AL + AH.
  {"c_byte_divmod",
   "long f(long a,long b){unsigned char x=(unsigned char)a,y=(unsigned char)b;"
   "return (long)(x/y)*256+(x%y);}\n",
   {233, 9}, "HighByte", 0},

  // Pure-C signed byte div/mod.
  {"c_byte_sdivmod",
   "long f(long a,long b){signed char x=(signed char)a,y=(signed char)b;"
   "return (long)(unsigned char)(x/y)*256+(unsigned char)(x%y);}\n",
   {(uint64_t)(-77), 5}, "HighByte", 0},

  // AH remainder fed straight into another computation (cross-alias dataflow).
  {"ah_rem_chain",
   "long f(long a,long b){unsigned short dvd=(unsigned short)a;"
   "unsigned char dvs=(unsigned char)b;unsigned short ax;"
   "__asm__ volatile(\"divb %[d]\":\"=a\"(ax):\"a\"(dvd),[d]\"q\"(dvs):\"cc\");"
   "unsigned char rem=(ax>>8)&0xFF;return (long)rem*rem;}\n",
   {1234, 100}, "HighByte", 0},

  // word division: DX:AX / divisor -> AX=quotient, DX=remainder, then read the
  // 32-bit EAX/EDX (a wider read of the size-2 AX/DX partial writes — same merge
  // path as the AH byte case).  100000/7=14285 (AX), 100000%7=5 (DX).
  {"divw_eax_edx",
   "long f(long a,long b){unsigned int dvd=(unsigned int)a;"
   "unsigned short dvs=(unsigned short)b;"
   "unsigned short lo=dvd&0xFFFF,hi=dvd>>16;unsigned int eax,edx;"
   "__asm__ volatile(\"divw %[d]\":\"=a\"(eax),\"=d\"(edx)"
   ":\"a\"(lo),\"d\"(hi),[d]\"r\"(dvs):\"cc\");"
   "return (long)(eax&0xFFFF)*65536+(edx&0xFFFF);}\n",
   {100000, 7}, "HighByte", 0},

  // signed word division: -100000/7=-14285 (AX), -100000%7=-3 (DX).
  {"idivw_eax_edx",
   "long f(long a,long b){int dvd=(int)a;short dvs=(short)b;"
   "unsigned short lo=(unsigned)dvd&0xFFFF,hi=(unsigned)dvd>>16;"
   "unsigned int eax,edx;"
   "__asm__ volatile(\"idivw %[d]\":\"=a\"(eax),\"=d\"(edx)"
   ":\"a\"(lo),\"d\"(hi),[d]\"r\"(dvs):\"cc\");"
   "return (long)(eax&0xFFFF)*65536+(edx&0xFFFF);}\n",
   {(uint64_t)(-100000), 7}, "HighByte", 0},

  // 1-operand word multiply: DX:AX = AX * r/m16.  Read EAX/EDX (wider than the
  // size-2 AX/DX partial writes).  50000*4=200000=0x30D40 -> AX=0x0D40 DX=0x3.
  {"mulw_eax_edx",
   "long f(long a,long b){unsigned short ax=(unsigned short)a;"
   "unsigned short m=(unsigned short)b;unsigned int eax,edx;"
   "__asm__ volatile(\"mulw %[m]\":\"=a\"(eax),\"=d\"(edx):\"a\"(ax),[m]\"r\"(m):\"cc\");"
   "return (long)(eax&0xFFFF)+(long)(edx&0xFFFF)*65536;}\n",
   {50000, 4}, "HighByte", 0},

  // 1-operand signed word multiply: DX:AX = AX * r/m16.  -30000*3=-90000.
  {"imulw_eax_edx",
   "long f(long a,long b){short ax=(short)a;short m=(short)b;"
   "unsigned int eax,edx;"
   "__asm__ volatile(\"imulw %[m]\":\"=a\"(eax),\"=d\"(edx):\"a\"(ax),[m]\"r\"(m):\"cc\");"
   "return (long)(int)(((edx&0xFFFF)<<16)|(eax&0xFFFF));}\n",
   {(uint64_t)(-30000), 3}, "HighByte", 0},

  // Reverse direction: a full RAX write, then read AH (the [15:8] byte).  This
  // is the wide-write -> narrow offset-1 read path (complement of the divb case).
  {"rax_to_ah",
   "long f(long a){unsigned long rax=(unsigned long)a;unsigned char ah;"
   "__asm__ volatile(\"movb %%ah,%0\":\"=q\"(ah):\"a\"(rax):);"
   "return (long)ah;}\n",
   {0x123456789ABCDEF0ULL}, "HighByte", 0},

  // Compute a fresh RAX, then read AH from it: forces an SSA wide write feeding
  // an offset-1 narrow read.
  {"rax_compute_ah",
   "long f(long a,long b){unsigned char ah;"
   "__asm__ volatile(\"movq %[x],%%rax\\n\\taddq %[y],%%rax\\n\\tmovb %%ah,%[h]\""
   ":[h]\"=q\"(ah):[x]\"r\"((unsigned long)a),[y]\"r\"((unsigned long)b):\"rax\");"
   "return (long)ah;}\n",
   {0x1100, 0x0034}, "HighByte", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(HighByte, X64HighByteRT, ::testing::ValuesIn(kX64),
                         rtTCName);
