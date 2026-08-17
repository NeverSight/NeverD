#include "NeverDLiftFixture.h"

class AArch64_Atomic : public NeverDLiftTest {
protected:
  void expectPairedClangSyntax(const fs::path &CFile,
                               const std::string &Source) {
    auto syntax = checkHighCClangSyntax(
        CFile, {"-target", "aarch64-none-elf", "-ffreestanding",
                "-march=armv9.4-a+lse128+the+d128+rcpc3", "-std=gnu11"});
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

TEST_F(AArch64_Atomic, ExclusiveOpsRemainTargetIntrinsics) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto Single = functionIR(r.out, "test_stxr_after_clrex");
  ASSERT_FALSE(Single.empty()) << r.out;
  EXPECT_NE(Single.find("@llvm.aarch64.ldxr"), std::string::npos) << Single;
  EXPECT_NE(Single.find("@llvm.aarch64.clrex"), std::string::npos) << Single;
  EXPECT_NE(Single.find("@llvm.aarch64.stxr"), std::string::npos) << Single;
  EXPECT_EQ(Single.find("ret i64 0"), std::string::npos) << Single;

  auto Pair = functionIR(r.out, "test_stxp_after_clrex");
  ASSERT_FALSE(Pair.empty()) << r.out;
  EXPECT_NE(Pair.find("@llvm.aarch64.ldxp"), std::string::npos) << Pair;
  EXPECT_NE(Pair.find("@llvm.aarch64.clrex"), std::string::npos) << Pair;
  EXPECT_NE(Pair.find("@llvm.aarch64.stxp"), std::string::npos) << Pair;
  EXPECT_EQ(Pair.find("ret i64 0"), std::string::npos) << Pair;
}

TEST_F(AArch64_Atomic, HighCExclusiveOpsUseStandardBuiltinsAndCompile) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  auto Single = functionC(source, "test_stxr_after_clrex");
  ASSERT_FALSE(Single.empty()) << source;
  auto Load = Single.find("__builtin_arm_ldrex");
  auto Store = Single.find("__builtin_arm_strex");
  EXPECT_NE(Load, std::string::npos) << Single;
  EXPECT_NE(Single.find("__builtin_arm_clrex()"), std::string::npos) << Single;
  ASSERT_NE(Store, std::string::npos) << Single;

  auto LoadLine = Single.rfind('\n', Load);
  LoadLine = LoadLine == std::string::npos ? 0 : LoadLine + 1;
  auto Equals = Single.find('=', LoadLine);
  ASSERT_NE(Equals, std::string::npos) << Single;
  auto LoadedName = Single.substr(LoadLine, Equals - LoadLine);
  LoadedName.erase(0, LoadedName.find_first_not_of(" \t"));
  LoadedName.erase(LoadedName.find_last_not_of(" \t") + 1);
  auto StoreEnd = Single.find('\n', Store);
  auto StoreCall = Single.substr(Store, StoreEnd - Store);
  EXPECT_EQ(StoreCall.find("(uintptr_t)(" + LoadedName + ")"),
            std::string::npos)
      << "STXR address must remain distinct from the loaded value: "
      << StoreCall;

  auto Pair = functionC(source, "test_stxp_after_clrex");
  ASSERT_FALSE(Pair.empty()) << source;
  EXPECT_NE(Pair.find("__builtin_arm_ldrex"), std::string::npos) << Pair;
  EXPECT_NE(Pair.find("__builtin_arm_strex"), std::string::npos) << Pair;
  EXPECT_NE(Pair.find("__int128"), std::string::npos) << Pair;
  EXPECT_EQ(source.find("__neverd_a64_"), std::string::npos) << source;
  EXPECT_EQ(source.find("__clrex()"), std::string::npos) << source;

