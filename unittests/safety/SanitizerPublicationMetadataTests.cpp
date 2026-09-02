//===- SanitizerPublicationMetadataTests.cpp - Metadata plan tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/SanitizerPublicationMetadata.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace neverd::safety::sanitizer_publication_metadata;

namespace {

OpaqueMetadataDigest digest(uint8_t Byte, uint64_t EncodedBytes = 1) {
  OpaqueMetadataDigest Digest;
  Digest.SHA256.fill(Byte);
  Digest.EncodedBytes = EncodedBytes;
  return Digest;
}

SecurityMetadataObservation attribute(SecurityMetadataKind Kind, uint8_t Byte,
                                      uint64_t EncodedBytes = 1) {
  SecurityMetadataObservation Observation;
  Observation.Kind = Kind;
  Observation.Origin = SecurityMetadataOrigin::SourceSnapshot;
  Observation.Readable = true;
  Observation.Value = digest(Byte, EncodedBytes);
  return Observation;
}

SanitizerPublicationMetadataRequestV1
minimalRequest(PublicationPlatform Platform = PublicationPlatform::Linux) {
  SanitizerPublicationMetadataRequestV1 Request;
  Request.Platform = Platform;
  Request.GuardedSiteCount = 1;
  Request.CandidateMutation = CandidateMutationDisposition::ByteMutated;
  if (Platform == PublicationPlatform::Linux)
    Request.SELinuxPolicy = SELinuxPolicyState::DisabledAttested;
  Request.Source.Node = PublicationNodeKind::RegularFile;
  Request.Source.LinkCount = 1;
  Request.Source.OwnerReadable = true;
  Request.Source.Owner = digest(1, 8);
  Request.Source.ACLReadable = true;
  if (Platform != PublicationPlatform::Windows) {
    Request.Source.POSIXModeReadable = true;
    Request.Source.POSIXMode = digest(10, 4);
  }
  Request.Source.AttributeEnumerationComplete = true;
  if (Platform == PublicationPlatform::Windows)
    Request.Source.ACL = digest(6, 32);
  Request.Destination.Node = PublicationNodeKind::Absent;
  Request.Destination.LinkCount = 0;
  return Request;
}

const SecurityMetadataAction *
findAction(const SanitizerPublicationMetadataPlanV1 &Plan,
           SecurityMetadataSubject Subject) {
  for (const SecurityMetadataAction &Action : Plan.Actions)
    if (Action.Subject == Subject)
      return &Action;
  return nullptr;
}

const SourceMetadataManifestEntryV1 *
findSourceMetadata(const SanitizerPublicationMetadataPlanV1 &Plan,
                   SecurityMetadataSubject Subject) {
  for (const SourceMetadataManifestEntryV1 &Entry : Plan.SourceMetadata)
    if (Entry.Subject == Subject)
      return &Entry;
  return nullptr;
}

size_t countActions(const SanitizerPublicationMetadataPlanV1 &Plan,
                    SecurityMetadataSubject Subject) {
  size_t Count = 0;
  for (const SecurityMetadataAction &Action : Plan.Actions)
    if (Action.Subject == Subject)
      ++Count;
  return Count;
}

void expectCanonicalUniqueActions(
    const SanitizerPublicationMetadataPlanV1 &Plan) {
  for (size_t Index = 1; Index < Plan.Actions.size(); ++Index)
    EXPECT_LT(static_cast<uint8_t>(Plan.Actions[Index - 1].Subject),
              static_cast<uint8_t>(Plan.Actions[Index].Subject));
}

TEST(SanitizerPublicationMetadata,
     PlansExclusiveCreationWithoutClaimingPublicationGuarantees) {
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(minimalRequest());

  ASSERT_TRUE(Plan.Complete);
  EXPECT_EQ(Plan.Version, kSanitizerPublicationMetadataSchemaVersion);
  EXPECT_EQ(kSanitizerPublicationMetadataAdapter,
            std::string_view("security-metadata-plan-v1"));
  EXPECT_EQ(Plan.Reason, SanitizerPublicationMetadataReason::None);
  ASSERT_TRUE(Plan.Platform.has_value());
  EXPECT_EQ(*Plan.Platform, PublicationPlatform::Linux);
  EXPECT_EQ(Plan.GuardedSiteCount, 1u);
  EXPECT_EQ(Plan.NamespaceDisposition,
            PublicationNamespaceDisposition::CreateExclusive);
  EXPECT_FALSE(Plan.Guarantees.NamespaceAtomic);
  EXPECT_FALSE(Plan.Guarantees.CompareAndSwap);
  EXPECT_FALSE(Plan.Guarantees.CrashDurable);
  expectCanonicalUniqueActions(Plan);

  const SecurityMetadataAction *Owner =
      findAction(Plan, SecurityMetadataSubject::Owner);
  ASSERT_NE(Owner, nullptr);
  EXPECT_EQ(Owner->Disposition,
            SecurityMetadataDisposition::PreserveFromSource);
  ASSERT_TRUE(Owner->ExpectedDigest.has_value());
  EXPECT_EQ(*Owner->ExpectedDigest, digest(1, 8));

  const SecurityMetadataAction *SetUID =
      findAction(Plan, SecurityMetadataSubject::SetUIDBit);
  const SecurityMetadataAction *SetGID =
      findAction(Plan, SecurityMetadataSubject::SetGIDBit);
  ASSERT_NE(SetUID, nullptr);
  ASSERT_NE(SetGID, nullptr);
  EXPECT_EQ(SetUID->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_EQ(SetGID->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_FALSE(SetUID->ExpectedDigest.has_value());
  EXPECT_FALSE(SetGID->ExpectedDigest.has_value());

  const SecurityMetadataAction *Capability =
      findAction(Plan, SecurityMetadataSubject::LinuxCapability);
  ASSERT_NE(Capability, nullptr);
  EXPECT_EQ(countActions(Plan, SecurityMetadataSubject::LinuxCapability), 1u);
  EXPECT_EQ(Capability->Disposition,
            SecurityMetadataDisposition::ClearAndVerifyAbsent);
  EXPECT_FALSE(Capability->ExpectedDigest.has_value());

  const SecurityMetadataAction *SELinux =
      findAction(Plan, SecurityMetadataSubject::LinuxSELinux);
  ASSERT_NE(SELinux, nullptr);
  EXPECT_EQ(SELinux->Disposition,
            SecurityMetadataDisposition::ClearAndVerifyAbsent);
  EXPECT_FALSE(SELinux->ExpectedDigest.has_value());
}

TEST(SanitizerPublicationMetadata,
     PreservesPOSIXModeAsExplicitPlannedMaterial) {
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(minimalRequest());

  const SecurityMetadataAction *Mode =
      findAction(Plan, static_cast<SecurityMetadataSubject>(10));
  ASSERT_NE(Mode, nullptr);
  EXPECT_EQ(Mode->Disposition, SecurityMetadataDisposition::PreserveFromSource);
  ASSERT_TRUE(Mode->ExpectedDigest.has_value());
  EXPECT_EQ(*Mode->ExpectedDigest, digest(10, 4));
}

TEST(SanitizerPublicationMetadata,
     EmitsCanonicalDigestOnlySourceMetadataPresentSet) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Source.ACL = digest(2, 24);
  Request.Source.SetUID = true;
  Request.Source.SetGID = true;
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::LinuxCapability, 3, 16),
      attribute(SecurityMetadataKind::LinuxSELinux, 4, 12),
  };

  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  ASSERT_EQ(Plan.SourceMetadata.size(), 7u);
  for (size_t Index = 1; Index < Plan.SourceMetadata.size(); ++Index)
    EXPECT_LT(static_cast<uint8_t>(Plan.SourceMetadata[Index - 1].Subject),
              static_cast<uint8_t>(Plan.SourceMetadata[Index].Subject));

