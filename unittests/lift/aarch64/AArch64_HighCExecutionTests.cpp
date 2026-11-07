//===- AArch64_HighCExecutionTests.cpp - AArch64 HighC run on the host ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Each kernel is decompiled to HighC and run on the host against its own C.
// The kernels keep to integer code, which the host compiles.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

#include <fstream>
#include <iterator>

class AArch64_HighCExecution : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "aarch64-linux-gnu"}, Kernel, Entry,
                              Names, {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
  }
};

// A vector read at a run-time lane goes through the stack: `orr x8, x9, x8,
// lsl #2` over `mov x9, sp` selects the lane the load reads, so the `str q1,
// [sp]` before it is no write to an unread slot.
TEST_F(AArch64_HighCExecution,
       ALaneIndexedThroughTheFrameReadsTheStoredVector) {
  expectRunsLikeSource(R"C(
typedef int v4si __attribute__((vector_size(16)));
int lane(int a) {
  v4si v = {a, a + 1, a * 3, a ^ 5};
  return v[(a >> 4) & 3];
}
)C",
                       "lane", {"lane"});
}

// The writer's pointer arrives in x0, which it never writes, and its caller
// reads nothing it returns: it returns no value.
TEST_F(AArch64_HighCExecution, AWriterThroughItsPointerReturnsNoValue) {
  expectRunsLikeSource(R"C(
void upd4(long *s, int n) __attribute__((noinline));
int structptr(int x) {
  long s[4] = {x, x + 1, x + 2, x + 3};
  upd4(s, 12);
  return (int)(s[0] ^ s[1] ^ s[2] ^ s[3]);
}
void upd4(long *s, int n) {
  for (int i = 0; i < n; i++) {
    s[0] = s[0] * 3 + s[1];
    s[1] ^= s[0];
    s[2] += s[0];
    s[3] -= s[1];
  }
}
)C",
                       "structptr", {"structptr", "upd4"});
}

// EXT takes a register-width window of two concatenated vectors; the C
// builds it from the two pieces, since no C integer on AArch64 holds the
// pair.
TEST_F(AArch64_HighCExecution, AnExtWindowNeedsNoWiderInteger) {
  expectRunsLikeSource(R"C(
typedef unsigned char v16u8 __attribute__((vector_size(16)));
#if defined(__aarch64__)
#define VBARRIER(v) __asm__("" : "+w"(v))
#else
#define VBARRIER(v) (void)0
#endif
int window(int a) {
  v16u8 x, y;
  for (int i = 0; i < 16; i++) {
    x[i] = (unsigned char)(a + i * 5);
    y[i] = (unsigned char)(a * 3 - i);
  }
  VBARRIER(x);
  VBARRIER(y);
  v16u8 z = __builtin_shufflevector(x, y, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                    21, 22, 23, 24, 25, 26, 27);
  VBARRIER(z);
  return z[0] + z[5] * 7 + z[15] * 13 + z[3] * 3;
}
)C",
                       "window", {"window"});
}

// FMAX and FMIN are IEEE 754-2019 maximum and minimum: a NaN operand gives a
// NaN, and -0 orders below +0, unlike fmaxf and fminf.
TEST_F(AArch64_HighCExecution, FMaxAndFMinPropagateANaN) {
  // The C spells these with a builtin Apple's Clang can lack.
  if (!neverd::test::compilerAccepts(
          NEVERD_TEST_CLANG, "c",
          "float f(float a, float b) {\n"
          "  return __builtin_elementwise_minimum(a, b);\n"
          "}\n",
          {}))
    GTEST_SKIP() << "this Clang lacks __builtin_elementwise_minimum";
  expectRunsLikeSource(R"C(
int extremes(int a) {
  float nan = __builtin_nanf("");
  float x = (a & 1) ? nan : (float)a;
  float y = (a & 2) ? -0.0f : (float)(a - 5);
  float z = (a & 4) ? 0.0f : (float)(7 - a);
  float m, n;
#if defined(__aarch64__)
  __asm__("fmax %s0, %s1, %s2" : "=w"(m) : "w"(x), "w"(y));
  __asm__("fmin %s0, %s1, %s2" : "=w"(n) : "w"(y), "w"(z));
#else
  m = x != x ? x : y != y ? y : x > y ? x : x < y ? y
      : __builtin_signbit(x) ? y : x;
  n = y != y ? y : z != z ? z : y < z ? y : y > z ? z
      : __builtin_signbit(y) ? y : z;
#endif
  return (m != m) + 2 * __builtin_signbit(n) + 4 * (m == m ? (int)m : 0) +
         64 * (n == n ? (int)n : 0);
}
)C",
                       "extremes", {"extremes"});
}

