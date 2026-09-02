//===- SanitizerPublicationMetadata.h - Security metadata -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the pure, fail-closed `security-metadata-plan-v1` contract.  The
/// caller supplies already-observed, digest-only filesystem facts; the planner
/// returns dispositions that a later authenticated publisher may execute.
///
/// This planner performs no filesystem operation.  In particular, successful
/// planning does not guarantee namespace atomicity, compare-and-swap exclusion,
/// crash durability, or that any metadata action was applied.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SANITIZERPUBLICATIONMETADATA_H
#define NEVERD_SAFETY_SANITIZERPUBLICATIONMETADATA_H

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace neverd::safety::sanitizer_publication_metadata {

inline constexpr std::string_view kSanitizerPublicationMetadataAdapter =
    "security-metadata-plan-v1";
inline constexpr uint32_t kSanitizerPublicationMetadataSchemaVersion = 1;

inline constexpr uint32_t kSanitizerPublicationMetadataMaxRecords = 64;
inline constexpr uint32_t kSanitizerPublicationMetadataMaxActions = 64;
inline constexpr uint64_t kSanitizerPublicationMetadataMaxEncodedBytes =
    uint64_t{1} << 24;
inline constexpr uint64_t kSanitizerPublicationPOSIXModeEncodedBytes = 4;

enum class PublicationPlatform : uint8_t {
  Linux = 1,
  MacOS = 2,
  Windows = 3,
};

enum class PublicationNodeKind : uint8_t {
  Absent = 1,
  RegularFile = 2,
  Symlink = 3,
  ReparsePoint = 4,
  Other = 5,
};

/// Typed security metadata known to v1.  Unknown xattrs and ADS are explicit
/// values so discovery layers cannot silently omit them from policy.
enum class SecurityMetadataKind : uint8_t {
  MacOSQuarantine = 1,
  MacOSProvenance = 2,
  LinuxCapability = 3,
  LinuxSELinux = 4,
  LinuxIMA = 5,
  LinuxEVM = 6,
  WindowsMOTW = 7,
  UnknownExtendedAttribute = 8,
  UnknownAlternateDataStream = 9,
};

enum class SecurityMetadataOrigin : uint8_t {
  SourceSnapshot = 1,
  FinalPathDerived = 2,
};

/// Result of comparing an authenticated candidate byte snapshot with the
/// authenticated source byte snapshot.  Guard counts are deliberately not a
/// substitute for this byte-level evidence: non-guard transformations can
/// mutate an artifact, and a zero-guard plan can still publish different bytes.
enum class CandidateMutationDisposition : uint8_t {
  Unauthenticated = 0,
  ByteIdentical = 1,
  ByteMutated = 2,
};

/// Attested SELinux state for the final publication path.  `Unavailable` is the
/// fail-closed default for Linux create-exclusive publication.  A
/// final-path-derived policy requires an exact readable label observation even
/// when the source has no SELinux xattr.  A true no-change plan does not
/// consult transform-specific SELinux policy because it performs no operation.
enum class SELinuxPolicyState : uint8_t {
  DisabledAttested = 1,
  FinalPathDerived = 2,
  Unavailable = 3,
};

struct OpaqueMetadataDigest {
  std::array<uint8_t, 32> SHA256{};
  uint64_t EncodedBytes = 0;
};

inline bool operator==(const OpaqueMetadataDigest &LHS,
                       const OpaqueMetadataDigest &RHS) {
  return LHS.SHA256 == RHS.SHA256 && LHS.EncodedBytes == RHS.EncodedBytes;
}

inline bool operator!=(const OpaqueMetadataDigest &LHS,
                       const OpaqueMetadataDigest &RHS) {
  return !(LHS == RHS);
}

/// Digest-only observation.  No raw quarantine payload, SID, ACL, xattr, or
/// ADS bytes enter the plan or its eventual receipt.
struct SecurityMetadataObservation {
  SecurityMetadataKind Kind = SecurityMetadataKind::UnknownExtendedAttribute;
  SecurityMetadataOrigin Origin = SecurityMetadataOrigin::SourceSnapshot;
  bool Readable = false;
  OpaqueMetadataDigest Value;
};

inline bool operator==(const SecurityMetadataObservation &LHS,
                       const SecurityMetadataObservation &RHS) {
  return LHS.Kind == RHS.Kind && LHS.Origin == RHS.Origin &&
         LHS.Readable == RHS.Readable && LHS.Value == RHS.Value;
}

inline bool operator!=(const SecurityMetadataObservation &LHS,
                       const SecurityMetadataObservation &RHS) {
  return !(LHS == RHS);
}

struct FileSecurityMetadataSnapshot {
  PublicationNodeKind Node = PublicationNodeKind::Absent;
  uint64_t LinkCount = 0;
  /// Owner is a digest of a canonical UID/GID or Windows owner SID encoding.
  bool OwnerReadable = false;
  std::optional<OpaqueMetadataDigest> Owner;
  /// ACL covers the complete canonical POSIX ACL or Windows security-descriptor
  /// policy selected by the observation layer, never raw ACL/SID bytes.
  bool ACLReadable = false;
  std::optional<OpaqueMetadataDigest> ACL;
  /// POSIX rwx/sticky mode is encoded as exactly four little-endian bytes of
  /// `st_mode & 01777`.  File-type and set-id bits are separate policy facts.
  /// Windows snapshots must leave both fields absent.
  bool POSIXModeReadable = false;
  std::optional<OpaqueMetadataDigest> POSIXMode;
  bool SetUID = false;
  bool SetGID = false;
  /// True only after the platform observer completed xattr/ADS enumeration.
  bool AttributeEnumerationComplete = false;
  /// Strictly increasing by SecurityMetadataKind, with no duplicate kind.
  std::vector<SecurityMetadataObservation> Attributes;
};

inline bool operator==(const FileSecurityMetadataSnapshot &LHS,
                       const FileSecurityMetadataSnapshot &RHS) {
  return LHS.Node == RHS.Node && LHS.LinkCount == RHS.LinkCount &&
         LHS.OwnerReadable == RHS.OwnerReadable && LHS.Owner == RHS.Owner &&
         LHS.ACLReadable == RHS.ACLReadable && LHS.ACL == RHS.ACL &&
         LHS.POSIXModeReadable == RHS.POSIXModeReadable &&
         LHS.POSIXMode == RHS.POSIXMode && LHS.SetUID == RHS.SetUID &&
         LHS.SetGID == RHS.SetGID &&
         LHS.AttributeEnumerationComplete == RHS.AttributeEnumerationComplete &&
         LHS.Attributes == RHS.Attributes;
}

inline bool operator!=(const FileSecurityMetadataSnapshot &LHS,
                       const FileSecurityMetadataSnapshot &RHS) {
  return !(LHS == RHS);
}

struct SanitizerPublicationMetadataRequestV1 {
  uint32_t Version = kSanitizerPublicationMetadataSchemaVersion;
  PublicationPlatform Platform = PublicationPlatform::Linux;
  uint64_t GuardedSiteCount = 0;
  /// Authenticated byte comparison result for the exact candidate that would be
  /// published.  Same-source no-change requires `ByteIdentical` explicitly.
  CandidateMutationDisposition CandidateMutation =
      CandidateMutationDisposition::Unauthenticated;
  /// Linux create-exclusive callers must attest disabled policy or request a
  /// final-path-derived label.  `Unavailable` fails closed for that
  /// disposition.
  SELinuxPolicyState SELinuxPolicy = SELinuxPolicyState::Unavailable;
  /// True only when the observation layer authenticated both paths as the same
  /// file.  The planner itself provides no path identity or CAS guarantee.
  bool SameSourceAndDestination = false;
  FileSecurityMetadataSnapshot Source;
  FileSecurityMetadataSnapshot Destination;
  /// An SELinux label is accepted only when a final-path policy lookup produced
  /// it.  A source label is never treated as a guess for the final path.
  std::optional<SecurityMetadataObservation> FinalPathSELinuxLabel;
};

struct SanitizerPublicationMetadataLimits {
  /// Zero is an actual zero budget.  Values above the hard ceiling are
  /// rejected rather than clamped.
  uint32_t MaxRecords = kSanitizerPublicationMetadataMaxRecords;
  uint32_t MaxActions = kSanitizerPublicationMetadataMaxActions;
  uint64_t MaxEncodedBytes = kSanitizerPublicationMetadataMaxEncodedBytes;
};

enum class PublicationNamespaceDisposition : uint8_t {
  None = 0,
  CreateExclusive = 1,
  NoChange = 2,
};

enum class SecurityMetadataSubject : uint8_t {
  Owner = 1,
  ACL = 2,
  SetUIDBit = 3,
  SetGIDBit = 4,
  MacOSQuarantine = 5,
  MacOSProvenance = 6,
  LinuxCapability = 7,
  LinuxSELinux = 8,
  WindowsMOTW = 9,
  POSIXMode = 10,
};

enum class SecurityMetadataDisposition : uint8_t {
  PreserveFromSource = 1,
  Clear = 2,
  ApplyFinalPathDerived = 3,
  /// Remove the subject and require the executor to attest its final absence.
  ClearAndVerifyAbsent = 4,
};

struct SecurityMetadataAction {
  SecurityMetadataSubject Subject = SecurityMetadataSubject::Owner;
  SecurityMetadataDisposition Disposition =
      SecurityMetadataDisposition::PreserveFromSource;
  std::optional<OpaqueMetadataDigest> ExpectedDigest;
};

/// Canonical digest-only present-set entry for the exact source metadata
/// snapshot that was planned.  Membership means present; absence from the
/// sorted manifest means absent.  SetUIDBit and SetGIDBit are presence-only and
/// therefore have no digest.  Every other present subject must have a digest.
struct SourceMetadataManifestEntryV1 {
  SecurityMetadataSubject Subject = SecurityMetadataSubject::Owner;
  std::optional<OpaqueMetadataDigest> ExpectedDigest;
};

inline bool operator==(const SourceMetadataManifestEntryV1 &LHS,
                       const SourceMetadataManifestEntryV1 &RHS) {
  return LHS.Subject == RHS.Subject && LHS.ExpectedDigest == RHS.ExpectedDigest;
}

inline bool operator!=(const SourceMetadataManifestEntryV1 &LHS,
                       const SourceMetadataManifestEntryV1 &RHS) {
  return !(LHS == RHS);
}

struct SanitizerPublicationPlannerGuarantees {
  /// All remain false in this pure planner.  An executor must independently
  /// attest any namespace, concurrency, and persistence properties it offers.
  bool NamespaceAtomic = false;
  bool CompareAndSwap = false;
  bool CrashDurable = false;
};

enum class SanitizerPublicationMetadataReason : uint16_t {
  None = 0,
  UnsupportedVersion = 1,
  InvalidLimits = 2,
  UnsupportedPlatform = 3,
  InvalidSourceNode = 4,
  InvalidDestinationNode = 5,
  SymlinkUnsupported = 6,
  ReparsePointUnsupported = 7,
  HardlinkUnsupported = 8,
  MissingOwnerMetadata = 9,
  UnreadableOwner = 10,
  UnreadableACL = 11,
  UnreadableMetadata = 12,
  MetadataRecordLimitExceeded = 13,
  MetadataByteBudgetExceeded = 14,
  ActionLimitExceeded = 15,
  ArithmeticOverflow = 16,
  NonCanonicalMetadataOrder = 17,
  DuplicateMetadata = 18,
  UnsupportedMetadataKind = 19,
  MetadataPlatformMismatch = 20,
  UnknownExtendedAttribute = 21,
  UnknownAlternateDataStream = 22,
  IntegrityMetadataUnsupported = 23,
  ProvenanceConflict = 24,
  SELinuxFinalPathLabelRequired = 25,
  SELinuxSourceLabelForbidden = 26,
  InvalidFinalPathSELinuxLabel = 27,
  SameSourceGuardedUnsupported = 28,
  ExistingDestinationCASUnsupported = 29,
  InvalidAbsentDestination = 30,
  SnapshotConflict = 31,
  InvalidMetadataOrigin = 32,
  IncompleteMetadataEnumeration = 33,
  MissingACLMetadata = 34,
  CandidateMutationAttestationRequired = 35,
  SameSourceMutationUnsupported = 36,
  InvalidCandidateMutationDisposition = 37,
  SELinuxPolicyUnavailable = 38,
  InvalidSELinuxPolicyState = 39,
  SELinuxPolicyConflict = 40,
  UnreadablePOSIXMode = 41,
  MissingPOSIXModeMetadata = 42,
  InvalidPOSIXModeEncoding = 43,
};

static_assert(static_cast<uint8_t>(PublicationPlatform::Linux) == 1 &&
              static_cast<uint8_t>(PublicationPlatform::MacOS) == 2 &&
              static_cast<uint8_t>(PublicationPlatform::Windows) == 3);
static_assert(static_cast<uint8_t>(PublicationNodeKind::Absent) == 1 &&
              static_cast<uint8_t>(PublicationNodeKind::RegularFile) == 2 &&
              static_cast<uint8_t>(PublicationNodeKind::Symlink) == 3 &&
              static_cast<uint8_t>(PublicationNodeKind::ReparsePoint) == 4 &&
              static_cast<uint8_t>(PublicationNodeKind::Other) == 5);
static_assert(
    static_cast<uint8_t>(SecurityMetadataKind::MacOSQuarantine) == 1 &&
    static_cast<uint8_t>(SecurityMetadataKind::MacOSProvenance) == 2 &&
    static_cast<uint8_t>(SecurityMetadataKind::LinuxCapability) == 3 &&
    static_cast<uint8_t>(SecurityMetadataKind::LinuxSELinux) == 4 &&
    static_cast<uint8_t>(SecurityMetadataKind::LinuxIMA) == 5 &&
    static_cast<uint8_t>(SecurityMetadataKind::LinuxEVM) == 6 &&
    static_cast<uint8_t>(SecurityMetadataKind::WindowsMOTW) == 7 &&
    static_cast<uint8_t>(SecurityMetadataKind::UnknownExtendedAttribute) == 8 &&
    static_cast<uint8_t>(SecurityMetadataKind::UnknownAlternateDataStream) ==
        9);
static_assert(static_cast<uint8_t>(SecurityMetadataOrigin::SourceSnapshot) ==
                  1 &&
              static_cast<uint8_t>(SecurityMetadataOrigin::FinalPathDerived) ==
                  2);
static_assert(
    static_cast<uint8_t>(CandidateMutationDisposition::Unauthenticated) == 0 &&
    static_cast<uint8_t>(CandidateMutationDisposition::ByteIdentical) == 1 &&
    static_cast<uint8_t>(CandidateMutationDisposition::ByteMutated) == 2);
static_assert(static_cast<uint8_t>(SELinuxPolicyState::DisabledAttested) == 1 &&
              static_cast<uint8_t>(SELinuxPolicyState::FinalPathDerived) == 2 &&
              static_cast<uint8_t>(SELinuxPolicyState::Unavailable) == 3);
static_assert(
    static_cast<uint8_t>(PublicationNamespaceDisposition::None) == 0 &&
    static_cast<uint8_t>(PublicationNamespaceDisposition::CreateExclusive) ==
        1 &&
    static_cast<uint8_t>(PublicationNamespaceDisposition::NoChange) == 2);
static_assert(
    static_cast<uint8_t>(SecurityMetadataSubject::Owner) == 1 &&
    static_cast<uint8_t>(SecurityMetadataSubject::ACL) == 2 &&
    static_cast<uint8_t>(SecurityMetadataSubject::SetUIDBit) == 3 &&
    static_cast<uint8_t>(SecurityMetadataSubject::SetGIDBit) == 4 &&
    static_cast<uint8_t>(SecurityMetadataSubject::MacOSQuarantine) == 5 &&
    static_cast<uint8_t>(SecurityMetadataSubject::MacOSProvenance) == 6 &&
    static_cast<uint8_t>(SecurityMetadataSubject::LinuxCapability) == 7 &&
    static_cast<uint8_t>(SecurityMetadataSubject::LinuxSELinux) == 8 &&
    static_cast<uint8_t>(SecurityMetadataSubject::WindowsMOTW) == 9 &&
    static_cast<uint8_t>(SecurityMetadataSubject::POSIXMode) == 10);
static_assert(
    static_cast<uint8_t>(SecurityMetadataDisposition::PreserveFromSource) ==
        1 &&
    static_cast<uint8_t>(SecurityMetadataDisposition::Clear) == 2 &&
    static_cast<uint8_t>(SecurityMetadataDisposition::ApplyFinalPathDerived) ==
        3 &&
    static_cast<uint8_t>(SecurityMetadataDisposition::ClearAndVerifyAbsent) ==
        4);
static_assert(
    static_cast<uint16_t>(SanitizerPublicationMetadataReason::None) == 0 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnsupportedVersion) == 1 &&
    static_cast<uint16_t>(SanitizerPublicationMetadataReason::InvalidLimits) ==
        2 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnsupportedPlatform) == 3 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidSourceNode) == 4 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidDestinationNode) == 5 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SymlinkUnsupported) == 6 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::ReparsePointUnsupported) == 7 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::HardlinkUnsupported) == 8 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::MissingOwnerMetadata) == 9 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnreadableOwner) == 10 &&
    static_cast<uint16_t>(SanitizerPublicationMetadataReason::UnreadableACL) ==
        11 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnreadableMetadata) == 12 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded) ==
        13 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::MetadataByteBudgetExceeded) == 14 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::ActionLimitExceeded) == 15 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::ArithmeticOverflow) == 16 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::NonCanonicalMetadataOrder) == 17 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::DuplicateMetadata) == 18 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnsupportedMetadataKind) == 19 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::MetadataPlatformMismatch) == 20 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnknownExtendedAttribute) == 21 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnknownAlternateDataStream) == 22 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::IntegrityMetadataUnsupported) ==
        23 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::ProvenanceConflict) == 24 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SELinuxFinalPathLabelRequired) ==
        25 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SELinuxSourceLabelForbidden) ==
        26 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidFinalPathSELinuxLabel) ==
        27 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SameSourceGuardedUnsupported) ==
        28 &&
    static_cast<uint16_t>(SanitizerPublicationMetadataReason::
                              ExistingDestinationCASUnsupported) == 29 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidAbsentDestination) == 30 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SnapshotConflict) == 31 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidMetadataOrigin) == 32 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::IncompleteMetadataEnumeration) ==
        33 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::MissingACLMetadata) == 34 &&
    static_cast<uint16_t>(SanitizerPublicationMetadataReason::
                              CandidateMutationAttestationRequired) == 35 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SameSourceMutationUnsupported) ==
        36 &&
    static_cast<uint16_t>(SanitizerPublicationMetadataReason::
                              InvalidCandidateMutationDisposition) == 37 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SELinuxPolicyUnavailable) == 38 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidSELinuxPolicyState) == 39 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::SELinuxPolicyConflict) == 40 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::UnreadablePOSIXMode) == 41 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::MissingPOSIXModeMetadata) == 42 &&
    static_cast<uint16_t>(
        SanitizerPublicationMetadataReason::InvalidPOSIXModeEncoding) == 43);

