//===- AArch64_StructReturnTests.cpp - Multi-register returns in HighC ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A small struct returns across several registers: a homogeneous float
// aggregate in v0..v3, any other one of up to sixteen bytes in x0:x1.  The
// callee returns the record of every field, and the caller reads each one.
// Each kernel is decompiled to HighC and run on the host against its own C.
//
//===----------------------------------------------------------------------===//

#include "HighCHostExecution.h"

#include <fstream>

class AArch64_StructReturn : public HighCHostExecutionTest {
protected:
  void expectRunsLikeSource(const std::string &Kernel, const std::string &Entry,
                            std::initializer_list<const char *> Names) {
    expectHighCRunsLikeSource({"-target", "aarch64-linux-gnu"}, Kernel, Entry,
                              Names, {0, 1, -1, 0x15, 0x27, 0x3F, 0x12345678});
  }
};

// Two doubles return in d0 and d1; the caller reads both.
TEST_F(AArch64_StructReturn, TwoDoublesReturnBoth) {
  expectRunsLikeSource(R"C(
typedef struct { double a, b; } dd;
dd mkdd(int x) __attribute__((noinline));
int retdd(int x) {
  dd r = mkdd(x);
  return (int)r.a ^ ((int)r.b * 9);
}
dd mkdd(int x) {
  dd r;
  r.a = (double)(x >> 2) + 0.25;
  r.b = (double)(x >> 4) - 1.75;
  return r;
}
)C",
                       "retdd", {"retdd", "mkdd", "dd"});
}

// Three floats are twelve bytes in s0..s2, which no integer bit casts from.
TEST_F(AArch64_StructReturn, ThreeFloatsReturnAll) {
  expectRunsLikeSource(R"C(
typedef struct { float a, b, c; } f3;
f3 mkf3(int x) __attribute__((noinline));
int retf3(int x) {
  f3 r = mkf3(x);
  return (int)r.a ^ ((int)r.b * 9) ^ ((int)r.c * 5);
}
f3 mkf3(int x) {
  f3 r;
  r.a = (float)(x >> 2) + 0.25f;
  r.b = (float)(x >> 4) - 1.75f;
  r.c = (float)x * 0.5f;
  return r;
}
)C",
                       "retf3", {"retf3", "mkf3", "f3"});
}

TEST_F(AArch64_StructReturn, TailCallForwardsAPreservedFirstDouble) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("tail-hfa.s");
  const auto Object = tmpFile("tail-hfa.o");
  std::ofstream(Source) << R"asm(
.text
.globl make_hfa
.type make_hfa,%function
make_hfa:
fadd d1,d0,d0
fadd d2,d1,d0
ret
.size make_hfa,.-make_hfa
.globl tail_hfa
.type tail_hfa,%function
tail_hfa:
b make_hfa
.size tail_hfa,.-tail_hfa
.globl sum_hfa
.type sum_hfa,%function
sum_hfa:
stp x29,x30,[sp,#-16]!
bl tail_hfa
fadd d0,d0,d1
fadd d0,d0,d2
ldp x29,x30,[sp],#16
ret
.size sum_hfa,.-sum_hfa
)asm";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  const auto IR = liftToLLVMIRUnopt(Object);
  ASSERT_EQ(IR.exitCode, 0) << IR.err;
  for (const char *Name : {"make_hfa", "tail_hfa"}) {
    const auto Signature =
        std::string("{ double, double, double } @") + Name + "(";
    EXPECT_NE(IR.out.find("define dso_local " + Signature), std::string::npos)
        << IR.out;
    EXPECT_NE(IR.out.find("call " + Signature), std::string::npos) << IR.out;
  }
}

// A double and an int are no float aggregate: they return in x0:x1, the
// double's bits in x0.
TEST_F(AArch64_StructReturn, ADoubleAndAnIntReturnInTheIntegerPair) {
  expectRunsLikeSource(R"C(
typedef struct { double d; int i; } di;
di mkdi(int x) __attribute__((noinline));
int retdi(int x) {
  di r = mkdi(x);
  return (int)r.d ^ (r.i * 9);
}
di mkdi(int x) {
  di r;
  r.d = (double)(x >> 2) + 0.25;
  r.i = x * 3 + 1;
  return r;
}
)C",
                       "retdi", {"retdi", "mkdi", "di"});
}

