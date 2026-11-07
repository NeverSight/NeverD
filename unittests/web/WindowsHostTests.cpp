//===- WindowsHostTests.cpp - Native NTFS analysis boundary tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native ownership, stream, sharing, privacy and disclosure qualification.
///
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#include "BlobStore.h"
#include "ExportDirectory.h"
#include "Internal.h"
#include "PathPolicy.h"
#include "gtest/gtest.h"
#include "platform/WindowsFile.h"
#include "rewrite/Export.h"

#include "neverd/web/Session.h"
#include "neverd/web/SourceRewrite.h"

#include "llvm/Support/FileSystem.h"

#include <aclapi.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <winioctl.h>

namespace {
using namespace neverd::web;
namespace fs = std::filesystem;
class WebWindowsHost : public ::testing::Test {
protected:
  fs::path Root;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-windows-web", Path));
    Root = win::wide(Path.str().str());
  }
  void TearDown() override {
    std::error_code EC;
    fs::remove_all(Root, EC);
  }
  static std::string path(const fs::path &P) { return win::utf8(P.native()); }
  static void write(const fs::path &P, std::string_view Bytes) {
    fs::create_directories(P.parent_path());
    std::ofstream F(P, std::ios::binary);
    F.write(Bytes.data(), Bytes.size());
    ASSERT_TRUE(F.good());
  }
  static std::string read(const fs::path &P) {
    std::ifstream F(P, std::ios::binary);
    return {std::istreambuf_iterator<char>(F), {}};
  }
  static void junction(const fs::path &Link, const fs::path &Target) {
    fs::create_directory(Link);
    win::Handle F(CreateFileW(
        Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    ASSERT_TRUE(F);
    struct Mount {
      DWORD Tag;
      WORD Length, Reserved, SubOffset, SubLength, PrintOffset, PrintLength;
      std::array<wchar_t, 4096> Names;
    } Data{};
    const std::wstring Sub = L"\\??\\" + Target.native();
    const auto &Print = Target.native();
    ASSERT_LT(Sub.size() + Print.size() + 2, Data.Names.size());
    Data.Tag = IO_REPARSE_TAG_MOUNT_POINT;
    Data.SubLength = WORD(Sub.size() * sizeof(wchar_t));
    Data.PrintOffset = Data.SubLength + sizeof(wchar_t);
    Data.PrintLength = WORD(Print.size() * sizeof(wchar_t));
    std::copy(Sub.begin(), Sub.end(), Data.Names.begin());
    std::copy(Print.begin(), Print.end(), Data.Names.begin() + Sub.size() + 1);
    Data.Length = WORD(8 + (Sub.size() + Print.size() + 2) * sizeof(wchar_t));
    DWORD Written;
    ASSERT_TRUE(DeviceIoControl(F.get(), FSCTL_SET_REPARSE_POINT, &Data,
                                8 + Data.Length, nullptr, 0, &Written,
                                nullptr));
  }
};

TEST_F(WebWindowsHost, RootAndAncestorJunctionsCannotEscape) {
  write(Root / "real" / "child" / "input.js", "PRIVATE_TARGET");
  junction(Root / "link", Root / "real");
  for (const auto Suffix : {"", "/", "/.", "/child/input.js"})
    EXPECT_THROW(capture(path(Root / "link") + Suffix, Limits{}), Error);
  EXPECT_THROW(capture(path(Root), Limits{}), Error);
  EXPECT_EQ(
      capture(path(Root / "real" / "child" / "input.js"), Limits{}).InputBytes,
      14u);
}
TEST_F(WebWindowsHost, HardLinksAndAlternateStreamsAreRefused) {
  write(Root / "outside", "PRIVATE_TARGET");
  fs::create_directory(Root / "input");
  fs::create_hard_link(Root / "outside", Root / "input" / "alias");
  EXPECT_THROW(capture(path(Root / "input"), Limits{}), Error);
  EXPECT_THROW(capture(path(Root / "outside"), Limits{}), Error);
  fs::remove(Root / "input" / "alias");
  write(Root / "outside:secret", "PRIVATE_ADS");
  EXPECT_THROW(capture(path(Root / "outside"), Limits{}), Error);
  EXPECT_THROW(capture(path(Root / "outside") + ":secret", Limits{}), Error);
  write(Root / "input:secret", "PRIVATE_DIRECTORY_ADS");
  EXPECT_THROW(capture(path(Root / "input"), Limits{}), Error);
}
TEST_F(WebWindowsHost,
       NamespaceAndInvalidUTF8CannotSelectDevicesOrRemoteInputs) {
  for (const auto *P :
       {"\\\\server\\share\\file", "\\\\?\\C:\\file", "\\\\.\\NUL",
        "C:relative", "C:\\file:stream", "C:\\NUL", "bad\xff"})
    EXPECT_THROW(capture(P, Limits{}), Error) << P;
}
TEST_F(WebWindowsHost, InputHandlesBlockWritersAndReplacement) {
  write(Root / "input", "abc");
  auto F = win::openRoot(path(Root / "input"));
  win::Handle Writer(
      CreateFileW((Root / "input").c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, 0, nullptr));
  EXPECT_FALSE(Writer);
  EXPECT_FALSE(
      MoveFileExW((Root / "input").c_str(), (Root / "moved").c_str(), 0));
  F = {};
  Writer = win::Handle(
      CreateFileW((Root / "input").c_str(), GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, 0, nullptr));
  ASSERT_TRUE(Writer);
  EXPECT_THROW(capture(path(Root / "input"), Limits{}), Error);
}
TEST_F(WebWindowsHost, EnumeratedIdentityDetectsReplacementBeforeOpen) {
  write(Root / "input", "old");
  auto Directory = win::openRoot(path(Root), true);
  const auto Entries = win::entries(Directory.get(), 1);
  ASSERT_EQ(Entries.size(), 1u);
  const auto DirectoryID = win::inspect(Directory.get()).id();
  // The retained enumeration directory prevents a path-based writer opening
  // its target parent. Check identity after releasing and reopening that lock.
  std::error_code Locked;
  fs::rename(Root / "input", Root / "old", Locked);
  ASSERT_TRUE(Locked);
  Directory = {};
  fs::rename(Root / "input", Root / "old");
  write(Root / "input", "new");
  Directory = win::openRoot(path(Root), true);
  ASSERT_EQ(win::inspect(Directory.get()).id(), DirectoryID);
  auto New = win::openChild(Directory.get(), "input");
  EXPECT_FALSE(Entries[0].matches(win::inspect(New.get())));
}
TEST_F(WebWindowsHost, UTF8NamesAndRelocationPreserveSnapshotIdentity) {
  if (!archivePathsAvailable())
    GTEST_SKIP() << "non-ASCII member names need the optional Unicode owner";
  const auto Name = win::wide("\xe6\xba\x90\xf0\x9f\x98\x80.js");
  write(Root / "one" / Name, "const x=1;");
  write(Root / "two" / Name, "const x=1;");
  const auto A = capture(path(Root / "one"), Limits{});
  const auto B = capture(path(Root / "two"), Limits{});
  ASSERT_EQ(A.Artifacts.size(), 2u);
  EXPECT_EQ(A.ID, B.ID);
  EXPECT_EQ(A.Artifacts[1].MemberPath, win::utf8(Name));
  EXPECT_EQ(A.Artifacts[1].Content.read(0, 10), "const x=1;");
}
TEST_F(WebWindowsHost, DirectoryEnumerationAndHandleRevisionRemainDistinct) {
  write(Root / "selected" / "one", "a");
  write(Root / "selected" / "two", "bad");
  write(Root / "selected" / "four" / "child", "abc");
  const auto S = capture(path(Root), Limits{});
  ASSERT_EQ(S.Artifacts.size(), 6u);
  auto Parent = win::openRoot(path(Root), true);
  const auto Entries = win::entries(Parent.get(), 1);
  ASSERT_EQ(Entries.size(), 1u);
  auto Directory = win::openChild(Parent.get(), "selected");
  const auto Before = win::inspect(Directory.get());
  auto Cached = Entries[0];
  Cached.WriteTime.QuadPart = Before.Times.LastWriteTime.QuadPart - 100000000;
  Cached.ChangeTime.QuadPart = Before.Times.ChangeTime.QuadPart - 100000000;
  EXPECT_TRUE(Cached.matches(Before));
  Cached.ID ^= 1;
  EXPECT_FALSE(Cached.matches(Before));
  Directory = {};
  Parent = {};
  {
    win::Handle Writer(CreateFileW(
        (Root / "selected").c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(Writer);
    ULARGE_INTEGER Stamp;
    Stamp.QuadPart = Before.Times.LastWriteTime.QuadPart + 100000000;
    const FILETIME Time{Stamp.LowPart, Stamp.HighPart};
    ASSERT_TRUE(SetFileTime(Writer.get(), nullptr, nullptr, &Time));
  }
  Directory = win::openRoot(path(Root / "selected"), true);
  const auto After = win::inspect(Directory.get());
  EXPECT_EQ(Before.id(), After.id());
  EXPECT_EQ(Before.Identity.dwFileAttributes, After.Identity.dwFileAttributes);
  EXPECT_NE(Before.Times.LastWriteTime.QuadPart,
            After.Times.LastWriteTime.QuadPart);
  EXPECT_FALSE(Before.same(After));
}
TEST_F(WebWindowsHost, SpoolIsUnlinkedPrivateNonInheritedAndReadOnlyAfterSeal) {
  auto F = win::privateSpool();
  BY_HANDLE_FILE_INFORMATION Info{};
  ASSERT_TRUE(GetFileInformationByHandle(F.get(), &Info));
  EXPECT_EQ(Info.nNumberOfLinks, 0u);
  DWORD Flags;
  ASSERT_TRUE(GetHandleInformation(F.get(), &Flags));
  EXPECT_EQ(Flags & HANDLE_FLAG_INHERIT, 0u);
  PSECURITY_DESCRIPTOR Descriptor = nullptr;
  PACL ACL = nullptr;
  ASSERT_EQ(GetSecurityInfo(F.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                            nullptr, nullptr, &ACL, nullptr, &Descriptor),
            ERROR_SUCCESS);
  ASSERT_NE(ACL, nullptr);
  SECURITY_DESCRIPTOR_CONTROL Control;
  DWORD Revision;
  EXPECT_TRUE(GetSecurityDescriptorControl(Descriptor, &Control, &Revision));
  EXPECT_NE(Control & SE_DACL_PROTECTED, 0);
  EXPECT_EQ(ACL->AceCount, 1u);
  LocalFree(Descriptor);
  win::writeAt(F.get(), "abc", 0);
  win::flush(F.get());
  F = win::readOnly(F.get());
  EXPECT_THROW(win::writeAt(F.get(), "x", 0), Error);
  std::array<char, 3> Bytes{};
  EXPECT_EQ(win::readAt(F.get(), Bytes.data(), Bytes.size(), 0), 3u);
  EXPECT_EQ(std::string(Bytes.data(), Bytes.size()), "abc");
}
TEST_F(WebWindowsHost,
       CapturedBlobsSurviveDeletedInputsAndConcurrentRangeReads) {
  std::string Bytes(200000, '\0');
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = char(I % 251);
  write(Root / "input", Bytes);
  auto S = capture(path(Root / "input"), Limits{});
  auto B = S.Artifacts[0].Content;
  fs::remove(Root / "input");
  S = {};
  std::vector<std::future<bool>> Jobs;
  for (unsigned K = 0; K != 4; ++K)
    Jobs.push_back(std::async(std::launch::async, [&, K] {
      for (unsigned I = 0; I != 200; ++I) {
        const auto Offset = (I * 389 + K * 877) % 190000;
        if (B.slice(Offset, 10000).read(17, 991) !=
            Bytes.substr(Offset + 17, 991))
          return false;
      }
      return true;
    }));
  for (auto &Job : Jobs)
    EXPECT_TRUE(Job.get());
  EXPECT_EQ(B.digest(), sha256(Bytes));
}
TEST_F(WebWindowsHost, StoreRequiresExactLengthSealAndCumulativeBudget) {
  write(Root / "input", "abc");
  auto F = win::openRoot(path(Root / "input"));
  for (uint64_t N : {2, 4}) {
    BlobStore Store(16);
    EXPECT_THROW(Store.capture(F.get(), N), Error);
    EXPECT_THROW(Store.seal(), Error);
  }
  BlobStore Store(4);
  const auto B = Store.capture(F.get(), 3).Content;
  EXPECT_THROW(B.read(0, 1), Error);
  EXPECT_THROW(Store.append("xx"), Error);
  Store.seal();
  EXPECT_EQ(B.read(0, 3), "abc");
  EXPECT_THROW(Store.append("x"), Error);
}
TEST_F(WebWindowsHost, ExportIsVerifiedPrivateAndManifestDoesNotClobber) {
  {
    ExportDirectory E(path(Root / "output"));
    ASSERT_NO_THROW(E.write("source.js", "const x=1;"));
    ASSERT_NO_THROW(E.complete("{}"));
    EXPECT_FALSE(
        MoveFileExW((Root / "output").c_str(), (Root / "moved").c_str(), 0));
  }
  EXPECT_EQ(read(Root / "output" / "source.js"), "const x=1;");
  EXPECT_EQ(read(Root / "output" / "manifest.json"), "{}");
  EXPECT_FALSE(fs::exists(Root / "output" / "manifest.pending"));
  EXPECT_THROW({ ExportDirectory E(path(Root / "output")); }, Error);
  {
    ExportDirectory E(path(Root / "other"));
    write(Root / "other" / "manifest.json", "foreign");
    EXPECT_THROW(E.complete("{}"), Error);
  }
  EXPECT_EQ(read(Root / "other" / "manifest.json"), "foreign");
}
TEST_F(WebWindowsHost, ExportRefusesReadMismatchAndCorruptHash) {
  ExportDirectory E(path(Root / "output"));
  EXPECT_THROW(E.write("short", 3, sha256("abc"),
                       [](uint64_t, uint64_t) { return "a"; }),
               Error);
  EXPECT_THROW(E.write("hash", 3, sha256("xyz"),
                       [](uint64_t, uint64_t) { return "abc"; }),
               Error);
  EXPECT_FALSE(fs::exists(Root / "output" / "manifest.json"));
}
TEST_F(WebWindowsHost, CapabilitiesAdvertiseTheNativeOwnersAndExports) {
  const auto C = Session::capabilities();
  EXPECT_NE(C.find("windows-ntfs-handle-v1"), C.npos);
  EXPECT_NE(C.find("windows-private-unlinked-spool-v1"), C.npos);
  for (const auto *Operation :
       {"desktop_resources_export", "datapack_export", "source_rewrite_export"})
    EXPECT_NE(C.find(Operation), C.npos);
}
#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebWindowsHost, RewriteCommitKeepsExactBytesAndRefusesExistingTargets) {
  {
    rewrite::Export E(path(Root / "output"));
    E.write(rewrite::ExportFile::Original, "1+2;");
    E.write(rewrite::ExportFile::Candidate, "(3);");
    E.write(rewrite::ExportFile::Receipt, "{}");
    EXPECT_FALSE(MoveFileExW((Root / "output" / "original.js").c_str(),
                             (Root / "moved.js").c_str(), 0));
    E.commit();
  }
  EXPECT_EQ(read(Root / "output" / "original.js"), "1+2;");
  EXPECT_EQ(read(Root / "output" / "candidate.js"), "(3);");
  EXPECT_EQ(read(Root / "output" / "receipt.json"), "{}");
  EXPECT_THROW({ rewrite::Export E(path(Root / "output")); }, Error);
}
TEST_F(WebWindowsHost, RewriteFailureRemovesOnlyOwnedFilesAndKeepsForeignData) {
  EXPECT_THROW(
      {
        rewrite::Export E(path(Root / "output"));
        E.write(rewrite::ExportFile::Original, "1+2;");
        write(Root / "output" / "candidate.js", "foreign");
        E.write(rewrite::ExportFile::Candidate, "(3);");
      },
      Error);
  EXPECT_FALSE(fs::exists(Root / "output" / "original.js"));
  EXPECT_EQ(read(Root / "output" / "candidate.js"), "foreign");
}
TEST_F(WebWindowsHost, RewriteIncompleteAndBudgetFailuresRemoveOwnedDirectory) {
  for (const bool Incomplete : {false, true}) {
    EXPECT_THROW(
        {
          rewrite::Export E(path(Root / "output"));
          E.write(rewrite::ExportFile::Original, "1+2;");
          if (Incomplete)
            E.commit();
          else
            E.write(rewrite::ExportFile::Candidate,
                    std::string(MaxSourceRewriteBytes + 1, 'x'));
        },
        Error);
    EXPECT_FALSE(fs::exists(Root / "output"));
  }
}
#endif
} // namespace
#endif
