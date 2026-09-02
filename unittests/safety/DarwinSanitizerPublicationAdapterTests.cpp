//===- DarwinSanitizerPublicationAdapterTests.cpp - Darwin adapter tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/DarwinSanitizerPublicationAdapter.h"
#include "neverd/safety/SanitizerPublicationMetadata.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"

#ifdef __APPLE__
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace neverd::safety::sanitizer_publication_metadata;

namespace {

llvm::Error testError(const char *Message) {
  return llvm::createStringError(llvm::errc::io_error, "%s", Message);
}

struct FakeFile {
  detail::DarwinPublicationFileStatusV1 Status;
  std::vector<uint8_t> Bytes;
  std::optional<CanonicalSecurityMetadata> ACL;
  std::map<std::string, CanonicalSecurityMetadata> Xattrs;
};

class FakeDarwinFilesystem {
public:
  FakeDarwinFilesystem() {
    Source.Status.Node = PublicationNodeKind::RegularFile;
    Source.Status.Device = 7;
    Source.Status.Inode = 101;
    Source.Status.Generation = 0; // Exercises held-fd transaction identity.
    Source.Status.LinkCount = 1;
    Source.Status.UID = 501;
    Source.Status.GID = 20;
    Source.Status.Mode = 0644;
    Source.Status.BirthSeconds = 1000;
    Source.Status.BirthNanoseconds = 17;
    Source.Status.ModifySeconds = 1001;
    Source.Status.ChangeSeconds = 1002;
    Source.Bytes = {1, 2, 3, 4};
    Source.Status.Size = Source.Bytes.size();

    Directory.Status.Node = PublicationNodeKind::Other;
    Directory.Status.Directory = true;
    Directory.Status.Device = 7;
    Directory.Status.Inode = 50;
    Directory.Status.Generation = 3;
    Directory.Status.LinkCount = 1;
    Directory.Status.UID = 501;
    Directory.Status.GID = 20;
    Directory.Status.Mode = 0755;

    Handles.emplace(kSourceFD, &Source);
    Handles.emplace(kDirectoryFD, &Directory);
  }

