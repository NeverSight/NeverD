//===- TranslationCLIContractTests.cpp - Translation CLI contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../TestProcess.h"
#include "NeverDTranslateOutput.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#ifdef _WIN32
#include "llvm/Support/Windows/WindowsSupport.h"
#endif

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TranslationCLIContract : public ::testing::Test {
protected:
  struct CommitProbe {
    bool FailClose = false;
    bool FailRename = false;
    bool FailRegister = false;
    unsigned CloseCalls = 0;
    unsigned RenameCalls = 0;
    unsigned RemoveCalls = 0;
    unsigned RegisterCalls = 0;
    unsigned UnregisterCalls = 0;
    std::vector<std::string> Calls;
  };

  void SetUp() override {
    Directory =
        fs::temp_directory_path() /
        ("neverd-translation-cli-" +
         std::to_string(neverd::test::currentProcessId()) + "-" +
         ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(Directory);
  }

  void TearDown() override {
    std::error_code EC;
    fs::remove_all(Directory, EC);
  }

  fs::path writeInput(llvm::ArrayRef<uint8_t> Bytes,
                      llvm::StringRef Name = "raw block.bin") const {
    const fs::path Path = Directory / Name.str();
    std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    EXPECT_TRUE(Stream.good());
    return Path;
  }

  struct ProcessResult {
    int ExitCode = -1;
    std::string StandardOutput;
    std::string StandardError;
  };

  ProcessResult run(llvm::StringRef Arguments) const {
    const fs::path StandardOutput = Directory / "stdout.txt";
    const fs::path StandardError = Directory / "stderr.txt";
    const std::string Command =
        neverd::test::shellQuote(NEVERD_BINARY) + " " + Arguments.str() +
        neverd::test::redirectOutput(StandardOutput.string(),
                                     StandardError.string());
    ProcessResult Result;
    Result.ExitCode =
        neverd::test::systemExitCode(neverd::test::runShellCommand(Command));
    Result.StandardOutput = readText(StandardOutput);
    Result.StandardError = readText(StandardError);
    return Result;
  }

  static std::vector<uint8_t> readBytes(const fs::path &Path) {
    std::ifstream Stream(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(Stream),
            std::istreambuf_iterator<char>()};
  }

  static std::string readText(const fs::path &Path) {
    std::ifstream Stream(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(Stream),
            std::istreambuf_iterator<char>()};
  }

  static llvm::Error commitWithProbe(const fs::path &Temporary,
                                     const fs::path &Output,
                                     CommitProbe &Probe) {
    auto Close = [&]() -> llvm::Error {
      ++Probe.CloseCalls;
      Probe.Calls.emplace_back("close");
      if (Probe.FailClose)
        return llvm::createStringError(std::errc::io_error,
                                       "injected close failure");
      return llvm::Error::success();
    };
    auto Rename = [&](llvm::StringRef From,
                      llvm::StringRef To) -> std::error_code {
      ++Probe.RenameCalls;
      Probe.Calls.emplace_back("rename");
      if (Probe.FailRename)
        return std::make_error_code(std::errc::permission_denied);
      return llvm::sys::fs::rename(From, To);
    };
    auto Discard = [&](llvm::StringRef Path) -> llvm::Error {
      ++Probe.RemoveCalls;
      Probe.Calls.emplace_back("remove");
      return llvm::errorCodeToError(llvm::sys::fs::remove(Path));
    };
    auto Register = [&](llvm::StringRef) -> llvm::Error {
      ++Probe.RegisterCalls;
      Probe.Calls.emplace_back("register");
      if (Probe.FailRegister)
        return llvm::createStringError(std::errc::operation_not_permitted,
                                       "injected registration failure");
      return llvm::Error::success();
    };
    auto Unregister = [&](llvm::StringRef) {
      ++Probe.UnregisterCalls;
      Probe.Calls.emplace_back("unregister");
    };
    const neverd::cli::detail::AtomicOutputCommitOperations Operations{
        Close, Rename, Discard, Register, Unregister};
    return neverd::cli::detail::commitTemporaryOutput(
        Temporary.string(), Output.string(), Operations);
  }

  void expectControlTransferObject(llvm::ArrayRef<uint8_t> GuestControl) const {
    const fs::path Input = writeInput(GuestControl);
    const fs::path Output = Directory / "translated control transfer.o";
    const ProcessResult Result =
        run("translate-object " + neverd::test::shellQuote(Input.string()) +
            " --format=elf --entry=401000 --generation=19 -o " +
            neverd::test::shellQuote(Output.string()));

    ASSERT_EQ(Result.ExitCode, 0) << Result.StandardError;
    EXPECT_TRUE(Result.StandardOutput.empty());
    const std::vector<uint8_t> Bytes = readBytes(Output);
    ASSERT_GE(Bytes.size(), 20u);
    EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes).take_front(4),
              (llvm::ArrayRef<uint8_t>{0x7f, 'E', 'L', 'F'}));
    EXPECT_EQ(Bytes[18], 0xb7);
  }

  fs::path Directory;
};