  const SourceMetadataManifestEntryV1 *Owner =
      findSourceMetadata(Plan, SecurityMetadataSubject::Owner);
  const SourceMetadataManifestEntryV1 *ACL =
      findSourceMetadata(Plan, SecurityMetadataSubject::ACL);
  const SourceMetadataManifestEntryV1 *SetUID =
      findSourceMetadata(Plan, SecurityMetadataSubject::SetUIDBit);
  const SourceMetadataManifestEntryV1 *SetGID =
      findSourceMetadata(Plan, SecurityMetadataSubject::SetGIDBit);
  const SourceMetadataManifestEntryV1 *Capability =
      findSourceMetadata(Plan, SecurityMetadataSubject::LinuxCapability);
  const SourceMetadataManifestEntryV1 *SELinux =
      findSourceMetadata(Plan, SecurityMetadataSubject::LinuxSELinux);
  const SourceMetadataManifestEntryV1 *Mode =
      findSourceMetadata(Plan, SecurityMetadataSubject::POSIXMode);
  ASSERT_NE(Owner, nullptr);
  ASSERT_NE(ACL, nullptr);
  ASSERT_NE(SetUID, nullptr);
  ASSERT_NE(SetGID, nullptr);
  ASSERT_NE(Capability, nullptr);
  ASSERT_NE(SELinux, nullptr);
  ASSERT_NE(Mode, nullptr);
  EXPECT_EQ(Owner->ExpectedDigest, Request.Source.Owner);
  EXPECT_EQ(ACL->ExpectedDigest, Request.Source.ACL);
  EXPECT_FALSE(SetUID->ExpectedDigest.has_value());
  EXPECT_FALSE(SetGID->ExpectedDigest.has_value());
  EXPECT_EQ(Capability->ExpectedDigest, Request.Source.Attributes[0].Value);
  EXPECT_EQ(SELinux->ExpectedDigest, Request.Source.Attributes[1].Value);
  EXPECT_EQ(Mode->ExpectedDigest, Request.Source.POSIXMode);
  EXPECT_EQ(Plan.ObservedRecordCount, 7u);
  EXPECT_EQ(Plan.ObservedEncodedBytes, 64u);
}