  detail::DarwinSanitizerPublicationNativeOperationsV1 operations() {
    detail::DarwinSanitizerPublicationNativeOperationsV1 Ops;
    Ops.GetEffectiveUserID = [&]() -> llvm::Expected<uint32_t> {
      ++EffectiveUIDCalls;
      return EffectiveUID +
             (ChangeEffectiveUIDAfterCandidateCreation && CreateCalls != 0);
    };
    Ops.OpenSourceNoFollow = [&](llvm::StringRef Path) -> llvm::Expected<int> {
      ++OpenSourceCalls;
      LastSourcePath = Path.str();
      return kSourceFD;
    };
    Ops.OpenDirectoryNoFollow =
        [&](llvm::StringRef Path) -> llvm::Expected<int> {
      ++OpenDirectoryCalls;
      LastDirectoryPath = Path.str();
      return kDirectoryFD;
    };
    Ops.OpenAnchoredReadOnlyNoFollow =
        [&](int DirectoryFD, llvm::StringRef Name) -> llvm::Expected<int> {
      ++OpenAnchoredCalls;
      EXPECT_EQ(DirectoryFD, kDirectoryFD);
      LastFinalName = Name.str();
      if (!DestinationPresent)
        return testError("anchored final is absent");
      const int FD = NextFD++;
      FakeFile *Opened = &Candidate;
      if (DestinationAliasesSource)
        Opened = &Source;
      else if (SubstituteTemporaryNameBeforeRename) {
        FinalReplacement = Candidate;
        FinalReplacement.Status.Inode = 303;
        Opened = &FinalReplacement;
      }
      Handles.emplace(FD, Opened);
      return FD;
    };
    Ops.CreateAnchoredExclusive = [&](int DirectoryFD, llvm::StringRef Name,
                                      uint32_t Mode)
        -> llvm::Expected<detail::DarwinPublicationCreatedFileV1> {
      ++CreateCalls;
      EXPECT_EQ(DirectoryFD, kDirectoryFD);
      EXPECT_EQ(Mode, 0600u);
      if (FailCreate)
        return testError("injected create failure");
      TempPresent = true;
      TempName = Name.str() + "injected";
      Candidate = {};
      Candidate.Status.Node = PublicationNodeKind::RegularFile;
      Candidate.Status.Device = 7;
      Candidate.Status.Inode = 202;
      Candidate.Status.Generation = 9;
      Candidate.Status.LinkCount = 1;
      Candidate.Status.UID = 501;
      Candidate.Status.GID = 20;
      Candidate.Status.Mode = 0600;
      Candidate.Status.BirthSeconds = 2000;
      Candidate.Status.ModifySeconds = 2001;
      Candidate.Status.ChangeSeconds = 2002;
      Candidate.ACL = CandidateACLAtCreate;
      Handles[kCandidateFD] = &Candidate;
      return detail::DarwinPublicationCreatedFileV1{kCandidateFD, TempName};
    };
    Ops.Stat =
        [&](int FD) -> llvm::Expected<detail::DarwinPublicationFileStatusV1> {
      auto It = Handles.find(FD);
      if (It == Handles.end())
        return testError("bad injected descriptor");
      detail::DarwinPublicationFileStatusV1 Status = It->second->Status;
      if (It->second == &Source && MutateSourceDuringObservation &&
          ++SourceStatCalls == 2)
        ++Status.ChangeNanoseconds;
      if (It->second == &Directory &&
          UnconfineDirectoryAfterCandidateCreation && CreateCalls != 0)
        Status.Mode |= 0020u;
      return Status;
    };
    Ops.StatAtNoFollow = [&](int DirectoryFD, llvm::StringRef Name)
        -> llvm::Expected<
            std::optional<detail::DarwinPublicationFileStatusV1>> {
      ++StatAtCalls;
      EXPECT_EQ(DirectoryFD, kDirectoryFD);
      LastFinalName = Name.str();
      if (!DestinationPresent)
        return std::nullopt;
      return DestinationAliasesSource ? Source.Status : Candidate.Status;
    };
    Ops.ReadAt =
        [&](int FD, uint64_t Offset,
            llvm::MutableArrayRef<uint8_t> Buffer) -> llvm::Expected<size_t> {
      auto It = Handles.find(FD);
      if (It == Handles.end())
        return testError("bad injected read descriptor");
      const std::vector<uint8_t> &Bytes = It->second->Bytes;
      if (It->second == &Source && SourceEarlyEOF && Offset < Bytes.size())
        return size_t{0};
      if (It->second == &Source && AppendSourceDuringRead &&
          !SourceAppendPerformed) {
        SourceAppendPerformed = true;
        Source.Bytes.push_back(0xee);
      }
      if (Offset >= Bytes.size())
        return size_t{0};
      const size_t Count =
          std::min(Buffer.size(), Bytes.size() - static_cast<size_t>(Offset));
      std::copy_n(Bytes.begin() + static_cast<size_t>(Offset), Count,
                  Buffer.begin());
      return Count;
    };
    Ops.WriteAt = [&](int FD, uint64_t Offset,
                      llvm::ArrayRef<uint8_t> Bytes) -> llvm::Expected<size_t> {
      if (FD != kCandidateFD)
        return testError("write was not candidate-bound");
      const size_t Count = std::min(Bytes.size(), MaxWriteBytes);
      const size_t End = static_cast<size_t>(Offset) + Count;
      Candidate.Bytes.resize(End);
      std::copy_n(Bytes.begin(), Count,
                  Candidate.Bytes.begin() + static_cast<size_t>(Offset));
      Candidate.Status.Size = Candidate.Bytes.size();
      return Count;
    };
    Ops.ReadCanonicalACL = [&](int FD)
        -> llvm::Expected<std::optional<CanonicalSecurityMetadata>> {
      if (ThrowFinalObservation && FD >= 20)
        throw std::runtime_error("injected final observation throw");
      auto It = Handles.find(FD);
      if (It == Handles.end())
        return testError("bad injected ACL descriptor");
      return It->second->ACL;
    };
    Ops.WriteCanonicalACL =
        [&](int FD, const std::optional<CanonicalSecurityMetadata> &ACL)
        -> llvm::Error {
      if (FD != kCandidateFD)
        return testError("ACL write was not candidate-bound");
      ++ACLWriteCalls;
      if (!ACL)
        ++ACLClearCalls;
      Candidate.ACL = ACL;
      return llvm::Error::success();
    };
    Ops.ListXattrs = [&](int FD) -> llvm::Expected<std::vector<std::string>> {
      auto It = Handles.find(FD);
      if (It == Handles.end())
        return testError("bad injected xattr descriptor");
      std::vector<std::string> Names;
      for (const auto &Entry : It->second->Xattrs)
        Names.push_back(Entry.first);
      return Names;
    };
    Ops.ReadXattr =
        [&](int FD,
            llvm::StringRef Name) -> llvm::Expected<CanonicalSecurityMetadata> {
      auto It = Handles.find(FD);
      if (It == Handles.end())
        return testError("bad injected xattr descriptor");
      auto Value = It->second->Xattrs.find(Name.str());
      if (Value == It->second->Xattrs.end())
        return testError("injected xattr disappeared");
      return Value->second;
    };
    Ops.WriteXattr = [&](int FD, llvm::StringRef Name,
                         llvm::ArrayRef<uint8_t> Value) -> llvm::Error {
      if (FD != kCandidateFD)
        return testError("xattr write was not candidate-bound");
      Candidate.Xattrs[Name.str()] =
          CanonicalSecurityMetadata(Value.begin(), Value.end());
      return llvm::Error::success();
    };
    Ops.ChangeOwner = [&](int FD, uint32_t UID, uint32_t GID) -> llvm::Error {
      if (FD != kCandidateFD)
        return testError("owner write was not candidate-bound");
      Candidate.Status.UID = UID;
      Candidate.Status.GID = GID;
      return llvm::Error::success();
    };
    Ops.ChangeMode = [&](int FD, uint32_t Mode) -> llvm::Error {
      if (FD != kCandidateFD)
        return testError("mode write was not candidate-bound");
      Candidate.Status.Mode = Mode;
      return llvm::Error::success();
    };
    Ops.QueryVolumeCapabilities = [&](int DirectoryFD)
        -> llvm::Expected<detail::DarwinPublicationVolumeCapabilitiesV1> {
      ++QueryVolumeCalls;
      EXPECT_EQ(DirectoryFD, kDirectoryFD);
      if (FailVolumeQuery)
        return testError("injected volume capability failure");
      const bool CurrentIgnoreOwnership =
          IgnoreOwnership ||
          (EnableIgnoreOwnershipAfterCandidateCreation && CreateCalls != 0);
      return detail::DarwinPublicationVolumeCapabilitiesV1{
          RenameExclusiveKnown,
          RenameExclusive,
          NoPermissionsKnown,
          NoPermissions ||
              (EnableNoPermissionsAfterCandidateCreation && CreateCalls != 0),
          IgnoreOwnershipKnown,
          CurrentIgnoreOwnership};
    };
    Ops.RenameExclusive = [&](int FromFD, llvm::StringRef From, int ToFD,
                              llvm::StringRef To) -> NamespacePublishResultV1 {
      ++RenameCalls;
      EXPECT_EQ(FromFD, kDirectoryFD);
      EXPECT_EQ(ToFD, kDirectoryFD);
      EXPECT_EQ(From, TempName);
      LastFinalName = To.str();
      if (ForcedPublishOutcome != SanitizerPublicationOutcome::Published)
        return {ForcedPublishOutcome, "injected exclusive rename failure"};
      if (DestinationPresent)
        return {SanitizerPublicationOutcome::NotPublished,
                "destination exists"};
      DestinationPresent = true;
      DestinationAliasesSource = false;
      TempPresent = false;
      return {SanitizerPublicationOutcome::Published, {}};
    };
    Ops.UnlinkAt = [&](int DirectoryFD, llvm::StringRef Name) -> llvm::Error {
      ++UnlinkCalls;
      EXPECT_EQ(DirectoryFD, kDirectoryFD);
      EXPECT_EQ(Name, TempName);
      if (ThrowUnlink)
        throw std::runtime_error("injected unlink throw");
      TempPresent = false;
      return llvm::Error::success();
    };
    Ops.Close = [&](int FD) -> llvm::Error {
      ++CloseCalls;
      if (FD == kCandidateFD)
        ++CandidateCloseCalls;
      else if (FD == kSourceFD)
        ++SourceCloseCalls;
      else if (FD == kDirectoryFD)
        ++DirectoryCloseCalls;
      else
        ++AnchoredFinalCloseCalls;
      if (ThrowCandidateClose && FD == kCandidateFD)
        throw std::runtime_error("injected candidate close throw");
      Handles.erase(FD);
      return llvm::Error::success();
    };
    return Ops;
  }

