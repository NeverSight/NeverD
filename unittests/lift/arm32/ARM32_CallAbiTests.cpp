#include "HighCHostExecution.h"
#include "NeverDLiftFixture.h"

#include <fstream>
#include <iterator>
#include <regex>

class ARM32_CallAbi : public HighCHostExecutionTest {};

// The base AAPCS (softfp) passes an external routine's float arguments in
// r0-r3, not in the VFP registers the image's own code computes in.  Here
// the VFP registers hold x + 1 and x * 0.5 while r0 and r1 carry them with a
// mantissa bit flipped, so binding the call from the VFP registers changes
// what powf receives.
TEST_F(ARM32_CallAbi, SoftFloatExternalTakesItsFloatsInCoreRegisters) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const std::string Kernel = R"C(
float powf(float, float);
int pw(int a) {
  float x = (float)a;
  union { float f; unsigned u; } b = {x + 1.0f}, e = {x * 0.5f}, r;
  b.u ^= 0x00400000u;
  e.u ^= 0x00200000u;
  r.f = powf(b.f, e.f);
  return (int)r.u;
}
)C";
  const auto Source = tmpFile("softfp-powf.c");
  const auto Object = tmpFile("softfp-powf.o");
  std::ofstream(Source) << Kernel;
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "arm-linux-gnueabi",
                               "-mcpu=cortex-a15", "-mfloat-abi=softfp", "-O2",
                               "-c", Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;

  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(Input.good());
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  ASSERT_EQ(C.find("unknown value"), std::string::npos) << C;

  const std::string Reference = std::regex_replace(
      Kernel, std::regex("\\bpw\\("), std::string("ref_pw("));
  // Softfp returns either spelling in R0. The tail call cannot distinguish
  // the source's integer bit view from a float return, so compare its bits
  // instead of numerically converting a recovered float on the host ABI.
  const auto Program = tmpFile("softfp-powf-host.exe");
  std::ofstream(tmpFile("softfp-powf-host.c")) << C << "\n"
                                               << Reference << R"C(
int main(void) {
  static const int Values[] = {-2, 0, 1, 2, 3, 4, 6};
  for (unsigned I = 0; I != sizeof(Values) / sizeof(Values[0]); ++I)
    if (__builtin_bit_cast(unsigned, pw(Values[I])) != (unsigned)ref_pw(Values[I]))
      return 1;
  return 0;
}
)C";
  const auto Built =
      exec(NEVERD_TEST_CLANG,
           {"-std=gnu11", "-O2", tmpFile("softfp-powf-host.c").string(), "-lm",
            "-o", Program.string()});
  ASSERT_EQ(Built.exitCode, 0) << Built.err << "\n" << C;
  const auto Run = exec(Program.string(), {});
  EXPECT_EQ(Run.exitCode, 0) << C;
}

// AAPCS returns a narrow value extended to 32 bits.  This function's -O0
// code loads its unsigned byte with LDRB, so it returns the byte
// zero-extended: its C must return an unsigned type, which the host extends
// alike, rather than sign-extend 0xA5 to 0xFFFFFFA5.
TEST_F(ARM32_CallAbi, AZeroExtendedByteReturnIsUnsigned) {
  expectHighCRunsLikeSource({"-target", "arm-linux-gnueabi", "-mcpu=cortex-a15",
                             "-mfloat-abi=softfp"},
                            R"C(
__attribute__((optnone)) int rev8(int x) {
  unsigned char b = (unsigned char)x;
  b = (unsigned char)(((b >> 4) & 0x0F) | ((b << 4) & 0xF0));
  b = (unsigned char)(((b >> 2) & 0x33) | ((b << 2) & 0xCC));
  b = (unsigned char)(((b >> 1) & 0x55) | ((b << 1) & 0xAA));
  return b;
}
)C",
                            "rev8", {"rev8"},
                            {0, 1, -1, 0xA5, 0x80, 0x55AA, 0x12345678});
}

