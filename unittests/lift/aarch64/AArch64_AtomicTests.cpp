#include "NeverDLiftFixture.h"

class AArch64_Atomic : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_atomic_a64.o";
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

    auto syntax =
        exec("clang", {"-std=c11", "-fsyntax-only", cFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << source;
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

    auto syntax =
        exec("clang", {"-std=gnu11", "-fsyntax-only", cFile.string()});
    EXPECT_EQ(syntax.exitCode, 0) << syntax.err << "\n" << source;
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
