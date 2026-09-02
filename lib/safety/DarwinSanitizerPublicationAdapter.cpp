//===- DarwinSanitizerPublicationAdapter.cpp - Darwin publication -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/DarwinSanitizerPublicationAdapter.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <cerrno>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <sys/acl.h>
#include <sys/attr.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <system_error>
#include <unistd.h>
#endif

namespace neverd::safety::sanitizer_publication_metadata {
namespace {

using detail::DarwinPublicationCreatedFileV1;
using detail::DarwinPublicationFileStatusV1;
using detail::DarwinPublicationVolumeCapabilitiesV1;
using detail::DarwinSanitizerPublicationNativeOperationsV1;

constexpr llvm::StringLiteral kQuarantineXattr("com.apple.quarantine");
constexpr llvm::StringLiteral kProvenanceXattr("com.apple.provenance");
constexpr llvm::StringLiteral kTemporaryPrefix(".neverd-sanitize-");
constexpr size_t kIOChunkBytes = 64 * 1024;

bool hasDotPathComponent(llvm::StringRef Path) {
  for (auto It = llvm::sys::path::begin(Path, llvm::sys::path::Style::posix),
            End = llvm::sys::path::end(Path);
       It != End; ++It)
    if (*It == "." || *It == "..")
      return true;
  return false;
}

llvm::Error adapterError(llvm::StringRef Detail) {
  return llvm::createStringError(llvm::errc::operation_not_permitted, "%s",
                                 Detail.str().c_str());
}

llvm::Error appendError(llvm::Error First, llvm::Error Second) {
  if (!First)
    return Second;
  if (!Second)
    return First;
  return llvm::joinErrors(std::move(First), std::move(Second));
}

template <typename IntegerT>
void appendLittleEndian(std::vector<uint8_t> &Bytes, IntegerT Value) {
  using UnsignedT = std::make_unsigned_t<IntegerT>;
  UnsignedT Bits = static_cast<UnsignedT>(Value);
  for (size_t Index = 0; Index < sizeof(UnsignedT); ++Index) {
    Bytes.push_back(static_cast<uint8_t>(Bits & 0xff));
    Bits >>= 8;
  }
}

CanonicalSecurityMetadata
canonicalOwner(const DarwinPublicationFileStatusV1 &Status) {
  CanonicalSecurityMetadata Bytes;
  Bytes.reserve(8);
  appendLittleEndian<uint32_t>(Bytes, Status.UID);
  appendLittleEndian<uint32_t>(Bytes, Status.GID);
  return Bytes;
}

CanonicalSecurityMetadata
canonicalPOSIXMode(const DarwinPublicationFileStatusV1 &Status) {
  const uint32_t Mode = Status.Mode & 01777u;
  CanonicalSecurityMetadata Bytes;
  Bytes.reserve(kSanitizerPublicationPOSIXModeEncodedBytes);
  appendLittleEndian<uint32_t>(Bytes, Mode);
  return Bytes;
}

llvm::Expected<std::pair<uint32_t, uint32_t>>
decodeOwner(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != 8)
    return adapterError("canonical Darwin owner is not eight bytes");
  auto Read32 = [&](size_t Offset) {
    return static_cast<uint32_t>(Bytes[Offset]) |
           (static_cast<uint32_t>(Bytes[Offset + 1]) << 8) |
           (static_cast<uint32_t>(Bytes[Offset + 2]) << 16) |
           (static_cast<uint32_t>(Bytes[Offset + 3]) << 24);
  };
  return std::make_pair(Read32(0), Read32(4));
}

llvm::Expected<uint32_t> decodeMode(llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.size() != kSanitizerPublicationPOSIXModeEncodedBytes)
    return adapterError("canonical Darwin POSIX mode is not four bytes");
  const uint32_t Mode = static_cast<uint32_t>(Bytes[0]) |
                        (static_cast<uint32_t>(Bytes[1]) << 8) |
                        (static_cast<uint32_t>(Bytes[2]) << 16) |
                        (static_cast<uint32_t>(Bytes[3]) << 24);
  if ((Mode & ~01777u) != 0)
    return adapterError(
        "canonical Darwin POSIX mode contains unrepresentable bits");
  return Mode;
}

ArtifactStableIdentityDigestV1
stableIdentity(const DarwinPublicationFileStatusV1 &Status) {
  // Do not add ctime or mtime here: every owner/ACL/xattr/mode application may
  // legitimately change ctime while the candidate remains the same object.
  static constexpr std::array<uint8_t, 32> Domain = {
      'N', 'e', 'v', 'e', 'r', 'D', '.', 'D', 'a', 'r', 'w',
      'i', 'n', '.', 'S', 't', 'a', 'b', 'l', 'e', 'I', 'd',
      'e', 'n', 't', 'i', 't', 'y', '.', 'v', '1', 0};
  std::vector<uint8_t> Encoding(Domain.begin(), Domain.end());
  Encoding.reserve(Encoding.size() + 8 * 6 + 1);
  appendLittleEndian<uint64_t>(Encoding, Status.Device);
  appendLittleEndian<uint64_t>(Encoding, Status.Inode);
  appendLittleEndian<int64_t>(Encoding, Status.BirthSeconds);
  appendLittleEndian<int64_t>(Encoding, Status.BirthNanoseconds);
  Encoding.push_back(Status.Generation == 0 ? 0 : 1);
  appendLittleEndian<uint64_t>(Encoding, Status.Generation);
  ArtifactStableIdentityDigestV1 Digest;
  Digest.SHA256 = llvm::SHA256::hash(Encoding);
  return Digest;
}

llvm::Error validateArtifactStatus(const DarwinPublicationFileStatusV1 &Status,
                                   llvm::StringRef Role) {
  if (Status.Node != PublicationNodeKind::RegularFile)
    return adapterError((Role + " is not a held regular file").str());
  if (Status.LinkCount != 1)
    return adapterError((Role + " does not have exactly one link").str());
  if (Status.Device == 0 || Status.Inode == 0)
    return adapterError((Role + " has no usable transaction identity").str());
  if (Status.Flags != 0)
    return adapterError(
        (Role + " has Darwin file flags that plan-v1 cannot express").str());
  return llvm::Error::success();
}

llvm::Expected<ArtifactContentDigestV1>
hashHeldFile(int FD, const DarwinPublicationFileStatusV1 &Before,
             const DarwinSanitizerPublicationNativeOperationsV1 &Native,
             llvm::StringRef Role) {
  llvm::SHA256 Hash;
  std::array<uint8_t, kIOChunkBytes> Buffer{};
  uint64_t Offset = 0;
  while (Offset < Before.Size) {
    const uint64_t Remaining = Before.Size - Offset;
    const size_t Requested =
        static_cast<size_t>(std::min<uint64_t>(Remaining, Buffer.size()));
    llvm::Expected<size_t> Read = Native.ReadAt(
        FD, Offset, llvm::MutableArrayRef<uint8_t>(Buffer.data(), Requested));
    if (!Read)
      return Read.takeError();
    if (*Read > Requested)
      return adapterError((Role + " read returned an impossible size").str());
    if (*Read == 0)
      return adapterError((Role + " reached EOF before its stated size").str());
    if (*Read > std::numeric_limits<uint64_t>::max() - Offset)
      return adapterError((Role + " byte count overflowed").str());
    Hash.update(llvm::ArrayRef<uint8_t>(Buffer.data(), *Read));
    Offset += *Read;
  }

  // The bounded loop cannot be held hostage by a concurrent appender.  A
  // single-byte probe proves the observed EOF before the post-read stat closes
  // the complete snapshot frame.
  llvm::Expected<size_t> Tail = Native.ReadAt(
      FD, Offset, llvm::MutableArrayRef<uint8_t>(Buffer.data(), 1));
  if (!Tail)
    return Tail.takeError();
  if (*Tail > 1)
    return adapterError(
        (Role + " tail probe returned an impossible size").str());
  if (*Tail != 0)
    return adapterError((Role + " grew while hashing").str());
  return ArtifactContentDigestV1{Hash.final(), Offset};
}

struct ObservedDarwinFile {
  ArtifactAuthenticationV1 Artifact;
  CompleteMetadataSnapshotV1 CompleteMetadata;
  FileSecurityMetadataSnapshot PlannerMetadata;
};