TEST(SanitizerPublicationMetadata,
     KeepsTraditionalAndStickyModeSeparateFromACLAndSetIDPolicy) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Source.ACL.reset();
  Request.Source.POSIXMode = digest(0x75, 4);
  Request.Source.SetUID = true;
  Request.Source.SetGID = true;

  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  const SecurityMetadataAction *ACL =
      findAction(Plan, SecurityMetadataSubject::ACL);
  const SecurityMetadataAction *Mode =
      findAction(Plan, SecurityMetadataSubject::POSIXMode);
  const SecurityMetadataAction *SetUID =
      findAction(Plan, SecurityMetadataSubject::SetUIDBit);
  const SecurityMetadataAction *SetGID =
      findAction(Plan, SecurityMetadataSubject::SetGIDBit);
  ASSERT_NE(ACL, nullptr);
  ASSERT_NE(Mode, nullptr);
  ASSERT_NE(SetUID, nullptr);
  ASSERT_NE(SetGID, nullptr);
  EXPECT_EQ(ACL->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_EQ(Mode->Disposition, SecurityMetadataDisposition::PreserveFromSource);
  EXPECT_EQ(Mode->ExpectedDigest, Request.Source.POSIXMode);
  EXPECT_EQ(SetUID->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_EQ(SetGID->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_FALSE(SetUID->ExpectedDigest.has_value());
  EXPECT_FALSE(SetGID->ExpectedDigest.has_value());

  Request = minimalRequest(PublicationPlatform::MacOS);
  Request.Source.POSIXMode = digest(0x10, 4);
  Plan = planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  Mode = findAction(Plan, SecurityMetadataSubject::POSIXMode);
  ASSERT_NE(Mode, nullptr);
  EXPECT_EQ(Mode->ExpectedDigest, Request.Source.POSIXMode);
}

TEST(SanitizerPublicationMetadata,
     RequiresCanonicalFourBytePOSIXModeOnlyOnPOSIXPlatforms) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Source.POSIXModeReadable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadablePOSIXMode);

  Request = minimalRequest(PublicationPlatform::MacOS);
  Request.Source.POSIXMode.reset();
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MissingPOSIXModeMetadata);

  for (const uint64_t EncodedBytes : {uint64_t{0}, uint64_t{3}, uint64_t{5}}) {
    Request = minimalRequest();
    Request.Source.POSIXMode->EncodedBytes = EncodedBytes;
    EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
              SanitizerPublicationMetadataReason::InvalidPOSIXModeEncoding);
  }

  Request = minimalRequest(PublicationPlatform::Windows);
  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  EXPECT_EQ(findAction(Plan, SecurityMetadataSubject::POSIXMode), nullptr);

  Request.Source.POSIXModeReadable = true;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
  Request.Source.POSIXModeReadable = false;
  Request.Source.POSIXMode = digest(10, 4);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
}

TEST(SanitizerPublicationMetadata,
     IncludesPOSIXModeInNoChangeAndAbsentDestinationSnapshots) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.GuardedSiteCount = 0;
  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.SameSourceAndDestination = true;
  Request.Destination = Request.Source;
  Request.Destination.POSIXMode = digest(11, 4);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::SnapshotConflict);

  Request = minimalRequest();
  Request.Destination.POSIXModeReadable = true;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidAbsentDestination);
  Request.Destination.POSIXModeReadable = false;
  Request.Destination.POSIXMode = digest(11, 4);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidAbsentDestination);
}

TEST(SanitizerPublicationMetadata,
     ChargesPOSIXModeToRecordAndEncodedByteBudgets) {
  SanitizerPublicationMetadataLimits Limits;
  Limits.MaxRecords = 1;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded);

  Limits = {};
  Limits.MaxEncodedBytes = 11;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::MetadataByteBudgetExceeded);

  Limits.MaxEncodedBytes = 12;
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(minimalRequest(), Limits);
  ASSERT_TRUE(Plan.ready());
  EXPECT_EQ(Plan.ObservedRecordCount, 2u);
  EXPECT_EQ(Plan.ObservedEncodedBytes, 12u);
}

TEST(SanitizerPublicationMetadata, RejectsLinkIndirectionAndHardlinks) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Source.Node = PublicationNodeKind::Symlink;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::SymlinkUnsupported);

  Request = minimalRequest(PublicationPlatform::Windows);
  Request.Destination.Node = PublicationNodeKind::ReparsePoint;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::ReparsePointUnsupported);

  Request = minimalRequest();
  Request.Source.LinkCount = 2;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::HardlinkUnsupported);

  Request = minimalRequest();
  Request.Destination.Node = PublicationNodeKind::RegularFile;
  Request.Destination.LinkCount = 2;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::HardlinkUnsupported);
}

