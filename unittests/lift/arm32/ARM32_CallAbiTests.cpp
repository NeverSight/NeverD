#include "../core/SourceCallExecution.h"
#include "HighCHostExecution.h"
#include "NeverDLiftFixture.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

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

// A 64-bit argument takes an even-odd register pair: r3 (after three narrow
// arguments) or r1 (after one) carries nothing, and the caller never sets it.
// Neither callee reads the register, so each call passes zero there.  In
// `mixarg` r1 is a loop join whose entry arm is the register's incoming
// value; the callee returns r0:r1 and writes r1 before it returns.
TEST_F(ARM32_CallAbi, APaddingRegisterTheCalleeNeverReadsPassesZero) {
  HostFlags = {"-m32"};
  expectHighCRunsLikeSource(SoftFp, R"C(
static long long hm(signed char a, short b, int c, long long d)
    __attribute__((noinline));
int pmix(int a) {
  unsigned w = (unsigned)a | 1u;
  long long acc = 0;
  for (int i = 0; i < 48; i++) {
    w = w * 1103515245u + 12345u;
    long long d = ((long long)(unsigned)w << 19) ^ ((long long)i * 2654435761LL);
    acc += hm((signed char)(w >> 2), (short)(w >> 7), (int)w, d);
  }
  return (int)(acc ^ (acc >> 32));
}
static long long hm(signed char a, short b, int c, long long d) {
  return (long long)a * 1000003LL + (long long)b * 131 + (long long)c +
         (d ^ (d >> 17));
}
)C",
                            "pmix", {"pmix", "hm"}, {0, 1, -1, 0x55AA, 77});
  expectHighCRunsLikeSource(SoftFp, R"C(
static long long ma(int x, long long y, int z) __attribute__((noinline));
int mixarg(int a) {
  unsigned w = (unsigned)a ^ 0xa5u;
  long long acc = 0;
  for (int i = 0; i < 40; i++) {
    w = w * 22695477u + 1u;
    long long y = (long long)(int)w * (long long)(int)(w >> 9);
    long long r = ma((int)(w >> 3), y, (int)(w >> 17));
    acc += r ^ (r >> 32);
  }
  return (int)(acc ^ (acc >> 32));
}
static long long ma(int x, long long y, int z) {
  return y + (long long)x * (long long)z + ((y >> 32) ^ (long long)(x + z));
}
)C",
                            "mixarg", {"mixarg", "ma"}, {0, 1, -1, 0x55AA, 77});
}

// An 8-byte stack argument is 8-byte aligned: the 4-byte lane before it is
// never stored.  Inside a loop the previous call ends the stored-slot proof
// (recoverCallAbi keeps the position unknown), and the callee's body, which
// reads no byte of the lane, lets the call pass zero.
TEST_F(ARM32_CallAbi, AStackPaddingLaneTheCalleeNeverReadsPassesZero) {
  HostFlags = {"-m32"};
  for (const char *Level : {"-O0", "-O2"}) {
    KernelOptimization = Level;
    expectHighCRunsLikeSource(SoftFp, R"C(
static long long hmw(int a, long long b, int c, long long d, int e, long long f)
    __attribute__((noinline));
int amixw(int a) {
  unsigned w = (unsigned)a | 1u;
  long long acc = 0;
  for (int i = 0; i < 32; i++) {
    w = w * 1103515245u + 12345u;
    long long b = ((long long)(unsigned)w << 19) ^ ((long long)i * 2654435761LL);
    acc = acc * 131 +
          hmw((int)w, b, (int)(w >> 3), b ^ 0x55557777LL, (int)(w >> 7), b * 3);
  }
  return (int)(acc ^ (acc >> 31));
}
static long long hmw(int a, long long b, int c, long long d, int e, long long f) {
  return (long long)a + b - (long long)c * 5 + d * 3 - (long long)e + f;
}
)C",
                              "amixw", {"amixw", "hmw"},
                              {0, 1, -1, 0x55AA, 77});
  }
}

