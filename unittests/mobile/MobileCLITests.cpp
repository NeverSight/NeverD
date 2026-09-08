//===- MobileCLITests.cpp - Real native mobile CLI deployment -------------===//
#include "MobileCommon.h"
#include "gtest/gtest.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <mutex>

#ifndef NEVERD_MOBILE_CLI
#error "Define NEVERD_MOBILE_CLI to the built native neverd executable"
#endif
#ifndef NEVERD_MOBILE_FIXTURES
#error                                                                         \
    "Define NEVERD_MOBILE_FIXTURES to the repository mobile fixture directory"
#endif

using namespace neverd::mobile;
namespace {
// runTool deliberately inherits the caller's cwd. A scoped change makes these
// integration tests exercise deployment outside the checkout without extending
// the process API or using a shell. Tests in this binary execute sequentially.
class OutsideDirectory {
  inline static std::mutex Mutex;
  std::unique_lock<std::mutex> Lock{Mutex};
  fs::path Previous = fs::current_path();

public:
  explicit OutsideDirectory(const fs::path &Directory) {
    fs::current_path(Directory);
  }
  ~OutsideDirectory() {
    std::error_code EC;
    fs::current_path(Previous, EC);
    if (EC)
      ADD_FAILURE() << "cannot restore test working directory: "
                    << EC.message();
  }
};

class MobileCLITest : public testing::Test {
protected:
  fs::path Root, Input;
  const fs::path Binary = fs::absolute(pathFromUTF8(NEVERD_MOBILE_CLI));
  unsigned Invocation = 0;

  void SetUp() override {
    ASSERT_TRUE(fs::is_regular_file(Binary)) << pathText(Binary);
    llvm::SmallString<256> Temporary;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
        pathText(fs::temp_directory_path() / "neverd native CLI outside"),
        Temporary));
    Root = fs::absolute(pathFromUTF8(Temporary.str().str()));
    Input = Root / pathFromUTF8("inputs \xce\xbb") / "Peer sample.smali";
    auto Fixture = pathFromUTF8(NEVERD_MOBILE_FIXTURES) / "Peer.smali";
    writeFile(Input, readFile(Fixture, 1024 * 1024));
  }
  void TearDown() override {
    std::error_code Ignored;
    fs::remove_all(Root, Ignored);
  }

  std::vector<std::string>
  command(const fs::path &Executable, const fs::path &Source,
          const fs::path &Output,
          std::initializer_list<std::string> Extra = {}) {
    std::vector<std::string> Args{pathText(Executable), "mobile",
                                  pathText(Source),     "-o",
                                  pathText(Output),     "--json"};
    Args.insert(Args.end(), Extra.begin(), Extra.end());
    return Args;
  }

  llvm::json::Value invoke(const fs::path &Executable, const fs::path &Source,
                           const fs::path &Output, bool Failure = false,
                           std::initializer_list<std::string> Extra = {}) {
    const auto Log = Root / "logs" / (std::to_string(Invocation++) + ".log");
    bool Rejected = false;
    {
      OutsideDirectory Outside(Root);
      try {
        // Preserve the normal DLL/plugin-host search environment. This test
        // proves mobile needs no Python helper, not that the optional embedded
        // Python plugin host has no native runtime dependency.
        runTool(
            command(Executable, Source, Output, Extra), Log, 30, {}, {},
            {{"NEVERD_PYTHON", pathText(Root / "missing interpreter")},
             {"NEVERD_JADX", pathText(Root / "missing compatibility tool")}});
      } catch (const Error &E) {
        Rejected = true;
        EXPECT_TRUE(Failure) << E.what();
        EXPECT_NE(std::string(E.what()).find("backend exited with status"),
                  std::string::npos)
            << E.what();
      }
    }
    EXPECT_EQ(Rejected, Failure);
    auto Report = parseJSON(readFile(Log, 16 * 1024 * 1024), "CLI report");
    const auto *Object = Report.getAsObject();
    EXPECT_NE(Object, nullptr);
    if (!Object)
      return Report;
    EXPECT_EQ(Object->getInteger("schema_version"), 1);
    EXPECT_EQ(Object->getString("status"), Failure ? "error" : "success");
    if (Failure) {
      auto Message = Object->getString("error");
      EXPECT_TRUE(Message && !Message->empty());
    }
    return Report;
  }

  void noStaging() const {
    for (const auto &Entry : fs::directory_iterator(Root))
      EXPECT_FALSE(
          pathText(Entry.path().filename()).starts_with(".neverd-mobile-"))
          << pathText(Entry.path());
  }

  fs::path deploy() const {
    auto Directory = Root / "native deployment";
    fs::create_directory(Directory);
    fs::copy_file(Binary, Directory / Binary.filename());
    // Executable-relative shared-library layouts differ between platforms and
    // build configurations. Copy only sibling native libraries, dereferencing
    // versioned .so aliases rather than depending on the old build directory.
    for (const auto &Entry : fs::directory_iterator(Binary.parent_path())) {
      auto Name = lowerASCII(pathText(Entry.path().filename()));
      if (Entry.is_regular_file() &&
          (Name.ends_with(".dll") || Name.ends_with(".dylib") ||
           Name.ends_with(".so") || Name.find(".so.") != std::string::npos))
        fs::copy_file(Entry.path(), Directory / Entry.path().filename());
    }
    return Directory / Binary.filename();
  }
};

