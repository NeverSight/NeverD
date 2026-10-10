//===- LLVMInterpreterModelTests.cpp - Scalar source semantic relations
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMInterpreterModelTest.h"

namespace neverd::analysis::llvm_model_test {
TEST_F(LLVMModel, StatusRemainsSeparateFromEveryStateWord) {
  parse("store i64 27, ptr %state, align 8\nret i64 3");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(0), {n(27)})}, n(3)));
  expect(Oracle({op(NdOp::COPY, r(0), {n(27)})}), Status::Different);
  expect(Oracle({op(NdOp::COPY, r(0), {n(28)})}, n(3)), Status::Different);
  auto C = llvmInterpreterMachineStateContract();
  ASSERT_EQ(C.ReturnRegisters.size(), 18U);
  ASSERT_EQ(C.PreservedRegisters.size(), 1U);
  ASSERT_EQ(C.EntryConstants.size(), 1U);
  EXPECT_EQ(C.PreservedRegisters[0].Offset, LLVMInterpreterDefinednessOffset);
  EXPECT_EQ(C.EntryConstants[0].Value, 0U);
}

TEST_F(LLVMModel, RawStateWritesPreserveUntouchedBytes) {
  parse(R"(
    %p = getelementptr inbounds i8, ptr %state, i64 19
    %x = load i16, ptr %p, align 1
    %y = xor i16 %x, 413
    store i16 %y, ptr %p, align 1
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::INT_XOR, r(19, 2), {r(19, 2), n(413, 2)})}));
  for (unsigned Offset = 0; Offset < 136; Offset += 8) {
    SCOPED_TRACE(Offset);
    expect(Oracle({op(NdOp::INT_XOR, r(19, 2), {r(19, 2), n(413, 2)}),
                   op(NdOp::INT_XOR, r(Offset), {r(Offset), n(1)})}),
           Status::Different);
  }
}

TEST_F(LLVMModel, IntegerPointerProjectionsRetainExactByteOffsets) {
  parse(R"(
    %base = ptrtoint ptr %state to i64
    %plus = add i64 %base, 32
    %back = sub i64 %plus, 8
    %p = inttoptr i64 %back to ptr
    store i32 123, ptr %p, align 4
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::COPY, r(24, 4), {n(123, 4)})}));
}

TEST_F(LLVMModel, TypedStateProjectionsPreserveIndependentByteOffsets) {
  for (auto [Type, Offset] :
       {std::pair{"i8", 4U}, {"i16", 8U}, {"i32", 16U}, {"i64", 32U}})
    for (const char *Flags : {"", "inbounds ", "nuw ", "nusw "}) {
      SCOPED_TRACE(std::string(Type) + Flags);
      parse("%p = getelementptr " + std::string(Flags) + Type +
            ", ptr %state, i64 4\n%x = load i64, ptr %p, align 1\n"
            "%y = xor i64 %x, 413\nstore i64 %y, ptr %p, align 1\nret i64 0");
      ASSERT_TRUE(Module);
      expect(Oracle({op(NdOp::INT_XOR, r(Offset), {r(Offset), n(413)})}));
    }
  // The formerly refused unused word projection has no observable effect.
  parse("%p = getelementptr i64, ptr %state, i64 1\nret i64 0");
  ASSERT_TRUE(Module);
  expect(Oracle());
}

TEST_F(LLVMModel, TypedStateProjectionsUseLayoutAllocationStride) {
  parse("%p = getelementptr inbounds i64, ptr %state, i64 2\n"
        "%x = load i64, ptr %p, align 1\n%y = xor i64 %x, 413\n"
        "store i64 %y, ptr %p, align 1\nret i64 0");
  ASSERT_TRUE(Module);
  Module->setDataLayout("e-p:64:64-i64:128");
  expect(Oracle({op(NdOp::INT_XOR, r(32), {r(32), n(413)})}));
}

TEST_F(LLVMModel, ChainedTypedStateProjectionsRetainNegativeOffsets) {
  for (const char *Flags : {"", "inbounds ", "nusw "}) {
    SCOPED_TRACE(Flags);
    parse("%p = getelementptr inbounds i64, ptr %state, i64 4\n"
          "%q = getelementptr " +
          std::string(Flags) +
          "i16, ptr %p, i64 -3\n"
          "%r = getelementptr i32, ptr %q, i64 1\n"
          "store i32 123, ptr %r, align 1\nret i64 0");
    ASSERT_TRUE(Module);
    expect(Oracle({op(NdOp::COPY, r(30, 4), {n(123, 4)})}));
  }
}

TEST_F(LLVMModel, ArithmeticAndComparisonsPreserveBitWidths) {
  const std::pair<const char *, NdOp> Operations[] = {
      {"add", NdOp::INT_ADD},  {"sub", NdOp::INT_SUB},
      {"mul", NdOp::INT_MULT}, {"and", NdOp::INT_AND},
      {"or", NdOp::INT_OR},    {"xor", NdOp::INT_XOR},
      {"shl", NdOp::INT_LEFT}, {"lshr", NdOp::INT_RIGHT},
      {"ashr", NdOp::INT_ASHR}};
  for (unsigned Width : {8U, 16U, 32U, 64U})
    for (auto [Name, Code] : Operations) {
      SCOPED_TRACE(std::to_string(Width) + Name);
      std::string T = "i" + std::to_string(Width);
      parse("%x = load " + T + ", ptr %state, align 1\n%y = " + Name + " " + T +
            " %x, 3\nstore " + T + " %y, ptr %state, align 1\nret i64 0");
      ASSERT_TRUE(Module);
      expect(Oracle(
          {op(Code, r(0, Width / 8), {r(0, Width / 8), n(3, Width / 8)})}));
    }
}

