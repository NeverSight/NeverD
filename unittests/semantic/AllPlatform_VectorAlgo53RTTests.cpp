//===- AllPlatform_VectorAlgo53RTTests.cpp - i64-element reduce → switch --===//
//
// Fifty-third batch of clang -O2 vector probes sitting at the intersection of
// three independently-fragile mechanisms, a combination no prior batch drove:
//   * i64-ELEMENT vector reduction — PADDQ (x86-64), .2d (AArch64), or VADD.i64
//     D-register PAIRS (ARM32, the #532 bug① / b49d8f6 mixed-half machinery).
//   * the HIGH 32 bits of the reduced i64 used as the dispatch selector —
//     `(unsigned)(sum>>32)&7` lives in the high pair half (EDX-equiv / upper D),
//     exactly the #533 OptStress336 "high-half selector" story but sourced from a
//     VECTOR horizontal reduction rather than a scalar loop accumulator.
//   * a switch JUMP TABLE reached straight off the reduction epilogue (#534
//     VectorAlgo52 / #532 bug② jump-table-base folding past vector intrinsics).
//
// VectorAlgo52 fed a 32-bit/byte reduction (low bits) into a switch; OptStress336
// fed a scalar i64 high half into a switch.  Nothing has fed the HIGH half of a
// VECTOR-reduced i64 into a jump table — so the resolver must trace the index
// back through both the i64 horizontal-reduction extract AND the high-half pick.
//
// Each kernel threads a loop-carried u64 accumulator through the switch arms and
// mutates the source array per outer iteration (touching high bits) so successive
// reductions and selected arms differ; folds both i64 halves into one return so a
// dropped/duplicated high word surfaces.  i64 fill is composed from two u32
// halves; all i64 math is add/sub/xor/and + CONSTANT shifts (no i64 multiply,
// divide, or variable shift) so ARM32 stays libcall-free.  x64 uses -msse4.2;
// a64/arm32 use the default NEON baseline.  Three targets (i386 skipped: no
// native i64 SIMD, matching VectorAlgo45-52).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo53RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo53RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo53RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo53RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo53RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo53RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec53TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // i64-element add reduction → switch on the HIGH half (sum>>32)&7: the high
    // pair register / upper D-lane feeds the jump-table index.
    {p+"_r64hi",
     t+" "+p+"_r64hi("+t+" a){\n"
     "  unsigned long long x[64]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ s=s*1664525u+1013904223u; unsigned lo=s;\n"
     "    s=s*1664525u+1013904223u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int r=0;r<40;r++){\n"
     "    unsigned long long sum=0;\n"
     "    for(int i=0;i<64;i++) sum += x[i];\n"
     "    switch((unsigned)(sum>>32)&7u){\n"
     "      case 0: acc += sum; break;\n"
     "      case 1: acc ^= sum<<3; break;\n"
     "      case 2: acc -= sum>>1; break;\n"
     "      case 3: acc += sum<<11; break;\n"
     "      case 4: acc ^= (sum>>32); break;\n"
     "      case 5: acc -= sum; break;\n"
     "      case 6: acc += sum<<40; break;\n"
     "      default: acc ^= sum<<17; break; }\n"
     "    x[r & 63] = acc ^ ((unsigned long long)(unsigned)r<<40);\n"
     "    acc ^= acc>>29; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo53", opt, fl},

    // i64-element xor reduction → 16-way DENSE table on the high half (sum>>32)&15.
    {p+"_r64dense",
     t+" "+p+"_r64dense("+t+" a){\n"
     "  unsigned long long x[64]; unsigned s=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; unsigned lo=s;\n"
     "    s=s*1103515245u+12345u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int r=0;r<36;r++){\n"
     "    unsigned long long red=0;\n"
     "    for(int i=0;i<64;i++) red ^= x[i] + (unsigned long long)i;\n"
     "    switch((unsigned)(red>>32)&15u){\n"
     "      case 0: acc+=red; break;\n"
     "      case 1: acc^=red<<2; break;\n"
     "      case 2: acc-=red<<4; break;\n"
     "      case 3: acc+=red<<33; break;\n"
     "      case 4: acc^=red<<6; break;\n"
     "      case 5: acc-=red; break;\n"
     "      case 6: acc+=red<<8; break;\n"
     "      case 7: acc^=(red>>32); break;\n"
     "      case 8: acc+=red<<10; break;\n"
     "      case 9: acc^=red<<48; break;\n"
     "      case 10: acc-=red<<3; break;\n"
     "      case 11: acc+=red<<35; break;\n"
     "      case 12: acc^=red<<12; break;\n"
     "      case 13: acc-=red<<1; break;\n"
     "      case 14: acc+=red<<14; break;\n"
     "      default: acc^=red>>17; break; }\n"
     "    x[r & 63] = acc + ((unsigned long long)(unsigned)r<<32);\n"
     "    acc ^= acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x2345678ULL}, "VectorAlgo53", opt, fl},

    // Selector mixes BOTH halves of the reduced i64: (red ^ (red>>32))&7 — both
    // pair registers / both D-lanes feed the dispatch index before the jump.
    {p+"_r64xor",
     t+" "+p+"_r64xor("+t+" a){\n"
     "  unsigned long long x[64], y[64]; unsigned s=(unsigned)a+0x77u;\n"
     "  for(int i=0;i<64;i++){ s=s*1664525u+1013904223u; unsigned lo=s;\n"
     "    s=s*1664525u+1013904223u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo;\n"
     "    s=s*1664525u+1013904223u; lo=s; s=s*1664525u+1013904223u; hi=s;\n"
     "    y[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int r=0;r<40;r++){\n"
     "    unsigned long long red=0;\n"
     "    for(int i=0;i<64;i++) red += (x[i]^y[i]);\n"
     "    switch((unsigned)(red ^ (red>>32))&7u){\n"
     "      case 0: acc += red<<9; break;\n"
     "      case 1: acc ^= red; break;\n"
     "      case 2: acc -= red<<2; break;\n"
     "      case 3: acc += red<<34; break;\n"
     "      case 4: acc ^= red<<19; break;\n"
     "      case 5: acc += (red>>32); break;\n"
     "      case 6: acc -= red; break;\n"
     "      default: acc ^= red<<25; break; }\n"
     "    x[r & 63] ^= acc; y[r & 63] += (acc>>17);\n"
     "    acc ^= acc>>31; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x3456789ULL}, "VectorAlgo53", opt, fl},

    // i64 reduction with a 64-bit-immediate add (NEON i64 constant pool, #531) per
    // element, high-half selector, and per-arm mixed-width recombine (#532 bug①).
    {p+"_r64imm",
     t+" "+p+"_r64imm("+t+" a){\n"
     "  unsigned long long x[64]; unsigned s=(unsigned)a+0x55u;\n"
     "  for(int i=0;i<64;i++){ s=s*22695477u+1u; unsigned lo=s;\n"
     "    s=s*22695477u+1u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int r=0;r<36;r++){\n"
     "    unsigned long long red=0;\n"
     "    for(int i=0;i<64;i++) red += x[i] + 0x0123456789abcdefULL;\n"
     "    switch((unsigned)(red>>32)&7u){\n"
     "      case 0: { unsigned low=(unsigned)red ^ 0xdeadbeefu;\n"
     "                acc += ((unsigned long long)(unsigned)(red>>32)<<32)|low; break; }\n"
     "      case 1: acc ^= red<<5; break;\n"
     "      case 2: acc -= red>>3; break;\n"
     "      case 3: acc += red<<32; break;\n"
     "      case 4: acc ^= (red>>32)^(unsigned)red; break;\n"
     "      case 5: acc -= red<<1; break;\n"
     "      case 6: acc += red<<40; break;\n"
     "      default: acc ^= red; break; }\n"
     "    x[r & 63] = acc ^ ((unsigned long long)(unsigned)(r*7)<<32);\n"
     "    acc ^= acc>>27; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x456789AULL}, "VectorAlgo53", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec53 =
    makeVec53TC("x64v53", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec53 =
    makeVec53TC("a64v53", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec53 =
    makeVec53TC("armv53", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo53, X64VectorAlgo53RT,
                         ::testing::ValuesIn(kX64Vec53), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo53, A64VectorAlgo53RT,
                         ::testing::ValuesIn(kA64Vec53), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo53, ARM32VectorAlgo53RT,
                         ::testing::ValuesIn(kARM32Vec53), rtTCName);
