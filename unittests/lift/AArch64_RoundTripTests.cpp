//===- AArch64_RoundTripTests.cpp - Semantic round-trip (AArch64)
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "llvm/ADT/StringRef.h"

class AArch64_RoundTrip : public NeverDLiftTest {};

static fs::path roundtripObj() {
  return fs::path(TEST_OBJ_DIR) / "test_roundtrip_a64.o";
}

static fs::path paramPhiObj() {
  return fs::path(TEST_OBJ_DIR) / "test_param_phi_a64.o";
}

static std::string functionText(llvm::StringRef C, llvm::StringRef Name) {
  std::string Header = Name.str() + "(";
  size_t Begin = C.find(Header);
  if (Begin == llvm::StringRef::npos)
    return {};
  size_t Open = C.find('{', Begin);
  if (Open == llvm::StringRef::npos)
    return {};
  unsigned Depth = 0;
  for (size_t I = Open; I < C.size(); ++I) {
    if (C[I] == '{')
      ++Depth;
    else if (C[I] == '}' && --Depth == 0)
      return C.slice(Begin, I + 1).str();
  }
  return {};
}

static size_t countOccurrences(llvm::StringRef Text, llvm::StringRef Needle) {
  size_t Count = 0;
  for (size_t Pos = 0; (Pos = Text.find(Needle, Pos)) != llvm::StringRef::npos;
       Pos += Needle.size())
    ++Count;
  return Count;
}

TEST_F(AArch64_RoundTrip, AllStagesSucceed) { verifyAllStages(roundtripObj()); }

TEST_F(AArch64_RoundTrip, NoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(roundtripObj());
}

TEST_F(AArch64_RoundTrip, DecompileProducesC) {
  auto R = decompileToHighC(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
}

TEST_F(AArch64_RoundTrip, HighIRHasControlFlowKeywords) {
  auto R = liftToHighIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("if")) << "Expected 'if' in HighIR";
  EXPECT_TRUE(R.contains("return")) << "Expected 'return' in HighIR";
}

TEST_F(AArch64_RoundTrip, LLVMIRHasArithOps) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("add ")) << "Expected 'add' in LLVM IR";
}