llvm::Expected<ObservedDarwinFile>
observeHeldFile(int FD,
                const DarwinSanitizerPublicationNativeOperationsV1 &Native,
                llvm::StringRef Role) {
  llvm::Expected<DarwinPublicationFileStatusV1> Before = Native.Stat(FD);
  if (!Before)
    return Before.takeError();
  if (llvm::Error Error = validateArtifactStatus(*Before, Role))
    return std::move(Error);

  llvm::Expected<ArtifactContentDigestV1> Content =
      hashHeldFile(FD, *Before, Native, Role);
  if (!Content)
    return Content.takeError();

  uint64_t EncodedBytes = 0;
  auto Charge = [&](size_t Amount) -> llvm::Error {
    if (Amount > std::numeric_limits<uint64_t>::max() - EncodedBytes)
      return adapterError("Darwin metadata byte count overflowed");
    EncodedBytes += Amount;
    if (EncodedBytes > kSanitizerPublicationMetadataMaxEncodedBytes)
      return adapterError("Darwin metadata exceeds the plan-v1 byte ceiling");
    return llvm::Error::success();
  };

  const CanonicalSecurityMetadata Owner = canonicalOwner(*Before);
  const CanonicalSecurityMetadata Mode = canonicalPOSIXMode(*Before);
  if (llvm::Error Error = Charge(Owner.size()))
    return std::move(Error);
  if (llvm::Error Error = Charge(Mode.size()))
    return std::move(Error);

  llvm::Expected<std::optional<CanonicalSecurityMetadata>> ACL =
      Native.ReadCanonicalACL(FD);
  if (!ACL)
    return ACL.takeError();
  if (*ACL)
    if (llvm::Error Error = Charge((*ACL)->size()))
      return std::move(Error);

  llvm::Expected<std::vector<std::string>> XattrNames = Native.ListXattrs(FD);
  if (!XattrNames)
    return XattrNames.takeError();
  std::sort(XattrNames->begin(), XattrNames->end());
  if (std::adjacent_find(XattrNames->begin(), XattrNames->end()) !=
      XattrNames->end())
    return adapterError("Darwin xattr enumeration returned a duplicate name");
  if (XattrNames->size() > kSanitizerPublicationMetadataMaxRecords)
    return adapterError("Darwin xattr enumeration exceeds the v1 record limit");

  std::map<SecurityMetadataSubject, CanonicalSecurityMetadata> Xattrs;
  for (const std::string &Name : *XattrNames) {
    SecurityMetadataSubject Subject;
    if (Name == kQuarantineXattr)
      Subject = SecurityMetadataSubject::MacOSQuarantine;
    else if (Name == kProvenanceXattr)
      Subject = SecurityMetadataSubject::MacOSProvenance;
    else
      return adapterError(
          "unsupported extended attribute on held Darwin file: " + Name);
    llvm::Expected<CanonicalSecurityMetadata> Value =
        Native.ReadXattr(FD, Name);
    if (!Value)
      return Value.takeError();
    if (llvm::Error Error = Charge(Value->size()))
      return std::move(Error);
    if (!Xattrs.try_emplace(Subject, std::move(*Value)).second)
      return adapterError(
          "Darwin xattr enumeration mapped two names to one subject");
  }

  llvm::Expected<DarwinPublicationFileStatusV1> After = Native.Stat(FD);
  if (!After)
    return After.takeError();
  if (*Before != *After)
    return adapterError(
        (Role + " changed while content and metadata were read").str());

  ObservedDarwinFile Observation;
  Observation.Artifact.Node = Before->Node;
  Observation.Artifact.LinkCount = Before->LinkCount;
  Observation.Artifact.Content = *Content;
  Observation.Artifact.StableIdentity = stableIdentity(*Before);

  Observation.CompleteMetadata.EnumerationComplete = true;
  Observation.CompleteMetadata.Values.push_back(
      {SecurityMetadataSubject::Owner, Owner});
  if (*ACL)
    Observation.CompleteMetadata.Values.push_back(
        {SecurityMetadataSubject::ACL, **ACL});
  if ((Before->Mode & 04000u) != 0)
    Observation.CompleteMetadata.Values.push_back(
        {SecurityMetadataSubject::SetUIDBit, {}});
  if ((Before->Mode & 02000u) != 0)
    Observation.CompleteMetadata.Values.push_back(
        {SecurityMetadataSubject::SetGIDBit, {}});
  for (const auto &Entry : Xattrs)
    Observation.CompleteMetadata.Values.push_back({Entry.first, Entry.second});
  Observation.CompleteMetadata.Values.push_back(
      {SecurityMetadataSubject::POSIXMode, Mode});
  std::sort(Observation.CompleteMetadata.Values.begin(),
            Observation.CompleteMetadata.Values.end(),
            [](const CompleteMetadataValueV1 &Left,
               const CompleteMetadataValueV1 &Right) {
              return static_cast<uint8_t>(Left.Subject) <
                     static_cast<uint8_t>(Right.Subject);
            });

  FileSecurityMetadataSnapshot &Planner = Observation.PlannerMetadata;
  Planner.Node = Before->Node;
  Planner.LinkCount = Before->LinkCount;
  Planner.OwnerReadable = true;
  Planner.Owner = digestCanonicalSecurityMetadata(Owner);
  Planner.ACLReadable = true;
  if (*ACL)
    Planner.ACL = digestCanonicalSecurityMetadata(**ACL);
  Planner.POSIXModeReadable = true;
  Planner.POSIXMode = digestCanonicalSecurityMetadata(Mode);
  Planner.SetUID = (Before->Mode & 04000u) != 0;
  Planner.SetGID = (Before->Mode & 02000u) != 0;
  Planner.AttributeEnumerationComplete = true;
  for (const auto &Entry : Xattrs) {
    SecurityMetadataKind Kind;
    switch (Entry.first) {
    case SecurityMetadataSubject::MacOSQuarantine:
      Kind = SecurityMetadataKind::MacOSQuarantine;
      break;
    case SecurityMetadataSubject::MacOSProvenance:
      Kind = SecurityMetadataKind::MacOSProvenance;
      break;
    default:
      return adapterError("internal Darwin xattr subject is invalid");
    }
    Planner.Attributes.push_back(
        {Kind, SecurityMetadataOrigin::SourceSnapshot, true,
         digestCanonicalSecurityMetadata(Entry.second)});
  }
  return Observation;
}

llvm::Expected<ArtifactAuthenticationV1>
authenticateHeldFile(int FD,
                     const DarwinSanitizerPublicationNativeOperationsV1 &Native,
                     llvm::StringRef Role) {
  llvm::Expected<DarwinPublicationFileStatusV1> Before = Native.Stat(FD);
  if (!Before)
    return Before.takeError();
  if (llvm::Error Error = validateArtifactStatus(*Before, Role))
    return std::move(Error);
  llvm::Expected<ArtifactContentDigestV1> Content =
      hashHeldFile(FD, *Before, Native, Role);
  if (!Content)
    return Content.takeError();
  llvm::Expected<DarwinPublicationFileStatusV1> After = Native.Stat(FD);
  if (!After)
    return After.takeError();
  if (*Before != *After)
    return adapterError((Role + " changed while content was read").str());
  return ArtifactAuthenticationV1{Before->Node, Before->LinkCount, *Content,
                                  stableIdentity(*Before)};
}

bool sameHeldObject(const ArtifactAuthenticationV1 &Left,
                    const ArtifactAuthenticationV1 &Right) {
  return Left.StableIdentity == Right.StableIdentity;
}

bool sameDirectoryIdentity(const DarwinPublicationFileStatusV1 &Left,
                           const DarwinPublicationFileStatusV1 &Right) {
  return Left.Device == Right.Device && Left.Inode == Right.Inode &&
         Left.Generation == Right.Generation &&
         Left.BirthSeconds == Right.BirthSeconds &&
         Left.BirthNanoseconds == Right.BirthNanoseconds;
}

llvm::Expected<DarwinPublicationFileStatusV1> observeConfinedDirectory(
    int FD, uint32_t EffectiveUID,
    const DarwinSanitizerPublicationNativeOperationsV1 &Native) {
  llvm::Expected<DarwinPublicationFileStatusV1> Before = Native.Stat(FD);
  if (!Before)
    return Before.takeError();
  if (!Before->Directory || Before->Device == 0 || Before->Inode == 0)
    return adapterError(
        "anchored Darwin destination is not an identifiable directory");
  if (Before->UID != EffectiveUID)
    return adapterError("anchored Darwin destination directory is not owned by "
                        "the effective uid");
  if ((Before->Mode & 0022u) != 0)
    return adapterError(
        "anchored Darwin destination directory permits group/other writers");
  if (Before->Flags != 0)
    return adapterError(
        "anchored Darwin destination directory has unrepresentable flags");

  llvm::Expected<std::optional<CanonicalSecurityMetadata>> ACL =
      Native.ReadCanonicalACL(FD);
  if (!ACL)
    return ACL.takeError();
  if (*ACL)
    return adapterError(
        "anchored Darwin destination directory has an extended ACL");

  llvm::Expected<DarwinPublicationFileStatusV1> After = Native.Stat(FD);
  if (!After)
    return After.takeError();
  if (*Before != *After)
    return adapterError("anchored Darwin destination directory changed during "
                        "confinement observation");
  return *Before;
}