TEST_F(MobileCLITest, RelocatedNativeCLIRecoversWithoutPythonMobileFiles) {
  auto Executable = deploy();
  for (const auto &Entry : fs::directory_iterator(Executable.parent_path())) {
    EXPECT_TRUE(Entry.is_regular_file()) << pathText(Entry.path());
    EXPECT_NE(lowerASCII(pathText(Entry.path().extension())), ".py");
  }
  EXPECT_FALSE(fs::exists(Executable.parent_path() / "mobile"));
  auto Output = Root / "recovered outside checkout";
  auto Report = invoke(Executable, Input, Output);
  const auto *Object = Report.getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_EQ(Object->getString("platform"), "android");
  EXPECT_EQ(Object->getString("source"), "Peer sample.smali");
  const auto *Backend = Object->getObject("backend");
  ASSERT_NE(Backend, nullptr);
  EXPECT_EQ(Backend->getString("name"), "neverd");
  EXPECT_EQ(Backend->getString("version"), "1");
  EXPECT_EQ(Backend->getString("execution"), "builtin");
  const auto *Coverage = Object->getObject("android_method_recovery");
  ASSERT_NE(Coverage, nullptr);
  EXPECT_EQ(Coverage->getInteger("method_count"), 2);
  EXPECT_EQ(Coverage->getInteger("recovered_method_count"), 2);
  EXPECT_EQ(Coverage->getInteger("declaration_only_method_count"), 0);
  EXPECT_EQ(Coverage->getInteger("unrecovered_method_count"), 0);
  EXPECT_EQ(Object->getInteger("java_source_count"), 1);
  auto Published = parseJSON(readFile(Output / "report.json", 1024 * 1024),
                             "published report");
  EXPECT_EQ(Report, Published);
  auto Metadata =
      parseJSON(readFile(Output / "metadata/android-methods.json", 1024 * 1024),
                "published coverage");
  ASSERT_NE(Metadata.getAsObject(), nullptr);
  EXPECT_EQ(*Coverage, *Metadata.getAsObject());
  auto Source = readFile(Output / "sources/fixture/Peer.java", 1024 * 1024);
  EXPECT_NE(Source.find("twice("), std::string::npos);
  EXPECT_NE(Source.find("greeting("), std::string::npos);
  EXPECT_EQ(jsonText(Report).find(pathText(Root)), std::string::npos);
  noStaging();
}

TEST_F(MobileCLITest, ExistingOutputSurvivesNativeJSONFailure) {
  auto Output = Root / "existing result";
  writeFile(Output / "keep", "original bytes");
  invoke(Binary, Input, Output, true);
  EXPECT_EQ(readFile(Output / "keep", 100), "original bytes");
  EXPECT_EQ(
      std::distance(fs::directory_iterator(Output), fs::directory_iterator()),
      1);
  noStaging();
}

TEST_F(MobileCLITest, InvalidRegisterFlowFailsWithoutPublishingPartialJava) {
  auto Invalid = Root / "uninitialized.smali";
  const std::string Source =
      ".class public LUninitialized;\n.super Ljava/lang/Object;\n"
      ".method public static value()I\n.registers 1\nreturn v0\n.end method\n";
  writeFile(Invalid, Source);
  auto Output = Root / "failed semantics";
  invoke(Binary, Invalid, Output, true);
  EXPECT_FALSE(fs::exists(Output));
  EXPECT_EQ(readFile(Invalid, 1024 * 1024), Source);
  noStaging();
}

TEST_F(MobileCLITest, GeneratedOutputBudgetFailureRemovesStagingAndResult) {
  auto Output = Root / "failed publication";
  // The input fits exactly, while even the emitted Java class and its report
  // exceed this budget. This reaches recovery instead of failing input sizing.
  auto Size = fs::file_size(Input);
  invoke(Binary, Input, Output, true, {"--max-bytes=" + std::to_string(Size)});
  EXPECT_FALSE(fs::exists(Output));
  noStaging();
}

TEST_F(MobileCLITest, RemovedPythonOptionIsRejectedByTheNativeArgumentParser) {
  auto Output = Root / "obsolete option output", Log = Root / "obsolete.log";
  {
    OutsideDirectory Outside(Root);
    EXPECT_THROW(
        runTool(command(Binary, Input, Output, {"--python", "missing"}), Log,
                30),
        Error);
  }
  auto Diagnostic = readFile(Log, 1024 * 1024);
  EXPECT_NE(Diagnostic.find("--python"), std::string::npos);
  EXPECT_NE(lowerASCII(Diagnostic).find("unknown"), std::string::npos);
  EXPECT_FALSE(fs::exists(Output));
  noStaging();
}
} // namespace
