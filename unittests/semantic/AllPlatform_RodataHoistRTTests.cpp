//===- AllPlatform_RodataHoistRTTests.cpp - hoisted rodata table -*- C++ -*-=//
//
// A read-only lookup table whose base clang hoists into a register OUTSIDE the
// loop (`lea rcx,[rip+tbl]` / `adrp`+`add` / `ldr rN,[pc]`) and then indexes
// inside the loop (`movzbl (rcx,idx)`).  When the SAME function also has a
// runtime-indexed store to a stack array, the array's negative frame
// displacements used to poison the emitter's StoredConstBases guard, disabling
// ALL table redirection — the hoisted table load degraded to a bare
// `inttoptr <absolute>` reading unmapped memory in the recompiled image (table
// reads as 0).  The fix keys redirection on the base being a real read-only
// data symbol (the .o's rodata reference goes through a relocation to it), which
// a frame-synthesized stack address never is.  These probes pair a hoisted
// rodata table with a runtime-indexed stack array so any regression of either
// the redirect (table→0) or the array protection (store/load split) diverges
// from the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RodataHoistRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RodataHoistRT, Verify) { roundTripX64(GetParam()); }

class A64RodataHoistRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RodataHoistRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RodataHoistRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RodataHoistRT, Verify) { roundTripARM32(GetParam()); }

// The base64 alphabet — a 64-byte read-only string clang lowers to a
// .rodata symbol referenced by relocation.
#define B64TAB \
  "  static const char tbl[]=\"ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
  "abcdefghijklmnopqrstuvwxyz0123456789+/\";\n"

// clang-format off
static std::vector<RoundTripTC> makeTC(const char *prefix, const char *T,
                                       int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // The original bug: fill a stack array by runtime index, then index the
    // hoisted rodata table inside a loop.  The array stores poison the guard;
    // the table read must still redirect to the rodata global.
    {p+"_b64arr",
     t+" "+p+"_b64arr("+t+" a){\n"
     "  unsigned char in[64];\n"
     "  for(int i=0;i<64;i++) in[i]=(unsigned char)((a*(i+1))>>3);\n"
     B64TAB
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<64;i++) acc=acc*31u+(unsigned char)tbl[in[i]&63];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "RodataHoist", opt, fl},
    // Two coexisting arrays (read-write) plus the hoisted table: the modal byte
    // distribution indexes the table while a scratch array is stored/reloaded.
    {p+"_tblhist",
     t+" "+p+"_tblhist("+t+" a){\n"
     "  unsigned char buf[48]; int h[16];\n"
     "  for(int i=0;i<16;i++) h[i]=0;\n"
     "  for(int i=0;i<48;i++) buf[i]=(unsigned char)((a+i*7)>>2);\n"
     B64TAB
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<48;i++){ unsigned idx=buf[i]&63; h[idx&15]++;\n"
     "    acc=acc*131u+(unsigned char)tbl[idx]; }\n"
     "  for(int i=0;i<16;i++) acc=acc*7u+(unsigned)h[i];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "RodataHoist", opt, fl},
    // Regression control: table only, no stack array (must already redirect,
    // like a CRC table) — confirms the symbol-keyed path matches old behaviour.
    {p+"_tblonly",
     t+" "+p+"_tblonly("+t+" a){\n"
     B64TAB
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<10;i++) acc=acc*31u+(unsigned char)tbl[(a>>(i*3))&63];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "RodataHoist", opt, fl},
    // Two distinct hoisted tables indexed in the same loop + a stack array: each
    // base must be independently recognized as a read-only data symbol.
    {p+"_twotab",
     t+" "+p+"_twotab("+t+" a){\n"
     "  unsigned char in[48];\n"
     "  for(int i=0;i<48;i++) in[i]=(unsigned char)((a*(i+1))>>2);\n"
     B64TAB
     "  static const unsigned char w[16]={3,5,7,11,13,17,19,23,"
     "29,31,37,41,43,47,53,59};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<48;i++){ unsigned idx=in[i]&63;\n"
     "    acc=acc*31u+(unsigned char)tbl[idx]+w[idx&15]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2468ACEULL}, "RodataHoist", opt, fl},
    // 32-bit-element table indexed (dword table, GEP by scaled byte index) +
    // a stack array store loop poisoning the guard.
    {p+"_dwtab",
     t+" "+p+"_dwtab("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*5)>>1);\n"
     "  static const unsigned tab[16]={0x11111111u,0x22222222u,0x33333333u,\n"
     "    0x44444444u,0x55555555u,0x66666666u,0x77777777u,0x88888888u,\n"
     "    0x99999999u,0xAAAAAAAAu,0xBBBBBBBBu,0xCCCCCCCCu,0xDDDDDDDDu,\n"
     "    0xEEEEEEEEu,0xFFFFFFFFu,0x12345678u};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++) acc=acc*131u+tab[in[i]&15];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x13579BDULL}, "RodataHoist", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeTC("x64rh", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeTC("a64rh", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeTC("armrh", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(RodataHoist, X64RodataHoistRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataHoist, A64RodataHoistRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataHoist, ARM32RodataHoistRT,
                         ::testing::ValuesIn(kARM), rtTCName);
