//===- X86_32_CallAbiTests.cpp - i386 call arguments in HighC -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Compilers give a directly called static i386 function a register
// convention: Clang's fastcc takes ECX and EDX, GCC's regparm EAX, EDX and
// ECX, then the stack.  A wrapper that only passes its arguments on reads
// them nowhere else.  Each kernel is decompiled to HighC and run on the host
// against its own C.
//
//===----------------------------------------------------------------------===//

#include "../../HostCompiler.h"
#include "HighCHostExecution.h"

class X86_32_CallAbi : public HighCHostExecutionTest {
protected:
  /// Build \p Kernel as the semantic grid builds i386 code.
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                              Kernel, Entry, Names, {-3, 0, 1, 77});
  }
  /// Build \p Kernel with GCC.
  void expectGccRunsLikeSource(const std::string &Kernel,
                               const std::string &Entry,
                               std::initializer_list<const char *> Names) {
    // -m32 selects i386 only from an x86 host's driver.
    if (!neverd::test::hostRunsX86())
      GTEST_SKIP() << "gcc -m32 builds i386 only on an x86 host";
    expectHighCRunsLikeSource("gcc", {"-m32"}, Kernel, Entry, Names,
                              {-3, 0, 1, 77});
  }
};

// A wrapper that only forwards ECX and EDX takes them as its parameters,
// and its call passes them on.
TEST_F(X86_32_CallAbi, ForwarderPassesItsRegisterArgumentsThrough) {
  expectRunsLikeSource(R"C(
static unsigned g2(unsigned, unsigned) __attribute__((noinline));
static unsigned f2(unsigned, unsigned) __attribute__((noinline));
int fwd2(int a) {
  unsigned b = (unsigned)a;
  return (int)f2(b, b * 2654435761u + 1u);
}
static unsigned f2(unsigned x, unsigned y) { return g2(x, y); }
static unsigned g2(unsigned x, unsigned y) {
  unsigned h = x ^ 0x9e3779b9u;
  for (unsigned i = 0; i < 8; i++)
    h = h * 31u + ((y >> (i & 7)) & 0xffu) + i;
  return h;
}
)C",
                       "fwd2", {"fwd2", "f2", "g2"});
}

// Two register arguments and eight on the stack, pushed afresh on every
// iteration of a loop, whose stack pointer is a PHI at the loop head.
TEST_F(X86_32_CallAbi, StackArgumentsPushedInALoopReachTheCallee) {
  expectRunsLikeSource(R"C(
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
                       "manyarg", {"manyarg", "ma"});
}

// A forwarder of a GCC regparm function takes EAX, EDX and ECX in that
// order, and so does a tail jump to it.
TEST_F(X86_32_CallAbi, GccRegparmFunctionsTakeEaxEdxEcx) {
  expectGccRunsLikeSource(R"C(
static unsigned __attribute__((noinline)) h3(unsigned a, unsigned b,
                                             unsigned c) {
  unsigned r = a * 31u;
  for (unsigned i = 0; i < (c & 7u); i++)
    r = r * 33u + b + i;
  return r ^ c;
}
static unsigned __attribute__((noinline)) w3(unsigned a, unsigned b,
                                             unsigned c) {
  return h3(a, b, c) + 1u;
}
int rp(int a) {
  unsigned u = (unsigned)a;
  return (int)w3(u, u * 2654435761u, u + 5u);
}
)C",
                          "rp", {"rp", "w3", "h3"});
}

// GCC drops a local function's unused parameter (IPA-SRA), so the next one
// takes EAX, and passes a fourth argument on the stack after the registers.
TEST_F(X86_32_CallAbi, GccRegparmStackArgumentFollowsTheRegisters) {
  expectGccRunsLikeSource(R"C(
static unsigned __attribute__((noinline)) k2(unsigned a, unsigned b,
                                             unsigned c) {
  return b * 7u + c;
}
static unsigned __attribute__((noinline)) h4(unsigned a, unsigned b,
                                             unsigned c, unsigned d) {
  return ((a * 31u + b) ^ c) - d * 3u;
}
int use2(int a) {
  unsigned u = (unsigned)a;
  return (int)(k2(u, u + 1u, u * 3u) + h4(u, u ^ 5u, u + 9u, u * 7u));
}
)C",
                          "use2", {"use2", "k2", "h4"});
}

