//===- X86_64_ImageTableTests.cpp - Image tables in HighC -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A table in the image prints as the object the output defines, and an index
// into it keeps the value the machine computed.  Each kernel is decompiled to
// HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

class X86_64_ImageTable : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu"}, Kernel, Entry,
                              Names, {-3, 0, 1, 77});
  }
};

// A call through a table of functions reads the table the output defines:
// the table's address added to an index is no offset into an object.
TEST_F(X86_64_ImageTable, ACallThroughAFunctionTableReadsTheTable) {
  expectRunsLikeSource(R"C(
typedef unsigned (*vf_t)(unsigned);
static unsigned h0(unsigned x) { return (x * 2654435761u) ^ (x >> 15); }
static unsigned h1(unsigned x) { return x + 0x85EBCA6Bu; }
static unsigned h2(unsigned x) { return (x << 13) | (x >> 19); }
struct obj { vf_t f; unsigned k; };
struct obj table[3] = {{h0, 3u}, {h1, 5u}, {h2, 7u}};
int dispatch(int a) {
  unsigned acc = (unsigned)a;
  for (int i = 0; i < 66; i++) {
    const struct obj *o = &table[(acc >> 2) % 3u];
    acc = o->f(acc) + o->k * (unsigned)i;
  }
  return (int)acc;
}
)C",
                       "dispatch", {"dispatch", "table", "h0", "h1", "h2"});
}

// Clang turns the bit loop into a table lookup per byte.  The first index is
// the hash's low byte, and each later one a byte XOR whose operand the table
// yields: neither is the wider value C would compute from what is under the
// extension.
TEST_F(X86_64_ImageTable, ATableIndexKeepsItsWidth) {
  expectRunsLikeSource(R"C(
int crc8(int a) {
  unsigned x = (unsigned)a * 2654435761u;
  unsigned char crc = 0;
  for (int i = 0; i < 4; i++) {
    crc ^= (unsigned char)(x >> (i * 8));
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x07)
                         : (unsigned char)(crc << 1);
  }
  return crc;
}
)C",
                       "crc8", {"crc8"});
}

// A pointer walking a static array is compared with the address one past the
// array's end, where no object starts: that is the array's address plus its
// size, not the number the image gives it.
TEST_F(X86_64_ImageTable, APointerComparesWithTheEndOfItsArray) {
  expectRunsLikeSource(R"C(
static int A[16];
int ptrcmp(int a) {
  unsigned s = (unsigned)a | 1u;
  int h = 0;
  for (int i = 0; i < 16; i++) {
    s = s * 1103515245u + 12345u;
    A[i] = (int)(s >> 9);
  }
  int *p = A + ((s >> 4) & 15u);
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    *p += (int)(s >> 11);
    h = h * 131 + *p;
    p++;
    if (p >= A + 16)
      p = A;
  }
  return h + A[0] + A[15];
}
)C",
                       "ptrcmp", {"ptrcmp", "A"});
}

// A table of functions the code reaches only through a selected base
// (`T = c ? FA : FB; T[i]`) holds the functions' addresses, not their
// offsets in the object's text.
TEST_F(X86_64_ImageTable, ASelectedTableOfFunctionsHoldsTheirAddresses) {
  expectRunsLikeSource(
      R"C(
static int a0(int x) { return x + 11; }
static int a1(int x) { return x * 5; }
static int a2(int x) { return x ^ 0x3c; }
static int a3(int x) { return (x << 2) + 3; }
static int b0(int x) { return x - 7; }
static int b1(int x) { return x * 9 + 2; }
static int b2(int x) { return (x >> 2) ^ x; }
static int b3(int x) { return x * x - 1; }
static int (*const FA[4])(int) = {a0, a1, a2, a3};
static int (*const FB[4])(int) = {b0, b1, b2, b3};
int fnsel(int a) {
  unsigned s = (unsigned)a, h = 0;
  for (int i = 0; i < 200; i++) {
    s = s * 1103515245u + 12345u;
    int (*const *T)(int) = ((s >> 4) & 1u) ? FA : FB;
    int (*f)(int) = T[(s >> 7) & 3u];
    h = h * 131u + (unsigned)f((int)(s >> 3));
    h ^= h >> 10;
  }
  return (int)h;
}
)C",
      "fnsel",
      {"fnsel", "FA", "FB", "a0", "a1", "a2", "a3", "b0", "b1", "b2", "b3"});
}

