//===- ByteMemoryForwardingTests.cpp - Exact overlapping memory
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/pass/ir/simplify/ByteMemoryForwardingPass.h"
#include "neverd/pass/ir/simplify/SymSimplifyPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <array>
#include <string>

using namespace neverd;

namespace {

std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &C, llvm::StringRef Body,
                                    llvm::StringRef Layout = "e-p:64:64") {
  llvm::SMDiagnostic Error;
  auto M = llvm::parseAssemblyString(
      "target datalayout = \"" + Layout.str() + "\"\n" + Body.str(), Error, C);
  if (!M) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Error.print("byte-forward-test", OS);
    ADD_FAILURE() << Text;
    return nullptr;
  }
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

std::string print(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  return Text;
}

template <typename T> unsigned count(const llvm::Function &F) {
  unsigned Result = 0;
  for (const llvm::Instruction &I : llvm::instructions(F))
    Result += llvm::isa<T>(I);
  return Result;
}

ByteMemoryForwardingOptions withSnapshots() {
  ByteMemoryForwardingOptions Options;
  Options.AllowStoreSnapshots = true;
  return Options;
}

ByteMemoryForwardingResult
forward(llvm::Function &F,
        ByteMemoryForwardingOptions Options = withSnapshots()) {
  auto Result = ByteMemoryForwardingPass::forward(F, Options);
  EXPECT_FALSE(llvm::verifyFunction(F, &llvm::errs())) << print(F);
  return Result;
}

llvm::ConstantInt *constantReturn(llvm::Function &F) {
  for (auto It = F.getEntryBlock().begin(); It != F.getEntryBlock().end();) {
    llvm::Instruction *I = &*It++;
    if (auto *C =
            llvm::ConstantFoldInstruction(I, F.getParent()->getDataLayout())) {
      I->replaceAllUsesWith(C);
      I->eraseFromParent();
    }
  }
  auto *Return =
      llvm::dyn_cast<llvm::ReturnInst>(F.getEntryBlock().getTerminator());
  return Return ? llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue())
                : nullptr;
}

TEST(ByteMemoryForwarding, OverlappingLastWritersRespectBothByteOrders) {
  // Independent byte-array oracle: start with a wide word, overwrite two
  // interior bytes, then read each supported width at a different offset.
  for (bool Little : {false, true}) {
    for (unsigned Bits : {8u, 16u, 24u, 32u, 40u, 64u, 72u, 120u, 128u}) {
      SCOPED_TRACE(::testing::Message() << Little << ':' << Bits);
      std::array<unsigned char, 16> Bytes{};
      for (unsigned J = 0; J != 16; ++J)
        Bytes[J] = Little ? J + 1 : 16 - J;
      Bytes[5] = Little ? 0xd3 : 0x6b;
      Bytes[6] = Little ? 0x6b : 0xd3;
      const unsigned Offset = Bits > 64 ? 0 : 4;
      llvm::APInt Expected(Bits, 0);
      for (unsigned J = 0; J != Bits / 8; ++J)
        Expected |= llvm::APInt(Bits, Bytes[Offset + J])
                    << ((Little ? J : Bits / 8 - 1 - J) * 8);
      llvm::APInt Initial(128, "100f0e0d0c0b0a090807060504030201", 16);
      llvm::SmallString<48> Decimal;
      Initial.toString(Decimal, 10, false);
      llvm::LLVMContext C;
      auto M = parse(C,
                     "define i" + std::to_string(Bits) + R"( @f() {
        %frame = alloca [16 x i8], align 16
        store i128 )" + Decimal.str().str() +
                         R"(, ptr %frame, align 1
        %p = getelementptr i8, ptr %frame, i64 5
        store i16 27603, ptr %p, align 1
        %q = getelementptr i8, ptr %frame, i64 )" +
                         std::to_string(Offset) + "\n %v = load i" +
                         std::to_string(Bits) + ", ptr %q, align 1\n ret i" +
                         std::to_string(Bits) + " %v\n}",
                     Little ? "e-p:64:64" : "E-p:64:64");
      ASSERT_TRUE(M);
      auto &F = *M->getFunction("f");
      EXPECT_EQ(forward(F).ForwardedLoads, 1u);
      EXPECT_EQ(count<llvm::StoreInst>(F), 2u);
      EXPECT_EQ(count<llvm::FreezeInst>(F), 0u);
      auto *Actual = constantReturn(F);
      ASSERT_NE(Actual, nullptr) << print(F);
      EXPECT_EQ(Actual->getValue(), Expected);
    }
  }
}

constexpr llvm::StringLiteral Simple = R"(
define i16 @f(i16 noundef %x) {
  %frame = alloca [8 x i8], align 8
  store i16 %x, ptr %frame, align 1
  %v = load i16, ptr %frame, align 1
  ret i16 %v
})";

TEST(ByteMemoryForwarding, DoesNotFreezeDefinedValuesOrAnExistingSnapshot) {
  for (llvm::StringRef Value : {"%x", "42", "%frozen"}) {
    llvm::LLVMContext C;
    std::string IR = Simple.str();
    IR.insert(IR.find("  store"), "  %frozen = freeze i16 %x\n");
    IR.replace(IR.find("store i16 %x"), 12, "store i16 " + Value.str());
    auto M = parse(C, IR);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = forward(F);
    EXPECT_EQ(R.ForwardedLoads, 1u);
    EXPECT_EQ(R.FrozenStores, 0u);
    EXPECT_EQ(count<llvm::FreezeInst>(F), 1u);
  }
}

TEST(ByteMemoryForwarding,
     SharesOneSnapshotWithTheOriginalStoreAndRetainedRead) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i16 @f(ptr %external) {
      %frame = alloca [4 x i8], align 4
      %chosen = select i1 undef, i16 0, i16 -1
      store i16 %chosen, ptr %frame, align 1
      %lo = load i8, ptr %frame, align 1
      %p = getelementptr i8, ptr %frame, i64 1
      %hi = load i8, ptr %p, align 1
      %a = zext i8 %lo to i16
      %b = zext i8 %hi to i16
      %b1 = shl i16 %b, 8
      %pair = or i16 %a, %b1
      store ptr %frame, ptr %external
      %retained = load i16, ptr %frame, align 1
      %result = xor i16 %retained, %pair
      ret i16 %result
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  auto R = forward(F);
  EXPECT_EQ(R.ForwardedLoads, 2u);
  EXPECT_EQ(R.FrozenStores, 1u);
  EXPECT_EQ(count<llvm::LoadInst>(F), 1u);
  auto *Store =
      llvm::dyn_cast<llvm::StoreInst>(&*std::next(F.begin()->begin(), 3));
  ASSERT_NE(Store, nullptr) << print(F);
  auto *Snapshot = llvm::dyn_cast<llvm::FreezeInst>(Store->getValueOperand());
  ASSERT_NE(Snapshot, nullptr);
  EXPECT_EQ(Snapshot->getNextNode(), Store);
  unsigned FragmentUses = 0;
  for (llvm::User *U : Snapshot->users())
    FragmentUses += llvm::isa<llvm::BinaryOperator>(U);
  EXPECT_EQ(FragmentUses, 2u);
  EXPECT_EQ(Snapshot->getOperand(0)->getNumUses(), 1u);
}

TEST(ByteMemoryForwarding,
     SeparateStoresOfAnUndefinedValueHaveSeparateSnapshots) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i16 @f(i16 %x) {
      %a = alloca [2 x i8]
      %b = alloca [2 x i8]
      store i16 %x, ptr %a, align 1
      store i16 %x, ptr %b, align 1
      %av = load i16, ptr %a, align 1
      %bv = load i16, ptr %b, align 1
      %v = xor i16 %av, %bv
      ret i16 %v
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  EXPECT_EQ(forward(F).FrozenStores, 2u);
  EXPECT_EQ(count<llvm::FreezeInst>(F), 2u);
  EXPECT_EQ(F.getArg(0)->getNumUses(), 2u);
}

