//===- LLVMScalarDecisionTests.cpp - Demand-driven scalar decisions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../LLVMHostFixture.h"
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

namespace neverd::analysis::scalar_test {
namespace {
// A bounded word recurrence, independently authored for the scalar model.
// Repeated exact shifts and extension guards build a deep definedness DAG.
// All data bits remain symbolic, including the high bits of the final XOR.
constexpr char Recurrence[] = R"(
define i64 @f(i64 noundef %x, i32 noundef %n) {
entry:
  %limit = and i32 %n, 15
  %seed = and i64 %x, 16777215
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = phi i64 [%seed, %entry], [%sum, %body]
  %go = icmp ult i32 %i, %limit
  br i1 %go, label %body, label %exit
body:
  %masked = and i64 %v, 1073741760
  %shifted = lshr exact i64 %masked, 6
  %small = trunc nuw nsw i64 %shifted to i32
  %wide = zext nneg i32 %small to i64
  %sum = add nuw nsw i64 %wide, %seed
  %next = add nuw nsw i32 %i, 1
  br label %loop
exit:
  %result = xor i64 %v, %x
  ret i64 %result
})";

void replace(std::string &IR, llvm::StringRef From, llvm::StringRef To) {
  auto At = IR.find(From.str());
  ASSERT_NE(At, std::string::npos);
  IR.replace(At, From.size(), To.str());
}

LLVMScalarEquivalenceResult
self(llvm::StringRef IR, const LLVMScalarEquivalenceLimits &Limits = {}) {
  Source Input(IR);
  const auto Before = Input.text();
  auto Result =
      checkLLVMScalarEquivalence(Input.function(), Input.function(), Limits);
  EXPECT_EQ(Input.text(), Before);
  return Result;
}

std::string twoBackedges() {
  std::string IR = Recurrence;
  replace(IR, "[%next, %body]", "[%next, %odd], [%next, %even]");
  replace(IR, "[%sum, %body]", "[%extra, %odd], [%sum, %even]");
  replace(IR, "  br label %loop\nexit:", R"(
  %bit = and i32 %i, 1
  %turn = icmp eq i32 %bit, 0
  br i1 %turn, label %even, label %odd
odd:
  %extra = add nuw nsw i64 %sum, 19
  br label %loop
even:
  br label %loop
exit:)");
  return IR;
}

constexpr char ShiftRecurrence[] = R"(
define i32 @f(i32 noundef %x, i32 noundef %y, i32 noundef %n) {
entry:
  %limit = and i32 %n, 7
  %seed = and i32 %x, 31
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %positive], [%next, %negative]
  %v = phi i32 [%seed, %entry], [%left, %positive], [%right, %negative]
  %more = icmp ult i32 %i, %limit
  br i1 %more, label %body, label %exit
body:
  %low = and i32 %v, 15
  %sum = add nuw nsw i32 %low, %seed
  %next = add nuw nsw i32 %i, 1
  %bit = and i32 %i, 1
  %even = icmp eq i32 %bit, 0
  br i1 %even, label %positive, label %negative
positive:
  %up = shl nuw nsw i32 %sum, 2
  %left = xor i32 %up, %y
  br label %loop
negative:
  %signed = or i32 %sum, -16
  %down = shl nsw i32 %signed, 1
  %right = xor i32 %down, %y
  br label %loop
exit:
  %result = xor i32 %v, %x
  ret i32 %result
})";

constexpr char ProductRecurrence[] = R"(
define i64 @f(i64 noundef %x, i32 noundef %n) {
entry:
  %limit = and i32 %n, 15
  %seed = and i64 %x, 4095
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = phi i64 [%seed, %entry], [%update, %body]
  %more = icmp ult i32 %i, %limit
  br i1 %more, label %body, label %exit
body:
  %masked = and i64 %v, 4095
  %twice = add nuw nsw i64 %masked, %masked
  %product = mul nuw nsw i64 %twice, 11
  %sum = add nuw nsw i64 %product, %seed
  %update = xor i64 %sum, %x
  %next = add nuw nsw i32 %i, 1
  br label %loop
exit:
  %result = xor i64 %v, %x
  ret i64 %result
})";
} // namespace