const char *toString(SanitizerPublicationMetadataReason Reason);

struct SanitizerPublicationMetadataPlanV1 {
  uint32_t Version = kSanitizerPublicationMetadataSchemaVersion;
  bool Complete = false;
  SanitizerPublicationMetadataReason Reason =
      SanitizerPublicationMetadataReason::None;
  std::optional<PublicationPlatform> Platform;
  uint64_t GuardedSiteCount = 0;
  CandidateMutationDisposition CandidateMutation =
      CandidateMutationDisposition::Unauthenticated;
  std::optional<SELinuxPolicyState> SELinuxPolicy;
  PublicationNamespaceDisposition NamespaceDisposition =
      PublicationNamespaceDisposition::None;
  SanitizerPublicationPlannerGuarantees Guarantees;
  uint32_t ObservedRecordCount = 0;
  uint64_t ObservedEncodedBytes = 0;
  /// Strictly increasing, duplicate-free, digest-only source present set.
  /// Counts above are derived from this manifest plus final-path-derived action
  /// material, never accepted as independent caller claims.
  std::vector<SourceMetadataManifestEntryV1> SourceMetadata;
  std::vector<SecurityMetadataAction> Actions;

  bool ready() const {
    return Complete && Reason == SanitizerPublicationMetadataReason::None;
  }
};

/// Plans only create-exclusive publication to an absent destination, or an
/// authenticated same-source empty-plan no-change whose exact candidate bytes
/// were independently attested identical.  A different existing destination is
/// rejected because v1 has no filesystem CAS primitive.
/// No-change means zero namespace and metadata operations: the executor must
/// reauthenticate the same file identity and complete metadata snapshot, then
/// leave the existing inode untouched; the plan therefore has no actions.
/// Create-exclusive plans preserve readable source owner and present ACL
/// digests, clear inherited ACL state when a POSIX source has no ACL, preserve
/// the exact POSIX rwx/sticky mode independently of ACLs, clear POSIX set-id
/// bits, always clear and verify absence of Linux capabilities, and apply only
/// an explicitly attested final-path SELinux label.  Windows sources require an
/// explicit complete security-descriptor digest and forbid POSIX mode facts.
SanitizerPublicationMetadataPlanV1 planSanitizerPublicationMetadata(
    const SanitizerPublicationMetadataRequestV1 &Request,
    const SanitizerPublicationMetadataLimits &Limits = {});

} // namespace neverd::safety::sanitizer_publication_metadata

#endif // NEVERD_SAFETY_SANITIZERPUBLICATIONMETADATA_H