// GCC -O0 reloads a char from its frame slot with `movzbl` and pushes it for
// a prototyped import.  The C prints the pushed value in the call, and it
// must keep the zero extension: the int8_t slot converts by its sign, which
// passed 0xFFFFFF80 for 0x80.
TEST_F(X86_32_CallAbi, AForwardedZeroExtensionReachesAnImportsArgument) {
  HostFlags = {"-m32"};
  KernelOptimization = "-O0";
  expectHighCRunsLikeSource("gcc", {"-m32"}, R"C(
unsigned int htonl(unsigned int);
int lowbyte(int a) {
  char c = (char)a;
  return (int)htonl((unsigned char)c);
}
)C",
                            "lowbyte", {"lowbyte"},
                            {0, 0x41, 0x7F, 0x80, 0xC1, -1, 0x1FF});
}

// Clang -O0 with -mstackrealign, as the Android NDK builds i386 code,
// realigns every frame (`and esp, -16`) and stores each stack argument with
// `mov [esp+N]`.  No entry offset reaches below the alignment, so the stores
// of a call through a pointer were taken for pushes, the last one first:
// `pick(d, x * 3, x + 1, x, ...)`.
TEST_F(X86_32_CallAbi, ARealignedFramesStackArgumentsKeepTheirSlots) {
  HostFlags = {"-m32"};
  KernelOptimization = "-O0";
  expectHighCRunsLikeSource(
      {"-target", "i386-linux-gnu", "-march=pentium4", "-mstackrealign"},
      R"C(
typedef int (*fn4)(int, int, int, int);
static int mix(int a, int b, int c, int d) {
  return (int)((((unsigned)a * 31u + (unsigned)b) * 31u + (unsigned)c) * 31u +
               (unsigned)d);
}
static fn4 volatile pick = mix;
int realigned_call(int x) {
  char d = (char)x;
  return pick(x, x + 1, x * 3, (unsigned char)d);
}
)C",
      "realigned_call", {"realigned_call", "mix", "pick"},
      {0, 1, -3, 77, 0x80, 0x1C1});
}

// The System V i386 psABI aligns the stack to sixteen at a call, so a
// function enters at 12 mod 16, and Clang keeps a vector in a frame slot with
// `movdqa %xmm0, (%esp)` without realigning.  The C frame enters at the same
// residue: at 0 mod 16 the slot is misaligned, and the move's check traps.
TEST_F(X86_32_CallAbi, AVectorSlotIsAlignedAsTheCallerAlignsTheStack) {
  HostFlags = {"-m32", "-msse2"};
  expectHighCRunsLikeSource({"-target", "i386-linux-gnu", "-march=pentium4"},
                            R"C(
typedef unsigned v4u __attribute__((vector_size(16)));
int spill(int a) {
  v4u v = {(unsigned)a, (unsigned)a * 3u, (unsigned)a ^ 5u, (unsigned)a + 7u};
  v = v * v + (v4u){1u, 2u, 3u, 4u};
  return (int)v[(unsigned)a & 3u];
}
)C",
                            "spill", {"spill"}, {0, 1, -3, 77, 0x80, 0x1C1});
}

// A call through a function pointer takes its arguments on the stack, as
// many as its caller pushes, whatever ECX and EDX hold at the call: here a
// quotient in EDX and the first argument in ECX, as Clang leaves them in a
// regparm caller.  The host cannot run i386 function pointers, so the test
// counts the arguments of each call through the pointer.
// `mov ecx, [esp+ebp+0Ch]; call [ecx]`: the call reads the method through
// the object's table pointer, two loads under an address that adds a
// constant; the C must not take the inner sum for a table slot.
TEST_F(X86_32_CallAbi, ACallThroughAFieldsTableLoadsTwice) {
  HostFlags = {"-m32", "-march=pentium4"};
  expectRunsLikeSource(R"C(
struct obj;
typedef int (*method)(struct obj *, int);
struct table { method m; int tag; };
struct obj { const struct table *vt; int val; };
static int addm(struct obj *, int) __attribute__((noinline));
static int mulm(struct obj *, int) __attribute__((noinline));
static const struct table va = {addm, 10};
static const struct table vm = {mulm, 20};
int vcall(int a) {
  unsigned s = (unsigned)a | 1u;
  struct obj o[4];
  long acc = 0;
  for (int i = 0; i < 4; i++) {
    o[i].vt = (i & 1) ? &vm : &va;
    o[i].val = (int)(s + (unsigned)i);
  }
  for (int i = 0; i < 64; i++) {
    s = s * 1103515245u + 12345u;
    struct obj *q = &o[(s >> 5) & 3u];
    acc += q->vt->m(q, (int)(s >> 9)) + q->vt->tag;
    acc ^= acc >> 6;
  }
  return (int)(acc + o[0].val + o[3].val);
}
static int addm(struct obj *o, int x) { o->val += x; return o->val; }
static int mulm(struct obj *o, int x) { o->val *= (x | 1); return o->val; }
)C",
                       "vcall", {"vcall", "addm", "mulm", "va", "vm"});
}