TEST(LLVMScalarDecision, RepeatedAdditionAndProductsKeepDataSymbolic) {
  constexpr char Sequence[] = R"(
define i32 @f(i32 noundef %x) {
  %v = and i32 %x, 1023
  %twice = add nuw nsw i32 %v, %v
  %triple = add nuw nsw i32 %twice, %v
  %product = mul nuw nsw i32 %triple, 7
  ret i32 %product
})";
  LLVMScalarEquivalenceLimits L;
  L.MaxControlBits = 0;
  L.MaxPartitions = 1;
  const auto R = self(Sequence, L);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_TRUE(R.ControlBits.empty());
  EXPECT_EQ(R.CompletedPartitions, 1U);
  constexpr char Reference[] = R"(
define i32 @f(i32 noundef %x) {
  %v = and i32 %x, 1023
  %result = mul i32 %v, 21
  ret i32 %result
})";
  EXPECT_EQ(check(Sequence, Reference, L).Status, Status::Proved);
  L.MaxWork = R.Work;
  EXPECT_EQ(self(Sequence, L).Status, Status::Proved);
  --L.MaxWork;
  const auto Short = self(Sequence, L);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_TRUE(Short.WorkLimitExceeded);
}

TEST(LLVMScalarDecision, ProductRecurrencesStillProveEveryOverflowGuard) {
  const auto R = self(ProductRecurrence);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits, (std::vector<LLVMScalarControlBit>{
                               {1, 0}, {1, 1}, {1, 2}, {1, 3}}));
  EXPECT_EQ(check(ProductRecurrence, ProductRecurrence).Status, Status::Proved);
  std::string Bad = ProductRecurrence;
  replace(Bad, "%masked = and i64 %v, 4095", "%masked = and i64 %v, -1");
  EXPECT_NE(self(Bad).Status, Status::Proved);
  Bad = ProductRecurrence;
  replace(Bad, "%twice, 11", "%twice, -1");
  EXPECT_NE(self(Bad).Status, Status::Proved);
}

TEST(LLVMScalarDecision, ProvesSignedAndUnsignedShiftGuardsOnBothBackedges) {
  const auto R = self(ShiftRecurrence);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 8U);
  EXPECT_EQ(R.ControlBits,
            (std::vector<LLVMScalarControlBit>{{2, 0}, {2, 1}, {2, 2}}));
  EXPECT_EQ(check(ShiftRecurrence, ShiftRecurrence).Status, Status::Proved);
  LLVMScalarEquivalenceLimits L;
  L.MaxWork = R.Work;
  EXPECT_EQ(self(ShiftRecurrence, L).Status, Status::Proved);
  --L.MaxWork;
  const auto Short = self(ShiftRecurrence, L);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_TRUE(Short.WorkLimitExceeded);

  std::string Wrong = ShiftRecurrence;
  replace(Wrong, "ret i32 %result", R"(
  %high = and i32 %y, -2147483648
  %wrong = xor i32 %result, %high
  ret i32 %wrong)");
  EXPECT_EQ(check(ShiftRecurrence, Wrong).Status, Status::Unproved);
}

TEST(LLVMScalarDecision, ShiftRelationsDoNotAuthorizeOverflowOrStaleFacts) {
  Source Input(ShiftRecurrence);
  ASSERT_EQ(
      checkLLVMScalarEquivalence(Input.function(), Input.function()).Status,
      Status::Proved);
  for (auto &B : Input.function())
    for (auto &I : B)
      if (I.getName() == "down")
        I.setOperand(1, llvm::ConstantInt::get(I.getType(), 31));
  EXPECT_NE(
      checkLLVMScalarEquivalence(Input.function(), Input.function()).Status,
      Status::Proved);
}