TEST(ByteMemoryForwarding, RefinementDoesNotCertifyHiddenArgumentIndependence) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i16 @f(i16 noundef %hidden) {
      %a = alloca [2 x i8]
      %choice = select i1 undef, i16 %hidden, i16 0
      store i16 %choice, ptr %a, align 1
      %v = load i16, ptr %a, align 1
      ret i16 %v
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  EXPECT_EQ(forward(F).FrozenStores, 1u);
  EXPECT_FALSE(F.getArg(0)->use_empty());
  // LLVM may subsequently refine the undefined choice to zero. This pass
  // neither deletes parameters nor issues a native independence certificate.
  EXPECT_EQ(F.arg_size(), 1u);
}

TEST(ByteMemoryForwarding, BarriersDiscardAllPriorWriters) {
  for (llvm::StringRef Barrier :
       {"store i8 7, ptr %other", "store float 1.0, ptr %frame, align 1",
        "call void @clobber(ptr %frame)", "call void @reader(ptr %frame)",
        "store volatile i16 1, ptr %frame, align 2",
        "%ordered = load volatile i16, ptr %frame, align 2",
        "store atomic i16 1, ptr %frame seq_cst, align 2",
        "%ordered = load atomic i16, ptr %frame acquire, align 2",
        "%rmw = atomicrmw add ptr %frame, i16 1 monotonic, align 2",
        "fence seq_cst", "call void @llvm.lifetime.end.p0(ptr %frame)",
        "call void @llvm.memset.p0.i64(ptr %frame, i8 0, i64 2, i1 false)"}) {
    SCOPED_TRACE(Barrier.str());
    llvm::LLVMContext C;
    std::string IR = Simple.str();
    IR.replace(IR.find("@f(i16 noundef %x)"), 18,
               "@f(i16 noundef %x, ptr %other)");
    IR.insert(IR.find("  %v = load"), "  " + Barrier.str() + "\n");
    IR += R"(
      declare void @clobber(ptr)
      declare void @reader(ptr) memory(read)
      declare void @llvm.lifetime.end.p0(ptr)
      declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
    )";
    auto M = parse(C, IR);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    const auto Before = print(F);
    EXPECT_EQ(forward(F).ForwardedLoads, 0u);
    EXPECT_EQ(print(F), Before);
  }
}

TEST(ByteMemoryForwarding, FreshCompleteWritesAfterABarrierCanBeForwarded) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    declare void @clobber(ptr)
    define i16 @f(i16 noundef %x) {
      %a = alloca [2 x i8]
      store i16 3, ptr %a, align 1
      call void @clobber(ptr %a)
      store i16 %x, ptr %a, align 1
      %v = load i16, ptr %a, align 1
      ret i16 %v
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  EXPECT_EQ(forward(F).ForwardedLoads, 1u);
  EXPECT_EQ(count<llvm::StoreInst>(F), 2u);
  EXPECT_EQ(count<llvm::CallInst>(F), 1u);
}

TEST(ByteMemoryForwarding, IncompleteDynamicAndOutOfObjectAccessesStayIntact) {
  for (llvm::StringRef Address :
       {"%frame", "%dynamic", "%negative", "%end", "%roundtrip"}) {
    for (unsigned Bits : {8u, 16u}) {
      SCOPED_TRACE(::testing::Message() << Address.str() << ':' << Bits);
      llvm::LLVMContext C;
      std::string IR = R"(
        define i16 @f(i64 %offset) {
          %frame = alloca [2 x i8]
          %dynamic = getelementptr i8, ptr %frame, i64 %offset
          %negative = getelementptr i8, ptr %frame, i64 -1
          %end = getelementptr i8, ptr %frame, i64 2
          %number = ptrtoint ptr %frame to i64
          %roundtrip = inttoptr i64 %number to ptr
          store i8 9, ptr )" +
                       Address.str() + ", align 1\n" + " %v = load i" +
                       std::to_string(Bits) + ", ptr %frame, align 1\n";
      IR += Bits == 8 ? " %wide = zext i8 %v to i16\n ret i16 %wide\n}"
                      : " ret i16 %v\n}";
      auto M = parse(C, IR);
      ASSERT_TRUE(M);
      auto &F = *M->getFunction("f");
      EXPECT_EQ(forward(F).ForwardedLoads,
                Address == "%frame" && Bits == 8 ? 1u : 0u);
    }
  }
}

TEST(ByteMemoryForwarding, UnknownStoreInvalidatesPreviouslyKnownBytes) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i16 @f(i64 %offset) {
      %a = alloca [8 x i8]
      store i16 21, ptr %a, align 1
      %p = getelementptr i8, ptr %a, i64 %offset
      store i8 0, ptr %p
      %v = load i16, ptr %a, align 1
      ret i16 %v
    })");
  ASSERT_TRUE(M);
  EXPECT_EQ(forward(*M->getFunction("f")).ForwardedLoads, 0u);
}

TEST(ByteMemoryForwarding, DoesNotCarryFactsAcrossBranchesOrLoopIterations) {
  for (bool Loop : {false, true}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
      define i16 @f(i1 %again) {
      entry:
        %a = alloca [2 x i8]
        store i16 3, ptr %a, align 1
        br label %next
      next:
        %v = load i16, ptr %a, align 1
        store i16 5, ptr %a, align 1
        br i1 %again, label %exit, label %)" +
                          std::string(Loop ? "next" : "exit") + R"(
      exit:
        ret i16 %v
      })");
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    EXPECT_EQ(forward(F).ForwardedLoads, 0u);
    EXPECT_EQ(count<llvm::LoadInst>(F), 1u);
  }
}

TEST(ByteMemoryForwarding,
     RejectsUncertifiedPointerRepresentationsAndAllocations) {
  for (llvm::StringRef Layout : {"e-p:128:128", "e-p:64:64:64:32"}) {
    llvm::LLVMContext C;
    auto M = parse(C, Simple, Layout);
    ASSERT_TRUE(M);
    EXPECT_EQ(forward(*M->getFunction("f")).ForwardedLoads, 0u);
  }
  for (llvm::StringRef Allocation :
       {"alloca [65537 x i8]", "alloca [8 x i8], i64 2", "alloca i64",
        "alloca [8 x i8], i16 %x"}) {
    llvm::LLVMContext C;
    std::string IR = Simple.str();
    IR.replace(IR.find("alloca [8 x i8], align 8"), 24, Allocation.str());
    auto M = parse(C, IR);
    ASSERT_TRUE(M);
    EXPECT_EQ(forward(*M->getFunction("f")).ForwardedLoads, 0u);
  }
}

TEST(ByteMemoryForwarding, SupportsIntegral32BitAddresses) {
  llvm::LLVMContext C;
  auto M = parse(C, Simple, "e-p:32:32");
  ASSERT_TRUE(M);
  EXPECT_EQ(forward(*M->getFunction("f")).ForwardedLoads, 1u);
}

TEST(ByteMemoryForwarding,
     RejectsOtherAddressSpacesIncludingNonIntegralPointers) {
  for (llvm::StringRef Layout :
       {"e-p:64:64-p1:64:64", "e-p:64:64-p1:64:64-ni:1"}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
      define i16 @f() {
        %a = alloca [2 x i8], addrspace(1)
        store i16 37, ptr addrspace(1) %a, align 1
        %v = load i16, ptr addrspace(1) %a, align 1
        ret i16 %v
      })",
                   Layout);
    ASSERT_TRUE(M);
    EXPECT_EQ(forward(*M->getFunction("f")).ForwardedLoads, 0u);
  }
}

TEST(ByteMemoryForwarding, ForwardedValuesCanBecomeLaterWriters) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i32 @f() {
      %a = alloca [4 x i8]
      store i32 287454020, ptr %a, align 1
      %lo = load i16, ptr %a, align 1
      %p = getelementptr i8, ptr %a, i64 1
      store i16 %lo, ptr %p, align 1
      %v = load i32, ptr %a, align 1
      ret i32 %v
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  EXPECT_EQ(forward(F).ForwardedLoads, 2u);
  auto *Value = constantReturn(F);
  ASSERT_NE(Value, nullptr);
  EXPECT_EQ(Value->getZExtValue(), 0x11334444u);
}

