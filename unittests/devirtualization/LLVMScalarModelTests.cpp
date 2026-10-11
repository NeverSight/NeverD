//===- LLVMScalarModelTests.cpp - Scalar interface and arithmetic oracles -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarEquivalenceTest.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace neverd::analysis::scalar_test {
TEST(LLVMScalarModel, IntegerProofDoesNotDependOnNativeArchitecture) {
  Source Sum("define i32 @f(i32 noundef %x, i32 noundef %y) {"
             " %r = add i32 %x, %y\nret i32 %r }");
  ASSERT_TRUE(Sum.Module);
  for (const char *Triple : {"x86_64-linux-gnu", "aarch64-linux-gnu",
                             "aarch64_be-linux-gnu", "armv7-linux-gnueabihf"}) {
    SCOPED_TRACE(Triple);
    Source Encoded(std::string("target triple = \"") + Triple +
                   "\"\n"
                   "define i32 @f(i32 noundef %x, i32 noundef %y) {"
                   " %a = sub i32 %x, 42\n %b = add i32 %y, 42\n"
                   " %r = add i32 %a, %b\n"
                   " ret i32 %r }");
    ASSERT_TRUE(Encoded.Module);
    auto Before = Encoded.text();
    auto Result =
        checkLLVMScalarEquivalence(Encoded.function(), Sum.function());
    EXPECT_EQ(Result.Status, Status::Proved) << Result.Diagnostic;
    EXPECT_EQ(Encoded.text(), Before);
  }
}

