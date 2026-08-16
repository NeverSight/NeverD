#include "NeverDLiftFixture.h"

class AArch64_RET : public NeverDLiftTest {};

static fs::path retObj() { return fs::path(TEST_OBJ_DIR) / "test_ret_a64.o"; }

TEST_F(AArch64_RET, ExplicitRegisterIsTheBranchTarget) {
  auto Low = liftToLowIR(retObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  EXPECT_NE(Low.out.find("[0x4.0] BRANCH  cst:0x8:8"), std::string::npos)
      << Low.out;
  EXPECT_EQ(Low.out.find("[0x4.0] RETURN"), std::string::npos) << Low.out;
  EXPECT_NE(Low.out.find("[0xC.0] RETURN  reg:0xF0:8"), std::string::npos)
      << Low.out;
}

TEST_F(AArch64_RET, LocalExplicitTargetRemainsReachable) {
  auto LLVM = liftToLLVMIR(retObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_NE(LLVM.out.find("ret i64 7"), std::string::npos) << LLVM.out;
}
