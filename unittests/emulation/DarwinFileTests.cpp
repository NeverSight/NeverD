//===- DarwinFileTests.cpp - Darwin descriptor and copy contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinSystem.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

namespace neverd::emulation::darwin_model {
namespace {
TEST(DarwinFileOptions, AdmissionCountsPathsTerminatorsFilesAndInputTogether) {
  DarwinFileOptions O;
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 4);
  O.StandardInput = {0xff};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/b"] = {};
  auto TooLarge = validateFileOptions(O);
  ASSERT_TRUE(bool(TooLarge));
  llvm::consumeError(std::move(TooLarge));
  O = {};
  for (unsigned I = 0; I != 256; ++I)
    O.Files["/file" + std::to_string(I)] = {};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/extra"] = {};
  auto TooMany = validateFileOptions(O);
  ASSERT_TRUE(bool(TooMany));
  llvm::consumeError(std::move(TooMany));
  O = {};
  O.Files['/' + std::string(256, 'a')] = {};
  auto LongName = validateFileOptions(O);
  ASSERT_TRUE(bool(LongName));
  llvm::consumeError(std::move(LongName));
}
TEST(DarwinFileOptions, SwapDeclarationRequiresExplicitMutableDirectory) {
  DarwinFileOptions O;
  O.SwapRenameDirectories.insert("/");
  auto Refused = [&](const DarwinFileOptions &Input) {
    auto E = validateFileOptions(Input);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  Refused(O);
  O.MutableDirectories.insert("/");
  Refused(O);
  O.Directories.insert("/");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  for (const char *Path : {"/missing", "/file", "/implicit"}) {
    auto Bad = O;
    Bad.Files["/file"] = {};
    Bad.Files["/implicit/child"] = {};
    Bad.MutableDirectories.insert(Path);
    Bad.SwapRenameDirectories.insert(Path);
    Refused(Bad);
  }
}
TEST(DarwinFileOptions, SwapDeclarationChargesAReferenceWithoutAnotherEntry) {
  DarwinFileOptions O;
  O.Directories.insert("/");
  O.MutableDirectories.insert("/");
  O.SwapRenameDirectories.insert("/");
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 9);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  auto TooLarge = validateFileOptions(O);
  EXPECT_TRUE(bool(TooLarge));
  llvm::consumeError(std::move(TooLarge));
  O.Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    O.Files["/f" + std::to_string(I)] = {};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/extra"] = {};
  auto TooMany = validateFileOptions(O);
  EXPECT_TRUE(bool(TooMany));
  llvm::consumeError(std::move(TooMany));
}
TEST(DarwinFileOptions,
     MovableDeclarationsRequireExplicitRootsAndMutableParents) {
  DarwinFileOptions O;
  O.Directories = {"/a", "/a/sub"};
  O.MutableDirectories.insert("/");
  O.MovableDirectories.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  auto Refused = [](const DarwinFileOptions &Input) {
    auto E = validateFileOptions(Input);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  auto Bad = O;
  Bad.MutableDirectories.clear();
  Refused(Bad);
  for (const char *Path : {"/", "/missing", "/a/implicit", "/file"}) {
    Bad = O;
    Bad.Files["/file"] = {};
    Bad.Files["/a/implicit/data"] = {};
    Bad.MovableDirectories = {Path};
    Refused(Bad);
  }
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    Bad = O;
    auto M = darwin_test::creationParentMetadata();
    M.Inode = 77;
    if (Kind == 0)
      M.Flags = 1;
    if (Kind == 1)
      M.Mode |= 02000;
    Bad.Metadata["/a/sub"] = M;
    if (Kind == 2)
      Bad.Metadata["/a"] = M;
    if (Kind == 3) {
      M.Mode = 0100644;
      M.Size = 0;
      M.LinkCount = 2;
      Bad.Files["/a/file"] = {};
      Bad.Metadata["/a/file"] = M;
    }
    Refused(Bad);
  }
  Bad = O;
  Bad.DirectoryContents["/a"] = {
      {{".", 7, 4, 1, 0}, {"..", 2, 4, 2, 0}, {"sub", 7, 4, 3, 0}}, 1};
  Refused(Bad);
  Bad.MovableDirectories.clear();
  EXPECT_FALSE(bool(validateFileOptions(Bad)));
}

TEST(DarwinFileOptions,
     ExchangeableDeclarationsRequireExplicitRootsAndMutableParents) {
  DarwinFileOptions O;
  O.Directories = {"/a", "/a/sub"};
  O.MutableDirectories.insert("/");
  O.ExchangeableDirectories.insert("/a");
  EXPECT_FALSE(bool(validateFileOptions(O)));
  auto Refused = [](const DarwinFileOptions &Input) {
    auto E = validateFileOptions(Input);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  auto Bad = O;
  Bad.MutableDirectories.clear();
  Refused(Bad);
  for (const char *Path : {"/", "/missing", "/a/implicit", "/file"}) {
    Bad = O;
    Bad.Files["/file"] = {};
    Bad.Files["/a/implicit/data"] = {};
    Bad.ExchangeableDirectories = {Path};
    Refused(Bad);
  }
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    Bad = O;
    auto M = darwin_test::creationParentMetadata();
    M.Inode = 77;
    if (Kind == 0)
      M.Flags = 1;
    if (Kind == 1)
      M.Mode |= 02000;
    Bad.Metadata["/a/sub"] = M;
    if (Kind == 2)
      Bad.Metadata["/a"] = M;
    if (Kind == 3) {
      M.Mode = 0100644;
      M.Size = 0;
      M.LinkCount = 2;
      Bad.Files["/a/file"] = {};
      Bad.Metadata["/a/file"] = M;
    }
    Refused(Bad);
  }
  Bad = O;
  Bad.DirectoryContents["/a"] = {
      {{".", 7, 4, 1, 0}, {"..", 2, 4, 2, 0}, {"sub", 7, 4, 3, 0}}, 1};
  Refused(Bad);
  Bad.ExchangeableDirectories.clear();
  EXPECT_FALSE(bool(validateFileOptions(Bad)));
}

TEST(DarwinFileOptions, MixedMoveAndExchangeDomainsRejectDeviceConflicts) {
  DarwinFileOptions O;
  O.Directories = {"/a", "/b"};
  O.MutableDirectories.insert("/");
  O.MovableDirectories.insert("/a");
  O.ExchangeableDirectories.insert("/b");
  auto M = darwin_test::creationParentMetadata();
  M.Inode = 7;
  M.Device = 1;
  O.Metadata["/a"] = M;
  M.Inode = 8;
  M.Device = 2;
  O.Metadata["/b"] = M;
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.ExchangeableDirectories.clear();
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.ExchangeableDirectories.insert("/b");
  O.Metadata["/b"].Device = 1;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/side"] = {};
  O.Metadata["/side"] = darwin_test::mutationMetadata(0);
  O.Metadata["/side"].Device = 2;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Metadata["/side"].Device = 1;
  EXPECT_FALSE(bool(validateFileOptions(O)));
}

TEST(DarwinFileOptions, MoveAndExchangeReferencesChargeIndependently) {
  DarwinFileOptions O;
  O.Directories.insert("/a");
  O.MutableDirectories.insert("/");
  O.MovableDirectories.insert("/a");
  O.ExchangeableDirectories.insert("/a");
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 17);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files.clear();
  for (unsigned I = 0; I != 254; ++I)
    O.Files["/f" + std::to_string(I)] = {};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/last"] = {};
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}

TEST(DarwinFileOptions,
     MovableDomainsRejectConflictingDevicesWithoutParentStat) {
  DarwinFileOptions O;
  O.Directories = {"/a", "/b"};
  O.MutableDirectories.insert("/");
  O.Metadata["/a"] = O.Metadata["/b"] = darwin_test::creationParentMetadata();
  O.Metadata["/a"].Inode = 7;
  O.Metadata["/a"].Device = 1;
  O.Metadata["/b"].Inode = 8;
  O.Metadata["/b"].Device = 2;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.MovableDirectories = {"/a", "/b"};
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Metadata["/b"].Device = 1;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/side"] = {};
  O.Metadata["/side"] = darwin_test::mutationMetadata(0);
  O.Metadata["/side"].Device = 2;
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Metadata["/side"].Device = 1;
  EXPECT_FALSE(bool(validateFileOptions(O)));
}

TEST(DarwinFileOptions, MovableReferenceChargesItsPathWithoutAnotherEntry) {
  DarwinFileOptions O;
  O.Directories = {"/a"};
  O.MutableDirectories.insert("/");
  O.MovableDirectories.insert("/a");
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 14);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O.Files.clear();
  for (unsigned I = 0; I != 254; ++I)
    O.Files["/f" + std::to_string(I)] = {};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/last"] = {};
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}

class DarwinFileTest : public testing::TestWithParam<uint64_t> {
protected:
  static constexpr uint64_t Base = 0x100000;
  std::shared_ptr<AddressSpace> Space;
  std::optional<DarwinFileOptions> Options;
  std::unique_ptr<DarwinFiles> Files;
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "memory-only test"};
  uint64_t Page;
  void SetUp() override {
    Page = GetParam();
    auto Physical = PhysicalMemory::create(Page * 4);
    ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
    auto Created = AddressSpace::create(*Physical, Page * 4);
    ASSERT_TRUE(bool(Created)) << llvm::toString(Created.takeError());
    Space = std::move(*Created);
    ASSERT_FALSE(
        bool(Space->map(Base, Page * 2, Read | Write | UserAccessible)));
    Options.emplace();
    Options->Files["/data"] = {'a', 'b', 0, 0xff, 'e', 'f'};
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
  }
  void path(llvm::StringRef Text, uint64_t Address = Base) {
    std::string Data = Text.str() + '\0';
    ASSERT_FALSE(bool(Space->write(
        Address,
        llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(Data.data()),
                                Data.size()))));
  }
  std::optional<ServiceResult> invoke(ServiceKind Kind,
                                      std::array<uint64_t, 6> Args) {
    auto Out = Files->handle(Kind, {0, 0, Args, std::nullopt}, Result);
    EXPECT_TRUE(bool(Out)) << (Out ? "" : llvm::toString(Out.takeError()));
    return Out ? *Out : std::nullopt;
  }
  uint64_t ok(ServiceKind Kind, std::array<uint64_t, 6> Args) {
    auto Out = invoke(Kind, Args);
    EXPECT_TRUE(Out.has_value()) << Result.Diagnostic;
    if (!Out)
      return UINT64_MAX;
    EXPECT_FALSE(Out->Error) << Out->Value;
    return Out->Value;
  }
  void error(ServiceKind Kind, std::array<uint64_t, 6> Args, uint64_t Code) {
    auto Out = invoke(Kind, Args);
    ASSERT_TRUE(Out.has_value()) << Result.Diagnostic;
    EXPECT_TRUE(Out->Error);
    EXPECT_EQ(Out->Value, Code);
  }
  void contents(uint64_t FD, llvm::ArrayRef<uint8_t> Expected) {
    EXPECT_EQ(ok(ServiceKind::Pread, {FD, Base + Page, 64, 0}),
              Expected.size());
    std::vector<uint8_t> Bytes(Expected.size());
    ASSERT_FALSE(bool(Space->read(Base + Page, Bytes)));
    EXPECT_EQ(Bytes, Expected.vec());
  }
  void mutationPolicy(uint32_t Unit = 4096) {
    Options->WritableFiles.insert("/data");
    Options->Metadata["/data"] = darwin_test::mutationMetadata(6);
    Options->Metadata["/data"].Blocks = Unit / 512;
    Options->MutationPolicies["/data"] = {Unit, {-7, 123456789}};
  }
  void creationPolicy(uint64_t First = darwin_test::CreationPolicy.FirstInode) {
    Options->MutableDirectories.insert("/");
    Options->Metadata["/"] = darwin_test::creationParentMetadata();
    Options->InitialUmask = 0027;
    Options->CreationPolicy = darwin_test::CreationPolicy;
    Options->CreationPolicy->FirstInode = First;
  }
  std::array<uint8_t, 144> status(uint64_t FD) {
    EXPECT_EQ(ok(ServiceKind::Fstat64, {FD, Base + 256}), 0u);
    std::array<uint8_t, 144> Bytes;
    llvm::cantFail(Space->read(Base + 256, Bytes));
    return Bytes;
  }
  void identity(uint64_t FD, llvm::StringRef Expected) {
    std::vector<uint8_t> Bytes(Expected.size() + 2, 0xcc);
    llvm::cantFail(Space->write(Base + Page, Bytes));
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 50, Base + Page}), 0u);
    llvm::cantFail(Space->read(Base + Page, Bytes));
    EXPECT_EQ(llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                              Expected.size()),
              Expected);
    EXPECT_EQ(Bytes[Expected.size()], 0u);
    EXPECT_EQ(Bytes.back(), 0xccu);
  }
  void renameFile(llvm::StringRef Source, llvm::StringRef Target) {
    path(Source);
    path(Target, Base + 128);
    EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  }
  uint64_t makeDirectory(llvm::StringRef Name) {
    path(Name);
    EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0700}), 0u);
    return ok(ServiceKind::Open, {Base});
  }
  uint64_t makeFile(llvm::StringRef Name, llvm::StringRef Bytes = "") {
    path(Name);
    const auto FD = ok(ServiceKind::Open, {Base, 0x202, 0600});
    path(Bytes, Base + 512);
    EXPECT_EQ(ok(ServiceKind::Write, {FD, Base + 512, Bytes.size()}),
              Bytes.size());
    return FD;
  }
  void exchangeableRoots() {
    Options->Directories = {"/", "/a", "/b"};
    Options->MutableDirectories = {"/"};
    Options->SwapRenameDirectories = {"/"};
    Options->ExchangeableDirectories = {"/a", "/b"};
  }
  void swapFiles(llvm::StringRef Source, llvm::StringRef Target) {
    path(Source);
    path(Target, Base + 128);
    EXPECT_EQ(ok(ServiceKind::RenameAtX,
                 {999, Base, 999, Base + 128, 0x1234567800000012ULL}),
              0u);
  }
};

TEST_P(DarwinFileTest,
       InitialDirectoryExchangeRetainsBothTreesAndObjectGrants) {
  exchangeableRoots();
  Options->MutableDirectories.insert("/a");
  Options->SwapRenameDirectories.insert("/a");
  for (const char *Root : {"/a", "/b"}) {
    const bool A = llvm::StringRef(Root) == "/a";
    const std::string Leaf = std::string(Root) + "/sub";
    Options->Directories.insert(Leaf);
    Options->Files[Leaf + "/f"] = {uint8_t(A ? 'L' : 'R')};
    auto M = darwin_test::creationParentMetadata();
    M.Inode = A ? 101 : 201;
    M.Size = 64;
    Options->Metadata[Leaf] = M;
    M = darwin_test::mutationMetadata(1);
    M.Inode = A ? 103 : 203;
    Options->Metadata[Leaf + "/f"] = M;
    Options->DirectoryContents[Leaf] = {{{".", A ? 101u : 201u, 4, 11, 0},
                                         {"..", A ? 102u : 202u, 4, 22, 0},
                                         {"f", A ? 103u : 203u, 8, 33, 0}},
                                        1};
  }
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/b");
  const auto B = ok(ServiceKind::Open, {Base});
  path("/a/sub");
  const auto Left = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Left});
  const auto LeftStat = status(Left);
  path("/b/sub");
  const auto Right = ok(ServiceKind::Open, {Base});
  const auto RightStat = status(Right);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Left, Base + Page, 32, Base + 512}),
      32u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Left}), 0u);
  swapFiles("/a", "/b");
  identity(A, "/b");
  identity(B, "/a");
  identity(Left, "/b/sub");
  identity(Right, "/a/sub");
  EXPECT_EQ(status(Left), LeftStat);
  EXPECT_EQ(status(Right), RightStat);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dup, Base + Page, 32, Base + 512}),
      32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 8)), 102u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Left, 0, 1}), 22u);
  path("..");
  identity(ok(ServiceKind::Open, {Base}), "/b");
  path("/a/sub/f");
  contents(ok(ServiceKind::Open, {Base}), {'R'});
  path("/b/sub/f");
  contents(ok(ServiceKind::Open, {Base}), {'L'});
  makeFile("/b/fresh", "x");
  path("/a/fresh");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  const auto C = makeDirectory("/b/left");
  makeDirectory("/b/right");
  swapFiles("/b/left", "/b/right");
  identity(C, "/b/right");
  path("/b/sub");
  path("/a", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  swapFiles("/a", "/b");
  identity(Left, "/a/sub");
  identity(Right, "/b/sub");
  EXPECT_EQ(status(Left), LeftStat);
  EXPECT_EQ(status(Right), RightStat);
}

TEST_P(DarwinFileTest,
       InitialAndCreatedDirectoryExchangePreservesBothOrientations) {
  exchangeableRoots();
  Options->Files["/a/f"] = {'L'};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/a/f");
  const auto F = ok(ServiceKind::Open, {Base});
  const auto Created = makeDirectory("/created");
  makeFile("/created/f", "R");
  swapFiles("/a", "/created");
  identity(A, "/created");
  identity(Created, "/a");
  identity(F, "/created/f");
  contents(F, {'L'});
  path("/a/f");
  contents(ok(ServiceKind::Open, {Base}), {'R'});
  swapFiles("/a", "/created");
  identity(A, "/a");
  identity(Created, "/created");
  identity(F, "/a/f");
  contents(F, {'L'});
}

TEST_P(DarwinFileTest,
       InitialDirectoryFileExchangeKeepsMetadataAndCreationSequence) {
  exchangeableRoots();
  creationPolicy();
  mutationPolicy();
  Options->MutableDirectories.insert("/a");
  auto M = Options->Metadata["/"];
  M.Inode = 42;
  M.GID = 77;
  Options->Metadata["/a"] = M;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/data");
  const auto F = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {F});
  auto Expected = status(F);
  llvm::support::endian::write64le(Expected.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 72, 123456789);
  EXPECT_EQ(ok(ServiceKind::Lseek, {F, 1, 0}), 1u);
  auto Lease = Files->mappingSource(F);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  swapFiles("/a", "/data");
  identity(A, "/data");
  identity(F, "/a");
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 1u);
  EXPECT_EQ(status(F), Expected);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'a', 'b', 0, 0xff, 'e', 'f'}));
  path("/data/new");
  const auto New = ok(ServiceKind::Open, {Base, 0x202, 0666});
  auto S = status(New);
  EXPECT_EQ(llvm::support::endian::read32le(S.data() + 20), 77u);
  EXPECT_EQ(llvm::support::endian::read16le(S.data() + 4), 0100640u);
  EXPECT_EQ(llvm::support::endian::read64le(S.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  swapFiles("/a", "/data");
  identity(A, "/a");
  identity(F, "/data");
  identity(New, "/a/new");
  EXPECT_EQ(status(F), Expected);
  path("/next");
  S = status(ok(ServiceKind::Open, {Base, 0x202, 0666}));
  EXPECT_EQ(llvm::support::endian::read64le(S.data() + 8),
            darwin_test::CreationPolicy.FirstInode + 1);
  EXPECT_EQ(Options->CreationPolicy->FirstInode,
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest,
       InitialDirectoryExchangeKeepsModeAndParentAuthoritySeparate) {
  exchangeableRoots();
  const auto Baseline = *Options;
  for (unsigned Case = 0; Case != 7; ++Case) {
    Files.reset();
    Options = Baseline;
    if (Case == 0 || Case == 3)
      Options->ExchangeableDirectories.erase("/a");
    if (Case == 1)
      Options->ExchangeableDirectories.erase("/b");
    if (Case == 2)
      Options->SwapRenameDirectories.clear();
    if (Case == 3)
      Options->MovableDirectories.insert("/a");
    if (Case >= 5) {
      Options->Files["/b/f"] = {'x'};
      if (Case == 6)
        Options->MutableDirectories.insert("/b");
    }
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/a");
    const auto A = ok(ServiceKind::Open, {Base});
    path("/b");
    const auto B = ok(ServiceKind::Open, {Base});
    path("/a");
    path(Case >= 5 ? "/b/f" : "/b", Base + 128);
    if (Case != 4) {
      EXPECT_FALSE(
          invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
      EXPECT_EQ(Result.Diagnostic, Case == 1 ? diagnostic::RenameSwapKind
                                   : Case == 2 || Case == 6
                                       ? diagnostic::RenameSwapSupport
                                   : Case == 5 ? diagnostic::DirectoryNotMutable
                                               : diagnostic::RenameKind);
      identity(A, "/a");
      identity(B, "/b");
    }
    if (Case == 2)
      swapFiles("/a", "/a");
    if (Case == 3) {
      renameFile("/a", "/moved");
      identity(A, "/moved");
    }
    if (Case == 4) {
      path("/a");
      path("/moved", Base + 128);
      EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
      makeDirectory("/created");
      path("/created");
      path("/b", Base + 128);
      EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
      identity(B, "/b");
    }
  }
}

TEST_P(DarwinFileTest,
       InitialDirectoryExchangeNoOpAndChargesRespectExactCapacity) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    const bool NoOp = Case == 0, Short = Case == 2;
    Files.reset();
    Options.emplace();
    exchangeableRoots();
    if (NoOp)
      Options->SwapRenameDirectories.clear();
    // No-op has 22 fixed bytes and no dynamic path charge. Distinct exchange
    // reserves another two-byte SWAP reference and six dynamic path bytes.
    // The short case leaves only five dynamic bytes and must roll back both.
    Options->Files["/data"] =
        std::vector<uint8_t>(darwin_file_limits::Bytes - (NoOp    ? 22
                                                          : Short ? 29
                                                                  : 30));
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/a");
    const auto A = ok(ServiceKind::Open, {Base});
    path("/b");
    const auto B = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 31, 0}), 31u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 77, 0}), 77u);
    if (Short) {
      for (bool Reverse : {false, true}) {
        path(Reverse ? "/b" : "/a");
        path(Reverse ? "/a" : "/b", Base + 128);
        EXPECT_FALSE(
            invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
        EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
        identity(A, "/a");
        identity(B, "/b");
        EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 31u);
        EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 77u);
      }
      swapFiles("/a", "/a");
      path("/x");
      EXPECT_EQ(
          ok(ServiceKind::Close, {ok(ServiceKind::Open, {Base, 0x202, 0600})}),
          0u);
      path("/y");
      EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
      continue;
    }
    for (unsigned I = 0; I != 16; ++I) {
      swapFiles("/a", NoOp ? "/a" : "/b");
      if (!NoOp)
        swapFiles("/a", "/b");
    }
    identity(A, "/a");
    identity(B, "/b");
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 31u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 77u);
    path("/x");
    EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  }
}

TEST_P(DarwinFileTest, InitialDirectoryExchangePreflightsBothCanonicalPaths) {
  exchangeableRoots();
  Options->Directories.erase("/b");
  Options->Directories.insert("/longer");
  Options->ExchangeableDirectories = {"/a", "/longer"};
  std::string Long = "/a";
  for (unsigned I = 0; I != 4; ++I)
    Long += '/' + std::string(250, 'd');
  Long += '/' + std::string(13, 'f');
  ASSERT_EQ(Long.size(), 1020u);
  Options->Files[Long] = {'L'};
  Options->Files["/longer/f"] = {'R'};
  auto M = darwin_test::mutationMetadata(1);
  M.Inode = 101;
  Options->Metadata[Long] = M;
  M.Inode = 102;
  Options->Metadata["/longer/f"] = M;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/longer");
  const auto B = ok(ServiceKind::Open, {Base});
  path(Long);
  const auto Left = ok(ServiceKind::Open, {Base});
  path("/longer/f");
  const auto Right = ok(ServiceKind::Open, {Base});
  const auto L = status(Left), R = status(Right);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 31, 0}), 31u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 77, 0}), 77u);
  for (bool Reverse : {false, true}) {
    path(Reverse ? "/longer" : "/a");
    path(Reverse ? "/a" : "/longer", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(A, "/a");
    identity(B, "/longer");
    identity(Left, Long);
    identity(Right, "/longer/f");
    EXPECT_EQ(status(Left), L);
    EXPECT_EQ(status(Right), R);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 31u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 77u);
    contents(Left, {'L'});
    contents(Right, {'R'});
  }
  makeDirectory("/c");
  swapFiles("/a", "/c");
  identity(A, "/c");
  identity(B, "/longer");
  EXPECT_EQ(status(Left), L);
}

TEST_P(DarwinFileTest,
       InitialDirectoryExchangeRejectsCyclesAndPreservesLookupOrder) {
  exchangeableRoots();
  Options->Directories.insert("/a/sub");
  Options->ExchangeableDirectories.insert("/a/sub");
  Options->MutableDirectories.insert("/a");
  Options->SwapRenameDirectories.insert("/a");
  Options->Files["/a/f"] = {'x'};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/b");
  const auto B = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 31, 0}), 31u);
  for (auto Pair : {std::pair{"/a", "/a/sub"}, std::pair{"/a/sub", "/a"},
                    std::pair{"/a/f", "/a"}}) {
    path(Pair.first);
    path(Pair.second, Base + 128);
    error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2},
          value::InvalidArgument);
  }
  for (const char *Source : {"/a/.", "/a/sub/.."}) {
    path(Source);
    path("/missing/", Base + 128);
    error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2},
          value::NoEntry);
    path("/b", Base + 128);
    error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2},
          value::InvalidArgument);
  }
  path("/a/.");
  path("/a", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameDotCaseSensitivity);
  identity(A, "/a");
  identity(B, "/b");
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 31u);
  path("/a/f");
  contents(ok(ServiceKind::Open, {Base}), {'x'});
}

TEST_P(DarwinFileTest, InitialDirectoryExchangeCarriesSameNamedRemovedOrphans) {
  exchangeableRoots();
  Options->Directories.insert("/a/held");
  Options->Directories.insert("/b/held");
  Options->MutableDirectories.insert("/a");
  Options->MutableDirectories.insert("/b");
  Options->MutableDirectories.insert("/a/held");
  Options->MutableDirectories.insert("/b/held");
  Options->RemovableDirectories = {"/a/held", "/b/held"};
  Options->Files["/a/held/f"] = {'L'};
  Options->Files["/b/held/f"] = {'R'};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a/held");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/b/held");
  const auto B = ok(ServiceKind::Open, {Base});
  path("/a/held/f");
  const auto Left = ok(ServiceKind::Open, {Base});
  path("/b/held/f");
  const auto Right = ok(ServiceKind::Open, {Base});
  auto L = Files->mappingSource(Left), R = Files->mappingSource(Right);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(L));
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(R));
  EXPECT_EQ(ok(ServiceKind::Close, {Left}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Right}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {A}), 0u);
  for (const char *Root : {"/a/held", "/b/held"}) {
    path(std::string(Root) + "/f");
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
    path(Root);
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  }
  swapFiles("/a", "/b");
  identity(A, "/b/held");
  identity(B, "/a/held");
  const auto ReusedA = makeDirectory("/b/held");
  const auto ReusedB = makeDirectory("/a/held");
  makeFile("/b/held/new", "x");
  makeFile("/a/held/new", "y");
  path("new");
  error(ServiceKind::OpenAt, {A, Base}, value::NoEntry);
  error(ServiceKind::OpenAt, {B, Base}, value::NoEntry);
  path("..");
  identity(ok(ServiceKind::Open, {Base}), "/b");
  swapFiles("/a", "/b");
  identity(A, "/a/held");
  identity(B, "/b/held");
  identity(ReusedA, "/a/held");
  identity(ReusedB, "/b/held");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(L).Bytes,
            llvm::ArrayRef<uint8_t>({'L'}));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(R).Bytes,
            llvm::ArrayRef<uint8_t>({'R'}));
  path("/");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  L = uint32_t(value::BadDescriptor);
  swapFiles("/a", "/b");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(R).Bytes,
            llvm::ArrayRef<uint8_t>({'R'}));
  R = uint32_t(value::BadDescriptor);
  swapFiles("/a", "/b");
  path("/a/held/new");
  contents(ok(ServiceKind::Open, {Base}), {'x'});
  path("/b/held/new");
  contents(ok(ServiceKind::Open, {Base}), {'y'});
}

TEST_P(DarwinFileTest,
       InitialDirectoryMoveKeepsCreationIdentityOnTheOriginalParent) {
  creationPolicy();
  Options->Directories.insert("/a");
  Options->MutableDirectories.insert("/a");
  Options->MovableDirectories.insert("/a");
  auto ParentMetadata = Options->Metadata["/"];
  ParentMetadata.Inode = 42;
  ParentMetadata.GID = 77;
  Options->Metadata["/a"] = ParentMetadata;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto Original = ok(ServiceKind::Open, {Base});
  makeDirectory("/dest");
  renameFile("/a", "/dest/moved");
  identity(Original, "/dest/moved");
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Original, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  path("new");
  const auto A = ok(ServiceKind::OpenAt, {Original, Base, 0x202, 0677});
  const auto First = status(A);
  EXPECT_EQ(llvm::support::endian::read32le(First.data()),
            uint32_t(ParentMetadata.Device));
  EXPECT_EQ(llvm::support::endian::read32le(First.data() + 20), 77u);
  EXPECT_EQ(llvm::support::endian::read32le(First.data() + 16), 1000u);
  EXPECT_EQ(llvm::support::endian::read16le(First.data() + 4), 0100650u);
  EXPECT_EQ(llvm::support::endian::read64le(First.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  error(ServiceKind::OpenAt, {Original, Base, 0xa02, 0777}, value::FileExists);
  EXPECT_EQ(ok(ServiceKind::Umask, {0077}), 0027u);
  EXPECT_EQ(status(ok(ServiceKind::OpenAt, {Original, Base, 0x202, 0777})),
            First);
  makeDirectory("/a");
  path("/a/new");
  const auto Reused = status(ok(ServiceKind::Open, {Base, 0x202, 0666}));
  EXPECT_EQ(llvm::support::endian::read32le(Reused.data()),
            uint32_t(Options->Metadata["/"].Device));
  EXPECT_EQ(llvm::support::endian::read32le(Reused.data() + 20),
            Options->Metadata["/"].GID);
  EXPECT_EQ(llvm::support::endian::read16le(Reused.data() + 4), 0100600u);
  EXPECT_EQ(llvm::support::endian::read64le(Reused.data() + 8),
            darwin_test::CreationPolicy.FirstInode + 1);
  path("/dest/moved/second");
  const auto Second = status(ok(ServiceKind::Open, {Base, 0x202, 0666}));
  EXPECT_EQ(llvm::support::endian::read32le(Second.data() + 20), 77u);
  EXPECT_EQ(llvm::support::endian::read16le(Second.data() + 4), 0100600u);
  EXPECT_EQ(llvm::support::endian::read64le(Second.data() + 8),
            darwin_test::CreationPolicy.FirstInode + 2);
  EXPECT_EQ(status(A), First);
  EXPECT_EQ(Options->Metadata["/a"].Inode, 42u);
  EXPECT_EQ(Options->Metadata["/a"].GID, 77u);
  EXPECT_EQ(Options->CreationPolicy->FirstInode,
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest,
       CreatedAncestorSwapCarriesMovedInitialDescendantsWithoutExtraCharges) {
  Options->Directories = {"/", "/a", "/a/sub"};
  Options->MutableDirectories.insert("/");
  Options->MovableDirectories.insert("/a");
  Options->SwapRenameDirectories.insert("/");
  Options->Files["/a/sub/f"] = {'x'};
  auto M = darwin_test::creationParentMetadata();
  M.Inode = 101;
  M.Size = 64;
  Options->Metadata["/a/sub"] = M;
  M = darwin_test::mutationMetadata(1);
  M.Inode = 102;
  Options->Metadata["/a/sub/f"] = M;
  Options->DirectoryContents["/a/sub"] = {
      {{".", 101, 4, 11, 0}, {"..", 103, 4, 22, 0}, {"f", 102, 8, 33, 0}}, 1};
  // Fixed paths/references/data and three 32-byte snapshot records cost 131.
  // /p, /q, /p/a, /p/a/sub and /p/a/sub/f consume 31 dynamic bytes.
  Options->Files["/data"].resize(darwin_file_limits::Bytes - 131 - 31);
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("/a/sub");
  const auto Leaf = ok(ServiceKind::Open, {Base});
  const auto Before = status(Leaf);
  path("/a/sub/f");
  const auto File = ok(ServiceKind::Open, {Base});
  const auto FileBefore = status(File);
  auto Lease = Files->mappingSource(File);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Leaf}), 0u);
  const auto P = makeDirectory("/p");
  const auto Q = makeDirectory("/q");
  renameFile("/a", "/p/a");
  for (unsigned I = 0; I != 16; ++I) {
    swapFiles("/p", "/q");
    identity(P, "/q");
    identity(Q, "/p");
    identity(Root, "/q/a");
    identity(Leaf, "/q/a/sub");
    identity(File, "/q/a/sub/f");
    EXPECT_EQ(status(Leaf), Before);
    EXPECT_EQ(status(File), FileBefore);
    path("..");
    identity(ok(ServiceKind::Open, {Base}), "/q/a");
    swapFiles("/p", "/q");
  }
  EXPECT_EQ(ok(ServiceKind::Lseek, {Leaf, 0, 2}), 64u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Leaf, 0, 0}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Leaf, Base + Page, 32, Base + 512}),
      32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 8)), 101u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Leaf, 0, 1}), 11u);
  path("/a/sub");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  path("/p/a");
  path("/q", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  identity(Root, "/p/a");
  path("/x");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  renameFile("/p/a", "/a");
  identity(Leaf, "/a/sub");
  EXPECT_EQ(status(Leaf), Before);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'x'}));
  makeFile("/x");
  makeFile("/y");
  path("/z");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
}

TEST_P(DarwinFileTest,
       InitialDirectoryMoveRetainsUnopenedDescendantObservations) {
  Options->Directories.insert("/a");
  Options->MutableDirectories = {"/", "/a"};
  Options->MovableDirectories.insert("/a");
  Options->Files["/a/never/leaf/f"] = {'u', 'v'};
  auto M = darwin_test::creationParentMetadata();
  M.Inode = 101;
  M.Size = 64;
  Options->Metadata["/a/never/leaf"] = M;
  M = darwin_test::mutationMetadata(2);
  M.Inode = 103;
  Options->Metadata["/a/never/leaf/f"] = M;
  Options->DirectoryContents["/a/never/leaf"] = {
      {{".", 101, 4, 11, 0}, {"..", 102, 4, 22, 0}, {"f", 103, 8, 33, 0}}, 1};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  const auto Destination = makeDirectory("/dest");
  renameFile("/a", "/dest/moved");
  path("/a/never/leaf");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  path("/dest/moved/never/leaf");
  const auto Leaf = ok(ServiceKind::Open, {Base});
  const auto Independent = ok(ServiceKind::Open, {Base});
  const auto Duplicate = ok(ServiceKind::Dup, {Leaf});
  const auto Before = status(Leaf);
  EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 8), 101u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Leaf, 0, 2}), 64u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 64u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Leaf, 0, 0}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Leaf, Base + Page, 32, Base + 512}),
      32u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 11u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 8)), 101u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Leaf}), 0u);
  path("..");
  const auto Parent = ok(ServiceKind::Open, {Base});
  identity(Parent, "/dest/moved/never");
  path("/dest/moved/never/leaf/f");
  const auto File = ok(ServiceKind::Open, {Base});
  const auto FileStatus = status(File);
  auto Lease = Files->mappingSource(File);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  renameFile("/dest/moved", "/a");
  identity(Leaf, "/a/never/leaf");
  identity(File, "/a/never/leaf/f");
  EXPECT_EQ(status(Leaf), Before);
  EXPECT_EQ(status(File), FileStatus);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'u', 'v'}));
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64,
               {Duplicate, Base + Page, 32, Base + 512}),
            32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 8)), 102u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Leaf, 0, 1}), 22u);
  const auto Reused = makeDirectory("/dest/moved");
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Reused, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Reused, Base + Page, 96, Base + 512}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  EXPECT_EQ(status(Leaf), Before);
  EXPECT_EQ(ok(ServiceKind::Close, {Destination}), 0u);
}

