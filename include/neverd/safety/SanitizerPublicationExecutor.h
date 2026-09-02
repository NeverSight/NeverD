//===- SanitizerPublicationExecutor.h - Fail-closed publication -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the platform-neutral coordinator for executing an authenticated
/// `security-metadata-plan-v1` transaction.  Platform adapters own all native
/// handles and filesystem operations; this layer owns ordering, raw-material
/// digest verification, complete readback verification, and failure cleanup.
///
/// A successful create-exclusive receipt proves only an atomic no-replace
/// namespace publication.  It deliberately does not claim replacement CAS or
/// crash durability.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SANITIZERPUBLICATIONEXECUTOR_H
#define NEVERD_SAFETY_SANITIZERPUBLICATIONEXECUTOR_H

#include "neverd/safety/SanitizerPublicationMetadata.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::safety::sanitizer_publication_metadata {

inline constexpr std::string_view kSanitizerPublicationExecutorAdapter =
    "security-metadata-exec-v1";
inline constexpr uint32_t kSanitizerPublicationExecutorSchemaVersion = 1;
inline constexpr uint32_t kSanitizerPublicationArtifactBindingVersion = 1;

/// Whether the one allowed namespace-publish callback definitely changed the
/// final namespace.  `Indeterminate` is always a failed transaction and never
/// produces a successful receipt.
enum class SanitizerPublicationOutcome : uint8_t {
  NotPublished = 1,
  Published = 2,
  Indeterminate = 3,
};

enum class SanitizerPublicationExecutionReason : uint16_t {
  None = 0,
  UnsupportedPlanVersion = 1,
  PlanNotReady = 2,
  MissingPlanPlatform = 3,
  UnsupportedPlanPlatform = 4,
  InvalidPlannerGuarantees = 5,
  InvalidNamespaceDisposition = 6,
  InvalidNoChangePlan = 7,
  InvalidCandidateMutationDisposition = 8,
  InvalidSELinuxPolicyState = 9,
  NonCanonicalActionOrder = 10,
  DuplicateAction = 11,
  InvalidAction = 12,
  IncompleteActionSet = 13,
  MissingOperation = 14,
  InsufficientExecutorCapabilities = 15,
  NoChangeReauthenticationFailed = 16,
  BeginFailed = 17,
  CreateExclusiveFailed = 18,
  CandidateReauthenticationFailed = 19,
  MaterialReadFailed = 20,
  MaterialDigestMismatch = 21,
  MetadataApplyFailed = 22,
  MetadataEnumerationFailed = 23,
  IncompleteMetadataEnumeration = 24,
  UnsupportedMetadataObserved = 25,
  NonCanonicalMetadataObservation = 26,
  DuplicateMetadataObservation = 27,
  UnexpectedMetadataObserved = 28,
  MissingMetadataObservation = 29,
  MetadataPresenceMismatch = 30,
  MetadataDigestMismatch = 31,
  DestinationReauthenticationFailed = 32,
  NamespacePublishNotPerformed = 33,
  NamespacePublishIndeterminate = 34,
  CallbackException = 35,
  MaterialByteBudgetExceeded = 36,
  ReadbackByteBudgetExceeded = 37,
  ArithmeticOverflow = 38,
  InvalidCanonicalMaterial = 39,
  InvalidSourceMetadataManifest = 40,
  SourceMetadataEnumerationFailed = 41,
  InvalidArtifactBinding = 42,
  SourceAuthenticationFailed = 43,
  SourceAuthenticationMismatch = 44,
  CandidateAuthenticationFailed = 45,
  CandidateAuthenticationMismatch = 46,
  PublishedFinalAuthenticationFailed = 47,
  PublishedFinalAuthenticationMismatch = 48,
  PublishedFinalizationFailed = 49,
};

static_assert(
    static_cast<uint8_t>(SanitizerPublicationOutcome::NotPublished) == 1 &&
    static_cast<uint8_t>(SanitizerPublicationOutcome::Published) == 2 &&
    static_cast<uint8_t>(SanitizerPublicationOutcome::Indeterminate) == 3);