  expectPairedClangSyntax(cFile, source);
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

TEST_F(AArch64_Atomic, LdsetpUsesOrderedAtomicI128Or) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Ordering;
  } Cases[] = {{"test_ldsetp", "monotonic"},
               {"test_ldsetpa", "acquire"},
               {"test_ldsetpal", "acq_rel"},
               {"test_ldsetpl", "release"}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find("atomicrmw or ptr"), std::string::npos) << F;
    EXPECT_NE(F.find("i128"), std::string::npos) << F;
    EXPECT_NE(F.find(std::string(Case.Ordering) + ", align 16"),
              std::string::npos)
        << F;
    EXPECT_NE(F.find("ret { i64, i64 }"), std::string::npos) << F;
    EXPECT_EQ(F.find("load i64"), std::string::npos) << F;
    EXPECT_EQ(F.find("store i64"), std::string::npos) << F;
  }
}

TEST_F(AArch64_Atomic, LdsetpHighCUsesStandardAtomicFetchOrAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__atomic_fetch_or"), std::string::npos) << source;
  EXPECT_NE(source.find("unsigned __int128"), std::string::npos) << source;

  for (const char *Name :
       {"test_ldsetp", "test_ldsetpa", "test_ldsetpal", "test_ldsetpl"}) {
    SCOPED_TRACE(Name);
    auto F = functionC(source, Name);
    ASSERT_FALSE(F.empty()) << source;
    size_t Count = 0;
    for (size_t Pos = F.find("__atomic_fetch_or"); Pos != std::string::npos;
         Pos = F.find("__atomic_fetch_or", Pos + 1))
      ++Count;
    EXPECT_EQ(Count, 1u) << F;
  }

  auto Relaxed = functionC(source, "test_ldsetp");
  ASSERT_FALSE(Relaxed.empty()) << source;
  EXPECT_NE(Relaxed.find("(uint64_t)(arg1)"), std::string::npos)
      << "the low pair half must be zero-extended before widening:\n"
      << Relaxed;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, LdaddUsesOrderedAtomicReadModifyWrite) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Type;
    const char *Ordering;
  } Cases[] = {
      {"test_ldadd", "i64", "monotonic"}, {"test_ldadda", "i64", "acquire"},
      {"test_ldaddal", "i64", "acq_rel"}, {"test_ldaddl", "i64", "release"},
      {"test_ldaddb", "i8", "monotonic"}, {"test_ldaddh", "i16", "monotonic"},
      {"test_stadd", "i64", "monotonic"}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find(std::string("atomicrmw add ptr") + " "), std::string::npos)
        << F;
    EXPECT_NE(F.find(std::string(", ") + Case.Type + " "), std::string::npos)
        << F;
    EXPECT_NE(F.find(std::string(Case.Ordering) + ", align"), std::string::npos)
        << F;
  }
}

TEST_F(AArch64_Atomic, LdaddHighCUsesAtomicFetchAddAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__atomic_fetch_add"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_RELAXED"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_ACQUIRE"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_RELEASE"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_ACQ_REL"), std::string::npos) << source;

  for (const char *Name :
       {"test_ldadd", "test_ldadda", "test_ldaddal", "test_ldaddl",
        "test_ldaddb", "test_ldaddh", "test_stadd"}) {
    SCOPED_TRACE(Name);
    auto F = functionC(source, Name);
    ASSERT_FALSE(F.empty()) << source;
    size_t Count = 0;
    for (size_t Pos = F.find("__atomic_fetch_add"); Pos != std::string::npos;
         Pos = F.find("__atomic_fetch_add", Pos + 1))
      ++Count;
    EXPECT_EQ(Count, 1u) << F;
  }

  auto Byte = functionC(source, "test_ldaddb");
  ASSERT_FALSE(Byte.empty()) << source;
  EXPECT_NE(Byte.find("\n    uint8_t "), std::string::npos) << Byte;
  EXPECT_EQ(Byte.find("\n    int8_t "), std::string::npos) << Byte;

  auto Half = functionC(source, "test_ldaddh");
  ASSERT_FALSE(Half.empty()) << source;
  EXPECT_NE(Half.find("\n    uint16_t "), std::string::npos) << Half;
  EXPECT_EQ(Half.find("\n    int16_t "), std::string::npos) << Half;

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, RemainingLseRmwUsesOrderedAtomicInstructions) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Operation;
    const char *Type;
    const char *Ordering;
  } Cases[] = {
      {"test_ldclr", "and", "i64", "monotonic"},
      {"test_ldclra", "and", "i64", "acquire"},
      {"test_ldclral", "and", "i64", "acq_rel"},
      {"test_ldclrl", "and", "i64", "release"},
      {"test_ldeoral", "xor", "i64", "acq_rel"},
      {"test_ldsetal", "or", "i64", "acq_rel"},
      {"test_ldsmaxal", "max", "i64", "acq_rel"},
      {"test_ldsminal", "min", "i64", "acq_rel"},
      {"test_ldumaxal", "umax", "i64", "acq_rel"},
      {"test_lduminal", "umin", "i64", "acq_rel"},
      {"test_swpal", "xchg", "i64", "acq_rel"},
      {"test_ldsetalb", "or", "i8", "acq_rel"},
      {"test_ldeoralh", "xor", "i16", "acq_rel"},
      {"test_stsetl", "or", "i64", "release"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find(std::string("atomicrmw ") + Case.Operation + " ptr"),
              std::string::npos)
        << F;
    EXPECT_NE(F.find(std::string(", ") + Case.Type + " "), std::string::npos)
        << F;
    EXPECT_NE(F.find(std::string(Case.Ordering) + ", align"),
              std::string::npos)
        << F;
    EXPECT_EQ(F.find("load i"), std::string::npos) << F;
    EXPECT_EQ(F.find("store i"), std::string::npos) << F;
  }
}

