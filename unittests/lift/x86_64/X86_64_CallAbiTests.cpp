//===- X86_64_CallAbiTests.cpp - x86-64 call arguments in HighC -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// System V x86-64 passes the seventh and later integer arguments on the
// stack, and a callee can come from a stack slot.  Each kernel is decompiled
// to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

#include <fstream>
#include <iterator>
#include <regex>
#include <string>

class X86_64_CallAbi : public HighCHostExecutionTest {
protected:
  /// Whether "gcc" is GCC building x86-64 Linux code, as the kernels that
  /// test GCC's own layout and scheduling need: elsewhere it may be another
  /// compiler or target another machine.
  bool gccBuildsX86_64Linux() {
    const RunResult Machine = exec("gcc", {"-dumpmachine"});
    return Machine.ok() && Machine.out.rfind("x86_64", 0) == 0 &&
           Machine.out.find("linux") != std::string::npos &&
           !exec("gcc", {"--version"}).contains("clang");
  }

  void SetUp() override {
    HighCHostExecutionTest::SetUp();
#if defined(__APPLE__) && defined(__aarch64__)
    // The generated C and LLVM IR retain x86 instructions and va_list layout.
    // Execute both them and the source oracle as x86-64 through Rosetta.
    HostFlags = {"-arch", "x86_64"};
#endif
  }

  /// Expect the C of both routes, HighC and then the LLVM route's, to run
  /// like \p Kernel built for System V x86-64 (expectHighCRunsLikeSource).
  void expectBothRoutesRunLikeSource(const std::string &Kernel,
                                     const std::string &Entry,
                                     std::initializer_list<const char *> Names,
                                     std::initializer_list<int> Values) {
    for (const bool Route : {false, true}) {
      SCOPED_TRACE(Route ? "LLVM C" : "HighC");
      LLVMC = Route;
      ASSERT_NO_FATAL_FAILURE(expectHighCRunsLikeSource(
          {"-target", "x86_64-linux-gnu"}, Kernel, Entry, Names, Values));
    }
  }

  /// Lifts \p Source's object to LLVM IR, links it with \p Source renamed,
  /// and compares \p Entry with the original over a range of arguments.
  /// \p Callee is the source's other function.
  void expectLiftedRunsLikeSource(const std::string &Source,
                                  const std::string &Entry,
                                  const std::string &Callee) {
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target compilation requires Clang";
    const auto SourceFile = tmpFile(Entry + ".c");
    const auto Object = tmpFile(Entry + ".o");
    const auto IR = tmpFile(Entry + ".ll");
    std::ofstream(SourceFile) << Source;
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O2", "-c",
                                 SourceFile.string(), "-o", Object.string()});
    ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
    const auto Lifted =
        exec(ndBin(), {"lift", Object.string(), "-o", IR.string()});
    ASSERT_EQ(Lifted.exitCode, 0) << Lifted.err;
    const auto Reference = tmpFile(Entry + "_ref.c");
    const auto Driver = tmpFile(Entry + "_driver.c");
    const auto Program = tmpFile(Entry + ".exe");
    std::ofstream(Reference) << std::regex_replace(
        std::regex_replace(Source, std::regex("\\b" + Entry + "\\b"),
                           "ref_" + Entry),
        std::regex("\\b" + Callee + "\\b"), "ref_" + Callee);
    std::ofstream(Driver) << "int " << Entry << "(int);\nint ref_" << Entry
                          << "(int);\n"
                          << "int main(void) {\n"
                          << "  for (int a = -3; a < 50; a += 7)\n"
                          << "    if (" << Entry << "(a) != ref_" << Entry
                          << "(a))\n"
                          << "      return 1;\n"
                          << "  return 0;\n"
                          << "}\n";
    std::vector<std::string> HostArgs = HostFlags;
    HostArgs.insert(HostArgs.end(),
                    {"-O1", "-w", IR.string(), Reference.string(),
                     Driver.string(), "-o", Program.string()});
    const auto Built = exec(NEVERD_TEST_CLANG, HostArgs);
    ASSERT_EQ(Built.exitCode, 0) << Built.err;
    EXPECT_EQ(exec(Program.string(), {}).exitCode, 0);
  }
};

// The callee's `setl al; setg cl` writes the low bytes of RAX and RCX and
// keeps the rest of each, which nothing reads: RCX is no fourth parameter,
// and the call passes the two arguments its caller sets.
TEST_F(X86_64_CallAbi, AByteWriteMakesNoParameter) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static int c64(long long, long long) __attribute__((noinline));
int cmpsum(int a) {
  unsigned h = 0, s = (unsigned)a | 1u;
  for (int i = 0; i < 12; i++) {
    unsigned xl = s, xh = s ^ 0x1234u, yl = s * 7u, yh = s + (unsigned)i;
    long long x = (long long)(((unsigned long long)xh << 32) | xl);
    long long y = (long long)(((unsigned long long)yh << 32) | yl);
    h = h * 131u + (unsigned)(c64(x, y) + 2);
    s = s * 1103515245u + 12345u;
  }
  return (int)h;
}
static int c64(long long a, long long b) { return a < b ? -1 : (a > b ? 1 : 0); }
)C",
                            "cmpsum", {"cmpsum", "c64"}, {0x71, 0, 1, -3, 77});
}