  DarwinSanitizerPublicationRequestV1
  request(std::vector<uint8_t> CandidateBytes = {9, 8, 7, 6}) const {
    DarwinSanitizerPublicationRequestV1 Request;
    Request.SourcePath = "/source/input";
    Request.DestinationPath = "/output/final";
    Request.ExpectedSourceContent = ArtifactContentDigestV1{
        llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(Source.Bytes)),
        static_cast<uint64_t>(Source.Bytes.size())};
    Request.CandidateBytes = std::move(CandidateBytes);
    Request.GuardedSiteCount = Request.CandidateBytes == Source.Bytes ? 0 : 1;
    return Request;
  }

  static constexpr int kSourceFD = 10;
  static constexpr int kDirectoryFD = 11;
  static constexpr int kCandidateFD = 12;

  FakeFile Source;
  FakeFile Directory;
  FakeFile Candidate;
  FakeFile FinalReplacement;
  std::map<int, FakeFile *> Handles;
  int NextFD = 20;
  uint32_t EffectiveUID = 501;
  bool DestinationPresent = false;
  bool DestinationAliasesSource = false;
  bool SubstituteTemporaryNameBeforeRename = false;
  bool TempPresent = false;
  bool RenameExclusiveKnown = true;
  bool RenameExclusive = true;
  bool NoPermissionsKnown = true;
  bool NoPermissions = false;
  bool IgnoreOwnershipKnown = true;
  bool IgnoreOwnership = false;
  bool FailVolumeQuery = false;
  bool EnableNoPermissionsAfterCandidateCreation = false;
  bool EnableIgnoreOwnershipAfterCandidateCreation = false;
  bool ChangeEffectiveUIDAfterCandidateCreation = false;
  bool FailCreate = false;
  bool MutateSourceDuringObservation = false;
  bool AppendSourceDuringRead = false;
  bool SourceEarlyEOF = false;
  bool SourceAppendPerformed = false;
  bool UnconfineDirectoryAfterCandidateCreation = false;
  bool ThrowUnlink = false;
  bool ThrowCandidateClose = false;
  bool ThrowFinalObservation = false;
  std::optional<CanonicalSecurityMetadata> CandidateACLAtCreate;
  size_t MaxWriteBytes = std::numeric_limits<size_t>::max();
  SanitizerPublicationOutcome ForcedPublishOutcome =
      SanitizerPublicationOutcome::Published;
  std::string TempName;
  std::string LastSourcePath;
  std::string LastDirectoryPath;
  std::string LastFinalName;
  int OpenSourceCalls = 0;
  int EffectiveUIDCalls = 0;
  int OpenDirectoryCalls = 0;
  int OpenAnchoredCalls = 0;
  int StatAtCalls = 0;
  int QueryVolumeCalls = 0;
  int SourceStatCalls = 0;
  int CreateCalls = 0;
  int RenameCalls = 0;
  int UnlinkCalls = 0;
  int CloseCalls = 0;
  int CandidateCloseCalls = 0;
  int SourceCloseCalls = 0;
  int DirectoryCloseCalls = 0;
  int AnchoredFinalCloseCalls = 0;
  int ACLWriteCalls = 0;
  int ACLClearCalls = 0;
};