TEST_P(DarwinFileTest,
       InitialDirectoryMoveKeepsGrantsOnObjectsAndSwapSeparate) {
  Options->Directories = {"/a", "/other"};
  Options->MutableDirectories = {"/", "/a", "/other"};
  Options->MovableDirectories = {"/a", "/other"};
  Options->SwapRenameDirectories.insert("/a");
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  const auto A = makeDirectory("/a/child");
  const auto B = makeDirectory("/a/second");
  swapFiles("/a/child", "/a/second");
  identity(A, "/a/second");
  renameFile("/a", "/moved");
  const auto FreshFile = makeFile("/moved/new", "x");
  contents(FreshFile, {'x'});
  swapFiles("/moved/second", "/moved/child");
  identity(A, "/moved/child");
  // Moving a created subtree into another connected initial parent does not
  // replace that object's existing SWAP capability with its new parent's.
  renameFile("/moved/child", "/other/held");
  const auto C = makeDirectory("/other/held/left");
  const auto D = makeDirectory("/other/held/right");
  swapFiles("/other/held/left", "/other/held/right");
  identity(C, "/other/held/right");
  const auto NewA = makeDirectory("/a");
  const auto E = makeDirectory("/a/left");
  const auto F = makeDirectory("/a/right");
  path("/a/left");
  path("/a/right", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
  identity(E, "/a/left");
  // Both actual parents must declare support; only /moved has the old grant.
  path("/moved/second");
  path("/other/held", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
  identity(B, "/moved/second");
  path("/moved");
  path("/other", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {F}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {NewA}), 0u);
}

TEST_P(DarwinFileTest,
       InitialDirectoryMoveKeepsMissingGrantsAndMountsExplicit) {
  Options->Directories = {"/a", "/a/sub", "/b"};
  Options->MutableDirectories = {"/", "/a/sub"};
  path("/a");
  path("/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  Options->MovableDirectories.insert("/a");
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/a");
  path("/missing/target", Base + 128);
  error(ServiceKind::Rename, {Base, Base + 128}, value::NoEntry);
  path("/b/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  path("/a/sub/cycle", Base + 128);
  error(ServiceKind::Rename, {Base, Base + 128}, value::InvalidArgument);
  path("/a/sub");
  path("/child", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  path("/a");
  path("/a", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  path("/data", Base + 128);
  error(ServiceKind::Rename, {Base, Base + 128}, value::NotDirectory);
  renameFile("/a", "/new");
  path("/new/fresh");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Rmdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryRemovalInitial);
}

TEST_P(DarwinFileTest, InitialDirectoryMoveChargesPathsOnceAndFailsAtomically) {
  Options->Directories.insert("/a");
  Options->MutableDirectories.insert("/");
  Options->MovableDirectories.insert("/a");
  Options->Files["/data"].resize(darwin_file_limits::Bytes - 14 - 6);
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  const auto Held = ok(ServiceKind::Open, {Base});
  renameFile("/a", "/long");
  for (unsigned I = 0; I != 16; ++I) {
    renameFile("/long", "/a");
    renameFile("/a", "/long");
  }
  identity(Held, "/long");
  path("/long");
  path("/longer", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(Held, "/long");
  path("/longer");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  path("/x");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  renameFile("/long", "/a");
  const auto New = makeFile("/x");
  EXPECT_EQ(ok(ServiceKind::Close, {New}), 0u);
}

TEST_P(DarwinFileTest,
       InitialDirectoryReplacementCreditsOnlyReleasedDynamicPaths) {
  for (bool Held : {false, true}) {
    Options->Directories = {"/a", "/b"};
    Options->MutableDirectories = {"/"};
    Options->MovableDirectories = {"/a", "/b"};
    Options->RemovableDirectories = {"/a", "/b"};
    Options->Files["/data"].resize(darwin_file_limits::Bytes - 26 - 6);
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/a");
    const auto Old = Held ? ok(ServiceKind::Open, {Base}) : UINT64_MAX;
    renameFile("/a", "/long");
    if (Held) {
      path("/b");
      path("/long", Base + 128);
      EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
      identity(Old, "/long");
      EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
    }
    renameFile("/b", "/long");
    path("/a");
    error(ServiceKind::Open, {Base}, value::NoEntry);
    path("/b");
    error(ServiceKind::Open, {Base}, value::NoEntry);
    // The retained removable grant follows the second initial object.
    path("/long");
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
    const auto New = makeFile("/x");
    EXPECT_EQ(ok(ServiceKind::Close, {New}), 0u);
  }
}

TEST_P(DarwinFileTest,
       InitialDirectoryReplacementCannotCreditCWDOrMappingOnlyParents) {
  for (bool Mapped : {false, true}) {
    Files.reset();
    Options.emplace();
    Options->Directories = {"/a", "/b"};
    Options->MutableDirectories = {"/"};
    Options->MovableDirectories = {"/a", "/b"};
    Options->RemovableDirectories = {"/a", "/b"};
    uint64_t Fixed = 26, Dynamic = 6;
    if (Mapped) {
      Options->MutableDirectories.insert("/a");
      Options->Files["/a/file"] = {'m'};
      Fixed += 12;
      Dynamic += 11;
    } else {
      Options->WorkingDirectory = "/a";
      Fixed += 3;
    }
    Options->Files["/data"].resize(darwin_file_limits::Bytes - Fixed - Dynamic);
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    DarwinFiles::MappingSource Mapping = uint32_t(0);
    if (Mapped) {
      path("/a/file");
      const auto FD = ok(ServiceKind::Open, {Base});
      Mapping = Files->mappingSource(FD);
      ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Mapping));
      EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
      EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    }
    renameFile("/a", "/long");
    path("/b");
    path("/long", Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    if (Mapped) {
      EXPECT_EQ(std::get<DarwinFiles::Mapping>(Mapping).Bytes,
                llvm::ArrayRef<uint8_t>({'m'}));
      Mapping = uint32_t(0);
    } else {
      path("/");
      EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
    }
    renameFile("/b", "/long");
    if (Mapped) {
      // Final lease release refunds the orphan's 11-byte dynamic path and
      // one byte of data. Target reclamation refunds its own charge once.
      makeFile("/free");
      makeFile("/more");
      path("/last");
      EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
    } else {
      path("/x");
      EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
    }
  }
}

TEST_P(DarwinFileTest, InitialDirectoryMovePreflightsUnopenedCanonicalPaths) {
  const auto Long = "/a/" + std::string(255, 'x') + '/' +
                    std::string(255, 'y') + '/' + std::string(255, 'z') + '/' +
                    std::string(247, 'w') + "/f";
  ASSERT_EQ(Long.size(), 1020u);
  Options->Directories.insert("/a");
  Options->MutableDirectories.insert("/");
  Options->MovableDirectories.insert("/a");
  Options->Files[Long] = {'x'};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/a");
  path("/longer", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  path(Long);
  const auto File = ok(ServiceKind::Open, {Base});
  contents(File, {'x'});
  identity(File, Long);
  path("/longer");
  error(ServiceKind::Open, {Base}, value::NoEntry);
}

TEST_P(DarwinFileTest,
       InitialDirectoryGraphKeepsDeepImplicitAncestorsUncharged) {
  Options->Files.clear();
  Options->Directories.insert("/a");
  Options->MutableDirectories.insert("/");
  Options->MovableDirectories.insert("/a");
  std::string Long = "/a";
  for (unsigned I = 0; I != 509; ++I)
    Long += "/x";
  Long += "/f";
  ASSERT_EQ(Long.size(), 1022u);
  Options->Files[Long] = {'x'};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  renameFile("/a", "/b");
  Long[1] = 'b';
  path(Long);
  const auto File = ok(ServiceKind::Open, {Base});
  identity(File, Long);
  const auto Created = makeFile("/fresh", "y");
  contents(Created, {'y'});
}

TEST_P(DarwinFileTest,
       InitialDirectoryMoveRetainsRemovedInitialNodesAndMappings) {
  Options->Directories = {"/a", "/a/empty"};
  Options->MutableDirectories = {"/", "/a", "/a/empty"};
  Options->MovableDirectories.insert("/a");
  Options->RemovableDirectories.insert("/a/empty");
  Options->WorkingDirectory = "/a/empty";
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path(".");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto File = makeFile("/a/empty/file", "held");
  auto Mapping = Files->mappingSource(File);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Mapping));
  path("/a/empty/file");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {File}), 0u);
  path("/a/empty");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  renameFile("/a", "/moved");
  identity(Old, "/moved/empty");
  const auto Replacement = makeDirectory("/moved/empty");
  const auto NewFile = makeFile("/moved/empty/new", "new");
  path("new");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  path(".");
  const auto Same = ok(ServiceKind::Open, {Base});
  identity(Same, "/moved/empty");
  path("..");
  const auto Parent = ok(ServiceKind::Open, {Base});
  identity(Parent, "/moved");
  path("../empty/new");
  const auto Reopened = ok(ServiceKind::Open, {Base});
  contents(Reopened, {'n', 'e', 'w'});
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Same}), 0u);
  path("/");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  renameFile("/moved", "/moved-again");
  identity(Parent, "/moved-again");
  identity(Replacement, "/moved-again/empty");
  identity(NewFile, "/moved-again/empty/new");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Mapping).Bytes,
            llvm::ArrayRef<uint8_t>({'h', 'e', 'l', 'd'}));
  Mapping = uint32_t(0);
  const auto Created = makeFile("/fresh", "x");
  contents(Created, {'x'});
}

TEST_P(DarwinFileTest, DirectorySwapRetainsNonemptySubtreesAndObjectState) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  const auto Left = makeDirectory("/p");
  const auto Right = makeDirectory("/q");
  const auto A = makeDirectory("/p/a");
  const auto B = makeDirectory("/q/b");
  const auto ChildA = makeDirectory("/p/a/sub");
  const auto ChildB = makeDirectory("/q/b/sub");
  const auto FileA = makeFile("/p/a/sub/file", "abc");
  const auto FileB = makeFile("/q/b/sub/file", "xyz");
  const auto BeforeA = status(FileA), BeforeB = status(FileB);
  const auto Duplicate = ok(ServiceKind::Dup, {FileA});
  path("/p/a/sub/file");
  const auto Independent = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FileA, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 7}), 7u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 2, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {ChildA}), 0u);
  auto Lease = Files->mappingSource(FileA);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  path("a");
  path("b///", Base + 128);
  ASSERT_EQ(ok(ServiceKind::RenameAtX, {Left, Base, Right, Base + 128, 2}), 0u);
  identity(A, "/q/b");
  identity(B, "/p/a");
  identity(ChildA, "/q/b/sub");
  identity(ChildB, "/p/a/sub");
  for (auto FD : {FileA, Duplicate, Independent})
    identity(FD, "/q/b/sub/file");
  identity(FileB, "/p/a/sub/file");
  EXPECT_EQ(status(FileA), BeforeA);
  EXPECT_EQ(status(FileB), BeforeB);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'a', 'b', 'c'}));
  path(".");
  const auto CWD = ok(ServiceKind::Open, {Base});
  identity(CWD, "/q/b/sub");
  path("..");
  const auto Parent = ok(ServiceKind::OpenAt, {A, Base});
  identity(Parent, "/q");
  path("sub/file");
  const auto Reopened = ok(ServiceKind::OpenAt, {A, Base});
  contents(Reopened, {'a', 'b', 'c'});
  path("/p/a/sub/file");
  contents(ok(ServiceKind::Open, {Base}), {'x', 'y', 'z'});
  swapFiles("/q/b", "/p/a");
  identity(A, "/p/a");
  identity(B, "/q/b");
  identity(CWD, "/p/a/sub");
  identity(FileA, "/p/a/sub/file");
  EXPECT_EQ(status(FileA), BeforeA);
}

TEST_P(DarwinFileTest, DirectorySwapSupportsBothMixedOrders) {
  for (bool DirectoryFirst : {true, false}) {
    SCOPED_TRACE(DirectoryFirst);
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    creationPolicy();
    mutationPolicy();
    Options->Directories.insert("/");
    Options->SwapRenameDirectories.insert("/");
    const auto Directory = makeDirectory("/folder");
    const auto Child = makeDirectory("/folder/sub");
    const auto Descendant = makeFile("/folder/sub/file", "c");
    path("/data");
    const auto File = ok(ServiceKind::Open, {Base, 2});
    auto FileBefore = status(File);
    const auto ChildBefore = status(Descendant);
    EXPECT_EQ(ok(ServiceKind::Fchdir, {Child}), 0u);
    auto Lease = Files->mappingSource(File);
    ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
    path(DirectoryFirst ? "/folder" : "/data");
    path(DirectoryFirst ? "/data" : "/folder", Base + 128);
    ASSERT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}), 0u);
    identity(Directory, "/data");
    identity(Child, "/data/sub");
    identity(Descendant, "/data/sub/file");
    identity(File, "/folder");
    EXPECT_EQ(status(Descendant), ChildBefore);
    llvm::support::endian::write64le(FileBefore.data() + 64, uint64_t(-7));
    llvm::support::endian::write64le(FileBefore.data() + 72, 123456789);
    EXPECT_EQ(status(File), FileBefore);
    contents(File, {'a', 'b', 0, 0xff, 'e', 'f'});
    EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
              llvm::ArrayRef<uint8_t>({'a', 'b', 0, 0xff, 'e', 'f'}));
    path(".");
    identity(ok(ServiceKind::Open, {Base}), "/data/sub");
    path("..");
    identity(ok(ServiceKind::OpenAt, {Directory, Base}), "/");
    swapFiles("/folder", "/data");
    identity(Directory, "/folder");
    identity(Descendant, "/folder/sub/file");
    identity(File, "/data");
    EXPECT_EQ(status(Descendant), ChildBefore);
  }
}

TEST_P(DarwinFileTest, DirectorySwapMissingTargetPrecedesSourceDotsAndGrant) {
  creationPolicy();
  Options->Directories.insert("/");
  const auto Directory = makeDirectory("/left");
  makeDirectory("/left/sub");
  for (bool Declared : {false, true}) {
    if (Declared)
      Options->SwapRenameDirectories.insert("/");
    for (const char *Source : {"/left/.", "/left/sub/.."})
      for (const char *Target : {"/absent", "/absent/"}) {
        SCOPED_TRACE(Source);
        SCOPED_TRACE(Target);
        path(Source);
        path(Target, Base + 128);
        error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}, 2);
        identity(Directory, "/left");
      }
  }
}

TEST_P(DarwinFileTest, DirectorySwapRejectsCyclesAndKeepsLookupPrecedence) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  const auto A = makeDirectory("/a");
  const auto B = makeDirectory("/b");
  const auto Child = makeDirectory("/a/sub");
  const auto File = makeFile("/a/sub/file", "a");
  const auto Before = status(File);
  for (auto [Source, Target] : {std::pair{"/a", "/a/sub"},
                                {"/a/sub", "/a"},
                                {"/a", "/a/sub/file"},
                                {"/a/sub/file", "/a"},
                                {"/a/.", "/b"},
                                {"/a/sub/..", "/b"},
                                {"/a", "/b/."}}) {
    path(Source);
    path(Target, Base + 128);
    error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}, 22);
    identity(A, "/a");
    identity(B, "/b");
    identity(Child, "/a/sub");
    identity(File, "/a/sub/file");
    EXPECT_EQ(status(File), Before);
  }
  path("/a");
  path("/a/sub/file/", Base + 128);
  error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}, 20);
  error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, 2}, 14);
  path("b", Base + 128);
  error(ServiceKind::RenameAtX, {999, Base, 88888, Base + 128, 2}, 9);
  error(ServiceKind::RenameAtX, {999, Base, File, Base + 128, 2}, 20);
  for (uint64_t Flags : {6u, 8u, 0x16u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          22);
  path("/a/.");
  path("/a", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameDotCaseSensitivity);
  path("/missing");
  error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, 2}, 2);
}

TEST_P(DarwinFileTest, DirectorySwapKeepsInitialDomainsAndGrantsExplicit) {
  creationPolicy();
  Options->Directories = {"/", "/initial"};
  Options->MutableDirectories.insert("/initial");
  Options->SwapRenameDirectories = {"/", "/initial"};
  Options->Metadata["/initial"] = darwin_test::creationParentMetadata();
  Options->Metadata["/initial"].Inode++;
  const auto A = makeDirectory("/a");
  const auto B = makeDirectory("/b");
  makeDirectory("/initial/child");
  path("/a");
  path("/initial/child", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  for (const char *Target : {"/", "/initial"}) {
    path(Target, Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapKind);
  }
  path("/initial");
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  path("/a");
  path("/b", Base + 128);
  // Each negative control owns complete, stable initial declarations.
  auto Retained = std::move(Files);
  auto WithoutSwap = Options;
  WithoutSwap->SwapRenameDirectories.clear();
  Files = std::make_unique<DarwinFiles>(*Space, WithoutSwap);
  makeDirectory("/a");
  makeDirectory("/b");
  path("/a");
  path("/b", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
  swapFiles("/a", "/./a");
  auto WithoutTargetGrant = Options;
  WithoutTargetGrant->Directories.insert("/target");
  WithoutTargetGrant->MovableDirectories.insert("/target");
  Files = std::make_unique<DarwinFiles>(*Space, WithoutTargetGrant);
  const auto Control = makeDirectory("/source");
  path("/source");
  path("/target/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  identity(Control, "/source");
  Files = std::move(Retained);
  identity(A, "/a");
  identity(B, "/b");
  EXPECT_EQ(Options->SwapRenameDirectories,
            (std::set<std::string>{"/", "/initial"}));
  path("/a");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path(".");
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {A, Base, B, UINT64_MAX, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
}

TEST_P(DarwinFileTest, DirectorySwapChargesInitialFileOnceWithoutCredits) {
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  Options->SwapRenameDirectories.insert("/");
  const auto File = ok(ServiceKind::Open, {Base, 2});
  const auto Directory = makeDirectory("/d");
  // Fixed inputs cost 18 bytes; initially only /d has a dynamic name charge.
  const uint64_t Capacity = darwin_file_limits::Bytes - 18;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {File, Capacity - 3}), 0u);
  path("/d");
  path("/data", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(File, "/data");
  identity(Directory, "/d");
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {File, Capacity - 8}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {File, Capacity - 9}), 0u);
  auto Lease = Files->mappingSource(File);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  ASSERT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}), 0u);
  identity(File, "/d");
  identity(Directory, "/data");
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {File, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.size(), Capacity - 9);
  Lease = uint32_t(0);
  for (unsigned I = 0; I != 8; ++I) {
    swapFiles("/d", "/data");
    swapFiles("/d", "/data");
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {File, Capacity - 9}), 0u);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {File, Capacity - 8}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  }
  swapFiles("/d", "/data");
  identity(File, "/data");
  identity(Directory, "/d");
  // Returning to its initial name does not erase the file's dynamic charge.
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {File, Capacity - 8}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, DirectorySwapDoesNotMoveOldOrphansAtAMixedRootName) {
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  const auto Directory = makeDirectory("/d");
  const auto Child = makeDirectory("/d/child");
  const auto Old = makeFile("/f", "o");
  auto Lease = Files->mappingSource(Old);
  path("/f");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto Fresh = makeFile("/f", "n");
  swapFiles("/d", "/f");
  identity(Directory, "/f");
  identity(Child, "/f/child");
  identity(Fresh, "/d");
  identity(Old, "/f");
  renameFile("/f", "/moved");
  identity(Directory, "/moved");
  identity(Child, "/moved/child");
  identity(Fresh, "/d");
  identity(Old, "/f");
  contents(Old, {'o'});
  contents(Fresh, {'n'});
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'o'}));
}

TEST_P(DarwinFileTest, DirectorySwapCannotCreditAnUnopenedRegularFile) {
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  Options->SwapRenameDirectories.insert("/");
  Options->Files["/target"] = std::vector<uint8_t>(32, 't');
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const auto Directory = makeDirectory("/s");
  // Fixed names/references:26; the unopened target's bytes:32; /s:3.
  const uint64_t Capacity = darwin_file_limits::Bytes - 26 - 32 - 3;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  path("/s");
  path("/target", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 7}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(Directory, "/s");
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 8}), 0u);
  ASSERT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}), 0u);
  identity(Directory, "/target");
  path("/s");
  contents(ok(ServiceKind::Open, {Base}), std::vector<uint8_t>(32, 't'));
}

TEST_P(DarwinFileTest, DirectorySwapRetainsBothSameNameOrphansAndRefundsOnce) {
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  Options->SwapRenameDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const auto A = makeDirectory("/a");
  const auto B = makeDirectory("/b");
  const auto OldDirA = makeDirectory("/a/dead");
  const auto OldDirB = makeDirectory("/b/dead");
  const auto OldA = makeFile("/a/same", "a");
  const auto OldB = makeFile("/b/same", "b");
  auto Lease = Files->mappingSource(OldB);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  for (const char *Name : {"/a/same", "/b/same"}) {
    path(Name);
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  }
  EXPECT_EQ(ok(ServiceKind::Close, {OldB}), 0u);
  for (const char *Name : {"/a/dead", "/b/dead"}) {
    path(Name);
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  }
  const auto NewDirA = makeDirectory("/a/dead");
  const auto NewDirB = makeDirectory("/b/dead");
  const auto NewA = makeFile("/a/same", "n");
  const auto NewB = makeFile("/b/same", "m");
  // Roots:6; four child directories:32; four file paths and bytes:36.
  const uint64_t Capacity = darwin_file_limits::Bytes - 18 - 74;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  swapFiles("/a", "/b");
  identity(A, "/b");
  identity(B, "/a");
  for (auto FD : {OldDirA, NewDirA})
    identity(FD, "/b/dead");
  for (auto FD : {OldDirB, NewDirB})
    identity(FD, "/a/dead");
  for (auto FD : {OldA, NewA})
    identity(FD, "/b/same");
  identity(NewB, "/a/same");
  contents(OldA, {'a'});
  contents(NewA, {'n'});
  contents(NewB, {'m'});
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'b'}));
  path("same");
  contents(ok(ServiceKind::OpenAt, {A, Base}), {'n'});
  contents(ok(ServiceKind::OpenAt, {B, Base}), {'m'});
  path("new");
  error(ServiceKind::OpenAt, {OldDirA, Base}, 2);
  error(ServiceKind::OpenAt, {OldDirB, Base}, 2);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Close, {OldA}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 9}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 10}));
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 18}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 19}));
  EXPECT_EQ(ok(ServiceKind::Close, {OldDirA}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 26}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 27}));
  EXPECT_EQ(ok(ServiceKind::Close, {OldDirB}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 34}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 35}));
}

TEST_P(DarwinFileTest, DirectorySwapPreflightsBothRetainedSubtreesAtomically) {
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  Options->SwapRenameDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const auto Source = makeDirectory("/s");
  const auto Target = makeDirectory("/long");
  const auto Removed = makeDirectory("/s/g");
  const auto File = makeFile("/s/g/f", "xy");
  auto Lease = Files->mappingSource(File);
  path("/s/g/f");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/s/g");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  // Three source paths grow by 3; the opposing root shrinks by 3.
  const uint64_t Capacity = darwin_file_limits::Bytes - 18;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 28}), 0u);
  for (bool Reverse : {false, true}) {
    path(Reverse ? "/long" : "/s");
    path(Reverse ? "/s" : "/long", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(Source, "/s");
    identity(Target, "/long");
    identity(Removed, "/s/g");
    identity(File, "/s/g/f");
    contents(File, {'x', 'y'});
  }
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 29}), 0u);
  ASSERT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}), 0u);
  identity(Source, "/long");
  identity(Target, "/s");
  identity(Removed, "/long/g");
  identity(File, "/long/g/f");
  path("..");
  identity(ok(ServiceKind::OpenAt, {Removed, Base}), "/long");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'x', 'y'}));
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 28}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, DirectorySwapPreflightsBothCanonicalPathLimits) {
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  const auto Source = makeDirectory("/s");
  const auto Child = makeDirectory("/s/x");
  const auto File = makeFile("/s/x/f", "s");
  std::string Prefix;
  for (char Letter : {'a', 'b', 'c'}) {
    Prefix += '/' + std::string(255, Letter);
    makeDirectory(Prefix);
  }
  const auto Boundary = Prefix + '/' + std::string(250, 'd');
  ASSERT_EQ(Boundary.size(), 1019u);
  const auto Long = Boundary + 'd';
  const auto Target = makeDirectory(Long);
  const auto TargetFile = makeFile(Long + "/v", "t");
  for (bool Reverse : {false, true}) {
    path(Reverse ? Long : "/s");
    path(Reverse ? "/s" : Long, Base + Page);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + Page, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(Source, "/s");
    identity(Child, "/s/x");
    identity(File, "/s/x/f");
    identity(Target, Long);
    identity(TargetFile, Long + "/v");
  }
  // Keep long source and target strings in separate guest buffers.
  path(Long);
  path(Boundary, Base + Page);
  ASSERT_EQ(ok(ServiceKind::Rename, {Base, Base + Page}), 0u);
  path("/s");
  path(Boundary, Base + Page);
  ASSERT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + Page, 2}), 0u);
  identity(Source, Boundary);
  identity(File, Boundary + "/x/f");
  identity(Target, "/s");
  identity(TargetFile, "/s/v");
  contents(File, {'s'});
  contents(TargetFile, {'t'});
}

TEST_P(DarwinFileTest, DirectorySwapConsumesNoEntryDescriptorOrCreationInode) {
  Options->Files.clear();
  for (unsigned I = 0; I != 252; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  creationPolicy(UINT64_MAX);
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  Options->DescriptorLimit = 6;
  const auto Source = makeDirectory("/s");
  const auto Target = makeDirectory("/t");
  const auto File = makeFile("/s/value", "x");
  const auto Before = status(File);
  swapFiles("/s", "/t");
  identity(Source, "/t");
  identity(Target, "/s");
  identity(File, "/t/value");
  EXPECT_EQ(status(File), Before);
  EXPECT_EQ(llvm::support::endian::read64le(Before.data() + 8), UINT64_MAX);
  path("/extra");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base, 0700}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  path("/f0");
  error(ServiceKind::Open, {Base}, 24);
  swapFiles("/t", "/s");
  identity(File, "/s/value");
}

TEST_P(DarwinFileTest, DirectoryRenameRetainsSubtreePathsCWDAndFileState) {
  creationPolicy();
  const auto Source = makeDirectory("/from");
  const auto Right = makeDirectory("/right");
  const auto Child = makeDirectory("/from/child");
  const auto File = makeFile("/from/child/file", "abc");
  const auto Empty = makeFile("/from/child/empty");
  const auto Before = status(File), EmptyBefore = status(Empty);
  const auto Duplicate = ok(ServiceKind::Dup, {File});
  const auto DirectoryDuplicate = ok(ServiceKind::Dup, {Source});
  path("/from/child/file");
  const auto Independent = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {File, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Source, 7}), 7u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Source, 2, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Child}), 0u);
  auto Mapping = Files->mappingSource(File);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Mapping));
  path("/from");
  path("/right/moved///", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX,
               {999, Base, 999, Base + 128, 0x1234567800000010ULL}),
            0u);
  identity(Source, "/right/moved");
  identity(DirectoryDuplicate, "/right/moved");
  identity(Child, "/right/moved/child");
  for (auto FD : {File, Duplicate, Independent})
    identity(FD, "/right/moved/child/file");
  identity(Empty, "/right/moved/child/empty");
  EXPECT_EQ(status(File), Before);
  EXPECT_EQ(status(Empty), EmptyBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {DirectoryDuplicate, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Source, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {DirectoryDuplicate, 1}), 0u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Mapping).Bytes,
            llvm::ArrayRef<uint8_t>({'a', 'b', 'c'}));
  path(".");
  const auto CWD = ok(ServiceKind::Open, {Base});
  identity(CWD, "/right/moved/child");
  path("..");
  const auto Parent = ok(ServiceKind::OpenAt, {Source, Base});
  identity(Parent, "/right");
  identity(Right, "/right");
  path("child/file");
  const auto Reopened = ok(ServiceKind::OpenAt, {Source, Base});
  EXPECT_EQ(status(Reopened), Before);
  contents(Reopened, {'a', 'b', 'c'});
  path("/from");
  error(ServiceKind::Access, {Base}, 2);
}

TEST_P(DarwinFileTest, DirectoryRenameDistinguishesOrphansAtReplacedNames) {
  creationPolicy();
  const auto Source = makeDirectory("/source");
  const auto Target = makeDirectory("/target");
  const auto SourceFile = makeFile("/source/dead", "s");
  const auto TargetFile = makeFile("/target/dead", "t");
  for (const char *Name : {"/source/dead", "/target/dead"}) {
    path(Name);
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  }
  renameFile("/source", "/target");
  identity(Source, "/target");
  identity(Target, "/target");
  identity(SourceFile, "/target/dead");
  identity(TargetFile, "/target/dead");
  path("new");
  error(ServiceKind::OpenAt, {Target, Base, 0x202, 0600}, 2);
  path(".");
  const auto OldDot = ok(ServiceKind::OpenAt, {Target, Base});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Target}), 0u);
  renameFile("/target", "/final");
  identity(Source, "/final");
  identity(SourceFile, "/final/dead");
  identity(TargetFile, "/target/dead");
  identity(Target, "/target");
  identity(OldDot, "/target");
  path(".");
  const auto OldCWD = ok(ServiceKind::Open, {Base});
  identity(OldCWD, "/target");
  path("..");
  const auto Parent = ok(ServiceKind::OpenAt, {Target, Base});
  identity(Parent, "/");
  path("new");
  error(ServiceKind::OpenAt, {OldDot, Base}, 2);
  contents(SourceFile, {'s'});
  contents(TargetFile, {'t'});
}

TEST_P(DarwinFileTest, DirectoryRenamePreservesRemovedInitialObjectMembership) {
  Options->Files["/old/f"] = {'o'};
  Options->Directories.insert("/old");
  Options->MutableDirectories = {"/", "/old"};
  Options->RemovableDirectories.insert("/old");
  path("/old");
  const auto OldDirectory = ok(ServiceKind::Open, {Base});
  path("/old/f");
  const auto OldFile = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/old");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto NewDirectory = makeDirectory("/old");
  const auto NewFile = makeFile("/old/f", "n");
  renameFile("/old", "/moved");
  identity(OldDirectory, "/old");
  identity(OldFile, "/old/f");
  identity(NewDirectory, "/moved");
  identity(NewFile, "/moved/f");
  path("f");
  error(ServiceKind::OpenAt, {OldDirectory, Base}, 2);
  const auto Current = ok(ServiceKind::OpenAt, {NewDirectory, Base});
  contents(Current, {'n'});
  contents(OldFile, {'o'});
}

TEST_P(DarwinFileTest, DirectoryRenameFollowsParentsAfterFileRenameAndSwap) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  const auto Directory = makeDirectory("/source");
  const auto A = makeFile("/source/a", "a");
  const auto B = makeFile("/b", "b");
  swapFiles("/source/a", "/b");
  renameFile("/source", "/new");
  identity(A, "/b");
  identity(B, "/new/a");
  renameFile("/b", "/new/moved");
  renameFile("/new", "/final");
  identity(A, "/final/moved");
  identity(B, "/final/a");
  path("/final/a");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  renameFile("/final", "/end");
  identity(A, "/end/moved");
  identity(B, "/end/a");
  identity(Directory, "/end");
  contents(A, {'a'});
  contents(B, {'b'});
}

TEST_P(DarwinFileTest, DirectoryRenamePreservesNativeErrorsWithoutEffects) {
  creationPolicy();
  const auto Left = makeDirectory("/left");
  const auto Source = makeDirectory("/left/source");
  const auto Child = makeDirectory("/left/source/child");
  const auto File = makeFile("/left/source/child/data", "abc");
  const auto Right = makeDirectory("/right");
  makeDirectory("/right/empty");
  makeDirectory("/right/busy");
  makeFile("/right/busy/member");
  makeFile("/right/regular");
  const auto Before = status(File);
  EXPECT_EQ(ok(ServiceKind::Lseek, {File, 1, 0}), 1u);
  for (auto [Name, Code] : {std::pair{"/right/regular", 20u},
                            {"/right/regular/", 20u},
                            {"/right/busy", 66u},
                            {"/right/.", 22u},
                            {"/right/busy/..", 22u},
                            {"/right/missing/../target", 2u},
                            {"/right/regular/../target", 20u},
                            {"/left/source/inside", 22u},
                            {"/left/source/child", 22u},
                            {"/left/source/child/data", 20u},
                            {"/left", 66u}}) {
    SCOPED_TRACE(Name);
    path("/left/source");
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, Code);
    identity(Source, "/left/source");
    identity(Child, "/left/source/child");
    identity(File, "/left/source/child/data");
    EXPECT_EQ(status(File), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {File, 0, 1}), 1u);
  }
  for (const char *Name : {"/right/regular", "/right/busy", "/right/empty",
                           "/left/source/child"}) {
    path(Name, Base + 128);
    error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}, 17);
  }
  path("/left/source", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameCaseSensitivity);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  for (const char *Name : {"/left/source/.", "/left/source/child/.."}) {
    path(Name);
    error(ServiceKind::Rename, {Base, UINT64_MAX}, 14);
    path("/left/source", Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameDotCaseSensitivity);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameCaseSensitivity);
    path("/right/regular", Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, 22);
    error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}, 17);
  }
  path("/missing");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 2);
  path("source");
  error(ServiceKind::RenameAt, {999, Base, Right, UINT64_MAX}, 9);
  error(ServiceKind::RenameAt, {File, Base, Right, UINT64_MAX}, 20);
  error(ServiceKind::RenameAt, {Left, Base, Right, UINT64_MAX}, 14);
  for (uint64_t Flags : {6u, 8u, 0x80000000u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          22);
  path("/left/source");
  path("/right/empty", Base + 128);
  for (uint64_t Flags : {2u, 0x12u}) {
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
  }
  path("/right/new///", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX,
               {999, Base, 999, Base + 128, 0x1234567800000014ULL}),
            0u);
  identity(Source, "/right/new");
  identity(File, "/right/new/child/data");
}

TEST_P(DarwinFileTest, DirectoryRenameKeepsInitialDomainsAndAuthorityExplicit) {
  Options->Directories.insert("/other");
  Options->MutableDirectories = {"/", "/other"};
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  auto Other = darwin_test::creationParentMetadata();
  Other.Inode++;
  Options->Metadata["/other"] = Other;
  const auto Source = makeDirectory("/source");
  makeDirectory("/other/created");
  path("/source");
  for (const char *Name : {"/other/new", "/other/created/new"}) {
    path(Name, Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  }
  path("/other/created", Base + 128);
  error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}, 17);
  // Root belongs to the same initial object domain, but remains an unknown
  // initial-directory replacement rather than a guessed empty directory.
  path("/", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  path("/other");
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  path("/source");
  auto Retained = std::move(Files);
  auto WithoutTargetGrant = Options;
  WithoutTargetGrant->Directories.insert("/target");
  WithoutTargetGrant->MovableDirectories.insert("/target");
  Files = std::make_unique<DarwinFiles>(*Space, WithoutTargetGrant);
  const auto Control = makeDirectory("/control");
  path("/control");
  path("/target/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  identity(Control, "/control");
  Files = std::move(Retained);
  identity(Source, "/source");
  renameFile("/source", "/new");
  identity(Source, "/new");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path(".");
  EXPECT_FALSE(
      invoke(ServiceKind::RenameAt, {Source, Base, Source, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
}

TEST_P(DarwinFileTest,
       DirectoryRenameChargesEveryRetainedSubtreePathAtomically) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const auto Source = makeDirectory("/s");
  const auto Removed = makeDirectory("/s/g");
  const auto File = makeFile("/s/g/f", "xy");
  auto Lease = Files->mappingSource(File);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  path("/s/g/f");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/s/g");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  // Three retained paths grow by three bytes each, beside two file bytes.
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 25}), 0u);
  path("/s");
  path("/long", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(Source, "/s");
  identity(Removed, "/s/g");
  identity(File, "/s/g/f");
  error(ServiceKind::Access, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 26}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  identity(Source, "/long");
  identity(Removed, "/long/g");
  identity(File, "/long/g/f");
  path("..");
  const auto Parent = ok(ServiceKind::OpenAt, {Removed, Base});
  identity(Parent, "/long");
  EXPECT_EQ(ok(ServiceKind::Close, {Parent}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 25}));
  renameFile("/long", "/x");
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 17}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 16}));
  path("/x");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  for (auto FD : {File, Source, Removed})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 16}));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'x', 'y'}));
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, DirectoryRenameReplacementCreditsOnlyReleasedTargets) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  for (unsigned Retained = 0; Retained != 4; ++Retained) {
    SCOPED_TRACE(Retained);
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
    const auto Data = ok(ServiceKind::Open, {Base, 2});
    const auto Source = makeDirectory("/s");
    const auto Child = makeFile("/s/f", "x");
    const auto Target = makeDirectory("/target");
    DarwinFiles::MappingSource Lease = uint32_t(0);
    if (Retained == 2)
      EXPECT_EQ(ok(ServiceKind::Fchdir, {Target}), 0u);
    if (Retained == 3) {
      const auto Old = makeFile("/target/o", "o");
      Lease = Files->mappingSource(Old);
      path("/target/o");
      EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
      EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
    }
    if (Retained != 1)
      EXPECT_EQ(ok(ServiceKind::Close, {Target}), 0u);
    const uint64_t Charge = Retained == 0 ? 19 : Retained == 3 ? 38 : 27;
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - Charge + 1}), 0u);
    path("/s");
    path("/target", Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(Source, "/s");
    identity(Child, "/s/f");
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - Charge}), 0u);
    EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
    identity(Source, "/target");
    identity(Child, "/target/f");
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - Charge + 1}));
    if (Retained == 1) {
      identity(Target, "/target");
      EXPECT_EQ(ok(ServiceKind::Close, {Target}), 0u);
    }
    if (Retained == 2) {
      path("/");
      EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
    }
    Lease = uint32_t(0);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 19}), 0u);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 18}));
  }
}

TEST_P(DarwinFileTest, DirectoryRenameRejectsOverlongLiveAndOrphanPaths) {
  Options->MutableDirectories.insert("/");
  std::string Parent;
  for (char C : {'a', 'b', 'c', 'd'})
    Parent += '/' + std::string(240, C);
  std::string Component;
  for (char C : {'a', 'b', 'c', 'd'}) {
    Component += '/' + std::string(240, C);
    makeDirectory(Component);
  }
  const auto Source = makeDirectory("/s");
  const auto Child = makeFile("/s/f", "x");
  const auto Orphan = makeFile("/s/orphan", "o");
  path("/s/orphan");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  // A short relative input can produce an oversized retained descendant.
  path(Parent);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  path("/s");
  for (unsigned Size : {58u, 52u}) {
    SCOPED_TRACE(Size);
    path(std::string(Size, 'x'), Base + 2048);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAt, {999, Base, uint32_t(-2), Base + 2048}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(Source, "/s");
    identity(Child, "/s/f");
    identity(Orphan, "/s/orphan");
    error(ServiceKind::Access, {Base + 2048}, 2);
  }
  // Include the NUL of the longest orphan name: the 1023-byte path fits.
  path(std::string(51, 'y'), Base + 2048);
  EXPECT_EQ(ok(ServiceKind::RenameAt, {999, Base, uint32_t(-2), Base + 2048}),
            0u);
  const auto New = Parent + '/' + std::string(51, 'y');
  identity(Source, New);
  identity(Child, New + "/f");
  identity(Orphan, New + "/orphan");
}

TEST_P(DarwinFileTest, MappingOnlyOrphanRetainsRemovedDirectoryEntryChain) {
  Options->Files.clear();
  for (unsigned I = 0; I != 252; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  const auto Source = makeDirectory("/s");
  const auto Removed = makeDirectory("/s/g");
  const auto File = makeFile("/s/g/f");
  auto Lease = Files->mappingSource(File);
  path("/s/g/f");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/s/g");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  renameFile("/s", "/moved");
  path("/moved");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  for (auto FD : {Source, Removed, File})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  path("/extra");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  Lease = uint32_t(0);
  makeDirectory("/one");
  makeDirectory("/two");
  makeFile("/three");
  path("/extra");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
}

TEST_P(DarwinFileTest,
       DirectoryRenameNeedsNoFreeEntryDescriptorOrCreationInode) {
  Options->Files.clear();
  for (unsigned I = 0; I != 253; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  creationPolicy(UINT64_MAX);
  Options->DescriptorLimit = 4;
  path("/s");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto File = makeFile("/s/file", "x");
  EXPECT_EQ(llvm::support::endian::read64le(status(File).data() + 8),
            UINT64_MAX);
  renameFile("/s", "/moved");
  identity(File, "/moved/file");
  path("/extra");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  error(ServiceKind::Open, {Base}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {File}), 0u);
  path("/f0");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/moved/next");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationInode);
}

TEST_P(DarwinFileTest, SwapRetainsBothObjectsDescriptionsMetadataAndMappings) {
  mutationPolicy();
  Options->Directories.insert("/");
  Options->MutableDirectories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/target"] = {'T', 'G'};
  Options->WritableFiles.insert("/target");
  auto M = darwin_test::mutationMetadata(2);
  M.Inode++;
  M.GID = 77;
  M.Blocks = 1;
  Options->Metadata["/target"] = M;
  Options->MutationPolicies["/target"] = {512, {9, 42}};
  const auto A = ok(ServiceKind::Open, {Base, 0x100000a});
  const auto Independent = ok(ServiceKind::Open, {Base});
  const auto Duplicate = ok(ServiceKind::Dup, {A});
  auto Source = status(A);
  path("/target");
  const auto B = ok(ServiceKind::Open, {Base, 2});
  auto Target = status(B);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4, 0}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 1, 0}), 1u);
  auto SourceLease = Files->mappingSource(A);
  auto TargetLease = Files->mappingSource(B);
  swapFiles("/data", "/target");
  for (auto FD : {A, Independent, Duplicate})
    identity(FD, "/target");
  identity(B, "/data");
  llvm::support::endian::write64le(Source.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Source.data() + 72, 123456789);
  llvm::support::endian::write64le(Target.data() + 64, 9);
  llvm::support::endian::write64le(Target.data() + 72, 42);
  EXPECT_EQ(status(A), Source);
  EXPECT_EQ(status(B), Target);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Duplicate, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(SourceLease).Bytes[0], 'a');
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes[0], 'T');
  path("/data");
  const auto Reopened = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(status(Reopened), Target);
  contents(Reopened, {'T', 'G'});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  SourceLease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 1}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  TargetLease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 0}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(A).data() + 6), 1u);
  EXPECT_EQ(llvm::support::endian::read16le(status(B).data() + 6), 1u);
  EXPECT_EQ(Options->Metadata.at("/target").Inode, M.Inode);
  EXPECT_EQ(Options->Metadata.at("/target").GID, M.GID);
  EXPECT_EQ(Options->Metadata.at("/target").ChangeTime.Seconds,
            M.ChangeTime.Seconds);
  EXPECT_EQ(Options->Files.at("/target"), (std::vector<uint8_t>{'T', 'G'}));
}

