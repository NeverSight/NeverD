//===- InterpreterLLVMRefinementAlignmentTests.cpp - Guest access domains ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
TEST(InterpreterLLVMRefinement, GuestAlignmentRequiresBothFreshPremises) {
  // mov [rsp-8],rax; ret. The instruction itself permits unaligned storage.
  Program P({0x48, 0x89, 0x44, 0x24, 0xf8, 0xc3});
  P.Options.EntryFrameAlignment = P.Frame.EntryAlignment =
      InterpreterEntryAlignment{16, 8};
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const std::string Body = R"(
    %spword = getelementptr i8, ptr %state, i64 32
    %sp = load i64, ptr %spword, align 8
    %addr = sub i64 %sp, 8
    %p = inttoptr i64 %addr to ptr
    %value = load i64, ptr %state, align 8
    store i64 %value, ptr %p, align 16
    ret i64 0
  )";
  const auto Good = P.check(Recovery.Residual, module(Body));
  ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
  EXPECT_TRUE(Good.Native.proved());
  EXPECT_EQ(Good.LLVM.Status, Status::Proved);
  EXPECT_EQ(Good.Native.Certificate->Relation.Contract.Frame->EntryAlignment,
            P.Frame.EntryAlignment);
  EXPECT_EQ(Good.LLVM.Certificate->Contract.Frame->EntryAlignment,
            P.Frame.EntryAlignment);

  for (auto Domain : {std::optional<InterpreterEntryAlignment>{},
                      std::optional<InterpreterEntryAlignment>{{16, 0}}}) {
    P.Options.EntryFrameAlignment = P.Frame.EntryAlignment = Domain;
    Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    const auto Bad = P.check(Recovery.Residual, module(Body));
    rejected(Bad, Stage::LLVM);
    EXPECT_TRUE(Bad.Native.proved());
    EXPECT_EQ(Bad.LLVM.Status, Status::ContractViolation);
  }
  P.Options.EntryFrameAlignment = P.Frame.EntryAlignment =
      InterpreterEntryAlignment{16, 8};
  Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  for (const auto &Edit : {std::pair<std::string, std::string>{
                               "sub i64 %sp, 8", "sub i64 %sp, 24"},
                           {"store i64 %value,", "store i64 5,"},
                           {"ret i64 0", "ret i64 1"}}) {
    auto Wrong = Body;
    const auto At = Wrong.find(Edit.first);
    ASSERT_NE(At, std::string::npos);
    Wrong.replace(At, Edit.first.size(), Edit.second);
    const auto Bad = P.check(Recovery.Residual, module(Wrong));
    rejected(Bad, Stage::LLVM);
    EXPECT_TRUE(Bad.Native.proved());
    EXPECT_EQ(Bad.LLVM.Status, Status::Different);
  }
}
} // namespace neverd::analysis::llvm_refinement_test