// An AVX2 callee ends with `vzeroupper`, which keeps every XMM register's
// low lanes: that reads no vector argument, so the callee takes only its
// pointer and its callers pass no vector value.
TEST_F(X86_64_CallAbi, VzeroupperReadsNoVectorArgument) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu", "-mavx2"}, R"C(
typedef int v8 __attribute__((vector_size(32)));
static int sum8(const int *p) __attribute__((noinline));
int avxsum(int a) {
  int buf[8];
  for (int i = 0; i < 8; i++)
    buf[i] = (a * (i + 1)) ^ i;
  return sum8(buf) + sum8(buf) * 3;
}
static int sum8(const int *p) {
  v8 v;
  __builtin_memcpy(&v, p, sizeof(v));
  v = v * v + v;
  int s = 0;
  for (int i = 0; i < 8; i++)
    s += v[i];
  return s;
}
)C",
                            "avxsum", {"avxsum", "sum8", "v8"},
                            {0x71, 0, 1, -3, 77});
}

// `test rdi, rdi; je; bsr rcx, rdi`: the bsr runs only with a nonzero
// source, so it always writes RCX, and the old RCX is no argument.
TEST_F(X86_64_CallAbi, AGuardedBsrReadsNoArgument) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static int top(unsigned long long x) __attribute__((noinline));
int usetop(int a) {
  unsigned long long v = (unsigned long long)a * 0x9E3779B97F4A7C15ull;
  return top(v) + top(v >> 7) * 3;
}
static int top(unsigned long long x) {
  if (!x)
    return -1;
  unsigned long long r;
  __asm__ volatile("bsr %1, %0" : "=c"(r) : "r"(x));
  return (int)r;
}
)C",
                            "usetop", {"usetop", "top"}, {0x71, 0, 1, -3, 77});
}

// Six register arguments and four pushed afresh on every iteration of a
// loop, whose stack pointer is a PHI at the loop head.
TEST_F(X86_64_CallAbi, StackArgumentsPushedInALoopReachTheCallee) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static unsigned ma(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                   unsigned, unsigned, unsigned, unsigned)
    __attribute__((noinline));
int manyarg(int a) {
  unsigned s = (unsigned)a, acc = 0x811C9DC5u;
  for (int k = 0; k < 40; k++) {
    unsigned r = ma(s, s ^ acc, s + (unsigned)k, acc, s * 3u, acc >> 2,
                    s ^ 0x9E3779B9u, acc * 7u, s + acc, (unsigned)k * 131u);
    acc = (acc ^ r) * 16777619u;
    s = s * 1664525u + 1013904223u;
  }
  return (int)acc;
}
static unsigned ma(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e,
                   unsigned f, unsigned g, unsigned h, unsigned i,
                   unsigned j) {
  return ((a * 131u + b) ^ (c << 3)) + (d - e) + (f ^ g) + (h * 5u) - i +
         (j >> 1);
}
)C",
                            "manyarg", {"manyarg", "ma"}, {-3, 0, 1, 77});
}

// `cmove` picks which of two stack slots the tail call loads its callee
// from, so the stores that fill both slots are read through that choice.
TEST_F(X86_64_CallAbi, ACalleePickedFromTwoStackSlotsIsTheStoredOne) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static int pick_p(int) __attribute__((noinline));
static int pick_q(int) __attribute__((noinline));
typedef int (*pick_fn)(int);
int pick(int a) {
  volatile pick_fn s0 = pick_p, s1 = pick_q;
  pick_fn fp = (a & 1) ? s1 : s0;
  return fp(a * 3 + 1);
}
static int pick_p(int x) {
  unsigned h = (unsigned)x;
  for (unsigned i = 0; i < 6; i++)
    h = h * 131u + i;
  return (int)h;
}
static int pick_q(int x) {
  return (int)(((unsigned)x * 2654435761u) ^ ((unsigned)x >> 7));
}
)C",
                            "pick", {"pick", "pick_p", "pick_q"},
                            {-3, 0, 1, 2, 77});
}

// At -O0 the loop leaves r >> 32 in rdx for the next indirect call and
// computes x through rcx, but on the first iteration rdx holds what the
// caller left there.  A register a join brings from entry is no argument:
// passed, it was a C variable read before any assignment, which Clang
// optimizes as it likes.
TEST_F(X86_64_CallAbi, ARegisterAJoinBringsFromEntryIsNoArgument) {
  KernelOptimization = "-O0";
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static long long td(long long x, int b) __attribute__((noinline));
int retll(int a) {
  unsigned w = (unsigned)a | 1u;
  long long acc = (long long)(unsigned)a;
  long long (*fp)(long long, int) = td;
  __asm__("" : "+r"(fp));
  for (int i = 0; i < 40; i++) {
    w = w * 1103515245u + 12345u;
    long long x = ((long long)w) | ((long long)(w >> 4) << 32);
    long long r = fp(x, (int)(w >> 9));
    acc = acc * 131 + (r ^ (r >> 32));
    acc ^= acc >> 17;
  }
  return (int)(acc ^ (acc >> 32));
}
static long long td(long long x, int b) {
  long long r = x * 131 + (long long)b;
  r ^= (long long)(unsigned)((unsigned long long)x >> 32) << 28;
  r += ((long long)b) << 33;
  return r;
}
)C",
                            "retll", {"retll", "td"}, {0, 1, -1, 0x4567, 77});
}

