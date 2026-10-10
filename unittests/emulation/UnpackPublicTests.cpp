//===- UnpackPublicTests.cpp - Recovery through the shared SDK and CLI ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../TestProcess.h"
#include "UnpackGeneratedTestSupport.h"
#include "UnpackLibraryTestSupport.h"
#include "UnpackTestSupport.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/unpack/UnpackCLIStrings.h"
#include "neverd/unpack/UnpackStrings.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace {
using namespace neverd;
using namespace neverd::unpack::test;
namespace text = neverd::unpack::strings;

class UnpackPublic : public testing::Test {
protected:
  neverd_session_t Session = nullptr;
  std::filesystem::path Directory;
  void SetUp() override {
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
    llvm::SmallString<128> Path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-unpack", Path));
    Directory = Path.str().str();
  }
  void TearDown() override {
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);
    if (Session)
      neverd_session_destroy(Session);
  }
  /// The report, or nothing with the session error left for the caller.
  std::optional<llvm::json::Object> api(const std::string &Input,
                                        const std::string &Output,
                                        const char *Options) {
    const char *Report =
        neverd_unpack_json(Session, Input.c_str(), Output.c_str(), Options);
    if (!Report)
      return std::nullopt;
    auto Free = llvm::scope_exit([&] { neverd_free_string(Report); });
    auto Parsed = llvm::json::parse(Report);
    if (!Parsed) {
      ADD_FAILURE() << llvm::toString(Parsed.takeError());
      return std::nullopt;
    }
    return *Parsed->getAsObject();
  }
  /// Run the command and return its exit code and report text.
  std::pair<int, std::string> cli(const std::string &Input,
                                  const std::string &Output,
                                  const std::string &Options) {
    const auto Report = (Directory / StandardOutput).string();
    const auto Command =
        test::shellQuote(NEVERD_UNPACK_CLI) + " " + unpack_cli::Command + " " +
        test::shellQuote(Input) + " -" + unpack_cli::OutputOption + " " +
        test::shellQuote(Output) + " --" + unpack_cli::OptionsOption + "=" +
        test::shellQuote(Options) + test::redirectStdout(Report) +
        test::silenceStderr();
    const int Status = test::systemExitCode(test::runShellCommand(Command));
    const auto Bytes = readFile(Report);
    return {Status, std::string(Bytes.begin(), Bytes.end())};
  }
};