constexpr std::array<uint8_t, 8> SupportedBlock{0x48, 0x89, 0xf8, 0x48,
                                                0x83, 0xc0, 0x01, 0xc3};
constexpr std::array<uint8_t, 10> SupportedScalarBlock{
    0x48, 0x89, 0xd8, 0x48, 0x83, 0xe8, 0x02, 0xc2, 0x10, 0x00};

TEST(TranslationCLIResultContract, RejectsMismatchedGuestEntry) {
  neverd_translate_object_request_v1 Request{};
  Request.struct_size = sizeof(Request);
  Request.guest_bytes_size = 1;
  Request.entry_pc = 0x401000;
  Request.executable_generation = 17;
  Request.object_format = NEVERD_TRANSLATE_OBJECT_FORMAT_ELF;

  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  Result.ok = 1;
  Result.object_format = Request.object_format;
  Result.guest_entry_pc = Request.entry_pc + 1;
  Result.guest_byte_count = Request.guest_bytes_size;
  Result.executable_generation = Request.executable_generation;

  llvm::Error Error =
      neverd::cli::detail::validateTranslationResultBindingV1(Request, Result);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("guest entry"),
            std::string::npos);
}

TEST(TranslationCLIResultContract, RejectsMismatchedExecutableGeneration) {
  neverd_translate_object_request_v1 Request{};
  Request.struct_size = sizeof(Request);
  Request.guest_bytes_size = 1;
  Request.entry_pc = 0x401000;
  Request.executable_generation = 17;
  Request.object_format = NEVERD_TRANSLATE_OBJECT_FORMAT_ELF;

  neverd_translate_object_result_v1 Result{};
  Result.struct_size = sizeof(Result);
  Result.ok = 1;
  Result.object_format = Request.object_format;
  Result.guest_entry_pc = Request.entry_pc;
  Result.guest_byte_count = Request.guest_bytes_size;
  Result.executable_generation = Request.executable_generation + 1;

  llvm::Error Error =
      neverd::cli::detail::validateTranslationResultBindingV1(Request, Result);
  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("executable generation"),
            std::string::npos);
}

TEST(TranslationCLICommitProtocol,
     KeepsSignalCleanupProtectionAcrossCloseAndPublication) {
  bool CleanupProtected = false;
  std::vector<std::string> Calls;
  auto Close = [&]() -> llvm::Error {
    Calls.emplace_back("close");
    EXPECT_TRUE(CleanupProtected);
    return llvm::Error::success();
  };
  auto Rename = [&](llvm::StringRef, llvm::StringRef) -> std::error_code {
    Calls.emplace_back("rename");
    EXPECT_TRUE(CleanupProtected);
    return {};
  };
  auto Discard = [&](llvm::StringRef) -> llvm::Error {
    Calls.emplace_back("remove");
    return llvm::Error::success();
  };
  auto Register = [&](llvm::StringRef) -> llvm::Error {
    Calls.emplace_back("register");
    CleanupProtected = true;
    return llvm::Error::success();
  };
  auto Unregister = [&](llvm::StringRef) {
    Calls.emplace_back("unregister");
    EXPECT_TRUE(CleanupProtected);
    CleanupProtected = false;
  };
  const neverd::cli::detail::AtomicOutputCommitOperations Operations{
      Close, Rename, Discard, Register, Unregister};

  llvm::Error Error = neverd::cli::detail::commitTemporaryOutput(
      "candidate.tmp", "output.o", Operations);

  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  EXPECT_FALSE(CleanupProtected);
  EXPECT_EQ(Calls, (std::vector<std::string>{"register", "close", "rename",
                                             "unregister"}));
}

