#include "NeverDLiftFixture.h"

class AArch64_Atomic : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto stringHeader = tmpFile("string.h");
    std::ofstream shim(stringHeader);
    ASSERT_TRUE(shim.good());
    shim << "void *memcpy(void *, const void *, __SIZE_TYPE__);\n";
    shim.close();

    auto syntax = exec(NEVERD_TEST_CLANG,
                       {"-target", "aarch64-none-elf", "-ffreestanding",
                        "-march=armv9.4-a+lse128+the+d128", "-std=gnu11", "-I",
                        tmp().string(), "-fsyntax-only", CFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << Source;
  }
};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_atomic_a64.o";
}

static std::string functionIR(const std::string &IR, const std::string &Name) {
  auto NamePos = IR.find("@" + Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = IR.rfind("define ", NamePos);
  auto End = IR.find("\n}", NamePos);
  if (Begin == std::string::npos || End == std::string::npos)
    return {};
  return IR.substr(Begin, End + 2 - Begin);
}

static std::string functionC(const std::string &Source,
                             const std::string &Name) {
  auto NamePos = Source.find(Name + "(");
  if (NamePos == std::string::npos)
    return {};
  auto Begin = Source.rfind('\n', NamePos);
  auto End = Source.find("\n}", NamePos);
  if (End == std::string::npos)
    return {};
  Begin = Begin == std::string::npos ? 0 : Begin + 1;
  return Source.substr(Begin, End + 2 - Begin);
}

TEST_F(AArch64_Atomic, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_atomic_a64.o not built";
    verifyAllStages(testObj());
}

TEST_F(AArch64_Atomic, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(AArch64_Atomic, LdxrStxrLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
    bool has_load = r.out.find("LOAD") != std::string::npos;
    bool has_store = r.out.find("STORE") != std::string::npos;
    EXPECT_TRUE(has_load) << "Expected LOAD for LDXR";
    EXPECT_TRUE(has_store) << "Expected STORE for STXR";
}

TEST_F(AArch64_Atomic, BarrierLifted) {
    auto r = liftToLowIR(testObj());
    ASSERT_EQ(r.exitCode, 0);
}

TEST_F(AArch64_Atomic, LLVMIRNoVerifierErrors) {
    verifyLLVMIRNoVerifierErrors(testObj());
}

TEST_F(AArch64_Atomic, LdarStlrKeepAcquireReleaseOrdering) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0) << r.err;
    auto acquireLoad = r.out.find("load atomic i64");
    ASSERT_NE(acquireLoad, std::string::npos) << r.out;
    EXPECT_NE(r.out.find("load atomic i64", acquireLoad + 1),
              std::string::npos)
        << "An unused LDAR must remain observable:\n" << r.out;
    EXPECT_NE(r.out.find("acquire, align 8"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("store atomic i64"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("release, align 8"), std::string::npos) << r.out;
}

TEST_F(AArch64_Atomic, HighCKeepsLdarStlrOrderingAndCompiles) {
    auto r = decompileToHighC(testObj());
    ASSERT_EQ(r.exitCode, 0) << r.err;

    auto cFile = tmpFile("decompiled_high.c");
    ASSERT_TRUE(fs::exists(cFile));
    std::ifstream input(cFile);
    ASSERT_TRUE(input.good());
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    EXPECT_NE(source.find("__atomic_load_n"), std::string::npos) << source;
    EXPECT_NE(source.find("__ATOMIC_ACQUIRE"), std::string::npos) << source;
    EXPECT_NE(source.find("__atomic_store_n"), std::string::npos) << source;
    EXPECT_NE(source.find("__ATOMIC_RELEASE"), std::string::npos) << source;

    expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, HighCUsesStandardOrderedI128Exchange) {
    auto r = decompileToHighC(testObj());
    ASSERT_EQ(r.exitCode, 0) << r.err;

    auto cFile = tmpFile("decompiled_high.c");
    ASSERT_TRUE(fs::exists(cFile));
    std::ifstream input(cFile);
    ASSERT_TRUE(input.good());
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    EXPECT_NE(source.find("__atomic_exchange_n"), std::string::npos) << source;
    EXPECT_NE(source.find("unsigned __int128"), std::string::npos) << source;
    EXPECT_NE(source.find("__ATOMIC_RELAXED"), std::string::npos) << source;
    EXPECT_NE(source.find("__ATOMIC_ACQUIRE"), std::string::npos) << source;
    EXPECT_NE(source.find("__ATOMIC_RELEASE"), std::string::npos) << source;
    EXPECT_NE(source.find("__ATOMIC_ACQ_REL"), std::string::npos) << source;
    EXPECT_EQ(source.find("neverd_a64_swpp"), std::string::npos) << source;

    expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, SwppUsesOrderedAtomicI128Exchange) {
    auto r = liftToLLVMIR(testObj());
    ASSERT_EQ(r.exitCode, 0) << r.err;

    EXPECT_NE(r.out.find("atomicrmw xchg ptr"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("i128"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("monotonic, align 16"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("acquire, align 16"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("release, align 16"), std::string::npos) << r.out;
    EXPECT_NE(r.out.find("acq_rel, align 16"), std::string::npos) << r.out;
}

TEST_F(AArch64_Atomic, LdclrpUsesOrderedAtomicI128And) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Ordering;
  } Cases[] = {{"test_ldclrp", "monotonic"},
               {"test_ldclrpa", "acquire"},
               {"test_ldclrpal", "acq_rel"},
               {"test_ldclrpl", "release"}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find("atomicrmw and ptr"), std::string::npos) << F;
    EXPECT_NE(F.find("i128"), std::string::npos) << F;
    EXPECT_NE(F.find(std::string(Case.Ordering) + ", align 16"),
              std::string::npos)
        << F;
    EXPECT_NE(F.find("ret { i64, i64 }"), std::string::npos) << F;
  }
}

TEST_F(AArch64_Atomic, LdclrpHighCUsesStandardAtomicFetchAndAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__atomic_fetch_and"), std::string::npos) << source;
  EXPECT_NE(source.find("unsigned __int128"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_RELAXED"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_ACQUIRE"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_RELEASE"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_ACQ_REL"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd_a64_ldclrp"), std::string::npos) << source;

  for (const char *Name :
       {"test_ldclrp", "test_ldclrpa", "test_ldclrpal", "test_ldclrpl"}) {
    SCOPED_TRACE(Name);
    auto F = functionC(source, Name);
    ASSERT_FALSE(F.empty()) << source;
    auto Atomic = F.find("__atomic_fetch_and");
    ASSERT_NE(Atomic, std::string::npos) << F;
    auto LineEnd = F.find('\n', Atomic);
    auto Call = F.substr(Atomic, LineEnd - Atomic);
    const bool UsesParams = Call.find("arg1") != std::string::npos &&
                            Call.find("arg2") != std::string::npos;
    const bool UsesSavedParams = Call.find("v0 + 16") != std::string::npos &&
                                 Call.find("v0 + 8") != std::string::npos;
    EXPECT_TRUE(UsesParams || UsesSavedParams) << Call;

    auto LineBegin = F.rfind('\n', Atomic);
    LineBegin = LineBegin == std::string::npos ? 0 : LineBegin + 1;
    auto Equals = F.find('=', LineBegin);
    ASSERT_NE(Equals, std::string::npos) << F;
    auto Lhs = F.substr(LineBegin, Equals - LineBegin);
    Lhs.erase(0, Lhs.find_first_not_of(" \t"));
    Lhs.erase(Lhs.find_last_not_of(" \t") + 1);
    EXPECT_EQ(Call.find(Lhs), std::string::npos)
        << "atomic clear mask must not use its own result: " << Call;
  }

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, RcwcaspUsesAddressOperandAndBothRegisterPairs) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;
  auto F = functionIR(r.out, "test_rcwcasp_pair");
  ASSERT_FALSE(F.empty()) << r.out;

  EXPECT_NE(F.find("inttoptr i64 %arg4 to ptr"), std::string::npos) << F;
  EXPECT_EQ(F.find("inttoptr i64 %arg2 to ptr"), std::string::npos) << F;
  EXPECT_NE(F.find("asm sideeffect \"rcwcasp x0, x1, x2, x3, [x4]\""),
            std::string::npos)
      << F;
  EXPECT_EQ(F.find("load i64"), std::string::npos) << F;
  EXPECT_EQ(F.find("store i64"), std::string::npos) << F;
  EXPECT_NE(F.find("ret { i64, i64 }"), std::string::npos) << F;
}

TEST_F(AArch64_Atomic, RcwcaspHighCUsesPairedClangBuiltinAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__builtin_arm_rcwcasp("), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd"), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
}