TEST_F(UnpackPublic, CAPIAndCLIWriteTheSameImageAndReport) {
  const auto Input = fixture(PlainPacked).string();
  const auto First = (Directory / FirstOutput).string();
  const auto Second = (Directory / SecondOutput).string();
  auto Report = api(Input, First, nullptr);
  if (!Report) {
    // Without any CPU transport there is nothing to compare.
    const std::string Reason = neverd_last_error(Session);
    if (Reason.find(Unavailable) != std::string::npos ||
        Reason == text::Disabled)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  EXPECT_EQ(Report->getString(text::OutcomeField), text::UnpackedOutcome);
  const Image Original = readImage(fixture(Plain));
  EXPECT_EQ(Report->getString(text::EntryField),
            llvm::utohexstr(Original.Entry, true));
  const auto *Output = Report->getObject(text::OutputField);
  ASSERT_NE(Output, nullptr);
  EXPECT_EQ(Output->getString(text::PathField), First);
  const auto Written = readFile(First);
  EXPECT_EQ(Output->getInteger(text::SizeField), int64_t(Written.size()));
  EXPECT_EQ(readImage(Written).Entry, Original.Entry);

  const auto [Status, Text] = cli(Input, Second, text::EmptyOptions);
  EXPECT_EQ(Status, unpack_cli::Success);
  EXPECT_EQ(readFile(Second), Written);
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto Other = *Parsed->getAsObject();
  // Only the requested path differs between the two reports.
  (*Other.getObject(text::OutputField))[text::PathField] = First;
  EXPECT_EQ(llvm::json::Value(std::move(Other)),
            llvm::json::Value(std::move(*Report)));
}

TEST_F(UnpackPublic, RunWithoutAnEntryWritesNothingAndIsIncomplete) {
  const auto Input = fixture(PlainPacked).string();
  const auto Output = (Directory / FirstOutput).string();
  auto Report = api(Input, Output, TinyLimitOptions);
  if (!Report)
    GTEST_SKIP() << neverd_last_error(Session);
  EXPECT_EQ(Report->getString(text::OutcomeField), text::NoEntryOutcome);
  EXPECT_EQ(Report->get(text::OutputField)->kind(), llvm::json::Value::Null);
  EXPECT_FALSE(std::filesystem::exists(Output));
  const auto [Status, Text] = cli(Input, Output, TinyLimitOptions);
  EXPECT_EQ(Status, unpack_cli::Incomplete);
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_NE(Text.find(text::NoEntryOutcome), std::string::npos);
}

TEST_F(UnpackPublic, ExplicitSnapshotsHaveTheSameCAPIAndCLIContract) {
  const auto Input = fixture(PlainPacked).string();
  const auto First = (Directory / FirstOutput).string();
  const auto Second = (Directory / SecondOutput).string();
  const char *Options = "{\"snapshot_only\":true}";
  auto Report = api(Input, First, Options);
  if (!Report) {
    const std::string Reason = neverd_last_error(Session);
    if (Reason.find(Unavailable) != std::string::npos ||
        Reason == text::Disabled)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  EXPECT_EQ(Report->getString(text::OutcomeField), text::SnapshotOutcome);
  ASSERT_TRUE(std::filesystem::exists(First));
  const auto [Status, Text] = cli(Input, Second, Options);
  EXPECT_EQ(Status, unpack_cli::Success) << Text;
  ASSERT_TRUE(std::filesystem::exists(Second));
  EXPECT_EQ(readFile(First), readFile(Second));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->getAsObject());
  EXPECT_EQ(Parsed->getAsObject()->getString(text::OutcomeField),
            text::SnapshotOutcome);
}

TEST_F(UnpackPublic, RuntimeRestorationHasTheSameCAPIAndCLIContract) {
#ifndef NEVERD_UNPACK_GENERATED_FIXTURE_DIR
  GTEST_SKIP() << generated::MissingTools;
#else
  const auto Original =
      readImage(std::filesystem::path(NEVERD_UNPACK_GENERATED_FIXTURE_DIR) /
                generated::X64Dir / generated::ProgramFile);
  for (unsigned Mode :
       {generated::OwnedRuntimeMode, generated::VirtualRuntimeMode}) {
    SCOPED_TRACE(Mode);
    const auto Packed = generated::pack(Original, Original.File, Mode);
    ASSERT_FALSE(HasFailure());
    const auto Input = (Directory / generated::PackedFile).string();
    const auto First = (Directory / FirstOutput).string();
    const auto Second = (Directory / SecondOutput).string();
    writeFile(Input, Packed);
    const char *Options =
        R"({"restore_runtime":true,"windows":{"peb_version":{"major":10,"minor":0,"build":19043,"platform":2}}})";
    auto Report = api(Input, First, Options);
    if (!Report) {
      const std::string Reason = neverd_last_error(Session);
      if (Reason.find(Unavailable) != std::string::npos ||
          Reason == text::Disabled)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    ASSERT_EQ(Report->getString(text::OutcomeField), "restored");
    ASSERT_TRUE(std::filesystem::exists(First));
    const auto [Status, Text] = cli(Input, Second, Options);
    EXPECT_EQ(Status, unpack_cli::Success) << Text;
    ASSERT_TRUE(std::filesystem::exists(Second));
    EXPECT_EQ(readFile(First), readFile(Second));
    auto Parsed = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_TRUE(Parsed->getAsObject());
    auto Other = *Parsed->getAsObject();
    (*Other.getObject(text::OutputField))[text::PathField] = First;
    EXPECT_EQ(llvm::json::Value(std::move(Other)),
              llvm::json::Value(std::move(*Report)));
  }
#endif
}

TEST_F(UnpackPublic, UnsupportedRuntimeStateNeverCreatesOrTruncatesOutput) {
#ifndef NEVERD_UNPACK_GENERATED_FIXTURE_DIR
  GTEST_SKIP() << generated::MissingTools;
#else
  const std::pair<const char *, unsigned> Cases[] = {
      {generated::ProgramFile, generated::HeapStateMode},
      {generated::TLSHeapProgramFile, generated::HeapStateMode},
      {generated::ProgramFile, generated::DirectServiceMode},
      {generated::ProgramFile, generated::LateDirectServiceMode},
      {generated::ProgramFile, generated::EncodedPointerMode},
      {generated::TLSHeapProgramFile, generated::EncodedPointerMode},
      {generated::ProgramFile, generated::DynamicTLSMode},
      {generated::ProgramFile, generated::DynamicFLSMode}};
  for (const auto &[File, Mode] : Cases) {
    SCOPED_TRACE(File);
    SCOPED_TRACE(Mode);
    const bool Dynamic =
        Mode == generated::DynamicTLSMode || Mode == generated::DynamicFLSMode;
    const auto Original =
        readImage(std::filesystem::path(NEVERD_UNPACK_GENERATED_FIXTURE_DIR) /
                  generated::X64Dir / File);
    const auto Packed = generated::pack(Original, Original.File, Mode);
    ASSERT_FALSE(HasFailure());
    const auto Input =
        (Directory / (std::string(File) + generated::PackedFile)).string();
    const auto Output =
        (Directory / (std::string(File) + std::to_string(Mode) + FirstOutput))
            .string();
    writeFile(Input, Packed);
    auto Report = api(Input, Output, nullptr);
    if (!Report) {
      const std::string Reason = neverd_last_error(Session);
      if (Reason.find(Unavailable) != std::string::npos ||
          Reason == text::Disabled)
        GTEST_SKIP() << Reason;
      FAIL() << Reason;
    }
    EXPECT_EQ(Report->getString(text::OutcomeField),
              text::UnsupportedStateOutcome);
    ASSERT_NE(Report->get(text::OutputField), nullptr);
    EXPECT_EQ(Report->get(text::OutputField)->kind(), llvm::json::Value::Null);
    const auto *State = Report->getObject(text::RuntimeStateField);
    ASSERT_NE(State, nullptr);
    EXPECT_EQ(State->getBoolean(text::HeapKnownField), true);
    ASSERT_TRUE(State->getInteger(text::HeapReferenceCountField));
    if (Mode == generated::HeapStateMode)
      EXPECT_GT(*State->getInteger(text::HeapReferenceCountField), 0);
    else if (Mode == generated::EncodedPointerMode) {
      EXPECT_EQ(State->getInteger("possible_encoded_pointers"), 1);
      EXPECT_EQ(State->getBoolean("encoded_pointer_inventory_known"), true);
      const auto *References = State->getArray("encoded_pointer_references");
      ASSERT_NE(References, nullptr);
      ASSERT_EQ(References->size(), 1u);
      const auto *Reference = References->front().getAsObject();
      ASSERT_NE(Reference, nullptr);
      EXPECT_EQ(Reference->getString("storage"),
                File == generated::TLSHeapProgramFile ? "thread_local"
                                                      : "image");
      EXPECT_TRUE(Reference->getString("value"));
      EXPECT_TRUE(Reference->getString("offset"));
    } else if (Dynamic) {
      EXPECT_EQ(State->getBoolean("dynamic_thread_local_inventory_known"),
                true);
      EXPECT_EQ(State->getInteger("live_dynamic_tls_slots"),
                Mode == generated::DynamicTLSMode ? 1 : 0);
      EXPECT_EQ(State->getInteger("live_dynamic_fls_slots"),
                Mode == generated::DynamicFLSMode ? 1 : 0);
    } else {
      EXPECT_EQ(*State->getInteger(text::HeapReferenceCountField), 0);
      ASSERT_TRUE(State->getInteger("direct_service_calls"));
      EXPECT_GT(*State->getInteger("direct_service_calls"), 0);
    }
    EXPECT_FALSE(std::filesystem::exists(Output));
    EXPECT_EQ(cli(Input, Output, text::EmptyOptions).first,
              unpack_cli::Incomplete);
    EXPECT_FALSE(std::filesystem::exists(Output));

    const std::vector<uint8_t> Existing{'k', 'e', 'e', 'p'};
    writeFile(Output, Existing);
    Report = api(Input, Output, nullptr);
    ASSERT_TRUE(Report) << neverd_last_error(Session);
    EXPECT_EQ(Report->getString(text::OutcomeField),
              text::UnsupportedStateOutcome);
    EXPECT_EQ(readFile(Output), Existing);
    const auto [Status, Text] = cli(Input, Output, text::EmptyOptions);
    EXPECT_EQ(Status, unpack_cli::Incomplete) << Text;
    EXPECT_EQ(readFile(Output), Existing);
    if (Mode == generated::EncodedPointerMode || Dynamic) {
      const char *Snapshot = "{\"snapshot_only\":true}";
      Report = api(Input, Output, Snapshot);
      ASSERT_TRUE(Report) << neverd_last_error(Session);
      EXPECT_EQ(Report->getString(text::OutcomeField), text::SnapshotOutcome);
      const auto *State = Report->getObject(text::RuntimeStateField);
      ASSERT_NE(State, nullptr);
      if (Mode == generated::EncodedPointerMode)
        EXPECT_EQ(State->getInteger("possible_encoded_pointers"), 1);
      else {
        EXPECT_EQ(State->getBoolean("dynamic_thread_local_inventory_known"),
                  true);
        EXPECT_EQ(State->getInteger("live_dynamic_tls_slots"),
                  Mode == generated::DynamicTLSMode ? 1 : 0);
        EXPECT_EQ(State->getInteger("live_dynamic_fls_slots"),
                  Mode == generated::DynamicFLSMode ? 1 : 0);
      }
      const auto Bytes = readFile(Output);
      EXPECT_FALSE(Bytes.empty());
      const auto [SnapshotStatus, SnapshotText] = cli(Input, Output, Snapshot);
      EXPECT_EQ(SnapshotStatus, unpack_cli::Success) << SnapshotText;
      EXPECT_EQ(readFile(Output), Bytes);
      auto Parsed = llvm::json::parse(SnapshotText);
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      ASSERT_TRUE(Parsed->getAsObject());
      EXPECT_EQ(*Parsed->getAsObject()->get(text::RuntimeStateField),
                *Report->get(text::RuntimeStateField));
    }
  }
#endif
}

TEST_F(UnpackPublic, CAPIAndCLIPreserveDLLExportsAndTLS) {
#ifndef NEVERD_UNPACK_LIBRARY_FIXTURE_DIR
  GTEST_SKIP() << test::library::MissingTools;
#else
  namespace library = unpack::test::library;
  const auto Fixtures =
      std::filesystem::path(NEVERD_UNPACK_LIBRARY_FIXTURE_DIR) / "X64";
  const auto Original = readImage(Fixtures / library::InputFile);
  const auto Input = (Directory / library::InputFile).string();
  const auto First = (Directory / "first.dll").string();
  const auto Second = (Directory / "second.dll").string();
  ASSERT_TRUE(library::write(Input, library::pack(Original, library::TLSMode)));
  llvm::json::Object Options{
      {"windows",
       llvm::json::Object{
           {"modules",
            llvm::json::Array{llvm::json::Object{
                {"name", library::DependencyFile},
                {"path", (Fixtures / library::DependencyFile).string()}}}}}}};
  const std::string Encoded =
      llvm::formatv("{0}", llvm::json::Value(std::move(Options))).str();
  auto Report = api(Input, First, Encoded.c_str());
  if (!Report) {
    const std::string Reason = neverd_last_error(Session);
    if (Reason.find(Unavailable) != std::string::npos ||
        Reason == text::Disabled)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  EXPECT_EQ(Report->getString(text::OutcomeField), text::UnpackedOutcome);
  const auto Image = readImage(readFile(First));
  EXPECT_TRUE(Image.FileCharacteristics & llvm::COFF::IMAGE_FILE_DLL);
  EXPECT_EQ(Image.Entry, Original.Entry);
  EXPECT_EQ(Image.Exports, Original.Exports);
  const auto [Status, Text] = cli(Input, Second, Encoded);
  EXPECT_EQ(Status, unpack_cli::Success);
  EXPECT_EQ(readFile(Second), readFile(First));
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->getAsObject()->getString(text::OutcomeField),
            text::UnpackedOutcome);
#endif
}

TEST_F(UnpackPublic, SetupFailuresAreErrorsNotReports) {
  const auto Input = fixture(PlainPacked).string();
  const auto Output = (Directory / FirstOutput).string();
  EXPECT_EQ(neverd_unpack_json(Session, nullptr, Output.c_str(), nullptr),
            nullptr);
  EXPECT_STREQ(neverd_last_error(Session), text::PathRequired);
  EXPECT_EQ(neverd_unpack_json(Session, Input.c_str(), "", nullptr), nullptr);
  EXPECT_STREQ(neverd_last_error(Session), text::OutputRequired);
  EXPECT_EQ(neverd_unpack_json(nullptr, Input.c_str(), Output.c_str(), nullptr),
            nullptr);
  // An image may never replace the input it was recovered from.
  EXPECT_FALSE(api(Input, Input, nullptr));
  const std::string Same = neverd_last_error(Session);
  EXPECT_TRUE(Same == text::SamePath || Same == text::Disabled) << Same;
  EXPECT_FALSE(api(Input, Output, UnknownOption));
  for (const char *Invalid :
       {R"({"restore_runtime":null})", R"({"restore_runtime":1})",
        R"({"restore_runtime":"yes"})",
        R"({"restore_runtime":true,"snapshot_only":true})"}) {
    EXPECT_FALSE(api(Input, Output, Invalid));
    EXPECT_EQ(cli(Input, Output, Invalid).first, unpack_cli::Error);
    EXPECT_FALSE(std::filesystem::exists(Output));
  }
  EXPECT_FALSE(api((Directory / Missing).string(), Output, nullptr));
  EXPECT_FALSE(std::filesystem::exists(Output));
  EXPECT_EQ(cli(Input, Output, UnknownOption).first, unpack_cli::Error);
  EXPECT_EQ(
      cli((Directory / Missing).string(), Output, text::EmptyOptions).first,
      unpack_cli::Error);
  EXPECT_FALSE(std::filesystem::exists(Output));
}
} // namespace