TEST(LLVMScalarDecision, DeepDefinednessRetainsTheCompleteControlDomain) {
  const auto R = self(Recurrence);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits, (std::vector<LLVMScalarControlBit>{
                               {1, 0}, {1, 1}, {1, 2}, {1, 3}}));
  const auto Paired = check(Recurrence, Recurrence);
  ASSERT_EQ(Paired.Status, Status::Proved) << Paired.Diagnostic;
  EXPECT_EQ(Paired.ControlBits, R.ControlBits);
  EXPECT_LT(R.Work, Paired.Work);

  std::string Wrong = Recurrence;
  replace(Wrong, "ret i64 %result", R"(
  %high = and i64 %x, -9223372036854775808
  %wrong = xor i64 %result, %high
  ret i64 %wrong)");
  EXPECT_EQ(check(Recurrence, Wrong).Status, Status::Unproved);
}

TEST(LLVMScalarDecision, ResolvesDeepBranchDecisionsWithoutDataEnumeration) {
  std::string IR = Recurrence;
  replace(IR, "  ret i64 %result", R"(
  %bounded = icmp ult i64 %v, 33554432
  %shift = select i1 %bounded, i64 0, i64 1
  %same = shl i64 %x, %shift
  %equal = icmp eq i64 %same, %x
  br i1 %equal, label %good, label %bad
good:
  ret i64 %result
bad:
  %poison = add nuw i8 255, 1
  ret i64 0)");
  const auto R = self(IR);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits.size(), 4U);
  EXPECT_EQ(check(Recurrence, IR).Status, Status::Proved);
}

TEST(LLVMScalarDecision,
     BothBackedgesAndLateUndefinedOperationsRemainRequired) {
  const auto IR = twoBackedges();
  const auto R = self(IR);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits.size(), 4U);
  EXPECT_EQ(check(IR, IR).Status, Status::Proved);

  std::string Bad = IR;
  replace(Bad, "  ret i64 %result", R"(
  %last = icmp eq i32 %limit, 15
  br i1 %last, label %bad, label %good
bad:
  %unused = add nuw i8 255, 1
  ret i64 %result
good:
  ret i64 %result)");
  const auto Refused = self(Bad);
  EXPECT_EQ(Refused.Status, Status::Unproved);
  EXPECT_EQ(Refused.CompletedPartitions, 15U);
  EXPECT_NE(Refused.Diagnostic.find("not defined"), std::string::npos);

  replace(Bad, "  %unused = add nuw i8 255, 1\n  ret i64 %result",
          "  br label %bad");
  LLVMScalarEquivalenceLimits L;
  L.MaxBlockVisits = 64;
  EXPECT_EQ(self(Bad, L).Status, Status::BudgetExceeded);
}

TEST(LLVMScalarDecision, RefinementChargesExactAndShortWorkAndRetainsCeilings) {
  const auto Baseline = self(Recurrence);
  ASSERT_EQ(Baseline.Status, Status::Proved) << Baseline.Diagnostic;
  LLVMScalarEquivalenceLimits L;
  L.MaxWork = Baseline.Work;
  const auto Exact = self(Recurrence, L);
  EXPECT_EQ(Exact.Status, Status::Proved) << Exact.Diagnostic;
  EXPECT_EQ(Exact.Work, L.MaxWork);
  EXPECT_FALSE(Exact.WorkLimitExceeded);
  --L.MaxWork;
  const auto Short = self(Recurrence, L);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_EQ(Short.Work, L.MaxWork);
  EXPECT_TRUE(Short.WorkLimitExceeded);

  for (unsigned Which = 0; Which != 4; ++Which) {
    SCOPED_TRACE(Which);
    L = {};
    if (Which == 0)
      L.MaxControlBits = 3;
    else if (Which == 1)
      L.MaxPartitions = 15;
    else if (Which == 2)
      L.MaxBlockVisits = 8;
    else
      L.MaxSymbolicNodes = 80;
    const auto Refused = self(Recurrence, L);
    EXPECT_EQ(Refused.Status, Status::BudgetExceeded);
    EXPECT_FALSE(Refused.WorkLimitExceeded);
  }
}