TEST_P(DarwinFileTest, SwapRetainsOwnGrantsAcrossCreatedParents) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  auto Original = darwin_test::mutationMetadata(6);
  Original.GID = 7;
  Options->Metadata["/data"] = Original;
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Child = makeDirectory("/child");
  path("/child/target");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  auto Before = status(B);
  swapFiles("/data", "/child/target");
  identity(A, "/child/target");
  identity(B, "/data");
  llvm::support::endian::write64le(Before.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Before.data() + 72, 123456789);
  EXPECT_EQ(status(B), Before);
  error(ServiceKind::Ftruncate, {A, 0}, 22);
  path("/child/target");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 1}), 0u);
  path("target");
  const auto Reopened = ok(ServiceKind::OpenAt, {Child, Base});
  contents(Reopened, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(Options->Metadata.at("/data").Inode, Original.Inode);
  EXPECT_EQ(Options->Metadata.at("/data").GID, Original.GID);
  EXPECT_EQ(Options->Metadata.at("/data").ChangeTime.Seconds,
            Original.ChangeTime.Seconds);
}

TEST_P(DarwinFileTest, SwapDeclarationFollowsOriginalDirectoryObjects) {
  for (bool RootDeclared : {false, true}) {
    Options.emplace();
    Options->Files["/data"] = {'d'};
    Options->Directories = {"/", "/empty"};
    Options->MutableDirectories = {"/", "/empty"};
    Options->RemovableDirectories.insert("/empty");
    Options->SwapRenameDirectories.insert("/empty");
    if (RootDeclared)
      Options->SwapRenameDirectories.insert("/");
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
    const auto A = ok(ServiceKind::Open, {Base});
    path("/empty");
    const auto Old = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
    const auto New = makeDirectory("/empty");
    path("/empty/target");
    const auto B = ok(ServiceKind::Open, {Base, 0x202});
    path("/data");
    path("/empty/target", Base + 128);
    if (RootDeclared) {
      EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, New, Base + 128, 2}),
                0u);
      identity(A, "/empty/target");
      identity(B, "/data");
    } else {
      EXPECT_FALSE(
          invoke(ServiceKind::RenameAtX, {999, Base, New, Base + 128, 2}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
      identity(A, "/data");
      identity(B, "/empty/target");
    }
    identity(Old, "/empty");
    EXPECT_EQ(Options->SwapRenameDirectories.contains("/"), RootDeclared);
  }
}

TEST_P(DarwinFileTest, SwapKeepsLookupAndFlagErrorsBeforeDeclaration) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  Options->Directories.insert("/folder");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  for (uint64_t Flags : {2u, 0x12u}) {
    path("/data");
    for (auto [Name, Code] : {std::pair{"/missing", 2u},
                              {"/missing/", 2u},
                              {"/target/", 20u},
                              {"/folder/.", 22u},
                              {"/folder/..", 22u},
                              {"/missing/../target", 2u}}) {
      path(Name, Base + 128);
      error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}, Code);
    }
    error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, Flags}, 14);
    path("/missing");
    error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, Flags}, 2);
    path("/data");
    path("/folder", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapKind);
    path("/target", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameSwapSupport);
  }
  for (uint64_t Flags : {6u, 8u, 0x16u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          22);
  for (uint64_t Flags : {1u, 3u, 0x13u}) {
    EXPECT_FALSE(invoke(ServiceKind::RenameAtX,
                        {999, UINT64_MAX, 999, UINT64_MAX, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameFlags);
  }
  EXPECT_EQ(status(A), Before);
  identity(A, "/data");
}

TEST_P(DarwinFileTest, SwapRequiresSharedDomainAuthorityAndConsistentDevices) {
  Options->Directories = {"/", "/other"};
  Options->MutableDirectories = {"/", "/other"};
  Options->SwapRenameDirectories = {"/", "/other"};
  Options->Files["/other/target"] = {'T'};
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Metadata["/other"] = darwin_test::creationParentMetadata();
  Options->Metadata["/other"].Inode++;
  const auto A = ok(ServiceKind::Open, {Base});
  path("/other/target", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(A, "/data");
  Options->Files.erase("/other/target");
  Options->Files["/target"] = {'T'};
  Options->Metadata["/target"] = darwin_test::mutationMetadata(1);
  Options->Metadata["/target"].Device++;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/data");
  path("/target", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  Options->MutableDirectories.clear();
  Options->SwapRenameDirectories.clear();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
}

TEST_P(DarwinFileTest, SwapNoopPreservesObservationsWithoutCapabilityOrCharge) {
  mutationPolicy();
  creationPolicy();
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  const auto Lease = Files->mappingSource(A);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 3, 0}), 3u);
  swapFiles("/data", "/./data");
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(Parent), ParentBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  path("/fresh");
  const auto B = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(llvm::support::endian::read64le(status(B).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_TRUE(Options->SwapRenameDirectories.empty());
}

TEST_P(DarwinFileTest, SwapConsumesNoDescriptorEntryOrCreationInode) {
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  swapFiles("/data", "/target");
  path("/fresh");
  const auto A = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  Options->CreationPolicy.reset();
  Options->InitialUmask.reset();
  Options->Metadata.clear();
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  swapFiles("/f0", "/f1");
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  Options->DescriptorLimit = 3;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  swapFiles("/f0", "/f1");
  path("/f0");
  error(ServiceKind::Open, {Base}, 24);
}

TEST_P(DarwinFileTest,
       SwapChargesBothNamesWithoutCreditingLinkedBytesOrLeases) {
  for (unsigned Retained = 0; Retained != 4; ++Retained) {
    SCOPED_TRACE(Retained);
    Options.emplace();
    Options->Files["/data"] = std::vector<uint8_t>(6, 'd');
    Options->Files["/target"] =
        std::vector<uint8_t>(darwin_file_limits::Bytes - 45, 't');
    Options->WritableFiles.insert("/data");
    Options->Directories.insert("/");
    Options->MutableDirectories.insert("/");
    Options->SwapRenameDirectories.insert("/");
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/data");
    const auto A = ok(ServiceKind::Open, {Base, 2});
    path("/target");
    const auto B = ok(ServiceKind::Open, {Base});
    auto Lease = Retained & 2 ? Files->mappingSource(B)
                              : DarwinFiles::MappingSource(uint32_t(0));
    if (!(Retained & 1))
      EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
    path("/data");
    path("/target", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
    identity(A, "/data");
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 5}), 0u);
    swapFiles("/data", "/target");
    identity(A, "/target");
    if (Retained & 1)
      identity(B, "/data");
    if (Retained & 2)
      EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.size(),
                darwin_file_limits::Bytes - 45);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 6}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    Lease = uint32_t(0);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 6}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    swapFiles("/target", "/data");
    swapFiles("/data", "/target");
    identity(A, "/target");
    contents(A, std::vector<uint8_t>(5, 'd'));
  }
}

TEST_P(DarwinFileTest, DirectoryCreationDistinguishesTrailingSlashesFromDots) {
  Options->MutableDirectories.insert("/");
  for (const char *Name : {"/data/", "/data/.", "/data/.."}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base, 0700}, 20);
  }
  for (const char *Name : {"/absent/.", "/absent/..", "/absent/child"}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base, 0700}, 2);
  }
  for (const char *Name : {"/", "/data", "/."}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base, UINT64_MAX}, 17);
  }
  path("/new///");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0x12345678ffff01c0ULL}), 0u);
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  for (const char *Name : {"/new", "/new/.", "/new/.."}) {
    path(Name);
    error(ServiceKind::Mkdir, {Base}, 17);
  }
  path("/new/child//");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  error(ServiceKind::Open, {Base, 2}, 21);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  path("/new/.");
  error(ServiceKind::Rmdir, {Base}, 22);
  path("/new///");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_TRUE(Options->Directories.empty());
}

TEST_P(DarwinFileTest, DirectoryAtCallsKeepFlagsCarrierAndLookupOrder) {
  Options->MutableDirectories.insert("/");
  const auto Regular = ok(ServiceKind::Open, {Base});
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  error(ServiceKind::MkdirAt, {UINT64_MAX, 1, 0700}, 14);
  error(ServiceKind::MkdirAt, {UINT64_MAX, Base, 0700}, 9);
  error(ServiceKind::MkdirAt, {Regular, Base, 0700}, 20);
  EXPECT_FALSE(invoke(ServiceKind::MkdirAt, {1, Base, 0700}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_EQ(ok(ServiceKind::MkdirAt,
               {0x1234567800000000ULL | Root, Base, UINT64_MAX}),
            0u);
  error(ServiceKind::UnlinkAt, {UINT64_MAX, 1, 0x40}, 22);
  error(ServiceKind::UnlinkAt, {UINT64_MAX, 1, 0x880}, 14);
  for (uint32_t Flags : {0x100u, 0x180u, 0x1000u, 0x1880u}) {
    EXPECT_FALSE(invoke(ServiceKind::UnlinkAt, {UINT64_MAX, 1, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::UnlinkFlags);
  }
  error(ServiceKind::UnlinkAt, {Regular, Base, 0x80}, 20);
  path("");
  error(ServiceKind::MkdirAt, {UINT64_MAX, Base}, 9);
  error(ServiceKind::MkdirAt, {Regular, Base}, 20);
  error(ServiceKind::MkdirAt, {Root, Base}, 2);
  path("/new");
  EXPECT_EQ(
      ok(ServiceKind::UnlinkAt, {UINT64_MAX, Base, 0xfedcba9800000880ULL}), 0u);
  EXPECT_EQ(ok(ServiceKind::MkdirAt, {UINT64_MAX, Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  Options.reset();
  error(ServiceKind::UnlinkAt, {UINT64_MAX, 1, 0x40}, 22);
  EXPECT_FALSE(invoke(ServiceKind::MkdirAt, {UINT64_MAX, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
}

TEST_P(DarwinFileTest, DirectoryCreationRequiresGrantsButNoFreeDescriptor) {
  Options->Directories.insert("/fixed");
  Options->DescriptorLimit = 3;
  path("/fixed/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/fixed");
  error(ServiceKind::Mkdir, {Base}, 17);
  EXPECT_FALSE(invoke(ServiceKind::Rmdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryRemovalInitial);
  Options->MutableDirectories.insert("/");
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  error(ServiceKind::Open, {Base}, 24);
  path("/new/child");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/data");
  error(ServiceKind::Rmdir, {Base}, 20);
  path("/");
  error(ServiceKind::Rmdir, {Base}, 21);
}

TEST_P(DarwinFileTest, RootRemovalDistinguishesSlashOnlyFromDotComponents) {
  for (const char *Name : {"/", "//", "///", "/.", "/..", "//.//"}) {
    SCOPED_TRACE(Name);
    path(Name);
    const uint64_t Code = llvm::StringRef(Name).contains('.') ? 16 : 21;
    error(ServiceKind::Rmdir, {Base}, Code);
    error(ServiceKind::Unlink, {Base}, Code);
    error(ServiceKind::UnlinkAt, {UINT64_MAX, Base, 0}, Code);
    error(ServiceKind::UnlinkAt, {UINT64_MAX, Base, 0x80}, Code);
    EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  }
}

TEST_P(DarwinFileTest, DirectoryRemovalPreservesHeldAndNonemptyObjects) {
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Directory});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
  path("child");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/new/child/..");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/new/child");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  identity(Dup, "/new");
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Dup}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Dup}), 0u);
  path(".");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("/new");
  const auto File = ok(ServiceKind::Open, {Base, 0x202});
  error(ServiceKind::Open, {Base, 0x100000}, 20);
  identity(File, "/new");
  path(".");
  const auto OldDirectory = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {OldDirectory, UINT64_MAX, 0}, 21);
  path("/", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base + 128}), 0u);
}

TEST(DarwinFileOptions, RemovableDirectoriesRequireExplicitConsistentIdentity) {
  DarwinFileOptions Good;
  Good.Files["/data"] = {};
  Good.Directories.insert("/empty");
  Good.MutableDirectories.insert("/");
  Good.RemovableDirectories.insert("/empty");
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (const char *Name : {"/", "/missing", "/data"}) {
    auto O = Good;
    O.RemovableDirectories = {Name};
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryRemovableOption);
  }
  auto O = Good;
  O.MutableDirectories.clear();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryRemovableOption);
  O = Good;
  O.Files["/implicit/child"] = {};
  O.RemovableDirectories = {"/implicit"};
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryRemovableOption);
  auto M = darwin_test::mutationMetadata(64);
  M.Mode = 0040755;
  M.Inode = 41;
  Good.Metadata["/"] = M;
  M.Inode = 42;
  Good.Metadata["/empty"] = M;
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (unsigned Flag : {1u, 2u, 4u, 0x20000u, 0x40000u, 0x100000u}) {
    O = Good;
    O.Metadata["/empty"].Flags = Flag;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceFlags);
  }
  for (unsigned Bits : {01000u, 02000u, 04000u}) {
    O = Good;
    O.Metadata["/empty"].Mode |= Bits;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceFlags);
  }
  O = Good;
  O.Metadata["/empty"].Device++;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryRemovalDevice);
  O = Good;
  O.Directories.insert("/alias");
  O.Metadata["/alias"] = O.Metadata.at("/empty");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)), diagnostic::NamespaceAlias);
  O.Metadata["/alias"].Device++;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  // Snapshot identities also constrain removal without complete stat inputs.
  O = {};
  O.Directories = {"/empty", "/alias"};
  O.MutableDirectories.insert("/");
  O.RemovableDirectories.insert("/empty");
  O.DirectoryContents["/"] = darwin_test::directoryContents();
  auto &Alias = O.DirectoryContents["/"].Entries[3];
  Alias.Name = "alias";
  Alias.Type = 4;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  Alias.Inode = O.DirectoryContents["/"].Entries[2].Inode;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)), diagnostic::NamespaceAlias);
}

TEST(DarwinFileOptions, RemovableReferencesJoinTheInitialStorageFootprint) {
  DarwinFileOptions O;
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 18);
  O.Directories.insert("/old");
  O.MutableDirectories.insert("/");
  O.RemovableDirectories.insert("/old");
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.StandardInput = {0};
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, InitialDirectoryRemovalKeepsCWDAndReusedFileSeparate) {
  Options->Directories.insert("/empty");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/empty");
  Options->WorkingDirectory = "/empty";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/empty");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Old});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  const auto New = ok(ServiceKind::Open, {Base, 0x202});
  path("xy", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {New, Base + 128, 2}), 2u);
  identity(Old, "/empty");
  identity(New, "/empty");
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Dup}), 0u);
  path(".");
  const auto CWD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {CWD, UINT64_MAX, 0}, 21);
  path("child");
  error(ServiceKind::Access, {Base}, 2);
  path("../empty");
  const auto SameNew = ok(ServiceKind::Open, {Base});
  contents(SameNew, {'x', 'y'});
  EXPECT_TRUE(Options->Directories.contains("/empty"));
  EXPECT_EQ(Options->WorkingDirectory, "/empty");
  EXPECT_TRUE(Options->RemovableDirectories.contains("/empty"));
}

TEST_P(DarwinFileTest, InitialDirectorySnapshotsNeverReturnAfterNameReuse) {
  Options->Directories.insert("/empty");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/empty");
  auto M = darwin_test::mutationMetadata(64);
  M.Mode = 0040755;
  M.Inode = 41;
  Options->Metadata["/"] = M;
  M.Inode = 0xfedcba9876543211ULL;
  Options->Metadata["/empty"] = M;
  auto RootSnapshot = darwin_test::directoryContents();
  RootSnapshot.Entries[2].Inode = M.Inode;
  Options->DirectoryContents["/"] = RootSnapshot;
  auto Snapshot = darwin_test::directoryContents();
  Snapshot.Entries.resize(2);
  Snapshot.Entries[0].Inode = M.Inode;
  Options->DirectoryContents["/empty"] = Snapshot;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("/empty");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Before = status(Old);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Old, Base + Page, 64, Base + 512}),
      64u);
  const auto Cursor = ok(ServiceKind::Lseek, {Old, 0, 1});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Old, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {Old, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Old, 0, 1}), Cursor);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto New = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {New, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {New, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  EXPECT_EQ(ok(ServiceKind::Lseek, {New, 0, 1}), 0u);
  EXPECT_EQ(Options->Metadata.at("/empty").Inode,
            llvm::support::endian::read64le(Before.data() + 8));
  EXPECT_EQ(Options->DirectoryContents.at("/empty").Entries.size(), 2u);
}

TEST_P(DarwinFileTest, InitialDirectoryParentChainDoesNotAttachToReplacement) {
  Options->Directories = {"/p", "/p/c"};
  Options->MutableDirectories = {"/", "/p"};
  Options->RemovableDirectories = {"/p", "/p/c"};
  Options->WorkingDirectory = "/p/c";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/p");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("../c");
  error(ServiceKind::Access, {Base}, 2);
  path("../../p/c");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("..");
  const auto Parent = ok(ServiceKind::Open, {Base});
  identity(Parent, "/p");
  path("c");
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  path("../.");
  error(ServiceKind::Mkdir, {Base}, 2);
  EXPECT_TRUE(Options->Directories.contains("/p/c"));
}

TEST_P(DarwinFileTest, InitialDirectoryImplicitChildrenSurviveRegularUnlink) {
  Options->Files["/p/implicit/leaf"] = {'v'};
  Options->Directories.insert("/p");
  Options->MutableDirectories = {"/", "/p", "/p/implicit"};
  Options->RemovableDirectories.insert("/p");
  path("/p/implicit/leaf");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/p");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/p/implicit");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Rmdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryRemovalInitial);
  contents(Old, {'v'});
  // A separate admitted configuration can explicitly remove that initial child.
  Options->Directories.insert("/p/implicit");
  Options->RemovableDirectories.insert("/p/implicit");
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/p/implicit/leaf");
  const auto Held = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/p/implicit");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  path("/p/implicit");
  error(ServiceKind::Access, {Base}, 2);
  contents(Held, {'v'});
}

TEST_P(DarwinFileTest, InitialDirectoryTombstonesCountOnlyLiveReplacements) {
  Options->Directories = {"/p", "/p/c"};
  Options->MutableDirectories = {"/", "/p"};
  Options->RemovableDirectories = {"/p", "/p/c"};
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto File = ok(ServiceKind::Open, {Base, 0x202});
  path("/p");
  error(ServiceKind::Rmdir, {Base}, 66);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  error(ServiceKind::Access, {Base}, 2);
  identity(File, "/p/c");
  contents(File, {});
}

TEST_P(DarwinFileTest, InitialDirectoryRemovalNeverRefundsFixedStorage) {
  Options->WritableFiles.insert("/data");
  Options->Directories.insert("/old");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/old");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 24;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  path("/old");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, InitialDirectoryRemovalNeverRefundsFixedEntries) {
  Options->Files.clear();
  for (unsigned I = 0; I != 254; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->Directories.insert("/old");
  Options->MutableDirectories.insert("/");
  Options->RemovableDirectories.insert("/old");
  path("/old");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  path("/f0");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/another");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
}

TEST_P(DarwinFileTest, InitialDirectoryRecreationInheritsTheCurrentParent) {
  creationPolicy();
  Options->Directories.insert("/empty");
  Options->RemovableDirectories.insert("/empty");
  auto Metadata = darwin_test::creationParentMetadata();
  Metadata.Inode++;
  Metadata.GID = 77;
  Options->Metadata["/empty"] = Metadata;
  path("/empty");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/empty/child");
  const auto Child = ok(ServiceKind::Open, {Base, 0x202});
  const auto Record = status(Child);
  EXPECT_EQ(llvm::support::endian::read32le(Record.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read32le(Record.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read64le(Record.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Old, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  path("child");
  error(ServiceKind::OpenAt, {Old, Base}, 2);
  EXPECT_EQ(Options->Metadata.at("/empty").GID, 77u);
}

TEST_P(DarwinFileTest, DeletedDirectoryDotOpensKeepIndependentCursors) {
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Old});
  path(".", Base + 128);
  const auto Independent = ok(ServiceKind::OpenAt, {Old, Base + 128});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Old, 7, 0}), 7u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto New = ok(ServiceKind::Open, {Base});
  path("/new/child");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("child");
  error(ServiceKind::OpenAt, {Old, Base}, 2);
  error(ServiceKind::FaccessAt, {Dup, Base}, 2);
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {New, Base}), 0u);
  path(".");
  const auto Reopened = ok(ServiceKind::OpenAt, {Old, Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Independent, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Reopened, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {New, 0, 1}), 0u);
  identity(Reopened, "/new");
  error(ServiceKind::Read, {Old, UINT64_MAX, 0}, 21);
  EXPECT_EQ(std::get<uint32_t>(Files->mappingSource(Old)), 22u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Old, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {Old, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {New, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
}

TEST_P(DarwinFileTest, DeletedDirectoryParentsRemainObjectsAfterNameReuse) {
  Options->MutableDirectories.insert("/");
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Child = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c/fresh");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("..");
  const auto Parent = ok(ServiceKind::Open, {Base});
  identity(Parent, "/p");
  path("c");
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  path("../../p/c/fresh");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("../c");
  error(ServiceKind::Access, {Base}, 2);
  path("..");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Parent}), 0u);
  path("c");
  error(ServiceKind::Access, {Base}, 2);
  path("../p/c");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  path("fresh");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
}

TEST_P(DarwinFileTest, DeletedDirectoryNamespaceLookupKeepsNativeIntent) {
  Options->MutableDirectories.insert("/");
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Child = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("..");
  error(ServiceKind::MkdirAt, {Child, Base}, 17);
  error(ServiceKind::OpenAt, {Child, Base, 0xa00}, 17);
  error(ServiceKind::UnlinkAt, {Child, Base, 0}, 1);
  error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 2);
  path("../.");
  error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 22);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  for (const char *Name : {"..", "../.", "./..", "../..", "../../."}) {
    SCOPED_TRACE(Name);
    path(Name);
    EXPECT_EQ(ok(ServiceKind::FaccessAt, {Child, Base}), 0u);
    error(ServiceKind::MkdirAt, {Child, Base}, 2);
    error(ServiceKind::OpenAt, {Child, Base, 0xa00}, 2);
    error(ServiceKind::UnlinkAt, {Child, Base, 0}, 2);
    error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 2);
  }
  for (const char *Name : {".", "./", "./."}) {
    path(Name);
    error(ServiceKind::MkdirAt, {Child, Base}, 17);
    error(ServiceKind::OpenAt, {Child, Base, 0xa00}, 17);
    error(ServiceKind::UnlinkAt, {Child, Base, 0}, 1);
    error(ServiceKind::UnlinkAt, {Child, Base, 0x80}, 22);
  }
  path("new");
  error(ServiceKind::MkdirAt, {Child, Base}, 2);
  error(ServiceKind::OpenAt, {Child, Base, 0x202}, 2);
  error(ServiceKind::FaccessAt, {Child, Base, 4}, 2);
  error(ServiceKind::MkdirAt, {UINT64_MAX, 1}, 14);
  error(ServiceKind::UnlinkAt, {Child, 1, 0x80}, 14);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {Child, Base}), 0u);
  error(ServiceKind::MkdirAt, {Child, Base}, 17);
  path("");
  error(ServiceKind::OpenAt, {Child, Base}, 2);
  path("/data", Base + 128);
  for (const char *Name : {".", "./.", "..", "./..", "..//"}) {
    path(Name);
    error(ServiceKind::RenameAt, {UINT64_MAX, Base + 128, Child, Base}, 22);
  }
  for (const char *Name :
       {"../.", "../..", "../../.", "missing/..", "../../new"}) {
    path(Name);
    error(ServiceKind::RenameAt, {UINT64_MAX, Base + 128, Child, Base}, 2);
  }
  path("../../data");
  error(ServiceKind::RenameAt, {Child, Base, UINT64_MAX, Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
}

TEST_P(DarwinFileTest, DeletedDirectoryPathBudgetRetainsCWDThenReclaims) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/x");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  error(ServiceKind::Access, {Base}, 2);
  path(".");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  path("/");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, DeletedDirectoryEntryBudgetRetainsParentChains) {
  Options->Files.clear();
  for (unsigned I = 0; I != 253; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/p/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Child = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/p");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/a");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  path("/", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/b");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/c");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
}

TEST_P(DarwinFileTest, DeletedDirectoryDup2ReleasesOnlyReplacedObjects) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Directory});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity}));
  identity(Dup, "/new");
  EXPECT_EQ(ok(ServiceKind::Dup2, {Data, Dup}), Dup);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  identity(Dup, "/data");
}

TEST_P(DarwinFileTest, DeletedDirectoryCannotAcquireReusedFileMetadata) {
  mutationPolicy();
  creationPolicy();
  const auto Original = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto Replacement = ok(ServiceKind::Open, {Base, 0x202, 0600});
  const auto NewStatus = status(Replacement);
  EXPECT_EQ(llvm::support::endian::read64le(NewStatus.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path(".");
  const auto Reopened = ok(ServiceKind::OpenAt, {Directory, Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Reopened, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  error(ServiceKind::Read, {Reopened, UINT64_MAX, 0}, 21);
  identity(Reopened, "/data");
  contents(Original, {'a', 'b', 0, 0xff, 'e', 'f'});
  contents(Replacement, {});
  EXPECT_EQ(status(Replacement), NewStatus);
}

TEST_P(DarwinFileTest, DirectoryPublicationInvalidatesOnlyChangedParents) {
  Options->WritableFiles.insert("/data");
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  auto RootMetadata = darwin_test::mutationMetadata(64);
  RootMetadata.Mode = 0040755;
  RootMetadata.Inode = 41;
  Options->Metadata["/"] = RootMetadata;
  Options->MutableDirectories.insert("/");
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  const auto Before = status(Root);
  path("/data");
  error(ServiceKind::Mkdir, {Base}, 17);
  path("/empty/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(status(Root), Before);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Root, Base + Page, 64, Base + 512}),
      64u);
  const auto Cursor = ok(ServiceKind::Lseek, {Root, 0, 1});
  path("/data", Base + 128);
  const auto Data = ok(ServiceKind::Open, {Base + 128, 2});
  // Initial paths/references cost 23 bytes; four observed records cost 128.
  // Four spare bytes cannot pay for "/new" including its terminator.
  EXPECT_EQ(
      ok(ServiceKind::Ftruncate, {Data, darwin_file_limits::Bytes - 151 - 4}),
      0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(status(Root), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), Cursor);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Root, Base + Page, 64, Base + 512}),
      64u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 512, 8)), Cursor);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, Cursor, 0}), Cursor);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {Root, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), Cursor);
  const auto New = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {New, UINT64_MAX}));
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {New, UINT64_MAX, 64, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  EXPECT_EQ(ok(ServiceKind::Close, {New}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(Options->Metadata.at("/").Size, 64u);
}

TEST_P(DarwinFileTest, DirectoryAndFileNamesShareEntryBudgetAndReclaim) {
  Options->Files.clear();
  for (unsigned I = 0; I != 254; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/extra", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base + 128, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/f0");
  const auto Old = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base + 128}));
  EXPECT_EQ(ok(ServiceKind::Close, {Old}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  for (unsigned I = 0; I != 300; ++I) {
    SCOPED_TRACE(I);
    path("/d" + std::to_string(I));
    EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
    EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  }
}

TEST_P(DarwinFileTest, DirectoryRefundDoesNotReleaseOrphanFileStorage) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  const uint64_t Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/x", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base + 128}));
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 14}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new/f");
  const auto Child = ok(ServiceKind::Open, {Base, 0x202});
  path("xy", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {Child, Base + 128, 2}), 2u);
  auto Lease = Files->mappingSource(Child);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  // The mapping-held orphan retains both its nine bytes and its parent path.
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 14}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity - 13}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'x', 'y'}));
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
}

TEST_P(DarwinFileTest, DirectoryReuseShadowsOldFileMetadataAndKeepsItsLease) {
  mutationPolicy();
  creationPolicy();
  Options->Metadata["/data"].Device = 456;
  Options->Metadata["/data"].GID = 123;
  const auto Old = ok(ServiceKind::Open, {Base});
  auto Lease = Files->mappingSource(Old);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto Unlinked = status(Old);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Directory, UINT64_MAX}));
  path("/data/child");
  const auto Child = ok(ServiceKind::Open, {Base, 0x202, 0777});
  const auto Record = status(Child);
  EXPECT_EQ(llvm::support::endian::read32le(Record.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read32le(Record.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read64le(Record.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  renameFile("/data/child", "/data/child");
  EXPECT_EQ(status(Child), Record);
  path("/data/moved");
  const auto Target = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("t", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {Target, Base + 128, 1}), 1u);
  renameFile("/data/child", "/data/moved");
  identity(Child, "/data/moved");
  contents(Target, {'t'});
  EXPECT_EQ(llvm::support::endian::read16le(status(Target).data() + 6), 0u);
  EXPECT_EQ(status(Old), Unlinked);
  contents(Old, Options->Files.at("/data"));
  EXPECT_EQ(ok(ServiceKind::Close, {Child}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Target}), 0u);
  path("/data/moved");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto New = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(New).data() + 8),
            darwin_test::CreationPolicy.FirstInode + 2);
  EXPECT_EQ(status(Old), Unlinked);
  identity(Old, "/data");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Options->Files.at("/data")));
  EXPECT_EQ(Options->Metadata.at("/data").GID, 123u);
}

TEST_P(DarwinFileTest, DirectoryIdentityInheritanceDoesNotInventDirectoryStat) {
  creationPolicy();
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0}), 0u);
  path("/new/deep");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, UINT64_MAX}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Directory, UINT64_MAX}));
  path("child");
  const auto File = ok(ServiceKind::OpenAt, {Directory, Base, 0x202, 0777});
  const auto Record = status(File);
  EXPECT_EQ(llvm::support::endian::read32le(Record.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read32le(Record.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read16le(Record.data() + 4), 0100750u);
  EXPECT_EQ(llvm::support::endian::read64le(Record.data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path("/new/deep");
  error(ServiceKind::Rmdir, {Base}, 66);
  EXPECT_EQ(Options->CreationPolicy->FirstInode,
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest, DirectoryRemovalAndReuseRetainUnlinkedChildObjects) {
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new/f");
  const auto Old = ok(ServiceKind::Open, {Base, 0x202});
  path("old!", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {Old, Base + 128, 4}), 4u);
  auto Lease = Files->mappingSource(Old);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  path("/new/f");
  const auto New = ok(ServiceKind::Open, {Base, 0x202});
  path("new!", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {New, Base + 128, 4}), 4u);
  contents(Old, {'o', 'l', 'd', '!'});
  contents(New, {'n', 'e', 'w', '!'});
  identity(Old, "/new/f");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'o', 'l', 'd', '!'}));
}

TEST_P(DarwinFileTest, DirectoryCreationBoundsCanonicalNamesAndInputCopy) {
  std::string Parent;
  for (char C : {'a', 'b', 'c', 'd'})
    Parent += '/' + std::string(240, C);
  Options->Directories.insert(Parent);
  Options->MutableDirectories.insert(Parent);
  Options->WorkingDirectory = Parent;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path(std::string(59, 'x'));
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  path(std::string(58, 'y'));
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
  const auto Directory = ok(ServiceKind::Open, {Base});
  identity(Directory, Parent + '/' + std::string(58, 'y'));
  path("z", Base + Page * 2 - 2);
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base + Page * 2 - 2}), 0u);
  llvm::cantFail(Space->writeInteger(Base + Page * 2 - 1, 'q', 1));
  error(ServiceKind::Mkdir, {Base + Page * 2 - 1}, 14);
  path("q");
  error(ServiceKind::Access, {Base}, 2);
  llvm::cantFail(Space->protect(Base, Page * 2, Read | UserAccessible));
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base}), 0u);
}

TEST_P(DarwinFileTest,
       AccessZeroActionBitsAndRequestedPermissionsStayDistinct) {
  const uint64_t Ignored[] = {
      0,        8,          0x80,       0x100,
      0x400000, 0x80000000, 0xffc001f8, 0x1234567800000000ULL};
  for (auto Mode : Ignored) {
    EXPECT_EQ(ok(ServiceKind::Access, {Base, Mode}), 0u);
    EXPECT_EQ(ok(ServiceKind::FaccessAt, {UINT64_MAX, Base, Mode, 0x830}), 0u);
  }
  const uint32_t Rights[] = {
      1,      2,      4,       0x200,   0x400,   0x800,   0x1000,   0x2000,
      0x4000, 0x8000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000, 0x200000};
  for (auto Right : Rights) {
    for (uint64_t Mode : {uint64_t(Right), uint64_t(0xffc001f8) | Right}) {
      EXPECT_FALSE(invoke(ServiceKind::Access, {Base, Mode}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
      EXPECT_FALSE(invoke(ServiceKind::FaccessAt, {UINT64_MAX, Base, Mode}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
      error(ServiceKind::Access, {1, Mode}, 14);
      path("/missing", Base + 128);
      error(ServiceKind::Access, {Base + 128, Mode}, 2);
      error(ServiceKind::FaccessAt, {UINT64_MAX, Base + 128, Mode}, 2);
    }
  }
  EXPECT_FALSE(invoke(ServiceKind::Access, {Base, 0x80001000}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
}

TEST_P(DarwinFileTest, FaccessFlagsPrecedePathAndUseOnlyLowCarrierBits) {
  Options.reset();
  for (uint32_t Flags : {1u, 0x40u, 0x400u, UINT32_MAX})
    error(ServiceKind::FaccessAt, {UINT64_MAX, 1, UINT64_MAX, Flags}, 22);
  EXPECT_FALSE(invoke(ServiceKind::Access, {1, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  EXPECT_FALSE(invoke(ServiceKind::FaccessAt, {UINT64_MAX, 1, 0, 0x830}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  Options.emplace();
  Options->DescriptorLimit = 3;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/");
  for (uint32_t Flags :
       {0u, 0x10u, 0x20u, 0x30u, 0x800u, 0x810u, 0x820u, 0x830u}) {
    EXPECT_EQ(ok(ServiceKind::FaccessAt,
                 {UINT64_MAX, Base, 0, 0xfedcba9800000000ULL | Flags}),
              0u);
    error(ServiceKind::FaccessAt, {UINT64_MAX, 1, 0, Flags}, 14);
  }
  path("/missing");
  error(ServiceKind::Access, {Base}, 2);
  // No available FD is required, and no query consumes a descriptor.
  error(ServiceKind::Open, {Base}, 24);
}

TEST_P(DarwinFileTest, AccessRelativePathsRetainFDAndAncestorErrorOrder) {
  const auto File = ok(ServiceKind::Open, {Base});
  path("/", Base + 128);
  const auto Directory = ok(ServiceKind::Open, {Base + 128});
  path("data");
  EXPECT_FALSE(invoke(ServiceKind::Access, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {Directory, Base, 0, 0x830}), 0u);
  EXPECT_EQ(
      ok(ServiceKind::FaccessAt, {0x1234567800000000ULL | Directory, Base}),
      0u);
  error(ServiceKind::FaccessAt, {File, Base}, 20);
  error(ServiceKind::FaccessAt, {UINT64_MAX, Base}, 9);
  EXPECT_FALSE(invoke(ServiceKind::FaccessAt, {1, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  for (const char *Text : {"data/", "data/..", "data/.", "data/child"}) {
    path(Text);
    error(ServiceKind::FaccessAt, {Directory, Base}, 20);
  }
  path("missing/..");
  error(ServiceKind::FaccessAt, {Directory, Base}, 2);
  path("");
  error(ServiceKind::FaccessAt, {Directory, Base}, 2);
  error(ServiceKind::FaccessAt, {File, Base}, 20);
  error(ServiceKind::FaccessAt, {UINT64_MAX, Base}, 9);
  error(ServiceKind::FaccessAt, {UINT64_MAX, 1}, 14);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  path("data");
  error(ServiceKind::FaccessAt, {Directory, Base}, 9);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::FaccessAt, {Directory, Base}), 0u);
}

TEST_P(DarwinFileTest, ExistenceDoesNotReadMetadataOrAlterDescriptionState) {
  mutationPolicy();
  Options->Metadata["/data"].Mode = 0x8000; // Observation, not an access grant.
  Options->DescriptorLimit = 5;
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  const auto Dup = ok(ServiceKind::Dup, {FD});
  const auto Before = status(FD);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
    EXPECT_EQ(ok(ServiceKind::FaccessAt, {Dup, Base}), 0u);
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 2u);
  }
  error(ServiceKind::Open, {Base}, 24);
  error(ServiceKind::Write, {FD, 1, 1}, 14);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Access, {Base, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileAccessPermissions);
}

TEST_P(DarwinFileTest, AccessUsesLiveNamesAcrossRemovalReuseAndRename) {
  mutationPolicy();
  creationPolicy();
  const auto Old = ok(ServiceKind::Open, {Base});
  auto Lease = Files->mappingSource(Old);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.size(), 6u);
  contents(Old, {'a', 'b', 0, 0xff, 'e', 'f'});
  const auto New = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  contents(New, {});
  renameFile("/data", "/moved");
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base + 128}), 0u);
  error(ServiceKind::Access, {Base + 128}, 2);
  path("/");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
}

TEST_P(DarwinFileTest, AccessStringCopyStopsAtNULAndPreservesPathFaults) {
  const auto End = Base + Page * 2;
  path("/data", End - 6);
  EXPECT_EQ(ok(ServiceKind::Access, {End - 6}), 0u);
  ASSERT_FALSE(bool(Space->write(End - 1, {'x'})));
  error(ServiceKind::Access, {End - 6}, 14);
  error(ServiceKind::Access, {UINT64_MAX}, 14);
  error(ServiceKind::Access, {0x800000000000ULL}, 14);
  ASSERT_FALSE(bool(Space->write(Base, std::vector<uint8_t>(1024, 'x'))));
  error(ServiceKind::Access, {Base}, 63);
  path("/" + std::string(256, 'x'));
  error(ServiceKind::Access, {Base}, 63);
  path("/data");
  ASSERT_FALSE(bool(Space->protect(Base, Page, Read | UserAccessible)));
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base, Page, Write | UserAccessible)));
  error(ServiceKind::Access, {Base}, 14);
}

TEST_P(DarwinFileTest, RenameMovesSharedIdentityAndRetainsReplacedObjects) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/target"] = {'T', 'G'};
  Options->WritableFiles.insert("/target");
  auto TargetMetadata = darwin_test::mutationMetadata(2);
  TargetMetadata.Inode++;
  TargetMetadata.Blocks = 1;
  Options->Metadata["/target"] = TargetMetadata;
  Options->MutationPolicies["/target"] = {512, {9, 42}};
  const auto A = ok(ServiceKind::Open, {Base, 0x100000a});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  auto SourceBefore = status(A);
  path("/target");
  const auto T = ok(ServiceKind::Open, {Base, 2});
  auto TargetBefore = status(T);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4, 0}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {T, 1, 0}), 1u);
  auto SourceLease = Files->mappingSource(A);
  auto TargetLease = Files->mappingSource(T);
  renameFile("/data", "/moved");
  for (auto FD : {A, B, D})
    identity(FD, "/moved");
  llvm::support::endian::write64le(SourceBefore.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(SourceBefore.data() + 72, 123456789);
  EXPECT_EQ(status(B), SourceBefore);
  EXPECT_EQ(status(T), TargetBefore);
  renameFile("/moved", "/target");
  llvm::support::endian::write16le(TargetBefore.data() + 6, 0);
  llvm::support::endian::write64le(TargetBefore.data() + 64, 9);
  llvm::support::endian::write64le(TargetBefore.data() + 72, 42);
  EXPECT_EQ(status(A), SourceBefore);
  EXPECT_EQ(status(T), TargetBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 4u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {T, 0, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {T, 3}), 2u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(SourceLease).Bytes[0], 'a');
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes[0], 'T');
  contents(B, {'a', 'b', 0, 0xff, 'e', 'f'});
  contents(T, {'T', 'G'});
  renameFile("/target", "/next");
  for (auto FD : {A, B, D})
    identity(FD, "/next");
  identity(T, "/target");
  path("/data");
  error(ServiceKind::Open, {Base}, 2);
  path("/target");
  error(ServiceKind::Open, {Base}, 2);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  SourceLease = TargetLease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {T, 0}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(T).data() + 6), 0u);
  EXPECT_EQ(Options->Files.at("/data").size(), 6u);
  EXPECT_EQ(Options->Metadata.at("/target").LinkCount, 1u);
  EXPECT_EQ(Options->MutationPolicies.at("/target").Time.Seconds, 9);
}

