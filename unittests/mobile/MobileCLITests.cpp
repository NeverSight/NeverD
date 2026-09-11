//===- MobileCLITests.cpp - Real native mobile CLI deployment -------------===//
#include "MobileCommon.h"
#include "gtest/gtest.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <mutex>
#include <string_view>

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


// An owned executable with one defined text symbol, not an executable payload
// for the test host. Both decoder architectures are exercised on every host.
std::string nativeMachO(bool AArch64) {
  std::string Bytes(4096, '\0');
  auto Put = [&](size_t Offset, uint64_t Value, unsigned Width = 4) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes.at(Offset + I) = static_cast<char>(Value >> (I * 8));
  };
  constexpr uint64_t Base = 0x100000000;
  constexpr size_t Text = 0x200, Symbols = 0x300, Strings = 0x310;
  const std::string_view Name = "_single_session";
  const std::string_view Code =
      AArch64 ? std::string_view("\xe0\x00\x80\x52\xc0\x03\x5f\xd6", 8)
              : std::string_view("\xb8\x07\x00\x00\x00\xc3", 6);
  Put(0, 0xfeedfacf);
  Put(4, AArch64 ? 0x0100000c : 0x01000007);
  Put(8, AArch64 ? 0 : 3);
  Put(12, 2); // MH_EXECUTE
  Put(16, 3);
  Put(20, 152 + 24 + 24);
  const size_t Segment = 32, Section = Segment + 72;
  Put(Segment, 0x19); // LC_SEGMENT_64
  Put(Segment + 4, 152);
  Bytes.replace(Segment + 8, 6, "__TEXT");
  Put(Segment + 24, Base, 8);
  Put(Segment + 32, Bytes.size(), 8);
  Put(Segment + 48, Bytes.size(), 8);
  Put(Segment + 56, 5);
  Put(Segment + 60, 5);
  Put(Segment + 64, 1);
  Bytes.replace(Section, 6, "__text");
  Bytes.replace(Section + 16, 6, "__TEXT");
  Put(Section + 32, Base + Text, 8);
  Put(Section + 40, Code.size(), 8);
  Put(Section + 48, Text);
  Put(Section + 52, AArch64 ? 2 : 0);
  Put(Section + 64, 0x80000400);
  const size_t Main = Segment + 152, Symtab = Main + 24;
  Put(Main, 0x80000028); // LC_MAIN
  Put(Main + 4, 24);
  Put(Main + 8, Text, 8);
  Put(Symtab, 2); // LC_SYMTAB
  Put(Symtab + 4, 24);
  Put(Symtab + 8, Symbols);
  Put(Symtab + 12, 1);
  Put(Symtab + 16, Strings);
  Put(Symtab + 20, Name.size() + 2);
  Put(Symbols, 1);
  Put(Symbols + 4, 0x0f, 1); // N_SECT | N_EXT
  Put(Symbols + 5, 1, 1);
  Put(Symbols + 8, Base + Text, 8);
  Bytes.replace(Strings + 1, Name.size(), Name);
  Bytes.replace(Text, Code.size(), Code);
  return Bytes;
}

size_t occurrences(std::string_view Text, std::string_view Needle) {
  size_t Count = 0, Offset = 0;
  while ((Offset = Text.find(Needle, Offset)) != std::string_view::npos) {
    ++Count;
    Offset += Needle.size();
  }
  return Count;
}

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
             {"NEVERD_JADX", pathText(Root / "missing compatibility tool")},
             {"NEVERD_NATIVE_PHASES", "1"}});
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

