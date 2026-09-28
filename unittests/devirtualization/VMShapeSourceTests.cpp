//===- VMShapeSourceTests.cpp - Public VM shape execution matrix
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

namespace {

std::string shapeFile(const fs::path &Path) {
  std::ifstream Input(Path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(Input), {});
}

class VMShapeSourceTest : public NeverDLiftTest {
protected:
  static fs::path fixture(const char *Name) {
    return fs::path(TEST_SOURCE_DIR) / "fixtures" / Name;
  }

  static const char *const *assemblies() {
    static const char *const Names[] = {"generic_vm_pointer.S",
                                        "generic_vm_virtual_calls.S",
                                        "generic_vm_rolling.S", nullptr};
    return Names;
  }

  RunResult buildShapes(const fs::path &Output) {
    std::vector<std::string> Args{
        "-target",   "x86_64-linux-gnu", "-fuse-ld=lld",
        "-nostdlib", "-static",          "-Wl,-e,generic_vm_pointer_program"};
    for (auto Name = assemblies(); *Name; ++Name)
      Args.push_back(fixture(*Name).string());
    Args.insert(Args.end(), {"-o", Output.string()});
    return exec(NEVERD_TEST_CLANG, Args);
  }

  RunResult recover(const fs::path &Binary, const std::string &Name, bool LLVM,
                    const fs::path &Source, const fs::path &Report,
                    const std::string &Budget = {}) {
    std::vector<std::string> Args{
        "decompile", Binary.string(),  "--func",
        Name,        "--devirtualize", "--recovery-report=" + Report.string(),
        "-o",        Source.string()};
    if (LLVM)
      Args.push_back("--llvm");
    if (!Budget.empty())
      Args.push_back(Budget);
    return exec(ndBin(), Args);
  }
};

struct VMShapeCase {
  const char *Name;
  unsigned Kind;
  bool LLVM;
};

class VMShapeRoundTripTest : public VMShapeSourceTest,
                             public ::testing::WithParamInterface<VMShapeCase> {
};

TEST_P(VMShapeRoundTripTest, ZeroHintsPreserveIndependentArithmeticAndStores) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM shape recovery requires clang";
  const auto &[Name, Kind, LLVM] = GetParam();
  const auto Binary = tmpFile("shapes.elf");
  const auto Built = buildShapes(Binary);
  ASSERT_TRUE(Built.ok()) << Built.err;
  const auto Source = tmpFile("recovered.c");
  const auto Report = tmpFile("recovery.json");
  const auto Recovered = recover(Binary, Name, LLVM, Source, Report);
  ASSERT_TRUE(Recovered.ok()) << Recovered.err << shapeFile(Report);
  auto JSON = llvm::json::parse(shapeFile(Report));
  ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getBoolean("complete"), true);
  EXPECT_EQ(Object->getBoolean("controlComplete"), true);
  EXPECT_EQ(Object->getBoolean("discoverControlState"), true);
  for (const char *Key : {"controlRegisters", "controlFrameSlots"}) {
    const auto *Fields = Object->getArray(Key);
    ASSERT_NE(Fields, nullptr);
    EXPECT_TRUE(Fields->empty()) << "No hand-picked state locations are needed";
  }
  const auto *Reads = Object->getArray("immutableReads");
  ASSERT_NE(Reads, nullptr);
  EXPECT_FALSE(Reads->empty());
  const auto Harness = tmpFile("reference.c");
  std::ofstream(Harness) << shapeFile(Source)
                         << "\n#define GENERIC_SHAPE_SOURCE\n"
                         << "#define GENERIC_SHAPE_FUNCTION " << Name << "\n"
                         << "#define GENERIC_SHAPE_KIND " << Kind << "\n"
                         << shapeFile(fixture("generic_vm_shapes_reference.c"));
  std::ofstream(tmpFile("immintrin.h")).close();
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = tmpFile("reference");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG,
             {"-std=c11", Optimization, "-fno-inline", "-Werror=return-type",
              "-Werror=implicit-function-declaration", "-fsanitize=undefined",
              "-fsanitize-trap=undefined", "-I", tmp().string(),
              Harness.string(), "-o", Executable.string()});
    ASSERT_TRUE(Compiled.ok()) << Compiled.err << shapeFile(Source);
    const auto Ran = exec(Executable.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err << shapeFile(Source);
  }
}