// `fcsel ..., gt` after an `fcmp` selects when Z is clear and N equals V.
// V is the unordered test, `!(a == b || a < b || b < a)`, which prints as
// a conjunction: compared with N it keeps its parentheses.
TEST_F(AArch64_HighCExecution, AGreaterThanAfterFcmpGroupsTheUnorderedTest) {
  expectRunsLikeSource(R"C(
int minmax(int a) {
  float mn = 1e30f, mx = -1e30f;
  for (int i = 0; i < 200; i++) {
    float x = (float)((int)(a * (i * 7 + 1)) % 2003 - 1000);
    if (x < mn)
      mn = x;
    if (x > mx)
      mx = x;
  }
  return (int)(mx - mn);
}
)C",
                       "minmax", {"minmax"});
}

// Ten doubles: eight in v0-v7 and two on the stack.  The callee's lift also
// lists x0-x7, which it never reads, and the call passes a value for each;
// the call passes the ones the definition declares.
TEST_F(AArch64_HighCExecution, AStackDoubleCallPassesWhatTheCalleeDeclares) {
  expectHighCRunsLikeSource(
      {"-target", "aarch64-linux-gnu", "-ffp-contract=off"},
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

// Twelve variadic ints: x1-x7 and five on the stack, which the callee's
// va_arg walk reads in place above its frame.  The callee takes them as
// trailing parameters, seeded where the walk reads them.
TEST_F(AArch64_HighCExecution, AVariadicCalleeReadsItsStackArgumentsInPlace) {
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

// FCVTZS from a half register converts the half's value, which a float
// holds exactly: the C converts it through float with the same saturation.
TEST_F(AArch64_HighCExecution, AHalfConvertsToAnIntegerThroughFloat) {
  expectHighCRunsLikeSource(
      {"-target", "aarch64-linux-gnu", "-march=armv8.2-a+fp16"}, R"C(
int halfint(int a) {
  unsigned short u = (unsigned short)a;
  _Float16 h;
  __builtin_memcpy(&h, &u, sizeof h);
  return (int)h;
}
)C",
      "halfint", {"halfint"},
      {0, 0x0001, 0x3C00, 0x4300, 0xC100, 0x5640, 0x7BFF});
}

// A call through a table of functions passes the running value the loop
// keeps in w0: no write in the call's block sets it, but the loop's join
// does, so it is the argument the call recovery found.
TEST_F(AArch64_HighCExecution, AnIndirectCallPassesTheValueItsLoopKeeps) {
  KernelOptimization = "-O1";
  expectRunsLikeSource(R"C(
typedef unsigned (*vf_t)(unsigned);
static unsigned h0(unsigned);
static unsigned h1(unsigned);
static unsigned h2(unsigned);
struct obj {
  vf_t f;
  unsigned k;
};
int vt(int a) {
  struct obj v[3] = {{h0, 3u}, {h1, 5u}, {h2, 7u}};
  unsigned acc = (unsigned)a;
  for (int i = 0; i < 66; i++) {
    struct obj *o = &v[(acc >> 2) % 3u];
    acc = o->f(acc) + o->k * (unsigned)i;
  }
  return (int)acc;
}
static unsigned h0(unsigned x) { return (x * 2654435761u) ^ (x >> 15); }
static unsigned h1(unsigned x) { return x + 0x85EBCA6Bu; }
static unsigned h2(unsigned x) { return (x << 13) | (x >> 19); }
)C",
                       "vt", {"vt", "h0", "h1", "h2", "obj", "vf_t"});
}

// A table of string pointers becomes a relative table whose address the
// code keeps in a register: no add-back names the table, but the loader
// relocates every entry as a displacement to its string, so the table's
// object reaches the strings.
TEST_F(AArch64_HighCExecution,
       ARelativeTableKnownByItsAddressReachesItsStrings) {
  expectRunsLikeSource(R"C(
static const char *const W[6] = {"alphaA", "bravoB", "charlC",
                                 "deltaD", "echoEe", "foxtrF"};
int offptr(int a) {
  unsigned s = (unsigned)a, h = 0;
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    unsigned j = (s >> 5) % 6u;
    const char *p = W[j] + ((s >> 9) % 6u);
    h = h * 131u + (unsigned)(unsigned char)*p;
    h ^= h >> 11;
  }
  return (int)h;
}
)C",
                       "offptr", {"offptr", "W"});
}