// `long f(long a) { return (signed char)a; }` sign-extends the low byte of
// whatever register its caller passes.  Built apart from its callers, its C
// must do the same: a `signed char` parameter would let Clang take the
// caller's extension as done and return 0x1FE whole.
TEST_F(X86_64_CallAbi, AByteReadOfAParameterTakesTheWholeRegister) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("sext8.c");
  const auto Object = tmpFile("sext8.o");
  std::ofstream(Source) << "long sext8(long a) { return (signed char)a; }\n";
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O2", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());

  const auto HighC = tmpFile("sext8-highc.c");
  const auto HighCObject = tmpFile("sext8-highc.o");
  const auto Driver = tmpFile("sext8-driver.c");
  const auto Program = tmpFile("sext8.exe");
  std::ofstream(HighC) << C;
  std::ofstream(Driver) << "long sext8(long);\n"
                           "int main(void) {\n"
                           "  return sext8(0x1FE) == -2 && sext8(0x17F) == 127 "
                           "? 0 : 1;\n"
                           "}\n";
  std::vector<std::string> CompileArgs = HostFlags;
  CompileArgs.insert(CompileArgs.end(),
                     {"-O2", "-c", HighC.string(), "-o", HighCObject.string()});
  const auto BuiltC = exec(NEVERD_TEST_CLANG, CompileArgs);
  ASSERT_EQ(BuiltC.exitCode, 0) << BuiltC.err << "\n" << C;
  std::vector<std::string> LinkArgs = HostFlags;
  LinkArgs.insert(LinkArgs.end(), {"-O2", Driver.string(), HighCObject.string(),
                                   "-o", Program.string()});
  const auto Linked = exec(NEVERD_TEST_CLANG, LinkArgs);
  ASSERT_EQ(Linked.exitCode, 0) << Linked.err;
  EXPECT_EQ(exec(Program.string(), {}).exitCode, 0) << C;
}

// Ten doubles: eight in xmm0-xmm7 and two on the stack.  The callee's lift
// also lists the six integer argument registers, which it never reads; its
// definition leaves them out, and so must its prototype and its calls.
TEST_F(X86_64_CallAbi, AStackDoubleCalleeIsDeclaredAsItIsDefined) {
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu", "-ffp-contract=off"},
      R"C(
static double d10(double, double, double, double, double, double, double,
                  double, double, double) __attribute__((noinline));
int sumd10(int a) {
  double b = (double)(a & 0xffff);
  return (int)d10(b, b + 1, b + 2, b + 3, b + 4, b + 5, b + 6, b + 7, b + 8,
                  b + 9);
}
static double d10(double a, double b, double c, double d, double e, double f,
                  double g, double h, double i, double j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 +
         j * 10;
}
)C",
      "sumd10", {"sumd10", "d10"}, {0, 1, 0x41, 0x1234});
}

// A 32-byte record passes on the stack, and the caller copies it there with
// two 16-byte stores; the callee reads each field with a floating-point
// memory operand.  The call passes one argument per eight-byte slot.
TEST_F(X86_64_CallAbi, ARecordCopiedToTheStackPassesEverySlot) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct {
  double a, b, c, d;
} H4;
static double sh4(H4 s, double e) __attribute__((noinline));
int hfa4d(int x) {
  H4 s;
  s.a = (double)(x & 0xff);
  s.b = (double)((x >> 8) & 0xff);
  s.c = (double)(x * 3);
  s.d = (double)(x - 7);
  return (int)sh4(s, (double)(x ^ 5));
}
static double sh4(H4 s, double e) {
  return s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * e;
}
)C",
                            "hfa4d", {"hfa4d", "sh4", "H4"},
                            {0, 1, -1, 0x10, 0x1234, 1000});
}

// A 48-byte record passes in memory, and the callee reads no register: its
// six slots are its parameters, after a place for each register it does not
// read.  The caller copies the record there with three 16-byte stores from
// its own copy.
TEST_F(X86_64_CallAbi, ARecordInMemoryAloneTakesTheSlotsPastTheRegisters) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct {
  long a, b, c, d, e, f;
} S6;
long sum6(S6 s) __attribute__((noinline));
int bigstruct(int x) {
  S6 s;
  s.a = x;
  s.b = x + 1;
  s.c = (long)x * 2;
  s.d = x - 3;
  s.e = (long)x * 5;
  s.f = x + 7;
  return (int)sum6(s);
}
long sum6(S6 s) {
  return s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * s.e + 6 * s.f;
}
)C",
                            "bigstruct", {"bigstruct", "sum6", "S6"},
                            {0, 1, -1, 0x1234, 1000});
}

// A record in memory before three integers: they take the first three
// registers, and the record's slots follow every register position, the
// three the callee does not read included.
TEST_F(X86_64_CallAbi, ARecordInMemoryAfterRegistersTakesTheSlotsPastThem) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct {
  long a, b, c;
} S3;
long mix3(S3 s, long p, long q, long r) __attribute__((noinline));
int mixstruct(int x) {
  S3 s;
  s.a = x;
  s.b = x + 1;
  s.c = x + 2;
  return (int)mix3(s, (long)x * 2, (long)x * 3, (long)x * 4);
}
long mix3(S3 s, long p, long q, long r) {
  return s.a + 3 * s.b + 7 * s.c + p * q - r;
}
)C",
                            "mixstruct", {"mixstruct", "mix3", "S3"},
                            {0, 1, -1, 0x99, 1000});
}

// `cvtsi2sd` writes xmm0's low double and keeps its incoming upper lane,
// and `unpcklpd xmm1, xmm0` reads only the low double of each operand: the
// function takes no vector argument, only its int.
TEST_F(X86_64_CallAbi, ABroadcastOfAConvertedIntTakesNoVectorArgument) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
typedef struct {
  double a, b, c, d;
} B4;
static double sb4(B4 s, double e) __attribute__((noinline));
int bcast(int x) {
  double v = (double)x;
  B4 s;
  s.a = v;
  s.b = v + 1;
  s.c = v + 2;
  s.d = v + 3;
  return (int)sb4(s, v + 4);
}
static double sb4(B4 s, double e) {
  return s.a + 2 * s.b + 3 * s.c + 4 * s.d + 5 * e;
}
)C",
                            "bcast", {"bcast", "sb4", "B4"},
                            {0, 1, -1, 0x10, 77, 1000});
}