TEST_F(TranslationCLIContract, AtomicCommitReplacesOnlyAfterClose) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Temporary = writeInput(Replacement, "candidate.tmp");
  const fs::path Output = writeInput(Original, "existing.o");
  CommitProbe Probe;

  llvm::Error Error = commitWithProbe(Temporary, Output, Probe);

  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Replacement.begin(), Replacement.end())));
  EXPECT_FALSE(fs::exists(Temporary));
  EXPECT_EQ(Probe.CloseCalls, 1u);
  EXPECT_EQ(Probe.RenameCalls, 1u);
  EXPECT_EQ(Probe.RemoveCalls, 0u);
  EXPECT_EQ(Probe.RegisterCalls, 1u);
  EXPECT_EQ(Probe.UnregisterCalls, 1u);
  EXPECT_EQ(Probe.Calls, (std::vector<std::string>{"register", "close",
                                                   "rename", "unregister"}));
}

TEST_F(TranslationCLIContract,
       AtomicReplaceExistingFailureNeverMovesTheOldOutputAside) {
  unsigned ReplaceCalls = 0;
  auto Replace = [&](llvm::StringRef From,
                     llvm::StringRef To) -> std::error_code {
    ++ReplaceCalls;
    EXPECT_EQ(From, "candidate.tmp");
    EXPECT_EQ(To, "existing.o");
    return std::make_error_code(std::errc::permission_denied);
  };
  const neverd::cli::detail::AtomicOutputReplaceOperations Operations{Replace};

  const std::error_code Error =
      neverd::cli::detail::replaceTemporaryOutputWithoutMoveAside(
          "candidate.tmp", "existing.o", Operations);

  EXPECT_EQ(Error, std::make_error_code(std::errc::permission_denied));
  EXPECT_EQ(ReplaceCalls, 1u);
}

TEST_F(TranslationCLIContract,
       AtomicReplaceUsesTheSameSingleOperationForAMissingOutput) {
  unsigned ReplaceCalls = 0;
  auto Replace = [&](llvm::StringRef From,
                     llvm::StringRef To) -> std::error_code {
    ++ReplaceCalls;
    EXPECT_EQ(From, "candidate.tmp");
    EXPECT_EQ(To, "new.o");
    return {};
  };
  const neverd::cli::detail::AtomicOutputReplaceOperations Operations{Replace};

  const std::error_code Error =
      neverd::cli::detail::replaceTemporaryOutputWithoutMoveAside(
          "candidate.tmp", "new.o", Operations);

  EXPECT_FALSE(Error);
  EXPECT_EQ(ReplaceCalls, 1u);
}

TEST_F(TranslationCLIContract,
       AtomicCommitCloseFailurePreservesOutputAndRemovesTemporary) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Temporary = writeInput(Replacement, "candidate.tmp");
  const fs::path Output = writeInput(Original, "existing.o");
  CommitProbe Probe;
  Probe.FailClose = true;

  llvm::Error Error = commitWithProbe(Temporary, Output, Probe);

  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("injected close failure"),
            std::string::npos);
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Original.begin(), Original.end())));
  EXPECT_FALSE(fs::exists(Temporary));
  EXPECT_EQ(Probe.CloseCalls, 1u);
  EXPECT_EQ(Probe.RenameCalls, 0u);
  EXPECT_EQ(Probe.RemoveCalls, 1u);
  EXPECT_EQ(Probe.RegisterCalls, 1u);
  EXPECT_EQ(Probe.UnregisterCalls, 1u);
  EXPECT_EQ(Probe.Calls, (std::vector<std::string>{"register", "close",
                                                   "remove", "unregister"}));
}