// The vectorized sum keeps a byte in w0, then `fmov x0, d0` and the
// remainder loop's `madd x0` write the whole register.  Where the paths join,
// a phi of the w0 view still holds the byte; the result is the x0 phi.
TEST_F(AArch64_HighCExecution, AJoinReturnsTheWholeRegisterNotAStaleView) {
  expectRunsLikeSource(R"C(
long runs(long a) __attribute__((noinline));
int rle(int a) {
  long s = runs(a);
  return (int)(s ^ (s >> 32));
}
long runs(long a) {
  unsigned char in[48], val[48], run[48];
  for (int i = 0; i < 48; i++)
    in[i] = (unsigned char)(((unsigned)a + i / 3 * 5) & 7);
  int n = 0, i = 0;
  while (i < 48) {
    unsigned char c = in[i];
    int cnt = 0;
    while (i < 48 && in[i] == c && cnt < 255) {
      cnt++;
      i++;
    }
    val[n] = c;
    run[n] = (unsigned char)cnt;
    n++;
  }
  long s = n * 10000;
  for (int k = 0; k < n; k++)
    s += (long)val[k] * 256 + run[k] * (k + 1);
  return s;
}
)C",
                       "rle", {"rle", "runs"});
}

// `mov w13, #0xc0000001` is the ORR alias, whose bitmask immediate Capstone
// gives sign-extended to 64 bits.  The W register receives its low 32, which
// the unsigned compare with x reads; as 0xFFFFFFFFC0000001 the compare was
// always true.
TEST_F(AArch64_HighCExecution, AMovedBitmaskImmediateIsItsLow32Bits) {
  expectRunsLikeSource(R"C(
int boolsc(int a) {
  unsigned w = (unsigned)a + 9u;
  long long acc = 0;
  for (int i = 0; i < 80; i++) {
    w = w * 1664525u + 1013904223u;
    int x = (int)w;
    if ((x > 0 && (x & 3) == 0) || ((unsigned)x > 0xC0000000u && (x >> 5) != 0))
      acc += (long long)x * 5;
    else if (x < 0 && ((x ^ i) & 1))
      acc -= x;
    else
      acc ^= (long long)x << 3;
  }
  return (int)(acc ^ (acc >> 32));
}
)C",
                       "boolsc", {"boolsc"});
}

// LDADD adds to m and returns its old value; the load after it reads the
// sum.  Folded into one expression, `*(m) + 31 * __atomic_fetch_add(...)`
// leaves the load and the add unsequenced, so the C may read m first.
TEST_F(AArch64_HighCExecution, AnAtomicAddStaysBeforeTheLoadAfterIt) {
  expectHighCRunsLikeSource(
      {"-target", "aarch64-linux-gnu", "-march=armv8.1-a"},
      R"C(
int ldadd(int a) {
  long m = 10;
  long o = __atomic_fetch_add(&m, (long)a, __ATOMIC_RELAXED);
  return (int)(o * 31 + m);
}
)C",
      "ldadd", {"ldadd"}, {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
}

// A table of pointers to arrays becomes a relative table whose entries point
// back to the arrays before it.  The code keeps the table's address in a
// register it indexes and adds each entry back to, so the table's object
// spans from the first array through the table.
TEST_F(AArch64_HighCExecution, ARelativeTableHeldInARegisterReachesItsArrays) {
  expectRunsLikeSource(R"C(
int ptab(int a) {
  static const int A[8] = {11, 22, 33, 44, 55, 66, 77, 88};
  static const int B[8] = {2, 3, 5, 7, 11, 13, 17, 19};
  static const int C[8] = {101, 202, 303, 404, 505, 606, 707, 808};
  static const int *const tabs[3] = {A, B, C};
  unsigned acc = (unsigned)a;
  for (int i = 0; i < 96; i++) {
    const int *tp = tabs[acc % 3u];
    acc = acc * 131u + (unsigned)tp[(acc >> 2) & 7u] + (unsigned)i;
  }
  return (int)acc;
}
)C",
                       "ptab", {"ptab"});
}

