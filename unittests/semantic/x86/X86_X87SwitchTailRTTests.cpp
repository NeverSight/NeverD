//===- X86_X87SwitchTailRTTests.cpp - x87 peeled-loop switch tails -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Peeled + rotated loops whose body is a `switch` keeping `long double` values
// resident on the x87 stack.  clang -O2 peels the first iteration (here forced
// by an explicit `first` flag) and reuses one rodata jump table for the peeled
// copy and another, adjacent table for the steady loop body.  Two defects fell
// out of this shape and are fixed by `JumpTableResolver`:
//
//   1. The peeled `switch(u%8)` index is masked by `and $0x7` (+ a `dec` from
//      the peeled specialisation), so it carries no `cmp` range guard.  The two
//      adjacent tables form one continuous relocation run, which the run-length
//      count over-read as a single table -- fabricating bogus successor edges
//      into the loop body.  `inferBoundsFromMask` now clamps to the mask.
//   2. The steady switch's PIC table base (`lea tab(%rip),%rdx`) is materialised
//      in the loop preheader, which sits *after* the peeled switch; folding the
//      base from the function entry halted at the peeled INDIR_BR before the
//      `lea`.  `foldRegConstant` now also emulates from intervening dominator
//      block prefixes, recovering the preheader-set base.
//
// The result is read from the low 64 mantissa bytes of a stored long double so
// original and lifted compare on well-defined state only.
//
// NB: the i386 `forcepeel` variant reaches the shared switch arms at two x87
// stack depths (the peeled first iteration vs the steady loop body).  A single
// absolute-slot block cannot represent both, so fixupFpuStack rebuilds the CFG
// as the product of (block x entry-TOP), giving each x87 phase its own re-based
// block copy; forcepeel therefore runs on both x64 and i386.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64X87SwTailRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64X87SwTailRT, Verify) { roundTripX64(GetParam()); }
class X86X87SwTailRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86X87SwTailRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
// forcepeel: an explicit `first` flag clang peels the first iteration to
// eliminate, while two f80 values (x=1.0L,y) stay resident across the switch
// whose arms share a faddp tail -- the shape that exercises both jump-table
// fixes.  x64-only (see file header).
static RoundTripTC forcepeelTC(const char *prefix, const char *T, int opt) {
  std::string p = prefix, t = T;
  return {p+"_forcepeel",
     t+" "+p+"_forcepeel("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=1.0L, y=2.0L; unsigned h=0;\n"
     "  int first=1;\n"
     "  for(int i=0;i<240;i++){ long double v=(long double)(int)(u%53)*0.05L;\n"
     "    if(first){ x=x+0.5L; first=0; }\n"
     "    switch(u%8u){\n"
     "      case 0: x=x*1.0000003L+v; break; case 1: y=y-v*0.5L; break;\n"
     "      case 2: x=x+y*0.25L; break;      case 3: y=y*1.0000007L-v; break;\n"
     "      case 4: x=x+v; break;            case 5: x=x*0.5L+y*0.5L; break;\n"
     "      case 6: y=y+x*0.125L; break;     default: x=x+y; break; }\n"
     "    h=h*131u+(unsigned)(x>y);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  long double r=x+y; unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)(h^(unsigned)m^(unsigned)(m>>32)); }\n",
     {0x17ULL}, "X87SwTail", opt};
}