llvm::Error validateVolumeAccessControl(
    const DarwinPublicationVolumeCapabilitiesV1 &Capabilities) {
  if (!Capabilities.NoPermissionsKnown)
    return adapterError(
        "Darwin volume access control has unknown POSIX permission support");
  if (Capabilities.NoPermissions)
    return adapterError(
        "Darwin volume access control does not support POSIX permissions");
  if (!Capabilities.IgnoreOwnershipKnown)
    return adapterError(
        "Darwin volume access control has unknown ownership enforcement");
  if (Capabilities.IgnoreOwnership)
    return adapterError(
        "Darwin volume access control is configured to ignore ownership");
  return llvm::Error::success();
}

llvm::Error validateNativeOperations(
    const DarwinSanitizerPublicationNativeOperationsV1 &Operations) {
  if (!Operations.GetEffectiveUserID || !Operations.OpenSourceNoFollow ||
      !Operations.OpenDirectoryNoFollow ||
      !Operations.OpenAnchoredReadOnlyNoFollow ||
      !Operations.CreateAnchoredExclusive || !Operations.Stat ||
      !Operations.StatAtNoFollow || !Operations.ReadAt || !Operations.WriteAt ||
      !Operations.ReadCanonicalACL || !Operations.WriteCanonicalACL ||
      !Operations.ListXattrs || !Operations.ReadXattr ||
      !Operations.WriteXattr || !Operations.ChangeOwner ||
      !Operations.ChangeMode || !Operations.QueryVolumeCapabilities ||
      !Operations.RenameExclusive || !Operations.UnlinkAt || !Operations.Close)
    return adapterError("Darwin publication syscall boundary is incomplete");
  return llvm::Error::success();
}

struct DarwinPublicationState {
  explicit DarwinPublicationState(
      DarwinSanitizerPublicationNativeOperationsV1 NativeOperations)
      : Native(std::move(NativeOperations)) {}

  ~DarwinPublicationState() {
    if (Terminal)
      return;
    try {
      llvm::consumeError(cleanup(!Published, false));
    } catch (...) {
      // Destructors cannot safely surface a malicious test seam exception.
    }
  }

  llvm::Error closeOne(int &FD) {
    if (FD == -1)
      return llvm::Error::success();
    const int Closing = std::exchange(FD, -1);
    return Native.Close(Closing);
  }

  llvm::Error cleanup(bool RemoveTemporary, bool EnforceExactlyOnce) {
    if (Terminal) {
      if (EnforceExactlyOnce)
        return adapterError("Darwin publication cleanup was already consumed");
      return llvm::Error::success();
    }
    std::optional<llvm::Error> FirstError;
    bool CallbackThrew = false;
    auto Invoke = [&](auto &&Callback) {
      try {
        llvm::Error Error = Callback();
        if (!Error)
          return;
        if (!FirstError)
          FirstError.emplace(std::move(Error));
        else
          llvm::consumeError(std::move(Error));
      } catch (...) {
        // Do not allocate, join errors, or inspect a foreign exception until
        // every independent cleanup action has had its one attempt.
        CallbackThrew = true;
      }
    };
    if (RemoveTemporary && !TemporaryName.empty()) {
      const std::string Removing = std::exchange(TemporaryName, {});
      Invoke([&] { return Native.UnlinkAt(DirectoryFD, Removing); });
    }
    Invoke([&] { return closeOne(CandidateFD); });
    Invoke([&] { return closeOne(SourceFD); });
    Invoke([&] { return closeOne(DirectoryFD); });
    Terminal = true;
    if (CallbackThrew) {
      if (FirstError)
        llvm::consumeError(std::move(*FirstError));
      return adapterError("Darwin publication cleanup callback threw after "
                          "best-effort cleanup");
    }
    if (FirstError)
      return std::move(*FirstError);
    return llvm::Error::success();
  }

  llvm::Expected<ObservedDarwinFile> openAndObserveFinal(llvm::StringRef Role) {
    llvm::Expected<int> Opened =
        Native.OpenAnchoredReadOnlyNoFollow(DirectoryFD, FinalName);
    if (!Opened)
      return Opened.takeError();
    int FinalFD = *Opened;
    llvm::scope_exit CloseOnException([&] {
      if (FinalFD == -1)
        return;
      const int Closing = std::exchange(FinalFD, -1);
      try {
        llvm::consumeError(Native.Close(Closing));
      } catch (...) {
        // A close attempt is never retried because the descriptor number may
        // already have been released and reused.
      }
    });
    llvm::Expected<ObservedDarwinFile> Observation =
        observeHeldFile(FinalFD, Native, Role);
    llvm::Error CloseError = closeOne(FinalFD);
    if (!Observation)
      return appendError(Observation.takeError(), std::move(CloseError));
    if (CloseError)
      return std::move(CloseError);
    return std::move(*Observation);
  }

  llvm::Expected<CanonicalSecurityMetadata>
  readSourceMaterial(SecurityMetadataSubject Subject,
                     SecurityMetadataOrigin Origin) {
    if (Origin != SecurityMetadataOrigin::SourceSnapshot)
      return adapterError(
          "Darwin adapter has no final-path-derived metadata material");
    llvm::Expected<DarwinPublicationFileStatusV1> Status =
        Native.Stat(SourceFD);
    if (!Status)
      return Status.takeError();
    if (llvm::Error Error = validateArtifactStatus(*Status, "held source"))
      return std::move(Error);
    switch (Subject) {
    case SecurityMetadataSubject::Owner:
      return canonicalOwner(*Status);
    case SecurityMetadataSubject::ACL: {
      llvm::Expected<std::optional<CanonicalSecurityMetadata>> ACL =
          Native.ReadCanonicalACL(SourceFD);
      if (!ACL)
        return ACL.takeError();
      if (!*ACL)
        return adapterError("held source ACL disappeared after planning");
      return std::move(**ACL);
    }
    case SecurityMetadataSubject::MacOSQuarantine:
      return Native.ReadXattr(SourceFD, kQuarantineXattr);
    case SecurityMetadataSubject::MacOSProvenance:
      return Native.ReadXattr(SourceFD, kProvenanceXattr);
    case SecurityMetadataSubject::POSIXMode:
      return canonicalPOSIXMode(*Status);
    default:
      return adapterError(
          "Darwin adapter cannot materialize this metadata subject");
    }
  }

  llvm::Error apply(const SecurityMetadataAction &Action,
                    llvm::ArrayRef<uint8_t> Material) {
    if (CandidateFD == -1)
      return adapterError("Darwin candidate metadata has no held descriptor");
    switch (Action.Subject) {
    case SecurityMetadataSubject::Owner: {
      if (Action.Disposition != SecurityMetadataDisposition::PreserveFromSource)
        return adapterError("Darwin owner action is not preserve-from-source");
      llvm::Expected<std::pair<uint32_t, uint32_t>> Owner =
          decodeOwner(Material);
      if (!Owner)
        return Owner.takeError();
      return Native.ChangeOwner(CandidateFD, Owner->first, Owner->second);
    }
    case SecurityMetadataSubject::ACL:
      if (Action.Disposition == SecurityMetadataDisposition::PreserveFromSource)
        return Native.WriteCanonicalACL(
            CandidateFD,
            CanonicalSecurityMetadata(Material.begin(), Material.end()));
      if (Action.Disposition == SecurityMetadataDisposition::Clear)
        return Native.WriteCanonicalACL(CandidateFD, std::nullopt);
      return adapterError("Darwin ACL action has an invalid disposition");
    case SecurityMetadataSubject::SetUIDBit:
    case SecurityMetadataSubject::SetGIDBit: {
      if (Action.Disposition != SecurityMetadataDisposition::Clear ||
          !Material.empty())
        return adapterError(
            "Darwin set-id action is not a material-free clear");
      llvm::Expected<DarwinPublicationFileStatusV1> Status =
          Native.Stat(CandidateFD);
      if (!Status)
        return Status.takeError();
      if (llvm::Error Error = validateArtifactStatus(*Status, "held candidate"))
        return Error;
      const uint32_t Bit = Action.Subject == SecurityMetadataSubject::SetUIDBit
                               ? 04000u
                               : 02000u;
      return Native.ChangeMode(CandidateFD, Status->Mode & ~Bit);
    }
    case SecurityMetadataSubject::MacOSQuarantine:
    case SecurityMetadataSubject::MacOSProvenance: {
      if (Action.Disposition != SecurityMetadataDisposition::PreserveFromSource)
        return adapterError("Darwin xattr action is not preserve-from-source");
      const llvm::StringRef Name =
          Action.Subject == SecurityMetadataSubject::MacOSQuarantine
              ? kQuarantineXattr
              : kProvenanceXattr;
      return Native.WriteXattr(CandidateFD, Name, Material);
    }
    case SecurityMetadataSubject::POSIXMode: {
      if (Action.Disposition != SecurityMetadataDisposition::PreserveFromSource)
        return adapterError("Darwin mode action is not preserve-from-source");
      llvm::Expected<uint32_t> Mode = decodeMode(Material);
      if (!Mode)
        return Mode.takeError();
      return Native.ChangeMode(CandidateFD, *Mode);
    }
    case SecurityMetadataSubject::LinuxCapability:
    case SecurityMetadataSubject::LinuxSELinux:
    case SecurityMetadataSubject::WindowsMOTW:
      return adapterError("cross-platform metadata reached Darwin adapter");
    }
    return adapterError("unknown metadata subject reached Darwin adapter");
  }