TEST_F(AArch64_Atomic, RemainingLseRmwHighCUsesAtomicBuiltinsAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  const struct {
    const char *Name;
    const char *Builtin;
    const char *Ordering;
  } Cases[] = {
      {"test_ldclr", "__builtin_arm_ldclr", "__ATOMIC_RELAXED"},
      {"test_ldclra", "__builtin_arm_ldclr", "__ATOMIC_ACQUIRE"},
      {"test_ldclral", "__builtin_arm_ldclr", "__ATOMIC_ACQ_REL"},
      {"test_ldclrl", "__builtin_arm_ldclr", "__ATOMIC_RELEASE"},
      {"test_ldeoral", "__builtin_arm_ldeor", "__ATOMIC_ACQ_REL"},
      {"test_ldsetal", "__builtin_arm_ldset", "__ATOMIC_ACQ_REL"},
      {"test_ldsmaxal", "__builtin_arm_ldsmax", "__ATOMIC_ACQ_REL"},
      {"test_ldsminal", "__builtin_arm_ldsmin", "__ATOMIC_ACQ_REL"},
      {"test_ldumaxal", "__builtin_arm_ldumax", "__ATOMIC_ACQ_REL"},
      {"test_lduminal", "__builtin_arm_ldumin", "__ATOMIC_ACQ_REL"},
      {"test_swpal", "__atomic_exchange_n", "__ATOMIC_ACQ_REL"},
      {"test_ldsetalb", "__builtin_arm_ldset", "__ATOMIC_ACQ_REL"},
      {"test_ldeoralh", "__builtin_arm_ldeor", "__ATOMIC_ACQ_REL"},
      {"test_stsetl", "__builtin_arm_ldset", "__ATOMIC_RELEASE"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionC(source, Case.Name);
    ASSERT_FALSE(F.empty()) << source;
    EXPECT_NE(F.find(Case.Builtin), std::string::npos) << F;
    EXPECT_NE(F.find(Case.Ordering), std::string::npos) << F;
  }

  EXPECT_NE(functionC(source, "test_ldsetalb").find(", 1, __ATOMIC_ACQ_REL"),
            std::string::npos);
  EXPECT_NE(functionC(source, "test_ldeoralh").find(", 2, __ATOMIC_ACQ_REL"),
            std::string::npos);

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, LseRmwUsesCompilerBuiltins) {
  auto high = liftToHighIR(testObj());
  ASSERT_EQ(high.exitCode, 0) << high.err;

  auto llvm = liftToLLVMIR(testObj());
  ASSERT_EQ(llvm.exitCode, 0) << llvm.err;
  auto llvmLdclr = functionIR(llvm.out, "test_ldclral");
  ASSERT_FALSE(llvmLdclr.empty()) << llvm.out;
  auto mask = llvmLdclr.find("xor i64 %arg1, -1");
  auto rmw = llvmLdclr.find("atomicrmw and ptr");
  ASSERT_NE(mask, std::string::npos) << llvmLdclr;
  ASSERT_NE(rmw, std::string::npos) << llvmLdclr;
  EXPECT_LT(mask, rmw) << llvmLdclr;

  auto decompiled = decompileToHighC(testObj());
  ASSERT_EQ(decompiled.exitCode, 0) << decompiled.err;
  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());

  const struct {
    const char *Name;
    const char *HighCall;
    const char *Builtin;
    const char *PrivateName;
    const char *GenericBuiltin;
  } Cases[] = {
      {"test_ldclral", "__builtin_arm_ldclr(arg1, arg0, 8, 4)",
       "__builtin_arm_ldclr", "__neverd_a64_atomic_and", "__atomic_fetch_and"},
      {"test_ldeoral", "__builtin_arm_ldeor(arg1, arg0, 8, 4)",
       "__builtin_arm_ldeor", "__neverd_a64_atomic_xor", "__atomic_fetch_xor"},
      {"test_ldsetal", "__builtin_arm_ldset(arg1, arg0, 8, 4)",
       "__builtin_arm_ldset", "__neverd_a64_atomic_or", "__atomic_fetch_or"},
      {"test_ldsmaxal", "__builtin_arm_ldsmax(arg1, arg0, 8, 4)",
       "__builtin_arm_ldsmax", "__neverd_a64_atomic_smax",
       "__atomic_fetch_max"},
      {"test_ldsminal", "__builtin_arm_ldsmin(arg1, arg0, 8, 4)",
       "__builtin_arm_ldsmin", "__neverd_a64_atomic_smin",
       "__atomic_fetch_min"},
      {"test_ldumaxal", "__builtin_arm_ldumax(arg1, arg0, 8, 4)",
       "__builtin_arm_ldumax", "__neverd_a64_atomic_umax",
       "__atomic_fetch_max"},
      {"test_lduminal", "__builtin_arm_ldumin(arg1, arg0, 8, 4)",
       "__builtin_arm_ldumin", "__neverd_a64_atomic_umin",
       "__atomic_fetch_min"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    EXPECT_NE(high.out.find(Case.HighCall), std::string::npos) << high.out;
    EXPECT_EQ(high.out.find(Case.PrivateName), std::string::npos) << high.out;
    auto F = functionC(source, Case.Name);
    ASSERT_FALSE(F.empty()) << source;
    EXPECT_NE(F.find(Case.Builtin), std::string::npos) << F;
    EXPECT_EQ(F.find(Case.GenericBuiltin), std::string::npos) << F;
  }
  EXPECT_EQ(functionC(source, "test_ldclral").find("~arg1"), std::string::npos);

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, CasAndCaspUseOrderedAtomicCompareExchange) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  const struct {
    const char *Name;
    const char *Type;
    const char *Ordering;
    unsigned Align;
  } Cases[] = {{"test_cas", "i64", "monotonic monotonic", 8},
               {"test_casa", "i64", "acquire acquire", 8},
               {"test_casal", "i64", "acq_rel acquire", 8},
               {"test_casl", "i64", "release monotonic", 8},
               {"test_casb", "i8", "monotonic monotonic", 1},
               {"test_cash", "i16", "monotonic monotonic", 2},
               {"test_casp", "i128", "monotonic monotonic", 16},
               {"test_caspa", "i128", "acquire acquire", 16},
               {"test_caspal", "i128", "acq_rel acquire", 16},
               {"test_caspl", "i128", "release monotonic", 16}};
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    auto F = functionIR(r.out, Case.Name);
    ASSERT_FALSE(F.empty()) << r.out;
    EXPECT_NE(F.find("cmpxchg ptr"), std::string::npos) << F;
    EXPECT_NE(F.find(std::string(", ") + Case.Type + " "), std::string::npos)
        << F;
    EXPECT_NE(F.find(std::string(Case.Ordering) + ", align " +
                     std::to_string(Case.Align)),
              std::string::npos)
        << F;
    EXPECT_EQ(F.find("load i"), std::string::npos) << F;
    EXPECT_EQ(F.find("store i"), std::string::npos) << F;
  }
}

