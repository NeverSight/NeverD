//===- BlobStoreTests.cpp - Private immutable blob storage tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private immutable blob storage tests.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Session.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace neverd::web;
namespace fs = std::filesystem;

template <typename F> void fails(F Call, const char *Code) {
  try {
    Call();
    FAIL() << "Expected " << Code;
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), Code);
  }
}

TEST(WebBlobs, EmptyAndInvalidRangesNeverAllocateOrWrap) {
  Blob Empty;
  EXPECT_EQ(Empty.size(), 0);
  EXPECT_EQ(Empty.read(0, 0), "");
  EXPECT_EQ(Empty.digest(), sha256(""));
  EXPECT_EQ(Empty.slice(0, 0).digest(), sha256(""));
  fails([&] { Empty.read(UINT64_MAX, 1); }, "invalid_blob_range");
  fails([&] { Empty.read(0, UINT64_MAX); }, "invalid_blob_range");
  fails([&] { Empty.slice(UINT64_MAX, 0); }, "invalid_blob_range");
  fails([&] { Empty.slice(0, UINT64_MAX); }, "invalid_blob_range");
}

#ifndef _WIN32
class WebBlobStore : public ::testing::Test {
protected:
  fs::path Root;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-web-blob-test", Path));
    Root = Path.str().str();
  }
  void TearDown() override {
    std::error_code EC;
    fs::remove_all(Root, EC);
  }
  void write(const char *Name, std::string_view Text) {
    std::ofstream Stream(Root / Name, std::ios::binary);
    Stream.write(Text.data(), Text.size());
    ASSERT_TRUE(Stream.good());
  }
  static std::set<int> descriptors() {
    std::set<int> Result;
#ifdef __APPLE__
    DIR *Entries = opendir("/dev/fd");
#else
    DIR *Entries = opendir("/proc/self/fd");
#endif
    if (!Entries) {
      ADD_FAILURE() << "Cannot enumerate owned descriptors";
      return Result;
    }
    const int Own = dirfd(Entries);
    while (const auto *Entry = readdir(Entries)) {
      char *End = nullptr;
      const long Number = std::strtol(Entry->d_name, &End, 10);
      if (End != Entry->d_name && !*End && Number >= 0 && Number != Own)
        Result.insert(int(Number));
    }
    closedir(Entries);
    return Result;
  }
  static std::string token(const std::string &Preview) {
    auto Parsed = llvm::json::parse(Preview);
    if (!Parsed) {
      ADD_FAILURE() << llvm::toString(Parsed.takeError());
      return {};
    }
    return Parsed->getAsObject()->getString("preview_token")->str();
  }
};

TEST_F(WebBlobStore, SnapshotBytesAndSlicesSurviveChangedDeletedInputs) {
  write("input", "original\nSECRET_BLOB_CANARY\nend");
  auto S = capture((Root / "input").string(), Limits{});
  ASSERT_EQ(S.Artifacts.size(), 1);
  const auto OriginalHash = S.Artifacts.front().BlobHash;
  Blob Data = S.Artifacts.front().Content;
  write("input", "modified");
  fs::remove(Root / "input");
  S = {};
  EXPECT_EQ(Data.read(0, Data.size()), "original\nSECRET_BLOB_CANARY\nend");
  EXPECT_EQ(Data.digest(), OriginalHash);
  Blob Slice = Data.slice(9, 18).slice(7, 4);
  Data = {};
  EXPECT_EQ(Slice.read(0, 4), "BLOB");
  EXPECT_EQ(Slice.digest(), sha256("BLOB"));
  EXPECT_EQ(Slice.read(4, 0), "");
  fails([&] { Slice.read(0, 4, 3); }, "blob_read_budget_exceeded");
  fails([&] { Slice.slice(3, 2); }, "invalid_blob_range");
  fails([&] { Slice.read(1, UINT64_MAX); }, "invalid_blob_range");
}