TEST(DarwinSanitizerPublicationAdapterTest,
     PreparationBindsHeldSourceParentAndCandidate) {
  FakeDarwinFilesystem FS;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());

  EXPECT_EQ(FS.OpenSourceCalls, 1);
  EXPECT_EQ(FS.OpenDirectoryCalls, 1);
  EXPECT_EQ(FS.LastDirectoryPath, "/output");
  EXPECT_EQ(FS.LastFinalName, "final");
  EXPECT_EQ(Prepared->MetadataRequest.Platform, PublicationPlatform::MacOS);
  EXPECT_EQ(Prepared->MetadataRequest.Source.Node,
            PublicationNodeKind::RegularFile);
  EXPECT_EQ(Prepared->MetadataRequest.Destination.Node,
            PublicationNodeKind::Absent);
  EXPECT_EQ(Prepared->MetadataRequest.CandidateMutation,
            CandidateMutationDisposition::ByteMutated);
  EXPECT_EQ(Prepared->Binding.ExpectedSourceContent.Size,
            FS.Source.Bytes.size());
  EXPECT_EQ(Prepared->Binding.ExpectedCandidateContent.Size, 4u);
  EXPECT_TRUE(Prepared->Operations.Capabilities.SourceIdentityPinned);
  EXPECT_TRUE(Prepared->Operations.Capabilities.DestinationDirectoryAnchored);
  EXPECT_EQ(Prepared->Operations.Capabilities.CandidatePublishOperandBinding,
            CandidatePublishOperandBindingV1::
                AccessControlConfinedDistinctCredentials);
  EXPECT_TRUE(Prepared->Operations.Capabilities.AtomicNoReplacePublish);
  EXPECT_EQ(FS.EffectiveUIDCalls, 1);

  ASSERT_FALSE(Prepared->Operations.Discard());
}

TEST(DarwinSanitizerPublicationAdapterTest,
     ExternalSourceContentMismatchPrecedesEveryMutation) {
  FakeDarwinFilesystem FS;
  DarwinSanitizerPublicationRequestV1 Request = FS.request();
  Request.ExpectedSourceContent->SHA256.front() ^= 0xff;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      std::move(Request), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError())
                .find("external source content anchor"),
            std::string::npos);
  EXPECT_EQ(FS.CreateCalls, 0);
  EXPECT_EQ(FS.RenameCalls, 0);
  EXPECT_EQ(FS.UnlinkCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     DotAndEmbeddedNulPathComponentsAreRejectedBeforeOpen) {
  FakeDarwinFilesystem FS;
  DarwinSanitizerPublicationRequestV1 Dot = FS.request();
  Dot.DestinationPath = "/output/../final";
  auto DotPrepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      std::move(Dot), FS.operations());
  ASSERT_FALSE(static_cast<bool>(DotPrepared));
  llvm::consumeError(DotPrepared.takeError());
  EXPECT_EQ(FS.OpenSourceCalls, 0);

  DarwinSanitizerPublicationRequestV1 Nul = FS.request();
  Nul.SourcePath = std::string("/source/\0input", 13);
  auto NulPrepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      std::move(Nul), FS.operations());
  ASSERT_FALSE(static_cast<bool>(NulPrepared));
  llvm::consumeError(NulPrepared.takeError());
  EXPECT_EQ(FS.OpenSourceCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     RelativePathsAreRejectedBeforeOpen) {
  FakeDarwinFilesystem SourceFS;
  DarwinSanitizerPublicationRequestV1 RelativeSource = SourceFS.request();
  RelativeSource.SourcePath = "source/input";
  auto SourcePrepared =
      detail::prepareDarwinSanitizerPublicationWithOperationsV1(
          std::move(RelativeSource), SourceFS.operations());
  ASSERT_FALSE(static_cast<bool>(SourcePrepared));
  EXPECT_NE(llvm::toString(SourcePrepared.takeError()).find("absolute"),
            std::string::npos);
  EXPECT_EQ(SourceFS.OpenSourceCalls, 0);

  FakeDarwinFilesystem DestinationFS;
  DarwinSanitizerPublicationRequestV1 RelativeDestination =
      DestinationFS.request();
  RelativeDestination.DestinationPath = "output/final";
  auto DestinationPrepared =
      detail::prepareDarwinSanitizerPublicationWithOperationsV1(
          std::move(RelativeDestination), DestinationFS.operations());
  ASSERT_FALSE(static_cast<bool>(DestinationPrepared));
  EXPECT_NE(llvm::toString(DestinationPrepared.takeError()).find("absolute"),
            std::string::npos);
  EXPECT_EQ(DestinationFS.OpenSourceCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     UnrepresentableDarwinFlagsPrecedeEveryMutation) {
  FakeDarwinFilesystem FS;
  FS.Source.Status.Flags = 0x2;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError()).find("file flags"),
            std::string::npos);
  EXPECT_EQ(FS.CreateCalls, 0);
  EXPECT_EQ(FS.RenameCalls, 0);
  EXPECT_EQ(FS.UnlinkCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     HiddenCompressionXattrFailsClosedWhenEnumerationExposesIt) {
  FakeDarwinFilesystem FS;
  // Production passes XATTR_SHOWCOMPRESSION so com.apple.decmpfs cannot be
  // hidden from this complete-enumeration policy.
  FS.Source.Xattrs["com.apple.decmpfs"] = {0x41};
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  const std::string Detail = llvm::toString(Prepared.takeError());
  EXPECT_NE(Detail.find("unsupported extended attribute"), std::string::npos);
  EXPECT_GE(FS.CloseCalls, 2);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     ContentObservationRejectsConcurrentStatusChange) {
  FakeDarwinFilesystem FS;
  FS.MutateSourceDuringObservation = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError()).find("changed while"),
            std::string::npos);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     ContentObservationIsBoundedAndRejectsAnAppendedTail) {
  FakeDarwinFilesystem FS;
  FS.AppendSourceDuringRead = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError()).find("grew while hashing"),
            std::string::npos);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     ContentObservationRejectsZeroProgressBeforeExpectedSize) {
  FakeDarwinFilesystem FS;
  FS.SourceEarlyEOF = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError()).find("EOF before"),
            std::string::npos);
  EXPECT_EQ(FS.CreateCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     InjectedDirectorySemanticMismatchFailsClosed) {
  FakeDarwinFilesystem FS;
  FS.Directory.Status.Directory = false;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError()).find("not a directory"),
            std::string::npos);
  EXPECT_EQ(FS.CreateCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     UnconfinedDestinationDirectoryFailsBeforeCandidateMutation) {
  for (int Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    FakeDarwinFilesystem FS;
    switch (Case) {
    case 0:
      FS.Directory.Status.UID = 502;
      break;
    case 1:
      FS.Directory.Status.Mode |= 0020u;
      break;
    case 2:
      FS.Directory.ACL = CanonicalSecurityMetadata{0xaa};
      break;
    case 3:
      FS.Directory.Status.Flags = 0x1;
      break;
    }
    auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
        FS.request(), FS.operations());
    ASSERT_FALSE(static_cast<bool>(Prepared));
    llvm::consumeError(Prepared.takeError());
    EXPECT_EQ(FS.CreateCalls, 0);
    EXPECT_EQ(FS.RenameCalls, 0);
    EXPECT_EQ(FS.UnlinkCalls, 0);
  }
}