TEST(LLVMScalarDecision, MaskAndAnnotationChangesCannotReuseEarlierFacts) {
  Source Input(Recurrence);
  ASSERT_EQ(
      checkLLVMScalarEquivalence(Input.function(), Input.function()).Status,
      Status::Proved);
  // A mask that retains discarded shift bits makes `exact` invalid for some
  // inputs. A prior proof of the same function object grants no authority.
  for (auto &B : Input.function())
    for (auto &I : B)
      if (I.getName() == "masked")
        I.setOperand(1, llvm::ConstantInt::get(I.getType(), 1073741823));
  EXPECT_NE(
      checkLLVMScalarEquivalence(Input.function(), Input.function()).Status,
      Status::Proved);
}

TEST(LLVMScalarDecision, HostCompilerCopyRetainsConversionPoisonGuards) {
  Source Input(Recurrence);
  ASSERT_TRUE(Input.Module);
  const auto Before = Input.text();
  const auto Text = neverd::test::printHostCompilerFixture(*Input.Module);
  EXPECT_EQ(Input.text(), Before);
  EXPECT_NE(Before.find("trunc nuw nsw"), std::string::npos);
  EXPECT_NE(Before.find("zext nneg"), std::string::npos);
  EXPECT_EQ(Text.find("trunc nuw"), std::string::npos);
  EXPECT_EQ(Text.find("trunc nsw"), std::string::npos);
  EXPECT_EQ(Text.find("zext nneg"), std::string::npos);
  EXPECT_NE(Text.find("lshr exact"), std::string::npos);
  EXPECT_NE(Text.find("add nuw nsw"), std::string::npos);
  EXPECT_NE(Text.find("i64 noundef %x"), std::string::npos);
  Source Copy(Text);
  ASSERT_TRUE(Copy.Module);
  EXPECT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
}

TEST(LLVMScalarDecision, HostCompilerCopyKeepsOriginalRangeContracts) {
  Source Input(R"(
define noundef range(i8 0, 4) i8 @f(i8 noundef range(i8 0, 4) %x) {
  %v = call noundef range(i8 0, 4) i8 @callee(i8 noundef range(i8 0, 4) %x)
  ret i8 %v
}
declare noundef range(i8 0, 4) i8 @callee(i8 noundef range(i8 0, 4))
)");
  ASSERT_TRUE(Input.Module);
  ASSERT_FALSE(llvm::verifyModule(*Input.Module, &llvm::errs()));
  const auto Before = Input.text();
  const auto Text = neverd::test::printHostCompilerFixture(*Input.Module);
  EXPECT_EQ(Input.text(), Before);
  EXPECT_NE(Before.find("range("), std::string::npos);
  EXPECT_EQ(Text.find("range("), std::string::npos);
  Source Copy(Text);
  ASSERT_TRUE(Copy.Module);
  ASSERT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
  for (const char *Name : {"f", "callee"}) {
    auto *Function = Copy.Module->getFunction(Name);
    ASSERT_TRUE(Function);
    EXPECT_TRUE(Function->getAttributes().hasRetAttr(llvm::Attribute::NoUndef));
    EXPECT_TRUE(Function->hasParamAttribute(0, llvm::Attribute::NoUndef));
  }
  auto *Call =
      llvm::dyn_cast<llvm::CallBase>(&Copy.function().getEntryBlock().front());
  ASSERT_TRUE(Call);
  EXPECT_TRUE(Call->getAttributes().hasRetAttr(llvm::Attribute::NoUndef));
  EXPECT_TRUE(Call->paramHasAttr(0, llvm::Attribute::NoUndef));
}