TEST(SanitizerPublicationMetadata, UsesOnlyTheFailClosedNamespaceModes) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.GuardedSiteCount = 0;
  Request.SameSourceAndDestination = true;
  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.Destination = Request.Source;
  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  EXPECT_EQ(Plan.CandidateMutation,
            CandidateMutationDisposition::ByteIdentical);
  EXPECT_EQ(Plan.NamespaceDisposition,
            PublicationNamespaceDisposition::NoChange);
  EXPECT_TRUE(Plan.Actions.empty());

  SanitizerPublicationMetadataLimits NoActionBudget;
  NoActionBudget.MaxActions = 0;
  Plan = planSanitizerPublicationMetadata(Request, NoActionBudget);
  EXPECT_TRUE(Plan.ready());
  EXPECT_TRUE(Plan.Actions.empty());

  Request.Destination.Owner = digest(9, 8);
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_EQ(Plan.Reason, SanitizerPublicationMetadataReason::SnapshotConflict);

  Request.Destination = Request.Source;
  Request.CandidateMutation = CandidateMutationDisposition::Unauthenticated;
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_EQ(
      Plan.Reason,
      SanitizerPublicationMetadataReason::CandidateMutationAttestationRequired);

  Request.CandidateMutation = CandidateMutationDisposition::ByteMutated;
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_EQ(Plan.Reason,
            SanitizerPublicationMetadataReason::SameSourceMutationUnsupported);

  Request.CandidateMutation = static_cast<CandidateMutationDisposition>(0xff);
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_EQ(
      Plan.Reason,
      SanitizerPublicationMetadataReason::InvalidCandidateMutationDisposition);

  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.GuardedSiteCount = 1;
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_FALSE(Plan.ready());
  EXPECT_EQ(Plan.Reason,
            SanitizerPublicationMetadataReason::SameSourceGuardedUnsupported);

  Request = minimalRequest();
  Request.Destination = Request.Source;
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_EQ(
      Plan.Reason,
      SanitizerPublicationMetadataReason::ExistingDestinationCASUnsupported);

  Request = minimalRequest();
  Request.Destination.LinkCount = 1;
  Plan = planSanitizerPublicationMetadata(Request);
  EXPECT_EQ(Plan.Reason,
            SanitizerPublicationMetadataReason::InvalidAbsentDestination);
}

TEST(SanitizerPublicationMetadata,
     NoChangePreservesTheAuthenticatedInodeWithoutMetadataActions) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.GuardedSiteCount = 0;
  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.SELinuxPolicy = SELinuxPolicyState::Unavailable;
  Request.SameSourceAndDestination = true;
  Request.Source.SetUID = true;
  Request.Source.SetGID = true;
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::LinuxCapability, 2),
      attribute(SecurityMetadataKind::LinuxSELinux, 3),
  };
  Request.Destination = Request.Source;

  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);

  ASSERT_TRUE(Plan.ready());
  EXPECT_EQ(Plan.NamespaceDisposition,
            PublicationNamespaceDisposition::NoChange);
  EXPECT_EQ(Plan.CandidateMutation,
            CandidateMutationDisposition::ByteIdentical);
  ASSERT_TRUE(Plan.SELinuxPolicy.has_value());
  EXPECT_EQ(*Plan.SELinuxPolicy, SELinuxPolicyState::Unavailable);
  EXPECT_TRUE(Plan.Actions.empty());
  EXPECT_EQ(Plan.SourceMetadata.size(), 6u);
  EXPECT_EQ(Plan.ObservedRecordCount, 6u);
  EXPECT_EQ(Plan.ObservedEncodedBytes, 14u);
}

TEST(SanitizerPublicationMetadata,
     NoChangeStillRejectsUnrepresentableSourceMetadata) {
  for (const SecurityMetadataKind Kind : {
           SecurityMetadataKind::LinuxIMA,
           SecurityMetadataKind::LinuxEVM,
           SecurityMetadataKind::UnknownExtendedAttribute,
           SecurityMetadataKind::UnknownAlternateDataStream,
       }) {
    SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
    Request.GuardedSiteCount = 0;
    Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
    Request.SELinuxPolicy = SELinuxPolicyState::Unavailable;
    Request.SameSourceAndDestination = true;
    Request.Source.Attributes = {attribute(Kind, 4)};
    Request.Destination = Request.Source;

    EXPECT_FALSE(planSanitizerPublicationMetadata(Request).ready());
  }
}

TEST(SanitizerPublicationMetadata, RequiresReadableOwnerAndACLObservations) {
  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(minimalRequest());
  ASSERT_TRUE(Plan.ready());
  const SecurityMetadataAction *ACL =
      findAction(Plan, SecurityMetadataSubject::ACL);
  ASSERT_NE(ACL, nullptr);
  EXPECT_EQ(ACL->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_FALSE(ACL->ExpectedDigest.has_value());

  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Source.OwnerReadable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadableOwner);

  Request = minimalRequest();
  Request.Source.Owner.reset();
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MissingOwnerMetadata);

  Request = minimalRequest();
  Request.Source.ACLReadable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadableACL);

  Request = minimalRequest();
  Request.Source.AttributeEnumerationComplete = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::IncompleteMetadataEnumeration);

  Request = minimalRequest();
  Request.Source.ACL = digest(2, 24);
  Plan = planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  ACL = findAction(Plan, SecurityMetadataSubject::ACL);
  ASSERT_NE(ACL, nullptr);
  EXPECT_EQ(ACL->Disposition, SecurityMetadataDisposition::PreserveFromSource);
  EXPECT_EQ(ACL->ExpectedDigest, Request.Source.ACL);

  Request = minimalRequest(PublicationPlatform::Windows);
  Request.Source.ACL.reset();
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MissingACLMetadata);
}