// `cvtsi2sd` and `mulsd` write xmm0's low double and keep its upper lane,
// which ep never set, and `cvtsi2ss` and `mulss` the twelve bytes above
// ef's float.  gp and hf read only the scalar, so each call passes it
// alone: the LLVM route, whose parameter takes the whole register, passes
// zero above it, and neither C reads the caller's unknown entry value.
TEST_F(X86_64_CallAbi, AScalarArgumentPassesOnlyItsLowLane) {
  expectBothRoutesRunLikeSource(R"C(
int gp(double x) __attribute__((noinline));
int hf(float y) __attribute__((noinline));
int ep(int a) __attribute__((noinline));
int ef(int a) __attribute__((noinline));
int gp(double x) { return (int)(x * 4.0) + 1; }
int hf(float y) { return (int)(y * 2.0f) + 3; }
int ep(int a) { return gp(a * 0.5); }
int ef(int a) { return hf(a * 1.5f); }
int lane_e(int a) { return ep(a) + ef(a) * 3; }
)C",
                                "lane_e", {"lane_e", "gp", "hf", "ep", "ef"},
                                {0, 1, -1, 7, 1000});
}

// A printf-family call whose format is a constant string passes the
// arguments its conversions name, a double in the low lane of its vector
// register, which AL counts.  Each call passes its doubles as doubles, the
// ones `cvtsi2sd`, `mulsd` and `addsd` leave beside an upper lane the caller
// never set as well as one `movsd` loads whole, and the constants pc and sf
// load from read-only data, which C passes as their values; as integer bits,
// C would pass them in integer registers, where these routines read none.
// sf hashes the text snprintf formats.
TEST_F(X86_64_CallAbi, AFormattedCallPassesItsDoublesAsDoubles) {
  expectBothRoutesRunLikeSource(
      R"C(
#include <stdio.h>
char Buf[96];
int gp(double x) __attribute__((noinline));
int ep(int a) __attribute__((noinline));
int pf(int a, const double *p) __attribute__((noinline));
int pc(int a) __attribute__((noinline));
int df(int a) __attribute__((noinline));
int sf(int a) __attribute__((noinline));
int gp(double x) { return printf("%f\n", x); }
int ep(int a) { return gp(a * 0.5); }
int pf(int a, const double *p) { return printf("%f %d\n", *p, a); }
int pc(int a) { return printf("%f %d\n", 2.5, a); }
int df(int a) { return dprintf(1, "%d %.2f %e\n", a, a * 2.5, a + 0.5); }
int sf(int a) {
  int n = snprintf(Buf, sizeof Buf, "%.3f|%d|%g|%.2f", a * 2.5, a, a * 0.25,
                   0.75);
  unsigned h = 0;
  for (int i = 0; i < n && i < (int)sizeof Buf; i++)
    h = h * 31 + (unsigned char)Buf[i];
  return (int)h;
}
int fmt_e(int a) {
  double v[2] = {a * 0.75, a - 0.5};
  return ep(a) + pf(a, &v[1]) * 3 + df(a) * 5 + sf(a) * 7 + pc(a) * 11;
}
)C",
      "fmt_e", {"fmt_e", "gp", "ep", "pf", "pc", "df", "sf", "Buf"},
      {0, 1, -1, 7, 1000});
}

// sb formats into a stack buffer.  The LLVM route's frame record lays the
// buffer's start out as a `uint64_t` member, whose address C converts to the
// `char *` <stdio.h> declares for snprintf's buffer only through void *.
// The hash of the text the call formats matches the kernel's.
TEST_F(X86_64_CallAbi, AStackBufferPassesAsTheHeadersPointer) {
  expectBothRoutesRunLikeSource(R"C(
#include <stdio.h>
int sb(int a) __attribute__((noinline));
int sb(int a) {
  char b[96];
  int n = snprintf(b, sizeof b, "%d|%d", a, a * 3);
  unsigned h = 0;
  for (int i = 0; i < n && i < (int)sizeof b; i++)
    h = h * 31 + (unsigned char)b[i];
  return (int)h;
}
int buf_e(int a) { return sb(a); }
)C",
                                "buf_e", {"buf_e", "sb"}, {0, 1, -1, 7, 1000});
}

// A pointer walking a string is compared against its end through `~W`, the
// bitwise complement of the string's address: the operator takes the address
// as the integer it is, never the array.
TEST_F(X86_64_CallAbi, AComplementOfAStringAddressIsAnInteger) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
static const char W[] = "the_quick_brown_fox_jumps_over_the_lazy_dog_0123";
int strroll(int a) {
  unsigned s = (unsigned)a, h = 0;
  const int N = (int)(sizeof(W) - 1);
  for (int it = 0; it < 16; it++) {
    s = s * 1103515245u + 12345u;
    const char *p = W + ((s >> 6) % (unsigned)N);
    unsigned r = 0;
    for (int k = 0; k < 12; k++) {
      r = r * 31u + (unsigned)(unsigned char)*p;
      p++;
      if (!*p)
        p = W;
    }
    h = h * 131u + r;
    h ^= h >> 10;
  }
  return (int)h;
}
)C",
                            "strroll", {"strroll", "W"}, {0, 1, -1, 0x44, 77});
}

// A System V variadic function tests AL and saves the argument registers to
// its register save area, which va_arg reads.  It takes every one of them as
// a parameter: the integer registers and the vector registers' doubles.
TEST_F(X86_64_CallAbi, AVariadicCalleeTakesItsIntegerArguments) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
#include <stdarg.h>
static int si(int n, ...) __attribute__((noinline));
int vsum_i(int a) {
  unsigned u = (unsigned)a;
  return si(4, (int)u, (int)(u * 3u + 1u), (int)(u ^ 0x55u), (int)(u + 7u));
}
static int si(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s * 31 + va_arg(ap, int);
  va_end(ap);
  return s;
}
)C",
                            "vsum_i", {"vsum_i", "si"}, {0, 1, -1, 0x41, 77});
}