static_assert(
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::None) == 0 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::UnsupportedPlanVersion) == 1 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::PlanNotReady) ==
        2 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MissingPlanPlatform) == 3 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::UnsupportedPlanPlatform) == 4 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidPlannerGuarantees) == 5 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidNamespaceDisposition) ==
        6 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidNoChangePlan) == 7 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::
                              InvalidCandidateMutationDisposition) == 8 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState) == 9 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::NonCanonicalActionOrder) == 10 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::DuplicateAction) == 11 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::InvalidAction) ==
        12 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::IncompleteActionSet) == 13 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MissingOperation) == 14 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::
                              InsufficientExecutorCapabilities) == 15 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::NoChangeReauthenticationFailed) ==
        16 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::BeginFailed) ==
        17 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::CreateExclusiveFailed) == 18 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::CandidateReauthenticationFailed) ==
        19 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MaterialReadFailed) == 20 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MaterialDigestMismatch) == 21 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MetadataApplyFailed) == 22 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MetadataEnumerationFailed) == 23 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration) ==
        24 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::UnsupportedMetadataObserved) ==
        25 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation) ==
        26 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::DuplicateMetadataObservation) ==
        27 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::UnexpectedMetadataObserved) ==
        28 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MissingMetadataObservation) ==
        29 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MetadataPresenceMismatch) == 30 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MetadataDigestMismatch) == 31 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::
                              DestinationReauthenticationFailed) == 32 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::NamespacePublishNotPerformed) ==
        33 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::NamespacePublishIndeterminate) ==
        34 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::CallbackException) == 35 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::MaterialByteBudgetExceeded) ==
        36 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded) ==
        37 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::ArithmeticOverflow) == 38 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidCanonicalMaterial) == 39 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest) ==
        40 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::SourceMetadataEnumerationFailed) ==
        41 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::InvalidArtifactBinding) == 42 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::SourceAuthenticationFailed) ==
        43 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::SourceAuthenticationMismatch) ==
        44 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::CandidateAuthenticationFailed) ==
        45 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch) ==
        46 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::
                              PublishedFinalAuthenticationFailed) == 47 &&
    static_cast<uint16_t>(SanitizerPublicationExecutionReason::
                              PublishedFinalAuthenticationMismatch) == 48 &&
    static_cast<uint16_t>(
        SanitizerPublicationExecutionReason::PublishedFinalizationFailed) ==
        49);

const char *toString(SanitizerPublicationOutcome Outcome);
const char *toString(SanitizerPublicationExecutionReason Reason);

/// Raw, canonical platform metadata.  These bytes are transaction-local: they
/// are hashed by the coordinator, passed directly to ApplyMetadata, and never
/// copied into the execution receipt.
using CanonicalSecurityMetadata = std::vector<uint8_t>;

struct CompleteMetadataValueV1 {
  SecurityMetadataSubject Subject = SecurityMetadataSubject::Owner;
  CanonicalSecurityMetadata CanonicalBytes;
};

/// Complete enumeration of every present security-metadata subject on a held
/// inode.  Absent subjects are omitted.  Values must be strictly increasing by
/// Subject.  A platform adapter sets UnsupportedMetadataCount for every xattr,
/// ADS, or security surface it cannot represent in v1.  Presence-only set-id
/// subjects use an empty CanonicalBytes value.
struct CompleteMetadataSnapshotV1 {
  bool EnumerationComplete = false;
  uint32_t UnsupportedMetadataCount = 0;
  std::vector<CompleteMetadataValueV1> Values;
};

/// Digest-only content identity.  Content size is intentionally not subject to
/// the 16 MiB metadata budget; adapters authenticate it without returning raw
/// artifact bytes to this coordinator.
struct ArtifactContentDigestV1 {
  std::array<uint8_t, 32> SHA256{};
  uint64_t Size = 0;
};

inline bool operator==(const ArtifactContentDigestV1 &LHS,
                       const ArtifactContentDigestV1 &RHS) {
  return LHS.SHA256 == RHS.SHA256 && LHS.Size == RHS.Size;
}

inline bool operator!=(const ArtifactContentDigestV1 &LHS,
                       const ArtifactContentDigestV1 &RHS) {
  return !(LHS == RHS);
}

/// SHA-256 of a platform adapter's canonical stable-object identity encoding
/// (for example device/inode/generation or volume/file-id).  Paths are never
/// stable identities.
struct ArtifactStableIdentityDigestV1 {
  std::array<uint8_t, 32> SHA256{};
};

inline bool operator==(const ArtifactStableIdentityDigestV1 &LHS,
                       const ArtifactStableIdentityDigestV1 &RHS) {
  return LHS.SHA256 == RHS.SHA256;
}

inline bool operator!=(const ArtifactStableIdentityDigestV1 &LHS,
                       const ArtifactStableIdentityDigestV1 &RHS) {
  return !(LHS == RHS);
}

/// Caller-authenticated byte and object binding, independent of the metadata
/// planner.  CreateExclusive requires an authenticated candidate digest and a
/// CandidateMutation value consistent with source/candidate digest equality.
struct ArtifactBindingV1 {
  uint32_t Version = kSanitizerPublicationArtifactBindingVersion;
  ArtifactContentDigestV1 ExpectedSourceContent;
  ArtifactStableIdentityDigestV1 ExpectedSourceStableIdentity;
  ArtifactContentDigestV1 ExpectedCandidateContent;
};

