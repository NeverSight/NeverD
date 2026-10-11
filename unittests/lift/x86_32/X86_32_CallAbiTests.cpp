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

// A call through a function pointer takes its arguments on the stack, as
// many as its caller pushes, whatever ECX and EDX hold at the call: here a
// quotient in EDX and the first argument in ECX, as Clang leaves them in a
// regparm caller.  The host cannot run i386 function pointers, so the test
// counts the arguments of each call through the pointer.
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