static const char *const VariadicDoubles = R"C(
#include <stdarg.h>
static double sd(int n, ...) __attribute__((noinline));
int vsum_d(int a) {
  double x = (double)a;
  return (int)sd(3, x, x * 2.0, x + 0.5);
}
static double sd(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; i++)
    s = s * 3.0 + va_arg(ap, double);
  va_end(ap);
  return s;
}
)C";

// The floating arguments of a variadic call travel in the vector registers,
// which the callee saves when AL says the caller set any.
TEST_F(X86_64_CallAbi, AVariadicCalleeTakesItsDoubleArguments) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, VariadicDoubles,
                            "vsum_d", {"vsum_d", "sd"}, {0, 1, -1, 7, 1000});
}

// The LLVM route reads the same parameters: its lifted callee reads the
// doubles a caller passes.
TEST_F(X86_64_CallAbi, ALiftedVariadicCalleeTakesItsDoubleArguments) {
  expectLiftedRunsLikeSource(VariadicDoubles, "vsum_d", "sd");
}

static const char *const ForwardedDoubles = R"C(
long logv(const char *f, ...) __attribute__((noinline));
long gd(double x) __attribute__((noinline));
long gdc(double x) __attribute__((noinline));
long gm(double x) __attribute__((noinline));
long g2d(double a, double b) __attribute__((noinline));
long gd(double x) { return logv("y", x); }
long gdc(double x) { return logv("y", x) + 1; }
long gm(double x) { return logv("z", x, 2.5); }
long g2d(double a, double b) { return logv("w", a, b); }
long logv(const char *f, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, f);
  long s = f[0];
  if (s == 'y') {
    s += (long)(__builtin_va_arg(ap, double) * 4);
  } else {
    double a = __builtin_va_arg(ap, double);
    s += (long)(a * (s == 'z' ? 8 : 16) - __builtin_va_arg(ap, double) * 2);
  }
  __builtin_va_end(ap);
  return s;
}
int fwd_e(int a) {
  return (int)(gd(a * 0.5) + gdc(a * 0.25) * 3 + gm(a + 0.5) * 5 +
               g2d(a * 1.5, a - 2.0) * 7);
}
)C";

// A call loads AL with the number of vector registers it passes a variadic
// callee, which saves them for va_arg.  gd (`lea rdi; mov al, 1; jmp logv`),
// gdc, which calls logv, gm, which sets XMM1 and passes on XMM0, and g2d pass
// their own doubles on untouched below that count: each takes them as its
// parameters, and fwd_e passes the doubles it computes.
TEST_F(X86_64_CallAbi, AVariadicCallPassesOnTheDoublesALCounts) {
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu"}, ForwardedDoubles, "fwd_e",
      {"fwd_e", "gd", "gdc", "gm", "g2d", "logv"}, {0, 1, -1, 7, 1000});
}

// The LLVM route's C passes them alike, though it holds a vector register
// whole: the upper lanes a scalar conversion leaves as fwd_e's entry value
// pass as zero, and gdc's alignment `push rax` passes logv nothing.
TEST_F(X86_64_CallAbi, ALiftedVariadicCallPassesOnTheDoublesALCounts) {
  LLVMC = true;
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu"}, ForwardedDoubles, "fwd_e",
      {"fwd_e", "gd", "gdc", "gm", "g2d", "logv"}, {0, 1, -1, 7, 1000});
}

static const char *const VariadicStackIntegers = R"C(
#include <stdarg.h>
static int sb(int n, ...) __attribute__((noinline));
int vsum_big(int a) {
  unsigned u = (unsigned)a;
  return sb(8, (int)u, (int)(u + 1u), (int)(u + 2u), (int)(u + 3u),
            (int)(u + 4u), (int)(u + 5u), (int)(u + 6u), (int)(u + 7u)) ^
         sb(2, (int)(u * 3u), (int)(u ^ 9u));
}
static int sb(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s = s * 131 + va_arg(ap, int);
  va_end(ap);
  return s;
}
)C";

// Past the six integer registers, va_arg reads its arguments from the stack
// above the return address.  The callee takes each slot its widest call
// stores there as a parameter after every register one, and a call that
// stores fewer passes zero for the rest.
TEST_F(X86_64_CallAbi, AVariadicCalleeTakesItsStackArguments) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"},
                            VariadicStackIntegers, "vsum_big",
                            {"vsum_big", "sb"}, {0, 1, -1, 0x9b, 77});
}

// A lifted call passes those slots after zero for every vector register its
// caller left unset, as the callee takes them in that order.
TEST_F(X86_64_CallAbi, ALiftedVariadicCalleeTakesItsStackArguments) {
  expectLiftedRunsLikeSource(VariadicStackIntegers, "vsum_big", "sb");
}