// Clang turns a table of pointers to arrays into a relative table, the
// offsets of the arrays from the table's own start: the table the output
// declares spans the arrays, which the code reaches only through it.
TEST_F(X86_64_ImageTable, ARelativeTableSpansTheObjectsItReaches) {
  KernelOptimization = "-O1";
  expectRunsLikeSource(R"C(
static const unsigned B0[3] = {11u, 22u, 33u};
static const unsigned B1[3] = {101u, 202u, 303u};
static const unsigned B2[3] = {1001u, 2002u, 3003u};
static const unsigned *const QS[3] = {B0, B1, B2};
int pptr1(int a) {
  unsigned h = (unsigned)a, acc = 0;
  for (int i = 0; i < 48; i++) {
    h = h * 1103515245u + 12345u;
    unsigned k = (h >> 4) % 3u, j = (h >> 7) % 3u;
    acc = acc * 131u + QS[k][j] + (unsigned)i;
  }
  return (int)acc;
}
)C",
                       "pptr1", {"pptr1", "QS", "QS_rel", "B0", "B1", "B2"});
}

// A 256-bit constant vector, here the counts of a variable shift, keeps its
// value: the C spells it as its 64-bit pieces shifted into place.
TEST_F(X86_64_ImageTable, AWideConstantVectorKeepsItsValue) {
  HostFlags = {"-mavx2"};
  expectHighCRunsLikeSource({"-target", "x86_64-linux-gnu", "-mavx2"}, R"C(
typedef int v8si __attribute__((vector_size(32)));
int shifts(int a) {
  v8si v = {a, a ^ 0x55, a * 3, a + 7, a, a >> 1, a * 5, a - 9};
  v8si s = {0, 2, 4, 6, 8, 10, 12, 14};
  v8si r = v >> s;
  int t = 0;
  for (int i = 0; i < 8; i++)
    t = t * 31 + (r[i] & 1023);
  return t;
}
)C",
                            "shifts", {"shifts"}, {-3, 0, 1, 77, 0x12345});
}

// At -O0 a local label table is copied from its read-only initializer, an
// object no symbol names: the copy reads the object the output declares,
// not the address the image gives it. Without PIC the initializer lies in
// .rodata; with PIC in .data.rel.ro, which the loader maps read-only once
// relocated, as a linker does.
TEST_F(X86_64_ImageTable, AnUnnamedInitializerIsDeclared) {
  KernelOptimization = "-O0";
  for (const bool PIC : {false, true}) {
    SCOPED_TRACE(PIC ? "PIC" : "-fno-pic");
    std::vector<std::string> Flags = {"-target", "x86_64-linux-gnu"};
    if (!PIC)
      Flags.push_back("-fno-pic");
    ASSERT_NO_FATAL_FAILURE(expectHighCRunsLikeSource(Flags,
                                                      R"C(
int loc8(int a) {
  const void *tab[8] = {&&L0, &&L1, &&L2, &&L3, &&L4, &&L5, &&L6, &&L7};
  unsigned acc = (unsigned)a;
  int pc = 0;
  for (int i = 0; i < 80; i++) {
    goto *tab[pc & 7];
  L0:
    acc += 0x9E3779B9u;
    pc = (int)(acc & 7);
    continue;
  L1:
    acc ^= (acc << 13) | (acc >> 19);
    pc = (int)((acc >> 7) & 7);
    continue;
  L2:
    acc *= 2654435761u;
    pc = (int)((acc >> 11) & 7);
    continue;
  L3:
    acc -= (acc >> 5);
    pc = (int)((acc >> 3) & 7);
    continue;
  L4:
    acc += 0x85EBCA6Bu;
    pc = (int)((acc >> 2) & 7);
    continue;
  L5:
    acc ^= acc >> 15;
    pc = (int)((acc >> 1) & 7);
    continue;
  L6:
    acc = (acc << 5) | (acc >> 27);
    pc = (int)((acc >> 9) & 7);
    continue;
  L7:
    acc += acc << 1;
    pc = (int)((acc >> 4) & 7);
    continue;
  }
  return (int)acc;
}
)C",
                                                      "loc8", {"loc8"},
                                                      {-3, 0, 1, 77}));
  }
}