TEST_P(DarwinFileTest, RenameNoopPreservesObservationsAndCreationSequence) {
  mutationPolicy();
  creationPolicy();
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  auto Lease = Files->mappingSource(A);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 3, 0}), 3u);
  renameFile("/data", "/./data");
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(Parent), ParentBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  identity(A, "/data");
  path("/new");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(B).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest, RenameEvenNoopRequiresNamespaceAuthority) {
  Options->Metadata["/data"] = darwin_test::mutationMetadata(6);
  Options->Metadata["/data"].Flags = 2;
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/./data", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  EXPECT_EQ(status(A), Before);
  identity(A, "/data");
}

TEST_P(DarwinFileTest,
       RenameExhaustedInodesAndVirginMetadataRemainIndependent) {
  creationPolicy(UINT64_MAX - 1);
  path("/source");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0755});
  auto Source = status(A);
  path("/target");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  auto Target = status(B);
  renameFile("/source", "/target");
  for (auto *Bytes : {&Source, &Target}) {
    llvm::support::endian::write64le(Bytes->data() + 64, uint64_t(-7));
    llvm::support::endian::write64le(Bytes->data() + 72, 123456789);
  }
  llvm::support::endian::write16le(Target.data() + 6, 0);
  EXPECT_EQ(status(A), Source);
  EXPECT_EQ(status(B), Target);
  path("x", Base + 512);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + 512, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 0}), 0u);
  llvm::support::endian::write64le(Source.data() + 96, 1);
  llvm::support::endian::write64le(Source.data() + 104, 8);
  for (auto *Bytes : {&Source, &Target}) {
    llvm::support::endian::write64le(Bytes->data() + 48, uint64_t(-7));
    llvm::support::endian::write64le(Bytes->data() + 56, 123456789);
  }
  EXPECT_EQ(status(A), Source);
  EXPECT_EQ(status(B), Target);
  renameFile("/target", "/source");
  identity(A, "/source");
  identity(B, "/target");
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  path("/fresh");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationInode);
}

TEST_P(DarwinFileTest, RenameBalancesDynamicPathsAndRetiredTargetCredit) {
  for (bool Dynamic : {false, true}) {
    for (unsigned Retained = 0; Retained != 4; ++Retained) {
      SCOPED_TRACE(testing::Message() << Dynamic << ":" << Retained);
      Options.emplace();
      Options->Files["/data"] = std::vector<uint8_t>(6);
      Options->WritableFiles.insert("/data");
      Options->MutableDirectories.insert("/");
      if (!Dynamic)
        Options->Files["/target"] =
            std::vector<uint8_t>(darwin_file_limits::Bytes - 35);
      Files = std::make_unique<DarwinFiles>(*Space, Options);
      path("/data");
      const auto A = ok(ServiceKind::Open, {Base, 2});
      path("/target");
      const auto T = ok(ServiceKind::Open, {Base, Dynamic ? 0x202u : 0u});
      if (Dynamic)
        EXPECT_EQ(
            ok(ServiceKind::Ftruncate, {T, darwin_file_limits::Bytes - 35}),
            0u);
      DarwinFiles::MappingSource Lease = uint32_t(0);
      if (Retained == 2)
        Lease = Files->mappingSource(T);
      if (Retained != 1 && Retained != 3)
        EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
      path("/data");
      path("/target", Base + 128);
      if (Retained) {
        // Exactly seven spare bytes cannot admit the eight-byte new path.
        EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
        EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
        identity(A, "/data");
        EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 5}), 0u);
      }
      EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
      identity(A, "/target");
      if (Retained) {
        EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 6}));
        EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
      }
      if (Retained == 1)
        EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
      if (Retained == 3)
        EXPECT_EQ(ok(ServiceKind::Dup2, {A, T}), T);
      Lease = uint32_t(0);
      // Initial paths/grants/references stay charged: 14 or 22 bytes.
      const uint64_t Maximum =
          darwin_file_limits::Bytes - (Dynamic ? 14 : 22) - 8;
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Maximum}), 0u);
      EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Maximum + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
      renameFile("/target", "/x");
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Maximum + 5}), 0u);
      path("/x");
      path("/target", Base + 128);
      EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Maximum}), 0u);
      EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
      EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Maximum + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    }
  }
}

TEST_P(DarwinFileTest, RenameKeepsEntryCountAndNeedsNoFreeDescriptor) {
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  renameFile("/f0", "/new");
  path("/another");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  Options->DescriptorLimit = 3;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  renameFile("/f0", "/new");
  path("/new");
  error(ServiceKind::Open, {Base}, 24);
}

TEST_P(DarwinFileTest, RenamePreservesOriginalPathErrorsAndFlagOrdering) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Directories.insert("/folder");
  Options->WorkingDirectory = "/";
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto Before = status(A);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  path("/missing");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 2);
  error(ServiceKind::Rename, {UINT64_MAX, UINT64_MAX}, 14);
  path("/data/");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 20);
  path("/data");
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 14);
  for (auto [Name, Code] : {std::pair{"/missing/../new", 2u},
                            {"/data/../new", 20u},
                            {"/new/", 2u},
                            {"/new/.", 2u},
                            {"/data/", 20u},
                            {"/folder", 21u},
                            {"/folder/", 21u},
                            {"/folder/.", 22u},
                            {"/folder/.//", 22u},
                            {"/folder/..", 22u},
                            {"/folder/..//", 22u}}) {
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, Code);
  }
  path("/" + std::string(256, 'x'), Base + 128);
  error(ServiceKind::Rename, {Base, Base + 128}, 63);
  path("");
  error(ServiceKind::RenameAt, {999, Base, Parent, UINT64_MAX}, 9);
  error(ServiceKind::RenameAt, {999, UINT64_MAX, Parent, UINT64_MAX}, 14);
  path("data");
  error(ServiceKind::RenameAt, {A, Base, Parent, UINT64_MAX}, 20);
  path("", Base + 128);
  error(ServiceKind::RenameAt, {Parent, Base, 999, Base + 128}, 9);
  error(ServiceKind::RenameAt, {Parent, Base, 999, UINT64_MAX}, 14);
  path("new", Base + 128);
  error(ServiceKind::RenameAt, {Parent, Base, A, Base + 128}, 20);
  for (uint64_t Flags : {8u, 6u, 0x80000000u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          22);
  for (uint64_t Flags : {1u, 0x11u}) {
    EXPECT_FALSE(invoke(ServiceKind::RenameAtX,
                        {999, UINT64_MAX, 999, UINT64_MAX, Flags}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameFlags);
  }
  for (uint64_t Flags : {2u, 0x12u, 4u, 0x14u})
    error(ServiceKind::RenameAtX, {999, UINT64_MAX, 999, UINT64_MAX, Flags},
          14);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(Parent), ParentBefore);
  path("/renamed", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {Parent, Base, 0x12345678000003e7ULL,
                                        Base + 128, 0x1234567800000010ULL}),
            0u);
  identity(A, "/renamed");
  path("/renamed");
  EXPECT_EQ(ok(ServiceKind::RenameAt, {999, Base, 999, Base + 128}), 0u);
}

TEST_P(DarwinFileTest, ExclusiveRenameChecksExistingTargetsBeforeMutation) {
  mutationPolicy();
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/target"] = {'t'};
  auto TargetMetadata = darwin_test::mutationMetadata(1);
  TargetMetadata.Inode++;
  TargetMetadata.Device++;
  Options->Metadata["/target"] = TargetMetadata;
  Options->Directories.insert("/folder");
  Options->Files["/other/target"] = {'o'};
  const auto A = ok(ServiceKind::Open, {Base, 2});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  const auto Before = status(A);
  path("/target");
  const auto B = ok(ServiceKind::Open, {Base});
  const auto TargetBefore = status(B);
  const auto Lease = Files->mappingSource(B);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  const auto RootBefore = status(Root);
  path("/data");
  // Distinct existing targets fail even without a mutable parent, or when a
  // later mount/device decision would otherwise be outside the model.
  for (uint64_t Flags : {4u, 0x14u}) {
    for (const char *Name :
         {"/target", "/folder", "/folder/", "/other/target"}) {
      path(Name, Base + 128);
      error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}, 17);
    }
    for (auto [Name, Code] : {std::pair{"/folder/.", 22u},
                              {"/folder/..", 22u},
                              {"/new/", 2u},
                              {"/target/", 20u},
                              {"/missing/../target", 2u}}) {
      path(Name, Base + 128);
      error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}, Code);
    }
    error(ServiceKind::RenameAtX, {999, Base, 999, UINT64_MAX, Flags}, 14);
  }
  path("/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/other/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(B), TargetBefore);
  EXPECT_EQ(status(Root), RootBefore);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 2u);
  identity(A, "/data");
  identity(B, "/target");
  contents(B, {'t'});
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes.front(), 't');
}

TEST_P(DarwinFileTest, ExclusiveRenameNeverGuessesSameObjectCaseSensitivity) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  for (const char *Name : {"/data", "/./data", "//data"}) {
    path(Name, Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 0x14}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameCaseSensitivity);
    EXPECT_EQ(status(A), Before);
    identity(A, "/data");
    // The original unflagged operation remains an admitted no-op.
    EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
    EXPECT_EQ(status(A), Before);
  }
}

TEST_P(DarwinFileTest, ExclusiveRenameRetainsDescriptionsLeasesAndMetadata) {
  mutationPolicy();
  creationPolicy();
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  const auto Lease = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  auto Before = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  path("/renamed", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {0x12345678000003e7ULL, Base, 999,
                                        Base + 128, 0x1234567800000014ULL}),
            0u);
  error(ServiceKind::Access, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
  for (const auto FD : {A, D}) {
    identity(FD, "/renamed");
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), Flags);
  }
  llvm::support::endian::write64le(Before.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Before.data() + 72, 123456789);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Options->Files.at("/data")));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  contents(D, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_TRUE(Options->Files.contains("/data"));
  EXPECT_FALSE(Options->Files.contains("/renamed"));
}

TEST_P(DarwinFileTest, ExclusiveRenameUsesTheExistingBoundedTransaction) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto Capacity = darwin_file_limits::Bytes - 14;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  path("/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, "/data");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  error(ServiceKind::Access, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 5}), 0u);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}), 0u);
  identity(A, "/new");
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity - 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, CrossParentRenameRetainsDescriptionsAndFileMetadata) {
  mutationPolicy();
  creationPolicy();
  const auto Left = makeDirectory("/left");
  makeDirectory("/right");
  const auto Nested = makeDirectory("/right/nested");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  const auto I = ok(ServiceKind::Open, {Base});
  const auto Lease = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  auto Before = status(A);
  const auto Flags = ok(ServiceKind::Fcntl, {A, 3});
  path("item", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, Left, Base + 128, 0x14}),
            0u);
  error(ServiceKind::Access, {Base}, 2);
  path("item");
  path("moved", Base + 128);
  EXPECT_EQ(
      ok(ServiceKind::RenameAt, {0x1234567800000000ULL | Left, Base,
                                 0x1234567800000000ULL | Nested, Base + 128}),
      0u);
  llvm::support::endian::write64le(Before.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Before.data() + 72, 123456789);
  for (const auto FD : {A, D, I}) {
    identity(FD, "/right/nested/moved");
    EXPECT_EQ(status(FD), Before);
  }
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), Flags);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {I, 0, 1}), 0u);
  path("moved");
  path("/again", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX,
               {Nested, Base, 999, Base + 128, 0x1234567800000014ULL}),
            0u);
  identity(A, "/again");
  contents(I, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Options->Files.at("/data")));
  EXPECT_TRUE(Options->Files.contains("/data"));
  EXPECT_FALSE(Options->Files.contains("/again"));
}

TEST_P(DarwinFileTest, CrossParentReplacementRetainsEachObjectsLastName) {
  mutationPolicy();
  creationPolicy();
  makeDirectory("/left");
  makeDirectory("/right");
  path("/right/target");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("old", Base + Page);
  EXPECT_EQ(ok(ServiceKind::Write, {T, Base + Page, 3}), 3u);
  const auto TargetBefore = status(T);
  const auto TargetLease = Files->mappingSource(T);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(TargetLease));
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto SourceBefore = status(A);
  renameFile("/data", "/right/target");
  auto Removed = TargetBefore;
  llvm::support::endian::write16le(Removed.data() + 6, 0);
  EXPECT_EQ(status(T), Removed);
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            llvm::support::endian::read64le(SourceBefore.data() + 8));
  renameFile("/right/target", "/left/moved");
  identity(T, "/right/target");
  identity(A, "/left/moved");
  contents(T, {'o', 'l', 'd'});
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes,
            llvm::ArrayRef<uint8_t>({'o', 'l', 'd'}));
  path("/right/target");
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, CrossParentRenameDoesNotInferMountsFromDeviceNumbers) {
  Options->Directories.insert("/other");
  Options->MutableDirectories = {"/", "/other"};
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  auto Other = darwin_test::creationParentMetadata();
  Other.Inode++;
  Options->Metadata["/other"] = Other;
  makeDirectory("/created");
  makeDirectory("/other/created");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  for (const char *Name : {"/other/new", "/other/created/new"}) {
    path(Name, Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
    error(ServiceKind::Access, {Base + 128}, 2);
  }
  identity(A, "/data");
  path("/other/created/item");
  const auto B = ok(ServiceKind::Open, {Base, 0x202, 0600});
  renameFile("/other/created/item", "/other/item");
  identity(B, "/other/item");
  path("/other/item");
  path("/created/item", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(B, "/other/item");
}

TEST_P(DarwinFileTest, CrossParentRenameUsesObjectsAfterInitialNameReuse) {
  Options->Directories.insert("/same");
  Options->MutableDirectories = {"/", "/same"};
  Options->RemovableDirectories.insert("/same");
  path("/same");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto OldChild = makeDirectory("/same/child");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/same/child/item", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  path("/same/child");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("/same");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  makeDirectory("/same");
  const auto NewChild = makeDirectory("/same/child");
  path("/data");
  path("item", Base + 128);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, NewChild, Base + 128, 4}),
            0u);
  identity(A, "/same/child/item");
  path("item");
  error(ServiceKind::OpenAt, {OldChild, Base}, 2);
  EXPECT_NE(ok(ServiceKind::OpenAt, {NewChild, Base}), UINT64_MAX);
  path("child/item");
  error(ServiceKind::OpenAt, {Old, Base}, 2);
  identity(OldChild, "/same/child");
}

TEST_P(DarwinFileTest, CrossParentRenameKeepsTheSourcesWriteAuthority) {
  Options->MutableDirectories.insert("/");
  makeDirectory("/new");
  path("/new/target");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  renameFile("/data", "/new/target");
  path("/new/target");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  error(ServiceKind::Ftruncate, {A, 0}, 22);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {T, 1}), 0u);
  contents(T, {0});
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
}

TEST_P(DarwinFileTest, CrossParentRenamePreservesLookupAndExclusiveOrder) {
  mutationPolicy();
  creationPolicy();
  const auto New = makeDirectory("/new");
  makeDirectory("/new/sub");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  for (auto [Name, Code] : {std::pair{"/new/.", 22u},
                            {"/new/sub/..", 22u},
                            {"/new/missing/../x", 2u},
                            {"/new/x/", 2u}}) {
    path(Name, Base + 128);
    for (uint64_t Flags : {0u, 0x14u})
      error(ServiceKind::RenameAtX, {999, Base, New, Base + 128, Flags}, Code);
  }
  for (const char *Name : {"/new", "/new/"}) {
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, 21);
    error(ServiceKind::RenameAtX, {999, Base, New, Base + 128, 4}, 17);
  }
  error(ServiceKind::RenameAt, {999, Base, New, UINT64_MAX}, 14);
  path("missing");
  error(ServiceKind::RenameAt, {New, Base, New, UINT64_MAX}, 2);
  path("/new");
  // Created directory sources now reach target lookup before mutation.
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 14);
  EXPECT_EQ(status(A), Before);
  identity(A, "/data");
}

TEST_P(DarwinFileTest, CrossParentRenameRejectsContradictoryOwnedDevices) {
  mutationPolicy();
  creationPolicy();
  Options->Metadata["/data"].Device++;
  makeDirectory("/new");
  path("/new/target");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  const auto TargetBefore = status(T);
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto Before = status(A);
  path("/new/moved", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  error(ServiceKind::Access, {Base + 128}, 2);
  path("/new/target", Base + 128);
  error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}, 17);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(status(T), TargetBefore);
  identity(A, "/data");
  identity(T, "/new/target");
}

TEST_P(DarwinFileTest, CrossParentRenameChargesTheCompleteNewPath) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  makeDirectory("/n");
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  // Initial path/grants reserve 14 bytes; the created directory reserves 3.
  const auto Capacity = darwin_file_limits::Bytes - 17;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  path("/n/x", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, "/data");
  EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
  error(ServiceKind::Access, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 5}), 0u);
  EXPECT_EQ(ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 4}), 0u);
  identity(A, "/n/x");
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity - 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, CrossParentReplacementCreditsOnlyReleasedMappingLeases) {
  Options->MutableDirectories.insert("/");
  Options->WritableFiles.insert("/data");
  makeDirectory("/n");
  path("/n/x");
  const auto T = ok(ServiceKind::Open, {Base, 0x202, 0600});
  path("old", Base + Page);
  EXPECT_EQ(ok(ServiceKind::Write, {T, Base + Page, 3}), 3u);
  auto Lease = Files->mappingSource(T);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  // 17 fixed bytes, plus the target's five-byte path and three-byte contents.
  const auto Capacity = darwin_file_limits::Bytes - 25;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  path("/n/x", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, "/data");
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>({'o', 'l', 'd'}));
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  identity(A, "/n/x");
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity + 3}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity + 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, RenameDirectoryAndUnknownMountMovesRemainExplicit) {
  Options->MutableDirectories = {"/", "/folder"};
  Options->Directories.insert("/folder");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  auto Other = darwin_test::creationParentMetadata();
  Other.Inode++;
  Options->Metadata["/folder"] = Other;
  const auto A = ok(ServiceKind::Open, {Base});
  path("/folder/new", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(A, "/data");
  path("/folder");
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
  path("/new/", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameKind);
}

TEST_P(DarwinFileTest, RenameNestedDotTargetsFailBeforeNamespaceAdmission) {
  Options->Files["/dir/data"] = {'x'};
  Options->Directories = {"/dir/sub", "/other"};
  // These lookup failures precede both namespace grants and mount checks.
  path("/dir/data");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/dir");
  const auto D = ok(ServiceKind::Open, {Base});
  path("/dir/data");
  for (auto [Name, Code] : {std::pair{"/dir/.", 22u},
                            {"/dir/sub/..", 22u},
                            {"/dir/..", 22u},
                            {"/other/.", 22u},
                            {"/other/..//", 22u},
                            {"/dir/missing/.", 2u},
                            {"/dir/data/..", 20u}}) {
    path(Name, Base + 128);
    error(ServiceKind::Rename, {Base, Base + 128}, Code);
  }
  path("data");
  for (const char *Name : {".", "sub/..", "..//"}) {
    path(Name, Base + 128);
    error(ServiceKind::RenameAt, {D, Base, D, Base + 128}, 22);
  }
  identity(A, "/dir/data");
  contents(A, {'x'});
  path("/dir/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), D + 1);
}

TEST_P(DarwinFileTest, RenameChecksOwnedDeviceAfterAnOldNameIsReused) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Metadata["/"] = darwin_test::creationParentMetadata();
  Options->Files["/old"] = {};
  auto Other = darwin_test::mutationMetadata(0);
  Other.Inode++;
  Other.Device = 456;
  Options->Metadata["/old"] = Other;
  const auto A = ok(ServiceKind::Open, {Base, 2});
  path("/old", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameMount);
  identity(A, "/data");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base + 128}), 0u);
  renameFile("/data", "/old");
  // Whole-EFAULT loses the full stat record, but cannot change its device.
  error(ServiceKind::Write, {A, UINT64_MAX, 1}, 14);
  renameFile("/old", "/new");
  identity(A, "/new");
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {A, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
  EXPECT_EQ(Options->Metadata.at("/old").Device, 456);
}

TEST_P(DarwinFileTest, RenameNeverInheritsTargetWriteAuthority) {
  Options->MutableDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  Options->WritableFiles.insert("/target");
  const auto A = ok(ServiceKind::Open, {Base});
  path("/target");
  const auto T = ok(ServiceKind::Open, {Base, 2});
  renameFile("/data", "/target");
  path("/target");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  error(ServiceKind::Ftruncate, {A, 0}, 22);
  EXPECT_FALSE(invoke(ServiceKind::Truncate, {Base, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {T, 0}), 0u);
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
}

TEST_P(DarwinFileTest, RenameCanonicalPathLimitPreservesRelativeSource) {
  std::string Parent;
  for (unsigned I = 0; I != 4; ++I)
    Parent += '/' + std::string(250, 'a');
  Parent += '/' + std::string(10, 'b');
  Options.emplace();
  Options->Files[Parent + "/s"] = {'x'};
  Options->MutableDirectories.insert(Parent);
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path(Parent);
  const auto P = ok(ServiceKind::Open, {Base});
  path("s");
  const auto A = ok(ServiceKind::OpenAt, {P, Base});
  path("0123456789", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::RenameAt, {P, Base, P, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::RenameLimit);
  identity(A, Parent + "/s");
  EXPECT_EQ(ok(ServiceKind::OpenAt, {P, Base}), A + 1);
}

TEST_P(DarwinFileTest, UmaskKeepsAllPermissionBitsWithoutCreationAuthority) {
  EXPECT_FALSE(invoke(ServiceKind::Umask, {0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileUmask);
  Options->InitialUmask = 0027;
  EXPECT_EQ(ok(ServiceKind::Umask, {0x12345678000001edULL}), 0027u);
  EXPECT_EQ(ok(ServiceKind::Umask, {07000}), 0755u);
  const auto A = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Umask, {UINT64_MAX}), 07000u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Umask, {0}), 07777u);
  EXPECT_EQ(ok(ServiceKind::Umask, {0022}), 0u);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
  EXPECT_FALSE(Options->CreationPolicy);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
}

TEST_P(DarwinFileTest, CreationMetadataOwnsIdentityAndUsesCurrentUmask) {
  creationPolicy();
  EXPECT_EQ(ok(ServiceKind::Umask, {07027}), 0027u);
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  path("new");
  const auto A =
      ok(ServiceKind::OpenAt, {Parent, Base, 0xe02, 0x1234567800000fffULL});
  auto Bytes = status(A);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data()), uint32_t(-123));
  EXPECT_EQ(llvm::support::endian::read16le(Bytes.data() + 4), 0100750u);
  EXPECT_EQ(llvm::support::endian::read16le(Bytes.data() + 6), 1u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8),
            0xfedcba9876543211ULL);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 16), 1000u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 20), 0xfedcba98u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 96), 0u);
  EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 104), 0u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 112), 8192u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 116), 0u);
  EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 120), 0x89abcdefu);
  for (unsigned Offset : {32u, 48u, 64u, 80u}) {
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + Offset),
              uint64_t(-19));
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + Offset + 8),
              987654321u);
  }
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 2u);
  error(ServiceKind::Lseek, {A, 0, 4}, 6);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Umask, {07777}), 07027u);
  path("second");
  const auto B = ok(ServiceKind::OpenAt, {Parent, Base, 0x202, 07777});
  const auto Second = status(B);
  EXPECT_EQ(llvm::support::endian::read16le(Second.data() + 4), 0100000u);
  EXPECT_EQ(llvm::support::endian::read64le(Second.data() + 8),
            0xfedcba9876543212ULL);
  EXPECT_EQ(llvm::support::endian::read32le(Second.data() + 20), 0xfedcba98u);
  EXPECT_EQ(status(A), Bytes);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(Options->CreationPolicy->FirstInode, 0xfedcba9876543211ULL);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest, CreationPolicySharesSparseMutationAndUnlinkState) {
  creationPolicy();
  path("/new");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0666});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  const auto Initial = status(A);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 8193}), 0u);
  auto Expected = Initial;
  llvm::support::endian::write64le(Expected.data() + 96, 8193);
  for (unsigned Offset : {48u, 64u}) {
    llvm::support::endian::write64le(Expected.data() + Offset, uint64_t(-7));
    llvm::support::endian::write64le(Expected.data() + Offset + 8, 123456789);
  }
  EXPECT_EQ(status(B), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 3}), 0u);
  error(ServiceKind::Lseek, {B, 0, 4}, 6);
  path("x", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {D, Base + 128, 1, 4096}), 1u);
  llvm::support::endian::write64le(Expected.data() + 104, 8);
  EXPECT_EQ(status(B), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 4}), 4096u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4096, 3}), 8192u);
  auto Lease = Files->mappingSource(A);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  EXPECT_EQ(status(D), Expected);
  const auto Fresh = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(Fresh).data() + 8),
            0xfedcba9876543212ULL);
  EXPECT_EQ(ok(ServiceKind::Write, {Fresh, Base + 128, 1}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  Lease = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 0}), 0u);
  llvm::support::endian::write64le(Expected.data() + 96, 0);
  llvm::support::endian::write64le(Expected.data() + 104, 0);
  EXPECT_EQ(status(B), Expected);
  error(ServiceKind::Write, {Fresh, UINT64_MAX, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {Fresh, Base + 128, 1, 0}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Fresh, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Fresh, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
}

TEST_P(DarwinFileTest, CreationInodesAreGlobalAndParentFieldsStayIndependent) {
  creationPolicy();
  Options->Directories.insert("/parent");
  Options->MutableDirectories.insert("/parent");
  auto Other = darwin_test::creationParentMetadata();
  Other.Device = 456;
  Other.GID = 123;
  Other.Inode = 42;
  Options->Metadata["/parent"] = Other;
  path("/");
  const auto Root = ok(ServiceKind::Open, {Base});
  path("/parent");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto ParentBefore = status(Parent);
  const char *Names[] = {"/one", "/parent/two", "/three"};
  for (unsigned I = 0; I != 3; ++I) {
    path(Names[I]);
    const auto A = ok(ServiceKind::Open, {Base, 0x202, 0600});
    const auto Bytes = status(A);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8),
              darwin_test::CreationPolicy.FirstInode + I);
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data()),
              I == 1 ? 456u : uint32_t(-123));
    EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 20),
              I == 1 ? 123u : 0xfedcba98u);
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Root, UINT64_MAX}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    if (!I)
      EXPECT_EQ(status(Parent), ParentBefore);
    else {
      EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, UINT64_MAX}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    }
    EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  }
}

TEST_P(DarwinFileTest, CreationUnlinkBeforeFirstWriteKeepsOwnedMetadata) {
  creationPolicy();
  path("/new");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0755});
  auto Expected = status(A);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  llvm::support::endian::write64le(Expected.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 72, 123456789);
  EXPECT_EQ(status(A), Expected);
  path("x", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 1, 4096}), 1u);
  llvm::support::endian::write64le(Expected.data() + 48, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 56, 123456789);
  llvm::support::endian::write64le(Expected.data() + 96, 4097);
  llvm::support::endian::write64le(Expected.data() + 104, 8);
  EXPECT_EQ(status(A), Expected);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 8193}), 0u);
  llvm::support::endian::write64le(Expected.data() + 96, 8193);
  EXPECT_EQ(status(A), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 4}), 4096u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 4096, 3}), 8192u);
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, CreationBudgetRefusalsPreserveInodeAndParentMetadata) {
  creationPolicy();
  Options->WritableFiles.insert("/data");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto Before = status(Parent);
  // Data path/reference and parent metadata/grant cost 16 bytes.
  const auto Capacity = darwin_file_limits::Bytes - 16;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 4}), 0u);
  path("new");
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {Parent, Base, 0xa02, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  EXPECT_EQ(status(Parent), Before);
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - 5}), 0u);
  const auto A = ok(ServiceKind::OpenAt, {Parent, Base, 0xa02, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest, CreationEntryRefusalsDoNotConsumeInodeSequence) {
  creationPolicy();
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  path("/f0");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
}

TEST_P(DarwinFileTest, CreationInodesNeverRecycleAfterExhaustionOrFailedOpens) {
  creationPolicy(UINT64_MAX);
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {Base, 0xe02, 0600}, 17);
  const auto Original = ok(ServiceKind::Open, {Base});
  path("/new");
  error(ServiceKind::Open, {Base, 0x202, 0600}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {Original}), 0u);
  error(ServiceKind::Open, {UINT64_MAX, 0x202}, 14);
  const auto A = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(A).data() + 8), UINT64_MAX);
  EXPECT_EQ(ok(ServiceKind::Umask, {0}), 0027u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0x200, 0777});
  EXPECT_EQ(llvm::support::endian::read64le(status(B).data() + 8), UINT64_MAX);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  EXPECT_EQ(ok(ServiceKind::Umask, {0022}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationInode);
  error(ServiceKind::Open, {Base}, 2);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x200}), Original);
  EXPECT_EQ(Options->CreationPolicy->FirstInode, UINT64_MAX);
}

TEST_P(DarwinFileTest, CreateDistinguishesNewTruncateAndDescriptionFlags) {
  Options->MutableDirectories.insert("/");
  for (uint32_t Access : {0u, 1u, 2u}) {
    path("/new" + std::to_string(Access));
    const auto A = ok(ServiceKind::Open, {Base, 0x1000608u | Access, 0666});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), Access | 8u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 2}), 0u);
    const auto B = ok(ServiceKind::Open, {Base, 2});
    const auto D = ok(ServiceKind::Dup, {B});
    path("xyz", Base + 128);
    EXPECT_EQ(ok(ServiceKind::Write, {B, Base + 128, 3}), 3u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 3u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
    contents(B, {'x', 'y', 'z'});
    if (!Access)
      error(ServiceKind::Write, {A, Base + 128, 1}, 9);
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, UINT64_MAX}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {B, 0, 4}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
    const auto T = ok(ServiceKind::Open, {Base, 0x600});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {T, 3}), 0x10000u);
    contents(T, {});
    for (auto FD : {A, B, D, T})
      EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  }
  EXPECT_EQ(Options->Files.size(), 1u);
  EXPECT_TRUE(Options->WritableFiles.empty());
}

TEST_P(DarwinFileTest, CreateExistingNamesPreservesAuthorityAndExclusiveOrder) {
  Options->Directories.insert("/empty");
  error(ServiceKind::Open, {Base, 0xe02}, 17);
  const auto A = ok(ServiceKind::Open, {Base, 0x200});
  auto Source = Files->mappingSource(A);
  error(ServiceKind::Open, {Base, 0xe02}, 17);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x602}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  contents(A, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0x800});
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  path("/absent");
  error(ServiceKind::Open, {Base, 0x800}, 2);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/empty/.");
  error(ServiceKind::Open, {Base, 0xa02}, 17);
  error(ServiceKind::Open, {Base, 0x202}, 21);
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x200}), B);
}

TEST_P(DarwinFileTest,
       CreateWalksOriginalComponentsAndReservesDescriptorsFirst) {
  Options->MutableDirectories.insert("/");
  Options->WorkingDirectory = "/";
  Options->DescriptorLimit = 5;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  error(ServiceKind::Open, {UINT64_MAX, 0x100200}, 22);
  error(ServiceKind::OpenAt, {999, UINT64_MAX, 0x200}, 14);
  path("");
  error(ServiceKind::OpenAt, {999, Base, 0x200}, 9);
  error(ServiceKind::Open, {Base, 0x200}, 2);
  for (const char *Name : {"/missing/", "/missing//", "/missing/.",
                           "/missing/..", "/missing/../new"}) {
    path(Name);
    error(ServiceKind::Open, {Base, 0xa02}, 2);
  }
  path("/data/");
  error(ServiceKind::Open, {Base, 0xa02}, 20);
  path("/");
  const auto Directory = ok(ServiceKind::Open, {Base});
  path("/data");
  const auto A = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Open, {UINT64_MAX, 0x203}, 22);
  error(ServiceKind::Open, {UINT64_MAX, 0x100200}, 24);
  path("/new");
  error(ServiceKind::Open, {Base, 0x202}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {Directory}), 0u);
  error(ServiceKind::Open, {Base}, 2);
  path("new");
  error(ServiceKind::OpenAt, {A, Base, 0x200}, 20);
  path("/./new");
  const auto B = ok(ServiceKind::OpenAt, {999, Base, 0xa02});
  EXPECT_EQ(B, Directory);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  path("/new");
  const auto C = ok(ServiceKind::Open, {Base, 0x800});
  contents(C, {});
  EXPECT_EQ(ok(ServiceKind::Close, {C}), 0u);
  path("relative");
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x200}), Directory);
}

TEST_P(DarwinFileTest, CreateInvalidatesParentOnlyAfterInsertion) {
  Options->WritableFiles.insert("/data");
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  auto Root = darwin_test::mutationMetadata(64);
  Root.Inode = 41;
  Root.Mode = 0040755;
  Options->Metadata["/"] = Root;
  Options->MutableDirectories.insert("/");
  path("/");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto Before = status(Parent);
  path("/data");
  error(ServiceKind::Open, {Base, 0xe02}, 17);
  const auto Existing = ok(ServiceKind::Open, {Base, 0x200});
  EXPECT_EQ(ok(ServiceKind::Close, {Existing}), 0u);
  path("/empty/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  error(ServiceKind::Open, {Base}, 2);
  EXPECT_EQ(status(Parent), Before);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Parent, Base + Page, 64, Base + 512}),
      64u);
  const auto Cursor = ok(ServiceKind::Lseek, {Parent, 0, 1});
  path("/data", Base + 128);
  const auto Data = ok(ServiceKind::Open, {Base + 128, 2});
  // Paths/references cost 23 bytes, and the four LP64 records cost 128.
  EXPECT_EQ(
      ok(ServiceKind::Ftruncate, {Data, darwin_file_limits::Bytes - 151 - 4}),
      0u);
  path("new");
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {Parent, Base, 0xa00}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  error(ServiceKind::OpenAt, {Parent, Base}, 2);
  EXPECT_EQ(status(Parent), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, 0, 1}), Cursor);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Parent, Base + Page, 64, Base + 512}),
      64u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, Cursor, 0}), Cursor);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
  EXPECT_EQ(ok(ServiceKind::OpenAt, {Parent, Base, 0xa00}), Existing);
  std::array<uint8_t, 160> Canary;
  Canary.fill(0x5a);
  llvm::cantFail(Space->write(Base + Page, Canary));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Parent, Base + Page, 64, Base + Page + 144}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, 0, 1}), Cursor);
  std::array<uint8_t, 160> After;
  llvm::cantFail(Space->read(Base + Page, After));
  EXPECT_EQ(After, Canary);
}

TEST_P(DarwinFileTest, CreateReusesNamesWithoutInheritingObjectsOrPolicies) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  auto Old = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Old));
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0xe02});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  path("new", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {B, Base + 128, 3}), 3u);
  contents(B, {'n', 'e', 'w'});
  contents(D, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(llvm::support::endian::read16le(status(D).data() + 6), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 4}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {B, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  auto Fresh = Files->mappingSource(B);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Fresh));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Old).Bytes.front(), 'a');
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Fresh).Bytes.front(), 'n');
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {D}), 0u);
  Old = uint32_t(0);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  Fresh = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, 0}), 0u);
  EXPECT_EQ(Options->Metadata.at("/data").LinkCount, 1u);
  EXPECT_EQ(Options->Files.at("/data").size(), 6u);
}

TEST_P(DarwinFileTest, CreateChargesAndReclaimsDynamicPathsWithObjectLifetime) {
  Options->WritableFiles.insert("/data");
  Options->MutableDirectories.insert("/");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  // Initial path, writable reference and mutable root remain charged.
  const uint64_t Capacity = darwin_file_limits::Bytes - 6 - 6 - 2;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 4}), 0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  error(ServiceKind::Open, {Base}, 2);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity - 5}), 0u);
  const auto B = ok(ServiceKind::Open, {Base, 0x202});
  const auto D = ok(ServiceKind::Dup, {B});
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  auto Source = Files->mappingSource(B);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, D}), D);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  Source = uint32_t(0);
  const auto C = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(C, B);
  EXPECT_EQ(ok(ServiceKind::Close, {C}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, Capacity}));
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
}