// logv's va_start stores gp_offset 8: one integer register carries a named
// parameter, and past it va_arg reads no more arguments than a caller
// passes.  gdc passes RSI to R9 on untouched, and g2's second call finds in
// them only what its first call left.  Neither sets them for logv, so each
// C passes zero there, and the LLVM route's, like HighC, gives gdc no
// parameter.
TEST_F(X86_64_CallAbi, AVariadicCallPassesZeroInRegistersItLeavesUnset) {
  expectBothRoutesRunLikeSource(R"C(
long logv(const char *f, ...) __attribute__((noinline));
long gdc(void) __attribute__((noinline));
long g2(void) __attribute__((noinline));
long gdc(void) { return logv("y") + 1; }
long g2(void) {
  long a = logv("y");
  return a + logv("y") * 2;
}
long logv(const char *f, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, f);
  long s = f[0] == 'x' ? __builtin_va_arg(ap, long) : 3;
  __builtin_va_end(ap);
  return s;
}
int unset_e(int a) { return (int)(gdc() + g2() * 5) + a; }
)C",
                                "unset_e", {"unset_e", "gdc", "g2", "logv"},
                                {0, 1, -1, 7, 1000});
  if (HasFatalFailure() || IsSkipped())
    return;
  std::ifstream Input(tmpFile("decompiled.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  EXPECT_TRUE(std::regex_search(C, std::regex("\\bgdc\\((void)?\\) \\{"))) << C;
}

// gdc aligns its stack for the call with `push rax`, which stores RAX's
// incoming value where a stack argument would go, and g7 with one above the
// two longs it pushes.  RAX holds nothing at entry, so neither slot passes
// logv an argument.  g6 overwrites its pushed slot with its sixth long and g8
// pushes three below a `sub rsp, 8`: each passes its own, which logv's
// va_arg reads.
TEST_F(X86_64_CallAbi, AnAlignmentPushPassesNoVariadicStackArgument) {
  expectBothRoutesRunLikeSource(R"C(
long logv(const char *f, ...) __attribute__((noinline));
long gdc(void) __attribute__((noinline));
long g6(long a) __attribute__((noinline));
long g7(long a) __attribute__((noinline));
long g8(long a) __attribute__((noinline));
long gdc(void) { return logv("y") + 1; }
long g6(long a) {
  return logv("llllll", a, a + 1, a + 2, a + 3, a + 4, a * 5) + 1;
}
long g7(long a) {
  return logv("lllllll", a, a + 1, a + 2, a + 3, a + 4, a * 5, a ^ 6) + 1;
}
long g8(long a) {
  return logv("llllllll", a, a + 1, a + 2, a + 3, a + 4, a * 5, a ^ 6, a - 7) +
         1;
}
long logv(const char *f, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, f);
  long s = 0;
  for (const char *p = f; *p; ++p)
    s = s * 31 + (*p == 'l' ? __builtin_va_arg(ap, long) : 1);
  __builtin_va_end(ap);
  return s;
}
int slots_e(int a) { return (int)(gdc() + g6(a) * 3 + g7(a) * 5 + g8(a) * 7); }
)C",
                                "slots_e",
                                {"slots_e", "gdc", "g6", "g7", "g8", "logv"},
                                {0, 1, -1, 7, 1000});
}

// ef reserves its local double's slot with `push rax`, which stores RAX's
// incoming value there, and overwrites it before it passes the slot's
// address to pf; ep passes its local's address to put, which writes it.
// RAX holds nothing at entry and neither function reads the slot back, so
// neither C stores an unknown value there.
TEST_F(X86_64_CallAbi, AnUnreadAlignmentPushStoresNoUnknownValue) {
  expectBothRoutesRunLikeSource(R"C(
int Sink;
void put(int *p, int a) __attribute__((noinline));
int pf(int a, const double *p) __attribute__((noinline));
int ef(int a) __attribute__((noinline));
int ep(int a) __attribute__((noinline));
void put(int *p, int a) {
  *p = a * 7 + 1;
  Sink += a;
}
int pf(int a, const double *p) { return (int)(*p * 2.0) + a; }
int ef(int a) {
  double v = a * 0.75;
  return pf(a, &v);
}
int ep(int a) {
  int x;
  put(&x, a);
  return Sink;
}
int slot_e(int a) { return ef(a) + ep(a) * 3; }
)C",
                                "slot_e",
                                {"slot_e", "Sink", "put", "pf", "ef", "ep"},
                                {0, 1, -1, 7, 1000});
}

// out_e reserves its local's slot with `push rax` and passes the slot's
// address to get, which fills it, before it reads the local back.  RAX
// carries nothing into a function that is not variadic, so the read sees
// what get wrote: neither C stores RAX's unknown incoming value there.
TEST_F(X86_64_CallAbi, ALocalARoutineFillsBehindAPushHoldsWhatItWrote) {
  expectBothRoutesRunLikeSource(R"C(
void get(int *p, int a) __attribute__((noinline));
void get(int *p, int a) { *p = a * 7 + 1; }
int out_e(int a) {
  int x;
  get(&x, a);
  return x + 2;
}
)C",
                                "out_e", {"out_e", "get"}, {0, 1, -1, 7, 1000});
}

/// The definition of \p Name in \p C, from the line that opens it to its
/// closing brace; empty where \p C defines no such function.
static std::string definitionOf(const std::string &C, const std::string &Name) {
  std::smatch Head;
  if (!std::regex_search(
          C, Head,
          std::regex("(^|\n)[^\n;]*\\b" + Name + "\\([^\n;]*\\) \\{\n")))
    return {};
  const size_t Begin = static_cast<size_t>(Head.position(0));
  return C.substr(Begin, C.find("\n}\n", Begin) - Begin);
}

// spill reads back the slot of its `push rax` before any routine has the
// slot's address, a value it keeps across its body, and chain saves R10, a
// nested function's static chain, where a routine fills the slot: neither
// is a local filled behind an alignment push, and each C still fails where
// it uses the register's unknown incoming value.
TEST_F(X86_64_CallAbi, AReadBackScratchSaveKeepsItsUnknownValue) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("kernel.c");
  const auto Object = tmpFile("kernel.o");
  std::ofstream(Source) << R"C(
