//===- AllPlatform_OptStress4RTTests.cpp - optimizer-path stressors -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fourth batch of high-yield roundtrip probes aimed at NeverD's own MedIR
// passes (MedFlags folding, MedDCE liveness, MedPropagation const/copy prop,
// LowToMed sub-register synthesis) plus the value emitter's wide-mul / div
// lowering.  These idioms are deliberately distinct from OptStress / 2 / 3:
// runtime-divisor div+rem pairs (real hardware divide, not strength-reduced),
// 64-bit high-half multiply (mulhi) extraction, count-leading-zeros
// normalization whose shift count can hit the bit width, saturating clamp via
// min/max, variable bit-field extract/insert, a stack histogram that forces
// load/store frame slots, and a comparison whose flag is produced in one block
// and consumed several blocks later.  Each kernel returns a value-dependent
// hash compiled at -O2; the roundtrip compares native vs lifted execution
// across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Opt4RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Opt4RT, Verify) { roundTripX64(GetParam()); }
class X86Opt4RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Opt4RT, Verify) { roundTripX86(GetParam()); }
class A64Opt4RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Opt4RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Opt4RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Opt4RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOpt4TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Runtime-divisor unsigned + signed divide AND remainder in the same loop.
    // The divisor is loop-carried (never constant) so clang must emit a real
    // div/udiv/sdiv (no magic-number strength reduction) — exercises the actual
    // hardware divide lift plus quotient/remainder pairing.  All 32-bit so i386
    // stays on native divl/idivl (never a 64-bit division libcall).
    {p+"_divrem",
     t+" "+p+"_divrem("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; int s=(int)a|1; unsigned h=0x811C9DC5u;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned d=((unsigned)i*2654435761u ^ h) | 1u;\n"
     "    unsigned uq=u/d, ur=u%d;\n"
     "    int sd=(int)d|1; int sq=s/sd, sr=s%sd;\n"
     "    h += uq*131u + ur*7u + (unsigned)sq*17u + (unsigned)sr;\n"
     "    u = (u*1664525u+1013904223u) ^ h; s = (int)h - i*3; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1357ULL}, "Opt4", 2},

    // 64-bit high-half multiply (mulhi).  On i386 `(unsigned long long)x*K>>32`
    // is a single 32x32->64 `mull` keeping EDX; on x86-64 a 64x64->128 path uses
    // `mulx`/`mulq` high half.  Stresses the value emitter's wide-mul high-part
    // extraction (the i128/i64 product narrowing).
    {p+"_mulhi",
     t+" "+p+"_mulhi("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned hi=(unsigned)(((unsigned long long)x*2654435761ull)>>32);\n"
     "    unsigned hs=(unsigned)(((unsigned long long)(unsigned)(h^x)*0x9E3779B1ull)>>32);\n"
     "    h += hi*131u + hs*7u + (x>>3);\n"
     "    x = (x ^ (x<<13)) + h + (unsigned)i; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x2468ULL}, "Opt4", 2},

    // Count-leading-zeros normalization: the shift amount comes from clz and can
    // equal 32 when the value is zero — the (1u<<n) / (x<<n) family is undefined
    // at n==32, so clang guards it; the lifter must reproduce the guard, not a
    // raw shift-by-bitwidth (the poison family that previously deleted RCL/SHLD).
    {p+"_clznorm",
     t+" "+p+"_clznorm("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned v=x ^ ((unsigned)i*0x9E3779B9u);\n"
     "    int lz = v ? __builtin_clz(v) : 32;\n"
     "    int tz = v ? __builtin_ctz(v) : 32;\n"
     "    unsigned norm = lz<32 ? (v<<lz) : 0u;\n"
     "    unsigned low  = tz<32 ? (v>>tz) : 0u;\n"
     "    h += (unsigned)lz*131u + (unsigned)tz*17u + (norm>>24) + (low&0xFFu);\n"
     "    x = (x*1664525u) ^ h ^ (v>>1); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x80000000ULL}, "Opt4", 2},

    // Saturating clamp via min/max plus signed overflow-detecting add.  The
    // clamp lowers to compare+select (cmov/csel/movCC); the saturating add
    // detects wrap by comparing signs — both stress MedFlags select pairing.
    {p+"_satclamp",
     t+" "+p+"_satclamp("+t+" a){\n"
     "  int acc=(int)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    int x=(int)((unsigned)a*(unsigned)(i*7+1)) - 0x40000000;\n"
     "    int s=acc+x; int ov=((acc^s)&(x^s))<0;\n"
     "    if(ov) s = (x<0)? (-2147483647-1) : 2147483647;\n"
     "    int lo=-1000, hi=1000; int c = s<lo?lo:(s>hi?hi:s);\n"
     "    h += (unsigned)c*131u + (unsigned)ov*7u + (unsigned)(s>>16);\n"
     "    acc = c ^ (int)h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x7ULL}, "Opt4", 2},

    // Variable bit-field extract + insert: read a field at a runtime offset and
    // width, modify it, write it back masked.  The (m<<s) / (v<<s) shifts use a
    // runtime count and the insert is read-modify-write — stresses mask folding
    // and sub-register stitching at arbitrary offsets.
    {p+"_bitfield",
     t+" "+p+"_bitfield("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned s=((unsigned)i*5u + (h>>2)) & 15u;\n"
     "    unsigned w=(((h>>1)+(unsigned)i) & 7u) + 1u;\n"
     "    unsigned m=(w>=32)?0xFFFFFFFFu:((1u<<w)-1u);\n"
     "    unsigned fld=(x>>s)&m;\n"
     "    unsigned nf=(fld*2654435761u + (unsigned)i) & m;\n"
     "    x=(x & ~(m<<s)) | (nf<<s);\n"
     "    h += fld*131u + nf*7u + s*17u + w; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xA5A5A5A5ULL}, "Opt4", 2},

    // Stack histogram: 16 buckets on the stack are indexed by a runtime value
    // and accumulated, then reduced.  Forces real load/store to frame slots with
    // computed indices — stresses LowToMed stack-slot synthesis + the optimizer's
    // memory dependence (no promotion to registers for the indexed writes).
    {p+"_hist",
     t+" "+p+"_hist("+t+" a){\n"
     "  unsigned b[16]; for(int i=0;i<16;i++) b[i]=0;\n"
     "  unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<128;i++){\n"
     "    unsigned k=(x>>4)&15u; b[k]+= (x>>8)&0xFFu;\n"
     "    unsigned k2=(x>>12)&15u; b[k2] ^= (x*131u);\n"
     "    x=(x*1664525u+1013904223u) ^ (x>>11); }\n"
     "  unsigned h=0; for(int i=0;i<16;i++) h=h*31u + b[i] + (unsigned)i*7u;\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x99ULL}, "Opt4", 2},

    // Cross-block flag liveness: a comparison made early is both stored as a
    // value and re-derived later, with partial-width writes in between, then a
    // borrow chain is read after a multi-way merge — stresses the flag pass'
    // cross-block CC liveness and the borrow it must not fold to a stale def.
    {p+"_xflag",
     t+" "+p+"_xflag("+t+" a){\n"
     "  unsigned acc=(unsigned)a; unsigned h=0; unsigned carry=0;\n"
     "  for(int i=0;i<60;i++){\n"
     "    unsigned x=acc ^ ((unsigned)i*2654435761u);\n"
     "    int lt=(x < acc); int ge=((int)x >= (int)acc);\n"
     "    unsigned d; unsigned nb=__builtin_sub_overflow(x,acc,&d);\n"
     "    unsigned r;\n"
     "    if(lt) r=d+carry; else if(ge) r=(d<<1)|nb; else r=d^0xFFu;\n"
     "    carry = nb ^ (r>>31);\n"
     "    h += r*131u + (unsigned)lt*7u + (unsigned)ge*17u + nb;\n"
     "    acc = r + (unsigned)i; }\n"
     "  return ("+t+")(unsigned long)(h + carry*2654435761u); }\n",
     {0xDEADBEEFULL}, "Opt4", 2},

    // Mixed-width widening/narrowing chain: a value flows through 8/16/32-bit
    // signed and unsigned reinterpretations with arithmetic at each width, then
    // recombines.  Stresses LowToMed sub-register synthesis (sign vs zero
    // extension) and the propagation pass' width tracking.
    {p+"_mixw",
     t+" "+p+"_mixw("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    signed char  c =(signed char)(x + (unsigned)i);\n"
     "    unsigned char uc=(unsigned char)(x>>7);\n"
     "    short        s =(short)((x>>3) ^ (unsigned)(i*9));\n"
     "    unsigned short us=(unsigned short)(x>>11);\n"
     "    int wide=(int)c*7 + (int)uc*131 + (int)s - (int)us;\n"
     "    unsigned u32=(unsigned)wide ^ (x<<1);\n"
     "    h += (unsigned)(c & 0xFF) + ((unsigned)s & 0xFFFF)*3u + u32*5u;\n"
     "    x = u32 + ((unsigned)(int)c) + h; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xFEEDFACEULL}, "Opt4", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOpt4TC("x64opt4", "long");
static const std::vector<RoundTripTC> kX86 = makeOpt4TC("x86opt4", "int");
static const std::vector<RoundTripTC> kA64 = makeOpt4TC("a64opt4", "long");
static const std::vector<RoundTripTC> kARM = makeOpt4TC("armopt4", "int");

INSTANTIATE_TEST_SUITE_P(Opt4, X64Opt4RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt4, X86Opt4RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt4, A64Opt4RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt4, ARM32Opt4RT, ::testing::ValuesIn(kARM), rtTCName);