/// One handle-bound object authentication result.  A valid publication object
/// is always a regular file with exactly one link.
struct ArtifactAuthenticationV1 {
  PublicationNodeKind Node = PublicationNodeKind::Other;
  uint64_t LinkCount = 0;
  ArtifactContentDigestV1 Content;
  ArtifactStableIdentityDigestV1 StableIdentity;
};

/// Atomic typed observation of object identity/content and its complete
/// metadata present set.  Adapters must derive both halves from the same held
/// object without an intervening pathname lookup.
struct AuthenticatedSourceArtifactV1 {
  ArtifactAuthenticationV1 Artifact;
  CompleteMetadataSnapshotV1 Metadata;
};

struct AuthenticatedCandidateArtifactV1 {
  ArtifactAuthenticationV1 Artifact;
  CompleteMetadataSnapshotV1 Metadata;
};

struct AuthenticatedPublishedFinalArtifactV1 {
  ArtifactAuthenticationV1 Artifact;
  CompleteMetadataSnapshotV1 Metadata;
};

/// One read-only NoChange reauthentication frame.  HeldSource is observed via
/// the already pinned source handle; AnchoredFinal is resolved through the
/// anchored destination directory and final name.  Both must describe the
/// exact same bound object and complete metadata present set.
struct AuthenticatedNoChangeArtifactsV1 {
  AuthenticatedSourceArtifactV1 HeldSource;
  AuthenticatedPublishedFinalArtifactV1 AnchoredFinal;
};

/// Strength of the proof that the namespace operand consumed by the atomic
/// publish operation still names the candidate authenticated through its held
/// handle.  AccessControlConfinedDistinctCredentials excludes attackers that
/// share the adapter process's effective credentials or possess DAC-bypass
/// authority.  KernelHeldObject is reserved for primitives that bind the
/// authenticated object in-kernel without a mutable source pathname.
enum class CandidatePublishOperandBindingV1 : uint8_t {
  None = 0,
  AccessControlConfinedDistinctCredentials = 1,
  KernelHeldObject = 2,
};

static_assert(
    static_cast<uint8_t>(CandidatePublishOperandBindingV1::None) == 0 &&
    static_cast<uint8_t>(CandidatePublishOperandBindingV1::
                             AccessControlConfinedDistinctCredentials) == 1 &&
    static_cast<uint8_t>(CandidatePublishOperandBindingV1::KernelHeldObject) ==
        2);

const char *toString(CandidatePublishOperandBindingV1 Binding);

/// Capabilities are explicit adapter claims checked before Begin.  Native
/// implementations must back them with held descriptors/HANDLEs and an atomic
/// no-replace primitive.  CandidatePublishOperandBinding additionally records
/// how the source operand is protected from substitution after candidate
/// authentication.  Pathname-only check-then-rename implementations must
/// leave the corresponding values unproven.
struct SanitizerPublicationExecutorCapabilitiesV1 {
  bool SourceIdentityPinned = false;
  bool DestinationDirectoryAnchored = false;
  bool TemporaryCreationExclusive = false;
  CandidatePublishOperandBindingV1 CandidatePublishOperandBinding =
      CandidatePublishOperandBindingV1::None;
  bool CompleteSourceMetadataEnumeration = false;
  bool CompleteCandidateMetadataEnumeration = false;
  bool CompletePublishedFinalMetadataEnumeration = false;
  bool ArtifactContentAuthentication = false;
  bool StableObjectIdentityAuthentication = false;
  bool AnchoredNoChangeFinalAuthentication = false;
  bool AnchoredPublishedFinalAuthentication = false;
  bool AtomicNoReplacePublish = false;
};

struct NamespacePublishResultV1 {
  SanitizerPublicationOutcome Outcome =
      SanitizerPublicationOutcome::Indeterminate;
  /// Must be empty for Published.  It may describe a definite failure or why
  /// the adapter cannot determine whether its one publish attempt committed.
  std::string Detail;
};

/// Platform boundary for one publication transaction.  Begin must pin and
/// reauthenticate the source identity and final directory without changing the
/// namespace.  Cleanup is armed before Begin is invoked: Discard must tolerate
/// a partially initialized Begin and is called exactly once if Begin returns an
/// error or throws.  CreateExclusiveCandidate creates/stages one same-directory
/// temp inode and retains its native handle.  Discard must be safe after every
/// non-Published result and must never remove the final destination.  Once
/// PublishNoReplace reports Published, Discard is forbidden and exactly one
/// FinalizePublished call closes held state without deleting the final inode.
struct SanitizerPublicationExecutorOperationsV1 {
  SanitizerPublicationExecutorCapabilitiesV1 Capabilities;