TEST_F(AArch64_Atomic, CasAndCaspHighCUseStandardCompareExchangeAndCompile) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("__atomic_compare_exchange_n"), std::string::npos)
      << source;
  EXPECT_NE(source.find("unsigned __int128"), std::string::npos) << source;
  EXPECT_EQ(source.find("__neverd_a64_cas"), std::string::npos) << source;

  for (const char *Name :
       {"test_cas", "test_casa", "test_casal", "test_casl", "test_casb",
        "test_cash", "test_casp", "test_caspa", "test_caspal", "test_caspl"}) {
    SCOPED_TRACE(Name);
    auto F = functionC(source, Name);
    ASSERT_FALSE(F.empty()) << source;
    size_t Count = 0;
    for (size_t Pos = F.find("__atomic_compare_exchange_n");
         Pos != std::string::npos;
         Pos = F.find("__atomic_compare_exchange_n", Pos + 1))
      ++Count;
    EXPECT_EQ(Count, 1u) << F;
  }

  auto Byte = functionC(source, "test_casb");
  ASSERT_FALSE(Byte.empty()) << source;
  EXPECT_NE(Byte.find("\n    uint8_t "), std::string::npos) << Byte;
  auto Half = functionC(source, "test_cash");
  ASSERT_FALSE(Half.empty()) << source;
  EXPECT_NE(Half.find("\n    uint16_t "), std::string::npos) << Half;

  EXPECT_NE(
      functionC(source, "test_casa").find("__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE"),
      std::string::npos);
  EXPECT_NE(functionC(source, "test_casal")
                .find("__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE"),
            std::string::npos);
  EXPECT_NE(
      functionC(source, "test_casl").find("__ATOMIC_RELEASE, __ATOMIC_RELAXED"),
      std::string::npos);

  expectPairedClangSyntax(cFile, source);
}