TEST_F(WebBlobStore, MembersShareOnePrivateUnlinkedNonInheritedDescriptor) {
  for (unsigned I = 0; I != 50; ++I)
    write(std::to_string(I).c_str(), "abc");
  const auto Before = descriptors();
  auto S = capture(Root.string(), Limits{});
  auto Added = descriptors();
  for (const auto FD : Before)
    Added.erase(FD);
  ASSERT_EQ(Added.size(), 1);
  const int FD = *Added.begin();
  struct stat Info{};
  ASSERT_EQ(fstat(FD, &Info), 0);
  EXPECT_TRUE(S_ISREG(Info.st_mode));
  EXPECT_EQ(Info.st_nlink, 0);
  EXPECT_EQ(Info.st_mode & 0777, 0400);
  EXPECT_EQ(Info.st_size, 150);
  EXPECT_NE(fcntl(FD, F_GETFD) & FD_CLOEXEC, 0);
  Blob Retained = S.Artifacts.back().Content;
  S = {};
  EXPECT_EQ(Retained.read(0, 3), "abc");
  Retained = {};
  EXPECT_EQ(fcntl(FD, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  EXPECT_EQ(descriptors(), Before);
}

TEST_F(WebBlobStore, ConcurrentReadsAndNestedViewsUseIndependentOffsets) {
  std::string Bytes(200000, '\0');
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = char(I % 251);
  write("input", Bytes);
  const auto S = capture((Root / "input").string(), Limits{});
  const auto Data = S.Artifacts.front().Content;
  std::vector<std::future<bool>> Jobs;
  for (unsigned K = 0; K != 4; ++K)
    Jobs.push_back(std::async(std::launch::async, [&, K] {
      for (unsigned I = 0; I != 200; ++I) {
        const auto Offset = (I * 389 + K * 877) % 190000;
        if (Data.slice(Offset, 10000).read(17, 991) !=
            Bytes.substr(Offset + 17, 991))
          return false;
      }
      return true;
    }));
  for (auto &Job : Jobs)
    EXPECT_TRUE(Job.get());
}

TEST_F(WebBlobStore, CaptureRequiresExactLengthSealingAndSharedBudget) {
  write("input", "abc");
  const int FD = open((Root / "input").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(FD, 0);
  for (const uint64_t Size : {2, 4}) {
    BlobStore Store(16);
    fails([&] { Store.capture(FD, Size); }, "input_changed");
    fails([&] { Store.seal(); }, "blob_store_closed");
  }
  BlobStore Store(6);
  auto First = Store.capture(FD, 3);
  EXPECT_EQ(First.Hash, sha256("abc"));
  fails([&] { First.Content.read(0, 3); }, "blob_not_sealed");
  fails([&] { First.Content.digest(); }, "blob_not_sealed");
  auto Second = Store.capture(FD, 3);
  fails([&] { Store.capture(FD, 1); }, "budget_exceeded");
  Store.seal();
  EXPECT_EQ(Second.Content.read(0, 3), "abc");
  EXPECT_EQ(First.Content.digest(), First.Hash);
  fails([&] { Store.capture(FD, 0); }, "blob_store_closed");
  fails([&] { Store.seal(); }, "blob_store_closed");
  close(FD);
  fails([] { BlobStore Store(0); }, "invalid_blob_store_budget");
  fails([] { BlobStore Store(UINT64_MAX); }, "invalid_blob_store_budget");
}

TEST_F(WebBlobStore, PreviewDiscardsStorageAndFailedCommitKeepsPublishedBytes) {
  write("input", "abc");
  const auto Before = descriptors();
  {
    Session S;
    const auto Path = (Root / "input").string();
    for (unsigned I = 0; I != 10; ++I) {
      S.preview(Path, "");
      EXPECT_EQ(descriptors(), Before);
    }
    const auto Published = S.commit(token(S.preview(Path, "")));
    const auto PublishedDescriptors = descriptors();
    EXPECT_EQ(PublishedDescriptors.size(), Before.size() + 1);
    const auto Pending = token(S.preview(Path, ""));
    EXPECT_EQ(descriptors(), PublishedDescriptors);
    write("input", "changed");
    fails([&] { S.commit(Pending); }, "input_changed");
    EXPECT_EQ(descriptors(), PublishedDescriptors);
    auto Meta = llvm::json::parse(S.metadata());
    ASSERT_TRUE(bool(Meta));
    EXPECT_EQ(Meta->getAsObject()->getString("revision"), "1");
    fails([&] { S.commit(Pending); }, "stale_preview");
    EXPECT_EQ(S.metadata().find("SECRET"), std::string::npos);
  }
  EXPECT_EQ(descriptors(), Before);
}

TEST_F(WebBlobStore, MaximumSnapshotUsesStreamingStorageAndBoundedReads) {
  const std::string Block(BlobTransferBytes, 'Q');
  llvm::SHA256 Hash;
  for (uint64_t I = 0; I < Limits::HardMemberBytes; I += Block.size())
    Hash.update(llvm::StringRef(Block));
  const auto ExpectedHash = llvm::toHex(Hash.final(), true);
  for (const auto *Name : {"one", "two"}) {
    std::ofstream Stream(Root / Name, std::ios::binary);
    for (uint64_t I = 0; I < Limits::HardMemberBytes; I += Block.size())
      Stream.write(Block.data(), Block.size());
    ASSERT_TRUE(Stream.good());
  }
  struct rusage Before{}, After{};
  ASSERT_EQ(getrusage(RUSAGE_SELF, &Before), 0);
  const auto S = capture(Root.string(), Limits{});
  ASSERT_EQ(S.InputBytes, Limits::HardInputBytes);
  ASSERT_EQ(S.Artifacts.size(), 3);
  for (const auto I : {1, 2}) {
    const auto &A = S.Artifacts[I];
    EXPECT_EQ(A.Content.size(), Limits::HardMemberBytes);
    EXPECT_EQ(A.BlobHash, ExpectedHash);
    EXPECT_EQ(A.Content.read(A.Content.size() - 10, 10), "QQQQQQQQQQ");
    fails([&] { A.Content.read(0, A.Content.size(), UINT64_MAX); },
          "blob_read_budget_exceeded");
    EXPECT_EQ(A.Content.slice(A.Content.size() - 10, 10).digest(),
              sha256("QQQQQQQQQQ"));
  }
  EXPECT_NE(S.Artifacts[1].ID, S.Artifacts[2].ID);
  ASSERT_EQ(getrusage(RUSAGE_SELF, &After), 0);
  uint64_t Growth =
      uint64_t(std::max<long>(0, After.ru_maxrss - Before.ru_maxrss));
#ifndef __APPLE__
  Growth *= 1024;
#endif
  RecordProperty("snapshot_bytes", std::to_string(S.InputBytes));
  RecordProperty("capture_peak_rss_growth_bytes", std::to_string(Growth));
  // This qualifies streaming import, not total parser/model/process memory.
  EXPECT_LT(Growth, 32 * 1024 * 1024);
  fs::resize_file(Root / "one", Limits::HardMemberBytes + 1);
  fails([&] { capture((Root / "one").string(), Limits{}); }, "budget_exceeded");
}

TEST_F(WebBlobStore, StorageWriteFailureCannotPublishOrLeakAProject) {
  write("small", "abc");
  write("large", std::string(BlobTransferBytes, 'S'));
  const pid_t Child = fork();
  ASSERT_GE(Child, 0);
  if (Child == 0) {
    // Fault injection is confined to this C++ test child. No input program,
    // script, helper executable or shell is launched.
    signal(SIGXFSZ, SIG_IGN);
    rlimit Limit{};
    if (getrlimit(RLIMIT_FSIZE, &Limit) != 0)
      _exit(10);
    Limit.rlim_cur = 16;
    if (setrlimit(RLIMIT_FSIZE, &Limit) != 0)
      _exit(11);
    try {
      Session S;
      S.commit(token(S.preview((Root / "small").string(), "")));
      const auto Metadata = S.metadata();
      const auto Open = descriptors();
      try {
        S.preview((Root / "large").string(), "");
        _exit(12);
      } catch (const Error &E) {
        if (std::string_view(E.what()) != "blob_storage_write_failed")
          _exit(13);
      }
      if (S.metadata() != Metadata || descriptors() != Open)
        _exit(14);
    } catch (...) {
      _exit(15);
    }
    _exit(0);
  }
  int Status;
  pid_t Done;
  do {
    Done = waitpid(Child, &Status, 0);
  } while (Done < 0 && errno == EINTR);
  ASSERT_EQ(Done, Child);
  ASSERT_TRUE(WIFEXITED(Status));
  EXPECT_EQ(WEXITSTATUS(Status), 0);
}
#endif
} // namespace