TEST(LLVMScalarModel, IntegerInputsAndResultsPreserveDeclaredWidths) {
  for (unsigned Bits : {1U, 8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const std::string T = "i" + std::to_string(Bits);
    Source S("define " + T + " @f(" + T + " noundef %x, " + T +
             " noundef %y) { %r = xor " + T + " %x, %y\nret " + T + " %r }");
    ASSERT_TRUE(S.Module);
    auto M = modelLLVMScalarFunction(S.function());
    ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
    EXPECT_EQ(M->ResultBits, Bits);
    ASSERT_EQ(M->Arguments.size(), 2U);
    EXPECT_EQ(M->Arguments[0].Bits, Bits);
    EXPECT_EQ(M->Arguments[0].Storage.Size, (Bits + 7) / 8);
    auto A = llvm::APInt::getAllOnes(Bits);
    auto B = llvm::APInt(64, 0x95).zextOrTrunc(Bits);
    auto R = evaluate(*M, {A, B});
    ASSERT_TRUE(R);
    EXPECT_EQ(*R, (A ^ B).zext((Bits + 7) / 8 * 8));
    EXPECT_EQ(checkLLVMScalarEquivalence(S.function(), S.function()).Status,
              Status::Proved);
  }
}

TEST(LLVMScalarModel, FunnelResultsMatchDoubleWidthConcatenation) {
  for (unsigned Bits : {1U, 8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const std::string T = "i" + std::to_string(Bits);
    for (bool Left : {false, true}) {
      SCOPED_TRACE(Left);
      const std::string Name =
          std::string("llvm.") + (Left ? "fshl." : "fshr.") + T;
      Source S("declare " + T + " @" + Name + "(" + T + ", " + T + ", " + T +
               ")\ndefine " + T + " @f(" + T + " noundef %a, " + T +
               " noundef %b, " + T + " noundef %n) { %r = call noundef " + T +
               " @" + Name + "(" + T + " %a, " + T + " %b, " + T +
               " %n)\nret " + T + " %r }");
      ASSERT_TRUE(S.Module);
      auto M = modelLLVMScalarFunction(S.function());
      ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
      for (uint64_t Raw = 0; Raw < Bits * 2 + 2; ++Raw) {
        // Include count zero, exact multiples, distinct operands, high raw
        // count bits and signed-looking data. The oracle works on a joined
        // double word rather than the model's two shifts and conditional.
        for (uint64_t High : {uint64_t{0}, uint64_t{1} << (Bits - 1)}) {
          auto A = llvm::APInt(64, 0xa79135bdf24680e1ULL).zextOrTrunc(Bits);
          auto B = llvm::APInt(64, 0x184fe6239a57cd02ULL).zextOrTrunc(Bits);
          auto N = llvm::APInt(64, Raw | High).zextOrTrunc(Bits);
          const unsigned Count = N.getZExtValue() % Bits;
          auto Pair = (A.zext(Bits * 2).shl(Bits) | B.zext(Bits * 2));
          auto Expected = Left ? Pair.shl(Count).lshr(Bits).trunc(Bits)
                               : Pair.lshr(Count).trunc(Bits);
          auto Actual = evaluate(*M, {A, B, N});
          ASSERT_TRUE(Actual);
          EXPECT_EQ(*Actual, Expected.zext((Bits + 7) / 8 * 8));
        }
      }
    }
  }
}

TEST(LLVMScalarModel, GuardedProductsCheckBothSignsAndFullHighWord) {
  for (unsigned Bits : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const std::string T = "i" + std::to_string(Bits);
    const llvm::APInt Values[] = {llvm::APInt(Bits, 0),
                                  llvm::APInt(Bits, 1),
                                  llvm::APInt(Bits, 3),
                                  llvm::APInt::getSignedMinValue(Bits),
                                  llvm::APInt::getSignedMaxValue(Bits),
                                  llvm::APInt::getAllOnes(Bits)};
    for (const char *Flags : {"nuw", "nsw", "nuw nsw"}) {
      SCOPED_TRACE(Flags);
      Source S("define " + T + " @f(" + T + " noundef %x, " + T +
               " noundef %y) { %r = mul " + Flags + " " + T + " %x, %y\nret " +
               T + " %r }");
      ASSERT_TRUE(S.Module);
      auto M = modelLLVMScalarFunction(S.function());
      ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
      for (auto A : Values)
        for (auto B : Values) {
          bool UnsignedOverflow = false, SignedOverflow = false;
          auto Expected = A.umul_ov(B, UnsignedOverflow);
          (void)A.smul_ov(B, SignedOverflow);
          bool Defined =
              (!llvm::StringRef(Flags).contains("nuw") || !UnsignedOverflow) &&
              (!llvm::StringRef(Flags).contains("nsw") || !SignedOverflow);
          auto R = evaluate(*M, {A, B}, Defined);
          ASSERT_TRUE(R);
          EXPECT_EQ(*R, Expected);
        }
    }
  }
}

TEST(LLVMScalarModel, UnsupportedEffectsAndContractsCannotBecomeScalarProofs) {
  const char *Cases[] = {
      "define i32 @f(ptr noundef %x) { %v = load i32, ptr %x\nret i32 %v }",
      "define i32 @f(i32 noundef %x) { %p = alloca i32\nret i32 %x }",
      "define i32 @f(i32 noundef %x) { %p = inttoptr i32 %x to ptr\nret i32 %x "
      "}",
      "define i32 @f(i32 noundef %x) { ret i32 undef }",
      "define i32 @f(i32 noundef %x) { ret i32 poison }",
      "define i32 @f(i32 noundef %x) { %v = freeze i32 %x\nret i32 %v }",
      "define i32 @f(i32 noundef %x) { %v = udiv i32 %x, 3\nret i32 %v }",
      "define i128 @f(i128 noundef %x) { ret i128 %x }",
      "define i7 @f(i7 noundef %x) { ret i7 %x }",
      "define fastcc i32 @f(i32 noundef %x) { ret i32 %x }",
      "define i32 @f(i32 noundef %x, ...) { ret i32 %x }",
      "define i32 @f(i32 noundef %x) \"unknown-contract\" { ret i32 %x }",
      "declare i32 @g(i32) memory(none)\n"
      "define i32 @f(i32 noundef %x) { %r = call i32 @g(i32 %x)\nret i32 %r }",
      "declare i32 @llvm.fshl.i32(i32, i32, i32)\n"
      "define i32 @f(i32 noundef %x) { %r = call range(i32 0, 8) i32 "
      "@llvm.fshl.i32(i32 %x, i32 %x, i32 0)\nret i32 %r }"};
  for (const char *IR : Cases) {
    SCOPED_TRACE(IR);
    auto R = check(IR, IR);
    EXPECT_EQ(R.Status, Status::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.Diagnostic.empty());
    EXPECT_EQ(R.CompletedPartitions, 0U);
  }
}

TEST(LLVMScalarModel, ReturnRangesRetainTheirDefinednessObligation) {
  constexpr char Good[] = R"(
define range(i8 0, 4) i8 @f(i8 noundef %x) {
 %r = and i8 %x, 3
 ret i8 %r
})";
  EXPECT_EQ(check(Good, Good).Status, Status::Proved);
  std::string Bad = Good;
  Bad.replace(Bad.find("0, 4"), 4, "0, 3");
  EXPECT_EQ(check(Good, Bad).Status, Status::Unproved);
}

TEST(LLVMScalarModel, ArithmeticDependencyCarriesCannotBeLost) {
  // The observed top bit after addition depends on every lower input bit,
  // including the rare all-ones carry. A single representative would lie.
  constexpr char Carry[] = R"(
define i8 @f(i8 noundef %x) {
 %sum = add i8 %x, 1
 %high = lshr i8 %sum, 7
 %c = icmp eq i8 %high, 0
 br i1 %c, label %a, label %b
a: ret i8 0
b: ret i8 1
})";
  constexpr char Wrong[] = "define i8 @f(i8 noundef %x) { ret i8 0 }";
  auto Self = check(Carry, Carry);
  ASSERT_EQ(Self.Status, Status::Proved) << Self.Diagnostic;
  EXPECT_EQ(Self.CompletedPartitions, 256U);
  EXPECT_EQ(check(Carry, Wrong).Status, Status::Unproved);
}