TEST_P(DarwinFileTest, CreateCountsNamedAndOrphanedObjectsAndReclaimsEntries) {
  Options->Files.clear();
  for (unsigned I = 0; I != 255; ++I)
    Options->Files["/f" + std::to_string(I)] = {};
  Options->MutableDirectories.insert("/");
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  path("/f0");
  const auto A = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x200}));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  for (unsigned I = 0; I != 300; ++I) {
    SCOPED_TRACE(I);
    const auto B = ok(ServiceKind::Open, {Base, 0xa00});
    EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
    path("/extra", Base + 128);
    EXPECT_FALSE(invoke(ServiceKind::Open, {Base + 128, 0x200}));
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  }
}

TEST_P(DarwinFileTest, CreateChecksCanonicalBudgetAfterRelativeResolution) {
  std::string Parent;
  for (char C : {'a', 'b', 'c', 'd'})
    Parent += '/' + std::string(240, C);
  Options->Directories.insert(Parent);
  Options->MutableDirectories.insert(Parent);
  Options->WorkingDirectory = Parent;
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path(std::string(59, 'x'));
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  error(ServiceKind::Open, {Base}, 2);
  path(std::string(58, 'y'));
  const auto A = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 50, Base + Page}), 0u);
  std::vector<uint8_t> Name(1024);
  llvm::cantFail(Space->read(Base + Page, Name));
  EXPECT_EQ(std::string(reinterpret_cast<char *>(Name.data())),
            Parent + '/' + std::string(58, 'y'));
}

TEST_P(DarwinFileTest, UnlinkRemovesNamesAndPreservesOpenObjectsAndPaths) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto Original = Options->Files.at("/data");
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 2, 0}), 2u);
  const auto Before = status(A);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  error(ServiceKind::Open, {Base}, 2);
  error(ServiceKind::Unlink, {Base}, 2);
  error(ServiceKind::Stat64, {Base, UINT64_MAX}, 2);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 2u);
  contents(B, Original);
  auto Expected = Before;
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  llvm::support::endian::write64le(Expected.data() + 64, uint64_t(-7));
  llvm::support::endian::write64le(Expected.data() + 72, 123456789);
  EXPECT_EQ(status(D), Expected);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 4}), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 1, 3}), 6u);
  path("Z", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {D, Base + 128, 1, 0}), 1u);
  EXPECT_EQ(llvm::support::endian::read16le(status(B).data() + 6), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 3}), 0u);
  EXPECT_EQ(llvm::support::endian::read16le(status(B).data() + 6), 0u);
  contents(B, {'Z', 'b', 0});
  std::array<uint8_t, 7> Name;
  Name.fill(0x5a);
  llvm::cantFail(Space->write(Base + 256, Name));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 50, Base + 256}), 0u);
  llvm::cantFail(Space->read(Base + 256, Name));
  EXPECT_EQ(Name, (std::array<uint8_t, 7>{'/', 'd', 'a', 't', 'a', 0, 0x5a}));
  error(ServiceKind::Fcntl, {D, 50, UINT64_MAX}, 14);
  for (auto FD : {A, B, D})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Open, {Base}, 2);
  EXPECT_EQ(Options->Files.at("/data"), Original);
  EXPECT_EQ(Options->Metadata.at("/data").LinkCount, 1u);
}

TEST_P(DarwinFileTest,
       UnlinkReadonlyFilesKeepsImplicitParentsAndRelativePaths) {
  Options->Files["/tree/child"] = {'x'};
  Options->MutableDirectories.insert("/tree");
  Options->WorkingDirectory = "/tree";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/tree");
  const auto Directory = ok(ServiceKind::Open, {Base});
  path("child", Base + 128);
  const auto A = ok(ServiceKind::OpenAt, {Directory, Base + 128});
  error(ServiceKind::UnlinkAt, {999, UINT64_MAX, 0x80000000}, 22);
  error(ServiceKind::UnlinkAt, {999, UINT64_MAX, 0}, 14);
  path("", Base + 256);
  error(ServiceKind::UnlinkAt, {999, Base + 256, 0}, 9);
  error(ServiceKind::UnlinkAt, {A, Base + 128, 0}, 20);
  EXPECT_FALSE(invoke(ServiceKind::UnlinkAt, {999, UINT64_MAX, 0x100}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::UnlinkFlags);
  path("/tree/child/", Base + 256);
  error(ServiceKind::Unlink, {Base + 256}, 20);
  EXPECT_EQ(
      ok(ServiceKind::UnlinkAt, {Directory, Base + 128, 0x1234567800000800ULL}),
      0u);
  contents(A, {'x'});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {A, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  error(ServiceKind::Open, {Base + 128}, 2);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
  path(".", Base + 128);
  const auto Parent = ok(ServiceKind::Open, {Base + 128});
  error(ServiceKind::Read, {Parent, 0, 0}, 21);
  error(ServiceKind::Unlink, {Base}, 1);
  path("/");
  error(ServiceKind::Unlink, {Base}, 21);
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  const auto Other = ok(ServiceKind::Open, {Base});
  contents(Other, {'a', 'b', 0, 0xff, 'e', 'f'});
}

TEST_P(DarwinFileTest, UnlinkInvalidatesOnlyItsParentObservations) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  auto Root = darwin_test::mutationMetadata(64);
  Root.Inode = 41;
  Root.Mode = 0040755;
  Options->Metadata["/"] = Root;
  auto Empty = Root;
  Empty.Inode = 42;
  Options->Metadata["/empty"] = Empty;
  Options->MutableDirectories.insert("/");
  path("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  path("/empty", Base + 128);
  const auto B = ok(ServiceKind::Open, {Base + 128});
  const auto EmptyStatus = status(B);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {A, Base + Page, 64, Base + 512}),
            64u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {D, Base + Page, 64, Base + 512}),
            64u);
  const auto Offset = ok(ServiceKind::Lseek, {A, 0, 1});
  error(ServiceKind::Unlink, {UINT64_MAX}, 14);
  EXPECT_EQ(ok(ServiceKind::Fstat64, {D, Base + 256}), 0u);
  path("/data", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base + 128}), 0u);
  const auto Fresh = ok(ServiceKind::Open, {Base});
  llvm::cantFail(Space->writeInteger(Base + 256, 0x7777, 2));
  llvm::cantFail(Space->writeInteger(Base + 512, 0x8888, 2));
  llvm::cantFail(Space->writeInteger(Base + 256 + 1020, 0x9999, 2));
  for (auto FD : {A, D, Fresh}) {
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + 256}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    EXPECT_FALSE(
        invoke(ServiceKind::GetDirEntries64, {FD, Base + 256, 64, UINT64_MAX}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                        {FD, Base + 256, 1024, Base + 512}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {FD, 0, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  }
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0x7777u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 512, 2)), 0x8888u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 1020, 2)), 0x9999u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), Offset);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {A}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(status(B), EmptyStatus);
}

TEST_P(DarwinFileTest, UnlinkBudgetWaitsForEveryDescriptionAndMappingLease) {
  Options->Files["/other"] = {};
  Options->WritableFiles = {"/data", "/other"};
  Options->MutableDirectories.insert("/");
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 2;
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto D = ok(ServiceKind::Dup, {A});
  path("/other", Base + 128);
  const auto B = ok(ServiceKind::Open, {Base + 128, 2});
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  auto Source = Files->mappingSource(A);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Source));
  const auto Bytes = std::get<DarwinFiles::Mapping>(Source).Bytes;
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {A, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Dup2, {B, D}), D);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(Bytes.front(), 'a');
  EXPECT_EQ(Bytes.back(), 0u);
  Source = uint32_t(0);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 2}), Capacity);
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, UnlinkFirstInitializesBudgetAndReclaimsCurrentSize) {
  Options->Files["/other"] = {};
  Options->WritableFiles = {"/data", "/other"};
  Options->MutableDirectories.insert("/");
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 2;
  // No prior open or mutation: the next growth must reclaim initial bytes.
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/other");
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 7}), 0u);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 11}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  error(ServiceKind::Open, {Base}, 2);
}

TEST(DarwinFileOptions, NamespaceAdmissionRejectsFlagsAndAllKnownAliases) {
  DarwinFileOptions Good;
  Good.Files["/tree/a"] = {};
  Good.MutableDirectories.insert("/tree");
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (auto Path : {"/missing", "/tree/a", "/tree/../tree"}) {
    auto O = Good;
    O.MutableDirectories = {Path};
    auto Error = validateFileOptions(O);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  auto M = darwin_test::mutationMetadata(0);
  Good.Metadata["/tree/a"] = M;
  M.Mode = 0040755;
  M.Inode = 43;
  Good.Metadata["/tree"] = M;
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (auto Path : {"/tree", "/tree/a"}) {
    for (auto Flags : {1u, 2u, 4u, 0x20000u, 0x40000u, 0x100000u}) {
      auto O = Good;
      O.Metadata[Path].Flags = Flags;
      EXPECT_EQ(llvm::toString(validateFileOptions(O)),
                diagnostic::NamespaceFlags);
    }
  }
  for (auto Bits : {01000, 02000, 04000}) {
    auto O = Good;
    O.Metadata["/tree"].Mode |= Bits;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceFlags);
  }
  for (auto Links : {0, 2}) {
    auto O = Good;
    O.Metadata["/tree/a"].LinkCount = Links;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceAlias);
  }
  for (auto Path : {"/tree", "/tree/a"}) {
    auto O = Good;
    if (std::string(Path) == "/tree")
      O.Directories.insert("/alias");
    else
      O.Files["/alias"] = {};
    O.Metadata["/alias"] = O.Metadata[Path];
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::NamespaceAlias);
    O.Metadata["/alias"].Device++;
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  DarwinFileOptions Snapshot;
  Snapshot.Files["/data"] = {};
  Snapshot.Directories.insert("/empty");
  Snapshot.MutableDirectories.insert("/");
  Snapshot.DirectoryContents["/"] = darwin_test::directoryContents();
  EXPECT_FALSE(bool(validateFileOptions(Snapshot)));
  Snapshot.DirectoryContents["/"].Entries[2].Inode = 41;
  EXPECT_EQ(llvm::toString(validateFileOptions(Snapshot)),
            diagnostic::NamespaceAlias);
}

TEST_P(DarwinFileTest, UnlinkDoesNotRestoreUnknownMetadataOrReuseAbsentNames) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  error(ServiceKind::Write, {FD, UINT64_MAX, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::UnlinkAt, {999, Base, 0}), 0u);
  path("X", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {FD, Base + 128, 1}), 1u);
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, UINT64_MAX}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {FD, 0, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  path("/");
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(Directory, FD);
  path("data");
  error(ServiceKind::OpenAt, {Directory, Base}, 2);
  path("/data");
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, WritableNodesSurviveSeparateOpensDupAndLastClose) {
  Options->WritableFiles.insert("/data");
  const auto Original = Options->Files.at("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto B = ok(ServiceKind::Open, {Base});
  auto D = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 0}), 1u);
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {D, Base + 128, 2}), 2u);
  contents(B, {'a', 'X', 'Y', 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 2, 8}), 2u);
  contents(B, {'a', 'X', 'Y', 0xff, 'e', 'f', 0, 0, 'X', 'Y'});
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 3u);
  for (auto FD : {A, B, D})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  auto Reopened = ok(ServiceKind::Open, {Base});
  contents(Reopened, {'a', 'X', 'Y', 0xff, 'e', 'f', 0, 0, 'X', 'Y'});
  EXPECT_EQ(Options->Files.at("/data"), Original);
}

TEST_P(DarwinFileTest, AppendFlagsAreSharedByDupAndIgnoredByPwrite) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 0x100000a});
  auto D = ok(ServiceKind::Dup, {A});
  auto B = ok(ServiceKind::Open, {Base, 2});
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 4, 0x1000a}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 2, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Write, {D, Base + 128, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 8u);
  contents(B, {'a', 'X', 'Y', 0xff, 'e', 'f', 'X', 'Y'});
  const auto WrittenFlags = ok(ServiceKind::Fcntl, {D, 3});
  EXPECT_EQ(WrittenFlags, 0x1000au);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 4, WrittenFlags & ~8u}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 4, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 10u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {A, 4, 4}));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
}

TEST_P(DarwinFileTest, TruncationChangesSharedBytesWithoutMovingCursors) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto B = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 5, 0}), 5u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 3}), 0u);
  contents(B, {'a', 'b', 0});
  EXPECT_EQ(ok(ServiceKind::Truncate, {Base, 8}), 0u);
  contents(B, {'a', 'b', 0, 0, 0, 0, 0, 0});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 5u);
  auto ReadOnlyTruncate = ok(ServiceKind::Open, {Base, 0x400});
  contents(B, {});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 5u);
  error(ServiceKind::Write, {ReadOnlyTruncate, 0, 0}, 9);
  error(ServiceKind::Ftruncate, {B, 0}, 22);
  error(ServiceKind::Ftruncate, {999, UINT64_MAX}, 22);
  error(ServiceKind::Truncate, {0, UINT64_MAX}, 22);
  path("/");
  error(ServiceKind::Truncate, {Base, 0}, 21);
  error(ServiceKind::Open, {Base, 1}, 21);
  error(ServiceKind::Open, {Base, 3}, 22);
}

TEST_P(DarwinFileTest, TruncationRecordsOnlyTheParticipatingOpenDescription) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto D = ok(ServiceKind::Dup, {A});
  auto B = ok(ServiceKind::Open, {Base, 2});
  error(ServiceKind::Ftruncate, {D, UINT64_MAX}, 22);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 2u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 6}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {D, 3}), 0x10002u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_EQ(ok(ServiceKind::Truncate, {Base, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, darwin_file_limits::Bytes}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
  for (unsigned Access : {0u, 1u, 2u}) {
    auto T = ok(ServiceKind::Open, {Base, 0x400 | Access});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {T, 3}), 0x10000u | Access);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 3}), 2u);
    EXPECT_EQ(ok(ServiceKind::Close, {T}), 0u);
  }
}

TEST_P(DarwinFileTest, SparseSeekUsesKnownUnitsAndSharesOnlyTheOpenCursor) {
  for (uint32_t Unit : {512u, 4096u, 16384u}) {
    SCOPED_TRACE(Unit);
    mutationPolicy(Unit);
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto A = ok(ServiceKind::Open, {Base, 2});
    const auto D = ok(ServiceKind::Dup, {A});
    const auto B = ok(ServiceKind::Open, {Base});
    const auto Initial = status(A);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 4}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 2, 0x1234567800000003ULL}), 6u);
    EXPECT_EQ(status(A), Initial);
    for (uint64_t Whence : {3u, 4u}) {
      error(ServiceKind::Lseek, {A, UINT64_MAX, Whence}, 22);
      error(ServiceKind::Lseek, {A, 6, Whence}, 6);
      error(ServiceKind::Lseek, {A, INT64_MAX, Whence}, 6);
      EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 6u);
    }
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 0}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Unit * 5 + 3}), 0u);
    error(ServiceKind::Lseek, {A, 0, 4}, 6);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), 6u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 1, 3}), 1u);
    llvm::cantFail(Space->writeInteger(Base + 128, 0, 2));
    EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 2, Unit * 2 - 1}), 2u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 4}), Unit);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, Unit + 7, 4}), Unit + 7);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, Unit - 1, 3}), Unit - 1);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, Unit, 3}), Unit * 3);
    error(ServiceKind::Lseek, {A, Unit * 3, 4}, 6);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, 0, 1}), Unit * 3);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {A, Base + 128, 1, Unit * 5 + 2}), 1u);
    EXPECT_EQ(ok(ServiceKind::Lseek, {A, Unit * 3, 4}), Unit * 5);
    EXPECT_EQ(ok(ServiceKind::Lseek, {D, Unit * 5, 3}), Unit * 5 + 3);
    EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Unit * 2}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Unit * 5 + 3}), 0u);
    error(ServiceKind::Lseek, {A, Unit * 2, 4}, 6);
    for (auto FD : {A, B, D})
      EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    const auto Reopened = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Lseek, {Reopened, 0, 4}), Unit);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Reopened, Unit, 3}), Unit * 2);
  }
}

TEST_P(DarwinFileTest,
       SparseSeekRequiresKnownAllocationAndPreservesErrorState) {
  const auto Unknown = ok(ServiceKind::Open, {Base});
  for (uint64_t Whence : {3u, 4u}) {
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {Unknown, 0, Whence}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
    error(ServiceKind::Lseek, {999, UINT64_MAX, Whence}, 9);
    error(ServiceKind::Lseek, {1, UINT64_MAX, Whence}, 29);
  }
  mutationPolicy();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 4}), 2u);
  error(ServiceKind::Write, {FD, UINT64_MAX, 1}, 14);
  path("X", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 0}), 1u);
  for (uint64_t Whence : {3u, 4u}) {
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {FD, 0, Whence}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 2u);
  }
  path("/");
  const auto Directory = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Directory, 0, 3}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileSeek);
}

TEST_P(DarwinFileTest, SparseSeekEmptyInputAndRejectedGrowthKeepTheirState) {
  mutationPolicy();
  Options->Files["/data"].clear();
  Options->Metadata["/data"].Size = Options->Metadata["/data"].Blocks = 0;
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  for (uint64_t Whence : {3u, 4u})
    error(ServiceKind::Lseek, {FD, 0, Whence}, 6);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 8193}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {FD, darwin_file_limits::Bytes}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 8192, 3}), 8192u);
  error(ServiceKind::Lseek, {FD, 8192, 4}, 6);
  error(ServiceKind::Lseek, {FD, 8193, 3}, 6);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 8192u);
}

TEST_P(DarwinFileTest, MutationMetadataSharesOneNodeAndPreservesOtherFields) {
  mutationPolicy();
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto B = ok(ServiceKind::Open, {Base});
  const auto D = ok(ServiceKind::Dup, {A});
  auto Expected = status(B);
  const auto Initial = Expected;
  EXPECT_EQ(ok(ServiceKind::Write, {A, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(status(B), Initial);
  // A same-size truncate still publishes the explicitly configured times.
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {D, 6}), 0u);
  for (auto Offset : {48u, 64u}) {
    llvm::support::endian::write64le(Expected.data() + Offset, uint64_t(-7));
    llvm::support::endian::write64le(Expected.data() + Offset + 8, 123456789);
  }
  EXPECT_EQ(status(A), Expected);
  EXPECT_EQ(status(B), Expected);
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {D, Base + 128, 2, 8191}), 2u);
  llvm::support::endian::write64le(Expected.data() + 96, 8193);
  llvm::support::endian::write64le(Expected.data() + 104, 24);
  EXPECT_EQ(status(B), Expected);
  for (auto Kind : {ServiceKind::Stat64, ServiceKind::Lstat64}) {
    EXPECT_EQ(ok(Kind, {Base, Base + 256}), 0u);
    std::array<uint8_t, 144> Bytes;
    llvm::cantFail(Space->read(Base + 256, Bytes));
    EXPECT_EQ(Bytes, Expected);
  }
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {B, UINT64_MAX, Base + 256, 0x400}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
  for (auto FD : {A, B, D})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  EXPECT_EQ(status(ok(ServiceKind::Open, {Base})), Expected);
  EXPECT_EQ(Options->Metadata.at("/data").Size, 6u);
  EXPECT_EQ(Options->Metadata.at("/data").Blocks, 8u);
  EXPECT_EQ(Options->Metadata.at("/data").ModificationTime.Seconds, INT64_MAX);
}

TEST_P(DarwinFileTest, SparseUnitMetadataTracksHolesWritesAndDiscardedUnits) {
  for (uint32_t Unit : {512u, 4096u, 16384u}) {
    SCOPED_TRACE(Unit);
    mutationPolicy(Unit);
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto FD = ok(ServiceKind::Open, {Base, 2});
    auto Check = [&](uint64_t Size, uint64_t Blocks) {
      auto Bytes = status(FD);
      EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 96), Size);
      EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 104), Blocks);
      // I/O advice stays independent of the policy's allocation unit.
      EXPECT_EQ(llvm::support::endian::read32le(Bytes.data() + 112), 4096u);
    };
    Check(6, Unit / 512);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit * 6 + 1}), 0u);
    Check(Unit * 6 + 1, Unit / 512);
    llvm::cantFail(Space->writeInteger(Base + 128, 0, 2));
    // A zero-valued write into a hole allocates, even without a byte change.
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 2, Unit * 2 - 1}), 2u);
    Check(Unit * 6 + 1, Unit / 512 * 3);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 2, Unit * 2 - 1}), 2u);
    Check(Unit * 6 + 1, Unit / 512 * 3);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit + 1}), 0u);
    Check(Unit + 1, Unit / 512 * 2);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit * 6 + 1}), 0u);
    Check(Unit * 6 + 1, Unit / 512 * 2);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit}), 0u);
    Check(Unit, Unit / 512);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 0}), 0u);
    Check(0, 0);
    auto Empty = ok(ServiceKind::Open, {Base, 0x400});
    EXPECT_EQ(status(Empty), status(FD));
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Unit * 6 + 1}), 0u);
    Check(Unit * 6 + 1, 0);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, Unit * 4}), 1u);
    Check(Unit * 6 + 1, Unit / 512);
  }
}

TEST_P(DarwinFileTest, MutationMetadataRetainsKnownStateAcrossRejectedEffects) {
  mutationPolicy();
  const auto FD = ok(ServiceKind::Open, {Base, 10});
  const auto Initial = status(FD);
  const uint64_t End = Base + Page * 2 - 2;
  path("X", End);
  EXPECT_FALSE(invoke(ServiceKind::Write, {FD, End, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialWrite);
  EXPECT_EQ(status(FD), Initial);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {FD, darwin_file_limits::Bytes}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(status(FD), Initial);
  {
    auto Mapping = Files->mappingSource(FD);
    EXPECT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Mapping));
    EXPECT_FALSE(invoke(ServiceKind::Write, {FD, End, 1}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationMapping);
    EXPECT_EQ(status(FD), Initial);
  }
  EXPECT_EQ(ok(ServiceKind::Write, {FD, End, 1}), 1u);
  const auto Current = status(FD);
  EXPECT_NE(Current, Initial);
  error(ServiceKind::Fstat64, {FD, 0}, 14);
  const uint64_t Partial = Base + Page * 2 - 8;
  llvm::cantFail(Space->writeInteger(Partial, 0xaabbccddeeff0011, 8));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Partial}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialStatus);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Partial, 8)),
            0xaabbccddeeff0011u);
  EXPECT_EQ(status(FD), Current);
}

TEST_P(DarwinFileTest, InitialDenseAllocationIncludesEmptyAndMultipleUnits) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    mutationPolicy(512);
    const unsigned Size = Case ? 513 : 0;
    Options->Files["/data"] = std::vector<uint8_t>(Size, 0);
    Options->Metadata["/data"].Size = Size;
    Options->Metadata["/data"].Blocks = Case ? 2 : 0;
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto FD = ok(ServiceKind::Open, {Base, Case ? 2u : 0x402u});
    path("", Base + 128);
    if (Case == 1)
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 512}), 0u);
    else if (Case == 2)
      EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 0}), 1u);
    else
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 0}), 0u);
    EXPECT_EQ(llvm::support::endian::read64le(status(FD).data() + 104), Case);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 2048}), 0u);
    EXPECT_EQ(llvm::support::endian::read64le(status(FD).data() + 104), Case);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 1536}), 1u);
    EXPECT_EQ(llvm::support::endian::read64le(status(FD).data() + 104),
              Case + 1);
  }
}

TEST_P(DarwinFileTest,
       UnknownMutationMetadataCannotBeResurrectedByLaterSuccess) {
  for (bool InitiallyModified : {false, true}) {
    mutationPolicy();
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    const auto FD = ok(ServiceKind::Open, {Base, 10});
    if (InitiallyModified)
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 8}), 0u);
    error(ServiceKind::Write, {FD, UINT64_MAX, 1}, 14);
    path("X", Base + 128);
    EXPECT_EQ(ok(ServiceKind::Write, {FD, Base + 128, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
    const auto Reopened = ok(ServiceKind::Open, {Base});
    contents(Reopened, {'a'});
    llvm::cantFail(Space->writeInteger(Base + 256, 0xaabb, 2));
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Reopened, Base + 256}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0xaabbu);
    EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + 256}));
  }
}

TEST(DarwinFileOptions, CreationPolicyRequiresExplicitParentsAndFreshIdentity) {
  DarwinFileOptions Good;
  Good.MutableDirectories.insert("/");
  Good.Metadata["/"] = darwin_test::creationParentMetadata();
  Good.InitialUmask = 07777;
  Good.CreationPolicy = darwin_test::CreationPolicy;
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (unsigned Case = 0; Case != 14; ++Case) {
    SCOPED_TRACE(Case);
    auto O = Good;
    auto &P = *O.CreationPolicy;
    switch (Case) {
    case 0:
      O.MutableDirectories.clear();
      break;
    case 1:
      O.InitialUmask.reset();
      break;
    case 2:
      O.InitialUmask = 010000;
      break;
    case 3:
      O.Metadata.clear();
      break;
    case 4:
      P.FirstInode = 0;
      break;
    case 5:
      P.FirstInode = 41;
      break;
    case 6:
      P.BlockSize = 0;
      break;
    case 7:
      P.BlockSize = uint32_t(INT32_MAX) + 1;
      break;
    case 8:
      P.Time.Nanoseconds = -1;
      break;
    case 9:
      P.Time.Nanoseconds = 1000000000;
      break;
    case 10:
      P.Mutation.AllocationUnit = 513;
      break;
    case 11:
      P.Mutation.Time.Nanoseconds = -1;
      break;
    case 12:
      O.Directories.insert("/empty");
      O.MutableDirectories.insert("/empty");
      break;
    case 13:
      O.Files["/data"] = {};
      O.Directories.insert("/empty");
      O.DirectoryContents["/"] = darwin_test::directoryContents();
      P.FirstInode = 0xfedcba9876543210ULL;
      break;
    }
    auto Rejected = validateFileOptions(O);
    EXPECT_TRUE(bool(Rejected));
    llvm::consumeError(std::move(Rejected));
  }
  Good.CreationPolicy->FirstInode = UINT64_MAX;
  Good.CreationPolicy->Time = {INT64_MIN, 999999999};
  Good.CreationPolicy->Mutation.Time = {INT64_MAX, 0};
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  Good.Metadata["/"].Inode = UINT64_MAX;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileCreationPolicy);
}

TEST(DarwinFileOptions, MutationPolicyAdmitsOnlyExplicitCoherentMetadata) {
  DarwinFileOptions Good;
  Good.Files["/data"] = std::vector<uint8_t>(10);
  Good.WritableFiles.insert("/data");
  Good.Metadata["/data"] = darwin_test::mutationMetadata();
  Good.MutationPolicies["/data"] = darwin_test::MutationPolicy;
  ASSERT_FALSE(bool(validateFileOptions(Good)));
  for (uint32_t Unit :
       {512u, 4096u, 16384u, uint32_t(darwin_file_limits::Bytes)}) {
    auto O = Good;
    O.MutationPolicies["/data"].AllocationUnit = Unit;
    O.Metadata["/data"].Blocks = Unit / 512;
    EXPECT_FALSE(bool(validateFileOptions(O)));
  }
  for (unsigned Case = 0; Case != 13; ++Case) {
    auto O = Good;
    auto &P = O.MutationPolicies["/data"];
    auto &M = O.Metadata["/data"];
    switch (Case) {
    case 0:
      O.WritableFiles.clear();
      break;
    case 1:
      O.Metadata.clear();
      break;
    case 2:
      P.AllocationUnit = 0;
      break;
    case 3:
      P.AllocationUnit = 256;
      break;
    case 4:
      P.AllocationUnit = 513;
      break;
    case 5:
      P.AllocationUnit = darwin_file_limits::Bytes * 2;
      break;
    case 6:
      P.Time.Nanoseconds = -1;
      break;
    case 7:
      P.Time.Nanoseconds = 1000000000;
      break;
    case 8:
      M.Blocks = 0;
      break;
    case 9:
      M.Mode |= 04000;
      break;
    case 10:
      M.Mode |= 02000;
      break;
    case 11:
      M.Mode |= 01000;
      break;
    case 12:
      M.LinkCount = 2;
      break;
    }
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileMutationPolicy)
        << Case;
  }
  Good.Metadata["/data"].Flags = 1;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileMutationPolicy);
  Good.Metadata["/data"].Flags = 0;
  Good.MutationPolicies["/absent"] = darwin_test::MutationPolicy;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileMutationPolicy);
  Good.MutationPolicies.erase("/absent");
  Good.StandardInput = std::vector<uint8_t>(darwin_file_limits::Bytes - 28);
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  Good.StandardInput->push_back(0);
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, WriteErrorOrderZeroCountsAndClippedAppendMatchNative) {
  Options->WritableFiles.insert("/data");
  auto A = ok(ServiceKind::Open, {Base, 2});
  auto RO = ok(ServiceKind::Open, {Base});
  auto WO = ok(ServiceKind::Open, {Base, 1});
  error(ServiceKind::Write, {999, 0, 0x80000000}, 22);
  error(ServiceKind::Write, {999, 0, 0}, 9);
  error(ServiceKind::Pwrite, {999, 0, 0, UINT64_MAX}, 22);
  error(ServiceKind::Pwrite, {999, 0, 0, UINT64_MAX - 1}, 9);
  error(ServiceKind::Pwrite, {A, 0, 0, UINT64_MAX - 1}, 22);
  error(ServiceKind::Pwrite, {1, 0, 0, 0}, 29);
  error(ServiceKind::Write, {RO, 0, 0}, 9);
  error(ServiceKind::Read, {WO, 0, 0}, 9);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {A, UINT64_MAX, 0, 99}), 0u);
  error(ServiceKind::Pwrite, {A, 0, 0, INT64_MAX}, 27);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 4, 8}), 0u);
  EXPECT_EQ(ok(ServiceKind::Write, {A, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
  error(ServiceKind::Write, {A, UINT64_MAX, 2}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 6u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Write, {A, 0, 0}, 27);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, INT64_MAX - 1, 0}),
            uint64_t(INT64_MAX - 1));
  path("XY", Base + 128);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base + 128, 2}), 1u);
  contents(RO, {'a', 'b', 0, 0xff, 'e', 'f', 'X'});
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 7u);
}

TEST_P(DarwinFileTest, PartialInputRefusesEffectsAndDirtyMetadataIsNotReused) {
  Options->WritableFiles.insert("/data");
  auto M = darwin_test::metadata(6);
  M.Flags = 0;
  Options->Metadata["/data"] = M;
  auto A = ok(ServiceKind::Open, {Base, 10});
  auto B = ok(ServiceKind::Open, {Base});
  const uint64_t End = Base + Page * 2 - 2;
  path("X", End);
  EXPECT_FALSE(invoke(ServiceKind::Write, {A, End, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialWrite);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 0u);
  contents(B, {'a', 'b', 0, 0xff, 'e', 'f'});
  EXPECT_EQ(ok(ServiceKind::Fstat64, {A, Base + 256}), 0u);
  EXPECT_EQ(ok(ServiceKind::Write, {A, End, 1}), 1u);
  ASSERT_FALSE(bool(Space->writeInteger(Base + 256, 0x7777, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0x7777u);
  ok(ServiceKind::Close, {A});
  ok(ServiceKind::Close, {B});
  auto C = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {C, 0}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + 256}));
}

TEST_P(DarwinFileTest, FailedAppendInvalidatesMetadataWithoutPublishingBytes) {
  Options->WritableFiles.insert("/data");
  auto M = darwin_test::metadata(6);
  M.Flags = 0;
  Options->Metadata["/data"] = M;
  const auto A = ok(ServiceKind::Open, {Base, 10});
  const auto B = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Write, {A, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fstat64, {B, Base + 256}), 0u);
  error(ServiceKind::Write, {A, UINT64_MAX, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 6u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 10u);
  contents(B, {'a', 'b', 0, 0xff, 'e', 'f'});
  llvm::cantFail(Space->writeInteger(Base + 256, 0x7777, 2));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {B, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutatedMetadata);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256, 2)), 0x7777u);
}

TEST_P(DarwinFileTest, AggregateMutationBudgetIsReclaimedByShrinking) {
  Options->Files["/other"] = {};
  Options->WritableFiles = {"/data", "/other"};
  Options->StandardInput = {1, 2, 3};
  const uint64_t Capacity = darwin_file_limits::Bytes - 2 * 6 - 2 * 7 - 3;
  auto A = ok(ServiceKind::Open, {Base, 2});
  path("/other", Base + 128);
  auto B = ok(ServiceKind::Open, {Base + 128, 2});
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {B, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {A, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {B, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Pwrite, {A, Base, 1, INT64_MAX - 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 2}), 0u);
}

TEST(DarwinFileOptions, WritableAdmissionRejectsAliasesAndRestrictiveFlags) {
  DarwinFileOptions O;
  O.Files["/a"] = {};
  O.Files["/b"] = {};
  O.WritableFiles = {"/missing"};
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileWritableOption);
  O.WritableFiles = {"/a"};
  auto M = darwin_test::metadata(0);
  M.Flags = 0;
  O.Metadata["/a"] = O.Metadata["/b"] = M;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileWritableAlias);
  O.Metadata["/b"].Device++;
  EXPECT_FALSE(bool(validateFileOptions(O)));
  for (uint32_t Flags : {2u, 4u, 0x20000u, 0x40000u}) {
    O.Metadata["/a"].Flags = Flags;
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileWritableFlags);
  }
}

class FailingFileInput : public GuestMemory {
  GuestMemory &Memory;

public:
  bool FailAccess = false, FailRead = false, FailWrite = false,
       FailBudget = false;
  uint64_t FailureAddress = 0;
  explicit FailingFileInput(GuestMemory &Memory) : Memory(Memory) {}
  llvm::Error map(uint64_t A, uint64_t S, unsigned P) override {
    return Memory.map(A, S, P);
  }
  llvm::Error protect(uint64_t A, uint64_t S, unsigned P) override {
    return Memory.protect(A, S, P);
  }
  llvm::Error read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) override {
    if (FailRead && A >= FailureAddress) {
      if (FailBudget)
        return llvm::make_error<GuestMemoryLimitError>();
      // A transport may fill part of the scratch span before failing.
      B.front() = 'x';
      return failure("file input transport failed");
    }
    return Memory.read(A, B);
  }
  llvm::Error write(uint64_t A, llvm::ArrayRef<uint8_t> B) override {
    if (FailWrite && A >= FailureAddress) {
      if (FailBudget)
        return llvm::make_error<GuestMemoryLimitError>();
      return failure("file output transport failed");
    }
    return Memory.write(A, B);
  }
  llvm::Expected<bool> canAccess(uint64_t A, uint64_t S,
                                 unsigned P) const override {
    if (FailAccess && A >= FailureAddress) {
      if (FailBudget)
        return llvm::make_error<GuestMemoryLimitError>();
      return failure("file input preflight failed");
    }
    return Memory.canAccess(A, S, P);
  }
};

TEST_P(DarwinFileTest, DirectoryPathTransportFailuresKeepNamespaceUnchanged) {
  Options->MutableDirectories.insert("/");
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  path("/new");
  for (auto Kind : {ServiceKind::Mkdir, ServiceKind::Rmdir}) {
    for (bool Preflight : {true, false}) {
      Input.FailureAddress = Base + 2;
      Input.FailAccess = Preflight;
      Input.FailRead = !Preflight;
      auto Failed = Files->handle(Kind, {0, 0, {Base}, std::nullopt}, Result);
      ASSERT_FALSE(bool(Failed));
      llvm::consumeError(Failed.takeError());
      Input.FailAccess = Input.FailRead = false;
      if (Kind == ServiceKind::Mkdir)
        error(ServiceKind::Access, {Base}, 2);
      else
        EXPECT_EQ(ok(ServiceKind::Access, {Base}), 0u);
    }
    EXPECT_EQ(ok(Kind, {Base}), 0u);
  }
  const auto FD = ok(ServiceKind::Open, {Base, 0x202});
  EXPECT_EQ(FD, 3u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, AccessTransportFailuresPreserveFilesAndDescriptors) {
  mutationPolicy();
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base});
  const auto Before = status(FD);
  path("/data", Base + 128);
  for (bool Preflight : {true, false}) {
    Input.FailureAddress = Base + 130;
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Access,
                                {0, 33, {Base + 128}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    llvm::consumeError(Failed.takeError());
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(ok(ServiceKind::Access, {Base + 128}), 0u);
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0u);
  }
}

TEST_P(DarwinFileTest, OpenAtPrefixTransportFailuresDoNotReserveDescriptors) {
  mutationPolicy();
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base});
  const auto Before = status(FD);
  path("/", Base + 128);
  const auto Dir = ok(ServiceKind::Open, {Base + 128});
  path("data", Base + 128);
  for (bool Preflight : {true, false}) {
    Input.FailureAddress = Base + 128;
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(
        ServiceKind::OpenAt,
        {0, 463, {Dir, Base + 128, 0x20000100}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(status(FD), Before);
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0u);
    EXPECT_EQ(ok(ServiceKind::OpenAt, {Dir, Base + 128, 0x100}), 5u);
    ok(ServiceKind::Close, {5});
  }
}

TEST_P(DarwinFileTest, RenameTargetInputFailuresPreserveBothObjects) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  Options->Files["/target"] = {'T'};
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto A = ok(ServiceKind::Open, {Base, 2});
  const auto Before = status(A);
  path("/target", Base + 128);
  const auto T = ok(ServiceKind::Open, {Base + 128});
  for (bool Preflight : {true, false}) {
    Input.FailureAddress = Base + 128;
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed =
        Files->handle(ServiceKind::Rename,
                      {0, 128, {Base, Base + 128}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    identity(A, "/data");
    identity(T, "/target");
    EXPECT_EQ(status(A), Before);
    contents(T, {'T'});
  }
  error(ServiceKind::Rename, {Base, UINT64_MAX}, 14);
  EXPECT_EQ(status(A), Before);
  EXPECT_EQ(ok(ServiceKind::Rename, {Base, Base + 128}), 0u);
  identity(A, "/target");
  contents(T, {'T'});
}

TEST_P(DarwinFileTest, UmaskNeedsNeitherGuestMemoryNorAvailableDescriptors) {
  Options->InitialUmask = 0027;
  Options->DescriptorLimit = 3;
  FailingFileInput Input(*Space);
  Input.FailAccess = Input.FailRead = true;
  Files = std::make_unique<DarwinFiles>(Input, Options);
  EXPECT_EQ(ok(ServiceKind::Umask, {07000, UINT64_MAX}), 0027u);
  error(ServiceKind::Open, {UINT64_MAX}, 24);
  EXPECT_EQ(ok(ServiceKind::Umask, {0022, UINT64_MAX}), 07000u);
  EXPECT_EQ(*Options->InitialUmask, 0027u);
}

TEST_P(DarwinFileTest, CreatePathFailuresPreserveMissingNameAndDescriptor) {
  Options->MutableDirectories.insert("/");
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  path("/new");
  for (bool Preflight : {true, false}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Open,
                                {0, 5, {Base, 0xa02}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    error(ServiceKind::Open, {Base}, 2);
  }
  const uint64_t End = Base + Page * 2;
  llvm::cantFail(Space->writeInteger(End - 1, '/', 1));
  error(ServiceKind::Open, {End - 1, 0xa02}, 14);
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0xa02}), 3u);
}