// The callers declare pair-aligned prototypes; the definitions, bound to the
// same symbols, take every lane as an int.  Where a definition reads the lane
// its caller left unset (directly, through a tail call, through an escaped
// address, or by returning r1 as it was passed), the call keeps its unknown
// value.  `ok` reads neither padding lane and passes zero.
TEST_F(ARM32_CallAbi, APaddingPositionTheCalleeReadsKeepsItsUnknownValue) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const std::string Kernel = R"C(
#define NI __attribute__((noinline))
#define STACK_CALLER(N) int N(int a) { unsigned h = a, acc = 0; \
  for (int i = 0; i < 9; i++) { h = h * 1103515245u + 12345u; \
    acc = acc * 131u + (unsigned)N##_callee((int)h, ((long long)h << 20) ^ i, \
                                            (int)(h >> 5), ((long long)h << 16) + i); } \
  return (int)acc; }
#define R1_CALLER(N) int N(int a) { unsigned w = (unsigned)a ^ 0xa5u; long long acc = 0; \
  for (int i = 0; i < 40; i++) { w = w * 22695477u + 1u; \
    long long y = (long long)(int)w * (long long)(int)(w >> 9); \
    long long r = N##_callee((int)(w >> 3), y, (int)(w >> 17)); acc += r ^ (r >> 32); } \
  return (int)(acc ^ (acc >> 32)); }
struct tail { int c, p5, dlo, dhi; };

int rdpad_callee(int, long long, int, long long) __asm__("rdpad_sym");
NI int rdpad_def(int a, int p1, int blo, int bhi, int c, int p5, int dlo, int dhi) __asm__("rdpad_sym");
NI int rdpad_def(int a, int p1, int blo, int bhi, int c, int p5, int dlo, int dhi) {
  return a + blo * 3 + bhi * 5 + c * 7 + p5 * 11 + dlo * 13 + dhi * 17; }
STACK_CALLER(rdpad)

int fwd_callee(int, long long, int, long long) __asm__("fwd_sym");
NI int fwd_def(int a, int p1, int blo, int bhi, int c, int p5, int dlo, int dhi) __asm__("fwd_sym");
NI int fwd_def(int a, int p1, int blo, int bhi, int c, int p5, int dlo, int dhi) {
  return rdpad_def(a, p1, blo, bhi, c, p5, dlo, dhi); }
STACK_CALLER(fwd)

NI int sum(const struct tail *t) { return t->c + t->p5 * 3 + t->dlo * 5 + t->dhi * 7; }
int esc_callee(int, long long, int, long long) __asm__("esc_sym");
NI int esc_def(int a, int p1, int blo, int bhi, struct tail t) __asm__("esc_sym");
NI int esc_def(int a, int p1, int blo, int bhi, struct tail t) { return sum(&t) + a + blo + bhi; }
STACK_CALLER(esc)

int ok_callee(int, long long, int, long long) __asm__("ok_sym");
NI int ok_def(int a, int p1, int blo, int bhi, int c, int p5, int dlo, int dhi) __asm__("ok_sym");
NI int ok_def(int a, int p1, int blo, int bhi, int c, int p5, int dlo, int dhi) {
  return a + blo * 3 + bhi * 5 + c * 7 + dlo * 13 + dhi * 17; }
STACK_CALLER(ok)

long long rdr1_callee(int, long long, int) __asm__("rdr1_sym");
NI long long rdr1_def(int x, int p1, int ylo, int yhi, int z) __asm__("rdr1_sym");
NI long long rdr1_def(int x, int p1, int ylo, int yhi, int z) {
  return (long long)x * z + p1 * 9 + ylo + ((long long)yhi << 32); }
R1_CALLER(rdr1)

long long retr1_callee(int, long long, int) __asm__("retr1_sym");
NI long long retr1_def(int x, int p1, int ylo, int yhi, int z) __asm__("retr1_sym");
NI long long retr1_def(int x, int p1, int ylo, int yhi, int z) {
  return ((long long)p1 << 32) | (unsigned)(x * z + ylo + yhi); }
R1_CALLER(retr1)

extern int (*hook)(int);
long long ind_callee(int, long long, int) __asm__("ind_sym");
NI long long ind_def(int x, int p1, int ylo, int yhi, int z) __asm__("ind_sym");
NI long long ind_def(int x, int p1, int ylo, int yhi, int z) {
  return hook(x) + (long long)z + ylo + ((long long)yhi << 32); }