  /// The only callback invoked for NoChange.  One read-only frame
  /// authenticates the held source and anchored final name, including both
  /// complete metadata snapshots.  No other operation may be invoked.
  std::function<llvm::Expected<AuthenticatedNoChangeArtifactsV1>()>
      ReauthenticateNoChange;
  std::function<llvm::Error()> Begin;
  std::function<llvm::Expected<AuthenticatedSourceArtifactV1>()>
      AuthenticateSourceAfterBegin;
  std::function<llvm::Error()> CreateExclusiveCandidate;
  std::function<llvm::Expected<ArtifactAuthenticationV1>()>
      AuthenticateCandidateAfterCreate;
  std::function<llvm::Expected<CanonicalSecurityMetadata>(
      SecurityMetadataSubject, SecurityMetadataOrigin)>
      ReadMaterial;
  std::function<llvm::Error(const SecurityMetadataAction &,
                            llvm::ArrayRef<uint8_t>)>
      ApplyMetadata;
  std::function<llvm::Expected<AuthenticatedCandidateArtifactV1>()>
      AuthenticateCandidateAfterMetadata;
  std::function<llvm::Error()> ReauthenticateDestinationAbsent;
  std::function<NamespacePublishResultV1()> PublishNoReplace;
  /// Anchored lookup of the committed final name, including a complete
  /// metadata enumeration from the same observed object.
  std::function<llvm::Expected<AuthenticatedPublishedFinalArtifactV1>()>
      AuthenticatePublishedFinal;
  /// Exactly-once post-Published handle cleanup.  It must never unlink or
  /// otherwise mutate the committed final artifact.
  std::function<llvm::Error()> FinalizePublished;
  std::function<llvm::Error()> Discard;
};

struct SanitizerPublicationExecutionGuaranteesV1 {
  bool NamespaceAtomic = false;
  bool DestinationCreateExclusive = false;
  bool CompareAndSwap = false;
  bool CrashDurable = false;
  CandidatePublishOperandBindingV1 CandidatePublishOperandBinding =
      CandidatePublishOperandBindingV1::None;
};

struct ArtifactAuthenticationReceiptV1 {
  ArtifactContentDigestV1 Content;
  ArtifactStableIdentityDigestV1 StableIdentity;
  bool RegularFileMatched = false;
  bool SingleLinkMatched = false;
  bool ContentMatched = false;
  bool StableIdentityMatched = false;
};

/// Successful receipts contain only counts, authenticated content/identity
/// digests and sizes, match bits, enum values, and guarantee bits.  Raw
/// artifact, owner, ACL, SID, xattr, and ADS bytes never leave the executor.
struct SanitizerPublicationExecutionReceiptV1 {
  uint32_t Version = kSanitizerPublicationExecutorSchemaVersion;
  bool Complete = false;
  std::optional<PublicationPlatform> Platform;
  PublicationNamespaceDisposition NamespaceDisposition =
      PublicationNamespaceDisposition::None;
  uint32_t AppliedActionCount = 0;
  uint32_t VerifiedActionCount = 0;
  uint32_t ObservedSourceMetadataCount = 0;
  uint32_t ObservedCandidateMetadataCount = 0;
  uint32_t ObservedFinalMetadataCount = 0;
  std::optional<ArtifactAuthenticationReceiptV1> SourceArtifact;
  std::optional<ArtifactAuthenticationReceiptV1> CandidateArtifact;
  std::optional<ArtifactAuthenticationReceiptV1> FinalArtifact;
  SanitizerPublicationExecutionGuaranteesV1 Guarantees;
};

struct SanitizerPublicationExecutionResultV1 {
  SanitizerPublicationExecutionReceiptV1 Receipt;
  SanitizerPublicationExecutionReason Reason =
      SanitizerPublicationExecutionReason::None;
  SanitizerPublicationOutcome Outcome =
      SanitizerPublicationOutcome::NotPublished;
  /// Failure detail is diagnostic only and is not part of the receipt.
  std::string Detail;

  bool succeeded() const {
    return Receipt.Complete &&
           Reason == SanitizerPublicationExecutionReason::None &&
           Outcome != SanitizerPublicationOutcome::Indeterminate;
  }
};

OpaqueMetadataDigest
digestCanonicalSecurityMetadata(llvm::ArrayRef<uint8_t> CanonicalBytes);

/// Executes one plan with exactly zero namespace/metadata mutations for
/// NoChange, or one create-exclusive candidate followed by at most one atomic
/// no-replace publish for CreateExclusive.  The independent artifact binding
/// is checked at source, candidate, and committed-final authentication points.
SanitizerPublicationExecutionResultV1 executeSanitizerPublicationMetadata(
    const SanitizerPublicationMetadataPlanV1 &Plan,
    const ArtifactBindingV1 &Binding,
    const SanitizerPublicationExecutorOperationsV1 &Operations);

} // namespace neverd::safety::sanitizer_publication_metadata

#endif // NEVERD_SAFETY_SANITIZERPUBLICATIONEXECUTOR_H