TEST(SanitizerPublicationMetadata,
     PreservesProvenanceMarkersAndClearsPrivilegeMetadata) {
  SanitizerPublicationMetadataRequestV1 Mac =
      minimalRequest(PublicationPlatform::MacOS);
  Mac.Source.SetUID = true;
  Mac.Source.SetGID = true;
  Mac.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSQuarantine, 2, 20),
      attribute(SecurityMetadataKind::MacOSProvenance, 3, 32)};
  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Mac);
  ASSERT_TRUE(Plan.ready());
  expectCanonicalUniqueActions(Plan);
  const SecurityMetadataAction *Quarantine =
      findAction(Plan, SecurityMetadataSubject::MacOSQuarantine);
  const SecurityMetadataAction *Provenance =
      findAction(Plan, SecurityMetadataSubject::MacOSProvenance);
  const SecurityMetadataAction *SetUID =
      findAction(Plan, SecurityMetadataSubject::SetUIDBit);
  const SecurityMetadataAction *SetGID =
      findAction(Plan, SecurityMetadataSubject::SetGIDBit);
  ASSERT_NE(Quarantine, nullptr);
  ASSERT_NE(Provenance, nullptr);
  ASSERT_NE(SetUID, nullptr);
  ASSERT_NE(SetGID, nullptr);
  EXPECT_EQ(Quarantine->Disposition,
            SecurityMetadataDisposition::PreserveFromSource);
  EXPECT_EQ(Provenance->Disposition,
            SecurityMetadataDisposition::PreserveFromSource);
  EXPECT_EQ(Quarantine->ExpectedDigest, Mac.Source.Attributes[0].Value);
  EXPECT_EQ(Provenance->ExpectedDigest, Mac.Source.Attributes[1].Value);
  EXPECT_EQ(SetUID->Disposition, SecurityMetadataDisposition::Clear);
  EXPECT_EQ(SetGID->Disposition, SecurityMetadataDisposition::Clear);

  SanitizerPublicationMetadataRequestV1 Linux = minimalRequest();
  Linux.Source.Attributes = {
      attribute(SecurityMetadataKind::LinuxCapability, 4, 16)};
  Plan = planSanitizerPublicationMetadata(Linux);
  ASSERT_TRUE(Plan.ready());
  expectCanonicalUniqueActions(Plan);
  const SecurityMetadataAction *Capability =
      findAction(Plan, SecurityMetadataSubject::LinuxCapability);
  ASSERT_NE(Capability, nullptr);
  EXPECT_EQ(countActions(Plan, SecurityMetadataSubject::LinuxCapability), 1u);
  EXPECT_EQ(Capability->Disposition,
            SecurityMetadataDisposition::ClearAndVerifyAbsent);
  EXPECT_EQ(Capability->ExpectedDigest, Linux.Source.Attributes[0].Value);

  SanitizerPublicationMetadataRequestV1 Windows =
      minimalRequest(PublicationPlatform::Windows);
  Windows.Source.Attributes = {
      attribute(SecurityMetadataKind::WindowsMOTW, 5, 48)};
  Plan = planSanitizerPublicationMetadata(Windows);
  ASSERT_TRUE(Plan.ready());
  expectCanonicalUniqueActions(Plan);
  const SecurityMetadataAction *MOTW =
      findAction(Plan, SecurityMetadataSubject::WindowsMOTW);
  const SecurityMetadataAction *WindowsACL =
      findAction(Plan, SecurityMetadataSubject::ACL);
  ASSERT_NE(MOTW, nullptr);
  ASSERT_NE(WindowsACL, nullptr);
  EXPECT_EQ(MOTW->Disposition, SecurityMetadataDisposition::PreserveFromSource);
  EXPECT_EQ(MOTW->ExpectedDigest, Windows.Source.Attributes[0].Value);
  EXPECT_EQ(WindowsACL->Disposition,
            SecurityMetadataDisposition::PreserveFromSource);
  EXPECT_EQ(WindowsACL->ExpectedDigest, Windows.Source.ACL);
  EXPECT_EQ(findAction(Plan, SecurityMetadataSubject::SetUIDBit), nullptr);
  EXPECT_EQ(findAction(Plan, SecurityMetadataSubject::SetGIDBit), nullptr);
}

TEST(SanitizerPublicationMetadata,
     RejectsUnreadableUnknownIntegrityAndCrossPlatformMetadata) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::UnknownExtendedAttribute, 2)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnknownExtendedAttribute);

  Request = minimalRequest(PublicationPlatform::Windows);
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::UnknownAlternateDataStream, 2)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnknownAlternateDataStream);

  for (const SecurityMetadataKind Kind :
       {SecurityMetadataKind::LinuxIMA, SecurityMetadataKind::LinuxEVM}) {
    Request = minimalRequest();
    Request.Source.Attributes = {attribute(Kind, 3)};
    EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
              SanitizerPublicationMetadataReason::IntegrityMetadataUnsupported);
  }

  Request = minimalRequest(PublicationPlatform::MacOS);
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::LinuxCapability, 4)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);

  Request = minimalRequest(PublicationPlatform::Windows);
  Request.Source.SetUID = true;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);

  Request = minimalRequest();
  Request.Source.Attributes = {attribute(
      static_cast<SecurityMetadataKind>(std::numeric_limits<uint8_t>::max()),
      5)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnsupportedMetadataKind);

  Request = minimalRequest(PublicationPlatform::MacOS);
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSQuarantine, 6)};
  Request.Source.Attributes[0].Readable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadableMetadata);

  Request.Source.Attributes[0].Readable = true;
  Request.Source.Attributes[0].Origin =
      SecurityMetadataOrigin::FinalPathDerived;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidMetadataOrigin);
}

