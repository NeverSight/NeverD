//===- AllPlatform_OptStress3RTTests.cpp - optimizer-path stressors -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Third batch of high-yield roundtrip probes aimed at NeverD's own MedIR passes
// (MedFlags folding, MedDCE liveness, MedPropagation const/copy prop, LowToMed
// sub-register synthesis).  These idioms are deliberately distinct from
// OptStress / OptStress2: signed-vs-unsigned comparison fan-out, variable
// rotates whose count can be zero (the 32-n shift is undefined unless masked),
// branchless abs / signum / sign-extend chains, SWAR popcount + parity, manual
// byte-swap / nibble shuffles, argmin/argmax index tracking, and a multi-way
// branch whose carry/borrow flag is read after the merge.  Each kernel returns
// a value-dependent hash compiled at -O2; the roundtrip compares native vs
// lifted execution across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Opt3RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Opt3RT, Verify) { roundTripX64(GetParam()); }
class X86Opt3RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Opt3RT, Verify) { roundTripX86(GetParam()); }
class A64Opt3RT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Opt3RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Opt3RT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Opt3RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOpt3TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Signed-vs-unsigned comparison fan-out: the same operands are compared both
    // ways and each result steers a separate accumulator update, with partial
    // writes between — stresses MedFlags' signed/unsigned condition folding.
    {p+"_sucmp",
     t+" "+p+"_sucmp("+t+" a){\n"
     "  unsigned u=(unsigned)a; int s=(int)a; unsigned h=0x811C9DC5u;\n"
     "  for(int i=0;i<64;i++){\n"
     "    int sv=s + i*7 - 200; unsigned uv=u + (unsigned)i*2654435761u;\n"
     "    int slt=(sv < (int)uv); int ult=(uv < (unsigned)sv);\n"
     "    int sge=(sv >= -5); int ule=(uv <= 0x8000u);\n"
     "    h += slt ? (h*31u + (unsigned)sv) : (h ^ uv);\n"
     "    h ^= ult ? (h>>5) : (h + 0x9E3779B9u);\n"
     "    h = ((h<<3)|(h>>29)) + (sge ? 0x1234u : ule ? 0x5678u : 0xABCDu);\n"
     "    s = (int)h - i; u = h ^ (u<<1); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x1357ULL}, "Opt3", 2},

    // Variable rotate where the count can be zero: the (32-n) shift is undefined
    // unless masked, so clang masks it — the lifter must reproduce the mask, not
    // a raw shift-by-bitwidth (the poison family that previously deleted RCL).
    {p+"_vrot",
     t+" "+p+"_vrot("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned n=(x ^ (unsigned)i) & 31u;\n"
     "    unsigned rl=(x<<n)|(x>>((32-n)&31));\n"
     "    unsigned rr=(x>>n)|(x<<((32-n)&31));\n"
     "    unsigned n2=(h>>3)&63u; unsigned sh=(n2<32) ? (x<<n2) : 0u;\n"
     "    h += rl ^ (rr*3u) ^ sh; x=(x*2654435761u) ^ h ^ (x>>7); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x2468ULL}, "Opt3", 2},

    // Branchless absolute value / signum / 16->32 sign-extend chains.  The
    // arithmetic abs `(x^m)-m` with `m=x>>31` and the short->int widening test
    // LowToMed's sign-extension and the propagation pass' width tracking.
    {p+"_absign",
     t+" "+p+"_absign("+t+" a){\n"
     "  int x=(int)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    int m=x>>31; int ax=(x^m)-m; int sg=(x>0)-(x<0);\n"
     "    short sw=(short)(x ^ (i*9)); int sext=(int)sw;\n"
     "    h += (unsigned)ax*131u + (unsigned)(sg+2)*7u + (unsigned)sext;\n"
     "    x=(int)h - ax*sg + sext; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xFFFFFEEEULL}, "Opt3", 2},

    // SWAR population count + parity via the 0x6996 magic — dense mask constants
    // and shifts the constant-propagation pass must fold at the right widths.
    {p+"_swar",
     t+" "+p+"_swar("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned v=x ^ ((unsigned)i*0x9E3779B9u);\n"
     "    v=v-((v>>1)&0x55555555u);\n"
     "    v=(v&0x33333333u)+((v>>2)&0x33333333u);\n"
     "    v=(v+(v>>4))&0x0F0F0F0Fu;\n"
     "    unsigned pc=(v*0x01010101u)>>24;\n"
     "    unsigned par=x; par^=par>>16; par^=par>>8; par^=par>>4;\n"
     "    par&=0xFu; par=(0x6996u>>par)&1u;\n"
     "    h += pc*131u + par*7u; x=((x<<1)|(x>>31)) ^ (h+pc); }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xDEADBEEFULL}, "Opt3", 2},

    // Manual byte-swap and nibble shuffle plus per-byte recombination — byte
    // masking / shifting the sub-register synthesis must keep coherent.
    {p+"_bytes",
     t+" "+p+"_bytes("+t+" a){\n"
     "  unsigned x=(unsigned)a; unsigned h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned bs=((x&0xFFu)<<24)|((x&0xFF00u)<<8)|((x>>8)&0xFF00u)|((x>>24)&0xFFu);\n"
     "    unsigned nz=((x&0x0F0F0F0Fu)<<4)|((x>>4)&0x0F0F0F0Fu);\n"
     "    unsigned b0=x&0xFFu,b1=(x>>8)&0xFFu,b2=(x>>16)&0xFFu,b3=(x>>24)&0xFFu;\n"
     "    unsigned re=(b0*131u)^(b1<<3)^(b2*7u)^b3;\n"
     "    h += bs ^ nz ^ re; x=(bs*2654435761u) ^ h ^ (unsigned)i; }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x01020304ULL}, "Opt3", 2},

    // Argmin/argmax index tracking: the running min/max and their indices are
    // updated through conditional selects whose results are loop-carried — a
    // select/CMOV chain that must stay paired with its comparison across blocks.
    {p+"_argmm",
     t+" "+p+"_argmm("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int mni=0,mxi=0;\n"
     "  unsigned mn=0xFFFFFFFFu,mx=0,h=0;\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned v=(x*2654435761u) ^ (x>>13);\n"
     "    if(v<mn){mn=v;mni=i;} if(v>mx){mx=v;mxi=i;}\n"
     "    h += (unsigned)mni*7u + (unsigned)mxi*131u + (mn>>16) + (mx&0xFFFFu);\n"
     "    x=v^h; }\n"
     "  return ("+t+")(unsigned long)(h ^ mn ^ mx ^ ((unsigned)(mni+mxi)*2654435761u)); }\n",
     {0x99ULL}, "Opt3", 2},

    // Multi-way branch whose carry/borrow is read after the merge: each arm
    // defines `acc` differently, then a single compare derives the carry the
    // next iteration consumes — stresses cross-block liveness + flag derivation.
    {p+"_nestc",
     t+" "+p+"_nestc("+t+" a){\n"
     "  unsigned acc=(unsigned)a; unsigned c=1u;\n"
     "  for(int i=0;i<60;i++){\n"
     "    unsigned d=acc&7u; unsigned prev=acc;\n"
     "    if(d<2) acc=acc*3u+c;\n"
     "    else if(d<4) acc=acc-(0x9E3779B9u+c);\n"
     "    else if(d<6){ unsigned t1=acc+c; acc=(t1<<2)|(t1>>30); }\n"
     "    else acc^=0xA5A5A5A5u;\n"
     "    c=(acc<prev) ? 1u : 0u;\n"
     "    acc+=(unsigned)i*131u + c; }\n"
     "  return ("+t+")(unsigned long)(acc + c*2654435761u); }\n",
     {0x5ULL}, "Opt3", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOpt3TC("x64opt3", "long");
static const std::vector<RoundTripTC> kX86 = makeOpt3TC("x86opt3", "int");
static const std::vector<RoundTripTC> kA64 = makeOpt3TC("a64opt3", "long");
static const std::vector<RoundTripTC> kARM = makeOpt3TC("armopt3", "int");

INSTANTIATE_TEST_SUITE_P(Opt3, X64Opt3RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt3, X86Opt3RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt3, A64Opt3RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Opt3, ARM32Opt3RT, ::testing::ValuesIn(kARM), rtTCName);