  DarwinSanitizerPublicationNativeOperationsV1 Native;
  int SourceFD = -1;
  int DirectoryFD = -1;
  int CandidateFD = -1;
  std::string FinalName;
  std::string TemporaryName;
  std::vector<uint8_t> CandidateBytes;
  uint32_t EffectiveUID = 0;
  DarwinPublicationFileStatusV1 InitialDirectoryStatus;
  ArtifactAuthenticationV1 InitialSourceArtifact;
  bool RenameExclusive = false;
  bool DirectoryNamespaceConfined = false;
  bool Begun = false;
  bool NoChangeConsumed = false;
  bool PublishAttempted = false;
  bool Published = false;
  bool Terminal = false;
};

SanitizerPublicationExecutorOperationsV1
makeExecutorOperations(const std::shared_ptr<DarwinPublicationState> &State) {
  SanitizerPublicationExecutorOperationsV1 Operations;
  Operations.Capabilities.SourceIdentityPinned = true;
  Operations.Capabilities.DestinationDirectoryAnchored = true;
  Operations.Capabilities.TemporaryCreationExclusive = true;
  Operations.Capabilities.CandidatePublishOperandBinding =
      State->DirectoryNamespaceConfined
          ? CandidatePublishOperandBindingV1::
                AccessControlConfinedDistinctCredentials
          : CandidatePublishOperandBindingV1::None;
  Operations.Capabilities.CompleteSourceMetadataEnumeration = true;
  Operations.Capabilities.CompleteCandidateMetadataEnumeration = true;
  Operations.Capabilities.CompletePublishedFinalMetadataEnumeration = true;
  Operations.Capabilities.ArtifactContentAuthentication = true;
  Operations.Capabilities.StableObjectIdentityAuthentication = true;
  Operations.Capabilities.AnchoredNoChangeFinalAuthentication = true;
  Operations.Capabilities.AnchoredPublishedFinalAuthentication = true;
  Operations.Capabilities.AtomicNoReplacePublish = State->RenameExclusive;

  Operations.ReauthenticateNoChange =
      [State]() -> llvm::Expected<AuthenticatedNoChangeArtifactsV1> {
    if (State->Terminal || State->Begun || State->NoChangeConsumed)
      return adapterError("Darwin no-change frame is not available");
    State->NoChangeConsumed = true;
    llvm::Expected<int> Final = State->Native.OpenAnchoredReadOnlyNoFollow(
        State->DirectoryFD, State->FinalName);
    if (!Final)
      return Final.takeError();
    int FinalFD = *Final;
    llvm::scope_exit CloseOnException([&] {
      if (FinalFD == -1)
        return;
      const int Closing = std::exchange(FinalFD, -1);
      try {
        llvm::consumeError(State->Native.Close(Closing));
      } catch (...) {
        // The close was attempted; retrying its numeric fd is unsafe.
      }
    });
    llvm::Expected<ObservedDarwinFile> Source = observeHeldFile(
        State->SourceFD, State->Native, "no-change held source");
    llvm::Expected<ObservedDarwinFile> Anchored =
        observeHeldFile(FinalFD, State->Native, "no-change anchored final");
    llvm::Error CloseError = State->closeOne(FinalFD);
    if (!Source) {
      llvm::Error Error = Source.takeError();
      if (!Anchored)
        Error = appendError(std::move(Error), Anchored.takeError());
      return appendError(std::move(Error), std::move(CloseError));
    }
    if (!Anchored)
      return appendError(Anchored.takeError(), std::move(CloseError));
    if (CloseError)
      return std::move(CloseError);
    if (!sameHeldObject(Source->Artifact, Anchored->Artifact))
      return adapterError(
          "no-change anchored final is not the held source object");
    return AuthenticatedNoChangeArtifactsV1{
        {Source->Artifact, Source->CompleteMetadata},
        {Anchored->Artifact, Anchored->CompleteMetadata}};
  };
  Operations.Begin = [State]() -> llvm::Error {
    if (State->Terminal || State->Begun || State->NoChangeConsumed)
      return adapterError("Darwin publication Begin was already consumed");
    if (!State->DirectoryNamespaceConfined)
      return adapterError(
          "Darwin create-exclusive directory was not access-control confined");
    llvm::Expected<DarwinPublicationFileStatusV1> Source =
        State->Native.Stat(State->SourceFD);
    if (!Source)
      return Source.takeError();
    if (llvm::Error Error = validateArtifactStatus(*Source, "held source"))
      return Error;
    if (stableIdentity(*Source) != State->InitialSourceArtifact.StableIdentity)
      return adapterError("held source identity changed before Begin");
    llvm::Expected<uint32_t> EffectiveUID = State->Native.GetEffectiveUserID();
    if (!EffectiveUID)
      return EffectiveUID.takeError();
    if (*EffectiveUID != State->EffectiveUID)
      return adapterError(
          "Darwin publication effective uid changed before Begin");
    llvm::Expected<DarwinPublicationVolumeCapabilitiesV1> Volume =
        State->Native.QueryVolumeCapabilities(State->DirectoryFD);
    if (!Volume)
      return Volume.takeError();
    if (llvm::Error Error = validateVolumeAccessControl(*Volume))
      return Error;
    llvm::Expected<DarwinPublicationFileStatusV1> Directory =
        observeConfinedDirectory(State->DirectoryFD, State->EffectiveUID,
                                 State->Native);
    if (!Directory)
      return Directory.takeError();
    if (!sameDirectoryIdentity(*Directory, State->InitialDirectoryStatus))
      return adapterError("anchored destination directory identity changed");
    State->Begun = true;
    return llvm::Error::success();
  };
  Operations.AuthenticateSourceAfterBegin =
      [State]() -> llvm::Expected<AuthenticatedSourceArtifactV1> {
    if (!State->Begun || State->Terminal)
      return adapterError("Darwin source authentication is out of sequence");
    llvm::Expected<ObservedDarwinFile> Source = observeHeldFile(
        State->SourceFD, State->Native, "held source after Begin");
    if (!Source)
      return Source.takeError();
    return AuthenticatedSourceArtifactV1{Source->Artifact,
                                         Source->CompleteMetadata};
  };
  Operations.CreateExclusiveCandidate = [State]() -> llvm::Error {
    if (!State->Begun || State->Terminal || State->CandidateFD != -1)
      return adapterError("Darwin candidate creation is out of sequence");
    llvm::Expected<DarwinPublicationCreatedFileV1> Created =
        State->Native.CreateAnchoredExclusive(State->DirectoryFD,
                                              kTemporaryPrefix, 0600);
    if (!Created)
      return Created.takeError();
    if (Created->Descriptor < 0 || Created->Name.empty() ||
        Created->Name == "." || Created->Name == ".." ||
        llvm::StringRef(Created->Name).contains('\0') ||
        llvm::sys::path::filename(
            Created->Name, llvm::sys::path::Style::posix) != Created->Name) {
      if (Created->Descriptor >= 0)
        llvm::consumeError(State->Native.Close(Created->Descriptor));
      return adapterError("Darwin exclusive temp returned an invalid name");
    }
    State->CandidateFD = Created->Descriptor;
    State->TemporaryName = std::move(Created->Name);
    uint64_t Offset = 0;
    while (Offset < State->CandidateBytes.size()) {
      const size_t Remaining =
          State->CandidateBytes.size() - static_cast<size_t>(Offset);
      llvm::Expected<size_t> Written = State->Native.WriteAt(
          State->CandidateFD, Offset,
          llvm::ArrayRef<uint8_t>(State->CandidateBytes.data() +
                                      static_cast<size_t>(Offset),
                                  Remaining));
      if (!Written)
        return Written.takeError();
      if (*Written == 0 || *Written > Remaining)
        return adapterError("Darwin candidate write made invalid progress");
      Offset += *Written;
    }
    return llvm::Error::success();
  };
  Operations.AuthenticateCandidateAfterCreate =
      [State]() -> llvm::Expected<ArtifactAuthenticationV1> {
    if (State->CandidateFD == -1 || State->Terminal)
      return adapterError("Darwin candidate authentication is out of sequence");
    return authenticateHeldFile(State->CandidateFD, State->Native,
                                "new held candidate");
  };
  Operations.ReadMaterial = [State](SecurityMetadataSubject Subject,
                                    SecurityMetadataOrigin Origin)
      -> llvm::Expected<CanonicalSecurityMetadata> {
    if (!State->Begun || State->Terminal)
      return adapterError("Darwin material read is out of sequence");
    return State->readSourceMaterial(Subject, Origin);
  };
  Operations.ApplyMetadata =
      [State](const SecurityMetadataAction &Action,
              llvm::ArrayRef<uint8_t> Material) -> llvm::Error {
    if (!State->Begun || State->Terminal)
      return adapterError("Darwin metadata application is out of sequence");
    return State->apply(Action, Material);
  };
  Operations.AuthenticateCandidateAfterMetadata =
      [State]() -> llvm::Expected<AuthenticatedCandidateArtifactV1> {
    if (State->CandidateFD == -1 || State->Terminal)
      return adapterError("Darwin candidate readback is out of sequence");
    llvm::Expected<ObservedDarwinFile> Candidate = observeHeldFile(
        State->CandidateFD, State->Native, "held candidate after metadata");
    if (!Candidate)
      return Candidate.takeError();
    return AuthenticatedCandidateArtifactV1{Candidate->Artifact,
                                            Candidate->CompleteMetadata};
  };
  Operations.ReauthenticateDestinationAbsent = [State]() -> llvm::Error {
    if (State->CandidateFD == -1 || State->Terminal)
      return adapterError("Darwin destination check is out of sequence");
    llvm::Expected<uint32_t> EffectiveUID = State->Native.GetEffectiveUserID();
    if (!EffectiveUID)
      return EffectiveUID.takeError();
    if (*EffectiveUID != State->EffectiveUID)
      return adapterError(
          "Darwin publication effective uid changed before publish");
    llvm::Expected<DarwinPublicationVolumeCapabilitiesV1> Volume =
        State->Native.QueryVolumeCapabilities(State->DirectoryFD);
    if (!Volume)
      return Volume.takeError();
    if (llvm::Error Error = validateVolumeAccessControl(*Volume))
      return Error;
    llvm::Expected<DarwinPublicationFileStatusV1> Directory =
        observeConfinedDirectory(State->DirectoryFD, State->EffectiveUID,
                                 State->Native);
    if (!Directory)
      return Directory.takeError();
    if (!sameDirectoryIdentity(*Directory, State->InitialDirectoryStatus))
      return adapterError("anchored destination directory identity changed");
    llvm::Expected<std::optional<DarwinPublicationFileStatusV1>> Destination =
        State->Native.StatAtNoFollow(State->DirectoryFD, State->FinalName);
    if (!Destination)
      return Destination.takeError();
    if (*Destination)
      return adapterError("anchored Darwin destination is no longer absent");
    return llvm::Error::success();
  };
  Operations.PublishNoReplace = [State]() -> NamespacePublishResultV1 {
    if (!State->RenameExclusive)
      return {SanitizerPublicationOutcome::NotPublished,
              "anchored volume does not attest RENAME_EXCL"};
    if (State->CandidateFD == -1 || State->Terminal || State->PublishAttempted)
      return {SanitizerPublicationOutcome::Indeterminate,
              "Darwin exclusive publication is out of sequence"};
    State->PublishAttempted = true;
    NamespacePublishResultV1 Result =
        State->Native.RenameExclusive(State->DirectoryFD, State->TemporaryName,
                                      State->DirectoryFD, State->FinalName);
    if (Result.Outcome == SanitizerPublicationOutcome::Published) {
      State->Published = true;
      State->TemporaryName.clear();
    }
    return Result;
  };
  Operations.AuthenticatePublishedFinal =
      [State]() -> llvm::Expected<AuthenticatedPublishedFinalArtifactV1> {
    if (!State->Published || State->Terminal)
      return adapterError("Darwin final authentication is out of sequence");
    llvm::Expected<ObservedDarwinFile> Final =
        State->openAndObserveFinal("anchored published final");
    if (!Final)
      return Final.takeError();
    return AuthenticatedPublishedFinalArtifactV1{Final->Artifact,
                                                 Final->CompleteMetadata};
  };
  Operations.FinalizePublished = [State]() -> llvm::Error {
    if (!State->Published)
      return adapterError("Darwin publication was not committed");
    return State->cleanup(false, true);
  };
  Operations.Discard = [State]() -> llvm::Error {
    if (State->Published)
      return adapterError("Darwin committed final cannot be discarded");
    return State->cleanup(true, true);
  };
  return Operations;
}

} // namespace