TEST(ByteMemoryForwarding, TrackingExhaustionDiscardsPreviouslyKnownWriters) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i16 @f() {
      %a = alloca [2 x i8]
      %b = alloca [2 x i8]
      store i16 3, ptr %a, align 1
      store i16 7, ptr %b, align 1
      %v = load i16, ptr %a, align 1
      ret i16 %v
    })");
  ASSERT_TRUE(M);
  ByteMemoryForwardingOptions Options;
  Options.MaxTrackedBytes = 2;
  auto R = forward(*M->getFunction("f"), Options);
  EXPECT_TRUE(R.BudgetExhausted);
  EXPECT_EQ(R.ForwardedLoads, 0u);
}

TEST(ByteMemoryForwarding, PoisonSnapshotIsRefinementAndKeepsTheStore) {
  llvm::LLVMContext C;
  std::string IR = Simple.str();
  IR.replace(IR.find("store i16 %x"), 12, "store i16 poison");
  IR.insert(IR.find("  %v = load"), "  store i8 9, ptr %frame\n");
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  auto R = forward(F);
  EXPECT_EQ(R.ForwardedLoads, 1u);
  EXPECT_EQ(R.FrozenStores, 1u);
  EXPECT_EQ(count<llvm::StoreInst>(F), 2u);
}

TEST(ByteMemoryForwarding, DefaultPolicyKeepsValuesThatNeedSnapshotsInMemory) {
  llvm::LLVMContext C;
  std::string IR = Simple.str();
  IR.erase(IR.find("noundef "), 8);
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  const std::string Before = print(F);
  EXPECT_EQ(ByteMemoryForwardingPass::forward(F).ForwardedLoads, 0u);
  EXPECT_EQ(print(F), Before);
}

TEST(ByteMemoryForwarding, AddressSpaceRoundTripDoesNotHideAnAliasingWrite) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i16 @f() {
      %a = alloca [2 x i8]
      store i16 37, ptr %a, align 1
      %other = addrspacecast ptr %a to ptr addrspace(1)
      %back = addrspacecast ptr addrspace(1) %other to ptr
      store i8 0, ptr %back
      %v = load i16, ptr %a, align 1
      ret i16 %v
    })",
                 "e-p:64:64-p1:64:64");
  ASSERT_TRUE(M);
  EXPECT_EQ(forward(*M->getFunction("f")).ForwardedLoads, 0u);
}

TEST(ByteMemoryForwarding, HighFanInDoesNotExpandTheOptionalDefinednessQuery) {
  for (uint64_t OutputBudget : {uint64_t(0), uint64_t(200)}) {
    llvm::LLVMContext C;
    std::string IR = R"(
      define i16 @f(i32 %index, i16 noundef %x) {
      entry:
        %a = alloca [2 x i8]
        switch i32 %index, label %next [
    )";
    for (unsigned I = 0; I != 1024; ++I)
      IR += " i32 " + std::to_string(I) + ", label %next\n";
    IR += " ]\nnext:\n %choice = phi i16 [ %x, %entry ]";
    for (unsigned I = 0; I != 1024; ++I)
      IR += ", [ %x, %entry ]";
    IR += R"(
        store i16 %choice, ptr %a, align 1
        %first = load i16, ptr %a, align 1
        %second = load i16, ptr %a, align 1
        %result = xor i16 %first, %second
        ret i16 %result
      })";
    auto M = parse(C, IR);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    ByteMemoryForwardingOptions Options = withSnapshots();
    Options.MaxNewInstructions = OutputBudget;
    auto R = forward(F, Options);
    EXPECT_EQ(R.ForwardedLoads, OutputBudget ? 2u : 0u);
    EXPECT_EQ(R.FrozenStores, OutputBudget ? 1u : 0u);
    EXPECT_EQ(R.Instructions, 8u);
  }
}

TEST(ByteMemoryForwarding,
     UseBudgetIsReservedBeforeChangingEitherMemoryAccess) {
  for (uint64_t Limit : {uint64_t(3), uint64_t(4)}) {
    llvm::LLVMContext C;
    std::string IR = Simple.str();
    IR.insert(IR.find("  ret"), "  %sum = add i16 %v, %v\n");
    IR.replace(IR.find("ret i16 %v"), 10, "ret i16 %sum");
    auto M = parse(C, IR);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    ByteMemoryForwardingOptions Options;
    Options.MaxUseSteps = Limit;
    auto R = forward(F, Options);
    EXPECT_EQ(R.ForwardedLoads, Limit == 4 ? 1u : 0u);
    EXPECT_EQ(R.BudgetExhausted, Limit != 4);
    EXPECT_EQ(count<llvm::StoreInst>(F), 1u);
  }
}

TEST(ByteMemoryForwarding, BudgetBoundariesNeverPartiallyRewriteALoad) {
  for (unsigned Budget = 0; Budget != 5; ++Budget) {
    llvm::LLVMContext C;
    auto M = parse(C, Simple);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto Full = forward(F);
    const uint64_t Limits[] = {Full.Instructions, Full.AddressSteps,
                               Full.PeakTrackedBytes, Full.NewInstructions,
                               Full.UseSteps};
    for (uint64_t Limit : {uint64_t(0), Limits[Budget] - 1, Limits[Budget]}) {
      SCOPED_TRACE(::testing::Message() << Budget << ':' << Limit);
      llvm::LLVMContext C2;
      auto M2 = parse(C2, Simple);
      ASSERT_TRUE(M2);
      auto &F2 = *M2->getFunction("f");
      ByteMemoryForwardingOptions Options;
      uint64_t *Fields[] = {&Options.MaxInstructions, &Options.MaxAddressSteps,
                            &Options.MaxTrackedBytes,
                            &Options.MaxNewInstructions, &Options.MaxUseSteps};
      *Fields[Budget] = Limit;
      auto R = forward(F2, Options);
      EXPECT_EQ(R.BudgetExhausted, Limit < Limits[Budget]);
      EXPECT_EQ(count<llvm::StoreInst>(F2), 1u);
      EXPECT_EQ(count<llvm::LoadInst>(F2) + R.ForwardedLoads, 1u);
      if (Limit == Limits[Budget])
        EXPECT_EQ(R.ForwardedLoads, 1u);
      if (Limit == 0 || (Budget != 0 && Limit < Limits[Budget]))
        EXPECT_EQ(R.ForwardedLoads, 0u);
    }
  }
}

TEST(ByteMemoryForwarding, ExhaustedOutputBudgetDoesNotFreezeAnUnusedStore) {
  llvm::LLVMContext C;
  std::string IR = Simple.str();
  IR.erase(IR.find("noundef "), 8);
  auto M = parse(C, IR);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  const std::string Before = print(F);
  ByteMemoryForwardingOptions Options = withSnapshots();
  Options.MaxNewInstructions =
      10; // Two bytes need ten instructions plus freeze.
  auto R = forward(F, Options);
  EXPECT_TRUE(R.BudgetExhausted);
  EXPECT_EQ(R.FrozenStores, 0u);
  EXPECT_EQ(print(F), Before);
}

TEST(ByteMemoryForwarding, SkipsFunctionsStampedByObfuscation) {
  llvm::LLVMContext C;
  auto M = parse(C, Simple);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  F.addFnAttr(kObfuscatedFnAttr);
  auto Before = print(F);
  EXPECT_EQ(forward(F).ForwardedLoads, 0u);
  EXPECT_EQ(print(F), Before);
}

constexpr llvm::StringLiteral RuntimeIR = R"(
define i64 @mix(i64 noundef %a, i64 noundef %b) {
  %frame = alloca [16 x i8], align 16
  %aw = zext i64 %a to i128
  %bw = zext i64 %b to i128
  %bh = shl i128 %bw, 64
  %wide = or i128 %aw, %bh
  store i128 %wide, ptr %frame, align 1
  %middle = getelementptr i8, ptr %frame, i64 5
  %short = trunc i64 %b to i16
  store i16 %short, ptr %middle, align 1
  %all = load i128, ptr %frame, align 1
  %low = trunc i128 %all to i64
  %highwide = lshr i128 %all, 64
  %high = trunc i128 %highwide to i64
  %answer = xor i64 %low, %high
  ret i64 %answer
})";