// SCVTF converts v as a signed 64-bit value, but the C of `v` is unsigned
// arithmetic: `(float)(0x123456789 ^ ...)` converted an unsigned value.  A
// signed conversion now reads its operand as a signed integer of its width.
TEST_F(AArch64_HighCExecution, ASignedConversionReadsItsOperandSigned) {
  expectRunsLikeSource(R"C(
int i64f(int a) {
  unsigned long long acc = 1;
  unsigned s = (unsigned)a | 1u;
  for (int i = 0; i < 100; i++) {
    s = s * 1103515245u + 12345u;
    long long v = ((long long)(int)s << 9) ^ 0x123456789LL;
    unsigned long long u = ((unsigned long long)s << 32) | (s ^ 0x55u);
    float f = (float)v, g = (float)u;
    long long bv = (long long)(f * 0.5f);
    unsigned long long bu = (unsigned long long)(g * 0.25f);
    acc = acc * 131 + (unsigned long long)bv + bu;
    acc ^= acc >> 29;
  }
  return (int)(acc ^ (acc >> 32));
}
)C",
                       "i64f", {"i64f"});
}

// `subs w8, w8, #8; b.lt` tests N != V of one subtraction: the loop runs
// while i < 8.  Spelled as the sign of i - 8 against __builtin_sub_overflow,
// such a condition crashed Clang 23's loop passes (c_crc8); it is the signed
// compare.
TEST_F(AArch64_HighCExecution, FlagsOfASubtractionCompareSigned) {
  KernelOptimization = "-O0";
  expectRunsLikeSource(R"C(
int crc8(int a) {
  unsigned char crc = 0, b = (unsigned char)a;
  for (int i = 0; i < 8; i++) {
    if ((crc ^ b) & 0x80)
      crc = (unsigned char)((crc << 1) ^ 7);
    else
      crc = (unsigned char)(crc << 1);
    b = (unsigned char)(b << 1);
  }
  return crc;
}
)C",
                       "crc8", {"crc8"});
  std::ifstream Input(tmpFile("decompiled_high.c"));
  const std::string C((std::istreambuf_iterator<char>(Input)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(C.find("__builtin_sub_overflow"), std::string::npos) << C;
}

// `movi d0, #0` zeroes the whole D register.  Lifted as a 4-byte constant
// copied into it, the accumulator's upper half was an unknown value, and
// the C trapped.
TEST_F(AArch64_HighCExecution, AScalarMoviWritesTheWholeRegister) {
  // The C fuses as the kernel does; the host runs its fma inline.
#if defined(__i386__) || defined(__x86_64__)
  HostFlags = {"-mfma"};
#endif
  expectHighCRunsLikeSource({"-target", "aarch64-linux-gnu"},
                            R"C(
int dotp(int a) {
  unsigned s = (unsigned)a;
  double acc = 0;
  for (int it = 0; it < 120; it++) {
    s = s * 1103515245u + 12345u;
    double u[6], v[6], du = 0, dv = 0, dot = 0;
    for (int i = 0; i < 6; i++) {
      s = s * 1103515245u + 12345u;
      u[i] = ((double)((s >> 9) & 0xff)) - 128.0;
      v[i] = ((double)((s >> 17) & 0xff)) - 128.0;
      du += u[i] * u[i];
      dv += v[i] * v[i];
    }
    for (int i = 0; i < 6; i++)
      dot += u[i] * v[i];
    double den = du * dv + 1.0;
    acc += dot * dot / den;
  }
  return (int)(long long)(acc * 64.0);
}
)C",
                            "dotp", {"dotp"},
                            {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
}

// `ldr x18, [x11, xzr, lsl #3]` reads the first label of a computed goto's
// table: the zero register is the index, and it reads as zero.
TEST_F(AArch64_HighCExecution, AZeroRegisterIndexReadsTheFirstEntry) {
  expectRunsLikeSource(R"C(
int lex(int a) {
  static const void *cls[4] = {&&DIG, &&ALP, &&SPC, &&OTH};
  unsigned acc = (unsigned)a, tok = 0;
  for (int i = 0; i < 96; i++) {
    unsigned c = ((acc >> (i & 15)) ^ (unsigned)(i * 131)) & 0xFF;
    int k = c < 64 ? 0 : c < 128 ? 1 : c < 192 ? 2 : 3;
    goto *cls[k];
  DIG:
    tok = tok * 10u + (c & 7u);
    acc += c;
    continue;
  ALP:
    tok = (tok << 4) ^ (c | 0x20u);
    acc ^= c << 1;
    continue;
  SPC:
    tok += 0x9E37u;
    acc = (acc << 1) | (acc >> 31);
    continue;
  OTH:
    tok ^= c * 2654435761u;
    acc -= c;
    continue;
  }
  return (int)(tok ^ acc);
}
)C",
                       "lex", {"lex"});
}