namespace detail {

llvm::Expected<PreparedDarwinSanitizerPublicationV1>
prepareDarwinSanitizerPublicationWithOperationsV1(
    DarwinSanitizerPublicationRequestV1 Request,
    DarwinSanitizerPublicationNativeOperationsV1 Native) {
  if (!Request.ExpectedSourceContent)
    return adapterError(
        "Darwin publication requires an external source content anchor");
  if (Request.SourcePath.empty())
    return adapterError("Darwin publication source path is empty");
  if (Request.DestinationPath.empty())
    return adapterError("Darwin publication destination path is empty");
  if (llvm::StringRef(Request.SourcePath).contains('\0') ||
      llvm::StringRef(Request.DestinationPath).contains('\0'))
    return adapterError("Darwin publication path contains an embedded NUL");
  if (!llvm::sys::path::is_absolute(Request.SourcePath,
                                    llvm::sys::path::Style::posix) ||
      !llvm::sys::path::is_absolute(Request.DestinationPath,
                                    llvm::sys::path::Style::posix))
    return adapterError("Darwin publication paths must be absolute");
  if (hasDotPathComponent(Request.SourcePath) ||
      hasDotPathComponent(Request.DestinationPath))
    return adapterError(
        "Darwin publication path contains a dot or dot-dot component");
  if (llvm::Error Error = validateNativeOperations(Native))
    return std::move(Error);

  llvm::SmallString<256> Destination(Request.DestinationPath);
  const llvm::StringRef FinalNameRef =
      llvm::sys::path::filename(Destination, llvm::sys::path::Style::posix);
  if (FinalNameRef.empty() || FinalNameRef == "." || FinalNameRef == ".." ||
      llvm::sys::path::is_absolute(FinalNameRef, llvm::sys::path::Style::posix))
    return adapterError(
        "Darwin publication destination must name one final component");
  const std::string FinalName = FinalNameRef.str();
  llvm::sys::path::remove_filename(Destination, llvm::sys::path::Style::posix);
  if (Destination.empty())
    Destination = ".";

  auto State = std::make_shared<DarwinPublicationState>(std::move(Native));
  State->FinalName = FinalName;
  State->CandidateBytes = std::move(Request.CandidateBytes);

  llvm::Expected<int> Source =
      State->Native.OpenSourceNoFollow(Request.SourcePath);
  if (!Source)
    return Source.takeError();
  State->SourceFD = *Source;
  llvm::Expected<int> Directory =
      State->Native.OpenDirectoryNoFollow(Destination);
  if (!Directory)
    return Directory.takeError();
  State->DirectoryFD = *Directory;

  llvm::Expected<DarwinPublicationFileStatusV1> DirectoryStatus =
      State->Native.Stat(State->DirectoryFD);
  if (!DirectoryStatus)
    return DirectoryStatus.takeError();
  if (DirectoryStatus->Device == 0 || DirectoryStatus->Inode == 0)
    return adapterError(
        "anchored Darwin destination directory has no usable identity");
  if (!DirectoryStatus->Directory)
    return adapterError(
        "anchored Darwin destination descriptor is not a directory");
  State->InitialDirectoryStatus = *DirectoryStatus;

  llvm::Expected<ObservedDarwinFile> SourceObservation = observeHeldFile(
      State->SourceFD, State->Native, "held source during preparation");
  if (!SourceObservation)
    return SourceObservation.takeError();
  if (SourceObservation->Artifact.Content != *Request.ExpectedSourceContent)
    return adapterError(
        "held source does not match the external source content anchor");
  State->InitialSourceArtifact = SourceObservation->Artifact;

  FileSecurityMetadataSnapshot DestinationSnapshot;
  bool SameSourceAndDestination = false;
  llvm::Expected<std::optional<DarwinPublicationFileStatusV1>> DestinationNode =
      State->Native.StatAtNoFollow(State->DirectoryFD, State->FinalName);
  if (!DestinationNode)
    return DestinationNode.takeError();
  if (*DestinationNode) {
    if ((*DestinationNode)->Node != PublicationNodeKind::RegularFile)
      return adapterError("anchored Darwin destination is not a regular file");
    llvm::Expected<ObservedDarwinFile> Existing =
        State->openAndObserveFinal("anchored destination during preparation");
    if (!Existing)
      return Existing.takeError();
    DestinationSnapshot = Existing->PlannerMetadata;
    SameSourceAndDestination =
        sameHeldObject(SourceObservation->Artifact, Existing->Artifact);
  } else {
    llvm::Expected<DarwinPublicationVolumeCapabilitiesV1> Volume =
        State->Native.QueryVolumeCapabilities(State->DirectoryFD);
    if (!Volume)
      return Volume.takeError();
    if (llvm::Error Error = validateVolumeAccessControl(*Volume))
      return std::move(Error);
    State->RenameExclusive =
        Volume->RenameExclusiveKnown && Volume->RenameExclusive;
    llvm::Expected<uint32_t> EffectiveUID = State->Native.GetEffectiveUserID();
    if (!EffectiveUID)
      return EffectiveUID.takeError();
    State->EffectiveUID = *EffectiveUID;
    llvm::Expected<DarwinPublicationFileStatusV1> Confined =
        observeConfinedDirectory(State->DirectoryFD, State->EffectiveUID,
                                 State->Native);
    if (!Confined)
      return Confined.takeError();
    if (!sameDirectoryIdentity(*Confined, State->InitialDirectoryStatus))
      return adapterError("anchored destination directory identity changed");
    State->InitialDirectoryStatus = *Confined;
    State->DirectoryNamespaceConfined = true;
  }

  ArtifactContentDigestV1 CandidateContent;
  CandidateContent.SHA256 =
      llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(State->CandidateBytes));
  CandidateContent.Size = State->CandidateBytes.size();