// `SA[i - 1]` takes one from a sum of SA's address and an index: SA is the
// table the access reads, declared whole, not the number of its address.
TEST_F(X86_64_ImageTable, AnIndexBiasedDownReadsTheString) {
  expectRunsLikeSource(R"C(
static const char SA[] = "kitten_sequence_alpha";
static const char SB[] = "sitting_sequins_alfa";
int editdist(int a) {
  unsigned s = (unsigned)a, h = 0;
  const int LA = (int)(sizeof(SA) - 1), LB = (int)(sizeof(SB) - 1);
  for (int it = 0; it < 8; it++) {
    s = s * 1103515245u + 12345u;
    unsigned off = (s >> 7) & 3u, row[24];
    for (int j = 0; j <= LB; j++)
      row[j] = (unsigned)j;
    for (int i = 1; i <= LA; i++) {
      unsigned prev = row[0];
      row[0] = (unsigned)i;
      for (int j = 1; j <= LB; j++) {
        unsigned cur = row[j];
        unsigned cost = ((SA[i - 1] + off) == (unsigned)SB[j - 1]) ? 0u : 1u;
        unsigned m = prev + cost, d = row[j - 1] + 1u;
        if (d < m)
          m = d;
        d = row[j] + 1u;
        if (d < m)
          m = d;
        row[j] = m;
        prev = cur;
      }
    }
    h = h * 131u + row[LB] + off;
  }
  return (int)h;
}
)C",
                       "editdist", {"editdist", "SA", "SB"});
}

// A table of string pointers becomes a relative table: each entry the
// offset of its string, in another section of the object, from the table.
// The code adds the entry back to the table's address, so the table's
// object reaches the strings.
TEST_F(X86_64_ImageTable, ARelativeTableReachesItsStrings) {
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

// A switch that picks a string becomes clang's lookup table: a relative
// table no symbol sizes, whose strings the object's layout puts before it.
// The code adds an entry back to the table in a variable it then reads
// through, so one object spans from the first string through the table.
// The object's constant pool, its writable G and its strings lie in that order
// within a few bytes.  G is no part of the constants around it, so both
// routes read G itself.
TEST_F(X86_64_ImageTable, AnObjectBetweenConstantSectionsIsItsOwn) {
  for (const bool Route : {false, true}) {
    SCOPED_TRACE(Route ? "LLVM C" : "HighC");
    LLVMC = Route;
    ASSERT_NO_FATAL_FAILURE(expectRunsLikeSource(R"C(
volatile double G = 2.5;
int gp(double x, int a) __attribute__((noinline));
int gp(double x, int a) {
  const char *s = (a & 1) ? "odd" : "even";
  return (int)(x * 4 + a * 0.5 + 0.25 * s[a & 3]);
}
int pf(int a) __attribute__((noinline));
int pf(int a) { return gp(G, a); }
int entry(int a) { return pf(a) + pf(a + 1) * 3; }
)C",
                                                 "entry",
                                                 {"entry", "gp", "pf", "G"}));
  }
}

TEST_F(X86_64_ImageTable, ASwitchTableReachesTheStringsBeforeIt) {
  expectRunsLikeSource(R"C(
int strsw(int a) {
  unsigned s = (unsigned)a, acc = 0;
  for (int i = 0; i < 300; i++) {
    unsigned c = (s >> 3) % 6u;
    const char *w;
    switch (c) {
    case 0: w = "alpha"; break;
    case 1: w = "bravo"; break;
    case 2: w = "charlie"; break;
    case 3: w = "delta"; break;
    case 4: w = "echo"; break;
    default: w = "fox";
    }
    acc = acc * 131u + (unsigned)(unsigned char)w[0] +
          ((unsigned)(unsigned char)w[1] << 8) + c;
    s = s * 1103515245u + 12345u;
  }
  return (int)acc;
}
)C",
                       "strsw", {"strsw"});
}