TEST(ByteMemoryForwarding, PipelineExposesOverlappingWordsToOrdinaryCleanup) {
  llvm::LLVMContext C;
  auto M = parse(C, RuntimeIR);
  ASSERT_TRUE(M);
  Pipeline::OptimizationOptions Options;
  Options.Strength = Pipeline::OptStrength::Thin;
  auto Result = Pipeline::optimizeModule(*M, Options);
  EXPECT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
  EXPECT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
  auto &F = *M->getFunction("mix");
  EXPECT_EQ(count<llvm::AllocaInst>(F), 0u) << print(F);
  EXPECT_EQ(count<llvm::LoadInst>(F), 0u) << print(F);
  EXPECT_EQ(count<llvm::StoreInst>(F), 0u) << print(F);
}

TEST(ByteMemoryForwarding,
     RecompiledOriginalAndForwardedIRMatchIndependentOracle) {
  llvm::LLVMContext C;
  auto M = parse(C, RuntimeIR);
  ASSERT_TRUE(M);
  auto *Original = M->getFunction("mix");
  llvm::ValueToValueMapTy Map;
  auto *Rewritten = llvm::CloneFunction(Original, Map);
  Rewritten->setName("forwarded");
  EXPECT_EQ(forward(*Rewritten).ForwardedLoads, 1u);
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program));
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> IR, Source, Binary, Error;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-byte-forward", "ll", IR));
  llvm::FileRemover RemoveIR(IR);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-byte-forward", "c", Source));
  llvm::FileRemover RemoveSource(Source);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-byte-forward", "exe", Binary));
  llvm::FileRemover RemoveBinary(Binary);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-byte-forward", "err", Error));
  llvm::FileRemover RemoveError(Error);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(IR, EC);
    ASSERT_FALSE(EC);
    M->print(OS, nullptr);
  }
  {
    llvm::raw_fd_ostream OS(Source, EC);
    ASSERT_FALSE(EC);
    OS << R"(
#include <stdint.h>
extern uint64_t mix(uint64_t, uint64_t);
extern uint64_t forwarded(uint64_t, uint64_t);
int main(void) {
  uint64_t state = 0x148673b9afc032d5ULL;
  const uint64_t edges[] = {0, 1, UINT64_MAX, UINT64_MAX / 2,
                            UINT64_MAX / 2 + 1, 0x1020304050607080ULL};
  for (unsigned i = 0; i < 4096 + 36; ++i) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    uint64_t a = i < 36 ? edges[i / 6] : state;
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    uint64_t b = i < 36 ? edges[i % 6] : state;
    uint64_t low = (a & ~(UINT64_C(65535) << 40)) | ((b & 65535) << 40);
    uint64_t expected = low ^ b;
    if (mix(a, b) != expected || forwarded(a, b) != expected) return 1;
  }
  return 0;
})";
  }
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Error.str()};
    int Exit = llvm::sys::ExecuteAndWait(
        Compiler, {Compiler, Optimization, IR, Source, "-o", Binary},
        std::nullopt, Redirects, 30);
    auto Errors = llvm::MemoryBuffer::getFile(Error);
    ASSERT_EQ(Exit, 0) << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Binary, {Binary}, std::nullopt,
                                        Redirects, 30),
              0);
  }
}

ByteMemoryForwardingOptions numericOptions() {
  ByteMemoryForwardingOptions Options;
  Options.SimplifyNumericMemory = true;
  return Options;
}

std::string affineNumericLoop(unsigned Bits, unsigned Advance = 0) {
  const std::string T = "i" + std::to_string(Bits);
  return "define i32 @f(" + T + R"( %root, i32 %x, i1 %again) {
  entry:
    %initial = sub )" +
         T + R"( %root, 6
    br label %loop
  loop:
    %address = phi )" +
         T + R"( [ %initial, %entry ], [ %back, %latch ]
    %offset = add )" +
         T + R"( %address, 13
    %p = inttoptr )" +
         T + R"( %offset to ptr
    %direct = add )" +
         T + R"( %root, 7
    %q = inttoptr )" +
         T + R"( %direct to ptr
    store i32 %x, ptr %p, align 1
    %v = load i32, ptr %q, align 1
    store i32 19, ptr %q, align 1
    br label %latch
  latch:
    %step = add )" +
         T + R"( %address, 11
    %back = sub )" +
         T + " %step, " + std::to_string(11 - Advance) + R"(
    br i1 %again, label %loop, label %exit
  exit:
    ret i32 %v
  })";
}

TEST(ByteMemoryForwarding, NumericLoopAddressEqualityEnablesLocalForwarding) {
  for (unsigned Bits : {32u, 64u}) {
    for (llvm::StringRef Endian : {"e", "E"}) {
      llvm::LLVMContext C;
      auto M = parse(C, affineNumericLoop(Bits),
                     Endian.str() + "-p:" + std::to_string(Bits) + ":" +
                         std::to_string(Bits));
      ASSERT_TRUE(M);
      auto &F = *M->getFunction("f");
      auto R = forward(F, numericOptions());
      EXPECT_EQ(R.ForwardedLoads, 1u) << print(F);
      EXPECT_EQ(R.RemovedStores, 1u);
      EXPECT_EQ(count<llvm::StoreInst>(F), 1u);
      EXPECT_EQ(count<llvm::LoadInst>(F), 0u);
      EXPECT_EQ(llvm::cast<llvm::ReturnInst>(F.back().getTerminator())
                    ->getReturnValue(),
                F.getArg(1));
    }
  }
}

TEST(ByteMemoryForwarding, NumericLoopBackedgeCanInvalidateTheEntryRelation) {
  llvm::LLVMContext C;
  auto M = parse(C, affineNumericLoop(64, 1));
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  const auto Before = print(F);
  auto R = forward(F, numericOptions());
  EXPECT_EQ(R.ForwardedLoads, 0u);
  EXPECT_EQ(R.RemovedStores, 0u);
  EXPECT_EQ(print(F), Before);
}

TEST(ByteMemoryForwarding, NumericPointerPhiAndSelectKeepEveryIncomingEdge) {
  for (unsigned Delta : {0u, 1u}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
      define i16 @f(i64 %root, i16 %x, i1 %again, i1 %choose) {
      entry:
        %p = inttoptr i64 %root to ptr
        br label %loop
      loop:
        %merged = phi ptr [ %p, %entry ], [ %back, %loop ]
        %selected = select i1 %choose, ptr %merged, ptr %p
        %shifted = getelementptr i8, ptr %selected, i64 3
        %back = getelementptr i8, ptr %shifted, i64 )" +
                          std::to_string(int(Delta) - 3) + R"(
        store i16 %x, ptr %selected, align 1
        %v = load i16, ptr %p, align 1
        br i1 %again, label %loop, label %exit
      exit:
        ret i16 %v
      })");
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, Delta == 0 ? 1u : 0u) << print(F);
    EXPECT_EQ(count<llvm::StoreInst>(F), 1u);
  }
}

std::string guardedNumericAddress(unsigned Bits, llvm::StringRef Op, int Mask,
                                  int Delta, llvm::StringRef Predicate = "eq",
                                  llvm::StringRef Input = "%root",
                                  unsigned GuardMask = 15) {
  const std::string T = "i" + std::to_string(Bits);
  return "define i64 @f(" + T + " %root, " + T + R"( %other, i64 %x) {
    %bits = and )" +
         T + " " + Input.str() + ", " + std::to_string(GuardMask) + R"(
    %ok = icmp )" +
         Predicate.str() + " " + T + R"( %bits, 8
    br i1 %ok, label %body, label %exit
  body:
    %masked = )" +
         Op.str() + " " + T + " %root, " + std::to_string(Mask) + R"(
    %shifted = add )" +
         T + " %root, " + std::to_string(Delta) + R"(
    %p = inttoptr )" +
         T + R"( %shifted to ptr
    %q = inttoptr )" +
         T + R"( %masked to ptr
    store i64 %x, ptr %p, align 1
    %seen = load i64, ptr %q, align 1
    ret i64 %seen
  exit:
    ret i64 0
  })";
}

