//===- MachOSignerTests.cpp - Controlled signer subprocess tests --------===//

#include "gtest/gtest.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"
#include "neverd/backend/codegen/MachO/MachOSigner.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd::macho_signing;

#define MACHO_SIGNER_TEST_TEXT(Name, Value) constexpr char Name[] = Value;
#define MACHO_SIGNER_TEST_VALUE(Name, Value) constexpr auto Name = Value;
#include "MachOSignerFixtures.def"

constexpr struct {
  uint64_t Bytes;
  uint8_t Exponent;
  const char *Text;
} Pages[] = {
#define MACHO_SIGNER_TEST_PAGE(Bytes, Exponent, Text) {Bytes, Exponent, Text},
#include "MachOSignerFixtures.def"
};

TEST(MachOSigner, VerifyUsesFixedProgramBoundedWaitAndNoInheritedStreams) {
  ProcessRequest Captured;
  const Operations Ops{[&](const ProcessRequest &Request) {
    Captured = Request;
    return ProcessResult{};
  }};

  llvm::Error Error = verifyStrict("/tmp/candidate", Ops);

  EXPECT_FALSE(static_cast<bool>(Error));
  EXPECT_EQ(Captured.Program, "/usr/bin/codesign");
  EXPECT_EQ(Captured.Arguments,
            (std::vector<std::string>{"/usr/bin/codesign", "--verify",
                                      "--strict", "/tmp/candidate"}));
  EXPECT_EQ(Captured.TimeoutSeconds, 30u);
  EXPECT_TRUE(Captured.DiscardStandardStreams);
  EXPECT_FALSE(Captured.InheritEnvironment);
  EXPECT_TRUE(Captured.Environment.empty());
}

TEST(MachOSigner, AdHocSigningUsesExactNoninteractiveIdentityArguments) {
  ProcessRequest Captured;
  const Operations Ops{[&](const ProcessRequest &Request) {
    Captured = Request;
    return ProcessResult{};
  }};

  llvm::Error Error =
      adHocResign("/tmp/backend", "com.neverd.fixture", std::nullopt, Ops);

  EXPECT_FALSE(static_cast<bool>(Error));
  EXPECT_EQ(Captured.Program, "/usr/bin/codesign");
  EXPECT_EQ(Captured.Arguments,
            (std::vector<std::string>{"/usr/bin/codesign", "--force", "--sign",
                                      "-", "--identifier", "com.neverd.fixture",
                                      "--timestamp=none", "/tmp/backend"}));
  EXPECT_EQ(Captured.TimeoutSeconds, 30u);
  EXPECT_TRUE(Captured.DiscardStandardStreams);
  EXPECT_FALSE(Captured.InheritEnvironment);
  EXPECT_TRUE(Captured.Environment.empty());
}

TEST(MachOSigner, EverySubprocessFailureIsBoundedAndActionable) {
  struct Case {
    ProcessResult Result;
    const char *Message;
  };
  const Case Cases[] = {
      {{-1, true, "permission denied"}, "failed to launch"},
      {{-2, false, "Child timed out"}, "timed out"},
      {{-2, false, "Killed: 9"}, "terminated by a signal"},
      {{7, false, {}}, "status 7"},
  };
  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Message);
    const Operations Ops{[&](const ProcessRequest &) { return Entry.Result; }};

    llvm::Error Error =
        adHocResign("/tmp/backend", "com.neverd.fixture", std::nullopt, Ops);

    ASSERT_TRUE(static_cast<bool>(Error));
    const std::string Detail = llvm::toString(std::move(Error));
    EXPECT_NE(Detail.find(Entry.Message), std::string::npos) << Detail;
    EXPECT_LE(Detail.size(), 512u);
  }
}

TEST(MachOSigner, EmptyPathOrIdentifierNeverLaunchesCodesign) {
  unsigned Calls = 0;
  const Operations Ops{[&](const ProcessRequest &) {
    ++Calls;
    return ProcessResult{};
  }};

  llvm::Error EmptyPath = verifyStrict("", Ops);
  llvm::Error EmptyIdentifier =
      adHocResign("/tmp/backend", "", std::nullopt, Ops);

  EXPECT_TRUE(static_cast<bool>(EmptyPath));
  EXPECT_TRUE(static_cast<bool>(EmptyIdentifier));
  llvm::consumeError(std::move(EmptyPath));
  llvm::consumeError(std::move(EmptyIdentifier));
  EXPECT_EQ(Calls, 0u);
}