TEST_F(TranslationCLIContract,
       AtomicCommitRenameFailurePreservesOutputAndRemovesTemporary) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Temporary = writeInput(Replacement, "candidate.tmp");
  const fs::path Output = writeInput(Original, "existing.o");
  CommitProbe Probe;
  Probe.FailRename = true;

  llvm::Error Error = commitWithProbe(Temporary, Output, Probe);

  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Original.begin(), Original.end())));
  EXPECT_FALSE(fs::exists(Temporary));
  EXPECT_EQ(Probe.CloseCalls, 1u);
  EXPECT_EQ(Probe.RenameCalls, 1u);
  EXPECT_EQ(Probe.RemoveCalls, 1u);
  EXPECT_EQ(Probe.RegisterCalls, 1u);
  EXPECT_EQ(Probe.UnregisterCalls, 1u);
  EXPECT_EQ(Probe.Calls,
            (std::vector<std::string>{"register", "close", "rename", "remove",
                                      "unregister"}));
}

TEST_F(TranslationCLIContract,
       AtomicCommitRegistrationFailurePreservesOutputAndRemovesTemporary) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Temporary = writeInput(Replacement, "candidate.tmp");
  const fs::path Output = writeInput(Original, "existing.o");
  CommitProbe Probe;
  Probe.FailRegister = true;

  llvm::Error Error = commitWithProbe(Temporary, Output, Probe);

  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("registration failure"),
            std::string::npos);
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Original.begin(), Original.end())));
  EXPECT_FALSE(fs::exists(Temporary));
  EXPECT_EQ(Probe.CloseCalls, 0u);
  EXPECT_EQ(Probe.RenameCalls, 0u);
  EXPECT_EQ(Probe.RemoveCalls, 1u);
  EXPECT_EQ(Probe.RegisterCalls, 1u);
  EXPECT_EQ(Probe.UnregisterCalls, 0u);
  EXPECT_EQ(Probe.Calls, (std::vector<std::string>{"register", "remove"}));
}

TEST_F(TranslationCLIContract,
       DiscardCloseFailureStillRemovesAndForgetsTheTemporary) {
  llvm::Expected<llvm::sys::fs::TempFile> TemporaryOrError =
      llvm::sys::fs::TempFile::create(
          (Directory / "discard-close-%%%%%%").string());
  ASSERT_TRUE(static_cast<bool>(TemporaryOrError))
      << llvm::toString(TemporaryOrError.takeError());
  llvm::sys::fs::TempFile Temporary = std::move(*TemporaryOrError);
  const fs::path TemporaryPath = Temporary.TmpName;
  ASSERT_FALSE(llvm::sys::Process::SafelyCloseFileDescriptor(Temporary.FD));

  llvm::Error Error = neverd::cli::detail::discardTemporaryOutput(Temporary);

  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error))
                .find("cannot close temporary translation output"),
            std::string::npos);
  EXPECT_FALSE(fs::exists(TemporaryPath));
  EXPECT_EQ(Temporary.FD, -1);
  EXPECT_TRUE(Temporary.TmpName.empty());
}

TEST_F(TranslationCLIContract,
       CommitCloseFailureReportsContextAndPreservesTheOldOutput) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  const fs::path Output = writeInput(Original, "close-failure-existing.o");
  llvm::Expected<llvm::sys::fs::TempFile> TemporaryOrError =
      llvm::sys::fs::TempFile::create(Output.string() + ".tmp-%%%%%%");
  ASSERT_TRUE(static_cast<bool>(TemporaryOrError))
      << llvm::toString(TemporaryOrError.takeError());
  llvm::sys::fs::TempFile Temporary = std::move(*TemporaryOrError);
  const fs::path TemporaryPath = Temporary.TmpName;
  ASSERT_FALSE(llvm::sys::Process::SafelyCloseFileDescriptor(Temporary.FD));

  llvm::Error Error = neverd::cli::detail::closeAndCommitTemporaryOutput(
      Temporary, Output.string());

  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error))
                .find("cannot close temporary translation output"),
            std::string::npos);
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Original.begin(), Original.end())));
  EXPECT_FALSE(fs::exists(TemporaryPath));
  EXPECT_EQ(Temporary.FD, -1);
  EXPECT_TRUE(Temporary.TmpName.empty());
}

