//===- LLVMInterpreterModelRejectionTests.cpp - Unsupported source inputs
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace neverd::analysis::llvm_model_test {
TEST_F(LLVMModel, UnsupportedInstructionsAndConstantsFailClearly) {
  const char *Bodies[] = {
      "ret i64 undef",
      "ret i64 poison",
      "%x = freeze i64 1\nret i64 %x",
      "%x = udiv i64 9, 3\nret i64 %x",
      "%x = srem i64 9, 3\nret i64 %x",
      "%x = shl i64 1, 64\nret i64 %x",
      "%x = shl i1 true, false\n%v = zext i1 %x to i64\nret i64 %v",
      "%x = add i1 true, true\n%v = zext i1 %x to i64\nret i64 %v",
      "%x = icmp slt i1 true, false\n%v = zext i1 %x to i64\nret i64 %v",
      "%x = trunc i128 123456789012345678901234567890 to i64\nret i64 %x",
      "%x = fadd double 1.0, 2.0\nret i64 0",
      "%x = add <2 x i64> zeroinitializer, zeroinitializer\nret i64 0",
      "%x = alloca i64\nret i64 0",
      "unreachable"};
  for (const char *Body : Bodies) {
    SCOPED_TRACE(Body);
    parse(Body);
    ASSERT_TRUE(Module);
    reject();
  }
  parse("%x = call i64 @other()\nret i64 %x", "declare i64 @other()");
  ASSERT_TRUE(Module);
  reject();
}

TEST_F(LLVMModel, PointerEscapesAndUnprovedMemoryModesAreRefused) {
  const char *Bodies[] = {
      "%x = ptrtoint ptr %state to i64\nret i64 %x",
      "%p = getelementptr i8, ptr %state, i64 136\nret i64 0",
      "%p = getelementptr i8, ptr %state, i64 -1\nret i64 0",
      "%p = getelementptr i8, ptr %state, i64 132\n%v = load i64, ptr %p, "
      "align 1\nret i64 %v",
      "%p = getelementptr i128, ptr %state, i64 1\nret i64 0",
      "%p = getelementptr i8, ptr %state, i64 1\n%v = load i64, ptr %p, align "
      "8\nret i64 %v",
      "%x = load i64, ptr %state, align 16\nret i64 %x",
      "%x = load i64, ptr %state, align 4294967296\nret i64 %x",
      "%x = load i64, ptr %state\n%p = getelementptr i8, ptr %state, i64 "
      "%x\nret i64 0",
      "%p = getelementptr i8, ptr %state, i64 16\n%q = getelementptr nuw i8, "
      "ptr %p, i64 -8\nret i64 0",
      "%x = ptrtoint ptr %state to i64\n%v = add nuw i64 %x, 8\nret i64 0",
      "%x = ptrtoint ptr %state to i32\nret i64 0",
      "%p = inttoptr i32 3 to ptr\nret i64 0",
      "%x = load i1, ptr %state\nret i64 0",
      "store i1 true, ptr %state\nret i64 0",
      "%x = load ptr, ptr %state\nret i64 0",
      "store ptr %state, ptr %state\nret i64 0",
      "%x = load volatile i64, ptr %state\nret i64 %x",
      "store volatile i64 3, ptr %state\nret i64 0",
      "%x = load atomic i64, ptr %state monotonic, align 8\nret i64 %x",
      "store atomic i64 3, ptr %state monotonic, align 8\nret i64 0",
      "%x = load i64, ptr %state\n%p = inttoptr i64 %x to ptr\n%q = "
      "getelementptr i8, ptr %p, i64 1\nret i64 0",
      "%x = icmp eq ptr %state, %state\nret i64 0"};
  for (const char *Body : Bodies) {
    SCOPED_TRACE(Body);
    parse(Body);
    ASSERT_TRUE(Module);
    reject();
  }
}

TEST_F(LLVMModel, TypedStateProjectionsRefuseWrappedAndUnprovedOffsets) {
  for (const char *Flags : {"", "inbounds ", "nuw ", "nusw "}) {
    SCOPED_TRACE(Flags);
    parse("%p = getelementptr " + std::string(Flags) +
          "i64, ptr %state, i64 2305843009213693956\nret i64 0");
    ASSERT_TRUE(Module);
    reject("state GEP offset overflow");
  }
  for (const char *Body :
       {"%p = getelementptr i64, ptr %state, i64 17\nret i64 0",
        "%p = getelementptr i64, ptr %state, i64 -1\nret i64 0",
        "%p = getelementptr i32, ptr %state, i64 33\n"
        "%x = load i64, ptr %p, align 1\nret i64 %x",
        "%p = getelementptr i24, ptr %state, i64 1\nret i64 0",
        "%p = getelementptr [2 x i64], ptr %state, i64 0, i64 1\nret i64 0",
        "%p = getelementptr i64, ptr %state, i32 1\nret i64 0",
        "%x = load i64, ptr %state\n"
        "%p = getelementptr i64, ptr %state, i64 %x\nret i64 0",
        "%p = getelementptr i64, ptr %state, i64 4\n"
        "%q = getelementptr nuw i64, ptr %p, i64 -1\nret i64 0",
        "%x = load i64, ptr %state\n%p = inttoptr i64 %x to ptr\n"
        "%q = getelementptr i64, ptr %p, i64 1\nret i64 0"}) {
    SCOPED_TRACE(Body);
    parse(Body);
    ASSERT_TRUE(Module);
    reject();
  }
}

TEST_F(LLVMModel, FunctionAttributesAndMetadataCannotBecomeSilentAssumptions) {
  for (const char *Attributes :
       {"noreturn", "speculatable", "naked", "\"unknown-contract\"=\"true\"",
        "memory(none)", "memory(read)"}) {
    SCOPED_TRACE(Attributes);
    parse("store i64 5, ptr %state, align 8\nret i64 0", {}, Attributes);
    ASSERT_TRUE(Module);
    reject();
  }
  parse("%x = load i64, ptr %state, !range !0\nret i64 %x",
        "!0 = !{i64 0, i64 4}");
  ASSERT_TRUE(Module);
  reject();
  parse("ret i64 0", "!0 = !{i64 7}", "!custom !0");
  ASSERT_TRUE(Module);
  reject("function metadata");
  parse("ret i64 0, !custom !0", "!0 = !{i64 7}");
  ASSERT_TRUE(Module);
  reject("instruction metadata");
  parse("br label %loop\nloop:\nbr label %loop, !llvm.loop !0",
        "!0 = distinct !{!0, !1}\n!1 = !{!\"llvm.loop.parallel_accesses\"}");
  ASSERT_TRUE(Module);
  reject("loop metadata");
}

TEST_F(LLVMModel, PeelingHistoryKeepsStateEffectsAndMetadataBudgets) {
  parse("store i64 29, ptr %state, align 8\n"
        "br label %exit, !llvm.loop !0\nexit: ret i64 0",
        "!0 = distinct !{!0, !1}\n"
        "!1 = !{!\"llvm.loop.peeled.count\", i32 19}");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(0), {n(29)})}));
  expect(Oracle({op(NdOp::COPY, r(0), {n(30)})}), Status::Different);
  LLVMInterpreterModelLimits Limits;
  Limits.MaxInputItems = 0;
  auto Result = model(Limits);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("budget"),
            std::string::npos);
}