TEST_F(AArch64_Atomic, LdiappUsesOneAcquireI128LoadAndKeepsHighHalf) {
  auto r = liftToLLVMIR(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto F = functionIR(r.out, "test_ldiapp_second");
  ASSERT_FALSE(F.empty()) << r.out;
  EXPECT_NE(F.find("load atomic i128"), std::string::npos) << F;
  EXPECT_NE(F.find("acquire, align 16"), std::string::npos) << F;
  EXPECT_NE(F.find("lshr i128"), std::string::npos) << F;
  EXPECT_NE(F.find(", 64"), std::string::npos) << F;
  EXPECT_NE(F.find("ret i64"), std::string::npos) << F;
}

TEST_F(AArch64_Atomic, LdiappHighCUsesOneAcquireI128LoadAndCompiles) {
  auto r = decompileToHighC(testObj());
  ASSERT_EQ(r.exitCode, 0) << r.err;

  auto cFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(cFile));
  std::ifstream input(cFile);
  ASSERT_TRUE(input.good());
  std::string source((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("unsigned __int128"), std::string::npos) << source;
  EXPECT_NE(source.find("__atomic_load_n"), std::string::npos) << source;
  EXPECT_NE(source.find("__ATOMIC_ACQUIRE"), std::string::npos) << source;

  auto F = functionC(source, "test_ldiapp_second");
  ASSERT_FALSE(F.empty()) << source;
  EXPECT_NE(F.find("__int128"), std::string::npos) << F;
  auto Load = F.find("neverd_mem_load_acquire_");
  ASSERT_NE(Load, std::string::npos) << F;
  EXPECT_EQ(F.find("neverd_mem_load_acquire_", Load + 1), std::string::npos)
      << "LDIAPP must be evaluated exactly once:\n"
      << F;

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
