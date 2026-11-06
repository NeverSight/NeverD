#include "X86FPStateFixture.h"

class X86_64_SSEAVX : public X86FPStateLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_sse_avx.o"; }

TEST_F(X86_64_SSEAVX, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_sse_avx.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_64_SSEAVX, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_64_SSEAVX, AddssLifts) {
  verifyFPState(testObj(), "test_addss", neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_64_SSEAVX, SubssLifts) {
  verifyFPState(testObj(), "test_subss", neverd::Intrinsic::X86FPSubState);
}

TEST_F(X86_64_SSEAVX, MulssLifts) {
  verifyFPState(testObj(), "test_mulss", neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_64_SSEAVX, DivssLifts) {
  verifyFPState(testObj(), "test_divss", neverd::Intrinsic::X86FPDivState);
}

TEST_F(X86_64_SSEAVX, AddsdLifts) {
  verifyFPState(testObj(), "test_addsd", neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_64_SSEAVX, SubsdLifts) {
  verifyFPState(testObj(), "test_subsd", neverd::Intrinsic::X86FPSubState);
}

TEST_F(X86_64_SSEAVX, MulsdLifts) {
  verifyFPState(testObj(), "test_mulsd", neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_64_SSEAVX, DivsdLifts) {
  verifyFPState(testObj(), "test_divsd", neverd::Intrinsic::X86FPDivState);
}

TEST_F(X86_64_SSEAVX, SqrtssLifts) {
  verifyFPState(testObj(), "test_sqrtss", neverd::Intrinsic::X86FPArithState,
                unsigned(neverd::X86FPArithKind::SquareRoot) | 16);
}

TEST_F(X86_64_SSEAVX, SqrtsdLifts) {
  verifyFPState(testObj(), "test_sqrtsd", neverd::Intrinsic::X86FPArithState,
                unsigned(neverd::X86FPArithKind::SquareRoot) | 24);
}

TEST_F(X86_64_SSEAVX, CvtSs2SiLifts) {
  verifyFPState(testObj(), "test_cvtss2si",
                neverd::Intrinsic::X86FPCvtToIntState);
}

TEST_F(X86_64_SSEAVX, CvtSi2SsLifts) {
  verifyFPState(testObj(), "test_cvtsi2ss",
                neverd::Intrinsic::X86FPScalarConvertState, 1);
}

TEST_F(X86_64_SSEAVX, CvtSd2SiLifts) {
  verifyFPState(testObj(), "test_cvtsd2si",
                neverd::Intrinsic::X86FPCvtToIntState);
}

TEST_F(X86_64_SSEAVX, CvtSi2SdLifts) {
  verifyFPState(testObj(), "test_cvtsi2sd",
                neverd::Intrinsic::X86FPScalarConvertState, 5);
}

TEST_F(X86_64_SSEAVX, CvtSd2SsLifts) {
  verifyFPState(testObj(), "test_cvtsd2ss",
                neverd::Intrinsic::X86FPScalarConvertState, 2);
}

TEST_F(X86_64_SSEAVX, CvtSs2SdLifts) {
  verifyFPState(testObj(), "test_cvtss2sd",
                neverd::Intrinsic::X86FPScalarConvertState, 4);
}

// MOVNTDQ faults on an unaligned address, on a path of its own. That is the
// only control flow that may end without a return.
TEST_F(X86_64_SSEAVX, NoUnreachableOutsideAlignmentFaults) {
  const auto R = liftToLLVMIR(testObj(), "");
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Trap = "\n\nalignment.trap:";
  size_t Faults = 0;
  for (size_t At = R.out.find("unreachable"); At != std::string::npos;
       At = R.out.find("unreachable", At + 1), ++Faults) {
    const size_t Block = R.out.rfind("\n\n", At);
    ASSERT_NE(Block, std::string::npos) << R.out;
    EXPECT_EQ(R.out.compare(Block, Trap.size(), Trap), 0) << R.out;
  }
  EXPECT_GT(Faults, 0u) << R.out;
}