// In a softfp image, Clang gives a static function the VFP convention, so
// `acc` passes `ha` its doubles in d0 and d1 and takes the result from d0.
// The call reads the registers ha reads, not r0-r3.
TEST_F(ARM32_CallAbi, ALocalVfpCalleeTakesItsDoublesInVfpRegisters) {
  expectHighCRunsLikeSource({"-target", "arm-linux-gnueabi", "-mcpu=cortex-a15",
                             "-mfloat-abi=softfp", "-ffp-contract=off"},
                            R"C(
static double ha(double, double) __attribute__((noinline));
int acc(int a) {
  double v = (double)a;
  for (int i = 0; i < 8; i++)
    v = ha(v, (double)i + 1.0);
  return (int)v;
}
static double ha(double a, double b) { return a * 0.5 + b * b - a / b; }
)C",
                            "acc", {"acc", "ha"},
                            {0, 1, -1, 7, 0x55, -1000, 0x12345});
}

// Eight integer arguments through a function pointer: r0-r3 and four stack
// slots.  The -O0 code stores the slots last first, [sp+12] four stores
// before the call, which still passes the eighth argument.  The pointer
// travels in a 32-bit local.
TEST_F(ARM32_CallAbi, AnIndirectCallPassesEveryStackArgument) {
  KernelOptimization = "-O0";
  HostFlags = {"-m32"};
  expectHighCRunsLikeSource({"-target", "arm-linux-gnueabi", "-mcpu=cortex-a15",
                             "-mfloat-abi=softfp"},
                            R"C(
static int t8(int a, int b, int c, int d, int e, int f, int g, int h)
    __attribute__((noinline));
int ind8(int a) {
  unsigned w = (unsigned)a ^ 0x5A5Au;
  int acc = 0;
  int (*fp)(int, int, int, int, int, int, int, int) = t8;
  __asm__("" : "+r"(fp));
  for (int i = 0; i < 4; i++) {
    w = w * 1664525u + 1013904223u;
    acc = acc * 131 + fp((int)w, (int)(w >> 2), (int)(w >> 5), (int)(w >> 9),
                         (int)(w >> 13), (int)(w >> 19), (int)(w >> 24),
                         (int)(w >> 28));
  }
  return acc;
}
static int t8(int a, int b, int c, int d, int e, int f, int g, int h) {
  return a - b * 2 + c * 3 - d * 4 + e * 5 - f * 6 + g * 7 - h * 8;
}
)C",
                            "ind8", {"ind8", "t8"},
                            {0, 1, -1, 0x7F00, 0x55AA, 0x12345678});
}

// Twelve variadic ints: r1-r3 and nine on the stack, which the callee's
// va_arg walk reads in place above its frame.  The callee takes them as
// trailing parameters, seeded where the walk reads them.
TEST_F(ARM32_CallAbi, AVariadicCalleeReadsItsStackArgumentsInPlace) {
  HostFlags = {"-m32"};
  expectHighCRunsLikeSource({"-target", "arm-linux-gnueabi", "-mcpu=cortex-a15",
                             "-mfloat-abi=softfp"},
                            R"C(
int vmany(int n, ...) __attribute__((noinline));
int vmany_e(int x) {
  unsigned u = (unsigned)x;
  return vmany(12, (int)u, (int)(u + 1u), (int)(u * 2u), (int)(u ^ 3u),
               (int)(u - 4u), (int)(u * 5u), (int)(u + 6u), (int)(u ^ 7u),
               (int)(u - 8u), (int)(u * 9u), (int)(u + 10u), (int)(u ^ 11u));
}
int vmany(int n, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s * 131 + __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return s;
}
)C",
                            "vmany_e", {"vmany_e", "vmany"},
                            {0, 1, -1, 0x23456, 77});
}

namespace {
const std::vector<std::string> SoftFp = {
    "-target", "arm-linux-gnueabi", "-mcpu=cortex-a15", "-mfloat-abi=softfp"};

/// The text of \p Name's definition in \p C, or empty.
std::string definitionOf(const std::string &C, const std::string &Name) {
  const size_t Begin = C.find(" " + Name + "(");
  for (size_t At = Begin; At != std::string::npos;
       At = C.find(" " + Name + "(", At + 1)) {
    const size_t Open = C.find_first_of(";{", At);
    if (Open == std::string::npos || C[Open] != '{')
      continue;
    const size_t End = C.find("\n}\n", Open);
    return C.substr(At, End == std::string::npos ? End : End - At);
  }
  return {};
}
} // namespace