TEST(SanitizerPublicationMetadata,
     RequiresExplicitSELinuxPolicyAttestationAndExactFinalPathLabel) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.SELinuxPolicy = SELinuxPolicyState::FinalPathDerived;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::SELinuxFinalPathLabelRequired);

  Request.FinalPathSELinuxLabel =
      attribute(SecurityMetadataKind::LinuxSELinux, 3, 16);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::SELinuxSourceLabelForbidden);

  Request.FinalPathSELinuxLabel->Origin =
      static_cast<SecurityMetadataOrigin>(0xff);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidMetadataOrigin);

  Request.FinalPathSELinuxLabel->Origin =
      SecurityMetadataOrigin::FinalPathDerived;
  SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  ASSERT_TRUE(Plan.SELinuxPolicy.has_value());
  EXPECT_EQ(*Plan.SELinuxPolicy, SELinuxPolicyState::FinalPathDerived);
  EXPECT_EQ(Plan.ObservedRecordCount, 3u);
  EXPECT_EQ(Plan.ObservedEncodedBytes, 28u);
  const SecurityMetadataAction *SELinux =
      findAction(Plan, SecurityMetadataSubject::LinuxSELinux);
  ASSERT_NE(SELinux, nullptr);
  EXPECT_EQ(SELinux->Disposition,
            SecurityMetadataDisposition::ApplyFinalPathDerived);
  ASSERT_TRUE(SELinux->ExpectedDigest.has_value());
  EXPECT_EQ(*SELinux->ExpectedDigest, Request.FinalPathSELinuxLabel->Value);

  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::LinuxSELinux, 2, 12)};
  Plan = planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  EXPECT_EQ(Plan.ObservedRecordCount, 4u);
  EXPECT_EQ(Plan.ObservedEncodedBytes, 40u);
  SELinux = findAction(Plan, SecurityMetadataSubject::LinuxSELinux);
  ASSERT_NE(SELinux, nullptr);
  ASSERT_TRUE(SELinux->ExpectedDigest.has_value());
  EXPECT_NE(*SELinux->ExpectedDigest, Request.Source.Attributes[0].Value);
  const SourceMetadataManifestEntryV1 *SourceSELinux =
      findSourceMetadata(Plan, SecurityMetadataSubject::LinuxSELinux);
  ASSERT_NE(SourceSELinux, nullptr);
  EXPECT_EQ(SourceSELinux->ExpectedDigest, Request.Source.Attributes[0].Value);
  EXPECT_EQ(SELinux->ExpectedDigest, Request.FinalPathSELinuxLabel->Value);

  Request.FinalPathSELinuxLabel->Kind = SecurityMetadataKind::LinuxCapability;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidFinalPathSELinuxLabel);

  Request.FinalPathSELinuxLabel->Kind = SecurityMetadataKind::LinuxSELinux;
  Request.FinalPathSELinuxLabel->Readable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadableMetadata);

  Request.FinalPathSELinuxLabel->Readable = true;
  Request.FinalPathSELinuxLabel->Value.EncodedBytes = 0;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidFinalPathSELinuxLabel);

  Request = minimalRequest();
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::LinuxSELinux, 5, 12)};
  Plan = planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  SELinux = findAction(Plan, SecurityMetadataSubject::LinuxSELinux);
  ASSERT_NE(SELinux, nullptr);
  EXPECT_EQ(SELinux->Disposition,
            SecurityMetadataDisposition::ClearAndVerifyAbsent);
  EXPECT_EQ(SELinux->ExpectedDigest, Request.Source.Attributes[0].Value);

  Request.FinalPathSELinuxLabel =
      attribute(SecurityMetadataKind::LinuxSELinux, 4);
  Request.FinalPathSELinuxLabel->Origin =
      SecurityMetadataOrigin::FinalPathDerived;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::SELinuxPolicyConflict);

  Request.Source.Attributes.clear();
  Request.FinalPathSELinuxLabel.reset();
  Request.SELinuxPolicy = SELinuxPolicyState::Unavailable;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::SELinuxPolicyUnavailable);

  Request.SELinuxPolicy = static_cast<SELinuxPolicyState>(0xff);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidSELinuxPolicyState);

  Request = minimalRequest(PublicationPlatform::MacOS);
  Request.SELinuxPolicy = SELinuxPolicyState::FinalPathDerived;
  Request.FinalPathSELinuxLabel =
      attribute(SecurityMetadataKind::LinuxSELinux, 4);
  Request.FinalPathSELinuxLabel->Origin =
      SecurityMetadataOrigin::FinalPathDerived;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataPlatformMismatch);
}

TEST(SanitizerPublicationMetadata,
     RequiresCanonicalUniqueMetadataAndRejectsProvenanceConflicts) {
  SanitizerPublicationMetadataRequestV1 Request =
      minimalRequest(PublicationPlatform::MacOS);
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSProvenance, 2),
      attribute(SecurityMetadataKind::MacOSQuarantine, 3)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::NonCanonicalMetadataOrder);

  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSQuarantine, 2),
      attribute(SecurityMetadataKind::MacOSQuarantine, 2)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::DuplicateMetadata);

  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSProvenance, 2),
      attribute(SecurityMetadataKind::MacOSProvenance, 3)};
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::ProvenanceConflict);

  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSProvenance, 2)};
  Request.Source.Attributes[0].Origin =
      SecurityMetadataOrigin::FinalPathDerived;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::ProvenanceConflict);

  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSProvenance, 2),
      attribute(SecurityMetadataKind::MacOSProvenance, 3)};
  Request.GuardedSiteCount = 0;
  Request.SameSourceAndDestination = true;
  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.Destination = Request.Source;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::ProvenanceConflict);

  Request = minimalRequest(PublicationPlatform::MacOS);
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSProvenance, 2)};
  Request.GuardedSiteCount = 0;
  Request.SameSourceAndDestination = true;
  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.Destination = Request.Source;
  Request.Destination.Attributes[0].Value = digest(3);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::ProvenanceConflict);
}