TEST(ByteMemoryForwarding, NumericBitwiseDisplacementsNeedDominatingGuards) {
  struct Operation {
    const char *Name;
    int Mask;
    int Delta;
  };
  for (auto Op : {Operation{"and", -16, -8}, Operation{"or", 3, 3},
                  Operation{"xor", 15, -1}}) {
    for (unsigned Bits : {32u, 64u}) {
      for (bool Little : {false, true}) {
        llvm::LLVMContext C;
        auto M =
            parse(C, guardedNumericAddress(Bits, Op.Name, Op.Mask, Op.Delta),
                  (Little ? "e-p:" : "E-p:") + std::to_string(Bits) + ":" +
                      std::to_string(Bits));
        ASSERT_TRUE(M);
        auto &F = *M->getFunction("f");
        auto R = forward(F, numericOptions());
        EXPECT_EQ(R.CanonicalizedAddresses, 1u) << print(F);
        EXPECT_EQ(R.ForwardedLoads, 1u) << print(F);
        EXPECT_EQ(count<llvm::LoadInst>(F), 0u);
      }
    }
  }
}

TEST(ByteMemoryForwarding, NumericAddressGuardsDoNotEscapeTheirScope) {
  for (auto Body : {
           guardedNumericAddress(64, "and", -16, -8, "ne"),
           guardedNumericAddress(64, "and", -16, -8, "eq", "%other"),
           guardedNumericAddress(64, "and", -16, -8, "eq", "%root", 7),
           guardedNumericAddress(64, "and", -32, -8),
       }) {
    llvm::LLVMContext C;
    auto M = parse(C, Body);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.CanonicalizedAddresses, 0u) << print(F);
    EXPECT_EQ(R.ForwardedLoads, 0u);
  }
  // Both outcomes reach the join; the equality edge does not dominate it.
  std::string Body = guardedNumericAddress(64, "and", -16, -8);
  const std::string Old = "label %body, label %exit\n  body:";
  Body.replace(Body.find(Old), Old.size(),
               "label %left, label %right\n"
               "  left: br label %body\n  right: br label %body\n  body:");
  llvm::LLVMContext C;
  auto M = parse(C, Body);
  ASSERT_TRUE(M);
  EXPECT_EQ(forward(*M->getFunction("f"), numericOptions()).ForwardedLoads, 0u);
}

TEST(ByteMemoryForwarding, NumericGuardDiscoveryHasFiniteAtomicProofWork) {
  const auto Body = guardedNumericAddress(64, "and", -16, -8);
  llvm::LLVMContext C;
  auto M = parse(C, Body);
  ASSERT_TRUE(M);
  auto Golden = forward(*M->getFunction("f"), numericOptions());
  ASSERT_EQ(Golden.ForwardedLoads, 1u);
  for (uint64_t Limit = 0; Limit <= Golden.AddressSteps; ++Limit) {
    llvm::LLVMContext Local;
    auto Copy = parse(Local, Body);
    ASSERT_TRUE(Copy);
    auto Options = numericOptions();
    Options.MaxAddressSteps = Limit;
    auto R = forward(*Copy->getFunction("f"), Options);
    EXPECT_LE(R.AddressSteps, Limit);
    EXPECT_EQ(R.BudgetExhausted, Limit < Golden.AddressSteps);
    if (Limit < 6)
      EXPECT_EQ(R.CanonicalizedAddresses, 0u);
  }
}

TEST(ByteMemoryForwarding, NumericGuardedRuntimePreservesBothOutcomesAndBytes) {
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    llvm::LLVMContext C;
    const char *Ops[] = {"and", "or", "xor"};
    const int Masks[] = {-16, 3, 15}, Deltas[] = {-8, 3, -1};
    auto M = parse(
        C, guardedNumericAddress(64, Ops[Mode], Masks[Mode], Deltas[Mode]));
    ASSERT_TRUE(M);
    llvm::ValueToValueMapTy Map;
    auto *Copy = llvm::CloneFunction(M->getFunction("f"), Map);
    Copy->setName("forwarded");
    ASSERT_EQ(forward(*Copy, numericOptions()).ForwardedLoads, 1u);
#ifdef NEVERD_TEST_CLANG
    const std::string Compiler = NEVERD_TEST_CLANG;
#else
    auto Program = llvm::sys::findProgramByName("clang");
    ASSERT_TRUE(bool(Program));
    const std::string Compiler = *Program;
#endif
    llvm::SmallString<128> IR, Source, Binary, Error;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-address-guard", "ll", IR));
    llvm::FileRemover RemoveIR(IR);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-address-guard", "c",
                                                    Source));
    llvm::FileRemover RemoveSource(Source);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-address-guard",
                                                    "exe", Binary));
    llvm::FileRemover RemoveBinary(Binary);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-address-guard",
                                                    "err", Error));
    llvm::FileRemover RemoveError(Error);
    std::error_code EC;
    {
      llvm::raw_fd_ostream OS(IR, EC);
      ASSERT_FALSE(EC);
      M->print(OS, nullptr);
    }
    {
      llvm::raw_fd_ostream OS(Source, EC);
      ASSERT_FALSE(EC);
      OS << "#define DELTA " << Deltas[Mode] << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t f(uintptr_t, uintptr_t, uint64_t);
extern uint64_t forwarded(uintptr_t, uintptr_t, uint64_t);
int main(void) {
  uint64_t random = UINT64_C(0x643abcdef1872359);
  for (unsigned k = 0; k != 4096; ++k) {
    random ^= random << 13; random ^= random >> 7; random ^= random << 17;
    uint64_t x = k == 0 ? 0 : k == 1 ? UINT64_MAX : random;
    _Alignas(16) unsigned char a[64], b[64], expected[64];
    for (unsigned j = 0; j != 64; ++j) a[j] = b[j] = expected[j] = k + 11*j;
    unsigned residue = k % 16;
    uint64_t answer = residue == 8 ? x : 0;
    if (residue == 8) memcpy(expected + 24 + DELTA, &x, 8);
    if (f((uintptr_t)(a+16+residue), 0, x) != answer ||
        forwarded((uintptr_t)(b+16+residue), 0, x) != answer) return 1;
    if (memcmp(a, expected, 64) || memcmp(b, expected, 64)) return 2;
  }
  return 0;
})";
    }
    for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
      const std::optional<llvm::StringRef> Redirects[] = {
          std::nullopt, std::nullopt, Error.str()};
      int Exit = llvm::sys::ExecuteAndWait(
          Compiler, {Compiler, Optimization, IR, Source, "-o", Binary},
          std::nullopt, Redirects, 30);
      auto Errors = llvm::MemoryBuffer::getFile(Error);
      ASSERT_EQ(Exit, 0) << (Errors ? (*Errors)->getBuffer().str() : "");
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Binary, {Binary}, std::nullopt,
                                          Redirects, 30),
                0);
    }
  }
}

TEST(ByteMemoryForwarding, NumericLoopDoesNotAssumeUndefinedOrFrozenEdges) {
  for (llvm::StringRef Back : {"undef", "poison", "%frozen"}) {
    llvm::LLVMContext C;
    std::string Body = affineNumericLoop(64);
    Body.replace(Body.find("[ %back, %latch ]"), 17,
                 "[ " + Back.str() + ", %latch ]");
    Body.insert(Body.find("    br i1 %again"),
                "    %frozen = freeze i64 %back\n");
    auto M = parse(C, Body);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    const auto Before = print(F);
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, 0u) << Back.str();
    EXPECT_EQ(R.RemovedStores, 0u);
    EXPECT_EQ(print(F), Before);
  }
}

TEST(ByteMemoryForwarding, NumericAffineAddressBudgetsDoNotUsePartialProofs) {
  llvm::LLVMContext C;
  auto M = parse(C, affineNumericLoop(64));
  ASSERT_TRUE(M);
  auto Golden = forward(*M->getFunction("f"), numericOptions());
  ASSERT_EQ(Golden.CanonicalizedAddresses, 1u);
  ASSERT_EQ(Golden.NewInstructions, 2u);
  for (uint64_t Limit : {uint64_t(0), uint64_t(1), uint64_t(2)}) {
    llvm::LLVMContext Local;
    auto Copy = parse(Local, affineNumericLoop(64));
    ASSERT_TRUE(Copy);
    auto &F = *Copy->getFunction("f");
    const auto Before = print(F);
    auto Options = numericOptions();
    Options.MaxNewInstructions = Limit;
    auto R = forward(F, Options);
    EXPECT_LE(R.NewInstructions, Limit);
    EXPECT_EQ(R.CanonicalizedAddresses, Limit == 2 ? 1u : 0u);
    EXPECT_EQ(R.ForwardedLoads, Limit == 2 ? 1u : 0u);
    if (Limit < 2)
      EXPECT_EQ(print(F), Before);
  }
  for (uint64_t Limit :
       {uint64_t(0), Golden.AddressSteps - 1, Golden.AddressSteps}) {
    llvm::LLVMContext Local;
    auto Copy = parse(Local, affineNumericLoop(64));
    ASSERT_TRUE(Copy);
    auto Options = numericOptions();
    Options.MaxAddressSteps = Limit;
    auto R = forward(*Copy->getFunction("f"), Options);
    EXPECT_LE(R.AddressSteps, Limit);
    EXPECT_EQ(R.BudgetExhausted, Limit < Golden.AddressSteps);
  }
}

