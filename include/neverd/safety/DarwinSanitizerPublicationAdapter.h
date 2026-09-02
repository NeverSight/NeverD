//===- DarwinSanitizerPublicationAdapter.h - Darwin publication -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A fail-closed Darwin filesystem adapter for
/// SanitizerPublicationExecutorOperationsV1.  The public contract is free of
/// Darwin headers so NeverDSafety remains buildable on non-Apple hosts.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_DARWINSANITIZERPUBLICATIONADAPTER_H
#define NEVERD_SAFETY_DARWINSANITIZERPUBLICATIONADAPTER_H

#include "neverd/safety/SanitizerPublicationExecutor.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neverd::safety::sanitizer_publication_metadata {

/// Input owned by the prepared adapter.  CandidateBytes are copied into a new
/// same-directory inode; a caller-provided candidate path is intentionally not
/// accepted.
struct DarwinSanitizerPublicationRequestV1 {
  std::string SourcePath;
  std::string DestinationPath;
  /// Mandatory external content anchor from the already authenticated session
  /// load (for example S->Img.Raw).  prepare rejects a missing value and a
  /// held-fd digest/size mismatch before candidate or namespace mutation.
  std::optional<ArtifactContentDigestV1> ExpectedSourceContent;
  std::vector<uint8_t> CandidateBytes;
  uint64_t GuardedSiteCount = 0;
};

/// One inseparable planning/execution frame.  prepareDarwin... opens the source
/// and destination parent once, derives both digest-only planner observations
/// and ArtifactBinding from the held source descriptor only after matching the
/// caller-authenticated ExpectedSourceContent, and captures that same held
/// state in every Operations callback.  Callers must not substitute a
/// separately constructed binding.  Content is the external session anchor;
/// stable identity is intentionally scoped to the prepared transaction.
/// Source metadata and stable identity are transaction-time observations, not
/// proof that session-load-time metadata or object identity was unchanged.  A
/// caller needing that stronger continuity must retain the original load fd or
/// supply a separate externally authenticated metadata/object anchor.
///
/// StableIdentity is SHA-256 over a domain-separated, fixed-width encoding of
/// device, inode, birth time, and a generation-present marker plus st_gen when
/// Darwin supplies a nonzero generation.  It deliberately excludes ctime and
/// mtime because metadata application changes ctime.  A zero st_gen does not
/// claim a durable cross-transaction ABA identity: safety is transaction-local
/// and follows from retaining the source fd until Discard/Finalize and
/// comparing the anchored final fd with that still-live object identity.
///
/// Source and destination must be absolute POSIX paths without dot/dot-dot
/// components.  Native preparation resolves every existing component from a
/// held parent descriptor with O_NOFOLLOW; subsequent work is scoped to the
/// retained source and destination-directory objects.  The destination
/// directory may be renamed elsewhere after it is opened, so anchoring proves
/// publication into that object rather than durable continuity of the original
/// textual pathname.
///
/// Darwin's RENAME_EXCL primitive names its source; it does not bind the
/// rename operand to CandidateFD.  This adapter therefore exposes create-
/// exclusive execution only when the held destination parent is owned by the
/// effective uid, has no extended ACL, grants no group/other write bit, and its
/// volume both supports POSIX permissions and does not ignore ownership.
/// Those directory and volume access-control facts are reauthenticated before
/// candidate creation and again immediately before publish.
/// CandidatePublishOperandBinding is scoped to principals that do not share
/// that effective uid and lack root/DAC-bypass authority; macOS provides no
/// primitive that isolates a pathname from an equally privileged process.
/// It also assumes the kernel, VFS, and mounted filesystem backend honor their
/// reported access-control semantics; an adversarial filesystem/server is
/// outside this binding strength's threat model.
/// Unconfined destination directories fail before mutation.  A NoChange frame
/// is read-only and deliberately does not require rename or confinement
/// capabilities.
struct PreparedDarwinSanitizerPublicationV1 {
  SanitizerPublicationMetadataRequestV1 MetadataRequest;
  ArtifactBindingV1 Binding;
  SanitizerPublicationExecutorOperationsV1 Operations;
};

/// Conservative process-wide availability.  On non-Apple hosts Available is
/// false and Capabilities is all-false.  On Apple, concrete filesystem claims
/// (especially RENAME_EXCL) are still made only by a successfully prepared
/// transaction after querying its anchored destination volume.
struct DarwinSanitizerPublicationAvailabilityV1 {
  bool Available = false;
  SanitizerPublicationExecutorCapabilitiesV1 Capabilities;
  std::string Detail;
};

DarwinSanitizerPublicationAvailabilityV1
getDarwinSanitizerPublicationAvailabilityV1();

llvm::Expected<PreparedDarwinSanitizerPublicationV1>
prepareDarwinSanitizerPublicationV1(
    DarwinSanitizerPublicationRequestV1 Request);