TEST(DarwinSanitizerPublicationAdapterTest,
     UnattestedVolumeAccessControlFailsBeforeCandidateMutation) {
  for (int Case = 0; Case != 4; ++Case) {
    SCOPED_TRACE(Case);
    FakeDarwinFilesystem FS;
    switch (Case) {
    case 0:
      FS.NoPermissionsKnown = false;
      break;
    case 1:
      FS.NoPermissions = true;
      break;
    case 2:
      FS.IgnoreOwnershipKnown = false;
      break;
    case 3:
      FS.IgnoreOwnership = true;
      break;
    }
    auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
        FS.request(), FS.operations());
    ASSERT_FALSE(static_cast<bool>(Prepared));
    EXPECT_NE(llvm::toString(Prepared.takeError()).find("access control"),
              std::string::npos);
    EXPECT_EQ(FS.CreateCalls, 0);
    EXPECT_EQ(FS.RenameCalls, 0);
    EXPECT_EQ(FS.UnlinkCalls, 0);
  }
}

TEST(DarwinSanitizerPublicationAdapterTest,
     CreateExclusiveRunsOneRenameAndFinalizeExactlyOnce) {
  FakeDarwinFilesystem FS;
  FS.MaxWriteBytes = 2;
  // Models an inherited directory ACL that must be deleted because the held
  // source's complete ACL observation was absent.
  FS.CandidateACLAtCreate = CanonicalSecurityMetadata{0xaa, 0xbb};
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready()) << toString(Plan.Reason);

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  EXPECT_TRUE(Result.succeeded()) << Result.Detail;
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
  EXPECT_EQ(Result.Receipt.Guarantees.CandidatePublishOperandBinding,
            CandidatePublishOperandBindingV1::
                AccessControlConfinedDistinctCredentials);
  EXPECT_EQ(FS.CreateCalls, 1);
  EXPECT_EQ(FS.RenameCalls, 1);
  EXPECT_EQ(FS.UnlinkCalls, 0);
  EXPECT_TRUE(FS.DestinationPresent);
  EXPECT_EQ(FS.Candidate.Bytes, std::vector<uint8_t>({9, 8, 7, 6}));
  EXPECT_EQ(FS.Candidate.Status.Mode, 0644u);
  EXPECT_FALSE(FS.Candidate.ACL.has_value());
  EXPECT_EQ(FS.ACLWriteCalls, 1);
  EXPECT_EQ(FS.ACLClearCalls, 1);
  EXPECT_EQ(FS.OpenAnchoredCalls, 1);

  llvm::Error SecondFinalize = Prepared->Operations.FinalizePublished();
  EXPECT_TRUE(static_cast<bool>(SecondFinalize));
  llvm::consumeError(std::move(SecondFinalize));
  EXPECT_EQ(FS.RenameCalls, 1);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     TemporaryNameSubstitutionIsCaughtByAnchoredFinalIdentity) {
  FakeDarwinFilesystem FS;
  FS.SubstituteTemporaryNameBeforeRename = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready()) << toString(Plan.Reason);

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  EXPECT_FALSE(Result.succeeded());
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
  EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                               PublishedFinalAuthenticationMismatch);
  EXPECT_EQ(FS.RenameCalls, 1);
  EXPECT_EQ(FS.OpenAnchoredCalls, 1);
  EXPECT_EQ(FS.UnlinkCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     PublishedFinalObservationThrowClosesEveryDescriptorOnce) {
  FakeDarwinFilesystem FS;
  FS.ThrowFinalObservation = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready()) << toString(Plan.Reason);

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  EXPECT_FALSE(Result.succeeded());
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::CallbackException);
  EXPECT_EQ(FS.AnchoredFinalCloseCalls, 1);
  EXPECT_EQ(FS.CandidateCloseCalls, 1);
  EXPECT_EQ(FS.SourceCloseCalls, 1);
  EXPECT_EQ(FS.DirectoryCloseCalls, 1);
  EXPECT_EQ(FS.CloseCalls, 4);
  EXPECT_EQ(FS.UnlinkCalls, 0);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     CleanupThrowStillAttemptsEveryIndependentClose) {
  FakeDarwinFilesystem FS;
  FS.ThrowUnlink = true;
  FS.ThrowCandidateClose = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  ASSERT_FALSE(Prepared->Operations.Begin());
  ASSERT_FALSE(Prepared->Operations.CreateExclusiveCandidate());

  llvm::Error Cleanup = Prepared->Operations.Discard();
  EXPECT_TRUE(static_cast<bool>(Cleanup));
  llvm::consumeError(std::move(Cleanup));
  EXPECT_EQ(FS.UnlinkCalls, 1);
  EXPECT_EQ(FS.CloseCalls, 3);
  EXPECT_EQ(FS.CandidateCloseCalls, 1);
  EXPECT_EQ(FS.SourceCloseCalls, 1);
  EXPECT_EQ(FS.DirectoryCloseCalls, 1);

  llvm::Error SecondCleanup = Prepared->Operations.Discard();
  EXPECT_TRUE(static_cast<bool>(SecondCleanup));
  llvm::consumeError(std::move(SecondCleanup));
  EXPECT_EQ(FS.UnlinkCalls, 1);
  EXPECT_EQ(FS.CloseCalls, 3);
  EXPECT_EQ(FS.CandidateCloseCalls, 1);
  EXPECT_EQ(FS.SourceCloseCalls, 1);
  EXPECT_EQ(FS.DirectoryCloseCalls, 1);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     RenameRaceDiscardsTemporaryExactlyOnce) {
  FakeDarwinFilesystem FS;
  FS.ForcedPublishOutcome = SanitizerPublicationOutcome::NotPublished;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready());

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  EXPECT_FALSE(Result.succeeded());
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_EQ(FS.RenameCalls, 1);
  EXPECT_EQ(FS.UnlinkCalls, 1);

  llvm::Error SecondDiscard = Prepared->Operations.Discard();
  EXPECT_TRUE(static_cast<bool>(SecondDiscard));
  llvm::consumeError(std::move(SecondDiscard));
  EXPECT_EQ(FS.UnlinkCalls, 1);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     DirectoryConfinementIsReauthenticatedImmediatelyBeforePublish) {
  FakeDarwinFilesystem FS;
  FS.UnconfineDirectoryAfterCandidateCreation = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready());

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  EXPECT_FALSE(Result.succeeded());
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::DestinationReauthenticationFailed);
  EXPECT_EQ(FS.RenameCalls, 0);
  EXPECT_EQ(FS.UnlinkCalls, 1);
}