TEST(ByteMemoryForwarding, NumericAddressRewriteAloneInvalidatesAnalyses) {
  llvm::LLVMContext C;
  auto M = parse(C, affineNumericLoop(64));
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  // Keep the address rewrite but forbid the subsequent memory scan.
  auto Options = numericOptions();
  Options.MaxMemorySteps = 0;
  llvm::FunctionAnalysisManager FAM;
  EXPECT_FALSE(ByteMemoryForwardingPass(Options).run(F, FAM).areAllPreserved());
  EXPECT_EQ(count<llvm::LoadInst>(F), 1u);
  EXPECT_EQ(count<llvm::StoreInst>(F), 2u);
  EXPECT_NE(print(F).find("numeric.address"), std::string::npos);
  EXPECT_FALSE(llvm::verifyFunction(F, &llvm::errs()));
}

TEST(ByteMemoryForwarding,
     NumericAddressAnalysisDoesNotPublishUnanchoredCycles) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define void @f() {
    entry:
      ret void
    unreachable:
      %address = phi i64 [ %next, %unreachable ]
      %next = add i64 %address, 0
      %p = inttoptr i64 %address to ptr
      store i32 7, ptr %p, align 1
      br label %unreachable
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  const auto Before = print(F);
  auto R = forward(F, numericOptions());
  EXPECT_EQ(R.CanonicalizedAddresses, 0u);
  EXPECT_FALSE(R.BudgetExhausted);
  EXPECT_EQ(print(F), Before);
}

TEST(ByteMemoryForwarding,
     NumericLoopRuntimeChecksEveryIterationAndMemoryByte) {
  for (unsigned Advance : {0u, 1u}) {
    llvm::LLVMContext C;
    std::string Body = affineNumericLoop(64, Advance);
    Body.replace(Body.find("i1 %again"), 9, "i32 %count");
    Body.insert(Body.find("    %address ="),
                "    %index = phi i32 [ 0, %entry ], [ %next, %latch ]\n");
    Body.insert(Body.find("    br i1 %again"),
                "    %next = add i32 %index, 1\n"
                "    %again = icmp ult i32 %next, %count\n");
    auto M = parse(C, Body);
    ASSERT_TRUE(M);
    llvm::ValueToValueMapTy Map;
    auto *Copy = llvm::CloneFunction(M->getFunction("f"), Map);
    Copy->setName("forwarded");
    auto R = forward(*Copy, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, Advance == 0 ? 1u : 0u);
#ifdef NEVERD_TEST_CLANG
    const std::string Compiler = NEVERD_TEST_CLANG;
#else
    auto Program = llvm::sys::findProgramByName("clang");
    ASSERT_TRUE(bool(Program));
    const std::string Compiler = *Program;
#endif
    llvm::SmallString<128> IR, Source, Binary, Error;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-loop-address", "ll", IR));
    llvm::FileRemover RemoveIR(IR);
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-loop-address", "c", Source));
    llvm::FileRemover RemoveSource(Source);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-loop-address",
                                                    "exe", Binary));
    llvm::FileRemover RemoveBinary(Binary);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-loop-address",
                                                    "err", Error));
    llvm::FileRemover RemoveError(Error);
    std::error_code EC;
    {
      llvm::raw_fd_ostream OS(IR, EC);
      ASSERT_FALSE(EC);
      M->print(OS, nullptr);
    }
    {
      llvm::raw_fd_ostream OS(Source, EC);
      ASSERT_FALSE(EC);
      OS << "#define ADVANCE " << Advance << R"(
#include <stdint.h>
#include <string.h>
extern uint32_t f(uintptr_t, uint32_t, uint32_t);
extern uint32_t forwarded(uintptr_t, uint32_t, uint32_t);
int main(void) {
  uint32_t random = UINT32_C(0x165b973d);
  for (unsigned k = 0; k != 4096; ++k) {
    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
    uint32_t x = k == 0 ? 0 : k == 1 ? UINT32_MAX : random;
    unsigned char a[64], b[64], expected[64];
    for (unsigned j = 0; j != 64; ++j) a[j] = b[j] = expected[j] = k + j * 11;
    uint32_t count = 1 + k % 16, answer = 0, last = 19;
    for (unsigned i = 0; i != count; ++i) {
      memcpy(expected + 15 + i * ADVANCE, &x, 4);
      memcpy(&answer, expected + 15, 4);
      memcpy(expected + 15, &last, 4);
    }
    if (f((uintptr_t)(a+8), x, count) != answer ||
        forwarded((uintptr_t)(b+8), x, count) != answer) return 1;
    if (memcmp(a, expected, 64) || memcmp(b, expected, 64)) return 2;
  }
  return 0;
})";
    }
    for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
      const std::optional<llvm::StringRef> Redirects[] = {
          std::nullopt, std::nullopt, Error.str()};
      int Exit = llvm::sys::ExecuteAndWait(
          Compiler, {Compiler, Optimization, IR, Source, "-o", Binary},
          std::nullopt, Redirects, 30);
      auto Errors = llvm::MemoryBuffer::getFile(Error);
      ASSERT_EQ(Exit, 0) << (Errors ? (*Errors)->getBuffer().str() : "");
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Binary, {Binary}, std::nullopt,
                                          Redirects, 30),
                0);
    }
  }
}

TEST(ByteMemoryForwarding, NumericSingleWriterCoercionNeedsNoSnapshot) {
  for (llvm::StringRef Stored : {"%x", "undef", "poison", "%frozen"}) {
    for (bool Little : {false, true}) {
      for (unsigned PointerBits : {32u, 64u}) {
        SCOPED_TRACE(::testing::Message()
                     << Stored.str() << Little << PointerBits);
        llvm::LLVMContext C;
        const std::string P = "i" + std::to_string(PointerBits);
        auto M =
            parse(C,
                  "define i16 @f(" + P + R"( %root, i64 %x) {
          %frozen = freeze i64 %x
          %start = sub )" +
                      P + R"( %root, 2
          %p = inttoptr )" +
                      P + R"( %start to ptr
          store i64 )" +
                      Stored.str() + R"(, ptr %p, align 1
          %q = inttoptr )" +
                      P + R"( %root to ptr
          %v = load i16, ptr %q, align 1
          ret i16 %v
        })",
                  (Little ? "e-p:" : "E-p:") + std::to_string(PointerBits) +
                      ":" + std::to_string(PointerBits));
        ASSERT_TRUE(M);
        auto &F = *M->getFunction("f");
        auto R = forward(F, numericOptions());
        EXPECT_EQ(R.ForwardedLoads, 1u) << print(F);
        EXPECT_EQ(R.FrozenStores, 0u);
        EXPECT_EQ(count<llvm::FreezeInst>(F), 1u);
        EXPECT_EQ(count<llvm::StoreInst>(F), 1u);
        EXPECT_EQ(R.RemovedStores, 0u);
        auto *Return = llvm::cast<llvm::ReturnInst>(F.back().getTerminator());
        auto *Trunc = llvm::cast<llvm::TruncInst>(Return->getReturnValue());
        auto *Shift = llvm::cast<llvm::BinaryOperator>(Trunc->getOperand(0));
        EXPECT_EQ(
            llvm::cast<llvm::ConstantInt>(Shift->getOperand(1))->getZExtValue(),
            Little ? 16u : 32u);
      }
    }
  }
}