#ifdef _WIN32
TEST_F(TranslationCLIContract,
       WindowsAtomicCommitPublishesWhenTheOutputDoesNotExist) {
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Output = Directory / "new-output.o";
  llvm::Expected<llvm::sys::fs::TempFile> TemporaryOrError =
      llvm::sys::fs::TempFile::create(Output.string() + ".tmp-%%%%%%");
  ASSERT_TRUE(static_cast<bool>(TemporaryOrError))
      << llvm::toString(TemporaryOrError.takeError());
  llvm::sys::fs::TempFile Temporary = std::move(*TemporaryOrError);
  const fs::path TemporaryPath = Temporary.TmpName;
  {
    llvm::raw_fd_ostream Stream(Temporary.FD, false);
    Stream.write(reinterpret_cast<const char *>(Replacement.data()),
                 Replacement.size());
    Stream.flush();
    ASSERT_FALSE(Stream.has_error());
  }

  llvm::Error Error = neverd::cli::detail::closeAndCommitTemporaryOutput(
      Temporary, Output.string());

  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Replacement.begin(), Replacement.end())));
  EXPECT_FALSE(fs::exists(TemporaryPath));
}

TEST_F(TranslationCLIContract,
       WindowsAtomicCommitReplacesAnExistingUnlockedOutput) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Output = writeInput(Original, "existing-unlocked.o");
  llvm::Expected<llvm::sys::fs::TempFile> TemporaryOrError =
      llvm::sys::fs::TempFile::create(Output.string() + ".tmp-%%%%%%");
  ASSERT_TRUE(static_cast<bool>(TemporaryOrError))
      << llvm::toString(TemporaryOrError.takeError());
  llvm::sys::fs::TempFile Temporary = std::move(*TemporaryOrError);
  const fs::path TemporaryPath = Temporary.TmpName;
  {
    llvm::raw_fd_ostream Stream(Temporary.FD, false);
    Stream.write(reinterpret_cast<const char *>(Replacement.data()),
                 Replacement.size());
    Stream.flush();
    ASSERT_FALSE(Stream.has_error());
  }

  llvm::Error Error = neverd::cli::detail::closeAndCommitTemporaryOutput(
      Temporary, Output.string());

  ASSERT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Replacement.begin(), Replacement.end())));
  EXPECT_FALSE(fs::exists(TemporaryPath));
}

TEST_F(TranslationCLIContract,
       LockedWindowsDestinationFailsWithoutReplacingTheOldOutput) {
  constexpr std::array<uint8_t, 3> Original{0xaa, 0xbb, 0xcc};
  constexpr std::array<uint8_t, 4> Replacement{0x10, 0x20, 0x30, 0x40};
  const fs::path Output = writeInput(Original, "locked-existing.o");
  llvm::Expected<llvm::sys::fs::TempFile> TemporaryOrError =
      llvm::sys::fs::TempFile::create(Output.string() + ".tmp-%%%%%%");
  ASSERT_TRUE(static_cast<bool>(TemporaryOrError))
      << llvm::toString(TemporaryOrError.takeError());
  llvm::sys::fs::TempFile Temporary = std::move(*TemporaryOrError);
  const fs::path TemporaryPath = Temporary.TmpName;
  {
    llvm::raw_fd_ostream Stream(Temporary.FD, false);
    Stream.write(reinterpret_cast<const char *>(Replacement.data()),
                 Replacement.size());
    Stream.flush();
    ASSERT_FALSE(Stream.has_error());
  }

  llvm::SmallVector<wchar_t, 128> WideOutput;
  ASSERT_FALSE(llvm::sys::windows::widenPath(Output.string(), WideOutput));
  llvm::ScopedFileHandle LockedOutput(::CreateFileW(
      WideOutput.begin(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(static_cast<bool>(LockedOutput));

  llvm::Error Error = neverd::cli::detail::closeAndCommitTemporaryOutput(
      Temporary, Output.string());

  ASSERT_TRUE(static_cast<bool>(Error));
  EXPECT_NE(llvm::toString(std::move(Error))
                .find("cannot atomically replace translation output"),
            std::string::npos);
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Original.begin(), Original.end())));
  EXPECT_FALSE(fs::exists(TemporaryPath));
}
#endif