TEST_P(DarwinFileTest, UnlinkPathFailuresPreserveNameAndMetadata) {
  mutationPolicy();
  Options->MutableDirectories.insert("/");
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base, 2});
  const auto Initial = status(FD);
  for (bool Preflight : {true, false}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Unlink,
                                {0, 10, {Base}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(status(FD), Initial);
    const auto B = ok(ServiceKind::Open, {Base});
    EXPECT_EQ(ok(ServiceKind::Close, {B}), 0u);
  }
  const uint64_t End = Base + Page * 2;
  llvm::cantFail(Space->writeInteger(End - 1, '/', 1));
  error(ServiceKind::Unlink, {End - 1}, 14);
  EXPECT_EQ(status(FD), Initial);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
}

TEST_P(DarwinFileTest,
       InputTransportFailuresDoNotCommitContentsOrAppendCursor) {
  mutationPolicy();
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  const auto FD = ok(ServiceKind::Open, {Base, 10});
  const auto Initial = status(FD);
  for (bool Preflight : {true, false}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    auto Failed = Files->handle(ServiceKind::Write,
                                {0, 4, {FD, Base, 2}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
    contents(FD, {'a', 'b', 0, 0xff, 'e', 'f'});
    EXPECT_EQ(status(FD), Initial);
  }
}

TEST_P(DarwinFileTest, DupSharesOffsetsButOpenAndPreadDoNot) {
  auto A = ok(ServiceKind::Open, {Base});
  auto B = ok(ServiceKind::Dup, {A});
  auto C = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {A, Base + Page, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {C, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Pread, {B, Base + Page, 2, 2}), 2u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 2)), 0xff00u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {A, 0, 1}), 2u);
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {B, Base + Page, 1}), 1u);
  error(ServiceKind::Read, {A, Base + Page, 1}, 9);
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), A);
}

TEST_P(DarwinFileTest, PerDescriptorFlagsDup2AndCaptureSinkIdentity) {
  auto A = ok(ServiceKind::Open, {Base, 0x1000000});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, A}), A);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  auto B = ok(ServiceKind::Dup, {A});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 2, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 2, 2}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {B, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 67, 20}), 20u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {20, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 0, 20}), 21u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {21, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 3}), 0u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {1, A}), A);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Fcntl, {A, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {1}), 0u);
  error(ServiceKind::Write, {1, Base, 1}, 9);
  EXPECT_EQ(ok(ServiceKind::Write, {A, Base, 1}), 1u);
  EXPECT_EQ(ok(ServiceKind::Dup2, {A, 2}), 2u);
  EXPECT_EQ(ok(ServiceKind::Write, {2, Base, 1}), 1u);
  EXPECT_EQ(Result.StandardOutput, "///");
  EXPECT_TRUE(Result.StandardError.empty());
  error(ServiceKind::Dup2, {A, UINT64_MAX}, 9);
  error(ServiceKind::Fcntl, {A, 0, UINT64_MAX}, 22);
}

TEST_P(DarwinFileTest, ReadErrorsEOFAndSeekOverflowPreserveCursor) {
  auto FD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Read, {99, 0, 0x80000000}, 22);
  error(ServiceKind::Pread, {99, 0, UINT64_MAX, UINT64_MAX}, 22);
  error(ServiceKind::Read, {99, 0, 1}, 9);
  error(ServiceKind::Read, {FD, 0, 1}, 14);
  error(ServiceKind::Read, {FD, UINT64_MAX, 1}, 14);
  error(ServiceKind::Pread, {FD, Base + Page, 0, UINT64_MAX}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 1, 2}), 7u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, UINT64_MAX, 0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {FD, 1, 1}, 84);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {FD, uint64_t(INT64_MIN), 1}, 22);
  error(ServiceKind::Lseek, {FD, 0, 99}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, uint64_t(-1), 2}), 5u);
  EXPECT_EQ(ok(ServiceKind::Read, {FD, Base + Page, 10}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'f');
}

TEST_P(DarwinFileTest, PartialCopyoutIsRefusedBeforeBytesOrOffsetChange) {
  auto FD = ok(ServiceKind::Open, {Base});
  const uint64_t End = Base + Page * 2 - 2;
  ASSERT_FALSE(bool(Space->writeInteger(End, 0x7777, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Read, {FD, End, 4}));
  EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 2)), 0x7777u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  error(ServiceKind::Read, {FD, Base + Page, 1}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, FiniteInputSharesItsCursorAndClosedFDZeroIsReusable) {
  EXPECT_FALSE(invoke(ServiceKind::Read, {0, Base + Page, 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInput);
  Options->StandardInput = {0, 0xff, 'x'};
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  auto Copy = ok(ServiceKind::Dup, {0});
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, Base + Page, 2}), 2u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 2)), 0xff00u);
  error(ServiceKind::Pread, {Copy, Base + Page, 1, 0}, 29);
  error(ServiceKind::Lseek, {Copy, 0, 0}, 29);
  EXPECT_EQ(ok(ServiceKind::Close, {0}), 0u);
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, Base + Page, 3}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'x');
  EXPECT_EQ(ok(ServiceKind::Read, {Copy, 0, 1}), 0u);
  Options->StandardInput = std::vector<uint8_t>();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_EQ(ok(ServiceKind::Read, {0, 0, 1}), 0u);
}

TEST_P(DarwinFileTest, PathErrorsAndResourceExhaustionDoNotLeakDescriptors) {
  Options->Files["/nested/file"] = {'x'};
  path("/nested/file");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  path("/nested");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  for (unsigned I = 0; I != 4; ++I)
    error(ServiceKind::Open, {0}, 14);
  path("");
  error(ServiceKind::Open, {Base}, 2);
  path("/missing");
  error(ServiceKind::Open, {Base}, 2);
  path("/data/child");
  error(ServiceKind::Open, {Base}, 20);
  path("/data/" + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 20);
  path("/missing/" + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 2);
  path('/' + std::string(256, 'a'));
  error(ServiceKind::Open, {Base}, 63);
  path(std::string(1024, 'a'));
  error(ServiceKind::Open, {Base}, 63);
  path("/data", Base + Page * 2 - 6);
  EXPECT_EQ(ok(ServiceKind::Open, {Base + Page * 2 - 6}), 3u);
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {0}, 24);
  error(ServiceKind::Dup, {3}, 24);
  EXPECT_EQ(ok(ServiceKind::Close, {3}), 0u);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
}

TEST_P(DarwinFileTest, UnsupportedOperationsDoNotPretendToBeMissingFiles) {
  for (auto Text : {"relative", "./data", "../data"}) {
    path(Text);
    EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
    EXPECT_EQ(Result.Stop, ProcessStopReason::UnsupportedService);
  }
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 1}));
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {3, 999}));
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {3, 0, 3}));
  Options.reset();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
  Options.emplace();
  error(ServiceKind::Open, {Base}, 2);
}

TEST_P(DarwinFileTest, Stat64PreservesAllFieldsPaddingAndDescriptorOffsets) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  // Independently specified little-endian LP64 record, including the rdev,
  // alignment gap and all reserved bytes. Also checked against the native SDK.
  const auto Expected = llvm::fromHex(
      "85ffffffa48103001032547698badcfeefcdab8998badcfe0000000000000000"
      "01000000000000800100000000000000ffffffffffffff7fffc99a3b00000000"
      "fdffffffffffffff0400000000000000fbffffffffffffff0600000000000000"
      "060000000000000008000000000000000010000034120000efcdab8900000000"
      "00000000000000000000000000000000");
  const uint64_t Buffer = Base + Page - 7;
  auto FD = ok(ServiceKind::Open, {Base});
  auto Copy = ok(ServiceKind::Dup, {FD});
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 2, 0}), 2u);
  EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    ASSERT_FALSE(bool(Space->writeInteger(Buffer - 1, 0xaa, 1)));
    ASSERT_FALSE(bool(Space->writeInteger(Buffer + 144, 0xbb, 1)));
    EXPECT_EQ(ok(Kind, {Kind == ServiceKind::Fstat64 ? Copy : Base, Buffer}),
              0u);
    std::array<uint8_t, 144> Bytes;
    ASSERT_FALSE(bool(Space->read(Buffer, Bytes)));
    EXPECT_EQ(llvm::toHex(llvm::ArrayRef<uint8_t>(Bytes)),
              llvm::toHex(Expected));
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer - 1, 1)), 0xaau);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 144, 1)), 0xbbu);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Copy, 0, 1}), 2u);
  }
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), FD);
}

TEST_P(DarwinFileTest, Stat64FailureOrderAndPartialOutputHaveNoSideEffects) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  auto FD = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Fstat64, {99, 0}, 9);
  error(ServiceKind::Stat64, {0, 0}, 14);
  path("/absent");
  error(ServiceKind::Stat64, {Base, 0}, 2);
  path("/data/child");
  error(ServiceKind::Lstat64, {Base, 0}, 20);
  path("/data");
  Options->DescriptorLimit = 4;
  error(ServiceKind::Open, {Base}, 24);
  EXPECT_EQ(ok(ServiceKind::Stat64, {Base, Base + Page}), 0u);
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    const uint64_t Source = Kind == ServiceKind::Fstat64 ? FD : Base;
    error(Kind, {Source, 0}, 14);
    error(Kind, {Source, UINT64_MAX}, 14);
    const auto End = Base + Page * 2 - 8;
    ASSERT_FALSE(bool(Space->writeInteger(End, 0xaabbccddeeff0011, 8)));
    EXPECT_FALSE(invoke(Kind, {Source, End}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialStatus);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 8)), 0xaabbccddeeff0011u);
  }
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  ASSERT_FALSE(bool(Space->protect(Base + Page, Page, Read | UserAccessible)));
  error(ServiceKind::Fstat64, {FD, Base + Page}, 14);
}

TEST_P(DarwinFileTest, UnknownMetadataAndDirectoriesAreNotFabricated) {
  auto FD = ok(ServiceKind::Open, {Base});
  for (auto Kind :
       {ServiceKind::Stat64, ServiceKind::Lstat64, ServiceKind::Fstat64}) {
    EXPECT_FALSE(
        invoke(Kind, {Kind == ServiceKind::Fstat64 ? FD : Base, Base + Page}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  }
  for (auto FD : {0u, 1u, 2u}) {
    EXPECT_FALSE(invoke(ServiceKind::Fstat64, {FD, Base + Page}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  }
  path("/");
  EXPECT_FALSE(invoke(ServiceKind::Stat64, {Base, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  Options.reset();
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Lstat64, {Base, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileInputs);
}

TEST(DarwinFileOptions,
     MetadataAdmissionRejectsUnknownPathsAndIncoherentValues) {
  DarwinFileOptions O;
  O.Files["/data"] = std::vector<uint8_t>(10);
  O.Metadata["/missing"] = darwin_test::metadata();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataPath);
  O.Metadata.clear();
  auto &M = O.Metadata["/data"];
  for (unsigned Case = 0; Case != 8; ++Case) {
    M = darwin_test::metadata();
    switch (Case) {
    case 0:
      M.Mode = 0040644;
      break;
    case 1:
      M.Size = 9;
      break;
    case 2:
      M.BlockSize = uint32_t(INT32_MAX) + 1;
      break;
    case 3:
      M.Blocks = uint64_t(INT64_MAX) + 1;
      break;
    case 4:
      M.AccessTime.Nanoseconds = -1;
      break;
    case 5:
      M.ModificationTime.Nanoseconds = 1000000000;
      break;
    case 6:
      M.ChangeTime.Nanoseconds = -1;
      break;
    case 7:
      M.BirthTime.Nanoseconds = 1000000000;
      break;
    }
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileMetadataOption);
  }
  M = darwin_test::metadata();
  ASSERT_FALSE(bool(validateFileOptions(O)));
}

TEST(DarwinFileOptions, DirectoryAdmissionIncludesImplicitPathsAndCWD) {
  DarwinFileOptions O;
  O.Files["/tree/data"] = {};
  O.Directories = {"/tree", "/tree/empty/deep"};
  O.WorkingDirectory = "/tree/empty";
  auto M = darwin_test::metadata(128);
  M.Mode = 0040755;
  O.Metadata["/"] = M;
  O.Metadata["/tree/empty"] = M;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  for (auto Bad : {"", "relative", "/tree/data", "/absent", "/tree/../tree"}) {
    O.WorkingDirectory = Bad;
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E)) << Bad;
    llvm::consumeError(std::move(E));
  }
  O.WorkingDirectory = "/";
  for (auto Bad : {"/tree/data", "/tree/data/child", "/tree/", "relative"}) {
    O.Directories.insert(Bad);
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::FileOptionPath);
    O.Directories.erase(Bad);
  }
  O.Metadata["/"].Size = uint64_t(INT64_MAX) + 1;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataOption);
  O.Metadata["/"] = darwin_test::metadata();
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileMetadataOption);
  O = {};
  for (unsigned I = 0; I != 255; ++I)
    O.Directories.insert("/dir" + std::to_string(I));
  O.Files["/data"] = {};
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/extra");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 8);
  O.WorkingDirectory = "/"; // Six path bytes plus two CWD bytes fit exactly.
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Directories.insert("/");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, RelativeLookupWalksAncestorsAndPreservesErrorOrder) {
  Options->Directories.insert("/empty");
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base, value::OpenDirectory});
  auto Dup = ok(ServiceKind::Dup, {Dir});
  path("/data");
  auto File = ok(ServiceKind::Open, {Base});
  error(ServiceKind::Open, {Base, value::OpenDirectory}, 20);
  for (auto Text : {"/data/.", "/data/..", "/data/", "/data//child"}) {
    path(Text);
    error(ServiceKind::Open, {Base}, 20);
  }
  path("/absent/../data");
  error(ServiceKind::Open, {Base}, 2);
  for (auto Text : {"//./data", "/empty/../../data", "/../data"}) {
    path(Text);
    auto FD = ok(ServiceKind::OpenAt, {999, Base});
    EXPECT_EQ(ok(ServiceKind::Read, {FD, Base + Page, 1}), 1u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
    ok(ServiceKind::Close, {FD});
  }
  path("../data");
  auto FD = ok(ServiceKind::OpenAt, {Dup, Base});
  ok(ServiceKind::Close, {FD});
  error(ServiceKind::OpenAt, {999, Base}, 9);
  error(ServiceKind::OpenAt, {File, Base}, 20);
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {0, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileWorkingDirectory);
  path("");
  error(ServiceKind::OpenAt, {999, Base}, 9);
  error(ServiceKind::OpenAt, {File, Base}, 20);
  error(ServiceKind::OpenAt, {Dir, Base}, 2);
  error(ServiceKind::Open, {Base}, 2);
  error(ServiceKind::OpenAt, {999, 0}, 14);
  Options->DescriptorLimit = 6;
  error(ServiceKind::OpenAt, {999, 0}, 14);
  error(ServiceKind::Open, {0}, 24);
  error(ServiceKind::OpenAt, {0xfffffffe, 0}, 24);
}

TEST_P(DarwinFileTest, NoFollowFlagsKeepLookupAndDescriptorState) {
  path("/");
  const auto Dir = ok(ServiceKind::Open, {Base, 0x100000});
  for (uint64_t Flags : {0x100ULL, 0x20000000ULL, 0x1234567800000100ULL,
                         0xfedcba9820000000ULL}) {
    SCOPED_TRACE(Flags);
    path("/data");
    const auto FD = ok(ServiceKind::Open, {Base, Flags | 0x1000000});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 1}), 1u);
    const auto Dup = ok(ServiceKind::Dup, {FD});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 1}), 0u);
    EXPECT_EQ(ok(ServiceKind::Read, {Dup, Base + Page, 1}), 1u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
    EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 1u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 3}), 0u);
    ok(ServiceKind::Close, {Dup});
    ok(ServiceKind::Close, {FD});
    path("data");
    const auto Relative =
        ok(ServiceKind::OpenAt, {0x1234567800000000ULL | Dir, Base, Flags});
    contents(Relative, {'a', 'b', 0, 0xff, 'e', 'f'});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Relative, 3}), 0u);
    ok(ServiceKind::Close, {Relative});
    path(".");
    const auto Directory =
        ok(ServiceKind::OpenAt, {Dir, Base, Flags | 0x100000});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Directory, 3}), 0u);
    identity(Directory, "/");
    ok(ServiceKind::Close, {Directory});
  }
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x1000}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileOpenFlags);
  EXPECT_EQ(ok(ServiceKind::Open, {Base, 0x100}), 4u);
}

TEST_P(DarwinFileTest, NoFollowFlagsPreserveWriteCreateAndTruncateControl) {
  mutationPolicy();
  creationPolicy();
  unsigned Created = 0;
  for (uint64_t Flags : {0x100u, 0x20000000u}) {
    SCOPED_TRACE(Flags);
    path("/data");
    const auto FD = ok(ServiceKind::Open, {Base, Flags | 0xa});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0xau);
    path("x", Base + 128);
    EXPECT_EQ(ok(ServiceKind::Pwrite, {FD, Base + 128, 1, 0}), 1u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {FD, 3}), 0x1000au);
    ok(ServiceKind::Close, {FD});
    const auto Truncated = ok(ServiceKind::Open, {Base, Flags | 0x402});
    contents(Truncated, {});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Truncated, 3}), 0x10002u);
    ok(ServiceKind::Close, {Truncated});
    path("/new" + std::to_string(Created));
    error(ServiceKind::Open, {Base, 0x20000702, 0666}, 22);
    error(ServiceKind::Access, {Base}, 2);
    const auto New = ok(ServiceKind::Open, {Base, Flags | 0x602, 0666});
    EXPECT_EQ(ok(ServiceKind::Fcntl, {New, 3}), 2u);
    EXPECT_EQ(llvm::support::endian::read64le(status(New).data() + 8),
              darwin_test::CreationPolicy.FirstInode + Created);
    error(ServiceKind::Open, {Base, Flags | 0xa00, 0666}, 17);
    contents(New, {});
    EXPECT_EQ(ok(ServiceKind::Write, {New, Base + 128, 1}), 1u);
    contents(New, {'x'});
    ok(ServiceKind::Close, {New});
    ++Created;
  }
}

TEST_P(DarwinFileTest, OpenAtFirstBytePrecedesFlagsAndFullPathImport) {
  path("/");
  const auto Dir = ok(ServiceKind::Open, {Base});
  path("/data");
  const auto File = ok(ServiceKind::Open, {Base});
  const auto Dup = ok(ServiceKind::Dup, {Dir});
  constexpr uint64_t UserEnd = 0x0000800000000000ULL;
  ASSERT_FALSE(
      bool(Space->map(UserEnd - Page, Page, Read | Write | UserAccessible)));
  for (auto End : {Base + Page * 2 - 1, UserEnd - 1}) {
    SCOPED_TRACE(End);
    for (unsigned Byte : {'r', '/', '\0'}) {
      llvm::cantFail(Space->writeInteger(End, Byte, 1));
      for (uint64_t Flags : {0u, 0x100u, 0x20000000u, 0x20000100u}) {
        SCOPED_TRACE(Flags);
        const bool Pair = Flags == 0x20000100;
        error(ServiceKind::OpenAt, {999, End, Flags},
              Byte == '/' ? (Pair ? 22 : 14) : 9);
        error(ServiceKind::OpenAt, {File, End, Flags},
              Byte == '/' ? (Pair ? 22 : 14) : 20);
        error(ServiceKind::OpenAt, {Dup, End, Flags},
              Pair   ? 22
              : Byte ? 14
                     : 2);
        error(ServiceKind::OpenAt, {999, End, Flags | 3}, Byte == '/' ? 22 : 9);
        error(ServiceKind::OpenAt, {Dup, End, Flags | 3}, 22);
      }
    }
  }
  for (auto Address : {uint64_t(0), UserEnd, UINT64_MAX}) {
    error(ServiceKind::OpenAt, {999, Address, 0x20000103}, 14);
    error(ServiceKind::Open, {Address, 0x20000100}, 22);
    error(ServiceKind::OpenAt, {0xfffffffe, Address, 0x20000100}, 22);
  }
  path("data");
  EXPECT_FALSE(invoke(ServiceKind::OpenAt, {0, Base, 0x20000100}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileDirectoryKind);
  Options->DescriptorLimit = 6;
  error(ServiceKind::OpenAt, {999, 0, 0x20000103}, 14);
  error(ServiceKind::Open, {0, 0x20000103}, 22);
  error(ServiceKind::Open, {0, 0x20000100}, 24);
  error(ServiceKind::OpenAt, {0xfffffffe, 0, 0x20000100}, 24);
  error(ServiceKind::OpenAt, {999, Base, 0x20000100}, 9);
  error(ServiceKind::OpenAt, {File, Base, 0x20000100}, 20);
  error(ServiceKind::OpenAt, {Dup, Base, 0x20000100}, 24);
  error(ServiceKind::OpenAt, {Dup, Base, 3}, 22);
  const auto End = Base + Page * 2 - 1;
  for (unsigned Byte : {'r', '/', '\0'}) {
    llvm::cantFail(Space->writeInteger(End, Byte, 1));
    error(ServiceKind::OpenAt, {Dup, End, 0x20000100}, 24);
    error(ServiceKind::OpenAt, {999, End, 0x20000100}, Byte == '/' ? 24 : 9);
  }
  ok(ServiceKind::Close, {File});
  path("data");
  EXPECT_EQ(ok(ServiceKind::OpenAt, {Dup, Base, 0x20000000}), File);
}

TEST_P(DarwinFileTest, NoFollowOpenAtRetainsRemovedDirectoryObject) {
  Options->MutableDirectories.insert("/");
  const auto Old = makeDirectory("/old");
  const auto Dup = ok(ServiceKind::Dup, {Old});
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto New = makeDirectory("/old");
  path("/old/data");
  const auto Fresh = ok(ServiceKind::Open, {Base, 0x202});
  ok(ServiceKind::Close, {Fresh});
  path("data");
  for (uint64_t Flags : {0x100u, 0x20000000u}) {
    error(ServiceKind::OpenAt, {Dup, Base, Flags}, 2);
    const auto Current = ok(ServiceKind::OpenAt, {New, Base, Flags});
    ok(ServiceKind::Close, {Current});
    path(".");
    const auto Retained = ok(ServiceKind::OpenAt, {Dup, Base, Flags});
    identity(Retained, "/old");
    ok(ServiceKind::Close, {Retained});
    path("data");
  }
}

TEST_P(DarwinFileTest,
       WorkingDirectorySurvivesFailuresClosureAndFDReplacement) {
  Options->Files["/tree/data"] = {'t'};
  Options->Directories.insert("/empty");
  Options->WorkingDirectory = "/tree";
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("data");
  auto File = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {File, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 't');
  error(ServiceKind::Fchdir, {File}, 20);
  error(ServiceKind::Fchdir, {999}, 9);
  path("/missing");
  error(ServiceKind::Chdir, {Base}, 2);
  path("/data");
  error(ServiceKind::Chdir, {Base}, 20);
  path(".");
  auto Dir = ok(ServiceKind::Open, {Base});
  ok(ServiceKind::Fchdir, {Dir});
  ok(ServiceKind::Close, {Dir});
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), Dir);
  path("data");
  auto StillTree = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {StillTree, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 't');
  path("/empty");
  auto Empty = ok(ServiceKind::Open, {Base});
  ok(ServiceKind::Fchdir, {Empty});
  ok(ServiceKind::Dup2, {File, Empty});
  path("data");
  error(ServiceKind::Open, {Base}, 2);
  path("..");
  ok(ServiceKind::Chdir, {Base});
  path("data");
  auto RootFile = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Read, {RootFile, Base + Page, 1}), 1u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 1)), 'a');
}

TEST_P(DarwinFileTest, DirectoryMetadataReadAndSeekUseExplicitObservations) {
  Options->Directories.insert("/empty");
  auto M = darwin_test::metadata(128);
  M.Mode = 0040700;
  Options->Metadata["/empty"] = M;
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base, value::OpenDirectory});
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Dir, value::GetFileFlags}), 0u);
  for (auto Kind : {ServiceKind::Read, ServiceKind::Pread})
    for (unsigned Count : {0, 1})
      error(Kind, {Dir, 0, Count}, 21);
  error(ServiceKind::Pread, {Dir, 0, 0, UINT64_MAX}, 22);
  EXPECT_EQ(std::get<uint32_t>(Files->mappingSource(Dir)), 22u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 7, 0}), 7u);
  auto Dup = ok(ServiceKind::Dup, {Dir});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dup, 0, 1}), 7u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 2}), 128u);
  error(ServiceKind::Lseek, {Dir, UINT64_MAX, 0}, 22);
  error(ServiceKind::Lseek, {Dir, uint64_t(-129), 1}, 22);
  error(ServiceKind::Lseek, {Dir, 0, 99}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, INT64_MAX, 0}), uint64_t(INT64_MAX));
  error(ServiceKind::Lseek, {Dir, 1, 1}, 84);
  for (unsigned Whence : {3, 4})
    EXPECT_FALSE(invoke(ServiceKind::Lseek, {Dir, 0, Whence}));
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 1}), uint64_t(INT64_MAX));
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dir, 0, Base + Page, value::AtFDOnly}),
            0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 4, 2)), 0040700u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 96, 8)), 128u);
  path("/");
  auto Root = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 7, 0}), 7u);
  EXPECT_FALSE(invoke(ServiceKind::Lseek, {Root, 0, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Root, 0, 1}), 7u);
}

TEST_P(DarwinFileTest,
       FstatAtSharesPathOwnerAndGetPathCopiesCanonicalIdentity) {
  Options->Metadata["/data"] = darwin_test::metadata(6);
  Options->Directories.insert("/empty");
  path("/empty");
  auto Dir = ok(ServiceKind::Open, {Base});
  path("..//./data");
  auto File = ok(ServiceKind::OpenAt, {Dir, Base});
  auto Dup = ok(ServiceKind::Dup, {File});
  ok(ServiceKind::Close, {File});
  const auto Buffer = Base + Page;
  for (auto Flags : {0u, 0x20u, 0x800u, 0x820u}) {
    EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dir, Base, Buffer, Flags}), 0u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 96, 8)), 6u);
  }
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {Dup, 0, Buffer, 0x400}), 0u);
  error(ServiceKind::FstatAt64, {999, 0, 0, 1}, 22);
  error(ServiceKind::FstatAt64, {999, 0, 0}, 14);
  error(ServiceKind::FstatAt64, {999, 0, 0, 0x400}, 9);
  EXPECT_FALSE(invoke(ServiceKind::FstatAt64, {Dir, Base, Buffer, 0x200}));
  path("");
  error(ServiceKind::FstatAt64, {999, Base, 0}, 9);
  error(ServiceKind::FstatAt64, {Dup, Base, 0}, 20);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {999, Base, Buffer}), 0u);
  ASSERT_FALSE(bool(Space->writeInteger(Buffer, UINT64_MAX, 8)));
  EXPECT_EQ(ok(ServiceKind::Fcntl, {Dup, 50, Buffer}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 0xffff00617461642fu);
  error(ServiceKind::Fcntl, {Dup, 50, 0}, 14);
  error(ServiceKind::Fcntl, {999, 50, 0}, 9);
  const auto End = Base + Page * 2 - 2;
  ASSERT_FALSE(bool(Space->writeInteger(End, 0xaaaa, 2)));
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {Dup, 50, End}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePartialPath);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 2)), 0xaaaau);
  EXPECT_FALSE(invoke(ServiceKind::Fcntl, {0, 50, Buffer}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FilePathIdentity);
}

TEST_P(DarwinFileTest,
       DirectoryRecordsPreserveLayoutCookiesAndIndependentOpens) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  Options->DirectoryContents["/"].Entries[2].SeekOffset = UINT64_MAX;
  path("/");
  const auto A = ok(ServiceKind::Open, {Base});
  const auto B = ok(ServiceKind::Dup, {A});
  const auto C = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  error(ServiceKind::GetDirEntries64, {A, Buffer, 63, Position}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 0u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {A, Buffer, 64, Position}), 64u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 22u);
  // Independent explicit wire bytes, including native tail padding.
  const auto Expected = llvm::fromHex(
      "2900000000000000000000000000000020000100042e00000000000000000000"
      "2900000000000000000000000000000020000200042e2e000000000000000000");
  std::array<uint8_t, 64> Records;
  ASSERT_FALSE(bool(Space->read(Buffer, Records)));
  EXPECT_EQ(llvm::toHex(Records), llvm::toHex(Expected));
  EXPECT_EQ(ok(ServiceKind::Close, {A}), 0u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 32, Position}), 32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 8, 8)), UINT64_MAX);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 22u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}),
            7u); // Cookies need not increase.
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 32, Position}), 32u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)),
            0xfedcba9876543210ULL);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 99u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, 0, 1, Position}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 99u);
  error(ServiceKind::GetDirEntries64, {B, 0, 0, Position}, 22);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {C, Buffer, 128, Position}), 128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  ok(ServiceKind::Lseek, {B, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {B, Buffer, 128, Position}), 128u);
  ok(ServiceKind::Lseek, {B, 88, 0});
  EXPECT_FALSE(
      invoke(ServiceKind::GetDirEntries64, {B, Buffer, 128, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPosition);
  EXPECT_EQ(ok(ServiceKind::Lseek, {B, 0, 1}), 88u);
}

TEST_P(DarwinFileTest, DirectoryEOFAndExtendedFlagsRespectOriginalBufferEnd) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  path("/");
  auto FD = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  for (uint64_t Count : {1023, 1024, 4096}) {
    ok(ServiceKind::Lseek, {FD, 0, 0});
    ASSERT_FALSE(bool(Space->writeInteger(Buffer + Count - 4, 0xaaaaaaaa, 4)));
    EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, Count, Position}),
              128u);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + Count - 4, 4)),
              Count < 1024 ? 0xaaaaaaaau : 1u);
  }
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, 0, 64, Position}), 0u);
  error(ServiceKind::GetDirEntries64, {FD, 0, 1024, Position}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 99u);
  // Count is capped for record payload, but its unsigned suffix address wraps.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(Buffer - 5, 0xaaaaaaaa, 4)));
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {FD, Buffer, UINT64_MAX, Position}),
      128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer - 5, 4)), 1u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // Extended output must explicitly clear EOF when a whole-record batch is
  // shorter than the remaining snapshot. The next call resumes at its cookie.
  for (unsigned I = 0; I != 4; ++I) {
    std::string Name(255, 'x');
    Name.back() = 'a' + I;
    Options->Files['/' + Name] = {};
    Options->DirectoryContents["/"].Entries.push_back(
        {Name, 50 + I, 8, 100 + I, 0});
  }
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Position}),
            968u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 1020, 4)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 102u);
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Position}),
            280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 102u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer + 1020, 4)), 1u);
}

TEST_P(DarwinFileTest, DirectoryCopyPhasesRetainEarlierEffectsAndAliasInOrder) {
  Options->Directories.insert("/empty");
  Options->DirectoryContents["/"] = darwin_test::directoryContents();
  path("/");
  auto FD = ok(ServiceKind::Open, {Base});
  const auto Buffer = Base + 256, Position = Base + 2048;
  error(ServiceKind::GetDirEntries64, {999, 0, 0, 0}, 9);
  error(ServiceKind::GetDirEntries64, {FD, 0, 128, Position}, 14);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  error(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, 0}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 41u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // A failing suffix occurs after data, offset advancement and position copy.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(Position, 0xaaaaaaaa, 8)));
  error(ServiceKind::GetDirEntries64, {FD, Buffer, Page * 4, Position}, 14);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  // With overlapping outputs, position overwrites data and flags overwrite it
  // last.
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 128, Buffer}), 128u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Buffer, 8)), 0u);
  const auto Suffix = Buffer + 1020;
  EXPECT_EQ(ok(ServiceKind::GetDirEntries64, {FD, Buffer, 1024, Suffix}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Suffix, 8)), 1u);
  const auto End = Base + Page * 2 - 4;
  ok(ServiceKind::Lseek, {FD, 0, 0});
  ASSERT_FALSE(bool(Space->writeInteger(End, 0xbbbbbbbb, 4)));
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64, {FD, End, 128, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialData);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 4)), 0xbbbbbbbbu);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64, {FD, Buffer, 128, End}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialPosition);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End, 4)), 0xbbbbbbbbu);
  // Position succeeded before an individually partial extended-flag copy.
  const auto PartialCount = Base + Page * 2 - 2 - Buffer + 4;
  ok(ServiceKind::Lseek, {FD, 0, 0});
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {FD, Buffer, PartialCount, Position}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryPartialFlags);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Position, 8)), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {FD, 0, 1}), 99u);
}

TEST_P(DarwinFileTest, DirectoryLongRecordsAndMissingObservationsStayExplicit) {
  auto File = ok(ServiceKind::Open, {Base});
  error(ServiceKind::GetDirEntries64, {File, 0, 0, 0}, 22);
  path("/");
  auto Dir = ok(ServiceKind::Open, {Base});
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Dir, Base + 256, 1024, Base + 2048}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryContents);
  Options->Directories.insert("/empty");
  const std::string Name(255, 'x');
  Options->Files['/' + Name] = {};
  auto &C = Options->DirectoryContents["/"];
  C = darwin_test::directoryContents();
  C.Entries.push_back({Name, 43, 8, 100, 0});
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/");
  Dir = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dir, Base + 256, 128, Base + 2048}),
      128u);
  error(ServiceKind::GetDirEntries64, {Dir, Base + 256, 279, Base + 2048}, 22);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Dir, 0, 1}), 99u);
  EXPECT_EQ(
      ok(ServiceKind::GetDirEntries64, {Dir, Base + 256, 280, Base + 2048}),
      280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 16, 2)), 280u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 18, 2)), 255u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 276, 4)), 0u);
}

TEST(DarwinFileOptions,
     DirectorySnapshotAdmissionRejectsIncoherentObservations) {
  DarwinFileOptions Original;
  Original.Files["/data"] = {};
  Original.Directories.insert("/empty");
  Original.DirectoryContents["/"] = darwin_test::directoryContents();
  Original.DirectoryContents["/empty"] = {
      {{".", 42, 4, 11, 0}, {"..", 41, 4, 22, 0}}, 1};
  ASSERT_FALSE(bool(
      validateFileOptions(Original))); // Cookies are local to each directory.
  for (unsigned Case = 0; Case != 17; ++Case) {
    auto O = Original;
    auto &C = O.DirectoryContents["/"];
    switch (Case) {
    case 0:
      C.MinimumBufferSize = 0;
      break;
    case 1:
      C.MinimumBufferSize = 128 * 1024 * 1024 + 1;
      break;
    case 2:
      C.Entries[0].Inode = 0;
      break;
    case 3:
      C.Entries[0].NextOffset = 0;
      break;
    case 4:
      C.Entries[0].NextOffset = uint64_t(INT64_MAX) + 1;
      break;
    case 5:
      C.Entries[0].NextOffset = C.Entries[1].NextOffset;
      break;
    case 6:
      C.Entries[0].Type = 8;
      break;
    case 7:
      C.Entries[2].Type = 10;
      break;
    case 8:
      C.Entries[1].Name = ".";
      break;
    case 9:
      C.Entries[2].Name = "absent";
      break;
    case 10:
      C.Entries[2].Name = "empty/child";
      break;
    case 11:
      C.Entries[2].Name = std::string(256, 'a');
      break;
    case 12:
      C.Entries[2].Name = std::string("bad\0name", 8);
      break;
    case 13:
      C.Entries[0].Inode = 43;
      break; // Root . and .. name the same object.
    case 14:
      O.DirectoryContents["/empty"].Entries[0].Inode = 43;
      break;
    case 15:
      C.Entries.pop_back();
      break;
    case 16:
      C.Entries[0].MinimumBufferSize = UINT32_MAX;
      break;
    }
    SCOPED_TRACE(Case);
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryContentsOption);
  }
  auto O = Original;
  O.Metadata["/data"] = darwin_test::metadata(0);
  ASSERT_FALSE(bool(validateFileOptions(O)));
  ++O.Metadata["/data"].Inode;
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::DirectoryContentsOption);
  O = Original;
  O.DirectoryContents["/"].Entries[2].Type = 0;
  O.DirectoryContents["/"].Entries[2].SeekOffset = UINT64_MAX;
  ASSERT_FALSE(bool(validateFileOptions(O)));
  for (auto Bad : {"/data", "/absent", "relative"}) {
    O.DirectoryContents[Bad] = {};
    EXPECT_EQ(llvm::toString(validateFileOptions(O)),
              diagnostic::DirectoryContentsOption);
    O.DirectoryContents.erase(Bad);
  }
}

TEST(DarwinFileOptions, DirectorySnapshotBoundsDeduplicateMetadataPaths) {
  DarwinFileOptions O;
  for (unsigned I = 0; I != 254; ++I)
    O.Directories.insert("/dir" + std::to_string(I));
  O.Files["/tree/data"] = {};
  auto M = darwin_test::metadata(128);
  M.Mode = 0040755;
  M.Inode = 41;
  O.Metadata["/tree"] = M;
  auto &C = O.DirectoryContents["/tree"];
  C = {{{".", 41, 4, 1, 0}, {"..", 40, 4, 2, 0}, {"data", 42, 8, 3, 0}}, 1};
  ASSERT_FALSE(
      bool(validateFileOptions(O))); // 255 nodes + shared implicit /tree = 256.
  O.Directories.insert("/extra");
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.Directories.insert("/empty");
  O.Files["/data"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 143);
  O.DirectoryContents["/"] = darwin_test::directoryContents();
  // /data NUL=6, /empty NUL=7, implicit root key=2, four records=128.
  ASSERT_FALSE(bool(validateFileOptions(O)));
  O.Files["/data"].push_back(0);
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
  O = {};
  O.DirectoryContents["/"] = {};
  O.DirectoryContents["/"].MinimumBufferSize = 1;
  O.DirectoryContents["/"].Entries.resize(darwin_file_limits::DirectoryEntries +
                                          1);
  EXPECT_EQ(llvm::toString(validateFileOptions(O)),
            diagnostic::FileOptionsLimit);
}