constexpr llvm::StringLiteral NumericOverwrite = R"(
define i32 @f(i64 %root, i32 %x, i64 %y) {
  %base = sub i64 %root, 2
  %p = inttoptr i64 %base to ptr
  store i32 %x, ptr %p, align 1
  %q0 = add i64 -2, %root
  %q = inttoptr i64 %q0 to ptr
  %v = load i32, ptr %q, align 1
  store i64 %y, ptr %q, align 1
  ret i32 %v
})";

TEST(ByteMemoryForwarding, NumericOverwritePreservesAForwardedObservation) {
  llvm::LLVMContext C;
  auto M = parse(C, NumericOverwrite);
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  auto R = forward(F, numericOptions());
  EXPECT_EQ(R.ForwardedLoads, 1u);
  EXPECT_EQ(R.RemovedStores, 1u);
  EXPECT_EQ(count<llvm::StoreInst>(F), 1u);
  EXPECT_EQ(count<llvm::LoadInst>(F), 0u);
  EXPECT_EQ(
      llvm::cast<llvm::ReturnInst>(F.back().getTerminator())->getReturnValue(),
      F.getArg(1));
}

TEST(ByteMemoryForwarding, NumericPartialOverwriteAcrossModularOriginStays) {
  for (llvm::StringRef Layout : {"e-p:64:64", "E-p:64:64"}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
      define i32 @f(i64 %root, i32 %x) {
        %a = sub i64 %root, 2
        %p = inttoptr i64 %a to ptr
        %q = inttoptr i64 %root to ptr
        store i32 %x, ptr %p, align 1
        store i8 19, ptr %q, align 1
        %v = load i32, ptr %p, align 1
        ret i32 %v
      })",
                   Layout);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, 0u);
    EXPECT_EQ(R.RemovedStores, 0u);
    EXPECT_EQ(count<llvm::StoreInst>(F), 2u);
  }
}

TEST(ByteMemoryForwarding, NumericDisjointReadKeepsOverwriteEvidence) {
  for (unsigned Bits : {32u, 64u}) {
    for (bool Little : {false, true}) {
      llvm::LLVMContext C;
      const std::string T = "i" + std::to_string(Bits);
      auto M = parse(C,
                     "define i32 @f(" + T + R"( %root, i64 %x) {
        %a = sub )" + T + R"( %root, 4
        %p = inttoptr )" +
                         T + R"( %a to ptr
        %b = add )" + T + R"( %root, 4
        %q = inttoptr )" +
                         T + R"( %b to ptr
        store i64 %x, ptr %p, align 1
        %seen = load i32, ptr %q, align 1
        store i64 19, ptr %p, align 1
        ret i32 %seen
      })",
                     (Little ? "e-p:" : "E-p:") + std::to_string(Bits) + ":" +
                         std::to_string(Bits));
      ASSERT_TRUE(M);
      auto &F = *M->getFunction("f");
      auto R = forward(F, numericOptions());
      EXPECT_EQ(R.ForwardedLoads, 0u);
      EXPECT_EQ(R.RemovedStores, 1u) << print(F);
      EXPECT_EQ(count<llvm::LoadInst>(F), 1u);
    }
  }
}

TEST(ByteMemoryForwarding, NumericSeveralWritesCanKillOneEarlierStore) {
  for (bool Little : {false, true}) {
    for (bool Complete : {false, true}) {
      llvm::LLVMContext C;
      auto M = parse(C,
                     R"(
        define void @f(i64 %root, i64 %x) {
          %a = sub i64 %root, 4
          %p = inttoptr i64 %a to ptr
          %q = inttoptr i64 %root to ptr
          store i64 %x, ptr %p, align 1
          store i32 11, ptr %p, align 1
          store )" + std::string(Complete ? "i32" : "i16") +
                         R"( 23, ptr %q, align 1
          ret void
        })",
                     Little ? "e-p:64:64" : "E-p:64:64");
      ASSERT_TRUE(M);
      auto &F = *M->getFunction("f");
      EXPECT_EQ(forward(F, numericOptions()).RemovedStores, Complete ? 1u : 0u)
          << print(F);
    }
  }
}

TEST(ByteMemoryForwarding, NumericPartialObservationKeepsTheWholeWriter) {
  // The surviving read contains one byte from the first store and one byte
  // from outside it. A later overwrite must not erase the observed writer.
  for (int ReadOffset : {-1, 7}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
      define i16 @f(i64 %root, i64 %x) {
        %p = inttoptr i64 %root to ptr
        %a = add i64 %root, )" +
                          std::to_string(ReadOffset) + R"(
        %q = inttoptr i64 %a to ptr
        store i64 %x, ptr %p, align 1
        %seen = load i16, ptr %q, align 1
        store i64 19, ptr %p, align 1
        ret i16 %seen
      })");
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, 0u);
    EXPECT_EQ(R.RemovedStores, 0u) << print(F);
  }
}

TEST(ByteMemoryForwarding, NumericUnknownObservationsPreventDeletion) {
  for (llvm::StringRef Barrier :
       {"%seen = load i8, ptr %other", "store i8 9, ptr %other",
        "%seen = load volatile i8, ptr %p", "fence seq_cst",
        "call void @may_throw()", "call void @observer(ptr %p)"}) {
    SCOPED_TRACE(Barrier.str());
    llvm::LLVMContext C;
    auto M = parse(C, R"(
      declare void @may_throw() memory(none)
      declare void @observer(ptr)
      define i32 @f(i64 %root, ptr %other, i32 %x) {
        %p = inttoptr i64 %root to ptr
        store i32 %x, ptr %p, align 1
      )" + Barrier.str() + R"(
        %v = load i32, ptr %p, align 1
        store i32 17, ptr %p, align 1
        ret i32 %v
      })");
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    EXPECT_EQ(forward(F, numericOptions()).RemovedStores, 0u) << print(F);
  }
}

TEST(ByteMemoryForwarding, NumericAndAllocaCachesCannotAssumeDisjointness) {
  for (bool NumericFirst : {false, true}) {
    llvm::LLVMContext C;
    auto M = parse(
        C, R"(
      define i32 @f(i32 %x, i32 %y) {
        %frame = alloca [8 x i8]
        %root = ptrtoint ptr %frame to i64
        %p = inttoptr i64 %root to ptr
      )" +
               std::string(NumericFirst
                               ? "store i32 %x, ptr %p\n store i32 %y, ptr "
                                 "%frame\n %v = load i32, ptr %p"
                               : "store i32 %x, ptr %frame\n store i32 %y, ptr "
                                 "%p\n %v = load i32, ptr %frame") +
               R"(
        ret i32 %v
      })");
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, 0u);
    EXPECT_EQ(R.RemovedStores, 0u);
  }
}

TEST(ByteMemoryForwarding, NumericBudgetsDoNotEraseAnUnforwardedObservation) {
  for (unsigned Limit : {0u, 1u, 2u}) {
    llvm::LLVMContext C;
    auto M = parse(C, NumericOverwrite);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto Options = numericOptions();
    Options.MaxUseSteps = Limit;
    auto R = forward(F, Options);
    EXPECT_EQ(R.ForwardedLoads, Limit == 2 ? 1u : 0u);
    EXPECT_EQ(R.RemovedStores, Limit == 2 ? 1u : 0u);
    EXPECT_EQ(count<llvm::StoreInst>(F), Limit == 2 ? 1u : 2u);
    EXPECT_EQ(R.BudgetExhausted, Limit != 2)
        << "limit=" << Limit << " uses=" << R.UseSteps
        << " memory=" << R.MemorySteps << " addresses=" << R.AddressSteps
        << " instructions=" << R.Instructions << " new=" << R.NewInstructions;
  }
}

TEST(ByteMemoryForwarding, NumericDeletionAloneInvalidatesAnalyses) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define void @f(i64 %root) {
      %p = inttoptr i64 %root to ptr
      store i32 7, ptr %p
      store i32 9, ptr %p
      ret void
    })");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  llvm::FunctionAnalysisManager FAM;
  EXPECT_FALSE(
      ByteMemoryForwardingPass(numericOptions()).run(F, FAM).areAllPreserved());
  EXPECT_EQ(count<llvm::StoreInst>(F), 1u);
}