TEST_F(LLVMModel, OneBitTruncationSignExtensionAndSelect) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    %b = trunc i64 %x to i1
    %s = sext i1 %b to i64
    %z = zext i1 %b to i64
    %selected = select i1 %b, i64 %s, i64 %z
    store i64 %selected, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  // Only the low bit matters; true becomes the full negative-one word.
  expect(Oracle({op(NdOp::INT_AND, r(256), {r(0), n(1)}),
                 op(NdOp::INT_SUB, r(0), {n(0), r(256)})}));
}

TEST_F(LLVMModel, CrossBlockValuesAndMergedPhis) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    %choose = icmp eq i64 %x, 0
    %a = add i64 %x, 5
    br i1 %choose, label %left, label %right
  left:
    br label %exit
  right:
    %b = xor i64 %x, 19
    br label %exit
  exit:
    %v = phi i64 [ %a, %left ], [ %b, %right ]
    store i64 %v, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::INT_EQUAL, r(256, 1), {r(0), n(0)}),
                 op(NdOp::INT_ADD, r(264), {r(0), n(5)}),
                 op(NdOp::INT_XOR, r(272), {r(0), n(19)}),
                 op(NdOp::SELECT, r(0), {r(256, 1), r(264), r(272)})}));
}

TEST_F(LLVMModel, SwitchDefaultAndRepeatedDestinationEdges) {
  parse(R"(
    %x = load i64, ptr %state, align 8
    switch i64 %x, label %other [i64 7, label %yes
                                 i64 12, label %yes
                                 i64 25, label %other]
  yes:
    br label %exit
  other:
    br label %exit
  exit:
    %result = phi i64 [19, %yes], [4, %other]
    store i64 %result, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  expect(Oracle({op(NdOp::INT_EQUAL, r(256, 1), {r(0), n(7)}),
                 op(NdOp::INT_EQUAL, r(264, 1), {r(0), n(12)}),
                 op(NdOp::BOOL_OR, r(272, 1), {r(256, 1), r(264, 1)}),
                 op(NdOp::SELECT, r(0), {r(272, 1), n(19), n(4)})}));
}

TEST_F(LLVMModel, DegenerateBranchesAndEmptySwitches) {
  for (const char *Terminator : {"br i1 %cond, label %exit, label %exit",
                                 "switch i1 %cond, label %exit []"}) {
    parse(std::string("%x = load i64, ptr %state, align 8\n") +
          "%cond = icmp eq i64 %x, 3\n" + Terminator + "\nexit:\nret i64 0");
    ASSERT_TRUE(Module);
    expect(Oracle());
  }
}

TEST_F(LLVMModel, GuestFrameRemainsMemoryAndObservesExtraWrites) {
  parse(R"(
    %sp_slot = getelementptr i8, ptr %state, i64 32
    %sp = load i64, ptr %sp_slot, align 8
    %addr = sub i64 %sp, 8
    %p = inttoptr i64 %addr to ptr
    %x = load i64, ptr %state, align 8
    store i64 %x, ptr %p, align 1
    %y = load i64, ptr %p, align 1
    store i64 %y, ptr %state, align 8
    ret i64 0
  )");
  ASSERT_TRUE(Module);
  auto C = llvmInterpreterMachineStateContract();
  C.Frame = LowIRIndependenceFrame{{32, 8}, -16, 8};
  C.PreservedRegisters.push_back({32, 8});
  C.PreservedFrameRanges = {{0, 8}};
  expect(Oracle({op(NdOp::INT_SUB, r(256), {r(32), n(8)}),
                 op(NdOp::STORE, {}, {r(256), r(0)})}),
         Status::Proved, C);
  expect(Oracle(), Status::Different, C);
}

TEST_F(LLVMModel, InputIsUnchangedAndRecordsBindTheGeneratedFunction) {
  parse("ret i64 0");
  ASSERT_TRUE(Module);
  std::string Before, After;
  llvm::raw_string_ostream B(Before), A(After);
  Module->print(B, nullptr);
  auto M = model();
  ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
  Module->print(A, nullptr);
  EXPECT_EQ(Before, After);
  auto Original = M->Function;
  M->Function.Blocks[0].Ops.back().Inputs[0] = n(3);
  auto R = checkLowIRRefinement(M->Function, M->Instructions, Original,
                                llvmInterpreterMachineStateContract(),
                                LowIRRefinementWitness::LiftedBits);
  EXPECT_EQ(R.Status, Status::Invalid) << R.Diagnostic;
  EXPECT_FALSE(R.Certificate);
}
} // namespace neverd::analysis::llvm_model_test