TEST_F(LLVMModel, UniformMutableAliasTagIsAcceptedButMixedTagsAreRefused) {
  const char *Metadata = R"(
    !0 = !{!1, !1, i64 0}
    !1 = !{!"word", !2, i64 0}
    !2 = !{!"root"}
    !3 = !{!4, !4, i64 0}
    !4 = !{!"other", !2, i64 0}
  )";
  parse("%x = load i64, ptr %state, !tbaa !0\n"
        "store i64 %x, ptr %state, !tbaa !0\nret i64 0",
        Metadata);
  ASSERT_TRUE(Module);
  expect(Oracle());
  parse("%x = load i64, ptr %state, !tbaa !0\n"
        "store i64 %x, ptr %state, !tbaa !3\nret i64 0",
        Metadata);
  ASSERT_TRUE(Module);
  reject("mixed TBAA");
}

TEST_F(LLVMModel, InputBlocksOperationsAndTraversalHaveIndependentBudgets) {
  parse("ret i64 0");
  ASSERT_TRUE(Module);
  for (unsigned Which = 0; Which != 4; ++Which) {
    SCOPED_TRACE(Which);
    LLVMInterpreterModelLimits Limits;
    uint64_t *Fields[] = {&Limits.MaxInputItems, &Limits.MaxBlocks,
                          &Limits.MaxOperations, &Limits.MaxWork};
    *Fields[Which] = 0;
    auto M = model(Limits);
    ASSERT_FALSE(static_cast<bool>(M));
    EXPECT_NE(llvm::toString(M.takeError()).find("budget exhausted"),
              std::string::npos);
  }
  LLVMInterpreterModelLimits Exact;
  Exact.MaxOperations = Exact.MaxBlocks = 1;
  auto M = model(Exact);
  ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
  parse("%x = add nuw i64 2, 3\nret i64 %x");
  ASSERT_TRUE(Module);
  Exact.MaxOperations = 2;
  auto Expanded = model(Exact);
  ASSERT_FALSE(static_cast<bool>(Expanded));
  llvm::consumeError(Expanded.takeError());
  parse("br label %exit\nexit:\n%v = phi i64 [7, %0]\nret i64 %v");
  ASSERT_TRUE(Module);
  Exact.MaxBlocks = 2; // Original blocks fit; the parallel PHI edge does not.
  Exact.MaxOperations = 100;
  auto Edge = model(Exact);
  ASSERT_FALSE(static_cast<bool>(Edge));
  EXPECT_NE(llvm::toString(Edge.takeError()).find("block budget"),
            std::string::npos);
}

TEST_F(LLVMModel, InvalidFunctionsAndIncompatibleDataLayoutsAreRefused) {
  for (const char *Layout : {"", "E-p:64:64", "e-p:32:32", "e-p:64:64:64:32"}) {
    SCOPED_TRACE(Layout);
    parse("ret i64 0");
    ASSERT_TRUE(Module);
    Module->setDataLayout(Layout);
    reject("data layout");
  }
  parse("ret i64 0");
  ASSERT_TRUE(Module);
  function().front().getTerminator()->eraseFromParent();
  reject("no terminator");
  parse("%x = load i64, ptr %state\nret i64 %x");
  ASSERT_TRUE(Module);
  function().front().back().setOperand(
      0, llvm::ConstantInt::get(llvm::Type::getInt8Ty(Context), 0));
  reject("invalid LLVM function");
}
} // namespace neverd::analysis::llvm_model_test
