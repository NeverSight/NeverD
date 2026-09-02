//===- SanitizerPublicationMetadata.cpp - Publication metadata plans -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SanitizerPublicationMetadata.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace neverd::safety::sanitizer_publication_metadata {
namespace {

SanitizerPublicationMetadataPlanV1
failure(SanitizerPublicationMetadataReason Reason) {
  SanitizerPublicationMetadataPlanV1 Plan;
  Plan.Reason = Reason;
  return Plan;
}

void addAction(SanitizerPublicationMetadataPlanV1 &Plan,
               SecurityMetadataSubject Subject,
               SecurityMetadataDisposition Disposition,
               std::optional<OpaqueMetadataDigest> Digest = std::nullopt) {
  Plan.Actions.push_back({Subject, Disposition, std::move(Digest)});
}

void addSourceMetadata(
    std::vector<SourceMetadataManifestEntryV1> &Manifest,
    SecurityMetadataSubject Subject,
    std::optional<OpaqueMetadataDigest> Digest = std::nullopt) {
  Manifest.push_back({Subject, std::move(Digest)});
}

SanitizerPublicationMetadataReason
chargeEncodedBytes(uint64_t &Used, uint64_t Amount, uint64_t Limit) {
  if (Amount > std::numeric_limits<uint64_t>::max() - Used)
    return SanitizerPublicationMetadataReason::ArithmeticOverflow;
  Used += Amount;
  if (Used > Limit)
    return SanitizerPublicationMetadataReason::MetadataByteBudgetExceeded;
  return SanitizerPublicationMetadataReason::None;
}

const SecurityMetadataObservation *
findObservation(const FileSecurityMetadataSnapshot &Snapshot,
                SecurityMetadataKind Kind) {
  const auto It =
      std::find_if(Snapshot.Attributes.begin(), Snapshot.Attributes.end(),
                   [Kind](const SecurityMetadataObservation &Observation) {
                     return Observation.Kind == Kind;
                   });
  return It == Snapshot.Attributes.end() ? nullptr : &*It;
}

const SourceMetadataManifestEntryV1 *
findSourceMetadata(const std::vector<SourceMetadataManifestEntryV1> &Manifest,
                   SecurityMetadataSubject Subject) {
  const auto It =
      std::find_if(Manifest.begin(), Manifest.end(),
                   [Subject](const SourceMetadataManifestEntryV1 &Entry) {
                     return Entry.Subject == Subject;
                   });
  return It == Manifest.end() ? nullptr : &*It;
}

SanitizerPublicationMetadataReason
validatePOSIXModeSnapshot(const FileSecurityMetadataSnapshot &Snapshot,
                          PublicationPlatform Platform) {
  if (Platform == PublicationPlatform::Windows)
    return Snapshot.POSIXModeReadable || Snapshot.POSIXMode
               ? SanitizerPublicationMetadataReason::MetadataPlatformMismatch
               : SanitizerPublicationMetadataReason::None;
  if (!Snapshot.POSIXModeReadable)
    return SanitizerPublicationMetadataReason::UnreadablePOSIXMode;
  if (!Snapshot.POSIXMode)
    return SanitizerPublicationMetadataReason::MissingPOSIXModeMetadata;
  if (Snapshot.POSIXMode->EncodedBytes !=
      kSanitizerPublicationPOSIXModeEncodedBytes)
    return SanitizerPublicationMetadataReason::InvalidPOSIXModeEncoding;
  return SanitizerPublicationMetadataReason::None;
}

} // namespace

