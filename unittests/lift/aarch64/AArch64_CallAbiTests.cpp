#include "NeverDLiftFixture.h"

#include "llvm/ADT/StringRef.h"

class AArch64_CallAbi : public NeverDLiftTest {};

static fs::path callAbiObj() {
  return fs::path(TEST_OBJ_DIR) / "test_call_abi_a64_macho";
}

static std::string callAbiFunctionText(llvm::StringRef IR,
                                       llvm::StringRef Name) {
  std::string Needle = "@" + Name.str() + "(";
  size_t Begin = IR.find(Needle);
  if (Begin == llvm::StringRef::npos)
    return {};
  Begin = IR.take_front(Begin).rfind("define ");
  if (Begin == llvm::StringRef::npos)
    return {};
  size_t End = IR.find("\n}", Begin);
  if (End == llvm::StringRef::npos)
    return {};
  return IR.slice(Begin, End + 2).str();
}

TEST_F(AArch64_CallAbi, IndirectFPCallUsesV0ForArgumentAndReturn) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "_indirect_double_call");
  ASSERT_FALSE(F.empty()) << R.out;
  size_t Call = F.find("call <2 x i64> %");
  ASSERT_NE(Call, std::string::npos) << F;
  size_t CallEnd = F.find('\n', Call);
  std::string CallLine = F.substr(Call, CallEnd - Call);
  EXPECT_NE(CallLine.find("(<2 x i64>"), std::string::npos) << CallLine;
  EXPECT_EQ(CallLine.find("i64 %arg0"), std::string::npos) << CallLine;
  EXPECT_NE(F.find("ret <2 x i64>"), std::string::npos) << F;

  auto Opt = liftToLLVMIR(callAbiObj());
  ASSERT_EQ(Opt.exitCode, 0) << Opt.err;
  std::string OptF = callAbiFunctionText(Opt.out, "_indirect_double_call");
  EXPECT_NE(OptF.find("ret <2 x i64> %call"), std::string::npos) << OptF;
}

TEST_F(AArch64_CallAbi, ExternalPairCallPreservesX0AndX1) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "_direct_external_pair_sum");
  ASSERT_FALSE(F.empty()) << R.out;
  EXPECT_NE(F.find("call { i64, i64 } @_make_external_pair()"),
            std::string::npos)
      << F;
  EXPECT_NE(F.find("extractvalue { i64, i64 }"), std::string::npos) << F;
  EXPECT_EQ(F.find("X1_call_clobber_unknown"), std::string::npos) << F;
}

TEST_F(AArch64_CallAbi, ExternalDarwinVarargsKeepOutgoingStackValues) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string F = callAbiFunctionText(R.out, "_direct_external_varargs");
  ASSERT_FALSE(F.empty()) << R.out;
  EXPECT_NE(F.find("call i64 (i64, ...) @_sum_external_varargs(i64 16, i64 32, "
                   "i64 48)"),
            std::string::npos)
      << F;
}

TEST_F(AArch64_CallAbi, ExternalZeroArgPrototypeIsStableAcrossCallSites) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  auto R = liftToLLVMIRUnopt(callAbiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;

  std::string Compute = callAbiFunctionText(R.out, "_external_error_compute");
  std::string Simple = callAbiFunctionText(R.out, "_external_error_simple");
  ASSERT_FALSE(Compute.empty()) << R.out;
  ASSERT_FALSE(Simple.empty()) << R.out;
  EXPECT_NE(Compute.find("@___error()"), std::string::npos) << Compute;
  EXPECT_NE(Simple.find("@___error()"), std::string::npos) << Simple;
  EXPECT_EQ(R.out.find("@___error(i"), std::string::npos) << R.out;
}

TEST_F(AArch64_CallAbi, AllStagesPass) {
  if (!fs::exists(callAbiObj()))
    GTEST_SKIP() << "AArch64 Mach-O ABI fixture is only built on Apple hosts";
  verifyAllStages(callAbiObj());
  verifyLLVMIRNoVerifierErrors(callAbiObj());
}