TEST(LLVMScalarModel, PeelingHistoryNeverBoundsTheActualLoop) {
  constexpr char Loop[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
  %limit = and i8 %n, 7
  br label %loop
loop:
  %i = phi i8 [0, %entry], [%next, %body]
  %v = phi i32 [%x, %entry], [%sum, %body]
  %more = icmp ult i8 %i, %limit
  br i1 %more, label %body, label %exit
body:
  %next = add nuw i8 %i, 1
  %sum = add i32 %v, 13
  br label %loop, !llvm.loop !0
exit:
  ret i32 %v
}
!0 = distinct !{!0, !1, !2}
!1 = !{!"llvm.loop.unroll.disable"}
!2 = !{!"llvm.loop.peeled.count", i32 )";
  constexpr char Formula[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
  %limit = and i8 %n, 7
  %wide = zext i8 %limit to i32
  %delta = mul i32 %wide, 13
  %sum = add i32 %x, %delta
  ret i32 %sum
})";
  for (const char *Count : {"0", "1", "11", "-1"}) {
    SCOPED_TRACE(Count);
    const auto Input = std::string(Loop) + Count + "}";
    const auto R = check(Input, Formula);
    ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
    EXPECT_EQ(R.CompletedPartitions, 8U);
    EXPECT_EQ(R.ControlBits,
              (std::vector<LLVMScalarControlBit>{{1, 0}, {1, 1}, {1, 2}}));
    std::string Wrong = Formula;
    Wrong.replace(Wrong.find("%wide, 13"), 9, "%wide, 12");
    EXPECT_EQ(check(Input, Wrong).Status, Status::Unproved);
    LLVMScalarEquivalenceLimits L;
    L.MaxWork = R.Work;
    EXPECT_EQ(check(Input, Formula, L).Status, Status::Proved);
    --L.MaxWork;
    const auto Short = check(Input, Formula, L);
    EXPECT_EQ(Short.Status, Status::BudgetExceeded);
    EXPECT_TRUE(Short.WorkLimitExceeded);
  }
}