const char *toString(SanitizerPublicationMetadataReason Reason) {
  switch (Reason) {
  case SanitizerPublicationMetadataReason::None:
    return "none";
  case SanitizerPublicationMetadataReason::UnsupportedVersion:
    return "unsupported_version";
  case SanitizerPublicationMetadataReason::InvalidLimits:
    return "invalid_limits";
  case SanitizerPublicationMetadataReason::UnsupportedPlatform:
    return "unsupported_platform";
  case SanitizerPublicationMetadataReason::InvalidSourceNode:
    return "invalid_source_node";
  case SanitizerPublicationMetadataReason::InvalidDestinationNode:
    return "invalid_destination_node";
  case SanitizerPublicationMetadataReason::SymlinkUnsupported:
    return "symlink_unsupported";
  case SanitizerPublicationMetadataReason::ReparsePointUnsupported:
    return "reparse_point_unsupported";
  case SanitizerPublicationMetadataReason::HardlinkUnsupported:
    return "hardlink_unsupported";
  case SanitizerPublicationMetadataReason::MissingOwnerMetadata:
    return "missing_owner_metadata";
  case SanitizerPublicationMetadataReason::UnreadableOwner:
    return "unreadable_owner";
  case SanitizerPublicationMetadataReason::UnreadableACL:
    return "unreadable_acl";
  case SanitizerPublicationMetadataReason::UnreadableMetadata:
    return "unreadable_metadata";
  case SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded:
    return "metadata_record_limit_exceeded";
  case SanitizerPublicationMetadataReason::MetadataByteBudgetExceeded:
    return "metadata_byte_budget_exceeded";
  case SanitizerPublicationMetadataReason::ActionLimitExceeded:
    return "action_limit_exceeded";
  case SanitizerPublicationMetadataReason::ArithmeticOverflow:
    return "arithmetic_overflow";
  case SanitizerPublicationMetadataReason::NonCanonicalMetadataOrder:
    return "noncanonical_metadata_order";
  case SanitizerPublicationMetadataReason::DuplicateMetadata:
    return "duplicate_metadata";
  case SanitizerPublicationMetadataReason::UnsupportedMetadataKind:
    return "unsupported_metadata_kind";
  case SanitizerPublicationMetadataReason::MetadataPlatformMismatch:
    return "metadata_platform_mismatch";
  case SanitizerPublicationMetadataReason::UnknownExtendedAttribute:
    return "unknown_extended_attribute";
  case SanitizerPublicationMetadataReason::UnknownAlternateDataStream:
    return "unknown_alternate_data_stream";
  case SanitizerPublicationMetadataReason::IntegrityMetadataUnsupported:
    return "integrity_metadata_unsupported";
  case SanitizerPublicationMetadataReason::ProvenanceConflict:
    return "provenance_conflict";
  case SanitizerPublicationMetadataReason::SELinuxFinalPathLabelRequired:
    return "selinux_final_path_label_required";
  case SanitizerPublicationMetadataReason::SELinuxSourceLabelForbidden:
    return "selinux_source_label_forbidden";
  case SanitizerPublicationMetadataReason::InvalidFinalPathSELinuxLabel:
    return "invalid_final_path_selinux_label";
  case SanitizerPublicationMetadataReason::SameSourceGuardedUnsupported:
    return "same_source_guarded_unsupported";
  case SanitizerPublicationMetadataReason::ExistingDestinationCASUnsupported:
    return "existing_destination_cas_unsupported";
  case SanitizerPublicationMetadataReason::InvalidAbsentDestination:
    return "invalid_absent_destination";
  case SanitizerPublicationMetadataReason::SnapshotConflict:
    return "snapshot_conflict";
  case SanitizerPublicationMetadataReason::InvalidMetadataOrigin:
    return "invalid_metadata_origin";
  case SanitizerPublicationMetadataReason::IncompleteMetadataEnumeration:
    return "incomplete_metadata_enumeration";
  case SanitizerPublicationMetadataReason::MissingACLMetadata:
    return "missing_acl_metadata";
  case SanitizerPublicationMetadataReason::CandidateMutationAttestationRequired:
    return "candidate_mutation_attestation_required";
  case SanitizerPublicationMetadataReason::SameSourceMutationUnsupported:
    return "same_source_mutation_unsupported";
  case SanitizerPublicationMetadataReason::InvalidCandidateMutationDisposition:
    return "invalid_candidate_mutation_disposition";
  case SanitizerPublicationMetadataReason::SELinuxPolicyUnavailable:
    return "selinux_policy_unavailable";
  case SanitizerPublicationMetadataReason::InvalidSELinuxPolicyState:
    return "invalid_selinux_policy_state";
  case SanitizerPublicationMetadataReason::SELinuxPolicyConflict:
    return "selinux_policy_conflict";
  case SanitizerPublicationMetadataReason::UnreadablePOSIXMode:
    return "unreadable_posix_mode";
  case SanitizerPublicationMetadataReason::MissingPOSIXModeMetadata:
    return "missing_posix_mode_metadata";
  case SanitizerPublicationMetadataReason::InvalidPOSIXModeEncoding:
    return "invalid_posix_mode_encoding";
  }
  return "unknown";
}