TEST(DarwinSanitizerPublicationAdapterTest,
     VolumeAccessControlIsReauthenticatedImmediatelyBeforePublish) {
  for (int Case = 0; Case != 2; ++Case) {
    SCOPED_TRACE(Case);
    FakeDarwinFilesystem FS;
    FS.EnableNoPermissionsAfterCandidateCreation = Case == 0;
    FS.EnableIgnoreOwnershipAfterCandidateCreation = Case == 1;
    auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
        FS.request(), FS.operations());
    ASSERT_TRUE(static_cast<bool>(Prepared))
        << llvm::toString(Prepared.takeError());
    const SanitizerPublicationMetadataPlanV1 Plan =
        planSanitizerPublicationMetadata(Prepared->MetadataRequest);
    ASSERT_TRUE(Plan.ready());

    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                            Prepared->Operations);
    EXPECT_FALSE(Result.succeeded());
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::DestinationReauthenticationFailed);
    EXPECT_EQ(FS.QueryVolumeCalls, 3);
    EXPECT_EQ(FS.RenameCalls, 0);
    EXPECT_EQ(FS.UnlinkCalls, 1);
  }
}

TEST(DarwinSanitizerPublicationAdapterTest,
     EffectiveUIDIsReauthenticatedBeforeBeginAndPublish) {
  {
    FakeDarwinFilesystem FS;
    auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
        FS.request(), FS.operations());
    ASSERT_TRUE(static_cast<bool>(Prepared))
        << llvm::toString(Prepared.takeError());
    FS.EffectiveUID = 0;
    const SanitizerPublicationMetadataPlanV1 Plan =
        planSanitizerPublicationMetadata(Prepared->MetadataRequest);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                            Prepared->Operations);
    EXPECT_FALSE(Result.succeeded());
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::BeginFailed);
    EXPECT_EQ(FS.CreateCalls, 0);
    EXPECT_EQ(FS.RenameCalls, 0);
  }

  {
    FakeDarwinFilesystem FS;
    FS.ChangeEffectiveUIDAfterCandidateCreation = true;
    auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
        FS.request(), FS.operations());
    ASSERT_TRUE(static_cast<bool>(Prepared))
        << llvm::toString(Prepared.takeError());
    const SanitizerPublicationMetadataPlanV1 Plan =
        planSanitizerPublicationMetadata(Prepared->MetadataRequest);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                            Prepared->Operations);
    EXPECT_FALSE(Result.succeeded());
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::DestinationReauthenticationFailed);
    EXPECT_EQ(FS.RenameCalls, 0);
    EXPECT_EQ(FS.UnlinkCalls, 1);
  }
}

