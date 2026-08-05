//===- AllPlatform_ConstPoolRTTests.cpp - rodata constant-pool ---*- C++ -*-=//
//
// Constant-pool / rodata lookup-table stress probes.  Each kernel embeds a
// LARGE (256-entry) `static const` table that clang lowers to .rodata and
// indexes inside a loop, exercising the emitter's table-redirection path
// (collectIndexedGlobalBase / tryResolveGlobalData → whole-segment rodata
// global + GEP).  The base reaches the table via a relocation:
//   * x64    `lea tbl(%rip),%reg` + scaled `mov (%reg,idx,N)`
//   * aarch64 `adrp`+`add` page/offset pair
//   * arm32   literal-pool `ldr rN,[pc]` + indexed load
// The recompiled image must re-embed the table and re-point the base; a
// regression reads the original (unmapped) VA → UC_ERR_READ_UNMAPPED or a
// stale/zero value.  Element widths (u8/u16/u32/s16) cover scaled indexing and
// sign-extended loads; the 2D kernel covers a base nested under row*stride+col.
//
// All arithmetic is bounded 32-bit with constant shifts so nothing lowers to a
// libcall Unicorn lacks; the harness compares native vs lifted folded returns.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ConstPoolRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ConstPoolRT, Verify) { roundTripX64(GetParam()); }

class A64ConstPoolRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ConstPoolRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32ConstPoolRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ConstPoolRT, Verify) { roundTripARM32(GetParam()); }

// Emit a comma-separated initializer of `N` values produced by `fn(i)`.
template <typename Fn>
static std::string tableInit(int N, Fn fn) {
  std::string s;
  for (int i = 0; i < N; ++i) {
    if (i)
      s += ",";
    s += std::to_string(fn(i));
  }
  return s;
}

// clang-format off
static std::vector<RoundTripTC> makeCPTC(const char *prefix, const char *T,
                                         int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;

  // Reflected CRC-16 (poly 0xA001) byte table — 256 u16 entries.
  std::string crc16Tab = tableInit(256, [](int i) {
    unsigned c = (unsigned)i;
    for (int b = 0; b < 8; ++b)
      c = (c & 1u) ? ((c >> 1) ^ 0xA001u) : (c >> 1);
    return c & 0xFFFFu;
  });
  // CRC-32 (poly 0xEDB88320) byte table — 256 u32 entries.
  std::string crc32Tab = tableInit(256, [](int i) {
    unsigned c = (unsigned)i;
    for (int b = 0; b < 8; ++b)
      c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    return c;
  });
  // 256-entry byte S-box (affine-ish permutation), fits .rodata as bytes.
  std::string sbox = tableInit(256, [](int i) {
    unsigned x = (unsigned)i;
    x = ((x * 167u + 13u) ^ (x >> 3)) & 0xFFu;
    return x;
  });
  // Signed 16-bit table (mix of negatives) — load must sign-extend.
  std::string stab = tableInit(256, [](int i) {
    return (int)(short)((i * 277 - 35000) & 0xFFFF);
  });

  return {
    // Table-driven CRC-16: 256-entry u16 table, scaled index, byte feed.
    {p+"_crc16tab",
     t+" "+p+"_crc16tab("+t+" a){\n"
     "  static const unsigned short tab[256]={"+crc16Tab+"};\n"
     "  unsigned crc=0xFFFFu;\n"
     "  for(int k=0;k<128;k++){\n"
     "    unsigned char b=(unsigned char)(a*131u+k*7u);\n"
     "    crc=(crc>>8)^tab[(crc^b)&0xFFu]; }\n"
     "  return ("+t+")(crc&0xFFFFu);\n"
     "}\n",
     {0x1234567ULL}, "ConstPool", opt, fl},

    // Table-driven CRC-32: 256-entry u32 table (dword scaled index).
    {p+"_crc32tab",
     t+" "+p+"_crc32tab("+t+" a){\n"
     "  static const unsigned tab[256]={"+crc32Tab+"};\n"
     "  unsigned crc=0xFFFFFFFFu;\n"
     "  for(int k=0;k<128;k++){\n"
     "    unsigned char b=(unsigned char)(a*2654435761u+k*7u);\n"
     "    crc=(crc>>8)^tab[(crc^b)&0xFFu]; }\n"
     "  return ("+t+")(crc^0xFFFFFFFFu);\n"
     "}\n",
     {0x2233445ULL}, "ConstPool", opt, fl},

    // 256-entry byte S-box substitution-permutation network.
    {p+"_sbox",
     t+" "+p+"_sbox("+t+" a){\n"
     "  static const unsigned char sb[256]={"+sbox+"};\n"
     "  unsigned acc=0; unsigned char st=(unsigned char)a;\n"
     "  for(int k=0;k<200;k++){\n"
     "    st=sb[(st+k)&0xFF];\n"
     "    st=sb[st^(unsigned char)(k*3)];\n"
     "    acc=acc*131u+st; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "ConstPool", opt, fl},

    // 2D table access: base nested under row*16 + col (multi-dim index).
    {p+"_tab2d",
     t+" "+p+"_tab2d("+t+" a){\n"
     "  static const unsigned char m[256]={"+sbox+"};\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<160;k++){\n"
     "    unsigned r=(unsigned)(a*(k+1))&15u, c=(unsigned)(a*3u+k)&15u;\n"
     "    acc=acc*131u+m[r*16u+c]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "ConstPool", opt, fl},

    // Signed 16-bit table: indexed load must sign-extend the negative entries.
    {p+"_signedtab",
     t+" "+p+"_signedtab("+t+" a){\n"
     "  static const short st[256]={"+stab+"};\n"
     "  int acc=0;\n"
     "  for(int k=0;k<200;k++){\n"
     "    int v=st[(unsigned)(a*7+k)&0xFF];\n"
     "    acc=acc*31+v; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "ConstPool", opt, fl},

    // Two tables of different element width indexed in the same loop: each base
    // must independently redirect into its own rodata global.
    {p+"_twowidth",
     t+" "+p+"_twowidth("+t+" a){\n"
     "  static const unsigned char b8[256]={"+sbox+"};\n"
     "  static const unsigned w32[256]={"+crc32Tab+"};\n"
     "  unsigned acc=0;\n"
     "  for(int k=0;k<200;k++){\n"
     "    unsigned idx=(unsigned)(a*(k+1))&0xFFu;\n"
     "    acc=acc*131u+b8[idx]+w32[(idx^k)&0xFFu]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "ConstPool", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64CP = makeCPTC("x64cp", "long", 2, "");
static const std::vector<RoundTripTC> kA64CP = makeCPTC("a64cp", "long", 2, "");
static const std::vector<RoundTripTC> kARMCP = makeCPTC("armcp", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(ConstPool, X64ConstPoolRT,
                         ::testing::ValuesIn(kX64CP), rtTCName);
INSTANTIATE_TEST_SUITE_P(ConstPool, A64ConstPoolRT,
                         ::testing::ValuesIn(kA64CP), rtTCName);
INSTANTIATE_TEST_SUITE_P(ConstPool, ARM32ConstPoolRT,
                         ::testing::ValuesIn(kARMCP), rtTCName);