SanitizerPublicationMetadataPlanV1 planSanitizerPublicationMetadata(
    const SanitizerPublicationMetadataRequestV1 &Request,
    const SanitizerPublicationMetadataLimits &Limits) {
  if (Request.Version != kSanitizerPublicationMetadataSchemaVersion)
    return failure(SanitizerPublicationMetadataReason::UnsupportedVersion);
  if (Limits.MaxRecords > kSanitizerPublicationMetadataMaxRecords ||
      Limits.MaxActions > kSanitizerPublicationMetadataMaxActions ||
      Limits.MaxEncodedBytes > kSanitizerPublicationMetadataMaxEncodedBytes)
    return failure(SanitizerPublicationMetadataReason::InvalidLimits);
  switch (Request.Platform) {
  case PublicationPlatform::Linux:
  case PublicationPlatform::MacOS:
  case PublicationPlatform::Windows:
    break;
  default:
    return failure(SanitizerPublicationMetadataReason::UnsupportedPlatform);
  }
  switch (Request.CandidateMutation) {
  case CandidateMutationDisposition::Unauthenticated:
  case CandidateMutationDisposition::ByteIdentical:
  case CandidateMutationDisposition::ByteMutated:
    break;
  default:
    return failure(SanitizerPublicationMetadataReason::
                       InvalidCandidateMutationDisposition);
  }
  switch (Request.SELinuxPolicy) {
  case SELinuxPolicyState::DisabledAttested:
  case SELinuxPolicyState::FinalPathDerived:
  case SELinuxPolicyState::Unavailable:
    break;
  default:
    return failure(
        SanitizerPublicationMetadataReason::InvalidSELinuxPolicyState);
  }
  if (Request.Source.Node == PublicationNodeKind::Symlink ||
      Request.Destination.Node == PublicationNodeKind::Symlink)
    return failure(SanitizerPublicationMetadataReason::SymlinkUnsupported);
  if (Request.Source.Node == PublicationNodeKind::ReparsePoint ||
      Request.Destination.Node == PublicationNodeKind::ReparsePoint)
    return failure(SanitizerPublicationMetadataReason::ReparsePointUnsupported);
  if (Request.Source.Node != PublicationNodeKind::RegularFile)
    return failure(SanitizerPublicationMetadataReason::InvalidSourceNode);
  if (Request.Destination.Node != PublicationNodeKind::Absent &&
      Request.Destination.Node != PublicationNodeKind::RegularFile)
    return failure(SanitizerPublicationMetadataReason::InvalidDestinationNode);
  if (Request.Source.LinkCount > 1 ||
      (Request.Destination.Node == PublicationNodeKind::RegularFile &&
       Request.Destination.LinkCount > 1))
    return failure(SanitizerPublicationMetadataReason::HardlinkUnsupported);
  if (Request.Source.LinkCount != 1)
    return failure(SanitizerPublicationMetadataReason::InvalidSourceNode);
  if (Request.Destination.Node == PublicationNodeKind::RegularFile &&
      Request.Destination.LinkCount != 1)
    return failure(SanitizerPublicationMetadataReason::InvalidDestinationNode);
  if (!Request.Source.OwnerReadable)
    return failure(SanitizerPublicationMetadataReason::UnreadableOwner);
  if (!Request.Source.Owner)
    return failure(SanitizerPublicationMetadataReason::MissingOwnerMetadata);
  if (!Request.Source.ACLReadable)
    return failure(SanitizerPublicationMetadataReason::UnreadableACL);
  if (Request.Platform == PublicationPlatform::Windows && !Request.Source.ACL)
    return failure(SanitizerPublicationMetadataReason::MissingACLMetadata);
  if (const SanitizerPublicationMetadataReason Reason =
          validatePOSIXModeSnapshot(Request.Source, Request.Platform);
      Reason != SanitizerPublicationMetadataReason::None)
    return failure(Reason);
  if (!Request.Source.AttributeEnumerationComplete)
    return failure(
        SanitizerPublicationMetadataReason::IncompleteMetadataEnumeration);
  if (Request.Destination.Node == PublicationNodeKind::RegularFile) {
    if (!Request.Destination.OwnerReadable)
      return failure(SanitizerPublicationMetadataReason::UnreadableOwner);
    if (!Request.Destination.Owner)
      return failure(SanitizerPublicationMetadataReason::MissingOwnerMetadata);
    if (!Request.Destination.ACLReadable)
      return failure(SanitizerPublicationMetadataReason::UnreadableACL);
    if (Request.Platform == PublicationPlatform::Windows &&
        !Request.Destination.ACL)
      return failure(SanitizerPublicationMetadataReason::MissingACLMetadata);
    if (const SanitizerPublicationMetadataReason Reason =
            validatePOSIXModeSnapshot(Request.Destination, Request.Platform);
        Reason != SanitizerPublicationMetadataReason::None)
      return failure(Reason);
    if (Request.SameSourceAndDestination &&
        !Request.Destination.AttributeEnumerationComplete)
      return failure(
          SanitizerPublicationMetadataReason::IncompleteMetadataEnumeration);
  }

  uint64_t SourceRecordCount =
      1 + static_cast<uint64_t>(Request.Source.ACL.has_value()) +
      static_cast<uint64_t>(Request.Source.SetUID) +
      static_cast<uint64_t>(Request.Source.SetGID) +
      static_cast<uint64_t>(Request.Platform != PublicationPlatform::Windows);
  if constexpr (sizeof(size_t) > sizeof(uint64_t))
    if (Request.Source.Attributes.size() > std::numeric_limits<uint64_t>::max())
      return failure(SanitizerPublicationMetadataReason::ArithmeticOverflow);
  const uint64_t AttributeCount = Request.Source.Attributes.size();
  if (AttributeCount > std::numeric_limits<uint64_t>::max() - SourceRecordCount)
    return failure(SanitizerPublicationMetadataReason::ArithmeticOverflow);
  SourceRecordCount += AttributeCount;
  if (SourceRecordCount > Limits.MaxRecords)
    return failure(
        SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded);

  bool NoChange = false;
  if (Request.SameSourceAndDestination) {
    if (Request.Destination.Node != PublicationNodeKind::RegularFile ||
        Request.Destination.LinkCount != 1)
      return failure(
          SanitizerPublicationMetadataReason::InvalidDestinationNode);
    if (Request.GuardedSiteCount != 0)
      return failure(
          SanitizerPublicationMetadataReason::SameSourceGuardedUnsupported);
    if (Request.CandidateMutation ==
        CandidateMutationDisposition::Unauthenticated)
      return failure(SanitizerPublicationMetadataReason::
                         CandidateMutationAttestationRequired);
    if (Request.CandidateMutation == CandidateMutationDisposition::ByteMutated)
      return failure(
          SanitizerPublicationMetadataReason::SameSourceMutationUnsupported);
    if (Request.Source != Request.Destination) {
      const SecurityMetadataObservation *SourceProvenance = findObservation(
          Request.Source, SecurityMetadataKind::MacOSProvenance);
      const SecurityMetadataObservation *DestinationProvenance =
          findObservation(Request.Destination,
                          SecurityMetadataKind::MacOSProvenance);
      if ((SourceProvenance == nullptr) != (DestinationProvenance == nullptr) ||
          (SourceProvenance && *SourceProvenance != *DestinationProvenance))
        return failure(SanitizerPublicationMetadataReason::ProvenanceConflict);
      return failure(SanitizerPublicationMetadataReason::SnapshotConflict);
    }
    NoChange = true;
  } else if (Request.Destination.Node != PublicationNodeKind::Absent) {
    return failure(
        SanitizerPublicationMetadataReason::ExistingDestinationCASUnsupported);
  }
  if (!NoChange) {
    if (Request.Destination.LinkCount != 0)
      return failure(
          SanitizerPublicationMetadataReason::InvalidAbsentDestination);
    if (Request.Destination.OwnerReadable || Request.Destination.Owner ||
        Request.Destination.ACLReadable || Request.Destination.ACL ||
        Request.Destination.POSIXModeReadable ||
        Request.Destination.POSIXMode || Request.Destination.SetUID ||
        Request.Destination.SetGID ||
        Request.Destination.AttributeEnumerationComplete ||
        !Request.Destination.Attributes.empty())
      return failure(
          SanitizerPublicationMetadataReason::InvalidAbsentDestination);
  }

  for (size_t Index = 0; Index < Request.Source.Attributes.size(); ++Index) {
    const SecurityMetadataObservation &Observation =
        Request.Source.Attributes[Index];
    if (!Observation.Readable)
      return failure(SanitizerPublicationMetadataReason::UnreadableMetadata);
    switch (Observation.Kind) {
    case SecurityMetadataKind::MacOSQuarantine:
    case SecurityMetadataKind::MacOSProvenance:
    case SecurityMetadataKind::LinuxCapability:
    case SecurityMetadataKind::LinuxSELinux:
    case SecurityMetadataKind::LinuxIMA:
    case SecurityMetadataKind::LinuxEVM:
    case SecurityMetadataKind::WindowsMOTW:
    case SecurityMetadataKind::UnknownExtendedAttribute:
    case SecurityMetadataKind::UnknownAlternateDataStream:
      break;
    default:
      return failure(
          SanitizerPublicationMetadataReason::UnsupportedMetadataKind);
    }
    if (Observation.Origin != SecurityMetadataOrigin::SourceSnapshot) {
      if (Observation.Kind == SecurityMetadataKind::MacOSProvenance)
        return failure(SanitizerPublicationMetadataReason::ProvenanceConflict);
      return failure(SanitizerPublicationMetadataReason::InvalidMetadataOrigin);
    }
    if (Index == 0)
      continue;
    const SecurityMetadataObservation &Previous =
        Request.Source.Attributes[Index - 1];
    const uint8_t PreviousKind = static_cast<uint8_t>(Previous.Kind);
    const uint8_t CurrentKind = static_cast<uint8_t>(Observation.Kind);
    if (PreviousKind > CurrentKind)
      return failure(
          SanitizerPublicationMetadataReason::NonCanonicalMetadataOrder);
    if (PreviousKind != CurrentKind)
      continue;
    if (Observation.Kind == SecurityMetadataKind::MacOSProvenance &&
        (Previous.Value != Observation.Value ||
         Previous.Origin != Observation.Origin))
      return failure(SanitizerPublicationMetadataReason::ProvenanceConflict);
    return failure(SanitizerPublicationMetadataReason::DuplicateMetadata);
  }

  if (Request.Platform == PublicationPlatform::Windows &&
      (Request.Source.SetUID || Request.Source.SetGID))
    return failure(
        SanitizerPublicationMetadataReason::MetadataPlatformMismatch);

  std::vector<SourceMetadataManifestEntryV1> SourceMetadata;
  SourceMetadata.reserve(static_cast<size_t>(SourceRecordCount));
  addSourceMetadata(SourceMetadata, SecurityMetadataSubject::Owner,
                    Request.Source.Owner);
  if (Request.Source.ACL)
    addSourceMetadata(SourceMetadata, SecurityMetadataSubject::ACL,
                      Request.Source.ACL);
  if (Request.Source.SetUID)
    addSourceMetadata(SourceMetadata, SecurityMetadataSubject::SetUIDBit);
  if (Request.Source.SetGID)
    addSourceMetadata(SourceMetadata, SecurityMetadataSubject::SetGIDBit);

  for (const SecurityMetadataObservation &Observation :
       Request.Source.Attributes) {
    switch (Observation.Kind) {
    case SecurityMetadataKind::MacOSQuarantine:
      if (Request.Platform != PublicationPlatform::MacOS)
        return failure(
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
      addSourceMetadata(SourceMetadata,
                        SecurityMetadataSubject::MacOSQuarantine,
                        Observation.Value);
      break;
    case SecurityMetadataKind::MacOSProvenance:
      if (Request.Platform != PublicationPlatform::MacOS)
        return failure(
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
      addSourceMetadata(SourceMetadata,
                        SecurityMetadataSubject::MacOSProvenance,
                        Observation.Value);
      break;
    case SecurityMetadataKind::LinuxCapability:
      if (Request.Platform != PublicationPlatform::Linux)
        return failure(
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
      addSourceMetadata(SourceMetadata,
                        SecurityMetadataSubject::LinuxCapability,
                        Observation.Value);
      break;
    case SecurityMetadataKind::LinuxSELinux:
      if (Request.Platform != PublicationPlatform::Linux)
        return failure(
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
      addSourceMetadata(SourceMetadata, SecurityMetadataSubject::LinuxSELinux,
                        Observation.Value);
      break;
    case SecurityMetadataKind::WindowsMOTW:
      if (Request.Platform != PublicationPlatform::Windows)
        return failure(
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
      addSourceMetadata(SourceMetadata, SecurityMetadataSubject::WindowsMOTW,
                        Observation.Value);
      break;
    case SecurityMetadataKind::LinuxIMA:
    case SecurityMetadataKind::LinuxEVM:
      return failure(
          SanitizerPublicationMetadataReason::IntegrityMetadataUnsupported);
    case SecurityMetadataKind::UnknownExtendedAttribute:
      return failure(
          SanitizerPublicationMetadataReason::UnknownExtendedAttribute);
    case SecurityMetadataKind::UnknownAlternateDataStream:
      return failure(
          SanitizerPublicationMetadataReason::UnknownAlternateDataStream);
    default:
      return failure(
          SanitizerPublicationMetadataReason::UnsupportedMetadataKind);
    }
  }
  if (Request.Platform != PublicationPlatform::Windows)
    addSourceMetadata(SourceMetadata, SecurityMetadataSubject::POSIXMode,
                      Request.Source.POSIXMode);
  std::sort(SourceMetadata.begin(), SourceMetadata.end(),
            [](const SourceMetadataManifestEntryV1 &LHS,
               const SourceMetadataManifestEntryV1 &RHS) {
              return static_cast<uint8_t>(LHS.Subject) <
                     static_cast<uint8_t>(RHS.Subject);
            });

  uint64_t ObservedRecordCount = SourceMetadata.size();
  uint64_t ObservedEncodedBytes = 0;
  SanitizerPublicationMetadataReason ChargeReason =
      SanitizerPublicationMetadataReason::None;
  for (const SourceMetadataManifestEntryV1 &Entry : SourceMetadata) {
    if (!Entry.ExpectedDigest)
      continue;
    ChargeReason = chargeEncodedBytes(ObservedEncodedBytes,
                                      Entry.ExpectedDigest->EncodedBytes,
                                      Limits.MaxEncodedBytes);
    if (ChargeReason != SanitizerPublicationMetadataReason::None)
      return failure(ChargeReason);
  }

  SanitizerPublicationMetadataPlanV1 Plan;
  Plan.Platform = Request.Platform;
  Plan.GuardedSiteCount = Request.GuardedSiteCount;
  Plan.CandidateMutation = Request.CandidateMutation;
  if (Request.Platform == PublicationPlatform::Linux)
    Plan.SELinuxPolicy = Request.SELinuxPolicy;
  Plan.NamespaceDisposition =
      NoChange ? PublicationNamespaceDisposition::NoChange
               : PublicationNamespaceDisposition::CreateExclusive;
  Plan.ObservedRecordCount = static_cast<uint32_t>(ObservedRecordCount);
  Plan.ObservedEncodedBytes = ObservedEncodedBytes;
  Plan.SourceMetadata = std::move(SourceMetadata);
  if (NoChange) {
    // The caller authenticated byte identity, file identity, and an equal,
    // complete metadata snapshot.  Do not interpret transform-specific
    // metadata or emit actions: this disposition preserves the existing inode.
    Plan.Complete = true;
    return Plan;
  }

  if (Request.Platform == PublicationPlatform::Linux) {
    if (Request.SELinuxPolicy == SELinuxPolicyState::Unavailable)
      return failure(
          SanitizerPublicationMetadataReason::SELinuxPolicyUnavailable);
    if (Request.SELinuxPolicy == SELinuxPolicyState::DisabledAttested &&
        Request.FinalPathSELinuxLabel)
      return failure(SanitizerPublicationMetadataReason::SELinuxPolicyConflict);
    if (Request.SELinuxPolicy == SELinuxPolicyState::FinalPathDerived &&
        !Request.FinalPathSELinuxLabel)
      return failure(
          SanitizerPublicationMetadataReason::SELinuxFinalPathLabelRequired);
  } else if (Request.SELinuxPolicy == SELinuxPolicyState::FinalPathDerived) {
    return failure(
        SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
  }
  if (Request.FinalPathSELinuxLabel) {
    const SecurityMetadataObservation &Label = *Request.FinalPathSELinuxLabel;
    if (Request.Platform != PublicationPlatform::Linux)
      return failure(
          SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
    if (Request.SELinuxPolicy != SELinuxPolicyState::FinalPathDerived)
      return failure(SanitizerPublicationMetadataReason::SELinuxPolicyConflict);
    if (Label.Kind != SecurityMetadataKind::LinuxSELinux ||
        Label.Value.EncodedBytes == 0)
      return failure(
          SanitizerPublicationMetadataReason::InvalidFinalPathSELinuxLabel);
    if (!Label.Readable)
      return failure(SanitizerPublicationMetadataReason::UnreadableMetadata);
    if (Label.Origin == SecurityMetadataOrigin::SourceSnapshot)
      return failure(
          SanitizerPublicationMetadataReason::SELinuxSourceLabelForbidden);
    if (Label.Origin != SecurityMetadataOrigin::FinalPathDerived)
      return failure(SanitizerPublicationMetadataReason::InvalidMetadataOrigin);

    if (ObservedRecordCount == std::numeric_limits<uint64_t>::max())
      return failure(SanitizerPublicationMetadataReason::ArithmeticOverflow);
    ++ObservedRecordCount;
    if (ObservedRecordCount > Limits.MaxRecords)
      return failure(
          SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded);
    ChargeReason = chargeEncodedBytes(
        ObservedEncodedBytes, Label.Value.EncodedBytes, Limits.MaxEncodedBytes);
    if (ChargeReason != SanitizerPublicationMetadataReason::None)
      return failure(ChargeReason);
    Plan.ObservedRecordCount = static_cast<uint32_t>(ObservedRecordCount);
    Plan.ObservedEncodedBytes = ObservedEncodedBytes;
  }

  const SourceMetadataManifestEntryV1 *SourceOwner =
      findSourceMetadata(Plan.SourceMetadata, SecurityMetadataSubject::Owner);
  addAction(Plan, SecurityMetadataSubject::Owner,
            SecurityMetadataDisposition::PreserveFromSource,
            SourceOwner->ExpectedDigest);
  const SourceMetadataManifestEntryV1 *SourceACL =
      findSourceMetadata(Plan.SourceMetadata, SecurityMetadataSubject::ACL);
  if (SourceACL) {
    addAction(Plan, SecurityMetadataSubject::ACL,
              SecurityMetadataDisposition::PreserveFromSource,
              SourceACL->ExpectedDigest);
  } else {
    addAction(Plan, SecurityMetadataSubject::ACL,
              SecurityMetadataDisposition::Clear);
  }
  for (const SecurityMetadataSubject Subject : {
           SecurityMetadataSubject::MacOSQuarantine,
           SecurityMetadataSubject::MacOSProvenance,
           SecurityMetadataSubject::WindowsMOTW,
       })
    if (const SourceMetadataManifestEntryV1 *Entry =
            findSourceMetadata(Plan.SourceMetadata, Subject))
      addAction(Plan, Subject, SecurityMetadataDisposition::PreserveFromSource,
                Entry->ExpectedDigest);
  const SourceMetadataManifestEntryV1 *SourceLinuxCapability =
      findSourceMetadata(Plan.SourceMetadata,
                         SecurityMetadataSubject::LinuxCapability);
  const SourceMetadataManifestEntryV1 *SourceLinuxSELinux = findSourceMetadata(
      Plan.SourceMetadata, SecurityMetadataSubject::LinuxSELinux);
  if (Request.SELinuxPolicy == SELinuxPolicyState::FinalPathDerived) {
    const SecurityMetadataObservation &Label = *Request.FinalPathSELinuxLabel;
    addAction(Plan, SecurityMetadataSubject::LinuxSELinux,
              SecurityMetadataDisposition::ApplyFinalPathDerived, Label.Value);
  } else if (Request.Platform == PublicationPlatform::Linux) {
    addAction(Plan, SecurityMetadataSubject::LinuxSELinux,
              SecurityMetadataDisposition::ClearAndVerifyAbsent,
              SourceLinuxSELinux ? SourceLinuxSELinux->ExpectedDigest
                                 : std::nullopt);
  }
  if (Request.Platform == PublicationPlatform::Linux)
    addAction(Plan, SecurityMetadataSubject::LinuxCapability,
              SecurityMetadataDisposition::ClearAndVerifyAbsent,
              SourceLinuxCapability ? SourceLinuxCapability->ExpectedDigest
                                    : std::nullopt);
  if (Request.Platform != PublicationPlatform::Windows) {
    addAction(Plan, SecurityMetadataSubject::SetUIDBit,
              SecurityMetadataDisposition::Clear);
    addAction(Plan, SecurityMetadataSubject::SetGIDBit,
              SecurityMetadataDisposition::Clear);
    addAction(Plan, SecurityMetadataSubject::POSIXMode,
              SecurityMetadataDisposition::PreserveFromSource,
              findSourceMetadata(Plan.SourceMetadata,
                                 SecurityMetadataSubject::POSIXMode)
                  ->ExpectedDigest);
  }
  std::sort(
      Plan.Actions.begin(), Plan.Actions.end(),
      [](const SecurityMetadataAction &LHS, const SecurityMetadataAction &RHS) {
        return static_cast<uint8_t>(LHS.Subject) <
               static_cast<uint8_t>(RHS.Subject);
      });
  if (Plan.Actions.size() > Limits.MaxActions)
    return failure(SanitizerPublicationMetadataReason::ActionLimitExceeded);
  Plan.Complete = true;
  return Plan;
}

} // namespace neverd::safety::sanitizer_publication_metadata