TEST(SanitizerPublicationMetadata, EnforcesVersionedHardAndCallerBudgets) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Version = 2;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnsupportedVersion);

  SanitizerPublicationMetadataLimits Limits;
  Limits.MaxRecords = kSanitizerPublicationMetadataMaxRecords + 1;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::InvalidLimits);

  Limits = {};
  Limits.MaxActions = kSanitizerPublicationMetadataMaxActions + 1;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::InvalidLimits);

  Limits = {};
  Limits.MaxEncodedBytes = kSanitizerPublicationMetadataMaxEncodedBytes + 1;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::InvalidLimits);

  Limits = {};
  Limits.MaxRecords = 0;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded);

  Limits = {};
  Limits.MaxEncodedBytes = 7;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::MetadataByteBudgetExceeded);

  Limits = {};
  Limits.MaxActions = 2;
  EXPECT_EQ(planSanitizerPublicationMetadata(minimalRequest(), Limits).Reason,
            SanitizerPublicationMetadataReason::ActionLimitExceeded);

  Request = minimalRequest();
  Request.Source.ACL = digest(2, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::ArithmeticOverflow);

  Request = minimalRequest();
  // The hard record ceiling is an allocation-free preflight.  It must reject
  // this oversized vector before scanning its deliberately duplicate,
  // unsupported entries.
  Request.Source.Attributes.resize(
      kSanitizerPublicationMetadataMaxRecords,
      attribute(SecurityMetadataKind::UnknownExtendedAttribute, 3));
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded);

  Request.GuardedSiteCount = 0;
  Request.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  Request.SELinuxPolicy = SELinuxPolicyState::Unavailable;
  Request.SameSourceAndDestination = true;
  Request.Destination = Request.Source;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded);
}

TEST(SanitizerPublicationMetadata,
     EmitsCanonicalDigestOnlyReceiptActionsAndCounts) {
  static_assert(
      std::is_same_v<decltype(SecurityMetadataAction{}.ExpectedDigest),
                     std::optional<OpaqueMetadataDigest>>);
  static_assert(std::is_same_v<decltype(SecurityMetadataObservation{}.Value),
                               OpaqueMetadataDigest>);

  SanitizerPublicationMetadataRequestV1 Request =
      minimalRequest(PublicationPlatform::MacOS);
  Request.Source.Attributes = {
      attribute(SecurityMetadataKind::MacOSQuarantine, 2, 20),
      attribute(SecurityMetadataKind::MacOSProvenance, 3, 32)};
  const SanitizerPublicationMetadataPlanV1 Plan =
      planSanitizerPublicationMetadata(Request);
  ASSERT_TRUE(Plan.ready());
  EXPECT_EQ(Plan.ObservedRecordCount, 4u);
  EXPECT_EQ(Plan.ObservedEncodedBytes, 64u);
  expectCanonicalUniqueActions(Plan);
}

TEST(SanitizerPublicationMetadata,
     RejectsInvalidOrUnauthenticatedSourceAndDestinationFacts) {
  SanitizerPublicationMetadataRequestV1 Request = minimalRequest();
  Request.Platform = static_cast<PublicationPlatform>(0xff);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnsupportedPlatform);

  Request = minimalRequest();
  Request.Source.Node = PublicationNodeKind::Other;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidSourceNode);

  Request = minimalRequest();
  Request.Destination.Node = PublicationNodeKind::Other;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidDestinationNode);

  Request = minimalRequest();
  Request.Destination.Owner = digest(7);
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::InvalidAbsentDestination);

  Request = minimalRequest();
  Request.Destination = Request.Source;
  Request.Destination.OwnerReadable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadableOwner);

  Request = minimalRequest();
  Request.Destination = Request.Source;
  Request.Destination.ACLReadable = false;
  EXPECT_EQ(planSanitizerPublicationMetadata(Request).Reason,
            SanitizerPublicationMetadataReason::UnreadableACL);
}