TEST(LLVMScalarDecision, HostCompilerCopyPreservesWholeObjectLifetimes) {
  Source Input(R"(
target datalayout = "e-p:64:64-p5:32:32-i64:64"
define void @f(i64 %n) {
  %fixed = alloca [3 x i32], align 4
  %dynamic = alloca i8, i64 %n
  %other = alloca i16, align 2, addrspace(5)
  call void @llvm.lifetime.start.p0(ptr %fixed)
  call void @llvm.lifetime.start.p0(ptr %dynamic)
  call void @llvm.lifetime.start.p5(ptr addrspace(5) %other)
  call void @llvm.lifetime.end.p0(ptr %fixed)
  call void @llvm.lifetime.end.p0(ptr %dynamic)
  call void @llvm.lifetime.end.p5(ptr addrspace(5) %other)
  ret void
}
declare void @llvm.lifetime.start.p0(ptr)
declare void @llvm.lifetime.end.p0(ptr)
declare void @llvm.lifetime.start.p5(ptr addrspace(5))
declare void @llvm.lifetime.end.p5(ptr addrspace(5))
)");
  ASSERT_TRUE(Input.Module);
  ASSERT_FALSE(llvm::verifyModule(*Input.Module, &llvm::errs()));
  const auto Before = Input.text();
  const auto Text = neverd::test::printHostCompilerFixture(*Input.Module);
  EXPECT_EQ(Input.text(), Before);
  for (llvm::StringRef Boundary : {"start", "end"}) {
    EXPECT_NE(
        Text.find(
            ("@llvm.lifetime." + Boundary + ".p0(i64 12, ptr %fixed)").str()),
        std::string::npos);
    EXPECT_NE(
        Text.find(
            ("@llvm.lifetime." + Boundary + ".p0(i64 -1, ptr %dynamic)").str()),
        std::string::npos);
    EXPECT_NE(Text.find(("@llvm.lifetime." + Boundary +
                         ".p5(i64 2, ptr addrspace(5) %other)")
                            .str()),
              std::string::npos);
  }
  // The new LLVM reader upgrades the legacy signatures back to whole-object
  // markers. This must still verify, with every original pointer retained.
  Source Copy(Text);
  ASSERT_TRUE(Copy.Module);
  ASSERT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
  unsigned Boundaries = 0;
  for (auto &Block : Copy.function())
    for (auto &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        if (Call->getIntrinsicID() == llvm::Intrinsic::lifetime_start ||
            Call->getIntrinsicID() == llvm::Intrinsic::lifetime_end) {
          ++Boundaries;
          ASSERT_EQ(Call->arg_size(), 1u);
          EXPECT_TRUE(llvm::isa<llvm::AllocaInst>(Call->getArgOperand(0)));
        }
  EXPECT_EQ(Boundaries, 6u);
}

TEST(LLVMScalarDecision, HostCompilerCopyKeepsBinary80BitsAndNamedText) {
  Source Input(R"(
@f0x3FFF8000000000000008 = global x86_fp80 f0x3FFF8000000000000008
@text = constant [24 x i8] c"f0x3FFF8000000000000008\00"
define x86_fp80 @f(i1 %choose) {
entry:
  br i1 %choose, label %f0x3FFF8000000000000008, label %other
f0x3FFF8000000000000008:
  br label %join
other:
  br label %join
join:
  %v = phi x86_fp80 [ f0x3FFF8000000000000008, %f0x3FFF8000000000000008 ],
                    [ f0xBFFF8000000000000008, %other ]
  ret x86_fp80 %v
}
!named = !{!0}
!0 = !{!"f0x3FFF8000000000000008"}
)");
  ASSERT_TRUE(Input.Module);
  ASSERT_FALSE(llvm::verifyModule(*Input.Module, &llvm::errs()));
  const auto Before = Input.text();
  const auto Text = neverd::test::printHostCompilerFixture(*Input.Module);
  EXPECT_EQ(Input.text(), Before);
  EXPECT_NE(Text.find("x86_fp80 0xK3FFF8000000000000008"), std::string::npos);
  EXPECT_NE(Text.find("0xKBFFF8000000000000008"), std::string::npos);
  for (llvm::StringRef Named :
       {"@f0x3FFF8000000000000008",
        "f0x3FFF8000000000000008:", "%f0x3FFF8000000000000008",
        "c\"f0x3FFF8000000000000008\\00\"", "!\"f0x3FFF8000000000000008\""})
    EXPECT_NE(Text.find(Named.str()), std::string::npos) << Named.str();
  Source Copy(Text);
  ASSERT_TRUE(Copy.Module);
  ASSERT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
  auto *Original = llvm::cast<llvm::PHINode>(&Input.function().back().front());
  auto *Roundtrip = llvm::cast<llvm::PHINode>(&Copy.function().back().front());
  for (unsigned I = 0; I != 2; ++I)
    EXPECT_EQ(llvm::cast<llvm::ConstantFP>(Original->getIncomingValue(I))
                  ->getValueAPF()
                  .bitcastToAPInt(),
              llvm::cast<llvm::ConstantFP>(Roundtrip->getIncomingValue(I))
                  ->getValueAPF()
                  .bitcastToAPInt());
}

TEST(LLVMScalarDecision, HostCompilerCopyPreservesScalarConversionPoison) {
  auto Evaluate = [](Source &Input, uint64_t Word) {
    auto *Argument = Input.function().getArg(0);
    Argument->replaceAllUsesWith(
        llvm::ConstantInt::get(Argument->getType(), Word));
    for (auto &Instruction : Input.function().getEntryBlock()) {
      if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        auto *Value = Return->getReturnValue();
        if (llvm::isa<llvm::PoisonValue>(Value))
          return std::pair{true, uint64_t(0)};
        auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Value);
        EXPECT_TRUE(Integer);
        return std::pair{false,
                         Integer ? Integer->getZExtValue() : uint64_t(0)};
      }
      auto *Folded = llvm::ConstantFoldInstruction(
          &Instruction, Input.Module->getDataLayout());
      EXPECT_TRUE(Folded);
      if (Folded)
        Instruction.replaceAllUsesWith(Folded);
    }
    ADD_FAILURE() << "constant conversion fixture did not return";
    return std::pair{false, uint64_t(0)};
  };
  for (unsigned Width : {1u, 8u})
    for (const char *Flags : {"nuw", "nsw", "nuw nsw"}) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Flags);
      Source Original("define i" + std::to_string(Width) +
                      " @f(i16 noundef %x) { %v = trunc " + Flags +
                      " i16 %x to i" + std::to_string(Width) + "\nret i" +
                      std::to_string(Width) + " %v }");
      ASSERT_TRUE(Original.Module);
      const auto Text =
          neverd::test::printHostCompilerFixture(*Original.Module);
      for (uint64_t Word : {0u, 1u, 2u, 63u, 64u, 127u, 128u, 255u, 256u,
                            32767u, 32768u, 65407u, 65408u, 65534u, 65535u}) {
        SCOPED_TRACE(Word);
        Source Copy(Text);
        ASSERT_TRUE(Copy.Module);
        EXPECT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
        const int64_t Signed =
            Word < 32768 ? int64_t(Word) : int64_t(Word) - 65536;
        const uint64_t Mask = (uint64_t(1) << Width) - 1;
        const int64_t Bound = int64_t(1) << (Width - 1);
        const bool Poison =
            (llvm::StringRef(Flags).contains("nuw") && Word > Mask) ||
            (llvm::StringRef(Flags).contains("nsw") &&
             (Signed < -Bound || Signed >= Bound));
        const auto Result = Evaluate(Copy, Word);
        EXPECT_EQ(Result.first, Poison);
        if (!Poison)
          EXPECT_EQ(Result.second, Word & Mask);
      }
    }
  Source Original(R"(define i16 @f(i8 noundef %x) {
    %v = zext nneg i8 %x to i16
    ret i16 %v })");
  ASSERT_TRUE(Original.Module);
  const auto Text = neverd::test::printHostCompilerFixture(*Original.Module);
  for (uint64_t Word : {0u, 1u, 127u, 128u, 255u}) {
    SCOPED_TRACE(Word);
    Source Copy(Text);
    ASSERT_TRUE(Copy.Module);
    const auto Result = Evaluate(Copy, Word);
    EXPECT_EQ(Result.first, Word >= 128);
    if (Word < 128)
      EXPECT_EQ(Result.second, Word);
  }
}