TEST(DarwinSanitizerPublicationAdapterTest,
     NoChangeUsesOneHeldSourceAndAnchoredFinalFrame) {
  FakeDarwinFilesystem FS;
  FS.DestinationPresent = true;
  FS.DestinationAliasesSource = true;
  FS.Directory.Status.Mode = 0777;
  FS.Directory.ACL = CanonicalSecurityMetadata{0xaa};
  FS.FailVolumeQuery = true;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(FS.Source.Bytes), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  ASSERT_TRUE(Prepared->MetadataRequest.SameSourceAndDestination);
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready()) << toString(Plan.Reason);
  ASSERT_EQ(Plan.NamespaceDisposition,
            PublicationNamespaceDisposition::NoChange);

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  EXPECT_TRUE(Result.succeeded()) << Result.Detail;
  EXPECT_EQ(Prepared->Operations.Capabilities.CandidatePublishOperandBinding,
            CandidatePublishOperandBindingV1::None);
  EXPECT_EQ(FS.EffectiveUIDCalls, 0);
  EXPECT_EQ(FS.QueryVolumeCalls, 0);
  EXPECT_EQ(FS.CreateCalls, 0);
  EXPECT_EQ(FS.RenameCalls, 0);
  EXPECT_GE(FS.OpenAnchoredCalls, 2); // planning plus the one execution frame.

  ASSERT_FALSE(Prepared->Operations.Discard());
}

TEST(DarwinSanitizerPublicationAdapterTest,
     UnknownRenameCapabilityIsNeverClaimed) {
  FakeDarwinFilesystem FS;
  FS.RenameExclusiveKnown = false;
  FS.RenameExclusive = false;
  auto Prepared = detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      FS.request(), FS.operations());
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  EXPECT_FALSE(Prepared->Operations.Capabilities.AtomicNoReplacePublish);
  ASSERT_FALSE(Prepared->Operations.Discard());
}

#ifndef __APPLE__
TEST(DarwinSanitizerPublicationAdapterTest,
     NonAppleAvailabilityIsExplicitAndAllFalse) {
  const DarwinSanitizerPublicationAvailabilityV1 Availability =
      getDarwinSanitizerPublicationAvailabilityV1();
  EXPECT_FALSE(Availability.Available);
  EXPECT_FALSE(Availability.Capabilities.SourceIdentityPinned);
  EXPECT_FALSE(Availability.Capabilities.DestinationDirectoryAnchored);
  EXPECT_FALSE(Availability.Capabilities.AtomicNoReplacePublish);

  auto Prepared = prepareDarwinSanitizerPublicationV1({});
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError()).find("requires Darwin"),
            std::string::npos);
}
#endif

#ifdef __APPLE__
class ScopedNativeTempDirectory {
public:
  ScopedNativeTempDirectory() {
    llvm::SmallString<128> Created;
    Error = llvm::sys::fs::createUniqueDirectory("neverd-darwin-publication",
                                                 Created);
    if (!Error) {
      Path = Created.str().str();
      std::error_code CanonicalError;
      const std::filesystem::path Canonical =
          std::filesystem::canonical(Path, CanonicalError);
      if (CanonicalError)
        Error = CanonicalError;
      else
        Path = Canonical;
    }
  }

  ~ScopedNativeTempDirectory() {
    if (!Path.empty())
      (void)llvm::sys::fs::remove_directories(Path.string());
  }

  std::error_code Error;
  std::filesystem::path Path;
};