long spill(long a);
long chain(long a);
void fill(long *p, long a) __attribute__((noinline));
void fill(long *p, long a) { *p = a * 3; }
__asm__(".text\n"
        ".globl spill\n"
        ".type spill,@function\n"
        "spill:\n"
        "  pushq %rax\n"
        "  leaq 1(%rdi), %rax\n"
        "  popq %rcx\n"
        "  addq %rcx, %rax\n"
        "  retq\n"
        ".globl chain\n"
        ".type chain,@function\n"
        "chain:\n"
        "  pushq %r10\n"
        "  movq %rdi, %rsi\n"
        "  movq %rsp, %rdi\n"
        "  callq fill\n"
        "  popq %rax\n"
        "  retq\n");
)C";
  const auto Compiled =
      exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-O2", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  for (const bool Route : {false, true}) {
    SCOPED_TRACE(Route ? "LLVM C" : "HighC");
    const auto Decompiled =
        Route ? decompileToC(Object) : decompileToHighC(Object);
    ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
    std::ifstream Input(tmpFile(Route ? "decompiled.c" : "decompiled_high.c"));
    const std::string C((std::istreambuf_iterator<char>(Input)),
                        std::istreambuf_iterator<char>());
    for (const char *Name : {"spill", "chain"}) {
      const std::string Body = definitionOf(C, Name);
      ASSERT_FALSE(Body.empty()) << Name << "\n" << C;
      EXPECT_NE(Body.find("unknown"), std::string::npos) << Body;
    }
  }
}

static const char *const VariadicForwarders = R"C(
#include <stdarg.h>
static int vsum(int n, va_list ap) __attribute__((noinline));
static int sum(int n, ...) __attribute__((noinline));
static double vdsum(int n, va_list ap) __attribute__((noinline));
static double dsum(int n, ...) __attribute__((noinline));
static int vsum(int n, va_list ap) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int) * (i + 1);
  return s;
}
static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int r = vsum(n, ap);
  va_end(ap);
  return r;
}
static double vdsum(int n, va_list ap) {
  double s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, double) * (i + 1);
  return s;
}
static double dsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double r = vdsum(n, ap);
  va_end(ap);
  return r;
}
int forward(int x) {
  return sum(0) + sum(2, x, 3) + sum(7, x, 1, 2, 3, 4, 5, x * 2) +
         sum(6, x, 1, 2, 3, 4, 0) + (int)dsum(2, 1.5, (double)x) +
         (int)dsum(1, (double)x * 3.0);
}
)C";

// A variadic function whose va_list only reaches a call, the
// `error(fmt, ...)` that calls `verror(fmt, va)`, is C's own: it takes its
// named parameter and `...`, starts its va_list with va_start, and each
// call passes what its caller sets.  `sum(0)` passes no argument past the
// count, though another call passes two on the stack: those of a call that
// passes fewer were only padding.  `sum(6, ..., 0)` passes its zero.
TEST_F(X86_64_CallAbi, AVariadicForwarderPassesItsVaListOn) {
  expectHighCRunsLikeSource(
      {"-target", "x86_64-linux-gnu"}, VariadicForwarders, "forward",
      {"forward", "sum", "vsum", "dsum", "vdsum"}, {0, 1, -1, 7, 1000});
  if (HasFatalFailure() || IsSkipped())
    return;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  const auto Definition = [&](const std::string &Name) {
    const size_t At = C.find(" " + Name + "(int32_t arg0");
    const size_t Open = At == std::string::npos ? At : C.find('{', At);
    if (Open == std::string::npos || C.find(';', At) < Open)
      return std::string();
    return C.substr(At, C.find("\n}\n", At) - At);
  };
  for (const std::string Name : {"sum", "dsum"}) {
    const std::string Body = Definition(Name);
    ASSERT_FALSE(Body.empty()) << Name << "\n" << C;
    EXPECT_EQ(Body.rfind(" " + Name + "(int32_t arg0, ...) {", 0), 0u) << C;
    EXPECT_NE(Body.find("va_start(arguments, arg0);"), std::string::npos) << C;
    EXPECT_NE(Body.find("va_end(arguments);"), std::string::npos) << C;
  }
  const std::string Caller = C.substr(C.find(" forward(int32_t arg0) {"));
  EXPECT_NE(Caller.find("sum(0)"), std::string::npos) << C;
  EXPECT_EQ(Caller.find("0.0"), std::string::npos) << C;
  EXPECT_EQ(Caller.find(", 0, 0)"), std::string::npos) << C;
  EXPECT_NE(Caller.find(", 4, 0)"), std::string::npos) << C;
  // Clang keeps x across the first dsum call in [rsp], below the stack
  // arguments: no integer argument.
  EXPECT_NE(Caller.find("dsum(2, 1.5, "), std::string::npos) << C;
}

// `ap` and `buf` live at different times, so Clang gives them one stack
// slot: report passes its address to vsum, which takes a va_list, and then
// to fill, which writes 200 bytes there.
static const char *const VaListSlotReuse = R"C(
#include <stdarg.h>
static int vsum(int n, va_list ap) __attribute__((noinline));
static int vsum(int n, va_list ap) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int) * (i + 1);
  return s;
}
static int fill(int n, char *buf, unsigned size) __attribute__((noinline));
static int fill(int n, char *buf, unsigned size) {
  for (unsigned i = 0; i < size; i++)
    buf[i] = (char)(n + i);
  int s = 0;
  for (unsigned i = 0; i < size; i += 7)
    s += buf[i] * (int)(i + 1);
  return s;
}
static int report(int n, ...) __attribute__((noinline));
static int report(int n, ...) {
  int r;
  {
    va_list ap;
    va_start(ap, n);
    r = vsum(n, ap);
    va_end(ap);
  }
  if (n) {
    char buf[200];
    r += fill(n, buf, sizeof buf);
  }
  return r;
}
int entry(int x) { return report(2, x, 3) + report(0) + report(3, x, x, x); }
)C";