TEST(LLVMScalarDecision, HostCompilerCopyPreservesVectorConversionPoison) {
  auto Check = [](Source &Copy, llvm::ArrayRef<uint64_t> Words, unsigned Width,
                  llvm::ArrayRef<bool> Poison) {
    auto *Argument = Copy.function().getArg(0);
    auto *Element = Argument->getType()->getScalarType();
    llvm::SmallVector<llvm::Constant *> Values;
    for (auto Word : Words)
      Values.push_back(llvm::ConstantInt::get(Element, Word));
    Argument->replaceAllUsesWith(llvm::ConstantVector::get(Values));
    for (auto &Instruction : Copy.function().getEntryBlock()) {
      if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        auto *Value = llvm::dyn_cast<llvm::Constant>(Return->getReturnValue());
        ASSERT_TRUE(Value);
        for (unsigned Lane = 0; Lane != Words.size(); ++Lane) {
          SCOPED_TRACE(Lane);
          auto *Result = Value->getAggregateElement(Lane);
          ASSERT_TRUE(Result);
          EXPECT_EQ(llvm::isa<llvm::PoisonValue>(Result), Poison[Lane]);
          if (!Poison[Lane]) {
            auto *Integer = llvm::dyn_cast<llvm::ConstantInt>(Result);
            ASSERT_TRUE(Integer);
            EXPECT_EQ(Integer->getZExtValue(),
                      Words[Lane] & ((uint64_t(1) << Width) - 1));
          }
        }
        return;
      }
      auto *Folded = llvm::ConstantFoldInstruction(
          &Instruction, Copy.Module->getDataLayout());
      ASSERT_TRUE(Folded);
      Instruction.replaceAllUsesWith(Folded);
    }
    FAIL() << "constant vector fixture did not return";
  };
  for (const char *Flags : {"nuw", "nsw", "nuw nsw"}) {
    SCOPED_TRACE(Flags);
    Source Original(std::string("define <4 x i8> @f(<4 x i16> noundef %x) {") +
                    " %v = trunc " + Flags +
                    " <4 x i16> %x to <4 x i8>\nret <4 x i8> %v }");
    ASSERT_TRUE(Original.Module);
    Source Copy(neverd::test::printHostCompilerFixture(*Original.Module));
    ASSERT_TRUE(Copy.Module);
    ASSERT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
    const bool Unsigned = llvm::StringRef(Flags).contains("nuw");
    const bool Signed = llvm::StringRef(Flags).contains("nsw");
    Check(Copy, {127, 128, 65408, 65407}, 8, {false, Signed, Unsigned, true});
  }
  Source Original(R"(define <4 x i16> @f(<4 x i8> noundef %x) {
    %v = zext nneg <4 x i8> %x to <4 x i16>
    ret <4 x i16> %v })");
  ASSERT_TRUE(Original.Module);
  Source Copy(neverd::test::printHostCompilerFixture(*Original.Module));
  ASSERT_TRUE(Copy.Module);
  ASSERT_FALSE(llvm::verifyModule(*Copy.Module, &llvm::errs()));
  Check(Copy, {0, 127, 128, 255}, 16, {false, false, true, true});
}

class LLVMScalarDecisionCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarDecisionCompiled, ProductRecurrenceMatchesIndependentOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  ASSERT_EQ(self(ProductRecurrence).Status, Status::Proved);
  const auto Source = tmpFile("product-recurrence.ll");
  const auto Harness = tmpFile("product-oracle.c");
  std::ofstream(Source) << ProductRecurrence;
  std::ofstream(Harness) << R"(
#include <stdint.h>
uint64_t f(uint64_t, uint32_t);
static uint64_t next_word(uint64_t *s) {
  *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
  return *s;
}
int main(void) {
  uint64_t state = UINT64_C(0x548137ac9fe062bd);
  const uint64_t edges[] = {0, 1, 4095, 4096, 65535, 65536,
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
  for (unsigned k = 0; k < 8192; ++k) {
    uint64_t x = k < 256 * 9 ? edges[k / 256] : next_word(&state);
    uint32_t n = k < 256 * 9 ? k % 256 : (uint32_t)next_word(&state);
    uint64_t seed = x & 4095, v = seed;
    for (uint32_t i = 0; i < (n & 15); ++i)
      v = ((v & 4095) * 22 + seed) ^ x;
    if (f(x, n) != (v ^ x)) return 1;
  }
  return 0;
})";
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    auto Program = tmpFile("product-oracle");
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Optimization, "-fsanitize=undefined",
                       "-fsanitize-trap=undefined", Source.string(),
                       Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}