TEST_F(X86_32_CallAbi, AFunctionPointerCallTakesItsStackArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("function-pointer-call.c");
  const auto Object = tmpFile("function-pointer-call.o");
  std::ofstream(Source) << R"C(
static int add3(int a, int b) { return a + 3 * b; }
static int sub2(int a, int b) { return a - 2 * b; }
__attribute__((noinline)) static int apply(int (*f)(int, int), int a, int b) {
  int q = a / 7, r = b % 5;
  return f(q, r) ^ f(r + 1, q - 1);
}
int pick(int x) { return apply((x & 1) ? add3 : sub2, x * 13, x >> 2); }
)C";
  const auto Compiled = exec(
      NEVERD_TEST_CLANG, {"-target", "i386-linux-gnu", "-march=pentium4",
                          "-fno-pic", "-O2", "-fno-asynchronous-unwind-tables",
                          "-c", Source.string(), "-o", Object.string()});
  ASSERT_EQ(Compiled.exitCode, 0) << Compiled.err;
  const auto Decompiled = decompileToHighC(Object);
  ASSERT_EQ(Decompiled.exitCode, 0) << Decompiled.err;
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  const size_t Body = C.find("apply(int32_t arg0");
  ASSERT_NE(Body, std::string::npos) << C;
  const std::string Apply = C.substr(Body, C.find("\n}", Body) - Body);
  // Each call casts the pointer to a function type, `(*)())`, then applies
  // it to an argument list: count the list's top-level commas.
  unsigned Calls = 0;
  for (size_t At = Apply.find("(*)())"); At != std::string::npos;
       At = Apply.find("(*)())", At + 1)) {
    size_t Open = Apply.find("))(", At + 6);
    ASSERT_NE(Open, std::string::npos) << Apply;
    Open += 2;
    unsigned Depth = 0, Arguments = 1;
    size_t I = Open;
    for (; I < Apply.size(); ++I) {
      if (Apply[I] == '(')
        ++Depth;
      else if (Apply[I] == ')' && --Depth == 0)
        break;
      else if (Apply[I] == ',' && Depth == 1)
        ++Arguments;
    }
    EXPECT_EQ(Arguments, 2u) << Apply.substr(Open, I - Open + 1) << "\n"
                             << Apply;
    ++Calls;
  }
  EXPECT_EQ(Calls, 2u) << Apply;
}

// A double passed on the stack fills two four-byte slots, and MULSD reads it
// from its home as one memory operand: the multiply takes both slots.
TEST_F(X86_32_CallAbi, AStackDoubleFeedsItsMultiplyWhole) {
  expectRunsLikeSource(R"C(
int fg(double, double, double, double, double, double)
    __attribute__((noinline));
int icfp(int x) {
  double v = (double)x;
  return fg(v, v + 1, v + 2, v + 3, v + 4, v + 5);
}
int fg(double a, double b, double c, double d, double e, double f) {
  return (int)(a * b + c * d + e * f);
}
)C",
                       "icfp", {"icfp", "fg"});
}

// Twelve variadic ints, all on the stack, which the callee's va_arg walk
// reads in place above its frame.  The callee takes them as trailing
// parameters, seeded where the walk reads them.
TEST_F(X86_32_CallAbi, AVariadicCalleeReadsItsStackArgumentsInPlace) {
  HostFlags = {"-m32", "-march=pentium4"};
  expectRunsLikeSource(R"C(
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
                       "vmany_e", {"vmany_e", "vmany"});
}

// Clang passes a static function's doubles in xmm0 and xmm1.  CVTSI2SD and
// MULSD write a register's low double and keep its upper half, which this
// function never set: the callee reads only the low double, so the call
// passes it alone and no unknown upper half reaches the C.
TEST_F(X86_32_CallAbi, ADoubleArgumentPassesOnlyItsLowLane) {
  HostFlags = {"-m32", "-march=pentium4"};
  expectRunsLikeSource(R"C(
static double h2(double, double) __attribute__((noinline));
int dd(int a) {
  double x = (double)a;
  double r = h2(x * 1.25, x * 0.5 + 3.0);
  return (int)r;
}
static double h2(double a, double b) { return a * b + a - b * 2.0; }
)C",
                       "dd", {"dd", "h2"});
}