namespace detail {

/// Portable representation of the Darwin stat fields used by the adapter.
/// Change/modify times are only a before/after race detector for a single
/// complete observation; they are never part of StableIdentity.
struct DarwinPublicationFileStatusV1 {
  PublicationNodeKind Node = PublicationNodeKind::Other;
  bool Directory = false;
  uint64_t Device = 0;
  uint64_t Inode = 0;
  uint64_t Generation = 0;
  uint64_t LinkCount = 0;
  uint64_t Size = 0;
  uint32_t UID = 0;
  uint32_t GID = 0;
  uint32_t Mode = 0;
  /// Darwin UF_*/SF_* flags are not representable by plan-v1.  Production
  /// observation fills st_flags and the adapter rejects every nonzero value.
  uint32_t Flags = 0;
  int64_t BirthSeconds = 0;
  int64_t BirthNanoseconds = 0;
  int64_t ModifySeconds = 0;
  int64_t ModifyNanoseconds = 0;
  int64_t ChangeSeconds = 0;
  int64_t ChangeNanoseconds = 0;
};

inline bool operator==(const DarwinPublicationFileStatusV1 &LHS,
                       const DarwinPublicationFileStatusV1 &RHS) {
  return LHS.Node == RHS.Node && LHS.Directory == RHS.Directory &&
         LHS.Device == RHS.Device && LHS.Inode == RHS.Inode &&
         LHS.Generation == RHS.Generation && LHS.LinkCount == RHS.LinkCount &&
         LHS.Size == RHS.Size && LHS.UID == RHS.UID && LHS.GID == RHS.GID &&
         LHS.Mode == RHS.Mode && LHS.Flags == RHS.Flags &&
         LHS.BirthSeconds == RHS.BirthSeconds &&
         LHS.BirthNanoseconds == RHS.BirthNanoseconds &&
         LHS.ModifySeconds == RHS.ModifySeconds &&
         LHS.ModifyNanoseconds == RHS.ModifyNanoseconds &&
         LHS.ChangeSeconds == RHS.ChangeSeconds &&
         LHS.ChangeNanoseconds == RHS.ChangeNanoseconds;
}

inline bool operator!=(const DarwinPublicationFileStatusV1 &LHS,
                       const DarwinPublicationFileStatusV1 &RHS) {
  return !(LHS == RHS);
}

struct DarwinPublicationVolumeCapabilitiesV1 {
  bool RenameExclusiveKnown = false;
  bool RenameExclusive = false;
  /// Raw VOL_CAP_FMT_NO_PERMISSIONS validity/value from the held volume.
  bool NoPermissionsKnown = false;
  bool NoPermissions = false;
  /// Raw MNT_IGNORE_OWNERSHIP state from the held mount.
  bool IgnoreOwnershipKnown = false;
  bool IgnoreOwnership = false;
};

struct DarwinPublicationCreatedFileV1 {
  int Descriptor = -1;
  std::string Name;
};

/// Fault-injection boundary.  Production callbacks are thin wrappers around
/// component-by-component openat/O_NOFOLLOW/O_CLOEXEC, pread/pwrite,
/// fstat/fstatat/fstatfs, Darwin volume/ACL/xattr APIs, and
/// renameatx_np(RENAME_EXCL).  Tests inject semantic equivalents and failures
/// here; this type is not an alternate production trust boundary.
struct DarwinSanitizerPublicationNativeOperationsV1 {
  std::function<llvm::Expected<uint32_t>()> GetEffectiveUserID;
  std::function<llvm::Expected<int>(llvm::StringRef)> OpenSourceNoFollow;
  std::function<llvm::Expected<int>(llvm::StringRef)> OpenDirectoryNoFollow;
  std::function<llvm::Expected<int>(int, llvm::StringRef)>
      OpenAnchoredReadOnlyNoFollow;
  /// Creates an unpredictable name beginning with Prefix using O_CREAT,
  /// O_EXCL, O_NOFOLLOW and O_CLOEXEC, returning the actual single-component
  /// name together with its held descriptor.
  std::function<llvm::Expected<DarwinPublicationCreatedFileV1>(
      int, llvm::StringRef, uint32_t)>
      CreateAnchoredExclusive;
  std::function<llvm::Expected<DarwinPublicationFileStatusV1>(int)> Stat;
  std::function<llvm::Expected<std::optional<DarwinPublicationFileStatusV1>>(
      int, llvm::StringRef)>
      StatAtNoFollow;
  std::function<llvm::Expected<size_t>(int, uint64_t,
                                       llvm::MutableArrayRef<uint8_t>)>
      ReadAt;
  std::function<llvm::Expected<size_t>(int, uint64_t, llvm::ArrayRef<uint8_t>)>
      WriteAt;
  /// Big-endian acl_copy_ext representation; nullopt means no extended ACL.
  std::function<llvm::Expected<std::optional<CanonicalSecurityMetadata>>(int)>
      ReadCanonicalACL;
  std::function<llvm::Error(int,
                            const std::optional<CanonicalSecurityMetadata> &)>
      WriteCanonicalACL;
  std::function<llvm::Expected<std::vector<std::string>>(int)> ListXattrs;
  std::function<llvm::Expected<CanonicalSecurityMetadata>(int, llvm::StringRef)>
      ReadXattr;
  std::function<llvm::Error(int, llvm::StringRef, llvm::ArrayRef<uint8_t>)>
      WriteXattr;
  std::function<llvm::Error(int, uint32_t, uint32_t)> ChangeOwner;
  std::function<llvm::Error(int, uint32_t)> ChangeMode;
  std::function<llvm::Expected<DarwinPublicationVolumeCapabilitiesV1>(int)>
      QueryVolumeCapabilities;
  /// Performs exactly one renameatx_np(..., RENAME_EXCL) equivalent.
  std::function<NamespacePublishResultV1(int, llvm::StringRef, int,
                                         llvm::StringRef)>
      RenameExclusive;
  std::function<llvm::Error(int, llvm::StringRef)> UnlinkAt;
  std::function<llvm::Error(int)> Close;
};

/// Test-only entry point implementing the complete adapter state machine over
/// an injected semantic syscall boundary.  It enforces the same held-binding
/// lifetime as the native factory.
llvm::Expected<PreparedDarwinSanitizerPublicationV1>
prepareDarwinSanitizerPublicationWithOperationsV1(
    DarwinSanitizerPublicationRequestV1 Request,
    DarwinSanitizerPublicationNativeOperationsV1 Operations);

} // namespace detail
} // namespace neverd::safety::sanitizer_publication_metadata

#endif // NEVERD_SAFETY_DARWINSANITIZERPUBLICATIONADAPTER_H