TEST_F(AArch64_RoundTrip, LLVMIRHasBitwiseOps) {
  auto R = liftToLLVMIRUnopt(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasBitwise =
      R.contains("and ") || R.contains("or ") || R.contains("xor ");
  EXPECT_TRUE(HasBitwise) << "Expected bitwise ops in LLVM IR";
}

TEST_F(AArch64_RoundTrip, MultipleFunctionsLifted) {
  auto R = liftToLowIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  int FuncCount = 0;
  std::string::size_type Pos = 0;
  while ((Pos = R.out.find("func ", Pos)) != std::string::npos) {
    ++FuncCount;
    ++Pos;
  }
  EXPECT_GE(FuncCount, 5) << "Expected at least 5 functions in LowIR";
}

TEST_F(AArch64_RoundTrip, LowIRHasExpectedOpcodes) {
  auto R = liftToLowIR(roundtripObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INT_ADD")) << "Expected INT_ADD in LowIR";
  EXPECT_TRUE(R.contains("RETURN")) << "Expected RETURN in LowIR";
}

TEST_F(AArch64_RoundTrip, ParameterWidthFlowsThroughBranchPhi) {
  auto R = decompileToHighC(paramPhiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
  std::ifstream In(CFile);
  std::string C((std::istreambuf_iterator<char>(In)), {});
  EXPECT_NE(C.find("int32_t test_param_phi(int32_t arg0"), std::string::npos)
      << C;
  EXPECT_EQ(C.find("v1_0"), std::string::npos) << C;
  EXPECT_EQ(C.find("v2_0"), std::string::npos) << C;

  // This only validates generated C syntax.  A Darwin cross-target on Linux
  // suppresses the host libc include paths even though no target ABI is used.
  auto Syntax = exec("clang", {"-fsyntax-only", CFile.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(AArch64_RoundTrip, LiveInIdentityAndCallClobbersStayExact) {
  auto R = decompileToHighC(paramPhiObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
  std::ifstream In(CFile);
  std::string C((std::istreambuf_iterator<char>(In)), {});

  std::string Load = functionText(C, "test_param_load32");
  ASSERT_FALSE(Load.empty()) << C;
  EXPECT_NE(Load.find("void* arg0"), std::string::npos) << Load;
  EXPECT_NE(Load.find("neverd_mem_load_"), std::string::npos) << Load;
  EXPECT_EQ(Load.find("return arg0;"), std::string::npos) << Load;

  std::string PostCall = functionText(C, "test_post_call_x1");
  ASSERT_FALSE(PostCall.empty()) << C;
  size_t Open = PostCall.find('{');
  ASSERT_NE(Open, std::string::npos) << PostCall;
  llvm::StringRef Header(PostCall.data(), Open);
  EXPECT_TRUE(Header.contains("(void)")) << Header.str();
  EXPECT_FALSE(Header.contains("arg1")) << Header.str();
  EXPECT_NE(PostCall.find("caller-saved register clobbered by call: unknown"),
            std::string::npos)
      << PostCall;

  std::string NarrowReturn = functionText(C, "test_post_call_w0");
  ASSERT_FALSE(NarrowReturn.empty()) << C;
  size_t NarrowOpen = NarrowReturn.find('{');
  ASSERT_NE(NarrowOpen, std::string::npos) << NarrowReturn;
  llvm::StringRef NarrowHeader(NarrowReturn.data(), NarrowOpen);
  EXPECT_TRUE(NarrowHeader.contains("(void)")) << NarrowHeader.str();
  EXPECT_FALSE(NarrowHeader.contains("arg0")) << NarrowHeader.str();
  EXPECT_NE(NarrowReturn.find("test_call_leaf"), std::string::npos)
      << NarrowReturn;

  auto LLVM = liftToLLVMIRUnopt(paramPhiObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  EXPECT_EQ(
      countOccurrences(LLVM.out, "%X1_call_clobber_unknown = freeze i64 undef"),
      1u)
      << LLVM.out;
  std::string NarrowLLVM = functionText(LLVM.out, "@test_post_call_w0");
  ASSERT_FALSE(NarrowLLVM.empty()) << LLVM.out;
  EXPECT_NE(NarrowLLVM.find("call i64 @test_call_leaf(i64 41)"),
            std::string::npos)
      << NarrowLLVM;
  EXPECT_NE(NarrowLLVM.find("trunc i64 %call to i32"), std::string::npos)
      << NarrowLLVM;
  std::string LateCmpLLVM =
      functionText(LLVM.out, "@test_late_cmp_consumes_x1");
  ASSERT_FALSE(LateCmpLLVM.empty()) << LLVM.out;
  EXPECT_NE(LLVM.out.find("define dso_local i64 @test_late_cmp_consumes_x1("),
            std::string::npos)
      << LLVM.out;
  EXPECT_EQ(LLVM.out.find("define dso_local { i64, i64 } "
                          "@test_late_cmp_consumes_x1("),
            std::string::npos)
      << LLVM.out;

  std::string D9LLVM = functionText(LLVM.out, "@test_post_call_d9");
  ASSERT_FALSE(D9LLVM.empty()) << LLVM.out;
  EXPECT_NE(D9LLVM.find("ret i64 123"), std::string::npos) << D9LLVM;
  EXPECT_EQ(D9LLVM.find("D9_call_clobber"), std::string::npos) << D9LLVM;

  std::string Q9LLVM = functionText(LLVM.out, "@test_post_call_q9_upper");
  ASSERT_FALSE(Q9LLVM.empty()) << LLVM.out;
  EXPECT_NE(Q9LLVM.find("Q9.1_call_clobber_unknown = freeze i128 undef"),
            std::string::npos)
      << Q9LLVM;
  EXPECT_NE(Q9LLVM.find("18446744073709551615"), std::string::npos) << Q9LLVM;
  EXPECT_NE(Q9LLVM.find("-18446744073709551616"), std::string::npos) << Q9LLVM;
  EXPECT_NE(Q9LLVM.find("Q9.1_call_clobber = or i128"), std::string::npos)
      << Q9LLVM;

  // This only validates generated C syntax.  A Darwin cross-target on Linux
  // suppresses the host libc include paths even though no target ABI is used.
  auto Syntax = exec("clang", {"-fsyntax-only", CFile.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}