TEST_F(TranslationCLIContract, EmitsAArch64ELFRelocatableObject) {
  const fs::path Input = writeInput(SupportedScalarBlock);
  const fs::path Output = Directory / "translated ELF.o";
  const ProcessResult Result =
      run("translate-object " + neverd::test::shellQuote(Input.string()) +
          " --format=elf --entry=401000 --generation=17 -o " +
          neverd::test::shellQuote(Output.string()));

  ASSERT_EQ(Result.ExitCode, 0) << Result.StandardError;
  EXPECT_TRUE(Result.StandardOutput.empty());
  const std::vector<uint8_t> Bytes = readBytes(Output);
  ASSERT_GE(Bytes.size(), 20u);
  EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes).take_front(4),
            (llvm::ArrayRef<uint8_t>{0x7f, 'E', 'L', 'F'}));
  EXPECT_EQ(Bytes[16], 1u); // ET_REL, little endian.
  EXPECT_EQ(Bytes[17], 0u);
  EXPECT_EQ(Bytes[18], 0xb7); // EM_AARCH64, little endian.
  EXPECT_EQ(Bytes[19], 0u);
}

TEST_F(TranslationCLIContract, EmitsAArch64MachORelocatableObject) {
  const fs::path Input = writeInput(SupportedBlock);
  const fs::path Output = Directory / "translated MachO.o";
  const ProcessResult Result =
      run("translate-object " + neverd::test::shellQuote(Input.string()) +
          " --format=macho --entry=401000 --generation=18 -o " +
          neverd::test::shellQuote(Output.string()));

  ASSERT_EQ(Result.ExitCode, 0) << Result.StandardError;
  const std::vector<uint8_t> Bytes = readBytes(Output);
  ASSERT_GE(Bytes.size(), 16u);
  EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes).take_front(4),
            (llvm::ArrayRef<uint8_t>{0xcf, 0xfa, 0xed, 0xfe}));
  EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes).slice(4, 4),
            (llvm::ArrayRef<uint8_t>{0x0c, 0x00, 0x00, 0x01}));
  EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes).slice(12, 4),
            (llvm::ArrayRef<uint8_t>{0x01, 0x00, 0x00, 0x00}));
}

TEST_F(TranslationCLIContract, EmitsObjectForCanonicalShortDirectJump) {
  constexpr std::array<uint8_t, 2> DirectJump{0xeb, 0xfe};
  expectControlTransferObject(DirectJump);
}

TEST_F(TranslationCLIContract, EmitsObjectForCanonicalNearDirectJump) {
  constexpr std::array<uint8_t, 5> DirectJump{0xe9, 0xfb, 0xff, 0xff, 0xff};
  expectControlTransferObject(DirectJump);
}

TEST_F(TranslationCLIContract,
       EmitsObjectsForCanonicalZeroFlagConditionalBranches) {
  constexpr std::array<uint8_t, 2> ShortEqual{0x74, 0xfe};
  constexpr std::array<uint8_t, 6> NearNotEqual{0x0f, 0x85, 0xfa,
                                                0xff, 0xff, 0xff};
  expectControlTransferObject(ShortEqual);
  expectControlTransferObject(NearNotEqual);
}

TEST_F(TranslationCLIContract,
       EmitsObjectsForCanonicalSingleFlagConditionalBranches) {
  constexpr std::array<uint8_t, 2> ShortBelow{0x72, 0xfe};
  constexpr std::array<uint8_t, 6> NearNotSign{0x0f, 0x89, 0xfa,
                                               0xff, 0xff, 0xff};
  expectControlTransferObject(ShortBelow);
  expectControlTransferObject(NearNotSign);
}

TEST_F(TranslationCLIContract,
       EmitsObjectsForCanonicalMultiFlagConditionalBranches) {
  constexpr std::array<uint8_t, 2> ShortBelowOrEqual{0x76, 0xfe};
  constexpr std::array<uint8_t, 6> NearAbove{0x0f, 0x87, 0xfa,
                                             0xff, 0xff, 0xff};
  constexpr std::array<uint8_t, 2> ShortLess{0x7c, 0xfe};
  constexpr std::array<uint8_t, 6> NearGreaterOrEqual{0x0f, 0x8d, 0xfa,
                                                      0xff, 0xff, 0xff};
  constexpr std::array<uint8_t, 2> ShortLessOrEqual{0x7e, 0xfe};
  constexpr std::array<uint8_t, 6> NearGreater{0x0f, 0x8f, 0xfa,
                                               0xff, 0xff, 0xff};
  expectControlTransferObject(ShortBelowOrEqual);
  expectControlTransferObject(NearAbove);
  expectControlTransferObject(ShortLess);
  expectControlTransferObject(NearGreaterOrEqual);
  expectControlTransferObject(ShortLessOrEqual);
  expectControlTransferObject(NearGreater);
}