R1_CALLER(ind)
int (*hook)(int);
)C";
  const auto Source = tmpFile("padding-readers.c");
  const auto Object = tmpFile("padding-readers.o");
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
  for (const char *Reader : {"rdpad", "fwd", "esc", "rdr1", "retr1", "ind"}) {
    const std::string Body = definitionOf(C, Reader);
    ASSERT_FALSE(Body.empty()) << Reader << C;
    EXPECT_NE(Body.find("(__builtin_trap(), 0 /* no recovered semantics"),
              std::string::npos)
        << Reader << Body;
  }
  const std::string Unread = definitionOf(C, "ok");
  ASSERT_FALSE(Unread.empty()) << C;
  EXPECT_EQ(Unread.find("__builtin_trap()"), std::string::npos) << Unread;
}

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
    EXPECT_NE(Body.find("(__builtin_trap(), 0 /* incoming register has no "
                        "definition"),
              std::string::npos)
        << Observer << Body;
  }
  const std::string Tested = definitionOf(C, "only_tested");
  ASSERT_FALSE(Tested.empty()) << C;
  EXPECT_EQ(Tested.find("__builtin_trap"), std::string::npos) << Tested;
}

using namespace neverd;

namespace {
/// A Thumb PE image of \p Code at 0x401000, which starts with the thunk of
/// the import \p Imported, `movw ip, #0x2000; movt ip, #0x40; ldr.w pc,
/// [ip]`, registered as the loader registers it, and the function \p Name
/// at \p Entry when given.
BinaryImage thumbThunkImage(std::vector<uint8_t> Code, const char *Imported,
                            va_t Entry = 0, const char *Name = nullptr) {
  constexpr va_t Text = 0x401000, Slot = 0x402000;
  const std::vector<uint8_t> Thunk = {0x42, 0xF2, 0x00, 0x0C, 0xC0, 0xF2,
                                      0x40, 0x0C, 0xDC, 0xF8, 0x00, 0xF0};
  std::copy(Thunk.begin(), Thunk.end(), Code.begin());
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = Entry ? Entry : Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Seg.FileSz = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Slot;
  Idata.Size = Idata.FileSz = 4;
  Idata.Data.resize(4);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  Import Imp;
  Imp.Module = "api-ms-win-crt-stdio-l1-1-0.dll";
  Imp.Name = Imported;
  Imp.IATAddr = Slot;
  Img.Imports.push_back(std::move(Imp));
  if (Entry) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  scanImportThunks(Img);
  EXPECT_EQ(Img.ImportStubIndices.count(Text), 1u);
  return Img;
}

/// The UCRT's header declaration of `__stdio_common_vfprintf`, which a
/// Windows image's calls of it follow.
class StdioHeader final : public NullDebugContext {
public:
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    if (Name != "__stdio_common_vfprintf")
      return std::nullopt;
    FunctionSym Function;
    Function.Name = Name.str();
    Function.ReturnType = NdType::makeInt(4, true);
    Function.CallConv = DebugCallConv::Cdecl;
    Function.PrototypeFromHeader = true;
    // An ARM pointer is four bytes.
    const auto Pointer = [] {
      TypeRef Type = NdType::makePtr();
      Type->Size = 4;
      return Type;
    };
    Function.Params = {{"_Options", NdType::makeInt(8, false)},
                       {"_Stream", Pointer()},
                       {"_Format", Pointer()},
                       {"_Locale", Pointer()},
                       {"_ArgList", Pointer()}};
    return Function;
  }
};

/// The HighC of \p Img, whose imports \p Debug declares.
std::string thumbHighC(const BinaryImage &Img, DebugContext &Debug) {
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  const PipelineResult Result = Pipeline().run(Img, Context, Options, &Debug);
  EXPECT_TRUE(Result.Success) << Result.Error;
  CEmitterOptions C;
  C.TheArch = Img.Arch;
  C.Format = Img.Format;
  C.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, C, &Debug));
  Out.flush();
  return Source;
}
} // namespace

namespace {
/// A Thumb PE image of \p Code at 0x401000 with the named functions.
BinaryImage
thumbImage(std::vector<uint8_t> Code,
           std::initializer_list<std::pair<va_t, const char *>> Functions) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = 0x401000;
  Seg.Size = Seg.FileSz = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}
} // namespace