// `lsreq r9, r1, r10; tsteq r9, #1` tests the value the first instruction
// computed: both select on one condition, so the test never reads what r9
// held before, which is the caller's on the first iteration.
TEST_F(ARM32_CallAbi, APredicatedTestReadsThePredicatedResultItFollows) {
  HostFlags = {"-m32"};
  expectHighCRunsLikeSource(SoftFp, R"C(
static const unsigned char br[32] = {1, 3, 5, 6, 4, 2, 1, 1, 2, 2, 3, 5, 6, 4, 5, 6,
                                     1, 3, 3, 4, 4, 2, 5, 1, 6, 2, 3, 5, 6, 4, 1, 2};
int paren(int a) {
  unsigned s = (unsigned)a, out = 0;
  for (int it = 0; it < 96; it++) {
    s = s * 1103515245u + 12345u;
    unsigned acc = s;
    unsigned st[32];
    int sp = 0;
    unsigned ok = 1u, maxd = 0u;
    for (int i = 0; i < 32; i++) {
      unsigned c = br[i];
      if (c == 1u || c == 3u || c == 5u) {
        if (sp < 32) { st[sp++] = c; if ((unsigned)sp > maxd) maxd = (unsigned)sp; }
      } else {
        if (sp > 0) { unsigned op = st[--sp]; if (op + 1u != c) ok = 0u; }
        else ok = 0u;
      }
      acc = acc * 131u + (unsigned)sp;
    }
    acc = acc * 131u + ok + maxd + (sp == 0 ? 1u : 0u);
    out = out * 1311u + acc;
  }
  return (int)out;
}
)C",
                            "paren", {"paren", "br"}, {0, 1, -1, 0x55AA, 77});
}

// The rewrite needs every reader to sit under the same condition.  r9's
// incoming value stays unknown where an unconditional instruction reads the
// predicated result (an add, a store); the predicated compare beside it
// reads the chosen arm.
TEST_F(ARM32_CallAbi, APredicatedResultReadOnEveryPathKeepsItsUnknownValue) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const std::string Kernel = R"C(
#define NK __attribute__((naked, noinline))
NK int later_read(int a, int b) { __asm__("push {r4, r9, lr}\n\ttst r0, #1\n\tlsreq r9, r1, #2\n\ttsteq r9, #1\n\tmoveq r0, #5\n\tadd r0, r0, r9\n\tpop {r4, r9, pc}"); }
NK int stored(int a, int *p) { __asm__("push {r4, r9, lr}\n\ttst r0, #1\n\tlsreq r9, r0, #2\n\tstr r9, [r1]\n\tmov r0, #0\n\tpop {r4, r9, pc}"); }
NK int only_tested(int a, int b) { __asm__("push {r4, r9, lr}\n\ttst r0, #1\n\tlsreq r9, r1, #2\n\ttsteq r9, #1\n\tmovne r0, #3\n\tmoveq r0, #4\n\tpop {r4, r9, pc}"); }
)C";
  const auto Source = tmpFile("predicated-readers.c");
  const auto Object = tmpFile("predicated-readers.o");
  std::ofstream(Source) << Kernel;
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "arm-linux-gnueabi",
                               "-mcpu=cortex-a15", "-mfloat-abi=softfp", "-O2",
                               "-c", Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(Input.good());
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  for (const char *Observer : {"later_read", "stored"}) {
    const std::string Body = definitionOf(C, Observer);
    ASSERT_FALSE(Body.empty()) << Observer << C;
    EXPECT_NE(Body.find("unknown register"), std::string::npos)
        << Observer << Body;
  }
  const std::string Tested = definitionOf(C, "only_tested");
  ASSERT_FALSE(Tested.empty()) << C;
  EXPECT_EQ(Tested.find("__builtin_trap"), std::string::npos) << Tested;
}