TEST(ByteMemoryForwarding, NumericPointerWidthsAndBlockBoundariesStayExact) {
  for (llvm::StringRef Layout : {"e-p:64:64:64:32", "e-p:32:32"}) {
    llvm::LLVMContext C;
    auto M = parse(C, NumericOverwrite, Layout);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    const auto Before = print(F);
    auto R = forward(F, numericOptions());
    EXPECT_EQ(R.ForwardedLoads, 0u);
    EXPECT_EQ(R.RemovedStores, 0u);
    EXPECT_EQ(print(F), Before);
  }
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i32 @f(i64 %root, i32 %x, i1 %again) {
      %p = inttoptr i64 %root to ptr
      store i32 %x, ptr %p
      br label %loop
    loop:
      %v = load i32, ptr %p
      store i32 11, ptr %p
      br i1 %again, label %loop, label %exit
    exit:
      ret i32 %v
    })");
  ASSERT_TRUE(M);
  auto R = forward(*M->getFunction("f"), numericOptions());
  EXPECT_EQ(R.ForwardedLoads, 0u);
  EXPECT_EQ(R.RemovedStores, 0u);
}

TEST(ByteMemoryForwarding, NumericMemoryWorkIsChargedAtAdjacentLimits) {
  llvm::LLVMContext C;
  auto M = parse(C, NumericOverwrite);
  ASSERT_TRUE(M);
  const auto Golden = forward(*M->getFunction("f"), numericOptions());
  ASSERT_GT(Golden.MemorySteps, 0u);
  for (uint64_t Limit :
       {uint64_t(0), Golden.MemorySteps - 1, Golden.MemorySteps}) {
    llvm::LLVMContext Local;
    auto Copy = parse(Local, NumericOverwrite);
    ASSERT_TRUE(Copy);
    auto Options = numericOptions();
    Options.MaxMemorySteps = Limit;
    auto R = forward(*Copy->getFunction("f"), Options);
    EXPECT_LE(R.MemorySteps, Limit);
    EXPECT_EQ(R.BudgetExhausted, Limit < Golden.MemorySteps);
    EXPECT_GE(count<llvm::StoreInst>(*Copy->getFunction("f")), 1u);
  }
}

TEST(ByteMemoryForwarding, NumericRuntimeChecksReturnAndEntireAliasedBuffer) {
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    llvm::LLVMContext C;
    auto M = parse(C, R"(
    define i64 @mix(ptr %buffer, i64 %other, i64 %x) {
      %alias = inttoptr i64 %other to ptr
      %root = ptrtoint ptr %buffer to i64
      %back = sub i64 %root, 2
      %p = inttoptr i64 %back to ptr
      %q = inttoptr i64 %root to ptr
      store i64 99, ptr %p, align 1
      %far = add i64 %root, 12
      %farptr = inttoptr i64 %far to ptr
      %farvalue = load i32, ptr %farptr, align 1
      store i64 %x, ptr %p, align 1
      %early = load i32, ptr %p, align 1
      store i8 29, ptr %q, align 1
      %slice = load i16, ptr %q, align 1
      %mid = xor i64 %x, 81985529216486895
      store i64 %mid, ptr %p, align 1
      store i8 73, ptr %alias, align 1
      %observed = load i8, ptr %p, align 1
      %final = add i64 %x, 1
      store i64 %final, ptr %p, align 1
      %tail = add i64 %root, 16
      %tailptr = inttoptr i64 %tail to ptr
      %upper = add i64 %root, 20
      %upperptr = inttoptr i64 %upper to ptr
      store i64 %x, ptr %tailptr, align 1
      %lo = trunc i64 %final to i32
      %shift = lshr i64 %final, 32
      %hi = trunc i64 %shift to i32
      store i32 %lo, ptr %tailptr, align 1
      store i32 %hi, ptr %upperptr, align 1
      %a = zext i32 %early to i64
      %b = zext i16 %slice to i64
      %c = zext i8 %observed to i64
      %ab = xor i64 %a, %b
      %abc = xor i64 %ab, %c
      %d = zext i32 %farvalue to i64
      %result = xor i64 %abc, %d
      ret i64 %result
    })");
    ASSERT_TRUE(M);
    llvm::ValueToValueMapTy Map;
    auto *Rewritten = llvm::CloneFunction(M->getFunction("mix"), Map);
    Rewritten->setName("forwarded");
    if (Mode == 0) {
      auto R = forward(*Rewritten, numericOptions());
      EXPECT_EQ(R.ForwardedLoads, 1u);
      EXPECT_EQ(R.RemovedStores, 2u);
    } else {
      Pipeline::OptimizationOptions Options;
      Options.Strength =
          Mode == 1 ? Pipeline::OptStrength::Thin : Pipeline::OptStrength::Deep;
      const auto R = Pipeline::optimizeModule(*M, Options);
      EXPECT_NE(R.Stop, OptimizationStopReason::InputInvalid);
      EXPECT_NE(R.Stop, OptimizationStopReason::VerificationFailed);
      ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
    }
#ifdef NEVERD_TEST_CLANG
    const std::string Compiler = NEVERD_TEST_CLANG;
#else
    auto Program = llvm::sys::findProgramByName("clang");
    ASSERT_TRUE(bool(Program));
    const std::string Compiler = *Program;
#endif
    llvm::SmallString<128> IR, Source, Binary, Error;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-numeric-memory", "ll", IR));
    llvm::FileRemover RemoveIR(IR);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-numeric-memory",
                                                    "c", Source));
    llvm::FileRemover RemoveSource(Source);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-numeric-memory",
                                                    "exe", Binary));
    llvm::FileRemover RemoveBinary(Binary);
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-numeric-memory",
                                                    "err", Error));
    llvm::FileRemover RemoveError(Error);
    std::error_code EC;
    {
      llvm::raw_fd_ostream OS(IR, EC);
      ASSERT_FALSE(EC);
      M->print(OS, nullptr);
    }
    {
      llvm::raw_fd_ostream OS(Source, EC);
      ASSERT_FALSE(EC);
      OS << R"(
#include <stdint.h>
#include <string.h>
extern uint64_t mix(void *, uintptr_t, uint64_t);
extern uint64_t forwarded(void *, uintptr_t, uint64_t);
int main(void) {
  uint64_t state = UINT64_C(0x532871ac943dbef1);
  for (unsigned k = 0; k != 8192; ++k) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    uint64_t x = k == 0 ? 0 : k == 1 ? UINT64_MAX : state;
    unsigned alias = k % 32;
    unsigned char a[32], b[32], expected[32];
    for (unsigned j = 0; j != 32; ++j) a[j] = b[j] = expected[j] = (k + j * 7);
    uint32_t farvalue = 0;
    for (unsigned j = 0; j != 4; ++j) farvalue |= (uint32_t)expected[20+j] << (8*j);
    uint64_t middle = x ^ UINT64_C(81985529216486895);
    unsigned observed = alias == 6 ? 73 : (unsigned char)middle;
    expected[alias] = 73;
    uint64_t answer = (uint32_t)x ^ (29u | ((x >> 16) & 65280u)) ^ observed ^ farvalue;
    for (unsigned j = 0; j != 8; ++j) expected[6+j] = expected[24+j] = (unsigned char)((x+1) >> (8*j));
    if (mix(a+8, (uintptr_t)(a+alias), x) != answer || forwarded(b+8, (uintptr_t)(b+alias), x) != answer)
      return 1;
    if (memcmp(a, expected, 32) || memcmp(b, expected, 32)) return 2;
  }
  return 0;
})";
    }
    for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
      const std::optional<llvm::StringRef> Redirects[] = {
          std::nullopt, std::nullopt, Error.str()};
      const int Exit = llvm::sys::ExecuteAndWait(
          Compiler, {Compiler, Optimization, IR, Source, "-o", Binary},
          std::nullopt, Redirects, 30);
      auto Errors = llvm::MemoryBuffer::getFile(Error);
      ASSERT_EQ(Exit, 0) << (Errors ? (*Errors)->getBuffer().str() : "");
      EXPECT_EQ(llvm::sys::ExecuteAndWait(Binary, {Binary}, std::nullopt,
                                          Redirects, 30),
                0);
    }
  }
}

} // namespace