TEST(MachOSigner, ExplicitPageSizeUsesBytesAndKeepsTheControlledProcess) {
  for (const auto &Page : Pages) {
    ProcessRequest Captured;
    const Operations Ops{[&](const ProcessRequest &Request) {
      Captured = Request;
      return ProcessResult{};
    }};
    auto Error = adHocResign(kCandidate, kIdentifier, Page.Bytes, Ops);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    ASSERT_GE(Captured.Arguments.size(), 3u);
    EXPECT_EQ(Captured.Arguments[Captured.Arguments.size() - 3], kPageOption);
    EXPECT_EQ(Captured.Arguments[Captured.Arguments.size() - 2], Page.Text);
    EXPECT_EQ(Captured.Arguments.back(), kCandidate);
    EXPECT_TRUE(Captured.DiscardStandardStreams);
    EXPECT_FALSE(Captured.InheritEnvironment);
    EXPECT_TRUE(Captured.Environment.empty());

    Error = adHocResign(kCandidate, kIdentifier, 0, Ops);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    EXPECT_EQ(Captured.Arguments[Captured.Arguments.size() - 2],
              kWholeFilePage);
  }
}

TEST(MachOSigner, InvalidPageGranularityNeverLaunchesCodesign) {
  const uint64_t InvalidPages[] = {
#define MACHO_SIGNER_TEST_INVALID_PAGE(Bytes) Bytes,
#include "MachOSignerFixtures.def"
  };
  unsigned Calls = 0;
  const Operations Ops{[&](const ProcessRequest &) {
    ++Calls;
    return ProcessResult{};
  }};
  for (uint64_t Bytes : InvalidPages) {
    auto Error = adHocResign(kCandidate, kIdentifier, Bytes, Ops);
    ASSERT_TRUE(bool(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find(kInvalidPage),
              std::string::npos);
  }
  EXPECT_EQ(Calls, 0u);
}

TEST(MachOSigner, NativeCodesignRetainsExplicitPageGranularity) {
#ifndef __APPLE__
  GTEST_SKIP() << kMissingHost;
#else
  llvm::SmallString<kPathCapacity> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(kTemporaryPrefix, Directory));
  const auto Cleanup = llvm::scope_exit(
      [&] { EXPECT_FALSE(llvm::sys::fs::remove_directories(Directory)); });
  for (const auto &Page : Pages) {
    SCOPED_TRACE(Page.Text);
    llvm::SmallString<kPathCapacity> Candidate(Directory);
    llvm::sys::path::append(Candidate, Page.Text);
    ASSERT_FALSE(
        llvm::sys::fs::copy_file(NEVERD_MACHO_SIGNATURE_FIXTURE, Candidate));
    // Start without a signature: shrinking an existing CodeDirectory can
    // leave old hash bytes in codesign's reserved tail. Publication preserves
    // the input page size; this control independently exercises both sizes.
    ASSERT_EQ(llvm::sys::ExecuteAndWait(
                  kCodesign, {kCodesign, kRemoveSignature, Candidate},
                  std::nullopt, {},
                  neverd::limits::backend::kMachOSignerCodesignTimeoutSeconds),
              0);
    auto Error = adHocResign(Candidate, kIdentifier, Page.Bytes);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    Error = verifyStrict(Candidate);
    ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    auto File = llvm::MemoryBuffer::getFile(Candidate);
    ASSERT_TRUE(bool(File));
    auto Profile = neverd::macho_signature::inspect(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>((*File)->getBufferStart()),
        (*File)->getBufferSize()));
    ASSERT_TRUE(bool(Profile)) << llvm::toString(Profile.takeError());
    ASSERT_EQ(Profile->Slices.size(), 1u);
    EXPECT_EQ(Profile->Slices.front().CodeDirectoryPageSize, Page.Exponent);
    EXPECT_TRUE(
        neverd::macho_signature::isCanonicalCodesignAdHocOutput(*Profile));
  }
#endif
}

} // namespace