TEST_F(LLVMScalarDecisionCompiled, ShiftBackedgesMatchIndependentArithmetic) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  ASSERT_EQ(self(ShiftRecurrence).Status, Status::Proved);
  const auto Source = tmpFile("shift-recurrence.ll");
  const auto Harness = tmpFile("shift-oracle.c");
  std::ofstream(Source) << ShiftRecurrence;
  std::ofstream(Harness) << R"(
#include <stdint.h>
uint32_t f(uint32_t, uint32_t, uint32_t);
static uint32_t next_word(uint32_t *s) {
  *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
  return *s;
}
int main(void) {
  uint32_t state = UINT32_C(0x3857bd9f);
  const uint32_t edges[] = {0, 1, 15, 16, 31, 32,
    UINT32_C(0x7fffffff), UINT32_C(0x80000000), UINT32_MAX};
  for (unsigned k = 0; k < 8192; ++k) {
    uint32_t x = k < 256 * 9 ? edges[k / 256] : next_word(&state);
    uint32_t y = next_word(&state);
    uint32_t n = k < 256 * 9 ? k % 256 : next_word(&state);
    uint32_t seed = x & 31, v = seed;
    for (uint32_t i = 0; i < (n & 7); ++i) {
      uint32_t sum = (v & 15) + seed;
      v = ((i & 1) ? ((sum | UINT32_C(0xfffffff0)) << 1) : (sum << 2)) ^ y;
    }
    if (f(x, y, n) != (v ^ x)) return 1;
  }
  return 0;
})";
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    auto Program = tmpFile("shift-oracle");
    auto Built = exec(NEVERD_TEST_CLANG,
                      {"-std=c11", Optimization, "-fsanitize=undefined",
                       "-fsanitize-trap=undefined", Source.string(),
                       Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}

TEST_F(LLVMScalarDecisionCompiled, DeepOneAndTwoBackedgeOracles) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (bool Two : {false, true}) {
    SCOPED_TRACE(Two);
    const std::string IR = Two ? twoBackedges() : Recurrence;
    ASSERT_EQ(self(IR).Status, Status::Proved);
    const auto Source = tmpFile("recurrence.ll");
    const auto Harness = tmpFile("oracle.c");
    scalar_test::Source Input(IR);
    ASSERT_TRUE(Input.Module);
    const auto Original = Input.text();
    std::ofstream(Source) << neverd::test::printHostCompilerFixture(
        *Input.Module);
    EXPECT_EQ(Input.text(), Original);
    std::ofstream(Harness) << R"(
#include <stdint.h>
uint64_t f(uint64_t, uint32_t);
static uint64_t next_word(uint64_t *s) {
  *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
  return *s;
}
int main(void) {
  uint64_t state = UINT64_C(0x4c718e9ad562b30f);
  const uint64_t edges[] = {0, 1, 63, 64, 16777215, 16777216,
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
  for (unsigned k = 0; k < 8192; ++k) {
    uint64_t x = k < 256 * 9 ? edges[k / 256] : next_word(&state);
    uint32_t n = k < 256 * 9 ? k % 256 : (uint32_t)next_word(&state);
    uint64_t seed = x & UINT64_C(0xffffff), v = seed;
    for (uint32_t i = 0; i < (n & 15); ++i) {
      v = ((v >> 6) & UINT64_C(0xffffff)) + seed;
      if (TWO && (i & 1)) v += 19;
    }
    if (f(x, n) != (v ^ x)) return 1;
  }
  return 0;
})";
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      auto Program = tmpFile("oracle");
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Optimization, Two ? "-DTWO=1" : "-DTWO=0",
                "-fsanitize=undefined", "-fsanitize-trap=undefined",
                Source.string(), Harness.string(), "-o", Program.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      const auto Ran = exec(Program.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err;
    }
  }
}
} // namespace neverd::analysis::scalar_test