TEST(SanitizerPublicationMetadata, ReasonNamesAreStable) {
  EXPECT_EQ(static_cast<uint8_t>(SecurityMetadataSubject::POSIXMode), 10u);
  EXPECT_EQ(static_cast<uint16_t>(
                SanitizerPublicationMetadataReason::UnreadablePOSIXMode),
            41u);
  EXPECT_EQ(static_cast<uint16_t>(
                SanitizerPublicationMetadataReason::MissingPOSIXModeMetadata),
            42u);
  EXPECT_EQ(static_cast<uint16_t>(
                SanitizerPublicationMetadataReason::InvalidPOSIXModeEncoding),
            43u);
  struct Case {
    SanitizerPublicationMetadataReason Reason;
    const char *Name;
  };
  const Case Cases[] = {
      {SanitizerPublicationMetadataReason::None, "none"},
      {SanitizerPublicationMetadataReason::UnsupportedVersion,
       "unsupported_version"},
      {SanitizerPublicationMetadataReason::InvalidLimits, "invalid_limits"},
      {SanitizerPublicationMetadataReason::UnsupportedPlatform,
       "unsupported_platform"},
      {SanitizerPublicationMetadataReason::InvalidSourceNode,
       "invalid_source_node"},
      {SanitizerPublicationMetadataReason::InvalidDestinationNode,
       "invalid_destination_node"},
      {SanitizerPublicationMetadataReason::SymlinkUnsupported,
       "symlink_unsupported"},
      {SanitizerPublicationMetadataReason::ReparsePointUnsupported,
       "reparse_point_unsupported"},
      {SanitizerPublicationMetadataReason::HardlinkUnsupported,
       "hardlink_unsupported"},
      {SanitizerPublicationMetadataReason::MissingOwnerMetadata,
       "missing_owner_metadata"},
      {SanitizerPublicationMetadataReason::UnreadableOwner, "unreadable_owner"},
      {SanitizerPublicationMetadataReason::UnreadableACL, "unreadable_acl"},
      {SanitizerPublicationMetadataReason::UnreadableMetadata,
       "unreadable_metadata"},
      {SanitizerPublicationMetadataReason::MetadataRecordLimitExceeded,
       "metadata_record_limit_exceeded"},
      {SanitizerPublicationMetadataReason::MetadataByteBudgetExceeded,
       "metadata_byte_budget_exceeded"},
      {SanitizerPublicationMetadataReason::ActionLimitExceeded,
       "action_limit_exceeded"},
      {SanitizerPublicationMetadataReason::ArithmeticOverflow,
       "arithmetic_overflow"},
      {SanitizerPublicationMetadataReason::NonCanonicalMetadataOrder,
       "noncanonical_metadata_order"},
      {SanitizerPublicationMetadataReason::DuplicateMetadata,
       "duplicate_metadata"},
      {SanitizerPublicationMetadataReason::UnsupportedMetadataKind,
       "unsupported_metadata_kind"},
      {SanitizerPublicationMetadataReason::MetadataPlatformMismatch,
       "metadata_platform_mismatch"},
      {SanitizerPublicationMetadataReason::UnknownExtendedAttribute,
       "unknown_extended_attribute"},
      {SanitizerPublicationMetadataReason::UnknownAlternateDataStream,
       "unknown_alternate_data_stream"},
      {SanitizerPublicationMetadataReason::IntegrityMetadataUnsupported,
       "integrity_metadata_unsupported"},
      {SanitizerPublicationMetadataReason::ProvenanceConflict,
       "provenance_conflict"},
      {SanitizerPublicationMetadataReason::SELinuxFinalPathLabelRequired,
       "selinux_final_path_label_required"},
      {SanitizerPublicationMetadataReason::SELinuxSourceLabelForbidden,
       "selinux_source_label_forbidden"},
      {SanitizerPublicationMetadataReason::InvalidFinalPathSELinuxLabel,
       "invalid_final_path_selinux_label"},
      {SanitizerPublicationMetadataReason::SameSourceGuardedUnsupported,
       "same_source_guarded_unsupported"},
      {SanitizerPublicationMetadataReason::ExistingDestinationCASUnsupported,
       "existing_destination_cas_unsupported"},
      {SanitizerPublicationMetadataReason::InvalidAbsentDestination,
       "invalid_absent_destination"},
      {SanitizerPublicationMetadataReason::SnapshotConflict,
       "snapshot_conflict"},
      {SanitizerPublicationMetadataReason::InvalidMetadataOrigin,
       "invalid_metadata_origin"},
      {SanitizerPublicationMetadataReason::IncompleteMetadataEnumeration,
       "incomplete_metadata_enumeration"},
      {SanitizerPublicationMetadataReason::MissingACLMetadata,
       "missing_acl_metadata"},
      {SanitizerPublicationMetadataReason::CandidateMutationAttestationRequired,
       "candidate_mutation_attestation_required"},
      {SanitizerPublicationMetadataReason::SameSourceMutationUnsupported,
       "same_source_mutation_unsupported"},
      {SanitizerPublicationMetadataReason::InvalidCandidateMutationDisposition,
       "invalid_candidate_mutation_disposition"},
      {SanitizerPublicationMetadataReason::SELinuxPolicyUnavailable,
       "selinux_policy_unavailable"},
      {SanitizerPublicationMetadataReason::InvalidSELinuxPolicyState,
       "invalid_selinux_policy_state"},
      {SanitizerPublicationMetadataReason::SELinuxPolicyConflict,
       "selinux_policy_conflict"},
      {SanitizerPublicationMetadataReason::UnreadablePOSIXMode,
       "unreadable_posix_mode"},
      {SanitizerPublicationMetadataReason::MissingPOSIXModeMetadata,
       "missing_posix_mode_metadata"},
      {SanitizerPublicationMetadataReason::InvalidPOSIXModeEncoding,
       "invalid_posix_mode_encoding"},
  };
  for (const Case &C : Cases)
    EXPECT_STREQ(toString(C.Reason), C.Name);
  EXPECT_STREQ(
      toString(static_cast<SanitizerPublicationMetadataReason>(0xffff)),
      "unknown");
}

} // namespace