static std::vector<RoundTripTC> makeSwTailTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // peelsw: first-iteration branch (acc>500.0, statically false for acc=1.0L)
    // is the classic clang loop-peel trigger; two f80 (acc,bias) resident across
    // an 8-way switch whose arms reach a shared faddp tail.
    {p+"_peelsw",
     t+" "+p+"_peelsw("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double acc=1.0L, bias=3.0L; unsigned h=0;\n"
     "  for(int i=0;i<256;i++){ long double v=(long double)(int)(u%37)*0.05L;\n"
     "    if(acc>500.0L){ acc=acc-500.0L; h+=1u; }\n"
     "    switch(u%8u){\n"
     "      case 0: acc=acc*1.0000003L; break;  case 1: bias=bias+v; break;\n"
     "      case 2: acc=acc+bias*0.1L; break;   case 3: bias=bias*0.9999997L; break;\n"
     "      case 4: acc=acc+v; break;           case 5: bias=bias-v*0.25L; break;\n"
     "      case 6: acc=acc+bias; break;        default: acc=acc+bias+v; break; }\n"
     "    h=h*131u+(unsigned)(acc>bias);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  long double r=acc+bias; unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)(h^(unsigned)m^(unsigned)(m>>32)); }\n",
     {0x29ULL}, "X87SwTail", 2},

    // sw8: two resident f80 (x=1.0L,y=2.0L), 8-way switch whose case 4 and the
    // default both reach a deeper shared tail merged with faddp.
    {p+"_sw8",
     t+" "+p+"_sw8("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=1.0L, y=2.0L; unsigned h=0;\n"
     "  for(int i=0;i<160;i++){ long double v=(long double)(int)(u%50)*0.1L;\n"
     "    switch(u%8u){\n"
     "      case 0: x=x*1.0000003L+v; break;  case 1: y=y-v*0.5L; break;\n"
     "      case 2: x=x+y*0.25L; break;       case 3: y=y*1.0000007L-v; break;\n"
     "      case 4: x=x+v; y=y+v; break;      case 5: x=x*0.5L+y*0.5L; break;\n"
     "      case 6: y=y+x*0.125L; break;      default: x=x+y; break; }\n"
     "    h=h*131u+(unsigned)(x>y);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  long double r=x+y; unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)(h^(unsigned)m^(unsigned)(m>>32)); }\n",
     {0x31ULL}, "X87SwTail", 2},

    // swtwo: two accumulators reaching a single faddp tail `x=x+y`, with an
    // extra post-switch conditional add stressing the merge.
    {p+"_swtwo",
     t+" "+p+"_swtwo("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; long double x=1.0L, y=10.0L; unsigned h=0;\n"
     "  for(int i=0;i<175;i++){ long double v=(long double)(int)(u%64)*0.03125L;\n"
     "    switch(u%7u){\n"
     "      case 0: x=x+v; break;          case 1: y=y-v; break;\n"
     "      case 2: x=x*1.0000002L; break; case 3: y=y*0.9999998L; break;\n"
     "      case 4: x=x+y*0.1L; break;     case 5: y=y+x*0.1L; break;\n"
     "      default: x=x+y; y=y*0.5L; break; }\n"
     "    if((u&16u)!=0){ x=x+y; }\n"
     "    h=h*131u+(unsigned)(x>y);\n"
     "    u=u*1103515245u+12345u; }\n"
     "  long double r=x+y; unsigned long long m; __builtin_memcpy(&m,&r,8);\n"
     "  return ("+t+")(unsigned)(h^(unsigned)m^(unsigned)(m>>32)); }\n",
     {0x53ULL}, "X87SwTail", 2},
  };
}

static std::vector<RoundTripTC> kX64Build() {
  auto v = makeSwTailTC("x64swt", "long");
  v.push_back(forcepeelTC("x64swt", "long", 2));   // x64-only jump-table probe
  return v;
}
// i386 PIC produces the GOT-relative constant-pool idiom; forcepeel additionally
// reaches the shared switch arms at two different x87 stack depths (a peeled
// first iteration vs the steady loop body), which fixupFpuStack now splits into
// per-TOP block copies.
static std::vector<RoundTripTC> kX86Build() {
  auto v = makeSwTailTC("x86swt", "int");
  v.push_back(forcepeelTC("x86swt", "int", 2));
  for (auto &c : v)
    c.ExtraFlags = "-fPIC";
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = kX64Build();
static const std::vector<RoundTripTC> kX86 = kX86Build();

INSTANTIATE_TEST_SUITE_P(X87SwTail, X64X87SwTailRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(X87SwTail, X86X87SwTailRT,
                         ::testing::ValuesIn(kX86), rtTCName);