// C's va_list holds the 24 bytes of one, which fill would write past: the
// slot is no va_list there, and C passes report's va_list to no call.
TEST_F(X86_64_CallAbi, ABufferInTheVaListsSlotTakesNoVaList) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, VaListSlotReuse,
                            "entry", {"entry", "report", "vsum", "fill"},
                            {0, 1, -1, 7, 1000});
  if (HasFatalFailure() || IsSkipped())
    return;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(C.find("arguments"), std::string::npos) << C;
}

// GCC sets a call's integer arguments before it converts its floating ones
// (`mov edi, 2; cvtsi2sd xmm1, ebp; call dsum`), whose MXCSR state ends no
// argument setup, emits the static functions first, the first of them at
// address 0, and sets R9 for `sum(7, ...)` before the call to `sum` ahead
// of it, which leaves R9 alone (-fipa-ra).
static const char *const GccVariadicForwarders = R"C(
#include <stdarg.h>
static int sum(int n, ...) __attribute__((noinline));
static double dsum(int n, ...) __attribute__((noinline));
static int vsum(int n, va_list ap) __attribute__((noinline));
static double vdsum(int n, va_list ap) __attribute__((noinline));
int forward(int x) {
  return sum(0) + sum(2, x, 3) + sum(7, x, 1, 2, 3, 4, 5, x * 2) +
         sum(6, x, 1, 2, 3, 4, 0) + (int)dsum(2, 1.5, (double)x) +
         (int)dsum(1, (double)x * 3.0);
}
static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int r = vsum(n, ap);
  va_end(ap);
  return r;
}
static double dsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double r = vdsum(n, ap);
  va_end(ap);
  return r;
}
static int vsum(int n, va_list ap) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int) * (i + 1);
  return s;
}
static double vdsum(int n, va_list ap) {
  double s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, double) * (i + 1);
  return s;
}
)C";

TEST_F(X86_64_CallAbi, ArgumentSetupCrossesFloatingConversions) {
  if (!gccBuildsX86_64Linux())
    GTEST_SKIP() << "needs GCC building x86-64 Linux code";
  expectHighCRunsLikeSource("gcc", {}, GccVariadicForwarders, "forward",
                            {"forward", "sum", "vsum", "dsum", "vdsum"},
                            {0, 1, -1, 7, 1000});
  if (HasFatalFailure() || IsSkipped())
    return;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  const std::string Caller = C.substr(C.find(" forward(int32_t arg0) {"));
  EXPECT_NE(Caller.find("dsum(2, "), std::string::npos) << C;
  // The arguments of the call opening at \p Open, as top-level commas count.
  const auto Arguments = [&](size_t Open) {
    size_t Count = 1;
    int Depth = 0;
    for (size_t I = Open; I < Caller.size(); ++I) {
      if (Caller[I] == '(')
        ++Depth;
      else if (Caller[I] == ')' && --Depth == 0)
        return Count;
      else if (Caller[I] == ',' && Depth == 1)
        ++Count;
    }
    return size_t(0);
  };
  // AL says one vector register: XMM1, still holding x * 3.0, passes none.
  const size_t One = Caller.find("dsum(1, ");
  ASSERT_NE(One, std::string::npos) << C;
  EXPECT_EQ(Arguments(One + 4), 2u) << C;
}

// Debug information declares `sum(int n, ...)`, which C defines with every
// argument register: the results of the calls the variadic arguments take
// are read, and assigned.
TEST_F(X86_64_CallAbi, ACallResultPassedAsAVariadicArgumentIsAssigned) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu", "-g"}, R"C(
#include <stdarg.h>
static int sum(int n, ...) __attribute__((noinline));
static int twice(int x) __attribute__((noinline));
static int sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int) * (i + 1);
  va_end(ap);
  return s;
}
static int twice(int x) { return 2 * x + 1; }
int entry(int x) { return sum((x & 1) + 1, twice(x), twice(x + 3)); }
)C",
                            "entry", {"entry", "sum", "twice"},
                            {0, 1, -1, 7, 1000});
}

// fwd returns what make returns, a struct of two registers, by a tail
// call (`xor rdi, 5; jmp make`).  The call returns both registers, not the
// RDX fwd held before it, which entry would read as the second field.
TEST_F(X86_64_CallAbi, ATailCallForwardsATwoRegisterStruct) {
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, R"C(
struct pair { long a, b; };
struct pair make(long x) __attribute__((noinline));
struct pair fwd(long x) __attribute__((noinline));
struct pair make(long x) { struct pair p = {x * 3, x + 7}; return p; }
struct pair fwd(long x) { return make(x ^ 5); }
long entry(long x) { struct pair p = fwd(x); return p.a * 2 + p.b; }
)C",
                            "entry", {"entry", "fwd", "make"},
                            {0, 1, -1, 7, 1000});
}

// A relocatable object's first function sits at address 0, which a call
// still names: GCC emits a static function first, and a call to it reads
// the callee's summary as any other does.
TEST_F(X86_64_CallAbi, ACallToTheFunctionAtAddressZeroPassesItsArguments) {
  if (!gccBuildsX86_64Linux())
    GTEST_SKIP() << "needs GCC building x86-64 Linux code";
  expectHighCRunsLikeSource("gcc", {}, R"C(
static int twice(int x, int y) __attribute__((noinline));
static int twice(int x, int y) { return x * 2 + y; }
int entry(int x) { return twice(x, 1) + twice(x + 3, x); }
)C",
                            "entry", {"entry", "twice"}, {0, 1, -1, 7, 1000});
}