INSTANTIATE_TEST_SUITE_P(
    PublicVMShapes, VMShapeRoundTripTest,
    ::testing::Values(VMShapeCase{"generic_vm_pointer_program", 0, false},
                      VMShapeCase{"generic_vm_pointer_program", 0, true},
                      VMShapeCase{"generic_vm_virtual_calls", 1, false},
                      VMShapeCase{"generic_vm_virtual_calls", 1, true},
                      VMShapeCase{"generic_vm_rolling_loop", 2, false},
                      VMShapeCase{"generic_vm_rolling_loop", 2, true}),
    [](const ::testing::TestParamInfo<VMShapeCase> &Info) {
      return std::string(Info.param.Name) +
             (Info.param.LLVM ? "_LLVMC" : "_HighC");
    });

TEST_F(VMShapeSourceTest, NativeShapesMatchOracleAcrossCallingConventions) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "native fixture execution requires an x64 Linux host";
#endif
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "native public VM shape execution requires clang";
  for (bool MSABI : {false, true})
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(MSABI ? "Win64" : "SysV");
      SCOPED_TRACE(Optimization);
      const auto Executable = tmpFile("native-reference");
      std::vector<std::string> Args{"-target",      "x86_64-linux-gnu",
                                    "-fuse-ld=lld", "-std=c11",
                                    "-no-pie",      Optimization};
      if (MSABI)
        Args.push_back("-DGENERIC_VM_MS_ABI");
      for (auto Name = assemblies(); *Name; ++Name)
        Args.push_back(fixture(*Name).string());
      Args.insert(Args.end(),
                  {fixture("generic_vm_shapes_reference.c").string(), "-o",
                   Executable.string()});
      const auto Compiled = exec(NEVERD_TEST_CLANG, Args);
      ASSERT_TRUE(Compiled.ok()) << Compiled.err;
      const auto Ran = exec(Executable.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err;
    }
}

TEST_F(VMShapeSourceTest, UnknownReturnCursorAndDecoderKeyNeverPublishSource) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM shape refusals require clang";
  const auto Binary = tmpFile("shapes.elf");
  ASSERT_TRUE(buildShapes(Binary).ok());
  for (const char *Name :
       {"generic_vm_virtual_return_unknown", "generic_vm_rolling_unknown_key"})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      const auto Source =
          tmpFile(std::string(Name) + std::to_string(LLVM) + ".c");
      const auto Report =
          tmpFile(std::string(Name) + std::to_string(LLVM) + ".json");
      const auto Recovered = recover(Binary, Name, LLVM, Source, Report);
      EXPECT_FALSE(Recovered.ok());
      EXPECT_FALSE(fs::exists(Source));
      auto JSON = llvm::json::parse(shapeFile(Report));
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
      const auto *Object = JSON->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getBoolean("controlComplete"), false);
      const auto Status = Object->getString("status");
      EXPECT_TRUE(Status == "unsupported" || Status == "unresolved-control")
          << shapeFile(Report);
    }
}

TEST_F(VMShapeSourceTest, InsufficientNodeBudgetNeverPublishesPartialPrograms) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "public VM shape budget checks require clang";
  const auto Binary = tmpFile("shapes.elf");
  ASSERT_TRUE(buildShapes(Binary).ok());
  for (const char *Name :
       {"generic_vm_pointer_program", "generic_vm_virtual_calls",
        "generic_vm_rolling_loop"})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Name);
      const auto Source =
          tmpFile(std::string(Name) + std::to_string(LLVM) + ".c");
      const auto Report =
          tmpFile(std::string(Name) + std::to_string(LLVM) + ".json");
      EXPECT_FALSE(
          recover(Binary, Name, LLVM, Source, Report, "--vm-max-nodes=1").ok());
      EXPECT_FALSE(fs::exists(Source));
      auto JSON = llvm::json::parse(shapeFile(Report));
      ASSERT_TRUE(bool(JSON)) << llvm::toString(JSON.takeError());
      const auto *Object = JSON->getAsObject();
      ASSERT_NE(Object, nullptr);
      EXPECT_EQ(Object->getBoolean("complete"), false);
      EXPECT_EQ(Object->getBoolean("controlComplete"), false);
      EXPECT_EQ(Object->getString("status"), "budget-exceeded");
      EXPECT_EQ(Object->getInteger("maxNodes"), 1);
    }
}

TEST_F(VMShapeSourceTest, PublicShapeAssemblySupportsELFAndCOFF) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "cross-target public VM shape assembly requires clang";
  for (const char *Triple : {"x86_64-linux-gnu", "x86_64-windows-msvc"})
    for (auto Name = assemblies(); *Name; ++Name) {
      SCOPED_TRACE(Triple);
      SCOPED_TRACE(*Name);
      const auto Compiled =
          exec(NEVERD_TEST_CLANG,
               {"-target", Triple, "-c", fixture(*Name).string(), "-o",
                tmpFile(std::string(*Name) + Triple + ".o").string()});
      EXPECT_TRUE(Compiled.ok()) << Compiled.err;
    }
}

} // namespace
