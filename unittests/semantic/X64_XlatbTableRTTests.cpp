//===- X64_XlatbTableRTTests.cpp - XLATB table lookup ------------*- C++ -*-=//
//
// x86 `XLATB` does `AL = [RBX + ZeroExtend(AL)]`: it indexes a 256-byte table at
// RBX by the UNSIGNED byte in AL, replacing AL with the looked-up byte while
// leaving the rest of RAX untouched.  Two correctness hazards hide here:
//
//   1. AL must be ZERO-extended into the effective address — a sign-extend would
//      turn indices 0x80..0xFF into NEGATIVE offsets (reads BEFORE the table).
//   2. Only the AL byte is written; AH and the upper bytes of EAX/RAX must be
//      preserved (sub-register write, not a full-width store).
//
// The lifter forms `EA = RBX + ZEXT(AL)` and stores the loaded byte back into
// the 1-byte RAX sub-register, so both hazards are handled — but XLATB had ZERO
// prior roundtrip coverage.  Each probe builds a 256-entry table on the stack
// (tbl[i] = i*7+3, no rodata needed), seeds AL with an index, runs XLATB, and
// returns either the looked-up byte or the full EAX (to pin upper-byte
// preservation and that indexing uses AL only, not the wider register).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64XlatbTableRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64XlatbTableRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {

  // index 0 -> tbl[0] = 3.
  {"xlatb_idx0",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int al=(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(al):\"b\"(tbl):\"memory\");\n"
   "  return al&0xFF;}\n",
   {0}, "Xlatb"},

  // index 1 -> tbl[1] = 10.
  {"xlatb_idx1",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int al=(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(al):\"b\"(tbl):\"memory\");\n"
   "  return al&0xFF;}\n",
   {1}, "Xlatb"},

  // index 0x7F (just below the sign bit) -> tbl[127].
  {"xlatb_idx7f",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int al=(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(al):\"b\"(tbl):\"memory\");\n"
   "  return al&0xFF;}\n",
   {0x7F}, "Xlatb"},

  // index 0x80 (MSB set): ZERO-extend -> tbl[128]; a sign-extend would read
  // tbl-128 (RED).
  {"xlatb_idx80",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int al=(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(al):\"b\"(tbl):\"memory\");\n"
   "  return al&0xFF;}\n",
   {0x80}, "Xlatb"},

  // index 0xFF (max): ZERO-extend -> tbl[255].
  {"xlatb_idxff",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int al=(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(al):\"b\"(tbl):\"memory\");\n"
   "  return al&0xFF;}\n",
   {0xFF}, "Xlatb"},

  // Upper EAX bytes set going in (EAX = 0xAB00 | index): XLATB must index by AL
  // ONLY and leave AH (0xAB) intact.  Returning the full EAX pins both: AL=0x10
  // -> tbl[16]=0x73, AH stays 0xAB -> 0xAB73.
  {"xlatb_preserve_ah",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int eax=0xAB00u|(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(eax):\"b\"(tbl):\"memory\");\n"
   "  return eax;}\n",
   {0x10}, "Xlatb"},

  // Same preservation check with a high index byte (0xFF) and upper bits set:
  // EAX=0xAB00|0xFF, AL=0xFF -> tbl[255]; AH stays 0xAB.
  {"xlatb_preserve_ah_hi",
   "unsigned long f(unsigned long idx){\n"
   "  unsigned char tbl[256];\n"
   "  for(int i=0;i<256;i++) tbl[i]=(unsigned char)(i*7u+3u);\n"
   "  unsigned int eax=0xAB00u|(unsigned)(idx&0xFF);\n"
   "  __asm__ volatile(\"xlatb\\n\\t\":\"+a\"(eax):\"b\"(tbl):\"memory\");\n"
   "  return eax;}\n",
   {0xFF}, "Xlatb"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Xlatb, X64XlatbTableRT,
                         ::testing::ValuesIn(kX64), rtTCName);