TEST_P(DarwinFileTest, CreationUsesSelectedEffectiveUIDAndRetainsParentGroup) {
  for (auto UID :
       {uint32_t(1000), uint32_t(0), uint32_t(7), uint32_t(INT32_MAX)}) {
    SCOPED_TRACE(UID);
    std::optional<DarwinSystemOptions> System;
    if (UID != 1000) {
      System.emplace();
      System->Credentials = DarwinCredentials{101, UID, 303, 404, std::nullopt};
    }
    creationPolicy();
    Files = std::make_unique<DarwinFiles>(
        *Space, Options, process_defaults::Output,
        credentialID(ServiceKind::GetEUID, System));
    const auto First = makeFile("/new", "a");
    const auto Original = status(First);
    EXPECT_EQ(llvm::support::endian::read32le(Original.data() + 16), UID);
    EXPECT_EQ(llvm::support::endian::read32le(Original.data() + 20),
              0xfedcba98u);
    renameFile("/new", "/renamed");
    EXPECT_EQ(status(First), Original);
    identity(First, "/renamed");
    const auto Fresh = makeFile("/new", "b");
    const auto NewStatus = status(Fresh);
    EXPECT_EQ(llvm::support::endian::read32le(NewStatus.data() + 16), UID);
    EXPECT_EQ(llvm::support::endian::read32le(NewStatus.data() + 20),
              0xfedcba98u);
    EXPECT_NE(llvm::support::endian::read64le(NewStatus.data() + 8),
              llvm::support::endian::read64le(Original.data() + 8));
    EXPECT_EQ(status(First), Original);
    contents(First, {'a'});
    contents(Fresh, {'b'});
    EXPECT_EQ(Options->Metadata.at("/").GID, 0xfedcba98u);
    EXPECT_EQ(Options->CreationPolicy->FirstInode, 0xfedcba9876543211ULL);
    if (System) {
      EXPECT_EQ(System->Credentials->RealUID, 101u);
      EXPECT_EQ(System->Credentials->EffectiveUID, UID);
      EXPECT_EQ(System->Credentials->EffectiveGID, 404u);
      EXPECT_FALSE(System->Credentials->GroupAccessList);
    }
  }
}

TEST_P(DarwinFileTest, ExplicitRootDoesNotGrantFileOrDirectoryMutation) {
  creationPolicy();
  Options->MutableDirectories.clear();
  std::optional<DarwinSystemOptions> System = DarwinSystemOptions{};
  System->Credentials = DarwinCredentials{7, 0, 9, 0, std::vector<uint32_t>{0}};
  Files =
      std::make_unique<DarwinFiles>(*Space, Options, process_defaults::Output,
                                    credentialID(ServiceKind::GetEUID, System));
  path("/new");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/data");
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 2}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileNotWritable);
  EXPECT_TRUE(Options->MutableDirectories.empty());
  EXPECT_TRUE(Options->WritableFiles.empty());
  EXPECT_EQ(Options->Files.at("/data"),
            (std::vector<uint8_t>{'a', 'b', 0, 0xff, 'e', 'f'}));
}

TEST(DarwinFileOptions,
     SymbolicLinksRequireRawTargetsAndDistinctCanonicalNames) {
  DarwinFileOptions Good;
  Good.Files["/data"] = {'x'};
  Good.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  auto Refused = [](const DarwinFileOptions &O) {
    auto E = validateFileOptions(O);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  };
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (const auto &Target :
       {std::vector<uint8_t>{}, std::vector<uint8_t>{'a', 0, 'b'},
        std::vector<uint8_t>(1024, 'x')}) {
    auto Bad = Good;
    Bad.SymbolicLinks["/link"] = Target;
    Refused(Bad);
  }
  for (const char *Name : {"link", "/", "/link/", "/a/../link", "/a//link"}) {
    auto Bad = Good;
    Bad.SymbolicLinks[Name] = {'x'};
    Refused(Bad);
  }
  for (unsigned Collision = 0; Collision != 5; ++Collision) {
    auto Bad = Good;
    if (Collision == 0)
      Bad.Files["/link"] = {};
    else if (Collision == 1)
      Bad.Directories.insert("/link");
    else if (Collision == 2)
      Bad.Files["/link/child"] = {};
    else if (Collision == 3)
      Bad.Directories.insert("/link/child");
    else
      Bad.SymbolicLinks["/link/child"] = {'x'};
    Refused(Bad);
  }
  Good.SymbolicLinks["/link"] = std::vector<uint8_t>(1023, 0xff);
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  Good.WorkingDirectory = "/link";
  Refused(Good);
}

TEST(DarwinFileOptions, SymbolicLinksExcludeNamespaceGrantsButPermitFileBytes) {
  DarwinFileOptions Good;
  Good.Files["/data"] = {'x'};
  Good.WritableFiles.insert("/data");
  Good.Metadata["/data"] = darwin_test::mutationMetadata(1);
  Good.MutationPolicies["/data"] = darwin_test::MutationPolicy;
  Good.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (unsigned Grant = 0; Grant != 6; ++Grant) {
    auto Bad = Good;
    Bad.Directories = {"/", "/elsewhere"};
    Bad.MutableDirectories.insert("/");
    if (Grant == 1)
      Bad.RemovableDirectories.insert("/elsewhere");
    else if (Grant == 2)
      Bad.MovableDirectories.insert("/elsewhere");
    else if (Grant == 3)
      Bad.ExchangeableDirectories.insert("/elsewhere");
    else if (Grant == 4)
      Bad.SwapRenameDirectories.insert("/");
    else if (Grant == 5) {
      Bad.CreationPolicy = darwin_test::CreationPolicy;
      Bad.InitialUmask = 0027;
      Bad.Metadata["/"] = darwin_test::creationParentMetadata();
    }
    auto WithoutLinks = Bad;
    WithoutLinks.SymbolicLinks.clear();
    EXPECT_FALSE(bool(validateFileOptions(WithoutLinks)));
    EXPECT_EQ(llvm::toString(validateFileOptions(Bad)),
              diagnostic::SymbolicLinkNamespace);
  }
}

TEST(DarwinFileOptions, FixedLinksAllowAllSeparateNamespaceGrants) {
  for (unsigned Grants = 0; Grants != 64; ++Grants) {
    SCOPED_TRACE(Grants);
    auto Good = darwin_test::mixedSymbolicLinkOptions();
    Good.Directories.insert("/work/child");
    Good.Metadata["/work/child"] = darwin_test::creationParentMetadata();
    Good.Metadata["/work/child"].Inode = 42;
    Good.CreationPolicy.reset();
    if (Grants & 1)
      Good.MutableDirectories.insert("/work/child");
    if (Grants & 2)
      Good.RemovableDirectories.insert("/work/child");
    if (Grants & 4)
      Good.MovableDirectories.insert("/work/child");
    if (Grants & 8)
      Good.ExchangeableDirectories.insert("/work/child");
    if (Grants & 16)
      Good.SwapRenameDirectories.insert("/work");
    if (Grants & 32)
      Good.CreationPolicy = darwin_test::CreationPolicy;
    EXPECT_FALSE(bool(validateFileOptions(Good)));
    Good.SymbolicLinks["/work/child/protected"] = {'x'};
    EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
              diagnostic::SymbolicLinkNamespace);
  }
}

TEST(DarwinFileOptions, FixedLinkProtectionUsesNamesAndDirectorySegments) {
  DarwinFileOptions Good;
  Good.Directories = {"/work", "/static/work"};
  Good.MutableDirectories = Good.Directories;
  Good.SymbolicLinks = {{"/workspace/link", {'/', 'w', 'o', 'r', 'k'}},
                        {"/static/link", {'w', 'o', 'r', 'k'}}};
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (const char *Ancestor : {"/", "/static", "/workspace"}) {
    auto Bad = Good;
    Bad.MutableDirectories.insert(Ancestor);
    EXPECT_EQ(llvm::toString(validateFileOptions(Bad)),
              diagnostic::SymbolicLinkNamespace);
  }
}

TEST(DarwinFileOptions, CreationIdentityIncludesProtectedLinkObservations) {
  auto Good = darwin_test::mixedSymbolicLinkOptions();
  Good.Metadata["/work/data"].Inode = 17;
  Good.CreationPolicy->FirstInode = 124;
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  Good.CreationPolicy->FirstInode = 123;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileCreationPolicy);
  Good.DirectoryContents["/static"] = {{{".", 50, 4, 1},
                                        {"..", 1, 4, 2},
                                        {"alias", 124, 10, 3},
                                        {"data-link", 123, 10, 4},
                                        {"missing-link", 1000, 10, 5}},
                                       1};
  Good.CreationPolicy->FirstInode = 1001;
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  Good.CreationPolicy->FirstInode = 1000;
  EXPECT_EQ(llvm::toString(validateFileOptions(Good)),
            diagnostic::FileCreationPolicy);
}

TEST(DarwinFileOptions, SymbolicLinkMetadataAndSnapshotsObserveTheLinkObject) {
  DarwinFileOptions Good;
  Good.SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  auto &M = Good.Metadata["/link"];
  M = darwin_test::metadata(4);
  M.Mode = 0120777;
  Good.DirectoryContents["/"] = {
      {{".", 41, 4, 1}, {"..", 41, 4, 2}, {"link", M.Inode, 10, 3}}, 1};
  EXPECT_FALSE(bool(validateFileOptions(Good)));
  for (unsigned BadField = 0; BadField != 5; ++BadField) {
    auto Bad = Good;
    if (BadField == 0)
      Bad.Metadata["/link"].Size = 5;
    else if (BadField == 1)
      Bad.Metadata["/link"].Mode = 0100777;
    else if (BadField == 2)
      Bad.DirectoryContents["/"].Entries.back().Type = 0;
    else if (BadField == 3)
      Bad.DirectoryContents["/"].Entries.back().Inode++;
    else
      Bad.DirectoryContents["/"].Entries.pop_back();
    auto E = validateFileOptions(Bad);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
}

TEST(DarwinFileOptions, SymbolicLinkNamesAndTargetsShareEntryAndByteLimits) {
  DarwinFileOptions O;
  O.SymbolicLinks["/l"] = {'x'}; // 3 name bytes including NUL, one target byte.
  O.Files["/a"] = std::vector<uint8_t>(darwin_file_limits::Bytes - 7);
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.Files["/a"].push_back(0);
  auto E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  O = {};
  for (unsigned I = 0; I != 255; ++I)
    O.Files["/f" + std::to_string(I)] = {};
  O.SymbolicLinks["/l"] = {'x'};
  EXPECT_FALSE(bool(validateFileOptions(O)));
  O.SymbolicLinks["/extra"] = {'x'};
  E = validateFileOptions(O);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}

TEST_P(DarwinFileTest, SymbolicLinkExpansionUsesActualParentsAndNewRootPolicy) {
  Options->WorkingDirectory = "/";
  Options->Files["/dir/item"] = {'i'};
  Options->SymbolicLinks = {
      {"/relative", {'d', 'a', 't', 'a'}},
      {"/absolute", {'/', 'd', 'a', 't', 'a'}},
      {"/dir/parent", {'.', '.', '/', 'd', 'a', 't', 'a'}},
      {"/chain", {'r', 'e', 'l', 'a', 't', 'i', 'v', 'e'}},
      {"/dirlink", {'d', 'i', 'r'}}};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  for (const char *Name :
       {"/relative", "/absolute", "relative", "absolute", "/dir/parent",
        "/chain", "/dirlink/../relative", "/dirlink/parent"}) {
    SCOPED_TRACE(Name);
    path(Name);
    const auto FD = ok(ServiceKind::Open, {Base});
    contents(FD, Options->Files.at("/data"));
    identity(FD, "/data");
    ok(ServiceKind::Close, {FD});
  }
  path("/dirlink");
  const auto Dir = ok(ServiceKind::Open, {Base});
  identity(Dir, "/dir");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  path("parent");
  const auto FD = ok(ServiceKind::OpenAt, {Dir, Base});
  contents(FD, Options->Files.at("/data"));
  identity(FD, "/data");
  ok(ServiceKind::Close, {FD});
  path("item");
  const auto Item = ok(ServiceKind::Open, {Base});
  contents(Item, Options->Files.at("/dir/item"));
}

TEST_P(DarwinFileTest, SymbolicLinkTrailingStateIsReparsedAfterEveryExpansion) {
  Options->WorkingDirectory = "/";
  Options->SymbolicLinks = {
      {"/relative", {'d', 'a', 't', 'a'}},
      {"/chain", {'r', 'e', 'l', 'a', 't', 'i', 'v', 'e'}},
      {"/own-slash", {'d', 'a', 't', 'a', '/'}},
      {"/cycle-slash",
       {'c', 'y', 'c', 'l', 'e', '-', 's', 'l', 'a', 's', 'h', '/'}}};
  path("/relative////");
  const auto FD = ok(ServiceKind::Open, {Base, 0x100});
  contents(FD, Options->Files.at("/data"));
  ok(ServiceKind::Close, {FD});
  error(ServiceKind::ReadLink, {Base, Base + Page, 32}, 22);
  path("/chain////");
  error(ServiceKind::Open, {Base, 0x100}, 62);
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 4u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page, 4)), 0x61746164u);
  path("/own-slash");
  error(ServiceKind::Open, {Base}, 20);
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 5u);
  path("/cycle-slash/");
  error(ServiceKind::ReadLink, {Base, Base + Page, 32}, 62);
}

TEST_P(DarwinFileTest,
       SymbolicLinksKeepTerminalRetentionSeparateFromNoExpansion) {
  Options->WorkingDirectory = "/";
  Options->SymbolicLinks = {{"/link", {'d', 'a', 't', 'a'}},
                            {"/dangling", {'a', 'b', 's', 'e', 'n', 't'}},
                            {"/cycle", {'c', 'y', 'c', 'l', 'e'}},
                            {"/dirlink", {'/'}}};
  for (const char *Name : {"/link", "/dangling", "/cycle", "/dirlink"}) {
    path(Name);
    for (uint32_t Flags : {0x100u, 0x20000000u})
      error(ServiceKind::Open, {Base, Flags}, 62);
    for (uint32_t Flags : {0u, 0x100u, 0x20000000u})
      error(ServiceKind::Open, {Base, Flags | 0xa00}, 17);
    error(ServiceKind::Open, {Base, 0x100100}, 20);
    for (uint32_t Flags : {0x20u, 0x800u, 0x820u})
      EXPECT_EQ(ok(ServiceKind::FaccessAt, {999, Base, 0, Flags}), 0u);
  }
  path("/dirlink/data");
  error(ServiceKind::FaccessAt, {999, Base, 0, 0x800}, 62);
  error(ServiceKind::UnlinkAt, {999, Base, 0x800}, 62);
  path("/link");
  EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  path("/data", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base + 128, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  error(ServiceKind::Mkdir, {Base, 0700}, 17);
  error(ServiceKind::Rmdir, {Base}, 20);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
}

TEST_P(DarwinFileTest,
       SymbolicLinkStatDistinguishesOwnMetadataAndFDOnlyTarget) {
  Options->WorkingDirectory = "/";
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  Options->Metadata["/data"] = darwin_test::metadata(6);
  auto &Link = Options->Metadata["/link"];
  Link = darwin_test::metadata(4);
  Link.Inode = 123;
  Link.Mode = 0120777;
  path("/link");
  const auto FD = ok(ServiceKind::Open, {Base});
  const auto Target = status(FD);
  EXPECT_EQ(ok(ServiceKind::Stat64, {Base, Base + 256}), 0u);
  std::array<uint8_t, 144> Bytes;
  llvm::cantFail(Space->read(Base + 256, Bytes));
  EXPECT_EQ(Bytes, Target);
  for (uint32_t Flags : {0x20u, 0x800u, 0x820u}) {
    EXPECT_EQ(ok(ServiceKind::FstatAt64, {999, Base, Base + 256, Flags}), 0u);
    llvm::cantFail(Space->read(Base + 256, Bytes));
    EXPECT_EQ(llvm::support::endian::read16le(Bytes.data() + 4), 0120777u);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 8), 123u);
    EXPECT_EQ(llvm::support::endian::read64le(Bytes.data() + 96), 4u);
  }
  EXPECT_EQ(ok(ServiceKind::Lstat64, {Base, Base + 256}), 0u);
  error(ServiceKind::FstatAt64, {999, 0, Base + 256, 0xc20}, 9);
  error(ServiceKind::FstatAt64, {FD, 0, Base + 256, 0xc21}, 22);
  EXPECT_EQ(ok(ServiceKind::FstatAt64, {FD, 0, Base + 256, 0xc20}), 0u);
  llvm::cantFail(Space->read(Base + 256, Bytes));
  EXPECT_EQ(Bytes, Target);
  Options->Metadata.erase("/link");
  // Changed initial catalogue input requires a fresh namespace owner.
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  EXPECT_FALSE(invoke(ServiceKind::Lstat64, {Base, Base + 256}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
}

TEST_P(DarwinFileTest, ReadLinkPreservesRawBytesCountsAndEveryAdjacentCanary) {
  const std::vector<uint8_t> Raw = {'.', '/', '/', 0xff, '/'};
  Options->SymbolicLinks["/raw"] = Raw;
  path("/raw");
  for (auto Count :
       {uint64_t(0), uint64_t(2), uint64_t(5), uint64_t(INT32_MAX)}) {
    std::array<uint8_t, 32> Expected;
    Expected.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, Expected));
    const auto Copy = std::min<uint64_t>(Count, Raw.size());
    EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page + 8, Count}), Copy);
    std::copy_n(Raw.begin(), Copy, Expected.begin() + 8);
    std::array<uint8_t, 32> Actual;
    llvm::cantFail(Space->read(Base + Page, Actual));
    EXPECT_EQ(Actual, Expected);
  }
  EXPECT_EQ(
      ok(ServiceKind::ReadLink, {Base, Base + Page, 0x1234567800000002ULL}),
      2u);
  error(ServiceKind::ReadLinkAt, {999, Base, Base + Page, 0x100000002ULL}, 22);
  error(ServiceKind::ReadLink, {0, 0, uint64_t(INT32_MAX) + 1}, 22);
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, UINT64_MAX, 0}), 0u);
  path("/data");
  error(ServiceKind::ReadLink, {Base, 0, 0}, 22);
  path("/absent");
  error(ServiceKind::ReadLink, {Base, 0, 0}, 2);
  error(ServiceKind::ReadLink, {0, 0, 0}, 14);
}

TEST_P(DarwinFileTest,
       ReadLinkChecksOnlyItsActualPrefixAndRefusesPartialCopies) {
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  path("/link");
  const auto End = Base + Page * 2;
  EXPECT_EQ(ok(ServiceKind::ReadLinkAt, {999, Base, End - 4, INT32_MAX}), 4u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End - 4, 4)), 0x61746164u);
  llvm::cantFail(Space->writeInteger(End - 2, 0xa5a5, 2));
  EXPECT_FALSE(invoke(ServiceKind::ReadLink, {Base, End - 2, 4}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkPartialOutput);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(End - 2, 2)), 0xa5a5u);
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, End - 2, 2}), 2u);
  error(ServiceKind::ReadLink, {Base, End, 1}, 14);
  llvm::cantFail(Space->protect(Base + Page, Page, Read | UserAccessible));
  error(ServiceKind::ReadLink, {Base, Base + Page, 1}, 14);
}

TEST_P(DarwinFileTest, ReadLinkAtUsesDirectoryObjectsAndAbsolutePathsIgnoreFD) {
  Options->SymbolicLinks["/dir/link"] = {'d', 'a', 't', 'a'};
  path("/dir");
  const auto Dir = ok(ServiceKind::Open, {Base});
  path("link");
  EXPECT_EQ(ok(ServiceKind::ReadLinkAt, {Dir, Base, Base + Page, 4}), 4u);
  error(ServiceKind::ReadLinkAt, {999, Base, Base + Page, 4}, 9);
  path("/data");
  const auto FD = ok(ServiceKind::Open, {Base});
  path("link");
  error(ServiceKind::ReadLinkAt, {FD, Base, Base + Page, 4}, 20);
  path("/dir/link");
  EXPECT_EQ(ok(ServiceKind::ReadLinkAt, {999, Base, Base + Page, 4}), 4u);
}

TEST_P(DarwinFileTest,
       SymbolicLinksEnforceExpansionAndTerminatorInclusiveLimits) {
  Options->WorkingDirectory = "/";
  for (unsigned I = 0; I != 33; ++I) {
    const auto Target = I == 32 ? "/data" : "/l" + std::to_string(I + 1);
    Options->SymbolicLinks["/l" + std::to_string(I)] =
        std::vector<uint8_t>(Target.begin(), Target.end());
  }
  path("/l1");
  const auto FD = ok(ServiceKind::Open, {Base});
  contents(FD, Options->Files.at("/data"));
  ok(ServiceKind::Close, {FD});
  path("/l0");
  error(ServiceKind::Open, {Base}, 62);
  std::string Prefix;
  for (unsigned I = 0; I != 510; ++I)
    Prefix += "./";
  const auto Fits = Prefix + 'd', TooLong = Prefix + "dd";
  Options->Files["/d/e"] = {'x'};
  Options->Files["/dd/e"] = {'y'};
  Options->SymbolicLinks["/fits"] =
      std::vector<uint8_t>(Fits.begin(), Fits.end());
  Options->SymbolicLinks["/too-long"] =
      std::vector<uint8_t>(TooLong.begin(), TooLong.end());
  // Added catalogue input requires a new namespace owner, not live mutation.
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  for (const char *Name : {"/fits/e", "/fits//e", "/fits////e"}) {
    path(Name);
    const auto FitsFD = ok(ServiceKind::Open, {Base});
    contents(FitsFD, Options->Files.at("/d/e"));
    ok(ServiceKind::Close, {FitsFD});
  }
  for (const char *Name : {"/too-long/e", "/too-long////e"}) {
    path(Name);
    error(ServiceKind::Open, {Base}, 63);
  }
}

TEST_P(DarwinFileTest, AtPathPrefixPrecedesLaterFaultsAcrossTheSharedResolver) {
  path("/");
  const auto Dir = ok(ServiceKind::Open, {Base});
  path("/data");
  const auto FD = ok(ServiceKind::Open, {Base});
  const auto End = Base + Page * 2;
  llvm::cantFail(Space->write(End - 1, {'x'}));
  for (auto Kind : {ServiceKind::ReadLinkAt, ServiceKind::FstatAt64,
                    ServiceKind::FaccessAt}) {
    const auto Output = Kind == ServiceKind::FaccessAt ? 0 : Base + Page;
    error(Kind, {999, End - 1, Output, 0}, 9);
    error(Kind, {FD, End - 1, Output, 0}, 20);
    error(Kind, {Dir, End - 1, Output, 0}, 14);
    error(Kind, {uint32_t(-2), End - 1, Output, 0}, 14);
    error(Kind, {999, End, Output, 0}, 14);
    llvm::cantFail(Space->write(End - 1, {'/'}));
    error(Kind, {999, End - 1, Output, 0}, 14);
    llvm::cantFail(Space->write(End - 1, {'x'}));
  }
  error(ServiceKind::ReadLinkAt, {999, End, 0, uint64_t(INT32_MAX) + 1}, 22);
  error(ServiceKind::FstatAt64, {999, End, 0, 1}, 22);
  error(ServiceKind::FaccessAt, {999, End, 0, 1}, 22);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 5u);
}

TEST_P(DarwinFileTest,
       SymbolicLinkFileMutationKeepsTargetOwnershipAndLinkStatus) {
  mutationPolicy();
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  auto &Link = Options->Metadata["/link"];
  Link = darwin_test::metadata(4);
  Link.Mode = 0120777;
  Link.Inode = 123;
  Options->WorkingDirectory = "/";
  path("/link");
  const auto Alias = ok(ServiceKind::Open, {Base, 2});
  path("/data");
  const auto Direct = ok(ServiceKind::Open, {Base});
  path("X", Base + Page);
  EXPECT_EQ(ok(ServiceKind::Pwrite, {Alias, Base + Page, 1, 0}), 1u);
  const uint8_t Expected[] = {'X', 'b', 0, 0xff, 'e', 'f'};
  contents(Direct, Expected);
  identity(Alias, "/data");
  path("/link");
  EXPECT_EQ(ok(ServiceKind::Truncate, {Base, 2}), 0u);
  contents(Direct, llvm::ArrayRef<uint8_t>(Expected).take_front(2));
  EXPECT_EQ(ok(ServiceKind::Lstat64, {Base, Base + 256}), 0u);
  EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + 256 + 96, 8)), 4u);
  EXPECT_EQ(Options->Files.at("/data"),
            (std::vector<uint8_t>{'a', 'b', 0, 0xff, 'e', 'f'}));
}

TEST_P(DarwinFileTest,
       ReadLinkTransportFailuresRemainErrorsWithoutFDOrByteEffects) {
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  path("/link");
  for (bool Preflight : {false, true}) {
    Input.FailAccess = Preflight;
    Input.FailRead = !Preflight;
    Input.FailureAddress = Base + 2;
    auto Failed =
        Files->handle(ServiceKind::ReadLink,
                      {0, 58, {Base, Base + Page, 4}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    EXPECT_EQ(llvm::toString(Failed.takeError()),
              Preflight ? "file input preflight failed"
                        : "file input transport failed");
    Input.FailAccess = Input.FailRead = false;
  }
  Input.FailAccess = true;
  Input.FailureAddress = Base + Page;
  auto Failed =
      Files->handle(ServiceKind::ReadLink,
                    {0, 58, {Base, Base + Page, 4}, std::nullopt}, Result);
  ASSERT_FALSE(bool(Failed));
  EXPECT_EQ(llvm::toString(Failed.takeError()), "file input preflight failed");
  Input.FailAccess = false;
  Input.FailWrite = true;
  Failed = Files->handle(ServiceKind::ReadLink,
                         {0, 58, {Base, Base + Page, 4}, std::nullopt}, Result);
  ASSERT_FALSE(bool(Failed));
  EXPECT_EQ(llvm::toString(Failed.takeError()), "file output transport failed");
  Input.FailWrite = false;
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 4}), 4u);
  path("/data");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
}

TEST_P(DarwinFileTest, ReadLinkMemoryBudgetErrorsKeepTheirTypeAndCanaryBytes) {
  Options->SymbolicLinks["/link"] = {'d', 'a', 't', 'a'};
  path("/link");
  for (unsigned Phase = 0; Phase != 3; ++Phase) {
    std::array<uint8_t, 32> Before, After;
    Before.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, Before));
    FailingFileInput Input(*Space);
    Input.FailBudget = true;
    Input.FailRead = Phase == 0;
    Input.FailAccess = Phase == 1;
    Input.FailWrite = Phase == 2;
    Input.FailureAddress = Phase == 0 ? Base + 2 : Base + Page;
    DarwinFiles Owner(Input, Options);
    auto Failed =
        Owner.handle(ServiceKind::ReadLink,
                     {0, 58, {Base, Base + Page + 8, 4}, std::nullopt}, Result);
    ASSERT_FALSE(bool(Failed));
    auto E = Failed.takeError();
    EXPECT_TRUE(E.isA<GuestMemoryLimitError>());
    llvm::consumeError(std::move(E));
    llvm::cantFail(Space->read(Base + Page, After));
    EXPECT_EQ(After, Before);
  }
}

TEST_P(DarwinFileTest, FixedLinksObserveCreatedMovedAndReplacedTargets) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  auto M = darwin_test::creationParentMetadata();
  M.Inode = 50;
  Options->Metadata["/static"] = M;
  Options->DirectoryContents["/static"] = {{{".", 50, 4, 11, 0},
                                            {"..", 1, 4, 22, 0},
                                            {"alias", 124, 10, 33, 0},
                                            {"data-link", 123, 10, 44, 0},
                                            {"missing-link", 125, 10, 55, 0}},
                                           1};
  Options->DirectoryContents["/work"] = {
      {{".", 41, 4, 11, 0},
       {"..", 1, 4, 22, 0},
       {"data", darwin_test::CreationPolicy.FirstInode - 1, 8, 33, 0}},
      1};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  path("/static");
  const auto Protected = ok(ServiceKind::Open, {Base});
  const auto ProtectedDuplicate = ok(ServiceKind::Dup, {Protected});
  const auto ProtectedStatus = status(Protected);
  auto Snapshot = [&](uint64_t FD, uint64_t Size) {
    std::array<uint8_t, 272> Bytes;
    Bytes.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, Bytes));
    llvm::cantFail(Space->writeInteger(Base + Page + 512, UINT64_MAX, 8));
    EXPECT_EQ(ok(ServiceKind::GetDirEntries64,
                 {FD, Base + Page + 8, 256, Base + Page + 512}),
              Size);
    EXPECT_EQ(llvm::cantFail(Space->readInteger(Base + Page + 512, 8)), 0u);
    llvm::cantFail(Space->read(Base + Page, Bytes));
    EXPECT_TRUE(std::all_of(Bytes.begin(), Bytes.begin() + 8,
                            [](uint8_t B) { return B == 0xa5; }));
    EXPECT_TRUE(std::all_of(Bytes.begin() + 8 + Size, Bytes.end(),
                            [](uint8_t B) { return B == 0xa5; }));
    return Bytes;
  };
  // Native LP64 record padding gives 32 + 32 + 32 + 40 + 40 bytes.
  const auto ProtectedContents = Snapshot(Protected, 176);
  EXPECT_EQ(ok(ServiceKind::Lseek, {ProtectedDuplicate, 0, 1}), 55u);
  path("/work");
  const auto Mutable = ok(ServiceKind::Open, {Base});
  const auto MutableStatus = status(Mutable);
  Snapshot(Mutable, 96);
  path("/static/data-link");
  const auto Old = ok(ServiceKind::Open, {Base, 2});
  auto LinkStatus = [&] {
    path("/static/data-link");
    std::array<uint8_t, 160> Bytes;
    Bytes.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, Bytes));
    EXPECT_EQ(ok(ServiceKind::Lstat64, {Base, Base + Page + 8}), 0u);
    llvm::cantFail(Space->read(Base + Page, Bytes));
    EXPECT_TRUE(std::all_of(Bytes.begin(), Bytes.begin() + 8,
                            [](uint8_t B) { return B == 0xa5; }));
    EXPECT_TRUE(std::all_of(Bytes.begin() + 152, Bytes.end(),
                            [](uint8_t B) { return B == 0xa5; }));
    return Bytes;
  };
  const auto Own = LinkStatus();
  path("/static/missing-link");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  error(ServiceKind::Open, {Base, 0x20000a02, 0600}, value::FileExists);
  path("/static/alias/new");
  error(ServiceKind::Open, {Base, 0x20000a02, 0600}, value::TooManyLinks);
  const auto Created = makeFile("/static/alias/new", "UV");
  EXPECT_EQ(llvm::support::endian::read64le(status(Created).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path("/static/missing-link");
  const auto Alias = ok(ServiceKind::Open, {Base});
  const uint8_t UV[] = {'U', 'V'};
  contents(Alias, UV);
  renameFile("/static/alias/new", "/static/alias/moved");
  path("/static/missing-link");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  error(ServiceKind::Open, {Base, 0x100}, value::TooManyLinks);
  path("/static/alias/moved");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  contents(Alias, UV);
  EXPECT_EQ(llvm::support::endian::read16le(status(Created).data() + 6), 0u);
  renameFile("/static/data-link/", "/work/renamed");
  identity(Old, "/work/renamed");
  path("/static/data-link");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  path("/static/alias/renamed");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto Replacement = makeFile("/static/alias/data", "XY");
  EXPECT_EQ(llvm::support::endian::read64le(status(Replacement).data() + 8),
            darwin_test::CreationPolicy.FirstInode + 1);
  path("/static/data-link");
  const auto Current = ok(ServiceKind::Open, {Base});
  const uint8_t XY[] = {'X', 'Y'};
  contents(Current, XY);
  const uint8_t Original[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  contents(Old, Original);
  EXPECT_EQ(llvm::support::endian::read64le(status(Old).data() + 8),
            Options->Metadata.at("/work/data").Inode);
  EXPECT_EQ(LinkStatus(), Own);
  std::array<uint8_t, 32> Bytes;
  Bytes.fill(0xcc);
  llvm::cantFail(Space->write(Base + Page, Bytes));
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page + 8, 32}), 12u);
  llvm::cantFail(Space->read(Base + Page, Bytes));
  const auto &Raw = Options->SymbolicLinks.at("/static/data-link");
  EXPECT_TRUE(std::equal(Raw.begin(), Raw.end(), Bytes.begin() + 8));
  EXPECT_TRUE(std::all_of(Bytes.begin(), Bytes.begin() + 8,
                          [](uint8_t B) { return B == 0xcc; }));
  EXPECT_TRUE(std::all_of(Bytes.begin() + 20, Bytes.end(),
                          [](uint8_t B) { return B == 0xcc; }));
  EXPECT_EQ(Options->Files.at("/work/data"),
            std::vector<uint8_t>(Original, Original + 10));
  EXPECT_EQ(status(Protected), ProtectedStatus);
  EXPECT_EQ(ok(ServiceKind::Lseek, {ProtectedDuplicate, 0, 1}), 55u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {ProtectedDuplicate, 0, 0}), 0u);
  EXPECT_EQ(Snapshot(Protected, 176), ProtectedContents);
  EXPECT_EQ(ok(ServiceKind::Lseek, {ProtectedDuplicate, 0, 1}), 55u);
  llvm::cantFail(Space->write(Base + Page, MutableStatus));
  EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Mutable, Base + Page}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  std::array<uint8_t, 144> Unchanged;
  llvm::cantFail(Space->read(Base + Page, Unchanged));
  EXPECT_EQ(Unchanged, MutableStatus);
  EXPECT_FALSE(invoke(ServiceKind::GetDirEntries64,
                      {Mutable, Base + Page, 256, Base + Page + 512}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Mutable, 0, 1}), 33u);
}

TEST_P(DarwinFileTest,
       FixedDirectoryAliasesKeepCreatedAndRemovedObjectParents) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  Options->SymbolicLinks["/static/box-link"] = {'.', '.', '/', 'w', 'o', 'r',
                                                'k', '/', 'b', 'o', 'x'};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  const auto Box = makeDirectory("/static/alias/box");
  const auto File = makeFile("/static/box-link/item", "ABC");
  path("/static/box-link/item");
  EXPECT_EQ(ok(ServiceKind::Truncate, {Base, 2}), 0u);
  const uint8_t AB[] = {'A', 'B'};
  contents(File, AB);
  path("/static/box-link");
  EXPECT_EQ(ok(ServiceKind::Chdir, {Base}), 0u);
  renameFile("/static/box-link/", "/static/alias/shift");
  identity(Box, "/work/shift");
  identity(File, "/work/shift/item");
  path("/static/box-link");
  error(ServiceKind::Open, {Base}, value::NoEntry);
  path("/static/alias/shift/item");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/static/alias/shift");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto Reused = makeDirectory("/static/alias/box");
  identity(Reused, "/work/box");
  path("new");
  error(ServiceKind::Open, {Base, 0xa02, 0600}, value::NoEntry);
  path("../data");
  const auto ParentFile = ok(ServiceKind::Open, {Base});
  const uint8_t Original[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  contents(ParentFile, Original);
  contents(File, AB);
  identity(Box, "/work/shift");
  path("/static/box-link/item");
  error(ServiceKind::Open, {Base}, value::NoEntry);
}

TEST_P(DarwinFileTest, FixedLinkRefusalsPreserveParentsAndCreationSequence) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  path("/work");
  const auto Parent = ok(ServiceKind::Open, {Base});
  const auto Before = status(Parent);
  path("/static/data-link");
  EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  path("/work/moved", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  path("/work/data", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base + 128, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  path("/static/data-link/");
  error(ServiceKind::UnlinkAt, {999, Base, 0x800}, value::TooManyLinks);
  path("/static/alias/new");
  error(ServiceKind::Open, {Base, 0x20000a02, 0600}, value::TooManyLinks);
  EXPECT_EQ(status(Parent), Before);
  const auto Created = makeFile("/static/missing-link", "N");
  EXPECT_EQ(Created, 4u);
  EXPECT_EQ(llvm::support::endian::read64le(status(Created).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path("/static/data-link");
  const auto Existing = ok(ServiceKind::Open, {Base});
  const uint8_t Original[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  contents(Existing, Original);
}

TEST_P(DarwinFileTest, FixedLinkSlashRenameUsesActualTargetsAndFlagOrder) {
  for (bool Directory : {false, true})
    for (bool SourceLink : {false, true})
      for (uint32_t Flags : {0u, 4u, 2u, 0x10u}) {
        SCOPED_TRACE(Directory);
        SCOPED_TRACE(SourceLink);
        SCOPED_TRACE(Flags);
        Options = darwin_test::mixedSymbolicLinkOptions();
        Options->SwapRenameDirectories.insert("/work");
        Options->SymbolicLinks["/static/dest-link"] = {
            '.', '.', '/', 'w', 'o', 'r', 'k', '/', 'd', 'e', 's', 't'};
        if (Directory) {
          Options->Files.clear();
          Options->WritableFiles.clear();
          Options->MutationPolicies.clear();
          for (const char *Name : {"/work/data", "/work/dest"}) {
            Options->Directories.insert(Name);
            Options->RemovableDirectories.insert(Name);
            Options->MovableDirectories.insert(Name);
            Options->ExchangeableDirectories.insert(Name);
            Options->Metadata[Name] = darwin_test::creationParentMetadata();
          }
        } else {
          Options->Files["/work/dest"] = {'D', 'E', 'S', 'T'};
          Options->Metadata["/work/dest"] = darwin_test::mutationMetadata(4);
          Options->MutationPolicies["/work/dest"] = darwin_test::MutationPolicy;
          Options->WritableFiles.insert("/work/dest");
        }
        Options->Metadata["/work/data"].Inode = 101;
        Options->Metadata["/work/dest"].Inode = 102;
        ASSERT_FALSE(bool(validateFileOptions(*Options)));
        Files = std::make_unique<DarwinFiles>(*Space, Options);
        path("/work/data");
        const auto Held = ok(ServiceKind::Open, {Base});
        if (Directory)
          EXPECT_EQ(ok(ServiceKind::Fchdir, {Held}), 0u);
        path(SourceLink ? "/static/data-link/" : "/work/data");
        path(SourceLink ? "/work/dest" : "/static/dest-link////", Base + 128);
        if (Flags == 4 || Flags == 0x10)
          error(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags},
                Flags == 4 ? value::FileExists : value::TooManyLinks);
        else
          EXPECT_EQ(
              ok(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, Flags}),
              0u);
        const bool Moved = Flags == 0 || Flags == 2;
        identity(Held, Moved ? "/work/dest" : "/work/data");
        if (!Directory) {
          const uint8_t Original[] = {'0', '1', '2', '3', '4',
                                      '5', '6', '7', '8', '9'};
          contents(Held, Original);
          EXPECT_EQ(llvm::support::endian::read64le(status(Held).data() + 8),
                    101u);
        }
        path("/static/data-link");
        EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 12}), 12u);
        if (Flags == 0)
          error(ServiceKind::Open, {Base}, value::NoEntry);
        else {
          const auto AtName = ok(ServiceKind::Open, {Base});
          if (!Directory)
            EXPECT_EQ(
                llvm::support::endian::read64le(status(AtName).data() + 8),
                Flags == 2 ? 102u : 101u);
        }
        if (Directory) {
          path("../dest");
          const auto FromCWD = ok(ServiceKind::Open, {Base});
          identity(FromCWD, "/work/dest");
        }
      }
}

