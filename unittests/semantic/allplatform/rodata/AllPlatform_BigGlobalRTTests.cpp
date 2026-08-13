//===- AllPlatform_BigGlobalRTTests.cpp - writable global > 4 KiB --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Drives the #531 KNOWN-OPEN: a writable global larger than the per-constant
// embed cap (kMaxEmbeddedDataLen = 4096) was not embedded as a single mutable
// global, so its accesses fell back to a bare `inttoptr(original VA)` that the
// relinked object never maps -> WRITE_UNMAPPED on x86-64/i386 (ARM32 only
// happened to land by image-layout luck).  The read-only constant pool (.text/
// .rodata) was already fixed in #531 via embedRodataRun/embedExecSegmentRun;
// this exercises the WRITABLE .bss/.data counterpart.
//
//   * _bigbss  : 8 KiB zero-init .bss array, dense fill then reduction.
//   * _bighist : 8 KiB .bss histogram, data-dependent SCATTER writes.
//   * _bigrmw  : 8 KiB .bss array, self-referential prefix RMW G[i]+=G[i-1].
//   * _bigdata : 8 KiB initialized .data array (D[0] seeded), in-place RMW so a
//                wrong .data initializer or mapping surfaces.
//
// libcall-free on i386/ARM32: 32-bit mul/add/xor/shift only (no 64-bit divide,
// no aggregate memset/memcpy — the arrays are .bss zero-init or .data static).
// Deterministic LCG seed; folds the whole array into one scalar return so a
// dropped/misplaced global surfaces.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64BigGlobalRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64BigGlobalRT, Verify) { roundTripX64(GetParam()); }
class X86BigGlobalRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86BigGlobalRT, Verify) { roundTripX86(GetParam()); }
class A64BigGlobalRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64BigGlobalRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32BigGlobalRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32BigGlobalRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeBigGlobalTC(const char *prefix) {
  std::string p = prefix;
  std::vector<RoundTripTC> v = {
    // 8 KiB zero-init .bss: dense fill then serial reduction.
    {p+"_bigbss",
     "unsigned "+p+"_bigbss(unsigned a){\n"
     "  static unsigned G[2048];\n"
     "  unsigned w=a^0x12345678u;\n"
     "  for(int i=0;i<2048;i++){ w=w*1103515245u+12345u; G[i]=w; }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<2048;i++){ s+=G[i]; s^=s>>7; }\n"
     "  return s; }\n",
     {0x1234u}, "BigGlobal", 2, "-O2"},

    // 8 KiB .bss histogram: data-dependent scatter writes.
    {p+"_bighist",
     "unsigned "+p+"_bighist(unsigned a){\n"
     "  static unsigned H[2048];\n"
     "  unsigned w=a^0x9e3779b9u;\n"
     "  for(int i=0;i<4000;i++){ w=w*1664525u+1013904223u; H[(w>>11)&2047]++; }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<2048;i++){ s=s*31u+H[i]; }\n"
     "  return s; }\n",
     {0x2345u}, "BigGlobal", 2, "-O2"},

    // 8 KiB .bss: self-referential prefix RMW (each store reads the prior cell).
    {p+"_bigrmw",
     "unsigned "+p+"_bigrmw(unsigned a){\n"
     "  static unsigned G[2048];\n"
     "  unsigned w=a|1u;\n"
     "  for(int i=0;i<2048;i++){ w=w*22695477u+1u; G[i]=w; }\n"
     "  for(int i=1;i<2048;i++){ G[i]=G[i]+G[i-1]*3u; }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<2048;i++){ s^=G[i]+(unsigned)i; }\n"
     "  return s; }\n",
     {0x3456u}, "BigGlobal", 2, "-O2"},

    // 8 KiB initialized .data (D[0] seeded): in-place RMW, then reduce.
    {p+"_bigdata",
     "unsigned "+p+"_bigdata(unsigned a){\n"
     "  static unsigned D[2048]={1u};\n"
     "  unsigned w=a^0x55555555u;\n"
     "  for(int i=0;i<2048;i++){ w=w*1103515245u+12345u; D[i]+=w; }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<2048;i++){ s=s*33u+D[i]; }\n"
     "  return s; }\n",
     {0x4567u}, "BigGlobal", 2, "-O2"},

    // 8 KiB BYTE .bss histogram: index mask is the size-1 directly (0x1FFF, a
    // contiguous low-bit mask) — colliding with the run's VA range.
    {p+"_bigbyte",
     "unsigned "+p+"_bigbyte(unsigned a){\n"
     "  static unsigned char H[8192];\n"
     "  unsigned w=a^0xabcdef01u;\n"
     "  for(int i=0;i<6000;i++){ w=w*1664525u+1013904223u; H[(w>>11)&8191]++; }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<8192;i++){ s=s*31u+H[i]; }\n"
     "  return s; }\n",
     {0x5678u}, "BigGlobal", 2, "-O2"},

    // 8 KiB SHORT .bss histogram: byte-offset mask is `(2^12-1)<<1` = 0x1FFE, a
    // SHIFTED mask (not 2^k-1) — exercises the scaled-index-mask collision the
    // contiguous-low-bit guard alone does not catch.
    {p+"_bigshort",
     "unsigned "+p+"_bigshort(unsigned a){\n"
     "  static unsigned short H[4096];\n"
     "  unsigned w=a^0x13572468u;\n"
     "  for(int i=0;i<6000;i++){ w=w*1664525u+1013904223u; H[(w>>11)&4095]++; }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<4096;i++){ s=s*31u+H[i]; }\n"
     "  return s; }\n",
     {0x6789u}, "BigGlobal", 2, "-O2"},

    // Two distinct large writable globals: each access must route to its OWN run
    // (a cross-segment base must not be confused with the other's range).
    {p+"_bigtwo",
     "unsigned "+p+"_bigtwo(unsigned a){\n"
     "  static unsigned A[2048];\n"
     "  static unsigned B[2048];\n"
     "  unsigned w=a^0x0f1e2d3cu;\n"
     "  for(int i=0;i<2048;i++){ w=w*1103515245u+12345u; A[i]=w; B[i]=w^(w>>13); }\n"
     "  unsigned s=0;\n"
     "  for(int i=0;i<2048;i++){ s+=A[i]*3u+B[i]; s^=s>>11; }\n"
     "  return s; }\n",
     {0x789au}, "BigGlobal", 2, "-O2"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeBigGlobalTC("x64big");
static const std::vector<RoundTripTC> kX86 = makeBigGlobalTC("x86big");
static const std::vector<RoundTripTC> kA64 = makeBigGlobalTC("a64big");
static const std::vector<RoundTripTC> kARM = makeBigGlobalTC("armbig");

INSTANTIATE_TEST_SUITE_P(BigGlobal, X64BigGlobalRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(BigGlobal, X86BigGlobalRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(BigGlobal, A64BigGlobalRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(BigGlobal, ARM32BigGlobalRT, ::testing::ValuesIn(kARM), rtTCName);