TEST(DarwinSanitizerPublicationAdapterNativeTest,
     PublishesInRealAnchoredTemporaryDirectory) {
  ScopedNativeTempDirectory Temp;
  ASSERT_FALSE(Temp.Error) << Temp.Error.message();
  const std::filesystem::path Source = Temp.Path / "source";
  const std::filesystem::path Destination = Temp.Path / "published";
  {
    std::ofstream Stream(Source, std::ios::binary);
    ASSERT_TRUE(Stream.good());
    Stream.write("source", 6);
  }
  ASSERT_EQ(::chmod(Source.c_str(), 0640), 0);

  DarwinSanitizerPublicationRequestV1 Request;
  Request.SourcePath = Source.string();
  Request.DestinationPath = Destination.string();
  const std::vector<uint8_t> SourceBytes{'s', 'o', 'u', 'r', 'c', 'e'};
  Request.ExpectedSourceContent = ArtifactContentDigestV1{
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(SourceBytes)),
      static_cast<uint64_t>(SourceBytes.size())};
  Request.CandidateBytes = {'c', 'a', 'n', 'd', 'i', 'd', 'a', 't', 'e'};
  Request.GuardedSiteCount = 1;
  auto Prepared = prepareDarwinSanitizerPublicationV1(std::move(Request));
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  if (!Prepared->Operations.Capabilities.AtomicNoReplacePublish) {
    llvm::consumeError(Prepared->Operations.Discard());
    GTEST_SKIP() << "temporary volume does not attest RENAME_EXCL";
  }
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Prepared->MetadataRequest);
  ASSERT_TRUE(Plan.ready()) << toString(Plan.Reason);

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Prepared->Binding,
                                          Prepared->Operations);
  ASSERT_TRUE(Result.succeeded()) << Result.Detail;
  auto Buffer = llvm::MemoryBuffer::getFile(Destination.string(), false);
  ASSERT_TRUE(static_cast<bool>(Buffer));
  EXPECT_EQ((*Buffer)->getBuffer(), "candidate");
  std::error_code ModeError;
  const auto Permissions =
      std::filesystem::status(Destination, ModeError).permissions();
  ASSERT_FALSE(ModeError) << ModeError.message();
  EXPECT_EQ(Permissions & std::filesystem::perms::mask,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write |
                std::filesystem::perms::group_read);
}

TEST(DarwinSanitizerPublicationAdapterNativeTest, RealUnknownXattrFailsClosed) {
  ScopedNativeTempDirectory Temp;
  ASSERT_FALSE(Temp.Error) << Temp.Error.message();
  const std::filesystem::path Source = Temp.Path / "source";
  const std::filesystem::path Destination = Temp.Path / "published";
  {
    std::ofstream Stream(Source, std::ios::binary);
    Stream << "source";
  }
  constexpr char Value[] = "x";
  ASSERT_EQ(::setxattr(Source.c_str(), "com.neverd.unknown", Value,
                       sizeof(Value) - 1, 0, 0),
            0);

  DarwinSanitizerPublicationRequestV1 Request;
  Request.SourcePath = Source.string();
  Request.DestinationPath = Destination.string();
  const std::vector<uint8_t> SourceBytes{'s', 'o', 'u', 'r', 'c', 'e'};
  Request.ExpectedSourceContent = ArtifactContentDigestV1{
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(SourceBytes)),
      static_cast<uint64_t>(SourceBytes.size())};
  Request.CandidateBytes = {'x'};
  auto Prepared = prepareDarwinSanitizerPublicationV1(std::move(Request));
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_NE(llvm::toString(Prepared.takeError())
                .find("unsupported extended attribute"),
            std::string::npos);
}

TEST(DarwinSanitizerPublicationAdapterNativeTest,
     RejectsSymlinkedAncestorComponents) {
  ScopedNativeTempDirectory Temp;
  ASSERT_FALSE(Temp.Error) << Temp.Error.message();
  const std::filesystem::path Real = Temp.Path / "real";
  const std::filesystem::path Link = Temp.Path / "link";
  ASSERT_TRUE(std::filesystem::create_directory(Real));
  const std::filesystem::path Source = Real / "source";
  {
    std::ofstream Stream(Source, std::ios::binary);
    Stream << "source";
  }
  ASSERT_EQ(::symlink(Real.c_str(), Link.c_str()), 0);

  const std::vector<uint8_t> SourceBytes{'s', 'o', 'u', 'r', 'c', 'e'};
  const ArtifactContentDigestV1 SourceAnchor{
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(SourceBytes)),
      static_cast<uint64_t>(SourceBytes.size())};
  auto RequestFor = [&](const std::filesystem::path &RequestedSource,
                        const std::filesystem::path &Destination) {
    DarwinSanitizerPublicationRequestV1 Request;
    Request.SourcePath = RequestedSource.string();
    Request.DestinationPath = Destination.string();
    Request.ExpectedSourceContent = SourceAnchor;
    Request.CandidateBytes = {'x'};
    return Request;
  };

  auto SymlinkedSource = prepareDarwinSanitizerPublicationV1(
      RequestFor(Link / "source", Temp.Path / "published-source"));
  ASSERT_FALSE(static_cast<bool>(SymlinkedSource));
  EXPECT_NE(llvm::toString(SymlinkedSource.takeError())
                .find("symlink-free component"),
            std::string::npos);

  auto SymlinkedDestination = prepareDarwinSanitizerPublicationV1(
      RequestFor(Source, Link / "published-destination"));
  ASSERT_FALSE(static_cast<bool>(SymlinkedDestination));
  EXPECT_NE(llvm::toString(SymlinkedDestination.takeError())
                .find("symlink-free component"),
            std::string::npos);
}
#else
TEST(DarwinSanitizerPublicationAdapterNativeTest,
     RealDirectoryCoverageRequiresApple) {
  GTEST_SKIP() << "Darwin filesystem adapter requires Apple APIs";
}
#endif

} // namespace