TEST_P(DarwinFileTest,
       RuntimeLinksPreserveOpaqueTargetsAndEmptyCopyBoundaries) {
  Options.emplace();
  Options->Directories.insert("/work");
  Options->MutableDirectories.insert("/work");
  Options->WorkingDirectory = "/work";
  Options->Files["/work/data"] = {'a'};
  for (auto Kind : {ServiceKind::Symlink, ServiceKind::SymlinkAt})
    for (unsigned Size : {0u, 1u, 255u, 256u, 1023u}) {
      SCOPED_TRACE(Size);
      std::string Target(Size, char(0xff));
      path(Target, Base + 1024);
      path("/work/link" + std::to_string(unsigned(Kind)) + "-" +
           std::to_string(Size));
      EXPECT_EQ(Kind == ServiceKind::Symlink
                    ? ok(Kind, {Base + 1024, Base})
                    : ok(Kind, {Base + 1024, 0x12345678000003e7ULL, Base}),
                0u);
      std::vector<uint8_t> Buffer(1040, 0xa5);
      llvm::cantFail(Space->write(Base + Page, Buffer));
      EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page + 8, 1024}), Size);
      llvm::cantFail(Space->read(Base + Page, Buffer));
      EXPECT_TRUE(std::all_of(Buffer.begin(), Buffer.begin() + 8,
                              [](uint8_t B) { return B == 0xa5; }));
      EXPECT_TRUE(std::all_of(Buffer.begin() + 8, Buffer.begin() + 8 + Size,
                              [](uint8_t B) { return B == 0xff; }));
      EXPECT_TRUE(std::all_of(Buffer.begin() + 8 + Size, Buffer.end(),
                              [](uint8_t B) { return B == 0xa5; }));
      if (!Size) {
        for (uint64_t Address : {uint64_t(1), UINT64_MAX}) {
          EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Address, 16}), 0u);
          EXPECT_EQ(ok(ServiceKind::ReadLinkAt, {999, Base, Address, 16}), 0u);
        }
        EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, UINT64_MAX, 0}), 0u);
        error(ServiceKind::Open, {Base}, value::NoEntry);
        path(std::string("/work/link") + std::to_string(unsigned(Kind)) +
             "-0/child");
        error(ServiceKind::Open, {Base}, value::NoEntry);
      } else {
        error(ServiceKind::ReadLink, {Base, UINT64_MAX, 1}, value::BadAddress);
      }
    }
  EXPECT_TRUE(Options->SymbolicLinks.empty());
}

TEST_P(DarwinFileTest,
       RuntimeLinksImportTargetBeforeDestinationAndOnlyThroughNUL) {
  Options->Directories.insert("/work");
  Options->MutableDirectories.insert("/work");
  Options->WorkingDirectory = "/work";
  const uint64_t End = Base + Page * 2;
  path("new");
  error(ServiceKind::SymlinkAt, {End, 999, End}, value::BadAddress);
  error(ServiceKind::Symlink, {UINT64_MAX, Base}, value::BadAddress);
  std::vector<uint8_t> Long(1024, 'x');
  llvm::cantFail(Space->write(Base + 1024, Long));
  error(ServiceKind::SymlinkAt, {Base + 1024, 999, Base}, value::NameTooLong);
  std::array<uint8_t, 8> Prefix;
  Prefix.fill('x');
  llvm::cantFail(Space->write(End - Prefix.size(), Prefix));
  error(ServiceKind::SymlinkAt, {End - 8, 999, Base}, value::BadAddress);
  path("partial", End - 8);
  error(ServiceKind::SymlinkAt, {End - 8, 999, Base}, value::BadDescriptor);
  EXPECT_EQ(ok(ServiceKind::SymlinkAt, {End - 8, 0x12345678fffffffeULL, Base}),
            0u);
  path("/work/new");
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 7u);
  error(ServiceKind::SymlinkAt, {End, 999, Base}, value::BadAddress);
  const uint8_t Raw[] = {'d', 'a', 't', 'a', 0, 0xff, 0xff};
  llvm::cantFail(Space->write(Base + 1024, Raw));
  path("/work/prefix");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 4u);
  std::array<uint8_t, 4> Copied;
  llvm::cantFail(Space->read(Base + Page, Copied));
  EXPECT_EQ(Copied, (std::array<uint8_t, 4>{'d', 'a', 't', 'a'}));
  path("");
  error(ServiceKind::Symlink, {Base + 1024, Base}, value::NoEntry);
  path("/work/" + std::string(256, 'n'));
  error(ServiceKind::Symlink, {Base + 1024, Base}, value::NameTooLong);
  path("/work/" + std::string(255, 'n'));
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
}

TEST_P(DarwinFileTest,
       RuntimeLinksRetainTerminalEntriesAndExpandConsumedSlashes) {
  for (auto Kind : {ServiceKind::Symlink, ServiceKind::SymlinkAt})
    for (unsigned Slashes : {0u, 1u, 4u}) {
      Options.emplace();
      Options->Directories = {"/work", "/work/dir"};
      Options->MutableDirectories.insert("/work");
      Options->WorkingDirectory = "/work";
      Options->Files["/work/data"] = {'a'};
      Files = std::make_unique<DarwinFiles>(*Space, Options);
      auto Make = [&](llvm::StringRef Target, llvm::StringRef Name) {
        path(Target, Base + 1024);
        path(Name);
        return Kind == ServiceKind::Symlink
                   ? invoke(Kind, {Base + 1024, Base})
                   : invoke(Kind, {Base + 1024, uint32_t(-2), Base});
      };
      ASSERT_TRUE(Make("missing", "/work/dangling"));
      ASSERT_TRUE(Make("data", "/work/file-link"));
      ASSERT_TRUE(Make("dir", "/work/dir-link"));
      for (const char *Name : {"data", "dir", "file-link", "dir-link"}) {
        const auto Out = Make("opaque", "/work/" + std::string(Name) +
                                            std::string(Slashes, '/'));
        ASSERT_TRUE(Out);
        EXPECT_TRUE(Out->Error);
        EXPECT_EQ(Out->Value, std::string(Name) == "data" && Slashes
                                  ? value::NotDirectory
                                  : value::FileExists);
      }
      auto Out = Make("data", "/work/dangling" + std::string(Slashes, '/'));
      ASSERT_TRUE(Out);
      EXPECT_EQ(Out->Error, Slashes == 0);
      EXPECT_EQ(Out->Value, Slashes ? 0u : value::FileExists);
      path("/work/dangling");
      EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 7u);
      if (Slashes) {
        path("/work/missing");
        EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 4u);
        const auto FD = ok(ServiceKind::Open, {Base});
        const uint8_t A[] = {'a'};
        contents(FD, A);
      }
    }
}

TEST_P(DarwinFileTest, RuntimeLinksDoNotReuseOldMetadataOrConsumeFileInodes) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  const auto OriginalLinks = Options->SymbolicLinks;
  path("/work/data");
  const auto Old = ok(ServiceKind::Open, {Base});
  const auto Before = status(Old);
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("other", Base + 1024);
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  std::array<uint8_t, 160> Buffer;
  Buffer.fill(0xa5);
  llvm::cantFail(Space->write(Base + Page, Buffer));
  EXPECT_FALSE(invoke(ServiceKind::Lstat64, {Base, Base + Page + 8}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMetadata);
  std::array<uint8_t, 160> After;
  llvm::cantFail(Space->read(Base + Page, After));
  EXPECT_EQ(After, Buffer);
  const auto New = makeFile("/work/other", "XY");
  EXPECT_EQ(New, Old + 1);
  EXPECT_EQ(llvm::support::endian::read64le(status(New).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path("/work/data");
  const auto Alias = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(status(Alias), status(New));
  identity(Old, "/work/data");
  const uint8_t XY[] = {'X', 'Y'};
  contents(Alias, XY);
  const uint8_t Original[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  contents(Old, Original);
  auto Expected = Before;
  llvm::support::endian::write16le(Expected.data() + 6, 0);
  llvm::support::endian::write64le(Expected.data() + 64,
                                   darwin_test::MutationPolicy.Time.Seconds);
  llvm::support::endian::write64le(
      Expected.data() + 72, darwin_test::MutationPolicy.Time.Nanoseconds);
  EXPECT_EQ(status(Old), Expected);
  EXPECT_EQ(Options->SymbolicLinks, OriginalLinks);
  EXPECT_EQ(
      Options->Files.at("/work/data"),
      (std::vector<uint8_t>{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}));
}

TEST_P(DarwinFileTest,
       RuntimeLinksUseHeldParentsAndRefuseTwoSidedSubtreeMoves) {
  Options->Files.clear();
  creationPolicy();
  Options->Directories.insert("/");
  Options->SwapRenameDirectories.insert("/");
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  const auto Box = makeDirectory("/box");
  const auto Other = makeDirectory("/other");
  renameFile("/box", "/shift");
  path("child");
  path("../data", Base + 1024);
  EXPECT_EQ(ok(ServiceKind::SymlinkAt,
               {Base + 1024, Box | 0x1234567800000000ULL, Base}),
            0u);
  EXPECT_EQ(ok(ServiceKind::Fchdir, {Box}), 0u);
  path("cwd");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  path("/shift/cwd");
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 7u);
  path("/shift");
  error(ServiceKind::Rmdir, {Base}, value::DirectoryNotEmpty);
  path("/other");
  path("/shift", Base + 128);
  error(ServiceKind::Rename, {Base, Base + 128}, value::DirectoryNotEmpty);
  for (bool Reverse : {false, true}) {
    path(Reverse ? "/other" : "/shift");
    path(Reverse ? "/shift" : "/other", Base + 128);
    EXPECT_FALSE(
        invoke(ServiceKind::RenameAtX, {999, Base, 999, Base + 128, 2}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkDirectoryMove);
    identity(Box, "/shift");
    identity(Other, "/other");
    path("/shift/child");
    EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 7u);
  }
  path("/shift");
  path("/moved", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkDirectoryMove);
  path("child");
  EXPECT_EQ(ok(ServiceKind::UnlinkAt, {Box, Base, 0}), 0u);
  error(ServiceKind::ReadLinkAt, {Box, Base, Base + Page, 32}, value::NoEntry);
  path("/shift");
  error(ServiceKind::Rmdir, {Base}, value::DirectoryNotEmpty);
  path("/moved", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkDirectoryMove);
  path("/shift/cwd");
  path("/different", Base + 128);
  EXPECT_FALSE(invoke(ServiceKind::Rename, {Base, Base + 128}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  path("/data");
  const auto Created = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(llvm::support::endian::read64le(status(Created).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  path("cwd");
  const auto Alias = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(status(Alias), status(Created));
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  renameFile("/shift", "/moved");
  identity(Box, "/moved");
  EXPECT_EQ(status(Alias), status(Created));
  const auto Reused = makeDirectory("/shift");
  identity(Reused, "/shift");
  identity(Box, "/moved");
  path("/moved");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  path("new");
  path("../data", Base + 1024);
  error(ServiceKind::Symlink, {Base + 1024, Base}, value::NoEntry);
}

TEST_P(DarwinFileTest, RuntimeLinksCannotTransferProtectedNamespaceGrants) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  path("../static", Base + 1024);
  path("/work/up");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  path("data", Base + 1024);
  path("/work/up/new");
  EXPECT_FALSE(invoke(ServiceKind::Symlink, {Base + 1024, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryNotMutable);
  path("/static/new");
  error(ServiceKind::ReadLink, {Base, Base + Page, 32}, value::NoEntry);
  path("/static/data-link");
  error(ServiceKind::Symlink, {Base + 1024, Base}, value::FileExists);
  EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
}

TEST_P(DarwinFileTest,
       RuntimeLinksShareExactEntryAndStorageLimitsWithoutEffects) {
  for (unsigned Extra : {0u, 1u}) {
    Options.emplace();
    Options->Directories = {"/work"};
    Options->MutableDirectories = {"/work"};
    Options->Files["/work/data"] = {};
    Options->Metadata["/work/data"] = darwin_test::mutationMetadata(0);
    Options->Metadata["/work"] = darwin_test::creationParentMetadata();
    Options->WritableFiles.insert("/work/data");
    Options->MutationPolicies["/work/data"] = darwin_test::MutationPolicy;
    // Catalogue: /work (+6), mutable reference (+6), /work/data (+11),
    // writable reference (+11), policy reference (+11). Link: /work/link
    // (+11)+4.
    const auto Capacity = darwin_file_limits::Bytes - 45 - 15;
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/work/data");
    const auto FD = ok(ServiceKind::Open, {Base, 2});
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {FD, Capacity + Extra}), 0u);
    const auto Before = status(FD);
    path("/work");
    const auto Parent = ok(ServiceKind::Open, {Base});
    const auto ParentBefore = status(Parent);
    path("data", Base + 1024);
    path("/work/link");
    if (Extra) {
      EXPECT_FALSE(invoke(ServiceKind::Symlink, {Base + 1024, Base}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkCreationLimit);
      error(ServiceKind::ReadLink, {Base, Base + Page, 32}, value::NoEntry);
      EXPECT_EQ(status(FD), Before);
      EXPECT_EQ(status(Parent), ParentBefore);
    } else {
      EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
      EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 4u);
      EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {FD, Capacity + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    }
  }
  Options.emplace();
  Options->Files.clear();
  Options->Directories = {"/work", "/static"};
  Options->MutableDirectories.insert("/work");
  Options->SymbolicLinks["/static/fixed"] = {'x'};
  for (unsigned I = 0; I != 251; ++I)
    Options->Files["/work/f" + std::to_string(I)] = {};
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("x", Base + 1024);
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  path("/work/b");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  path("/work/c");
  EXPECT_FALSE(invoke(ServiceKind::Symlink, {Base + 1024, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkCreationLimit);
  error(ServiceKind::ReadLink, {Base, Base + Page, 32}, value::NoEntry);
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  EXPECT_FALSE(invoke(ServiceKind::Open, {Base, 0x202, 0600}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileCreationLimit);
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 1u);
  path("/static/fixed");
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 1u);
  path("/work/f0");
  EXPECT_EQ(ok(ServiceKind::Open, {Base}), 3u);
}

TEST_P(DarwinFileTest,
       RuntimeLinkRemovalRetainsTargetsAndRejectsProtectedEntries) {
  for (unsigned Route : {0u, 1u, 2u, 3u, 4u})
    for (const std::string &Target :
         {std::string("data"), std::string("dir"), std::string("missing"),
          std::string("link"), std::string(), std::string(3, char(0xff)),
          std::string("../static/data-link")}) {
      SCOPED_TRACE(Route);
      SCOPED_TRACE(Target);
      Options = darwin_test::mixedSymbolicLinkOptions();
      Options->Directories.insert("/work/dir");
      Options->Metadata["/work/dir"] = darwin_test::creationParentMetadata();
      Options->Metadata["/work/dir"].Inode = 42;
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
      Files = std::make_unique<DarwinFiles>(*Space, Options);
      const auto OriginalLinks = Options->SymbolicLinks;
      path("/work/data");
      const auto Data = ok(ServiceKind::Open, {Base});
      const auto Duplicate = ok(ServiceKind::Dup, {Data});
      const auto DataBefore = status(Data);
      const auto FlagsBefore = ok(ServiceKind::Fcntl, {Data, 3});
      EXPECT_EQ(ok(ServiceKind::Lseek, {Data, 3, 0}), 3u);
      auto Lease = Files->mappingSource(Data);
      ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
      path("/work/dir");
      const auto Directory = ok(ServiceKind::Open, {Base});
      const auto DirectoryBefore = status(Directory);
      path("/work");
      const auto Parent = ok(ServiceKind::Open, {Base});
      if (Route == 2)
        EXPECT_EQ(ok(ServiceKind::Fchdir, {Parent}), 0u);
      path(Target, Base + 1024);
      path(Route == 0 ? "/work/link" : "link");
      if (Route == 1 || Route >= 3)
        EXPECT_EQ(ok(ServiceKind::SymlinkAt,
                     {Base + 1024, Parent | 0x1234567800000000ULL, Base}),
                  0u);
      else
        EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
      if (Route == 1 || Route >= 3)
        EXPECT_EQ(ok(ServiceKind::UnlinkAt,
                     {Parent | 0x1234567800000000ULL, Base,
                      (Route == 3 ? 0u : 0x8765432100000000ULL) |
                          (Route >= 3 ? 0x800u : 0u)}),
                  0u);
      else
        EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
      path("/work/link");
      error(ServiceKind::Unlink, {Base}, value::NoEntry);
      error(ServiceKind::ReadLink, {Base, UINT64_MAX, 32}, value::NoEntry);
      error(ServiceKind::Lstat64, {Base, UINT64_MAX}, value::NoEntry);
      error(ServiceKind::Access, {Base, 0}, value::NoEntry);
      EXPECT_EQ(status(Data), DataBefore);
      EXPECT_EQ(status(Duplicate), DataBefore);
      EXPECT_EQ(status(Directory), DirectoryBefore);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {Data, 3}), FlagsBefore);
      EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 3u);
      const uint8_t Original[] = {'0', '1', '2', '3', '4',
                                  '5', '6', '7', '8', '9'};
      contents(Data, Original);
      identity(Data, "/work/data");
      identity(Directory, "/work/dir");
      EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
      path(".");
      const auto FromCWD = ok(ServiceKind::Open, {Base});
      EXPECT_EQ(status(FromCWD), DirectoryBefore);
      path("..");
      const auto FromParent = ok(ServiceKind::Open, {Base});
      identity(FromParent, "/work");
      // Returning to the original set of names does not restore an old
      // directory metadata observation.
      std::array<uint8_t, 144> Canary;
      Canary.fill(0xa5);
      llvm::cantFail(Space->write(Base + Page, Canary));
      EXPECT_FALSE(invoke(ServiceKind::Fstat64, {Parent, Base + Page}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryMutated);
      std::array<uint8_t, 144> After;
      llvm::cantFail(Space->read(Base + Page, After));
      EXPECT_EQ(After, Canary);
      // A created link pointing at a protected link can be removed; the
      // initial entry itself remains protected and retains its raw target.
      path("/static/data-link");
      EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
      EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 12u);
      std::array<uint8_t, 12> Raw;
      llvm::cantFail(Space->read(Base + Page, Raw));
      EXPECT_EQ(std::vector<uint8_t>(Raw.begin(), Raw.end()),
                OriginalLinks.at("/static/data-link"));
      EXPECT_EQ(Options->SymbolicLinks, OriginalLinks);
      EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
      EXPECT_EQ(ok(ServiceKind::Close, {Duplicate}), 0u);
      EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
                llvm::ArrayRef<uint8_t>(Original));
    }
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalReclaimsExactBytesOnceAtFullCapacity) {
  for (unsigned TargetSize : {0u, 1023u}) {
    SCOPED_TRACE(TargetSize);
    Options.emplace();
    Options->Directories.insert("/work");
    Options->MutableDirectories.insert("/work");
    Options->Files["/work/data"] = {};
    Options->Metadata["/work/data"] = darwin_test::mutationMetadata(0);
    Options->WritableFiles.insert("/work/data");
    Options->MutationPolicies["/work/data"] = darwin_test::MutationPolicy;
    Options->DescriptorLimit = 4;
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/work/data");
    const auto Data = ok(ServiceKind::Open, {Base, 2});
    EXPECT_EQ(Data, 3u);
    // /work declaration+grant: 12; /work/data name+writable+policy: 33.
    const uint64_t Capacity = darwin_file_limits::Bytes - 45;
    const uint64_t Charge = 11 + TargetSize; // /work/link + NUL + target.
    for (unsigned Cycle : {0u, 1u, 2u}) {
      SCOPED_TRACE(Cycle);
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity - Charge}), 0u);
      path(std::string(TargetSize, 'x'), Base + 1024);
      path("/work/link");
      EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
      error(ServiceKind::Dup, {Data}, value::TooManyFiles);
      EXPECT_FALSE(
          invoke(ServiceKind::Ftruncate, {Data, Capacity - Charge + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
      const auto Before = status(Data);
      EXPECT_EQ(ok(ServiceKind::UnlinkAt, {999, Base, 0}), 0u);
      EXPECT_EQ(status(Data), Before);
      error(ServiceKind::UnlinkAt, {999, Base, 0}, value::NoEntry);
      EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
      EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 1}));
      EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    }
  }
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalReleasesOneEntryForEveryNodeKind) {
  Options.emplace();
  Options->Directories = {"/work", "/static"};
  Options->MutableDirectories.insert("/work");
  Options->SymbolicLinks["/static/fixed"] = {'x'};
  for (unsigned I = 0; I != 251; ++I)
    Options->Files["/work/f" + std::to_string(I)] = {};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("x", Base + 1024);
  for (const char *Name : {"/work/a", "/work/b"}) {
    path(Name);
    EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  }
  path("/work/c");
  EXPECT_FALSE(invoke(ServiceKind::Symlink, {Base + 1024, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkCreationLimit);
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  error(ServiceKind::Unlink, {Base}, value::NoEntry);
  path("/work/c");
  EXPECT_EQ(ok(ServiceKind::Mkdir, {Base, 0700}), 0u);
  path("/work/d");
  EXPECT_FALSE(invoke(ServiceKind::Symlink, {Base + 1024, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkCreationLimit);
  path("/work/c");
  EXPECT_EQ(ok(ServiceKind::Rmdir, {Base}), 0u);
  const auto Data = ok(ServiceKind::Open, {Base, 0x202, 0600});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/work/d");
  EXPECT_FALSE(invoke(ServiceKind::Symlink, {Base + 1024, Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkCreationLimit);
  EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  path("/work/e");
  EXPECT_FALSE(invoke(ServiceKind::Mkdir, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::DirectoryCreationLimit);
  path("/work/b");
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 1}), 1u);
  path("/static/fixed");
  EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
  EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 1}), 1u);
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalKeepsReusedNamesAndOrphanLeasesApart) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  path("/work/data");
  const auto Old = ok(ServiceKind::Open, {Base});
  auto OldLease = Files->mappingSource(Old);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(OldLease));
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  const auto OldStatus = status(Old);
  const auto Target = makeFile("/work/target", "XY");
  const auto TargetStatus = status(Target);
  auto TargetLease = Files->mappingSource(Target);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(TargetLease));
  path("target", Base + 1024);
  path("/work/data");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  const auto Alias = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(status(Alias), TargetStatus);
  EXPECT_EQ(status(Target), TargetStatus);
  EXPECT_EQ(status(Old), OldStatus);
  const auto New = makeFile("/work/data", "NEW");
  EXPECT_EQ(llvm::support::endian::read64le(status(Target).data() + 8),
            darwin_test::CreationPolicy.FirstInode);
  EXPECT_EQ(llvm::support::endian::read64le(status(New).data() + 8),
            darwin_test::CreationPolicy.FirstInode + 1);
  const uint8_t Original[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  const uint8_t XY[] = {'X', 'Y'};
  const uint8_t NEW[] = {'N', 'E', 'W'};
  contents(Old, Original);
  contents(Alias, XY);
  contents(New, NEW);
  identity(Old, "/work/data");
  identity(New, "/work/data");
  identity(Alias, "/work/target");
  for (const auto FD : {Old, Target, Alias, New})
    EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(OldLease).Bytes,
            llvm::ArrayRef<uint8_t>(Original));
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(TargetLease).Bytes,
            llvm::ArrayRef<uint8_t>(XY));
  EXPECT_EQ(ok(ServiceKind::Umask, {0077}), 0027u);
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalDoesNotCreditMappingOnlyTargets) {
  Options.emplace();
  Options->Directories.insert("/work");
  Options->MutableDirectories.insert("/work");
  Options->Files["/work/data"] = {'0', '1', '2', '3', '4',
                                  '5', '6', '7', '8', '9'};
  Options->Files["/work/other"] = {};
  Options->WritableFiles.insert("/work/other");
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/work/data");
  const auto Data = ok(ServiceKind::Open, {Base});
  auto Lease = Files->mappingSource(Data);
  ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
  path("/work/other");
  const auto Other = ok(ServiceKind::Open, {Base, 2});
  // Parent name+grant: 12; data name+contents: 21; other name+grant: 24.
  const uint64_t Capacity = darwin_file_limits::Bytes - 57;
  path("data", Base + 1024);
  path("/work/link");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Other, Capacity - 15}), 0u);
  path("/work/data");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  EXPECT_EQ(ok(ServiceKind::Close, {Data}), 0u);
  path("/work/link");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  error(ServiceKind::Unlink, {Base}, value::NoEntry);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Other, Capacity}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Other, Capacity + 1}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  const uint8_t Original[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
            llvm::ArrayRef<uint8_t>(Original));
  Lease = uint32_t(0);
  // Only the orphan's ten data bytes are reclaimed. Its initial input path
  // remains part of the reserved catalogue cost.
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Other, Capacity + 10}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Other, Capacity + 11}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalConsumesOnlyTheCurrentSlashSuffix) {
  for (const std::string &Target :
       {std::string("data"), std::string("dir"), std::string("missing"),
        std::string("inner"), std::string()})
    for (unsigned Slashes : {0u, 1u, 4u}) {
      SCOPED_TRACE(Target);
      SCOPED_TRACE(Slashes);
      Options = darwin_test::mixedSymbolicLinkOptions();
      Options->Directories.insert("/work/dir");
      Options->Metadata["/work/dir"] = darwin_test::creationParentMetadata();
      Options->Metadata["/work/dir"].Inode = 42;
      ASSERT_FALSE(bool(validateFileOptions(*Options)));
      Files = std::make_unique<DarwinFiles>(*Space, Options);
      path("/work/data");
      const auto Data = ok(ServiceKind::Open, {Base});
      const auto Duplicate = ok(ServiceKind::Dup, {Data});
      const auto DataStatus = status(Data);
      const auto Flags = ok(ServiceKind::Fcntl, {Data, 3});
      EXPECT_EQ(ok(ServiceKind::Lseek, {Data, 3, 0}), 3u);
      auto Lease = Files->mappingSource(Data);
      ASSERT_TRUE(std::holds_alternative<DarwinFiles::Mapping>(Lease));
      path("/work/dir");
      const auto Directory = ok(ServiceKind::Open, {Base});
      const auto DirectoryStatus = status(Directory);
      EXPECT_EQ(ok(ServiceKind::Fchdir, {Directory}), 0u);
      path(Target, Base + 1024);
      path("/work/inner");
      EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
      path("inner", Base + 1024);
      path("/work/a");
      EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
      path("/work/a" + std::string(Slashes, '/'));
      EXPECT_EQ(ok(ServiceKind::UnlinkAt, {999, Base, 0}), 0u);
      path(Slashes ? "/work/inner" : "/work/a");
      error(ServiceKind::ReadLink, {Base, UINT64_MAX, 32}, value::NoEntry);
      error(ServiceKind::Unlink, {Base}, value::NoEntry);
      const std::string Remaining = Slashes ? "inner" : Target;
      path(Slashes ? "/work/a" : "/work/inner");
      std::array<uint8_t, 64> Buffer;
      Buffer.fill(0xa5);
      llvm::cantFail(Space->write(Base + Page, Buffer));
      EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page + 8, 32}),
                Remaining.size());
      std::copy(Remaining.begin(), Remaining.end(), Buffer.begin() + 8);
      std::array<uint8_t, 64> After;
      llvm::cantFail(Space->read(Base + Page, After));
      EXPECT_EQ(After, Buffer);
      EXPECT_EQ(status(Data), DataStatus);
      EXPECT_EQ(status(Duplicate), DataStatus);
      EXPECT_EQ(status(Directory), DirectoryStatus);
      EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 3u);
      EXPECT_EQ(ok(ServiceKind::Fcntl, {Data, 3}), Flags);
      path(".");
      const auto CWD = ok(ServiceKind::Open, {Base});
      EXPECT_EQ(status(CWD), DirectoryStatus);
      path("..");
      const auto Parent = ok(ServiceKind::Open, {Base});
      identity(Parent, "/work");
      const uint8_t Original[] = {'0', '1', '2', '3', '4',
                                  '5', '6', '7', '8', '9'};
      contents(Data, Original);
      for (const auto FD : {Data, Duplicate, Directory, CWD, Parent})
        EXPECT_EQ(ok(ServiceKind::Close, {FD}), 0u);
      EXPECT_EQ(std::get<DarwinFiles::Mapping>(Lease).Bytes,
                llvm::ArrayRef<uint8_t>(Original));
    }
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalRefundsTheResolvedLinkNotItsSpelling) {
  for (unsigned Slashes : {1u, 4u}) {
    SCOPED_TRACE(Slashes);
    Options.emplace();
    Options->Directories.insert("/work");
    Options->MutableDirectories.insert("/work");
    Options->Files["/work/data"] = {};
    Options->WritableFiles.insert("/work/data");
    ASSERT_FALSE(bool(validateFileOptions(*Options)));
    Files = std::make_unique<DarwinFiles>(*Space, Options);
    path("/work/data");
    const auto Data = ok(ServiceKind::Open, {Base, 2});
    path("data", Base + 1024);
    path("/work/inner-long");
    EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
    path("inner-long", Base + 1024);
    path("/work/a");
    EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
    // Fixed parent+grant 12, data name+writable 22; a costs 8+10,
    // while the resolved inner-long node costs 17+4.
    const uint64_t Capacity = darwin_file_limits::Bytes - 34 - 18 - 21;
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
    path("/work/a" + std::string(Slashes, '/'));
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
    error(ServiceKind::Unlink, {Base}, value::NoEntry);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 21}), 0u);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 22}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    path("/work/a");
    EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 10u);
    EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
    EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 21 + 18}), 0u);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 21 + 19}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
  }
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalKeepsProtectedResolvedOperands) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  auto M = darwin_test::creationParentMetadata();
  M.Inode = 50;
  Options->Metadata["/static"] = M;
  Options->DirectoryContents["/static"] = {{{".", 50, 4, 11, 0},
                                            {"..", 1, 4, 22, 0},
                                            {"alias", 124, 10, 33, 0},
                                            {"data-link", 123, 10, 44, 0},
                                            {"missing-link", 125, 10, 55, 0}},
                                           1};
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/static");
  const auto Static = ok(ServiceKind::Open, {Base});
  const auto Duplicate = ok(ServiceKind::Dup, {Static});
  const auto Before = status(Static);
  auto Snapshot = [&]() {
    std::array<uint8_t, 272> Buffer;
    Buffer.fill(0xa5);
    llvm::cantFail(Space->write(Base + Page, Buffer));
    EXPECT_EQ(ok(ServiceKind::GetDirEntries64,
                 {Static, Base + Page + 8, 256, Base + Page + 512}),
              176u);
    llvm::cantFail(Space->read(Base + Page, Buffer));
    EXPECT_TRUE(std::all_of(Buffer.begin(), Buffer.begin() + 8,
                            [](uint8_t Byte) { return Byte == 0xa5; }));
    EXPECT_TRUE(std::all_of(Buffer.begin() + 184, Buffer.end(),
                            [](uint8_t Byte) { return Byte == 0xa5; }));
    return Buffer;
  };
  const auto InitialSnapshot = Snapshot();
  path("/static/data-link");
  EXPECT_EQ(ok(ServiceKind::Lstat64, {Base, Base + 256}), 0u);
  std::array<uint8_t, 144> LinkStatus;
  llvm::cantFail(Space->read(Base + 256, LinkStatus));
  path("../static/data-link", Base + 1024);
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  for (unsigned Slashes : {1u, 4u}) {
    path("/work/a" + std::string(Slashes, '/'));
    EXPECT_FALSE(invoke(ServiceKind::Unlink, {Base}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::SymbolicLinkMutation);
    error(ServiceKind::UnlinkAt, {999, Base, 0x800}, value::TooManyLinks);
    path("/work/a");
    EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 19u);
    path("/static/data-link");
    EXPECT_EQ(ok(ServiceKind::Lstat64, {Base, Base + 256}), 0u);
    std::array<uint8_t, 144> Current;
    llvm::cantFail(Space->read(Base + 256, Current));
    EXPECT_EQ(Current, LinkStatus);
  }
  // Bare deletion removes a; its protected target remains protected.
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::UnlinkAt, {999, Base, 0x800}), 0u);
  path("data", Base + 1024);
  path("/work/inner");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  // The textual initial alias survives; only its mutable target's child goes.
  path("/static/alias/inner");
  EXPECT_EQ(ok(ServiceKind::Unlink, {Base}), 0u);
  path("/work/inner");
  error(ServiceKind::ReadLink, {Base, UINT64_MAX, 32}, value::NoEntry);
  EXPECT_EQ(status(Static), Before);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 55u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 0}), 0u);
  EXPECT_EQ(Snapshot(), InitialSnapshot);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Duplicate, 0, 1}), 55u);
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalErrorsDoNotRefundOrRemoveNames) {
  Options.emplace();
  Options->Directories.insert("/work");
  Options->MutableDirectories.insert("/work");
  Options->WorkingDirectory = "/work";
  Options->Files["/work/data"] = {};
  Options->WritableFiles.insert("/work/data");
  Options->DescriptorLimit = 5;
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  Files = std::make_unique<DarwinFiles>(*Space, Options);
  path("/work/data");
  const auto Data = ok(ServiceKind::Open, {Base, 2});
  path("/work");
  const auto Parent = ok(ServiceKind::Open, {Base});
  path("data", Base + 1024);
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  path(".", Base + 1024);
  path("/work/alias");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  // Parent+grant 12, CWD6, data name+writable22, a12 and alias13.
  const uint64_t Capacity = darwin_file_limits::Bytes - 40 - 12 - 13;
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity}), 0u);
  EXPECT_EQ(ok(ServiceKind::Lseek, {Data, 3, 0}), 3u);
  const auto Flags = ok(ServiceKind::Fcntl, {Data, 3});
  auto Preserved = [&]() {
    path("/work/a");
    EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 4u);
    path("/work/alias");
    EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 1u);
    EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 1}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
    EXPECT_EQ(ok(ServiceKind::Lseek, {Data, 0, 1}), 3u);
    EXPECT_EQ(ok(ServiceKind::Fcntl, {Data, 3}), Flags);
  };
  for (const char *Name : {"alias/a", "a/", "a////"}) {
    path(Name);
    error(ServiceKind::UnlinkAt, {Parent, Base, 0x800}, value::TooManyLinks);
    Preserved();
  }
  for (uint64_t BadFlags : {0x20u, 0x820u}) {
    error(ServiceKind::UnlinkAt, {999, 0, BadFlags}, value::InvalidArgument);
    Preserved();
  }
  path("a");
  error(ServiceKind::UnlinkAt, {Parent, Base, 0x880}, value::NotDirectory);
  Preserved();
  path("a");
  error(ServiceKind::UnlinkAt, {999, Base, 0}, value::BadDescriptor);
  error(ServiceKind::UnlinkAt, {Data, Base, 0}, value::NotDirectory);
  path("");
  error(ServiceKind::UnlinkAt, {999, Base, 0}, value::BadDescriptor);
  error(ServiceKind::UnlinkAt, {Parent, Base, 0}, value::NoEntry);
  error(ServiceKind::UnlinkAt, {999, 0, 0}, value::BadAddress);
  const auto Last = Base + Page * 2 - 1;
  llvm::cantFail(Space->writeInteger(Last, 'a', 1));
  error(ServiceKind::UnlinkAt, {999, Last, 0}, value::BadDescriptor);
  error(ServiceKind::UnlinkAt, {Data, Last, 0}, value::NotDirectory);
  error(ServiceKind::UnlinkAt, {Parent, Last, 0}, value::BadAddress);
  llvm::cantFail(Space->writeInteger(Last, '/', 1));
  error(ServiceKind::UnlinkAt, {999, Last, 0}, value::BadAddress);
  Preserved();
  for (uint64_t FlagsOnlyUnsupported : {0x100u, 0x1000u}) {
    EXPECT_FALSE(invoke(ServiceKind::UnlinkAt, {999, 0, FlagsOnlyUnsupported}));
    EXPECT_EQ(Result.Diagnostic, diagnostic::UnlinkFlags);
    Preserved();
  }
  path("a/child");
  error(ServiceKind::UnlinkAt, {Parent, Base, 0}, value::NotDirectory);
  path("missing/../a");
  error(ServiceKind::UnlinkAt, {Parent, Base, 0}, value::NoEntry);
  Preserved();
  error(ServiceKind::Dup, {Data}, value::TooManyFiles);
  path("/work/a");
  EXPECT_EQ(ok(ServiceKind::UnlinkAt, {999, Base, 0x8765432100000800ULL}), 0u);
  error(ServiceKind::UnlinkAt, {999, Base, 0}, value::NoEntry);
  EXPECT_EQ(ok(ServiceKind::Ftruncate, {Data, Capacity + 12}), 0u);
  EXPECT_FALSE(invoke(ServiceKind::Ftruncate, {Data, Capacity + 13}));
  EXPECT_EQ(Result.Diagnostic, diagnostic::FileMutationLimit);
}

TEST_P(DarwinFileTest, RuntimeLinkRemovalTransportFailuresKeepLiveEntries) {
  Options = darwin_test::mixedSymbolicLinkOptions();
  FailingFileInput Input(*Space);
  Files = std::make_unique<DarwinFiles>(Input, Options);
  path("/work");
  const auto Parent = ok(ServiceKind::Open, {Base});
  EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, 33, 0}), 33u);
  path("data", Base + 1024);
  path("/work/link");
  EXPECT_EQ(ok(ServiceKind::Symlink, {Base + 1024, Base}), 0u);
  for (auto Kind : {ServiceKind::Unlink, ServiceKind::UnlinkAt})
    for (bool Preflight : {false, true})
      for (bool Budget : {false, true})
        for (unsigned Offset : {0u, 2u}) {
          SCOPED_TRACE(unsigned(Kind));
          SCOPED_TRACE(Preflight);
          SCOPED_TRACE(Budget);
          SCOPED_TRACE(Offset);
          path(Kind == ServiceKind::Unlink ? "/work/link" : "link");
          Input.FailureAddress = Base + Offset;
          Input.FailAccess = Preflight;
          Input.FailRead = !Preflight;
          Input.FailBudget = Budget;
          const std::array<uint64_t, 6> Args =
              Kind == ServiceKind::Unlink
                  ? std::array<uint64_t, 6>{Base}
                  : std::array<uint64_t, 6>{Parent, Base, 0};
          auto Failed = Files->handle(Kind, {0, 0, Args, std::nullopt}, Result);
          ASSERT_FALSE(bool(Failed));
          auto Error = Failed.takeError();
          if (Budget) {
            EXPECT_TRUE(Error.isA<GuestMemoryLimitError>());
            llvm::consumeError(std::move(Error));
          } else {
            EXPECT_EQ(llvm::toString(std::move(Error)),
                      Preflight ? "file input preflight failed"
                                : "file input transport failed");
          }
          Input.FailAccess = Input.FailRead = Input.FailBudget = false;
          path("/work/link");
          EXPECT_EQ(ok(ServiceKind::ReadLink, {Base, Base + Page, 32}), 4u);
          EXPECT_EQ(ok(ServiceKind::Lseek, {Parent, 0, 1}), 33u);
        }
  path("link");
  EXPECT_EQ(ok(ServiceKind::UnlinkAt, {Parent, Base, 0}), 0u);
  error(ServiceKind::ReadLinkAt, {Parent, Base, UINT64_MAX, 32},
        value::NoEntry);
}

INSTANTIATE_TEST_SUITE_P(OSPages, DarwinFileTest, testing::Values(4096, 16384));
} // namespace
} // namespace neverd::emulation::darwin_model