TEST(LLVMScalarModel, LoopHistoryDoesNotExcusePoisonOrNontermination) {
  constexpr char Infinite[] = R"(
define i8 @f(i8 noundef %x) {
entry: br label %loop
loop: br label %loop, !llvm.loop !0
}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.peeled.count", i32 1}
)";
  LLVMScalarEquivalenceLimits L;
  L.MaxBlockVisits = 4;
  EXPECT_EQ(check(Infinite, Infinite, L).Status, Status::BudgetExceeded);
  constexpr char Poison[] = R"(
define i8 @f(i8 noundef %x) {
entry:
  %bad = add nuw i8 255, 1
  br label %exit, !llvm.loop !0
exit: ret i8 %x
}
!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.peeled.count", i32 1000}
)";
  EXPECT_EQ(check(Poison, Poison).Status, Status::Unproved);
}

TEST(LLVMScalarModel, LoopMetadataShapeAndUnknownContractsAreChecked) {
  Source S(R"(
define i8 @f(i8 noundef %x) {
entry: br label %exit
exit: ret i8 %x
}
)");
  ASSERT_TRUE(S.Module);
  auto &C = S.Context;
  auto *Count = llvm::MDString::get(C, "llvm.loop.peeled.count");
  auto *One = llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(C), 1));
  auto *Wide = llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(C), 1));
  auto Reject = [&](llvm::MDNode *Root) {
    S.function().front().getTerminator()->setMetadata(
        llvm::LLVMContext::MD_loop, Root);
    const auto Before = S.text();
    const auto R = checkLLVMScalarEquivalence(S.function(), S.function());
    EXPECT_EQ(R.Status, Status::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.Diagnostic.empty());
    EXPECT_EQ(R.CompletedPartitions, 0U);
    EXPECT_EQ(S.text(), Before);
  };
  // Construct malformed attachments after parsing: LLVM's assembly debug-info
  // upgrader can dereference a null loop operand before our importer runs.
  llvm::Metadata *Properties[] = {
      llvm::MDNode::get(C, {Count}),
      llvm::MDNode::get(C, {Count, Wide}),
      llvm::MDNode::get(C, {Count, One, One}),
      llvm::MDNode::get(C, {Count, llvm::MDString::get(C, "one")}),
      llvm::MDNode::get(C, {Count, nullptr}),
      llvm::MDNode::get(
          C, {llvm::MDString::get(C, "llvm.loop.unroll.disable"), One}),
      llvm::MDNode::get(
          C, {llvm::MDString::get(C, "llvm.loop.mustprogress"), One}),
      llvm::MDNode::get(
          C, {llvm::MDString::get(C, "llvm.loop.parallel_accesses")}),
      llvm::MDNode::get(
          C, {llvm::MDString::get(C, "llvm.loop.peeled.unknown"), One}),
      llvm::MDNode::get(C, {}),
      llvm::MDNode::get(C, {One}),
      One,
      nullptr};
  for (unsigned N = 0; N < std::size(Properties); ++N) {
    SCOPED_TRACE(N);
    auto *Root = llvm::MDNode::getDistinct(C, {nullptr, Properties[N]});
    Root->replaceOperandWith(0, Root);
    Reject(Root);
  }
  llvm::MDNode *Roots[] = {llvm::MDNode::getDistinct(C, {}),
                           llvm::MDNode::getDistinct(C, {nullptr}),
                           llvm::MDNode::getDistinct(C, {One})};
  for (unsigned N = 0; N < std::size(Roots); ++N) {
    SCOPED_TRACE(N);
    Reject(Roots[N]);
  }
}
} // namespace neverd::analysis::scalar_test