  PreparedDarwinSanitizerPublicationV1 Prepared;
  Prepared.MetadataRequest.Platform = PublicationPlatform::MacOS;
  Prepared.MetadataRequest.GuardedSiteCount = Request.GuardedSiteCount;
  Prepared.MetadataRequest.CandidateMutation =
      CandidateContent == SourceObservation->Artifact.Content
          ? CandidateMutationDisposition::ByteIdentical
          : CandidateMutationDisposition::ByteMutated;
  Prepared.MetadataRequest.SameSourceAndDestination = SameSourceAndDestination;
  Prepared.MetadataRequest.Source = SourceObservation->PlannerMetadata;
  Prepared.MetadataRequest.Destination = std::move(DestinationSnapshot);
  Prepared.Binding.ExpectedSourceContent = SourceObservation->Artifact.Content;
  Prepared.Binding.ExpectedSourceStableIdentity =
      SourceObservation->Artifact.StableIdentity;
  Prepared.Binding.ExpectedCandidateContent = CandidateContent;
  Prepared.Operations = makeExecutorOperations(State);
  return Prepared;
}

} // namespace detail

#ifdef __APPLE__
namespace {

llvm::Error errnoError(llvm::StringRef Operation, int ErrorNumber = errno) {
  const std::error_code Error(ErrorNumber, std::generic_category());
  return llvm::createStringError(Error, "%s", Operation.str().c_str());
}

llvm::Expected<int>
openAbsolutePathWithoutSymlinkComponents(llvm::StringRef Path, int FinalFlags,
                                         llvm::StringRef Role) {
  if (!llvm::sys::path::is_absolute(Path, llvm::sys::path::Style::posix))
    return adapterError((Role + " path is not absolute").str());

  // Finish every potentially allocating component copy before acquiring an fd
  // so a C++ allocation exception cannot leak a partially traversed chain.
  std::vector<std::string> Components;
  size_t Offset = 1;
  while (Offset < Path.size()) {
    while (Offset < Path.size() && Path[Offset] == '/')
      ++Offset;
    if (Offset == Path.size())
      break;
    const size_t End = Path.find('/', Offset);
    const size_t ComponentEnd =
        End == llvm::StringRef::npos ? Path.size() : End;
    Components.push_back(Path.slice(Offset, ComponentEnd).str());
    Offset = ComponentEnd;
  }

  const int DirectoryFlags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  int Current = ::openat(AT_FDCWD, "/", DirectoryFlags);
  if (Current < 0)
    return errnoError("cannot anchor root while opening " + Role.str());

  for (size_t Index = 0; Index != Components.size(); ++Index) {
    const bool Last = Index + 1 == Components.size();
    const int Next = ::openat(Current, Components[Index].c_str(),
                              Last ? FinalFlags : DirectoryFlags);
    if (Next < 0) {
      const int OpenError = errno;
      (void)::close(Current);
      return errnoError("cannot open symlink-free component of " + Role.str(),
                        OpenError);
    }
    if (::close(Current) != 0) {
      const int CloseError = errno;
      (void)::close(Next);
      return errnoError("cannot close intermediate descriptor while opening " +
                            Role.str(),
                        CloseError);
    }
    Current = Next;
  }
  return Current;
}

PublicationNodeKind nodeKind(mode_t Mode) {
  if (S_ISREG(Mode))
    return PublicationNodeKind::RegularFile;
  if (S_ISLNK(Mode))
    return PublicationNodeKind::Symlink;
  return PublicationNodeKind::Other;
}

llvm::Expected<DarwinPublicationFileStatusV1>
portableStatus(const struct stat &Native) {
  if (Native.st_size < 0)
    return adapterError("Darwin stat returned a negative file size");
  DarwinPublicationFileStatusV1 Status;
  Status.Node = nodeKind(Native.st_mode);
  Status.Directory = S_ISDIR(Native.st_mode);
  Status.Device = static_cast<uint64_t>(Native.st_dev);
  Status.Inode = static_cast<uint64_t>(Native.st_ino);
  Status.Generation = static_cast<uint64_t>(Native.st_gen);
  Status.LinkCount = static_cast<uint64_t>(Native.st_nlink);
  Status.Size = static_cast<uint64_t>(Native.st_size);
  Status.UID = static_cast<uint32_t>(Native.st_uid);
  Status.GID = static_cast<uint32_t>(Native.st_gid);
  Status.Mode = static_cast<uint32_t>(Native.st_mode) & 07777u;
  Status.Flags = static_cast<uint32_t>(Native.st_flags);
  Status.BirthSeconds = Native.st_birthtimespec.tv_sec;
  Status.BirthNanoseconds = Native.st_birthtimespec.tv_nsec;
  Status.ModifySeconds = Native.st_mtimespec.tv_sec;
  Status.ModifyNanoseconds = Native.st_mtimespec.tv_nsec;
  Status.ChangeSeconds = Native.st_ctimespec.tv_sec;
  Status.ChangeNanoseconds = Native.st_ctimespec.tv_nsec;
  return Status;
}

llvm::Expected<CanonicalSecurityMetadata> serializeACL(acl_t ACL) {
  const ssize_t Size = ::acl_size(ACL);
  if (Size < 0)
    return errnoError("cannot size Darwin ACL");
  if (static_cast<uint64_t>(Size) >
      kSanitizerPublicationMetadataMaxEncodedBytes)
    return adapterError("Darwin ACL exceeds the plan-v1 byte ceiling");
  CanonicalSecurityMetadata Bytes(static_cast<size_t>(Size));
  const ssize_t Written = ::acl_copy_ext(Bytes.data(), ACL, Size);
  if (Written < 0)
    return errnoError("cannot encode Darwin ACL");
  if (Written != Size)
    return adapterError("Darwin ACL external encoding changed size");
  return Bytes;
}

llvm::Expected<std::optional<CanonicalSecurityMetadata>>
readCanonicalACLNative(int FD) {
  errno = 0;
  acl_t ACL = ::acl_get_fd_np(FD, ACL_TYPE_EXTENDED);
  if (!ACL) {
    const int ErrorNumber = errno;
    // Darwin reports ENOENT when a file has no extended ACL.  That is a
    // complete negative observation, unlike EOPNOTSUPP/ENOTSUP, which must
    // remain fail-closed because the namespace could not be enumerated.
    if (ErrorNumber == ENOENT)
      return std::nullopt;
    return errnoError("cannot enumerate Darwin ACL", ErrorNumber);
  }
  llvm::scope_exit FreeACL([&] { (void)::acl_free(ACL); });
  llvm::Expected<CanonicalSecurityMetadata> Encoded = serializeACL(ACL);
  if (!Encoded)
    return Encoded.takeError();

  acl_t EmptyACL = ::acl_init(0);
  if (!EmptyACL)
    return errnoError("cannot allocate canonical empty Darwin ACL");
  llvm::scope_exit FreeEmpty([&] { (void)::acl_free(EmptyACL); });
  llvm::Expected<CanonicalSecurityMetadata> Empty = serializeACL(EmptyACL);
  if (!Empty)
    return Empty.takeError();
  if (*Encoded == *Empty)
    return std::nullopt;
  return std::optional<CanonicalSecurityMetadata>(std::move(*Encoded));
}

llvm::Error
writeCanonicalACLNative(int FD,
                        const std::optional<CanonicalSecurityMetadata> &Bytes) {
  if (!Bytes) {
    // An empty acl_init(0) value is not a valid extended ACL for a file.
    // Darwin's filesec contract uses this public sentinel to request removal.
    acl_t RemoveACL = static_cast<acl_t>(_FILESEC_REMOVE_ACL);
    if (::acl_set_fd_np(FD, RemoveACL, ACL_TYPE_EXTENDED) != 0)
      return errnoError("cannot clear Darwin ACL from held candidate");
    return llvm::Error::success();
  }

  if (Bytes->empty())
    return adapterError("present Darwin ACL has an empty external encoding");
  acl_t ACL = ::acl_copy_int(Bytes->data());
  if (!ACL)
    return errnoError("cannot decode Darwin ACL external encoding");
  llvm::scope_exit FreeACL([&] { (void)::acl_free(ACL); });
  if (::acl_valid(ACL) != 0)
    return errnoError("Darwin ACL external encoding is invalid");
  if (::acl_valid_fd_np(FD, ACL_TYPE_EXTENDED, ACL) != 0)
    return errnoError("Darwin ACL is invalid for held candidate");
  if (::acl_set_fd_np(FD, ACL, ACL_TYPE_EXTENDED) != 0)
    return errnoError("cannot apply Darwin ACL to held candidate");
  return llvm::Error::success();
}

llvm::Expected<std::vector<std::string>> listXattrsNative(int FD) {
  for (unsigned Attempt = 0; Attempt != 4; ++Attempt) {
    errno = 0;
    const ssize_t Required =
        ::flistxattr(FD, nullptr, 0, XATTR_SHOWCOMPRESSION);
    if (Required < 0) {
      return errnoError("cannot size Darwin xattr enumeration");
    }
    if (Required == 0)
      return std::vector<std::string>{};
    if (static_cast<uint64_t>(Required) >
        kSanitizerPublicationMetadataMaxEncodedBytes)
      return adapterError("Darwin xattr name list exceeds the v1 byte ceiling");
    std::vector<char> Names(static_cast<size_t>(Required));
    const ssize_t Read =
        ::flistxattr(FD, Names.data(), Names.size(), XATTR_SHOWCOMPRESSION);
    if (Read < 0) {
      if (errno == ERANGE)
        continue;
      return errnoError("cannot enumerate Darwin xattr names");
    }
    if (Read > Required)
      return adapterError("Darwin xattr enumeration exceeded its buffer");
    if (Read != Required)
      continue;
    Names.resize(static_cast<size_t>(Read));
    std::vector<std::string> Result;
    size_t Offset = 0;
    while (Offset < Names.size()) {
      const auto End = std::find(Names.begin() + Offset, Names.end(), '\0');
      if (End == Names.end())
        return adapterError("Darwin xattr list is not NUL terminated");
      const size_t Length = static_cast<size_t>(End - (Names.begin() + Offset));
      if (Length == 0)
        return adapterError("Darwin xattr list contains an empty name");
      Result.emplace_back(Names.data() + Offset, Length);
      Offset += Length + 1;
    }
    return Result;
  }
  return adapterError("Darwin xattr name list changed repeatedly");
}

llvm::Expected<CanonicalSecurityMetadata>
readXattrNative(int FD, llvm::StringRef Name) {
  if (Name.empty() || Name.contains('\0'))
    return adapterError("Darwin xattr name is invalid");
  const std::string NativeName = Name.str();
  for (unsigned Attempt = 0; Attempt != 4; ++Attempt) {
    const ssize_t Required =
        ::fgetxattr(FD, NativeName.c_str(), nullptr, 0, 0, 0);
    if (Required < 0)
      return errnoError("cannot size Darwin xattr value");
    if (static_cast<uint64_t>(Required) >
        kSanitizerPublicationMetadataMaxEncodedBytes)
      return adapterError("Darwin xattr value exceeds the v1 byte ceiling");
    if (Required == 0)
      return CanonicalSecurityMetadata{};
    CanonicalSecurityMetadata Bytes(static_cast<size_t>(Required));
    const ssize_t Read =
        ::fgetxattr(FD, NativeName.c_str(), Bytes.data(), Bytes.size(), 0, 0);
    if (Read < 0) {
      if (errno == ERANGE)
        continue;
      return errnoError("cannot read Darwin xattr value");
    }
    if (Read != Required)
      continue;
    return Bytes;
  }
  return adapterError("Darwin xattr value changed repeatedly");
}

llvm::Error writeXattrNative(int FD, llvm::StringRef Name,
                             llvm::ArrayRef<uint8_t> Bytes) {
  if (Name.empty() || Name.contains('\0'))
    return adapterError("Darwin xattr name is invalid");
  const std::string NativeName = Name.str();
  const void *Data = Bytes.empty() ? nullptr : Bytes.data();
  if (::fsetxattr(FD, NativeName.c_str(), Data, Bytes.size(), 0, 0) != 0)
    return errnoError("cannot apply Darwin xattr to held candidate");
  return llvm::Error::success();
}

std::string randomTemporaryName(llvm::StringRef Prefix) {
  std::array<uint8_t, 16> Random{};
  ::arc4random_buf(Random.data(), Random.size());
  static constexpr char Hex[] = "0123456789abcdef";
  std::string Name = Prefix.str();
  Name.reserve(Name.size() + Random.size() * 2 + 4);
  for (uint8_t Byte : Random) {
    Name.push_back(Hex[Byte >> 4]);
    Name.push_back(Hex[Byte & 0x0f]);
  }
  Name += ".tmp";
  return Name;
}

DarwinSanitizerPublicationNativeOperationsV1 nativeDarwinOperations() {
  DarwinSanitizerPublicationNativeOperationsV1 Operations;
  Operations.GetEffectiveUserID = []() -> llvm::Expected<uint32_t> {
    return static_cast<uint32_t>(::geteuid());
  };
  Operations.OpenSourceNoFollow =
      [](llvm::StringRef Path) -> llvm::Expected<int> {
    return openAbsolutePathWithoutSymlinkComponents(
        Path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC,
        "held Darwin source");
  };
  Operations.OpenDirectoryNoFollow =
      [](llvm::StringRef Path) -> llvm::Expected<int> {
    return openAbsolutePathWithoutSymlinkComponents(
        Path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
        "Darwin destination parent");
  };
  Operations.OpenAnchoredReadOnlyNoFollow =
      [](int DirectoryFD, llvm::StringRef Name) -> llvm::Expected<int> {
    const std::string NativeName = Name.str();
    const int FD = ::openat(DirectoryFD, NativeName.c_str(),
                            O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (FD < 0)
      return errnoError("cannot open anchored Darwin final with openat");
    return FD;
  };
  Operations.CreateAnchoredExclusive =
      [](int DirectoryFD, llvm::StringRef Prefix,
         uint32_t Mode) -> llvm::Expected<DarwinPublicationCreatedFileV1> {
    for (unsigned Attempt = 0; Attempt != 32; ++Attempt) {
      std::string Name = randomTemporaryName(Prefix);
      const int FD =
          ::openat(DirectoryFD, Name.c_str(),
                   O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                   static_cast<mode_t>(Mode));
      if (FD >= 0)
        return DarwinPublicationCreatedFileV1{FD, std::move(Name)};
      if (errno != EEXIST)
        return errnoError("cannot create exclusive anchored Darwin temp");
    }
    return adapterError(
        "cannot allocate a unique exclusive anchored Darwin temp name");
  };
  Operations.Stat =
      [](int FD) -> llvm::Expected<DarwinPublicationFileStatusV1> {
    struct stat Status{};
    if (::fstat(FD, &Status) != 0)
      return errnoError("cannot fstat held Darwin descriptor");
    return portableStatus(Status);
  };
  Operations.StatAtNoFollow = [](int DirectoryFD, llvm::StringRef Name)
      -> llvm::Expected<std::optional<DarwinPublicationFileStatusV1>> {
    const std::string NativeName = Name.str();
    struct stat Status{};
    if (::fstatat(DirectoryFD, NativeName.c_str(), &Status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT)
        return std::nullopt;
      return errnoError("cannot inspect anchored Darwin final with fstatat");
    }
    llvm::Expected<DarwinPublicationFileStatusV1> Portable =
        portableStatus(Status);
    if (!Portable)
      return Portable.takeError();
    return std::optional<DarwinPublicationFileStatusV1>(*Portable);
  };
  Operations.ReadAt =
      [](int FD, uint64_t Offset,
         llvm::MutableArrayRef<uint8_t> Buffer) -> llvm::Expected<size_t> {
    if (Offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
      return adapterError("Darwin pread offset is unrepresentable");
    const size_t Count =
        std::min(Buffer.size(),
                 static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    for (;;) {
      const ssize_t Read =
          ::pread(FD, Buffer.data(), Count, static_cast<off_t>(Offset));
      if (Read >= 0)
        return static_cast<size_t>(Read);
      if (errno != EINTR)
        return errnoError("cannot pread held Darwin file");
    }
  };
  Operations.WriteAt =
      [](int FD, uint64_t Offset,
         llvm::ArrayRef<uint8_t> Buffer) -> llvm::Expected<size_t> {
    if (Offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
      return adapterError("Darwin pwrite offset is unrepresentable");
    const size_t Count =
        std::min(Buffer.size(),
                 static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    for (;;) {
      const ssize_t Written =
          ::pwrite(FD, Buffer.data(), Count, static_cast<off_t>(Offset));
      if (Written >= 0)
        return static_cast<size_t>(Written);
      if (errno != EINTR)
        return errnoError("cannot pwrite held Darwin candidate");
    }
  };
  Operations.ReadCanonicalACL = readCanonicalACLNative;
  Operations.WriteCanonicalACL = writeCanonicalACLNative;
  Operations.ListXattrs = listXattrsNative;
  Operations.ReadXattr = readXattrNative;
  Operations.WriteXattr = writeXattrNative;
  Operations.ChangeOwner = [](int FD, uint32_t UID,
                              uint32_t GID) -> llvm::Error {
    if (::fchown(FD, static_cast<uid_t>(UID), static_cast<gid_t>(GID)) != 0)
      return errnoError("cannot apply owner to held Darwin candidate");
    return llvm::Error::success();
  };
  Operations.ChangeMode = [](int FD, uint32_t Mode) -> llvm::Error {
    if ((Mode & ~07777u) != 0)
      return adapterError("Darwin candidate mode has unrepresentable bits");
    if (::fchmod(FD, static_cast<mode_t>(Mode)) != 0)
      return errnoError("cannot apply mode to held Darwin candidate");
    return llvm::Error::success();
  };
  Operations.QueryVolumeCapabilities = [](int DirectoryFD)
      -> llvm::Expected<DarwinPublicationVolumeCapabilitiesV1> {
    struct statfs MountStatus{};
    if (::fstatfs(DirectoryFD, &MountStatus) != 0)
      return errnoError(
          "cannot query anchored Darwin mount access-control flags");
    struct AttributeBuffer {
      uint32_t Length;
      vol_capabilities_attr_t Capabilities;
    } Buffer{};
    struct attrlist Attributes{};
    Attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    Attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
    if (::fgetattrlist(DirectoryFD, &Attributes, &Buffer, sizeof(Buffer), 0) !=
        0)
      return errnoError(
          "cannot query anchored Darwin volume rename capabilities");
    if (Buffer.Length < sizeof(Buffer))
      return adapterError("Darwin volume capability response is incomplete");
    DarwinPublicationVolumeCapabilitiesV1 Result;
    Result.RenameExclusiveKnown =
        (Buffer.Capabilities.valid[VOL_CAPABILITIES_INTERFACES] &
         VOL_CAP_INT_RENAME_EXCL) != 0;
    Result.RenameExclusive =
        Result.RenameExclusiveKnown &&
        (Buffer.Capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] &
         VOL_CAP_INT_RENAME_EXCL) != 0;
    Result.NoPermissionsKnown =
        (Buffer.Capabilities.valid[VOL_CAPABILITIES_FORMAT] &
         VOL_CAP_FMT_NO_PERMISSIONS) != 0;
    Result.NoPermissions =
        Result.NoPermissionsKnown &&
        (Buffer.Capabilities.capabilities[VOL_CAPABILITIES_FORMAT] &
         VOL_CAP_FMT_NO_PERMISSIONS) != 0;
    Result.IgnoreOwnershipKnown = true;
    Result.IgnoreOwnership = (MountStatus.f_flags & MNT_IGNORE_OWNERSHIP) != 0;
    return Result;
  };
  Operations.RenameExclusive =
      [](int FromDirectoryFD, llvm::StringRef From, int ToDirectoryFD,
         llvm::StringRef To) -> NamespacePublishResultV1 {
    const std::string NativeFrom = From.str();
    const std::string NativeTo = To.str();
    if (::renameatx_np(FromDirectoryFD, NativeFrom.c_str(), ToDirectoryFD,
                       NativeTo.c_str(), RENAME_EXCL) == 0)
      return {SanitizerPublicationOutcome::Published, {}};
    const int ErrorNumber = errno;
    const std::error_code Error(ErrorNumber, std::generic_category());
    if (ErrorNumber == EEXIST)
      return {SanitizerPublicationOutcome::NotPublished,
              "exclusive Darwin destination already exists: " +
                  Error.message()};
    // Failures such as EIO are conservatively indeterminate.  Discard removes
    // only the old temp name, so it cannot unlink a possibly committed final.
    return {SanitizerPublicationOutcome::Indeterminate,
            "exclusive Darwin rename outcome is indeterminate: " +
                Error.message()};
  };
  Operations.UnlinkAt = [](int DirectoryFD,
                           llvm::StringRef Name) -> llvm::Error {
    const std::string NativeName = Name.str();
    if (::unlinkat(DirectoryFD, NativeName.c_str(), 0) == 0 || errno == ENOENT)
      return llvm::Error::success();
    return errnoError("cannot unlink anchored Darwin temp");
  };
  Operations.Close = [](int FD) -> llvm::Error {
    // A failed close must never be retried: the numeric fd may already have
    // been released and reused.
    if (::close(FD) == 0)
      return llvm::Error::success();
    return errnoError("cannot close held Darwin descriptor");
  };
  return Operations;
}

} // namespace
#endif

DarwinSanitizerPublicationAvailabilityV1
getDarwinSanitizerPublicationAvailabilityV1() {
  DarwinSanitizerPublicationAvailabilityV1 Availability;
#ifdef __APPLE__
  Availability.Available = true;
  Availability.Detail =
      "Darwin APIs are available; concrete capabilities remain false until "
      "prepare anchors and queries the destination volume";
#else
  Availability.Detail =
      "native sanitizer publication requires Darwin/Apple filesystem APIs";
#endif
  // Process-wide capability claims remain all-false on every host.  In
  // particular, RENAME_EXCL is a per-volume property authenticated by prepare.
  return Availability;
}

llvm::Expected<PreparedDarwinSanitizerPublicationV1>
prepareDarwinSanitizerPublicationV1(
    DarwinSanitizerPublicationRequestV1 Request) {
#ifdef __APPLE__
  return detail::prepareDarwinSanitizerPublicationWithOperationsV1(
      std::move(Request), nativeDarwinOperations());
#else
  (void)Request;
  return adapterError(
      "native sanitizer publication requires Darwin/Apple filesystem APIs");
#endif
}

} // namespace neverd::safety::sanitizer_publication_metadata