// MSVC ARM pushes r3 beside the registers it saves to keep the stack 8-byte
// aligned and pops it back unread, and its /Od code homes r0 and r1 unread:
// those take no argument.  A function that reads a slot back, lets the
// frame's address out or may return r0 as it came keeps the register.
//   f:  push {r3, lr}; bl g; movs r0, #0; pop {r3, pc}
//   f2: push {r3, lr}; ldr r0, [sp]; pop {r3, pc}
//   f3: push {r3, lr}; mov r0, sp; bl g; pop {r3, pc}
//   g:  bx lr
//   g2: push {r0, r1}; push.w {r11, lr}; ldr r11, [sp], #4;
//       ldr pc, [sp], #12
//   g3: the same with movs r0, #0 after the second push
TEST(ARM32_SavedRegisters, AnAlignmentPushOfR3TakesNoArgument) {
  const BinaryImage Img = thumbImage(
      {0x08, 0xB5, 0x00, 0xF0, 0x0A, 0xF8, 0x00, 0x20, 0x08, 0xBD, // f
       0x08, 0xB5, 0x00, 0x98, 0x08, 0xBD,                         // f2
       0x08, 0xB5, 0x68, 0x46, 0x00, 0xF0, 0x01, 0xF8, 0x08, 0xBD, // f3
       0x70, 0x47,                                                 // g
       0x03, 0xB4, 0x2D, 0xE9, 0x00, 0x48, 0x5D, 0xF8, 0x04, 0xBB, // g2
       0x5D, 0xF8, 0x0C, 0xFB,                                     //
       0x03, 0xB4, 0x2D, 0xE9, 0x00, 0x48, 0x00, 0x20, 0x5D, 0xF8, // g3
       0x04, 0xBB, 0x5D, 0xF8, 0x0C, 0xFB},
      {{0x401000, "f"},
       {0x40100A, "f2"},
       {0x401010, "f3"},
       {0x40101A, "g"},
       {0x40101C, "g2"},
       {0x40102A, "g3"}});
  NullDebugContext Debug;
  const std::string C = thumbHighC(Img, Debug);
  const auto Signature = [&](const std::string &Name) {
    const std::string Definition = definitionOf(C, Name);
    return Definition.substr(0, Definition.find('{'));
  };
  EXPECT_NE(Signature("f").find("f(void)"), std::string::npos) << C;
  EXPECT_NE(Signature("f2").find("arg3"), std::string::npos) << C;
  EXPECT_NE(Signature("f3").find("arg3"), std::string::npos) << C;
  EXPECT_NE(Signature("g2").find("arg0"), std::string::npos) << C;
  EXPECT_EQ(Signature("g2").find("arg1"), std::string::npos) << C;
  EXPECT_NE(Signature("g3").find("g3(void)"), std::string::npos) << C;
}

// AAPCS passes a 64-bit integer in an even-odd register pair, its low word
// first.  `__stdio_common_vfprintf(unsigned __int64 options, FILE *stream,
// ...)` takes the options in r0:r1 and the stream in r2: matched to its
// parameters one register each, a call passed the options' high word as
// the stream and dropped the va_list.
//   f: push {r4, r5, r7, lr}; sub sp, #8; movs r4, #0; str r4, [sp];
//      str r4, [sp, #4]; mov r2, r0; mov r3, r1; movs r0, #1; movs r1, #2;
//      bl thunk; add sp, #8; pop {r4, r5, r7, pc}
TEST(ARM32_ImportCalls, ADoublewordArgumentTakesAnEvenOddPair) {
  std::vector<uint8_t> Code(0x40, 0x00);
  const std::vector<uint8_t> F = {0xB0, 0xB5, 0x82, 0xB0, 0x00, 0x24, 0x00,
                                  0x94, 0x01, 0x94, 0x02, 0x46, 0x0B, 0x46,
                                  0x01, 0x20, 0x02, 0x21, 0xFF, 0xF7, 0xED,
                                  0xFF, 0x02, 0xB0, 0xB0, 0xBD};
  std::copy(F.begin(), F.end(), Code.begin() + 0x10);
  const BinaryImage Img =
      thumbThunkImage(Code, "__stdio_common_vfprintf", 0x401010, "f");
  StdioHeader Header;
  const std::string Source = thumbHighC(Img, Header);
  EXPECT_NE(Source.find("__stdio_common_vfprintf(((uint64_t)2 << 32 | 1), "
                        "(FILE *)arg0, (const char *)arg1, 0, 0)"),
            std::string::npos)
      << Source;
}

