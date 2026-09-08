//===- MobileAndroidTests.cpp - Native Android publication contracts
//-------===//
#include "MobileCommon.h"
#include "gtest/gtest.h"

#include <atomic>

using namespace neverd::mobile;
namespace {
struct Temporary {
  fs::path path;
  Temporary() {
    static std::atomic<uint64_t> sequence{0};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      path = fs::temp_directory_path() /
             ("neverd-native-android-test-" + std::to_string(stamp) + "-" +
              std::to_string(sequence++));
      if (fs::create_directory(path))
        return;
    }
    throw Error("cannot create exclusive Android test directory");
  }
  ~Temporary() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
std::string input(std::string name = "Lfixture/Value;") {
  return ".class public " + name +
         "\n.super Ljava/lang/Object;\n.method public static "
         "value(I)I\n.registers 1\nreturn p0\n.end method\n";
}
llvm::json::Object recover(const fs::path &source, const fs::path &output,
                           Limits limits = {}) {
  Options options;
  options.input = source;
  options.output = output;
  options.limits = limits;
  fs::create_directory(output);
  Budget budget(limits);
  return recoverAndroid(options, output, budget);
}
TEST(MobileAndroid, BuiltinSourceAndMetadataDescribeExactRecoveredInventory) {
  Temporary temporary;
  auto source = temporary.path / "input.smali",
       output = temporary.path / "output";
  writeFile(source, input());
  auto report = recover(source, output);
  EXPECT_EQ(report.getString("status"), "success");
  EXPECT_EQ(report.getString("input_kind"), "smali");
  auto *backend = report.getObject("backend");
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->getString("name"), "neverd");
  EXPECT_EQ(backend->getString("execution"), "builtin");
  auto *coverage = report.getObject("android_method_recovery");
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(coverage->getInteger("method_count"), 1);
  EXPECT_EQ(coverage->getInteger("recovered_method_count"), 1);
  EXPECT_EQ(report.getInteger("java_source_count"), 1);
  EXPECT_TRUE(fs::is_regular_file(output / "sources/fixture/Value.java"));
  auto metadata =
      parseJSON(readFile(output / "metadata/android-methods.json", 100000),
                "test metadata");
  EXPECT_EQ(metadata.getAsObject()->getInteger("recovered_method_count"), 1);
  EXPECT_FALSE(fs::exists(output / ".android-work"));
}
TEST(MobileAndroid, DirectoryInputKeepsOriginalClassOwnershipAndDeclarations) {
  Temporary temporary;
  auto source = temporary.path / "smali", output = temporary.path / "output";
  writeFile(source / "nested/Value.smali", input());
  writeFile(source / "Declarations.smali",
            ".class public abstract Lfixture/Declarations;\n.super "
            "Ljava/lang/Object;\n.method public abstract pending()I\n.end "
            "method\n.method public native nativeValue()J\n.end method\n");
  auto report = recover(source, output);
  EXPECT_EQ(report.getString("input_kind"), "smali-directory");
  EXPECT_EQ(report.getInteger("smali_count"), 2);
  auto *coverage = report.getObject("android_method_recovery");
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(coverage->getInteger("method_count"), 3);
  EXPECT_EQ(coverage->getInteger("recovered_method_count"), 1);
  EXPECT_EQ(coverage->getInteger("declaration_only_method_count"), 2);
  auto *names = report.getArray("input_code_files");
  ASSERT_NE(names, nullptr);
  ASSERT_EQ(names->size(), 2u);
  EXPECT_EQ((*names)[0].getAsString(), "Declarations.smali");
  EXPECT_EQ((*names)[1].getAsString(), "nested/Value.smali");
  EXPECT_FALSE(fs::exists(output / ".android-work"));
}
TEST(MobileAndroid, ConflictingOutputClassesAreRejectedBeforeFirstSourceWrite) {
  Temporary temporary;
  auto source = temporary.path / "smali", output = temporary.path / "output";
  writeFile(source / "A.smali", input("Lfixture/Value;"));
  writeFile(source / "B.smali", input("Lfixture/value;"));
  EXPECT_THROW(recover(source, output), Error);
  EXPECT_TRUE(fs::is_empty(output));
}
TEST(MobileAndroid, LateMalformedClassLeavesNoEarlierJavaBody) {
  Temporary temporary;
  auto source = temporary.path / "smali", output = temporary.path / "output";
  writeFile(source / "A.smali", input());
  writeFile(
      source / "B.smali",
      ".class public LBad;\n.super Ljava/lang/Object;\n.method public static "
      "fail()V\n.registers 0\nunknown-opcode\nreturn-void\n.end method\n");
  EXPECT_THROW(recover(source, output), Error);
  EXPECT_TRUE(fs::is_empty(output));
}
TEST(MobileAndroid, ArtifactDirectoriesAndMetadataConsumeFileBudget) {
  Temporary temporary;
  auto source = temporary.path / "input.smali",
       output = temporary.path / "output";
  writeFile(source, input());
  Limits limits;
  limits.max_files = 3;
  EXPECT_THROW(recover(source, output, limits), Error);
  EXPECT_TRUE(fs::is_empty(output));
}
TEST(MobileAndroid, ExpiredAnalysisCleansTemporaryCodeFiles) {
  Temporary temporary;
  auto source = temporary.path / "input.smali",
       output = temporary.path / "output";
  writeFile(source, input());
  fs::create_directory(output);
  Options options;
  options.input = source;
  options.output = output;
  Budget budget;
  budget.deadline = std::chrono::steady_clock::time_point::min();
  EXPECT_THROW(recoverAndroid(options, output, budget), Error);
  EXPECT_TRUE(fs::is_empty(output));
}
TEST(MobileAndroid, InvalidRawUTF8CannotEnterTheSmaliSourceModel) {
  Temporary temporary;
  auto source = temporary.path / "input.smali",
       output = temporary.path / "output";
  writeFile(source, input() + std::string(1, char(0xff)));
  EXPECT_THROW(recover(source, output), Error);
  EXPECT_TRUE(fs::is_empty(output));
}
TEST(MobileAndroid, ExplicitUnavailableBackendNeverSilentlyFallsBack) {
  Temporary temporary;
  auto source = temporary.path / "input.smali",
       output = temporary.path / "output";
  writeFile(source, input());
  fs::create_directory(output);
  Options options;
  options.input = source;
  options.output = output;
  options.jadx = pathText(temporary.path / "missing-jadx-backend");
  Budget budget;
  EXPECT_THROW(recoverAndroid(options, output, budget), Error);
  EXPECT_FALSE(fs::exists(output / "sources"));
  EXPECT_FALSE(fs::exists(output / ".android-work"));
}
fs::path fakeBackend(const fs::path &root, const std::string &name) {
  auto fixture = pathFromUTF8(NEVERD_MOBILE_PROCESS_FIXTURE);
  auto copied = root / pathFromUTF8(name + pathText(fixture.extension()));
  fs::copy_file(fixture, copied);
  return copied;
}
TEST(MobileAndroid, ExplicitBackendCannotExceedTemporaryWorkspaceByteBudget) {
  for (const auto &name :
       {"fake-jadx-version-growth", "fake-jadx-output-growth"}) {
    SCOPED_TRACE(name);
    Temporary temporary;
    auto source = temporary.path / "input.smali",
         output = temporary.path / "output";
    writeFile(source, input());
    fs::create_directory(output);
    Options options;
    options.input = source;
    options.output = output;
    options.jadx = pathText(fakeBackend(temporary.path, name));
    options.limits.max_bytes = 4096;
    Budget budget(options.limits);
    try {
      recoverAndroid(options, output, budget);
      FAIL() << "backend temporary growth was not rejected";
    } catch (const Error &error) {
      EXPECT_NE(std::string(error.what()).find("byte limits"),
                std::string::npos)
          << error.what();
    }
    EXPECT_FALSE(fs::exists(output / ".android-work"));
  }
}
TEST(MobileAndroid, ExplicitBackendWithinWorkspaceBudgetKeepsItsSource) {
  Temporary temporary;
  auto source = temporary.path / "input.smali",
       output = temporary.path / "output";
  writeFile(source, input());
  fs::create_directory(output);
  Options options;
  options.input = source;
  options.output = output;
  options.jadx = pathText(fakeBackend(temporary.path, "fake-jadx-valid"));
  options.limits.max_bytes = 4096;
  Budget budget(options.limits);
  auto report = recoverAndroid(options, output, budget);
  ASSERT_NE(report.getObject("backend"), nullptr);
  EXPECT_EQ(report.getObject("backend")->getString("name"), "jadx");
  EXPECT_EQ(report.getInteger("java_source_count"), 1);
  EXPECT_EQ(readFile(output / "sources/Fixture.java", 4096),
            "class Fixture {}\n");
  EXPECT_FALSE(fs::exists(output / ".android-work"));
}
} // namespace