// Clang folds the count of a static variadic function's only call into its
// body and reads four of the arguments with one 16-byte load from the
// stack.  The load reads four parameters, one per slot.
TEST_F(X86_32_CallAbi, AWideLoadOfStackArgumentsReadsEverySlot) {
  HostFlags = {"-m32", "-march=pentium4"};
  expectRunsLikeSource(R"C(
#include <stdarg.h>
static int mx(int n, ...) __attribute__((noinline));
int vmax_i(int a) {
  unsigned u = (unsigned)a;
  return mx(5, (int)u, (int)(u ^ 0x7fu), (int)(0 - u), (int)(u * 5u),
            (int)(u >> 1));
}
static int mx(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int m = -2147483647 - 1;
  for (int i = 0; i < n; i++) {
    int v = va_arg(ap, int);
    if (v > m)
      m = v;
  }
  va_end(ap);
  return m;
}
)C",
                       "vmax_i", {"vmax_i", "mx"});
}

// `pcmpeqd x, x` sets every bit of x whatever x held, so the function reads
// no vector register and takes only its stack argument.
TEST_F(X86_32_CallAbi, AVectorOfOnesReadsNoRegister) {
  expectRunsLikeSource(R"C(
unsigned ones_plus(unsigned a) {
  unsigned r;
  __asm__("pcmpeqd %%xmm0, %%xmm0\n\tmovd %%xmm0, %0" : "=r"(r) : : "xmm0");
  return r + a * 3u;
}
)C",
                       "ones_plus", {"ones_plus"});
}

// Code built for SSE2 computes a double in an XMM register and returns it
// on the x87 stack, as cdecl does (`movsd %xmm0, (%esp); fldl (%esp)`): the
// callee returns that value.
TEST_F(X86_32_CallAbi, AnSSEDoubleReturnsOnTheX87Stack) {
  // The C reads the constant through a 32-bit address, and its SSE
  // operations take XMM registers.
  HostFlags = {"-m32", "-msse2"};
  expectRunsLikeSource(R"C(
double d3(double a, double b, double c) __attribute__((noinline));
int fp3(int x) {
  double v = (double)x;
  return (int)d3(v, v + 1, v + 2);
}
double d3(double a, double b, double c) { return a + 2 * b + 3 * c; }
)C",
                       "fp3", {"fp3", "d3"});
}

// Sign-extending bytes in place, `punpcklwd %xmm1, %xmm4` takes its low
// half from XMM4 before the function wrote it, and `psrad $24` shifts that
// half out.  i386 passes no argument in XMM4 (System V uses XMM0-XMM2), so
// it is no parameter that would move the stack argument.
TEST_F(X86_32_CallAbi, AnXmmRegisterPastTheArgumentOnesIsNoParameter) {
  HostFlags = {"-m32", "-msse2"};
  expectRunsLikeSource(R"C(
int narrowarr(int a) {
  unsigned s = (unsigned)a, h = 0;
  for (int it = 0; it < 120; it++) {
    s = s * 1103515245u + 12345u;
    unsigned char b[24];
    signed char sb[24];
    unsigned short w[24];
    for (int i = 0; i < 24; i++) {
      s = s * 1103515245u + 12345u;
      b[i] = (unsigned char)(s >> 9);
      sb[i] = (signed char)(s >> 17);
      w[i] = (unsigned short)(s >> 3);
    }
    unsigned acc = 0;
    for (int i = 0; i < 24; i++)
      acc += (unsigned)b[i] + (unsigned)(int)sb[i] + (unsigned)w[i];
    h = h * 131u + acc;
    h ^= h >> 12;
  }
  return (int)h;
}
)C",
                       "narrowarr", {"narrowarr"});
}

// The same idiom on the high words, `punpckhwd %xmm1, %xmm2; psrad $16`,
// takes XMM2's incoming words, which can carry an argument, and shifts
// them out of every lane: a shift moves bytes, and reads none it drops.
TEST_F(X86_32_CallAbi, BytesAShiftDropsAreNotRead) {
  HostFlags = {"-m32", "-msse2"};
  expectRunsLikeSource(R"C(
int ssum16(int a) {
  short v[13];
  unsigned s = (unsigned)a | 1u;
  for (int i = 0; i < 13; i++) {
    v[i] = (short)(s >> 4);
    s = s * 1103515245u + 12345u;
  }
  long long acc = 0;
  for (int r = 0; r < 60; r++) {
    int sum = 0;
    for (int i = 0; i < 13; i++)
      sum += (int)v[i];
    acc = acc * 131 + sum;
    v[r % 13] ^= (short)r;
  }
  return (int)(unsigned)(acc ^ (acc >> 32));
}
)C",
                       "ssum16", {"ssum16"});
}