// A vector returns whole in v0, not as an aggregate of its lanes.
TEST_F(AArch64_StructReturn, AVectorReturnsAllItsLanes) {
  expectRunsLikeSource(R"C(
typedef unsigned v4su __attribute__((vector_size(16)));
v4su mk4(unsigned x) __attribute__((noinline));
int use4(int x) {
  v4su v = mk4((unsigned)x);
  return (int)(v[0] ^ v[3] * 3u ^ v[1] << 2);
}
v4su mk4(unsigned x) {
  v4su v = {x, x + 1u, x, x * 7u};
  return v + v;
}
)C",
                       "use4", {"use4", "mk4", "v4su"});
}


// A leaf can preserve the default GP result without forfeiting a floating
// record. Only real reads of both result fields warrant the aggregate.
TEST_F(AArch64_StructReturn, BareReturnRequiresTwoObservedFPFields) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target compilation requires Clang";
  const auto Source = tmpFile("hfa-passthrough.s");
  const auto Object = tmpFile("hfa-passthrough.o");
  std::ofstream(Source) << R"asm(
.text
.macro leaf name
.p2align 2
.globl \name
.type \name,%function
\name:
ret
.size \name,.-\name
.endm
leaf identity_pair
leaf identity_scalar
.macro caller name, target, extra
.p2align 2
.globl \name
.type \name,%function
\name:
stp x29,x30,[sp,#-16]!
mov x29,sp
bl \target
.if \extra
fadd d0,d0,d1
.else
fadd d0,d0,d0
.endif
ldp x29,x30,[sp],#16
ret
.size \name,.-\name
.endm
caller use_pair,identity_pair,1
caller use_scalar,identity_scalar,0
leaf identity_only_d1
.p2align 2
.globl use_only_d1
.type use_only_d1,%function
use_only_d1:
stp x29,x30,[sp,#-16]!
mov x29,sp
bl identity_only_d1
fmov d0,d1
ldp x29,x30,[sp],#16
ret
.size use_only_d1,.-use_only_d1
leaf identity_redefined_d1
.p2align 2
.globl use_redefined_d1
.type use_redefined_d1,%function
use_redefined_d1:
stp x29,x30,[sp,#-16]!
mov x29,sp
bl identity_redefined_d1
fmov d1,#16.0
fadd d0,d0,d1
ldp x29,x30,[sp],#16
ret
.size use_redefined_d1,.-use_redefined_d1
)asm";
  const auto Assembled =
      exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_EQ(Assembled.exitCode, 0) << Assembled.err;
  const auto IR = liftToLLVMIRUnopt(Object);
  ASSERT_EQ(IR.exitCode, 0) << IR.err;
  const auto Pair = IR.out.find(
      "define dso_local { double, double } @identity_pair(double %arg0, "
      "double %arg1)");
  ASSERT_NE(Pair, std::string::npos) << IR.out;
  const auto End = IR.out.find("\n}", Pair);
  ASSERT_NE(End, std::string::npos) << IR.out;
  const auto Body = IR.out.substr(Pair, End - Pair);
  EXPECT_NE(Body.find("ret { double, double }"), std::string::npos) << Body;
  EXPECT_NE(IR.out.find("call { double, double } @identity_pair(double "),
            std::string::npos)
      << IR.out;

  // Observing only D0, only D1, or a freshly written D1 must not infer both
  // arguments as an HFA. Require a definition so dropping the negative
  // function entirely cannot satisfy the test.
  for (const char *Control : {"identity_scalar", "identity_only_d1",
                              "identity_redefined_d1"}) {
    SCOPED_TRACE(Control);
    const auto Name = std::string(" @") + Control + "(";
    const auto Definition = IR.out.find(Name);
    ASSERT_NE(Definition, std::string::npos) << IR.out;
    const auto Line = IR.out.rfind('\n', Definition);
    ASSERT_NE(Line, std::string::npos) << IR.out;
    const auto Header = IR.out.substr(Line + 1, Definition - Line - 1);
    ASSERT_TRUE(Header.starts_with("define ")) << Header;
    EXPECT_EQ(Header.find('{'), std::string::npos) << Header;
  }
}