// No Windows header library covers an ARM32 image, so no header declares
// its UCRT imports: the C library's prototype table does, with
// `__stdio_common_vfprintf`'s options a uint64_t, and the call takes them
// as the pair.  With the table's parameter count alone, the options' high
// word was the stream.
TEST(ARM32_ImportCalls, AnImportNoHeaderDeclaresTakesItsTablesTypes) {
  std::vector<uint8_t> Code(0x40, 0x00);
  const std::vector<uint8_t> F = {0xB0, 0xB5, 0x82, 0xB0, 0x00, 0x24, 0x00,
                                  0x94, 0x01, 0x94, 0x02, 0x46, 0x0B, 0x46,
                                  0x01, 0x20, 0x02, 0x21, 0xFF, 0xF7, 0xED,
                                  0xFF, 0x02, 0xB0, 0xB0, 0xBD};
  std::copy(F.begin(), F.end(), Code.begin() + 0x10);
  const BinaryImage Img =
      thumbThunkImage(Code, "__stdio_common_vfprintf", 0x401010, "f");
  NullDebugContext NoHeaders;
  const std::string Source = thumbHighC(Img, NoHeaders);
  EXPECT_NE(Source.find("__stdio_common_vfprintf(((uint64_t)2 << 32 | 1), "
                        "(FILE *)arg0, (const char *)arg1, 0, 0)"),
            std::string::npos)
      << Source;
}

// An MSVC import thunk composes its slot's address, `movw r12, #lo; movt
// r12, #hi; ldr pc, [r12]`, and its call through the loaded word named no
// import.  Where no call of the image names the thunk either, as the
// exception directory alone names `__C_specific_handler`'s, the thunk took
// no argument and passed the import unknown values.  Folded to the slot,
// its call is the import's, and it forwards the six words a caller passes.
TEST(ARM32_ImportThunks, AThunkComposingItsSlotForwardsItsArguments) {
  const BinaryImage Img = thumbThunkImage(std::vector<uint8_t>(0x10, 0x00),
                                          "__stdio_common_vfprintf");
  StdioHeader Header;
  const std::string Source = thumbHighC(Img, Header);
  EXPECT_NE(Source.find("__stdio_common_vfprintf(((uint64_t)((uint32_t)(arg1)) "
                        "<< 32 | (uint64_t)((uint32_t)(arg0))), (FILE *)arg2, "
                        "(const char *)arg3, (_locale_t)arg4, (char *)arg5)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

// Windows on ARM passes and returns a double in d0 (AAPCS-VFP).  The
// arity table declares `exp`, which no header declares for an ARM32 image,
// yet no ARM declaration bound a floating call, and a thunk's jump to it
// read the result from r0: `int32_t thunk(double) { return bit_cast(exp()) }`.
//   f: push {r7, lr}; vadd.f64 d0, d0, d0; bl thunk; pop {r7, pc}
TEST(ARM32_ImportCalls, AFloatingImportTakesAndReturnsItsDoubleInD0) {
  std::vector<uint8_t> Code(0x20, 0x00);
  const std::vector<uint8_t> F = {0x80, 0xB5, 0x30, 0xEE, 0x00, 0x0B,
                                  0xFF, 0xF7, 0xF3, 0xFF, 0x80, 0xBD};
  std::copy(F.begin(), F.end(), Code.begin() + 0x10);
  BinaryImage Img = thumbThunkImage(Code, "exp", 0x401010, "f");
  Img.Imports.front().Module = "api-ms-win-crt-math-l1-1-0.dll";
  NullDebugContext NoHeaders;
  const std::string Source = thumbHighC(Img, NoHeaders);
  EXPECT_EQ(Source.find("__builtin_bit_cast"), std::string::npos) << Source;
  llvm::SmallVector<llvm::StringRef, 2> LinkFlags;
#if defined(__linux__)
  LinkFlags.push_back("-Wl,--no-as-needed");
#endif
#ifndef _WIN32
  LinkFlags.push_back("-lm");
#endif
  source_call_execution_test::compileAndRun(Source + R"C(
#include <math.h>
int main(void) {
  return fabs(sub_401000(1.0) - 2.718281828459045) > 1e-12 ||
         fabs(f(0.5) - 2.718281828459045) > 1e-12;
}
)C",
                                            LinkFlags);
}