TEST_F(MobileCLITest, RelocatedIOSWorkerKeepsOneSessionAndMetadataAcrossISAs) {
  const auto Executable = deploy();
  for (bool AArch64 : {false, true}) {
    const std::string Architecture = AArch64 ? "arm64" : "x86_64";
    SCOPED_TRACE(Architecture);
    const auto Original = nativeMachO(AArch64);
    const auto Source = Input.parent_path() / (Architecture + " native.macho");
    writeFile(Source, Original);
    const auto Output = Root / (Architecture + " recovered");
    const auto Control = Root / (Architecture + " metadata only");
    const auto Report =
        invoke(Executable, Source, Output, false,
               {"--platform=ios", "--arch=" + Architecture, "--timeout=20"});
    const auto *Object = Report.getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getString("platform"), "ios");
    EXPECT_EQ(Object->getString("architecture"), Architecture);
    EXPECT_EQ(Object->getBoolean("metadata_only"), false);
    EXPECT_EQ(Object->getInteger("native_function_count"), 1);
    const auto *Outputs = Object->getObject("outputs");
    ASSERT_NE(Outputs, nullptr);
    EXPECT_EQ(Outputs->getString("native_log"), "logs/native.log");
    EXPECT_FALSE(Outputs->get("swift_native_log"));
    const auto *ObjC = Object->getObject("objc_method_recovery");
    const auto *Swift = Object->getObject("swift_method_recovery");
    ASSERT_NE(ObjC, nullptr);
    ASSERT_NE(Swift, nullptr);
    EXPECT_EQ(ObjC->getInteger("method_count"), 0);
    EXPECT_EQ(Swift->getInteger("method_count"), 0);
    const auto Native = readFile(Output / "sources/native.c", 1024 * 1024);
    EXPECT_FALSE(Native.empty());
    const auto Log = readFile(Output / "logs/native.log", 1024 * 1024);
    for (const auto *Phase : {"session_load", "objc_export"}) {
      const auto Prefix = std::string("[neverd-child-phase] phase=") + Phase;
      EXPECT_EQ(occurrences(Log, Prefix + " event=begin iteration=0 "), 1U);
      EXPECT_EQ(occurrences(Log, Prefix + " event=completed iteration=0 "), 1U);
      EXPECT_EQ(occurrences(Log, Prefix + " event="), 2U);
    }
    EXPECT_LT(Log.find("phase=session_load event=completed "),
              Log.find("phase=objc_export event=begin "));
    EXPECT_NE(Log.find("phase=pipeline event=completed iteration=0 "),
              std::string::npos);
    EXPECT_EQ(Log.find(" event=failed "), std::string::npos);
    EXPECT_EQ(Log.find(" event=aborted "), std::string::npos);
    EXPECT_EQ(std::distance(fs::directory_iterator(Output / "logs"),
                            fs::directory_iterator()),
              1);
    EXPECT_FALSE(fs::exists(Output / "artifacts/ios-worker-request.json"));
    EXPECT_FALSE(fs::exists(Output / "artifacts/ios-worker-result.json"));
    const auto Published =
        parseJSON(readFile(Output / "report.json", 1024 * 1024), "report");
    EXPECT_EQ(Report, Published);
    const auto Metadata =
        invoke(Executable, Source, Control, false,
               {"--platform=ios", "--arch=" + Architecture, "--metadata-only",
                "--timeout=20"});
    const auto *ControlObject = Metadata.getAsObject();
    ASSERT_NE(ControlObject, nullptr);
    EXPECT_EQ(ControlObject->getBoolean("metadata_only"), true);
    for (const auto *Key : {"native_function_count", "objc_method_recovery",
                            "swift_method_recovery"}) {
      const auto *Value = ControlObject->get(Key);
      ASSERT_NE(Value, nullptr) << Key;
      EXPECT_EQ(*Value, llvm::json::Value(nullptr)) << Key;
    }
    EXPECT_FALSE(fs::exists(Control / "logs"));
    EXPECT_EQ(readFile(Source, 1024 * 1024), Original);
    EXPECT_EQ(readFile(Output / "artifacts/selected.macho", 1024 * 1024),
              Original);
    EXPECT_EQ(readFile(Control / "artifacts/selected.macho", 1024 * 1024),
              Original);
    for (const auto *Name : {"swift.json", "objc.json"}) {
      SCOPED_TRACE(Name);
      const auto WorkerMetadata = parseJSON(
          readFile(Output / "metadata" / Name, 1024 * 1024), "worker metadata");
      const auto DirectMetadata = parseJSON(
          readFile(Control / "metadata" / Name, 1024 * 1024), "direct metadata");
      EXPECT_EQ(WorkerMetadata, DirectMetadata);
    }
    noStaging();
  }
}

TEST_F(MobileCLITest, IOSOutputBudgetFailureLeavesNoWorkerOrPartialResult) {
  const auto Source = Input.parent_path() / "bounded native.macho";
  const auto Original = nativeMachO(false);
  writeFile(Source, Original);
  const auto Output = Root / "failed native publication";
  // Slice publication fits exactly; metadata and source cannot fit as well.
  invoke(Binary, Source, Output, true,
         {"--platform=ios", "--arch=x86_64", "--timeout=20",
          "--max-bytes=" + std::to_string(Original.size())});
  EXPECT_FALSE(fs::exists(Output));
  EXPECT_EQ(readFile(Source, 1024 * 1024), Original);
  noStaging();
}
} // namespace