TEST_F(TranslationCLIContract, HelpPublishesCanonicalControlEncodings) {
  const ProcessResult Result = run("translate-object --help");

  ASSERT_EQ(Result.ExitCode, 0) << Result.StandardError;
  const std::string Help = Result.StandardOutput + Result.StandardError;
  EXPECT_NE(Help.find("direct-relative EB cb/E9 cd JMP"), std::string::npos)
      << Help;
  EXPECT_NE(Help.find("JE/JNE 74/75 cb or 0F 84/85 cd"), std::string::npos)
      << Help;
  EXPECT_NE(Help.find("JBE/JA 76/77 cb or 0F 86/87 cd"), std::string::npos)
      << Help;
  EXPECT_NE(Help.find("JLE/JG 7E/7F cb or 0F 8E/8F cd"), std::string::npos)
      << Help;
  EXPECT_NE(Help.find("JRCXZ/JECXZ/JCXZ and LOOP/LOOPE/LOOPNE remain "
                      "unpublished and fail closed"),
            std::string::npos)
      << Help;
  EXPECT_NE(Help.find("AArch64 relocatable object"), std::string::npos) << Help;
}

TEST_F(TranslationCLIContract, TypedFailureDoesNotReplaceExistingOutput) {
  const fs::path Input = writeInput({});
  const fs::path Output = Directory / "existing.o";
  const std::array<uint8_t, 4> Original{0xde, 0xad, 0xbe, 0xef};
  {
    std::ofstream Stream(Output, std::ios::binary | std::ios::trunc);
    Stream.write(reinterpret_cast<const char *>(Original.data()),
                 Original.size());
  }

  const ProcessResult Result =
      run("translate-object " + neverd::test::shellQuote(Input.string()) +
          " --format=elf -o " + neverd::test::shellQuote(Output.string()));

  EXPECT_EQ(Result.ExitCode, 2);
  EXPECT_NE(Result.StandardError.find("[invalid-argument]"), std::string::npos)
      << Result.StandardError;
  EXPECT_NE(Result.StandardError.find("guest byte range is empty"),
            std::string::npos)
      << Result.StandardError;
  EXPECT_EQ(readBytes(Output),
            (std::vector<uint8_t>(Original.begin(), Original.end())));
}

TEST_F(TranslationCLIContract, RejectsBytesAfterTheBlockTerminator) {
  constexpr std::array<uint8_t, 9> BlockWithTrailingByte{
      0x48, 0x89, 0xf8, 0x48, 0x83, 0xc0, 0x01, 0xc3, 0x90};
  const fs::path Input = writeInput(BlockWithTrailingByte);
  const fs::path Output = Directory / "must-not-exist.o";
  const ProcessResult Result =
      run("translate-object " + neverd::test::shellQuote(Input.string()) +
          " --format=elf -o " + neverd::test::shellQuote(Output.string()));

  EXPECT_EQ(Result.ExitCode, 2);
  EXPECT_NE(Result.StandardError.find("[invalid-argument]"), std::string::npos)
      << Result.StandardError;
  EXPECT_NE(Result.StandardError.find("trailing bytes"), std::string::npos)
      << Result.StandardError;
  EXPECT_FALSE(fs::exists(Output));
}

TEST_F(TranslationCLIContract, RequiresAnOutputPath) {
  const fs::path Input = writeInput(SupportedBlock);
  const ProcessResult Result =
      run("translate-object " + neverd::test::shellQuote(Input.string()) +
          " --format=elf");

  EXPECT_NE(Result.ExitCode, 0);
  EXPECT_NE(Result.StandardError.find("-o"), std::string::npos)
      << Result.StandardError;
}

} // namespace
