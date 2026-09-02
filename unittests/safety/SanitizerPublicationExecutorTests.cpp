//===- SanitizerPublicationExecutorTests.cpp - Executor contract tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/SanitizerPublicationExecutor.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::safety::sanitizer_publication_metadata;

namespace {

constexpr std::array<uint8_t, 3> kDefaultOwnerBytes = {0x11, 0x22, 0x33};
constexpr std::array<uint8_t, 4> kPOSIXMode0755 = {0xed, 0x01, 0x00, 0x00};

ArtifactContentDigestV1 artifactContent(uint8_t Byte, uint64_t Size) {
  ArtifactContentDigestV1 Digest;
  Digest.SHA256.fill(Byte);
  Digest.Size = Size;
  return Digest;
}

ArtifactStableIdentityDigestV1 artifactIdentity(uint8_t Byte) {
  ArtifactStableIdentityDigestV1 Digest;
  Digest.SHA256.fill(Byte);
  return Digest;
}

ArtifactBindingV1
artifactBindingFor(const SanitizerPublicationMetadataPlanV1 &Plan) {
  ArtifactBindingV1 Binding;
  Binding.ExpectedSourceContent = artifactContent(0xa1, 4096);
  Binding.ExpectedSourceStableIdentity = artifactIdentity(0xb1);
  Binding.ExpectedCandidateContent =
      Plan.CandidateMutation == CandidateMutationDisposition::ByteIdentical
          ? Binding.ExpectedSourceContent
          : artifactContent(0xa2, 4103);
  return Binding;
}

ArtifactAuthenticationV1
artifactAuthentication(const ArtifactContentDigestV1 &Content,
                       const ArtifactStableIdentityDigestV1 &Identity) {
  return {PublicationNodeKind::RegularFile, 1, Content, Identity};
}

llvm::Error testError(const char *Message) {
  return llvm::createStringError(llvm::errc::io_error, "%s", Message);
}

SecurityMetadataAction preserve(SecurityMetadataSubject Subject,
                                llvm::ArrayRef<uint8_t> Bytes) {
  return {Subject, SecurityMetadataDisposition::PreserveFromSource,
          digestCanonicalSecurityMetadata(Bytes)};
}

SecurityMetadataAction clear(SecurityMetadataSubject Subject) {
  return {Subject, SecurityMetadataDisposition::Clear, std::nullopt};
}

SourceMetadataManifestEntryV1 sourceMetadata(SecurityMetadataSubject Subject,
                                             llvm::ArrayRef<uint8_t> Bytes) {
  return {Subject, digestCanonicalSecurityMetadata(Bytes)};
}

SourceMetadataManifestEntryV1 sourcePresence(SecurityMetadataSubject Subject) {
  return {Subject, std::nullopt};
}

SanitizerPublicationMetadataPlanV1
linuxPlan(llvm::ArrayRef<uint8_t> OwnerBytes) {
  SanitizerPublicationMetadataPlanV1 Plan;
  Plan.Complete = true;
  Plan.Platform = PublicationPlatform::Linux;
  Plan.GuardedSiteCount = 1;
  Plan.CandidateMutation = CandidateMutationDisposition::ByteMutated;
  Plan.SELinuxPolicy = SELinuxPolicyState::DisabledAttested;
  Plan.NamespaceDisposition = PublicationNamespaceDisposition::CreateExclusive;
  Plan.ObservedRecordCount = 2;
  Plan.ObservedEncodedBytes = OwnerBytes.size() + kPOSIXMode0755.size();
  Plan.SourceMetadata = {
      sourceMetadata(SecurityMetadataSubject::Owner, OwnerBytes),
      sourceMetadata(SecurityMetadataSubject::POSIXMode, kPOSIXMode0755),
  };
  Plan.Actions = {
      preserve(SecurityMetadataSubject::Owner, OwnerBytes),
      clear(SecurityMetadataSubject::ACL),
      clear(SecurityMetadataSubject::SetUIDBit),
      clear(SecurityMetadataSubject::SetGIDBit),
      {SecurityMetadataSubject::LinuxCapability,
       SecurityMetadataDisposition::ClearAndVerifyAbsent, std::nullopt},
      {SecurityMetadataSubject::LinuxSELinux,
       SecurityMetadataDisposition::ClearAndVerifyAbsent, std::nullopt},
      preserve(SecurityMetadataSubject::POSIXMode, kPOSIXMode0755),
  };
  return Plan;
}

SanitizerPublicationMetadataPlanV1 noChangePlan() {
  SanitizerPublicationMetadataPlanV1 Plan;
  Plan.Complete = true;
  Plan.Platform = PublicationPlatform::Linux;
  Plan.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
  // No-change is deliberately allowed when transform-specific SELinux policy
  // is unavailable because it performs no inode or metadata operation.
  Plan.SELinuxPolicy = SELinuxPolicyState::Unavailable;
  Plan.NamespaceDisposition = PublicationNamespaceDisposition::NoChange;
  Plan.ObservedRecordCount = 2;
  Plan.ObservedEncodedBytes = kDefaultOwnerBytes.size() + kPOSIXMode0755.size();
  Plan.SourceMetadata = {
      sourceMetadata(SecurityMetadataSubject::Owner, kDefaultOwnerBytes),
      sourceMetadata(SecurityMetadataSubject::POSIXMode, kPOSIXMode0755),
  };
  return Plan;
}

SanitizerPublicationMetadataPlanV1
macOSPlan(llvm::ArrayRef<uint8_t> OwnerBytes,
          llvm::ArrayRef<uint8_t> QuarantineBytes,
          llvm::ArrayRef<uint8_t> ProvenanceBytes) {
  SanitizerPublicationMetadataPlanV1 Plan;
  Plan.Complete = true;
  Plan.Platform = PublicationPlatform::MacOS;
  Plan.GuardedSiteCount = 1;
  Plan.CandidateMutation = CandidateMutationDisposition::ByteMutated;
  Plan.NamespaceDisposition = PublicationNamespaceDisposition::CreateExclusive;
  Plan.ObservedRecordCount = 4;
  Plan.ObservedEncodedBytes = OwnerBytes.size() + QuarantineBytes.size() +
                              ProvenanceBytes.size() + kPOSIXMode0755.size();
  Plan.SourceMetadata = {
      sourceMetadata(SecurityMetadataSubject::Owner, OwnerBytes),
      sourceMetadata(SecurityMetadataSubject::MacOSQuarantine, QuarantineBytes),
      sourceMetadata(SecurityMetadataSubject::MacOSProvenance, ProvenanceBytes),
      sourceMetadata(SecurityMetadataSubject::POSIXMode, kPOSIXMode0755),
  };
  Plan.Actions = {
      preserve(SecurityMetadataSubject::Owner, OwnerBytes),
      clear(SecurityMetadataSubject::ACL),
      clear(SecurityMetadataSubject::SetUIDBit),
      clear(SecurityMetadataSubject::SetGIDBit),
      preserve(SecurityMetadataSubject::MacOSQuarantine, QuarantineBytes),
      preserve(SecurityMetadataSubject::MacOSProvenance, ProvenanceBytes),
      preserve(SecurityMetadataSubject::POSIXMode, kPOSIXMode0755),
  };
  return Plan;
}

SanitizerPublicationMetadataPlanV1
windowsPlan(llvm::ArrayRef<uint8_t> OwnerBytes,
            llvm::ArrayRef<uint8_t> ACLBytes,
            llvm::ArrayRef<uint8_t> MOTWBytes) {
  SanitizerPublicationMetadataPlanV1 Plan;
  Plan.Complete = true;
  Plan.Platform = PublicationPlatform::Windows;
  Plan.GuardedSiteCount = 1;
  Plan.CandidateMutation = CandidateMutationDisposition::ByteMutated;
  Plan.NamespaceDisposition = PublicationNamespaceDisposition::CreateExclusive;
  Plan.ObservedRecordCount = 3;
  Plan.ObservedEncodedBytes =
      OwnerBytes.size() + ACLBytes.size() + MOTWBytes.size();
  Plan.SourceMetadata = {
      sourceMetadata(SecurityMetadataSubject::Owner, OwnerBytes),
      sourceMetadata(SecurityMetadataSubject::ACL, ACLBytes),
      sourceMetadata(SecurityMetadataSubject::WindowsMOTW, MOTWBytes),
  };
  Plan.Actions = {
      preserve(SecurityMetadataSubject::Owner, OwnerBytes),
      preserve(SecurityMetadataSubject::ACL, ACLBytes),
      preserve(SecurityMetadataSubject::WindowsMOTW, MOTWBytes),
  };
  return Plan;
}

enum class FailurePoint {
  None,
  Begin,
  ThrowBegin,
  SourceObserve,
  Create,
  CandidateFirst,
  CandidateSecond,
  Read,
  Apply,
  Observe,
  Destination,
  Discard,
  ThrowApply,
  ThrowPublish,
  FinalAuthentication,
  Finalize,
  ThrowFinalAuthentication,
  ThrowFinalize,
};

struct Harness {
  std::vector<uint8_t> OwnerBytes{kDefaultOwnerBytes.begin(),
                                  kDefaultOwnerBytes.end()};
  std::vector<uint8_t> ACLBytes{0x44, 0x55};
  std::vector<uint8_t> SELinuxBytes{0x66, 0x77, 0x88};
  std::vector<uint8_t> QuarantineBytes{0x91, 0x92};
  std::vector<uint8_t> ProvenanceBytes{0x93, 0x94};
  std::vector<uint8_t> MOTWBytes{0x95, 0x96};
  std::vector<uint8_t> POSIXModeBytes{kPOSIXMode0755.begin(),
                                      kPOSIXMode0755.end()};
  CompleteMetadataSnapshotV1 SourceSnapshot;
  CompleteMetadataSnapshotV1 Snapshot;
  ArtifactAuthenticationV1 SourceArtifact;
  ArtifactAuthenticationV1 NoChangeFinalArtifact;
  ArtifactAuthenticationV1 CandidateArtifact;
  ArtifactAuthenticationV1 CandidateAfterMetadataArtifact;
  ArtifactAuthenticationV1 FinalArtifact;
  std::optional<CompleteMetadataSnapshotV1> FinalSnapshotOverride;
  std::optional<CompleteMetadataSnapshotV1> NoChangeFinalSnapshotOverride;
  FailurePoint Failure = FailurePoint::None;
  int FailApplyCall = 1;
  NamespacePublishResultV1 PublishResult{SanitizerPublicationOutcome::Published,
                                         {}};
  std::vector<std::string> Events;
  std::vector<SecurityMetadataOrigin> MaterialOrigins;
  int NoChangeCalls = 0;
  int BeginCalls = 0;
  int SourceObserveCalls = 0;
  int CreateCalls = 0;
  int CandidateCalls = 0;
  int ReadCalls = 0;
  int ApplyCalls = 0;
  int ObserveCalls = 0;
  int DestinationCalls = 0;
  int PublishCalls = 0;
  int FinalAuthenticationCalls = 0;
  int FinalizeCalls = 0;
  int DiscardCalls = 0;

  Harness() {
    const ArtifactBindingV1 Binding = artifactBindingFor(linuxPlan(OwnerBytes));
    SourceArtifact = artifactAuthentication(
        Binding.ExpectedSourceContent, Binding.ExpectedSourceStableIdentity);
    NoChangeFinalArtifact = SourceArtifact;
    CandidateArtifact = artifactAuthentication(Binding.ExpectedCandidateContent,
                                               artifactIdentity(0xb2));
    CandidateAfterMetadataArtifact = CandidateArtifact;
    FinalArtifact = CandidateArtifact;
    SourceSnapshot.EnumerationComplete = true;
    SourceSnapshot.Values.push_back(
        {SecurityMetadataSubject::Owner, OwnerBytes});
    SourceSnapshot.Values.push_back(
        {SecurityMetadataSubject::POSIXMode, POSIXModeBytes});
    Snapshot.EnumerationComplete = true;
    Snapshot.Values.push_back({SecurityMetadataSubject::Owner, OwnerBytes});
    Snapshot.Values.push_back(
        {SecurityMetadataSubject::POSIXMode, POSIXModeBytes});
  }
};

SanitizerPublicationExecutorOperationsV1 operations(Harness &H) {
  SanitizerPublicationExecutorOperationsV1 Ops;
  Ops.Capabilities.SourceIdentityPinned = true;
  Ops.Capabilities.DestinationDirectoryAnchored = true;
  Ops.Capabilities.TemporaryCreationExclusive = true;
  Ops.Capabilities.CandidatePublishOperandBinding =
      CandidatePublishOperandBindingV1::KernelHeldObject;
  Ops.Capabilities.CompleteSourceMetadataEnumeration = true;
  Ops.Capabilities.CompleteCandidateMetadataEnumeration = true;
  Ops.Capabilities.CompletePublishedFinalMetadataEnumeration = true;
  Ops.Capabilities.ArtifactContentAuthentication = true;
  Ops.Capabilities.StableObjectIdentityAuthentication = true;
  Ops.Capabilities.AnchoredNoChangeFinalAuthentication = true;
  Ops.Capabilities.AnchoredPublishedFinalAuthentication = true;
  Ops.Capabilities.AtomicNoReplacePublish = true;
  Ops.ReauthenticateNoChange =
      [&]() -> llvm::Expected<AuthenticatedNoChangeArtifactsV1> {
    ++H.NoChangeCalls;
    H.Events.emplace_back("no-change");
    return AuthenticatedNoChangeArtifactsV1{
        {H.SourceArtifact, H.SourceSnapshot},
        {H.NoChangeFinalArtifact, H.NoChangeFinalSnapshotOverride
                                      ? *H.NoChangeFinalSnapshotOverride
                                      : H.SourceSnapshot}};
  };
  Ops.Begin = [&]() -> llvm::Error {
    ++H.BeginCalls;
    H.Events.emplace_back("begin");
    if (H.Failure == FailurePoint::ThrowBegin)
      throw std::runtime_error("begin threw");
    if (H.Failure == FailurePoint::Begin)
      return testError("begin failed");
    return llvm::Error::success();
  };
  Ops.AuthenticateSourceAfterBegin =
      [&]() -> llvm::Expected<AuthenticatedSourceArtifactV1> {
    ++H.SourceObserveCalls;
    H.Events.emplace_back("source-observe");
    if (H.Failure == FailurePoint::SourceObserve)
      return testError("source observe failed");
    return AuthenticatedSourceArtifactV1{H.SourceArtifact, H.SourceSnapshot};
  };
  Ops.CreateExclusiveCandidate = [&]() -> llvm::Error {
    ++H.CreateCalls;
    H.Events.emplace_back("create");
    if (H.Failure == FailurePoint::Create)
      return testError("create failed");
    return llvm::Error::success();
  };
  Ops.AuthenticateCandidateAfterCreate =
      [&]() -> llvm::Expected<ArtifactAuthenticationV1> {
    ++H.CandidateCalls;
    H.Events.emplace_back("candidate");
    if (H.Failure == FailurePoint::CandidateFirst)
      return testError("candidate failed");
    return H.CandidateArtifact;
  };
  Ops.ReadMaterial = [&](SecurityMetadataSubject Subject,
                         SecurityMetadataOrigin Origin)
      -> llvm::Expected<CanonicalSecurityMetadata> {
    ++H.ReadCalls;
    H.MaterialOrigins.push_back(Origin);
    H.Events.push_back("read-" + std::to_string(static_cast<uint8_t>(Subject)));
    if (H.Failure == FailurePoint::Read)
      return testError("read failed");
    switch (Subject) {
    case SecurityMetadataSubject::Owner:
      return H.OwnerBytes;
    case SecurityMetadataSubject::ACL:
      return H.ACLBytes;
    case SecurityMetadataSubject::MacOSQuarantine:
      return H.QuarantineBytes;
    case SecurityMetadataSubject::MacOSProvenance:
      return H.ProvenanceBytes;
    case SecurityMetadataSubject::LinuxSELinux:
      return H.SELinuxBytes;
    case SecurityMetadataSubject::WindowsMOTW:
      return H.MOTWBytes;
    case SecurityMetadataSubject::POSIXMode:
      return H.POSIXModeBytes;
    default:
      return testError("unexpected material read");
    }
  };
  Ops.ApplyMetadata = [&](const SecurityMetadataAction &Action,
                          llvm::ArrayRef<uint8_t>) -> llvm::Error {
    ++H.ApplyCalls;
    H.Events.push_back("apply-" +
                       std::to_string(static_cast<uint8_t>(Action.Subject)));
    if (H.Failure == FailurePoint::ThrowApply)
      throw std::runtime_error("apply threw");
    if (H.Failure == FailurePoint::Apply && H.ApplyCalls == H.FailApplyCall)
      return testError("apply failed");
    return llvm::Error::success();
  };
  Ops.AuthenticateCandidateAfterMetadata =
      [&]() -> llvm::Expected<AuthenticatedCandidateArtifactV1> {
    ++H.CandidateCalls;
    ++H.ObserveCalls;
    H.Events.emplace_back("observe");
    if (H.Failure == FailurePoint::Observe ||
        H.Failure == FailurePoint::CandidateSecond)
      return testError("observe failed");
    return AuthenticatedCandidateArtifactV1{H.CandidateAfterMetadataArtifact,
                                            H.Snapshot};
  };
  Ops.ReauthenticateDestinationAbsent = [&]() -> llvm::Error {
    ++H.DestinationCalls;
    H.Events.emplace_back("destination");
    if (H.Failure == FailurePoint::Destination)
      return testError("destination failed");
    return llvm::Error::success();
  };
  Ops.PublishNoReplace = [&]() -> NamespacePublishResultV1 {
    ++H.PublishCalls;
    H.Events.emplace_back("publish");
    if (H.Failure == FailurePoint::ThrowPublish)
      throw std::runtime_error("publish threw");
    return H.PublishResult;
  };
  Ops.AuthenticatePublishedFinal =
      [&]() -> llvm::Expected<AuthenticatedPublishedFinalArtifactV1> {
    ++H.FinalAuthenticationCalls;
    H.Events.emplace_back("final-observe");
    if (H.Failure == FailurePoint::ThrowFinalAuthentication)
      throw std::runtime_error("final authentication threw");
    if (H.Failure == FailurePoint::FinalAuthentication)
      return testError("final authentication failed");
    return AuthenticatedPublishedFinalArtifactV1{
        H.FinalArtifact,
        H.FinalSnapshotOverride ? *H.FinalSnapshotOverride : H.Snapshot};
  };
  Ops.FinalizePublished = [&]() -> llvm::Error {
    ++H.FinalizeCalls;
    H.Events.emplace_back("finalize");
    if (H.Failure == FailurePoint::ThrowFinalize)
      throw std::runtime_error("finalize threw");
    if (H.Failure == FailurePoint::Finalize)
      return testError("finalize failed");
    return llvm::Error::success();
  };
  Ops.Discard = [&]() -> llvm::Error {
    ++H.DiscardCalls;
    H.Events.emplace_back("discard");
    if (H.Failure == FailurePoint::Discard)
      return testError("discard failed");
    return llvm::Error::success();
  };
  return Ops;
}

SanitizerPublicationExecutionResultV1 executeSanitizerPublicationMetadata(
    const SanitizerPublicationMetadataPlanV1 &Plan,
    const SanitizerPublicationExecutorOperationsV1 &Operations) {
  return neverd::safety::sanitizer_publication_metadata::
      executeSanitizerPublicationMetadata(Plan, artifactBindingFor(Plan),
                                          Operations);
}

TEST(SanitizerPublicationExecutor,
     RejectsPOSIXPlanThatOmitsExplicitModePolicyBeforeBegin) {
  Harness H;
  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  Plan.Actions.pop_back();
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));

  EXPECT_FALSE(Result.succeeded());
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::IncompleteActionSet);
  EXPECT_EQ(H.BeginCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     RejectsInvalidOrMutationInconsistentArtifactBindingsBeforeBegin) {
  Harness H;
  const SanitizerPublicationExecutorOperationsV1 Ops = operations(H);

  struct Case {
    SanitizerPublicationMetadataPlanV1 Plan;
    ArtifactBindingV1 Binding;
  };
  std::vector<Case> Cases;
  {
    SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
    ArtifactBindingV1 Binding = artifactBindingFor(Plan);
    ++Binding.Version;
    Cases.push_back({std::move(Plan), std::move(Binding)});
  }
  {
    SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
    Plan.CandidateMutation = CandidateMutationDisposition::Unauthenticated;
    Cases.push_back({Plan, artifactBindingFor(Plan)});
  }
  {
    SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
    Plan.CandidateMutation = CandidateMutationDisposition::ByteIdentical;
    ArtifactBindingV1 Binding = artifactBindingFor(Plan);
    Binding.ExpectedCandidateContent = artifactContent(0xee, 4096);
    Cases.push_back({std::move(Plan), std::move(Binding)});
  }
  {
    SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
    ArtifactBindingV1 Binding = artifactBindingFor(Plan);
    Binding.ExpectedCandidateContent = Binding.ExpectedSourceContent;
    Cases.push_back({std::move(Plan), std::move(Binding)});
  }

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    SCOPED_TRACE(Index);
    const SanitizerPublicationExecutionResultV1 Result = neverd::safety::
        sanitizer_publication_metadata::executeSanitizerPublicationMetadata(
            Cases[Index].Plan, Cases[Index].Binding, Ops);
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::InvalidArtifactBinding);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_TRUE(H.Events.empty());
  }
  EXPECT_EQ(H.BeginCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 0);
  EXPECT_EQ(H.FinalizeCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     ArtifactContentSizeIsAuthenticatedButNotMetadataBudgeted) {
  Harness H;
  const SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  ArtifactBindingV1 Binding = artifactBindingFor(Plan);
  Binding.ExpectedSourceContent.Size =
      kSanitizerPublicationMetadataMaxEncodedBytes + 1;
  Binding.ExpectedCandidateContent.Size =
      kSanitizerPublicationMetadataMaxEncodedBytes + 2;
  H.SourceArtifact.Content = Binding.ExpectedSourceContent;
  H.CandidateArtifact.Content = Binding.ExpectedCandidateContent;
  H.CandidateAfterMetadataArtifact.Content = Binding.ExpectedCandidateContent;
  H.FinalArtifact.Content = Binding.ExpectedCandidateContent;

  const SanitizerPublicationExecutionResultV1 Result =
      neverd::safety::sanitizer_publication_metadata::
          executeSanitizerPublicationMetadata(Plan, Binding, operations(H));
  EXPECT_TRUE(Result.succeeded());
  ASSERT_TRUE(Result.Receipt.SourceArtifact.has_value());
  ASSERT_TRUE(Result.Receipt.CandidateArtifact.has_value());
  ASSERT_TRUE(Result.Receipt.FinalArtifact.has_value());
  EXPECT_EQ(Result.Receipt.SourceArtifact->Content.Size,
            Binding.ExpectedSourceContent.Size);
  EXPECT_EQ(Result.Receipt.CandidateArtifact->Content.Size,
            Binding.ExpectedCandidateContent.Size);
  EXPECT_EQ(Result.Receipt.FinalArtifact->Content.Size,
            Binding.ExpectedCandidateContent.Size);
}

TEST(SanitizerPublicationExecutor,
     ExecutesOneHandleBoundCreateExclusiveTransaction) {
  Harness H;
  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));

  EXPECT_TRUE(Result.succeeded());
  EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::None);
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
  EXPECT_EQ(Result.Receipt.Version, kSanitizerPublicationExecutorSchemaVersion);
  EXPECT_EQ(kSanitizerPublicationExecutorAdapter,
            std::string_view("security-metadata-exec-v1"));
  ASSERT_TRUE(Result.Receipt.Platform.has_value());
  EXPECT_EQ(*Result.Receipt.Platform, PublicationPlatform::Linux);
  EXPECT_EQ(Result.Receipt.NamespaceDisposition,
            PublicationNamespaceDisposition::CreateExclusive);
  EXPECT_EQ(Result.Receipt.AppliedActionCount, 7u);
  EXPECT_EQ(Result.Receipt.VerifiedActionCount, 7u);
  EXPECT_EQ(Result.Receipt.ObservedSourceMetadataCount, 2u);
  EXPECT_EQ(Result.Receipt.ObservedCandidateMetadataCount, 2u);
  EXPECT_EQ(Result.Receipt.ObservedFinalMetadataCount, 2u);
  const ArtifactBindingV1 Binding = artifactBindingFor(Plan);
  ASSERT_TRUE(Result.Receipt.SourceArtifact.has_value());
  ASSERT_TRUE(Result.Receipt.CandidateArtifact.has_value());
  ASSERT_TRUE(Result.Receipt.FinalArtifact.has_value());
  EXPECT_TRUE(Result.Receipt.SourceArtifact->ContentMatched);
  EXPECT_EQ(Result.Receipt.SourceArtifact->Content,
            Binding.ExpectedSourceContent);
  EXPECT_TRUE(Result.Receipt.SourceArtifact->StableIdentityMatched);
  EXPECT_TRUE(Result.Receipt.CandidateArtifact->ContentMatched);
  EXPECT_EQ(Result.Receipt.CandidateArtifact->Content,
            Binding.ExpectedCandidateContent);
  EXPECT_TRUE(Result.Receipt.CandidateArtifact->StableIdentityMatched);
  EXPECT_TRUE(Result.Receipt.FinalArtifact->ContentMatched);
  EXPECT_EQ(Result.Receipt.FinalArtifact->Content,
            Binding.ExpectedCandidateContent);
  EXPECT_TRUE(Result.Receipt.FinalArtifact->StableIdentityMatched);
  EXPECT_TRUE(Result.Receipt.Guarantees.NamespaceAtomic);
  EXPECT_TRUE(Result.Receipt.Guarantees.DestinationCreateExclusive);
  EXPECT_FALSE(Result.Receipt.Guarantees.CompareAndSwap);
  EXPECT_FALSE(Result.Receipt.Guarantees.CrashDurable);
  EXPECT_EQ(Result.Receipt.Guarantees.CandidatePublishOperandBinding,
            CandidatePublishOperandBindingV1::KernelHeldObject);
  EXPECT_EQ(H.CandidateCalls, 2);
  EXPECT_EQ(H.PublishCalls, 1);
  EXPECT_EQ(H.FinalAuthenticationCalls, 1);
  EXPECT_EQ(H.FinalizeCalls, 1);
  EXPECT_EQ(H.DiscardCalls, 0);
  EXPECT_EQ(H.Events,
            (std::vector<std::string>{
                "begin", "source-observe", "create", "candidate", "read-1",
                "apply-1", "apply-2", "apply-3", "apply-4", "apply-7",
                "apply-8", "read-10", "apply-10", "observe", "destination",
                "publish", "final-observe", "finalize"}));
}

TEST(SanitizerPublicationExecutor,
     NoChangeCallsOnlyItsSingleReadOnlyReauthentication) {
  Harness H;
  SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(noChangePlan(), Ops);

  EXPECT_TRUE(Result.succeeded());
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_EQ(Result.Receipt.NamespaceDisposition,
            PublicationNamespaceDisposition::NoChange);
  EXPECT_EQ(Result.Receipt.AppliedActionCount, 0u);
  EXPECT_EQ(Result.Receipt.VerifiedActionCount, 0u);
  EXPECT_EQ(Result.Receipt.ObservedSourceMetadataCount, 2u);
  EXPECT_EQ(Result.Receipt.ObservedCandidateMetadataCount, 0u);
  EXPECT_EQ(Result.Receipt.ObservedFinalMetadataCount, 2u);
  ASSERT_TRUE(Result.Receipt.SourceArtifact.has_value());
  ASSERT_TRUE(Result.Receipt.FinalArtifact.has_value());
  EXPECT_TRUE(Result.Receipt.SourceArtifact->ContentMatched);
  EXPECT_TRUE(Result.Receipt.FinalArtifact->ContentMatched);
  EXPECT_EQ(Result.Receipt.FinalArtifact->StableIdentity,
            Result.Receipt.SourceArtifact->StableIdentity);
  EXPECT_FALSE(Result.Receipt.CandidateArtifact.has_value());
  EXPECT_FALSE(Result.Receipt.Guarantees.NamespaceAtomic);
  EXPECT_FALSE(Result.Receipt.Guarantees.DestinationCreateExclusive);
  EXPECT_FALSE(Result.Receipt.Guarantees.CompareAndSwap);
  EXPECT_FALSE(Result.Receipt.Guarantees.CrashDurable);
  EXPECT_EQ(H.NoChangeCalls, 1);
  EXPECT_EQ(H.Events, (std::vector<std::string>{"no-change"}));
  EXPECT_EQ(H.BeginCalls, 0);
  EXPECT_EQ(H.SourceObserveCalls, 0);
  EXPECT_EQ(H.CreateCalls, 0);
  EXPECT_EQ(H.ReadCalls, 0);
  EXPECT_EQ(H.ApplyCalls, 0);
  EXPECT_EQ(H.ObserveCalls, 0);
  EXPECT_EQ(H.DestinationCalls, 0);
  EXPECT_EQ(H.PublishCalls, 0);
  EXPECT_EQ(H.FinalAuthenticationCalls, 0);
  EXPECT_EQ(H.FinalizeCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     NoChangeFailureStillPerformsNoMutationOrDiscard) {
  Harness H;
  SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  Ops.ReauthenticateNoChange =
      [&]() -> llvm::Expected<AuthenticatedNoChangeArtifactsV1> {
    ++H.NoChangeCalls;
    H.Events.emplace_back("no-change");
    return testError("snapshot changed");
  };
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(noChangePlan(), Ops);

  EXPECT_FALSE(Result.succeeded());
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::NoChangeReauthenticationFailed);
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_EQ(H.Events, (std::vector<std::string>{"no-change"}));
  EXPECT_EQ(H.DiscardCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     NoChangeRejectsAStaleOrUnrepresentableSourceSnapshotReadOnly) {
  struct Case {
    const char *Name;
    CompleteMetadataSnapshotV1 Snapshot;
    SanitizerPublicationExecutionReason Reason;
  };
  Harness Seed;
  CompleteMetadataSnapshotV1 WrongDigest = Seed.SourceSnapshot;
  WrongDigest.Values[0].CanonicalBytes.push_back(0xff);
  CompleteMetadataSnapshotV1 Added = Seed.SourceSnapshot;
  Added.Values.insert(Added.Values.end() - 1,
                      {SecurityMetadataSubject::ACL, Seed.ACLBytes});
  CompleteMetadataSnapshotV1 Missing = Seed.SourceSnapshot;
  Missing.Values.pop_back();
  CompleteMetadataSnapshotV1 Unsupported = Seed.SourceSnapshot;
  Unsupported.UnsupportedMetadataCount = 1;
  const Case Cases[] = {
      {"digest", std::move(WrongDigest),
       SanitizerPublicationExecutionReason::MetadataDigestMismatch},
      {"added", std::move(Added),
       SanitizerPublicationExecutionReason::UnexpectedMetadataObserved},
      {"missing", std::move(Missing),
       SanitizerPublicationExecutionReason::MissingMetadataObservation},
      {"unsupported", std::move(Unsupported),
       SanitizerPublicationExecutionReason::UnsupportedMetadataObserved},
  };

  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Name);
    Harness H;
    H.SourceSnapshot = Entry.Snapshot;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(noChangePlan(), operations(H));
    EXPECT_EQ(Result.Reason, Entry.Reason);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(H.NoChangeCalls, 1);
    EXPECT_EQ(H.Events, (std::vector<std::string>{"no-change"}));
    EXPECT_EQ(H.BeginCalls, 0);
    EXPECT_EQ(H.SourceObserveCalls, 0);
    EXPECT_EQ(H.CreateCalls, 0);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 0);
  }
}

TEST(SanitizerPublicationExecutor,
     NoChangeAuthenticatesBoundSourceContentAndStableIdentity) {
  const SanitizerPublicationMetadataPlanV1 Plan = noChangePlan();
  for (int Case = 0; Case < 2; ++Case) {
    SCOPED_TRACE(Case);
    Harness H;
    ArtifactBindingV1 Binding = artifactBindingFor(Plan);
    if (Case == 0) {
      Binding.ExpectedSourceContent = artifactContent(0xee, 4096);
      Binding.ExpectedCandidateContent = Binding.ExpectedSourceContent;
    } else {
      Binding.ExpectedSourceStableIdentity = artifactIdentity(0xee);
    }
    const SanitizerPublicationExecutionResultV1 Result =
        neverd::safety::sanitizer_publication_metadata::
            executeSanitizerPublicationMetadata(Plan, Binding, operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::SourceAuthenticationMismatch);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(H.NoChangeCalls, 1);
    EXPECT_EQ(H.Events, (std::vector<std::string>{"no-change"}));
    EXPECT_EQ(H.BeginCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 0);
    EXPECT_EQ(H.FinalizeCalls, 0);
  }
}

TEST(SanitizerPublicationExecutor,
     NoChangeRejectsAnchoredFinalIdentityContentAndMetadataSwapReadOnly) {
  auto ExpectOnlyNoChange = [](const Harness &H) {
    EXPECT_EQ(H.NoChangeCalls, 1);
    EXPECT_EQ(H.Events, (std::vector<std::string>{"no-change"}));
    EXPECT_EQ(H.BeginCalls, 0);
    EXPECT_EQ(H.SourceObserveCalls, 0);
    EXPECT_EQ(H.CreateCalls, 0);
    EXPECT_EQ(H.CandidateCalls, 0);
    EXPECT_EQ(H.ReadCalls, 0);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.ObserveCalls, 0);
    EXPECT_EQ(H.DestinationCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.FinalAuthenticationCalls, 0);
    EXPECT_EQ(H.FinalizeCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 0);
  };

  {
    Harness H;
    H.NoChangeFinalArtifact.StableIdentity = artifactIdentity(0xee);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(noChangePlan(), operations(H));
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                                 PublishedFinalAuthenticationMismatch);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_FALSE(Result.Receipt.Complete);
    ExpectOnlyNoChange(H);
  }
  {
    Harness H;
    H.NoChangeFinalArtifact.Content = artifactContent(0xee, 4096);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(noChangePlan(), operations(H));
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                                 PublishedFinalAuthenticationMismatch);
    EXPECT_FALSE(Result.Receipt.Complete);
    ExpectOnlyNoChange(H);
  }
  {
    Harness H;
    CompleteMetadataSnapshotV1 Tampered = H.SourceSnapshot;
    Tampered.Values[0].CanonicalBytes.push_back(0xff);
    H.NoChangeFinalSnapshotOverride = std::move(Tampered);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(noChangePlan(), operations(H));
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                                 PublishedFinalAuthenticationMismatch);
    EXPECT_FALSE(Result.Receipt.Complete);
    ExpectOnlyNoChange(H);
  }
}

TEST(SanitizerPublicationExecutor,
     NoChangeMissingAnchoredFinalCapabilityRejectsBeforeItsOnlyCallback) {
  Harness H;
  SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  Ops.Capabilities.AnchoredNoChangeFinalAuthentication = false;
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(noChangePlan(), Ops);

  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities);
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_EQ(H.NoChangeCalls, 0);
  EXPECT_TRUE(H.Events.empty());
  EXPECT_EQ(H.BeginCalls, 0);
  EXPECT_EQ(H.CreateCalls, 0);
  EXPECT_EQ(H.PublishCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 0);
  EXPECT_EQ(H.FinalizeCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     ClearActionsNeverRequestRawMaterialEvenWhenPlanRecordsSourceDigest) {
  Harness H;
  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  const std::vector<uint8_t> CapabilityBytes{0xaa};
  const std::vector<uint8_t> SELinuxBytes{0xbb};
  Plan.Actions[4].ExpectedDigest =
      digestCanonicalSecurityMetadata(CapabilityBytes);
  Plan.Actions[5].ExpectedDigest =
      digestCanonicalSecurityMetadata(SELinuxBytes);
  Plan.SourceMetadata.insert(
      Plan.SourceMetadata.begin() + 1,
      sourcePresence(SecurityMetadataSubject::SetUIDBit));
  Plan.SourceMetadata.insert(
      Plan.SourceMetadata.end() - 1,
      sourceMetadata(SecurityMetadataSubject::LinuxCapability,
                     CapabilityBytes));
  Plan.SourceMetadata.insert(
      Plan.SourceMetadata.end() - 1,
      sourceMetadata(SecurityMetadataSubject::LinuxSELinux, SELinuxBytes));
  Plan.ObservedRecordCount += 3;
  Plan.ObservedEncodedBytes += CapabilityBytes.size() + SELinuxBytes.size();
  H.SourceSnapshot.Values.insert(H.SourceSnapshot.Values.begin() + 1,
                                 {SecurityMetadataSubject::SetUIDBit, {}});
  H.SourceSnapshot.Values.insert(
      H.SourceSnapshot.Values.end() - 1,
      {SecurityMetadataSubject::LinuxCapability, CapabilityBytes});
  H.SourceSnapshot.Values.insert(
      H.SourceSnapshot.Values.end() - 1,
      {SecurityMetadataSubject::LinuxSELinux, SELinuxBytes});
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));

  EXPECT_TRUE(Result.succeeded());
  EXPECT_EQ(H.ReadCalls, 2);
  ASSERT_EQ(H.MaterialOrigins.size(), 2u);
  EXPECT_EQ(H.MaterialOrigins[0], SecurityMetadataOrigin::SourceSnapshot);
  EXPECT_EQ(H.MaterialOrigins[1], SecurityMetadataOrigin::SourceSnapshot);
  EXPECT_EQ(Result.Receipt.ObservedSourceMetadataCount, 5u);
}

TEST(SanitizerPublicationExecutor,
     FinalPathDerivedSELinuxIsIndependentFromTheSourceLabel) {
  Harness H;
  const std::vector<uint8_t> SourceSELinuxBytes{0x51, 0x52};
  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  Plan.SELinuxPolicy = SELinuxPolicyState::FinalPathDerived;
  Plan.Actions[5] = {SecurityMetadataSubject::LinuxSELinux,
                     SecurityMetadataDisposition::ApplyFinalPathDerived,
                     digestCanonicalSecurityMetadata(H.SELinuxBytes)};
  Plan.SourceMetadata.insert(
      Plan.SourceMetadata.end() - 1,
      sourceMetadata(SecurityMetadataSubject::LinuxSELinux,
                     SourceSELinuxBytes));
  Plan.ObservedRecordCount += 2;
  Plan.ObservedEncodedBytes +=
      SourceSELinuxBytes.size() + H.SELinuxBytes.size();
  H.SourceSnapshot.Values.insert(
      H.SourceSnapshot.Values.end() - 1,
      {SecurityMetadataSubject::LinuxSELinux, SourceSELinuxBytes});
  H.Snapshot.Values.insert(
      H.Snapshot.Values.end() - 1,
      {SecurityMetadataSubject::LinuxSELinux, H.SELinuxBytes});

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));
  EXPECT_TRUE(Result.succeeded());
  ASSERT_EQ(H.MaterialOrigins.size(), 3u);
  EXPECT_EQ(H.MaterialOrigins[0], SecurityMetadataOrigin::SourceSnapshot);
  EXPECT_EQ(H.MaterialOrigins[1], SecurityMetadataOrigin::FinalPathDerived);
  EXPECT_EQ(H.MaterialOrigins[2], SecurityMetadataOrigin::SourceSnapshot);
  EXPECT_NE(Plan.SourceMetadata[1].ExpectedDigest,
            Plan.Actions[5].ExpectedDigest);
  EXPECT_EQ(Result.Receipt.ObservedSourceMetadataCount, 3u);
  EXPECT_EQ(Result.Receipt.ObservedCandidateMetadataCount, 3u);
}

TEST(SanitizerPublicationExecutor,
     MacOSXattrsAndWindowsSecuritySurfacesUseTheSameExactPolicy) {
  {
    Harness H;
    const SanitizerPublicationMetadataPlanV1 Plan =
        macOSPlan(H.OwnerBytes, H.QuarantineBytes, H.ProvenanceBytes);
    H.Snapshot.Values = {
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
        {SecurityMetadataSubject::MacOSQuarantine, H.QuarantineBytes},
        {SecurityMetadataSubject::MacOSProvenance, H.ProvenanceBytes},
        {SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes},
    };
    H.SourceSnapshot = H.Snapshot;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, operations(H));
    EXPECT_TRUE(Result.succeeded());
    EXPECT_EQ(H.ReadCalls, 4);
    EXPECT_EQ(Result.Receipt.VerifiedActionCount, 7u);
  }
  {
    Harness H;
    const SanitizerPublicationMetadataPlanV1 Plan =
        windowsPlan(H.OwnerBytes, H.ACLBytes, H.MOTWBytes);
    H.Snapshot.Values = {
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
        {SecurityMetadataSubject::ACL, H.ACLBytes},
        {SecurityMetadataSubject::WindowsMOTW, H.MOTWBytes},
    };
    H.SourceSnapshot = H.Snapshot;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, operations(H));
    EXPECT_TRUE(Result.succeeded());
    EXPECT_EQ(H.ReadCalls, 3);
    EXPECT_EQ(Result.Receipt.VerifiedActionCount, 3u);
  }
}

TEST(SanitizerPublicationExecutor,
     WindowsCannotReplaceItsCompleteSecurityDescriptorWithAnACLDefault) {
  Harness H;
  SanitizerPublicationMetadataPlanV1 Plan =
      windowsPlan(H.OwnerBytes, H.ACLBytes, H.MOTWBytes);
  Plan.Actions[1] = clear(SecurityMetadataSubject::ACL);
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));
  EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::InvalidAction);
  EXPECT_TRUE(H.Events.empty());
}

TEST(SanitizerPublicationExecutor,
     ValidatesPOSIXModePlanShapeOnTheDeclaredPlatform) {
  Harness H;
  const SanitizerPublicationExecutorOperationsV1 Ops = operations(H);

  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  Plan.Actions.back().ExpectedDigest->EncodedBytes = 3;
  EXPECT_EQ(executeSanitizerPublicationMetadata(Plan, Ops).Reason,
            SanitizerPublicationExecutionReason::InvalidAction);

  Plan = linuxPlan(H.OwnerBytes);
  Plan.Actions.back() = clear(SecurityMetadataSubject::POSIXMode);
  EXPECT_EQ(executeSanitizerPublicationMetadata(Plan, Ops).Reason,
            SanitizerPublicationExecutionReason::InvalidAction);

  Plan = windowsPlan(H.OwnerBytes, H.ACLBytes, H.MOTWBytes);
  Plan.Actions.push_back(
      preserve(SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes));
  EXPECT_EQ(executeSanitizerPublicationMetadata(Plan, Ops).Reason,
            SanitizerPublicationExecutionReason::InvalidAction);
  EXPECT_TRUE(H.Events.empty());
}

TEST(SanitizerPublicationExecutor,
     RejectsInvalidPlansBeforeInvokingAnyAdapterCallback) {
  Harness H;
  const SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  const SanitizerPublicationMetadataPlanV1 Baseline = linuxPlan(H.OwnerBytes);

  struct Case {
    SanitizerPublicationMetadataPlanV1 Plan;
    SanitizerPublicationExecutionReason Reason;
  };
  std::vector<Case> Cases;
  {
    auto Plan = Baseline;
    ++Plan.Version;
    Cases.push_back(
        {std::move(Plan),
         SanitizerPublicationExecutionReason::UnsupportedPlanVersion});
  }
  {
    auto Plan = Baseline;
    Plan.Complete = false;
    Cases.push_back(
        {std::move(Plan), SanitizerPublicationExecutionReason::PlanNotReady});
  }
  {
    auto Plan = Baseline;
    Plan.Platform.reset();
    Cases.push_back({std::move(Plan),
                     SanitizerPublicationExecutionReason::MissingPlanPlatform});
  }
  {
    auto Plan = Baseline;
    Plan.Guarantees.NamespaceAtomic = true;
    Cases.push_back(
        {std::move(Plan),
         SanitizerPublicationExecutionReason::InvalidPlannerGuarantees});
  }
  {
    auto Plan = Baseline;
    std::swap(Plan.Actions[0], Plan.Actions[1]);
    Cases.push_back(
        {std::move(Plan),
         SanitizerPublicationExecutionReason::NonCanonicalActionOrder});
  }
  {
    auto Plan = Baseline;
    Plan.Actions[1] = Plan.Actions[0];
    Cases.push_back({std::move(Plan),
                     SanitizerPublicationExecutionReason::DuplicateAction});
  }
  {
    auto Plan = Baseline;
    Plan.Actions.erase(Plan.Actions.begin());
    Cases.push_back({std::move(Plan),
                     SanitizerPublicationExecutionReason::IncompleteActionSet});
  }
  {
    auto Plan = Baseline;
    Plan.Actions[4].Disposition =
        SecurityMetadataDisposition::PreserveFromSource;
    Cases.push_back(
        {std::move(Plan), SanitizerPublicationExecutionReason::InvalidAction});
  }
  {
    auto Plan = noChangePlan();
    Plan.GuardedSiteCount = 1;
    Cases.push_back({std::move(Plan),
                     SanitizerPublicationExecutionReason::InvalidNoChangePlan});
  }

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    SCOPED_TRACE(Index);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Cases[Index].Plan, Ops);
    EXPECT_FALSE(Result.succeeded());
    EXPECT_EQ(Result.Reason, Cases[Index].Reason);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_FALSE(Result.Receipt.Complete);
  }
  EXPECT_TRUE(H.Events.empty());
}

TEST(SanitizerPublicationExecutor,
     RejectsMalformedOrActionDivergentSourceManifestsBeforeBegin) {
  Harness H;
  const SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  const SanitizerPublicationMetadataPlanV1 Baseline = linuxPlan(H.OwnerBytes);
  constexpr std::array<uint8_t, 1> InvalidPresenceDigest = {0x01};
  constexpr std::array<uint8_t, 3> WrongOwnerBytes = {0xfe, 0xfd, 0xfc};

  std::vector<SanitizerPublicationMetadataPlanV1> Cases;
  {
    auto Plan = Baseline;
    Plan.SourceMetadata.erase(Plan.SourceMetadata.begin());
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    Plan.SourceMetadata.insert(Plan.SourceMetadata.begin(),
                               Plan.SourceMetadata.front());
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    std::swap(Plan.SourceMetadata[0], Plan.SourceMetadata[1]);
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    Plan.SourceMetadata.back().Subject =
        SecurityMetadataSubject::MacOSQuarantine;
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    ++Plan.ObservedRecordCount;
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    ++Plan.ObservedEncodedBytes;
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    Plan.SourceMetadata.insert(
        Plan.SourceMetadata.end() - 1,
        sourceMetadata(SecurityMetadataSubject::SetUIDBit,
                       InvalidPresenceDigest));
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    Plan.SourceMetadata.insert(
        Plan.SourceMetadata.begin() + 1,
        sourceMetadata(SecurityMetadataSubject::ACL, H.ACLBytes));
    ++Plan.ObservedRecordCount;
    Plan.ObservedEncodedBytes += H.ACLBytes.size();
    Cases.push_back(std::move(Plan));
  }
  {
    auto Plan = Baseline;
    Plan.SourceMetadata[0].ExpectedDigest =
        digestCanonicalSecurityMetadata(WrongOwnerBytes);
    Cases.push_back(std::move(Plan));
  }

  for (size_t Index = 0; Index < Cases.size(); ++Index) {
    SCOPED_TRACE(Index);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Cases[Index], Ops);
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_TRUE(H.Events.empty());
  }
  EXPECT_EQ(H.NoChangeCalls, 0);
  EXPECT_EQ(H.BeginCalls, 0);
  EXPECT_EQ(H.SourceObserveCalls, 0);
  EXPECT_EQ(H.CreateCalls, 0);
  EXPECT_EQ(H.ApplyCalls, 0);
  EXPECT_EQ(H.PublishCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     RejectsMissingOperationsAndUnprovenCapabilitiesBeforeBegin) {
  Harness H;
  const SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  Ops.PublishNoReplace = {};
  SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::MissingOperation);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.Capabilities.AtomicNoReplacePublish = false;
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.Capabilities.CandidatePublishOperandBinding =
      CandidatePublishOperandBindingV1::None;
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.Capabilities.CandidatePublishOperandBinding =
      static_cast<CandidatePublishOperandBindingV1>(0xff);
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.AuthenticateSourceAfterBegin = {};
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::MissingOperation);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.Capabilities.CompleteSourceMetadataEnumeration = false;
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.AuthenticatePublishedFinal = {};
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::MissingOperation);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.FinalizePublished = {};
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::MissingOperation);
  EXPECT_TRUE(H.Events.empty());

  Ops = operations(H);
  Ops.Capabilities.AnchoredPublishedFinalAuthentication = false;
  Result = executeSanitizerPublicationMetadata(Plan, Ops);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities);
  EXPECT_TRUE(H.Events.empty());
}

TEST(SanitizerPublicationExecutor,
     EveryFailureAfterBeginDiscardsExactlyOnceAndNeverRetriesPublish) {
  struct Case {
    FailurePoint Failure;
    SanitizerPublicationExecutionReason Reason;
    SanitizerPublicationOutcome Outcome;
    int ExpectedDiscardCalls;
    int ExpectedPublishCalls;
  };
  const Case Cases[] = {
      {FailurePoint::Begin, SanitizerPublicationExecutionReason::BeginFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::SourceObserve,
       SanitizerPublicationExecutionReason::SourceAuthenticationFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::Create,
       SanitizerPublicationExecutionReason::CreateExclusiveFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::CandidateFirst,
       SanitizerPublicationExecutionReason::CandidateAuthenticationFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::Read,
       SanitizerPublicationExecutionReason::MaterialReadFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::Apply,
       SanitizerPublicationExecutionReason::MetadataApplyFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::Observe,
       SanitizerPublicationExecutionReason::CandidateAuthenticationFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::CandidateSecond,
       SanitizerPublicationExecutionReason::CandidateAuthenticationFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
      {FailurePoint::Destination,
       SanitizerPublicationExecutionReason::DestinationReauthenticationFailed,
       SanitizerPublicationOutcome::NotPublished, 1, 0},
  };

  for (const Case &Entry : Cases) {
    SCOPED_TRACE(static_cast<int>(Entry.Failure));
    Harness H;
    H.Failure = Entry.Failure;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_FALSE(Result.succeeded());
    EXPECT_EQ(Result.Reason, Entry.Reason);
    EXPECT_EQ(Result.Outcome, Entry.Outcome);
    EXPECT_EQ(H.DiscardCalls, Entry.ExpectedDiscardCalls);
    EXPECT_EQ(H.PublishCalls, Entry.ExpectedPublishCalls);
    EXPECT_EQ(H.FinalAuthenticationCalls, 0);
    EXPECT_EQ(H.FinalizeCalls, 0);
    EXPECT_LE(H.PublishCalls, 1);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_FALSE(Result.Receipt.Guarantees.NamespaceAtomic);
    EXPECT_FALSE(Result.Receipt.Guarantees.DestinationCreateExclusive);
    if (Entry.ExpectedDiscardCalls != 0) {
      ASSERT_FALSE(H.Events.empty());
      EXPECT_EQ(H.Events.back(), "discard");
    }
  }
}

TEST(SanitizerPublicationExecutor,
     BeginExceptionAlsoDiscardsOnePartiallyInitializedTransaction) {
  Harness H;
  H.Failure = FailurePoint::ThrowBegin;
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                          operations(H));
  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::CallbackException);
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_EQ(H.BeginCalls, 1);
  EXPECT_EQ(H.CreateCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 1);
  EXPECT_EQ(H.Events, (std::vector<std::string>{"begin", "discard"}));
}

TEST(SanitizerPublicationExecutor,
     HeldSourceEnumerationMustExactlyMatchThePlannedPresentSet) {
  struct Case {
    const char *Name;
    SanitizerPublicationMetadataPlanV1 Plan;
    CompleteMetadataSnapshotV1 Snapshot;
    SanitizerPublicationExecutionReason Reason;
  };
  Harness Seed;
  const SanitizerPublicationMetadataPlanV1 Baseline =
      linuxPlan(Seed.OwnerBytes);
  std::vector<Case> Cases;
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.EnumerationComplete = false;
    Cases.push_back(
        {"incomplete", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.UnsupportedMetadataCount = 1;
    Cases.push_back(
        {"unsupported-surface", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::UnsupportedMetadataObserved});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.Values.insert(Snapshot.Values.begin(), Snapshot.Values.front());
    Cases.push_back(
        {"duplicate", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::DuplicateMetadataObservation});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    std::swap(Snapshot.Values[0], Snapshot.Values[1]);
    Cases.push_back(
        {"noncanonical", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.Values.insert(Snapshot.Values.end() - 1,
                           {SecurityMetadataSubject::ACL, Seed.ACLBytes});
    Cases.push_back(
        {"new-clear-subject", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::UnexpectedMetadataObserved});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.Values.pop_back();
    Cases.push_back(
        {"missing", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::MissingMetadataObservation});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.Values[0].CanonicalBytes.push_back(0xff);
    Cases.push_back(
        {"digest", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::MetadataDigestMismatch});
  }
  {
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.Values.push_back({static_cast<SecurityMetadataSubject>(0xff), {}});
    Cases.push_back(
        {"unknown-subject", Baseline, std::move(Snapshot),
         SanitizerPublicationExecutionReason::UnsupportedMetadataObserved});
  }
  {
    SanitizerPublicationMetadataPlanV1 Plan = Baseline;
    Plan.SourceMetadata.insert(
        Plan.SourceMetadata.end() - 1,
        sourcePresence(SecurityMetadataSubject::SetUIDBit));
    ++Plan.ObservedRecordCount;
    CompleteMetadataSnapshotV1 Snapshot = Seed.SourceSnapshot;
    Snapshot.Values.insert(Snapshot.Values.end() - 1,
                           {SecurityMetadataSubject::SetUIDBit, {0x01}});
    Cases.push_back(
        {"presence-only-has-bytes", std::move(Plan), std::move(Snapshot),
         SanitizerPublicationExecutionReason::MetadataPresenceMismatch});
  }

  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Name);
    Harness H;
    H.SourceSnapshot = Entry.Snapshot;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Entry.Plan, operations(H));
    EXPECT_EQ(Result.Reason, Entry.Reason);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(H.BeginCalls, 1);
    EXPECT_EQ(H.SourceObserveCalls, 1);
    EXPECT_EQ(H.CreateCalls, 0);
    EXPECT_EQ(H.ReadCalls, 0);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.ObserveCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.Events,
              (std::vector<std::string>{"begin", "source-observe", "discard"}));
  }
}

TEST(SanitizerPublicationExecutor,
     TypedObjectAuthenticationRejectsSourceAndCandidateSubstitution) {
  {
    Harness H;
    H.SourceArtifact.LinkCount = 2;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::SourceAuthenticationMismatch);
    EXPECT_EQ(H.BeginCalls, 1);
    EXPECT_EQ(H.CreateCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 0);
  }
  {
    Harness H;
    H.CandidateArtifact.Content = artifactContent(0xee, 4103);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch);
    EXPECT_EQ(H.CreateCalls, 1);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 0);
  }
  {
    Harness H;
    H.CandidateArtifact.StableIdentity = H.SourceArtifact.StableIdentity;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
  {
    Harness H;
    H.CandidateAfterMetadataArtifact.Content = artifactContent(0xef, 4103);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch);
    EXPECT_EQ(H.ApplyCalls, 7);
    EXPECT_EQ(H.ObserveCalls, 1);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 0);
  }
  {
    Harness H;
    H.CandidateAfterMetadataArtifact.StableIdentity = artifactIdentity(0xef);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 0);
  }
}

TEST(SanitizerPublicationExecutor,
     EveryActionFailureStopsImmediatelyAndDiscardsExactlyOnce) {
  for (int FailedAction = 1; FailedAction <= 7; ++FailedAction) {
    SCOPED_TRACE(FailedAction);
    Harness H;
    H.Failure = FailurePoint::Apply;
    H.FailApplyCall = FailedAction;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::MetadataApplyFailed);
    EXPECT_EQ(H.ApplyCalls, FailedAction);
    EXPECT_EQ(H.ObserveCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    ASSERT_FALSE(H.Events.empty());
    EXPECT_EQ(H.Events.back(), "discard");
  }
}

TEST(SanitizerPublicationExecutor,
     PublicationOutcomeIsExplicitAndPublishIsAttemptedOnlyOnce) {
  for (SanitizerPublicationOutcome Outcome :
       {SanitizerPublicationOutcome::NotPublished,
        SanitizerPublicationOutcome::Indeterminate}) {
    SCOPED_TRACE(static_cast<int>(Outcome));
    Harness H;
    H.PublishResult = {Outcome, "publish failed"};
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_FALSE(Result.succeeded());
    EXPECT_EQ(Result.Outcome, Outcome);
    EXPECT_EQ(
        Result.Reason,
        Outcome == SanitizerPublicationOutcome::NotPublished
            ? SanitizerPublicationExecutionReason::NamespacePublishNotPerformed
            : SanitizerPublicationExecutionReason::
                  NamespacePublishIndeterminate);
    EXPECT_EQ(H.PublishCalls, 1);
    EXPECT_EQ(H.FinalAuthenticationCalls, 0);
    EXPECT_EQ(H.FinalizeCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_FALSE(Result.Receipt.Guarantees.NamespaceAtomic);
    EXPECT_FALSE(Result.Receipt.Guarantees.DestinationCreateExclusive);
  }
}

TEST(SanitizerPublicationExecutor,
     PublishedFinalSwapOrTamperingFailsButOnlyFinalizesCommittedState) {
  {
    Harness H;
    H.FinalArtifact.StableIdentity = artifactIdentity(0xee);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                                 PublishedFinalAuthenticationMismatch);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_FALSE(Result.Receipt.Guarantees.NamespaceAtomic);
    EXPECT_EQ(H.PublishCalls, 1);
    EXPECT_EQ(H.FinalAuthenticationCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 1);
    EXPECT_EQ(H.DiscardCalls, 0);
    ASSERT_GE(H.Events.size(), 3u);
    EXPECT_EQ(H.Events[H.Events.size() - 3], "publish");
    EXPECT_EQ(H.Events[H.Events.size() - 2], "final-observe");
    EXPECT_EQ(H.Events.back(), "finalize");
  }
  {
    Harness H;
    H.FinalArtifact.Content = artifactContent(0xee, 4103);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                                 PublishedFinalAuthenticationMismatch);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
    EXPECT_EQ(H.FinalizeCalls, 1);
    EXPECT_EQ(H.DiscardCalls, 0);
  }
  {
    Harness H;
    CompleteMetadataSnapshotV1 Tampered = H.Snapshot;
    Tampered.Values[0].CanonicalBytes.push_back(0xff);
    H.FinalSnapshotOverride = std::move(Tampered);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason, SanitizerPublicationExecutionReason::
                                 PublishedFinalAuthenticationMismatch);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
    EXPECT_EQ(H.FinalizeCalls, 1);
    EXPECT_EQ(H.DiscardCalls, 0);
  }
}

TEST(
    SanitizerPublicationExecutor,
    PublishedFinalAuthenticationAndFinalizationFailuresAreMutuallyExclusiveWithDiscard) {
  struct Case {
    FailurePoint Failure;
    SanitizerPublicationExecutionReason Reason;
  };
  const Case Cases[] = {
      {FailurePoint::FinalAuthentication,
       SanitizerPublicationExecutionReason::PublishedFinalAuthenticationFailed},
      {FailurePoint::ThrowFinalAuthentication,
       SanitizerPublicationExecutionReason::CallbackException},
      {FailurePoint::Finalize,
       SanitizerPublicationExecutionReason::PublishedFinalizationFailed},
      {FailurePoint::ThrowFinalize,
       SanitizerPublicationExecutionReason::CallbackException},
  };

  for (const Case &Entry : Cases) {
    SCOPED_TRACE(static_cast<int>(Entry.Failure));
    Harness H;
    H.Failure = Entry.Failure;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason, Entry.Reason);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_FALSE(Result.Receipt.Guarantees.NamespaceAtomic);
    EXPECT_EQ(H.PublishCalls, 1);
    EXPECT_EQ(H.FinalAuthenticationCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 1);
    EXPECT_EQ(H.DiscardCalls, 0);
    EXPECT_EQ(H.Events.back(), "finalize");
  }

  Harness H;
  H.Failure = FailurePoint::FinalAuthentication;
  SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  Ops.FinalizePublished = [&]() -> llvm::Error {
    ++H.FinalizeCalls;
    H.Events.emplace_back("finalize");
    return testError("finalize also failed");
  };
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes), Ops);
  EXPECT_EQ(
      Result.Reason,
      SanitizerPublicationExecutionReason::PublishedFinalAuthenticationFailed);
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Published);
  EXPECT_NE(Result.Detail.find("final authentication failed"),
            std::string::npos);
  EXPECT_NE(Result.Detail.find("finalize also failed"), std::string::npos);
  EXPECT_EQ(H.FinalizeCalls, 1);
  EXPECT_EQ(H.DiscardCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     DiscardFailureIsJoinedWithoutReplacingThePrimaryFailure) {
  Harness H;
  H.Failure = FailurePoint::Apply;
  SanitizerPublicationExecutorOperationsV1 Ops = operations(H);
  Ops.Discard = [&]() -> llvm::Error {
    ++H.DiscardCalls;
    H.Events.emplace_back("discard");
    return testError("discard failed");
  };
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes), Ops);

  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::MetadataApplyFailed);
  EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
  EXPECT_NE(Result.Detail.find("apply failed"), std::string::npos);
  EXPECT_NE(Result.Detail.find("discard failed"), std::string::npos);
  EXPECT_EQ(H.DiscardCalls, 1);
  EXPECT_EQ(H.PublishCalls, 0);
}

TEST(SanitizerPublicationExecutor,
     CallbackExceptionsFailClosedAndStillUseOneDiscard) {
  {
    Harness H;
    H.Failure = FailurePoint::ThrowApply;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::CallbackException);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
  }
  {
    Harness H;
    H.Failure = FailurePoint::ThrowPublish;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::CallbackException);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::Indeterminate);
    EXPECT_EQ(H.PublishCalls, 1);
    EXPECT_EQ(H.DiscardCalls, 1);
    EXPECT_EQ(H.FinalizeCalls, 0);
    EXPECT_FALSE(Result.Receipt.Complete);
  }
}

TEST(SanitizerPublicationExecutor,
     CoordinatorHashesRawMaterialInsteadOfTrustingTheAdapter) {
  Harness H;
  const SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  H.OwnerBytes.push_back(0xff);
  H.Snapshot.Values[0].CanonicalBytes = H.OwnerBytes;
  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));

  EXPECT_EQ(Result.Reason,
            SanitizerPublicationExecutionReason::MaterialDigestMismatch);
  EXPECT_EQ(H.ReadCalls, 1);
  EXPECT_EQ(H.ApplyCalls, 0);
  EXPECT_EQ(H.ObserveCalls, 0);
  EXPECT_EQ(H.PublishCalls, 0);
  EXPECT_EQ(H.DiscardCalls, 1);
}

TEST(SanitizerPublicationExecutor,
     RejectsNonCanonicalPOSIXModeMaterialBeforeApplyingThatAction) {
  const std::array<std::array<uint8_t, 4>, 4> InvalidModes = {{
      {{0x00, 0x08, 0x00, 0x00}}, // S_ISUID
      {{0x00, 0x04, 0x00, 0x00}}, // S_ISGID
      {{0x00, 0x80, 0x00, 0x00}}, // file-type bit
      {{0x00, 0x00, 0x00, 0x80}}, // high bit
  }};

  for (const auto &InvalidMode : InvalidModes) {
    Harness H;
    H.POSIXModeBytes.assign(InvalidMode.begin(), InvalidMode.end());
    SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
    Plan.Actions.back() =
        preserve(SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes);
    Plan.SourceMetadata.back() =
        sourceMetadata(SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes);
    H.SourceSnapshot.Values.back().CanonicalBytes = H.POSIXModeBytes;

    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::InvalidCanonicalMaterial);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(std::find(H.Events.begin(), H.Events.end(), "apply-10"),
              H.Events.end());
    EXPECT_EQ(H.ObserveCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
}

TEST(SanitizerPublicationExecutor,
     AcceptsCanonicalLittleEndianStickyPOSIXModeMaterial) {
  Harness H;
  H.POSIXModeBytes = {0xed, 0x03, 0x00, 0x00}; // 01755
  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
  Plan.Actions.back() =
      preserve(SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes);
  Plan.SourceMetadata.back() =
      sourceMetadata(SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes);
  H.SourceSnapshot.Values.back().CanonicalBytes = H.POSIXModeBytes;
  H.Snapshot.Values.back().CanonicalBytes = H.POSIXModeBytes;

  const SanitizerPublicationExecutionResultV1 Result =
      executeSanitizerPublicationMetadata(Plan, operations(H));
  EXPECT_TRUE(Result.succeeded());
  EXPECT_NE(std::find(H.Events.begin(), H.Events.end(), "apply-10"),
            H.Events.end());
}

TEST(SanitizerPublicationExecutor,
     RawMaterialAndReadbackAreBoundedBeforeUnrestrictedHashing) {
  {
    Harness H;
    H.SourceSnapshot.Values[0].CanonicalBytes.assign(
        static_cast<size_t>(kSanitizerPublicationMetadataMaxEncodedBytes) + 1,
        0x40);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded);
    EXPECT_EQ(H.SourceObserveCalls, 1);
    EXPECT_EQ(H.CreateCalls, 0);
    EXPECT_EQ(H.ReadCalls, 0);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.ObserveCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
  {
    Harness H;
    const SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(H.OwnerBytes);
    H.OwnerBytes.assign(
        static_cast<size_t>(kSanitizerPublicationMetadataMaxEncodedBytes) + 1,
        0x41);
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::MaterialByteBudgetExceeded);
    EXPECT_EQ(H.ReadCalls, 1);
    EXPECT_EQ(H.ApplyCalls, 0);
    EXPECT_EQ(H.ObserveCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
  {
    Harness H;
    H.Snapshot.Values.push_back(
        {SecurityMetadataSubject::MacOSQuarantine,
         std::vector<uint8_t>(
             static_cast<size_t>(kSanitizerPublicationMetadataMaxEncodedBytes),
             0x43)});
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded);
    EXPECT_EQ(H.ObserveCalls, 1);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
}

TEST(SanitizerPublicationExecutor,
     CompleteReadbackMustMatchTheExactPlannedPresentSet) {
  struct Case {
    const char *Name;
    CompleteMetadataSnapshotV1 Snapshot;
    SanitizerPublicationExecutionReason Reason;
  };
  Harness Seed;
  CompleteMetadataSnapshotV1 Incomplete = Seed.Snapshot;
  Incomplete.EnumerationComplete = false;
  CompleteMetadataSnapshotV1 Unsupported = Seed.Snapshot;
  Unsupported.UnsupportedMetadataCount = 1;
  CompleteMetadataSnapshotV1 Duplicate = Seed.Snapshot;
  Duplicate.Values.insert(Duplicate.Values.begin(),
                          {SecurityMetadataSubject::Owner, Seed.OwnerBytes});
  CompleteMetadataSnapshotV1 Unexpected = Seed.Snapshot;
  Unexpected.Values.insert(Unexpected.Values.end() - 1,
                           {SecurityMetadataSubject::MacOSQuarantine, {0x99}});
  CompleteMetadataSnapshotV1 ClearStillPresent = Seed.Snapshot;
  ClearStillPresent.Values.push_back(
      {SecurityMetadataSubject::ACL, Seed.ACLBytes});
  std::sort(ClearStillPresent.Values.begin(), ClearStillPresent.Values.end(),
            [](const CompleteMetadataValueV1 &Left,
               const CompleteMetadataValueV1 &Right) {
              return static_cast<uint8_t>(Left.Subject) <
                     static_cast<uint8_t>(Right.Subject);
            });
  CompleteMetadataSnapshotV1 Missing;
  Missing.EnumerationComplete = true;
  CompleteMetadataSnapshotV1 WrongDigest = Seed.Snapshot;
  WrongDigest.Values[0].CanonicalBytes.push_back(0xff);

  const Case Cases[] = {
      {"incomplete", std::move(Incomplete),
       SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration},
      {"unsupported", std::move(Unsupported),
       SanitizerPublicationExecutionReason::UnsupportedMetadataObserved},
      {"duplicate", std::move(Duplicate),
       SanitizerPublicationExecutionReason::DuplicateMetadataObservation},
      {"unexpected", std::move(Unexpected),
       SanitizerPublicationExecutionReason::UnexpectedMetadataObserved},
      {"clear-present", std::move(ClearStillPresent),
       SanitizerPublicationExecutionReason::MetadataPresenceMismatch},
      {"missing", std::move(Missing),
       SanitizerPublicationExecutionReason::MissingMetadataObservation},
      {"wrong-digest", std::move(WrongDigest),
       SanitizerPublicationExecutionReason::MetadataDigestMismatch},
  };

  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Name);
    Harness H;
    H.Snapshot = Entry.Snapshot;
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason, Entry.Reason);
    EXPECT_EQ(Result.Outcome, SanitizerPublicationOutcome::NotPublished);
    EXPECT_EQ(H.ObserveCalls, 1);
    EXPECT_EQ(H.CandidateCalls, 2);
    EXPECT_EQ(H.DestinationCalls, 0);
    EXPECT_EQ(H.PublishCalls, 0);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
}

TEST(SanitizerPublicationExecutor,
     POSIXModePresenceAndDigestParticipateInExactReadback) {
  {
    Harness H;
    H.Snapshot.Values.pop_back();
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::MissingMetadataObservation);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
  {
    Harness H;
    H.Snapshot.Values.back().CanonicalBytes = {0xec, 0x01, 0x00, 0x00};
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(linuxPlan(H.OwnerBytes),
                                            operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::MetadataDigestMismatch);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
  {
    Harness H;
    H.SourceSnapshot.Values = {
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
        {SecurityMetadataSubject::ACL, H.ACLBytes},
        {SecurityMetadataSubject::WindowsMOTW, H.MOTWBytes},
    };
    H.Snapshot.Values = {
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
        {SecurityMetadataSubject::ACL, H.ACLBytes},
        {SecurityMetadataSubject::WindowsMOTW, H.MOTWBytes},
        {SecurityMetadataSubject::POSIXMode, H.POSIXModeBytes},
    };
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(
            windowsPlan(H.OwnerBytes, H.ACLBytes, H.MOTWBytes), operations(H));
    EXPECT_EQ(Result.Reason,
              SanitizerPublicationExecutionReason::UnexpectedMetadataObserved);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
}

TEST(SanitizerPublicationExecutor, ReadbackOrderIsCanonicalAndDuplicateFree) {
  Harness Seed;
  SanitizerPublicationMetadataPlanV1 Plan = linuxPlan(Seed.OwnerBytes);
  Plan.Actions[1] = preserve(SecurityMetadataSubject::ACL, Seed.ACLBytes);
  Plan.SourceMetadata.insert(
      Plan.SourceMetadata.begin() + 1,
      sourceMetadata(SecurityMetadataSubject::ACL, Seed.ACLBytes));
  ++Plan.ObservedRecordCount;
  Plan.ObservedEncodedBytes += Seed.ACLBytes.size();

  {
    Harness H;
    H.SourceSnapshot.Values.insert(H.SourceSnapshot.Values.begin() + 1,
                                   {SecurityMetadataSubject::ACL, H.ACLBytes});
    H.Snapshot.Values = {
        {SecurityMetadataSubject::ACL, H.ACLBytes},
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
    };
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
  {
    Harness H;
    H.SourceSnapshot.Values.insert(H.SourceSnapshot.Values.begin() + 1,
                                   {SecurityMetadataSubject::ACL, H.ACLBytes});
    H.Snapshot.Values = {
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
        {SecurityMetadataSubject::Owner, H.OwnerBytes},
    };
    const SanitizerPublicationExecutionResultV1 Result =
        executeSanitizerPublicationMetadata(Plan, operations(H));
    EXPECT_EQ(
        Result.Reason,
        SanitizerPublicationExecutionReason::DuplicateMetadataObservation);
    EXPECT_EQ(H.DiscardCalls, 1);
  }
}

TEST(SanitizerPublicationExecutor, StableOutcomeAndReasonNames) {
  EXPECT_EQ(static_cast<uint16_t>(
                SanitizerPublicationExecutionReason::InvalidCanonicalMaterial),
            39u);
  EXPECT_EQ(
      static_cast<uint16_t>(
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest),
      40u);
  EXPECT_EQ(
      static_cast<uint16_t>(
          SanitizerPublicationExecutionReason::SourceMetadataEnumerationFailed),
      41u);
  EXPECT_EQ(static_cast<uint16_t>(
                SanitizerPublicationExecutionReason::InvalidArtifactBinding),
            42u);
  EXPECT_EQ(
      static_cast<uint16_t>(
          SanitizerPublicationExecutionReason::PublishedFinalizationFailed),
      49u);
  EXPECT_STREQ(toString(SanitizerPublicationOutcome::NotPublished),
               "not_published");
  EXPECT_STREQ(toString(SanitizerPublicationOutcome::Published), "published");
  EXPECT_STREQ(toString(SanitizerPublicationOutcome::Indeterminate),
               "indeterminate");
  EXPECT_STREQ(toString(CandidatePublishOperandBindingV1::None), "none");
  EXPECT_STREQ(toString(CandidatePublishOperandBindingV1::
                            AccessControlConfinedDistinctCredentials),
               "access_control_confined_distinct_credentials");
  EXPECT_STREQ(toString(CandidatePublishOperandBindingV1::KernelHeldObject),
               "kernel_held_object");
  const std::array<std::pair<SanitizerPublicationExecutionReason, const char *>,
                   50>
      Reasons = {{
          {SanitizerPublicationExecutionReason::None, "none"},
          {SanitizerPublicationExecutionReason::UnsupportedPlanVersion,
           "unsupported_plan_version"},
          {SanitizerPublicationExecutionReason::PlanNotReady, "plan_not_ready"},
          {SanitizerPublicationExecutionReason::MissingPlanPlatform,
           "missing_plan_platform"},
          {SanitizerPublicationExecutionReason::UnsupportedPlanPlatform,
           "unsupported_plan_platform"},
          {SanitizerPublicationExecutionReason::InvalidPlannerGuarantees,
           "invalid_planner_guarantees"},
          {SanitizerPublicationExecutionReason::InvalidNamespaceDisposition,
           "invalid_namespace_disposition"},
          {SanitizerPublicationExecutionReason::InvalidNoChangePlan,
           "invalid_no_change_plan"},
          {SanitizerPublicationExecutionReason::
               InvalidCandidateMutationDisposition,
           "invalid_candidate_mutation_disposition"},
          {SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState,
           "invalid_selinux_policy_state"},
          {SanitizerPublicationExecutionReason::NonCanonicalActionOrder,
           "noncanonical_action_order"},
          {SanitizerPublicationExecutionReason::DuplicateAction,
           "duplicate_action"},
          {SanitizerPublicationExecutionReason::InvalidAction,
           "invalid_action"},
          {SanitizerPublicationExecutionReason::IncompleteActionSet,
           "incomplete_action_set"},
          {SanitizerPublicationExecutionReason::MissingOperation,
           "missing_operation"},
          {SanitizerPublicationExecutionReason::
               InsufficientExecutorCapabilities,
           "insufficient_executor_capabilities"},
          {SanitizerPublicationExecutionReason::NoChangeReauthenticationFailed,
           "no_change_reauthentication_failed"},
          {SanitizerPublicationExecutionReason::BeginFailed, "begin_failed"},
          {SanitizerPublicationExecutionReason::CreateExclusiveFailed,
           "create_exclusive_failed"},
          {SanitizerPublicationExecutionReason::CandidateReauthenticationFailed,
           "candidate_reauthentication_failed"},
          {SanitizerPublicationExecutionReason::MaterialReadFailed,
           "material_read_failed"},
          {SanitizerPublicationExecutionReason::MaterialDigestMismatch,
           "material_digest_mismatch"},
          {SanitizerPublicationExecutionReason::MetadataApplyFailed,
           "metadata_apply_failed"},
          {SanitizerPublicationExecutionReason::MetadataEnumerationFailed,
           "metadata_enumeration_failed"},
          {SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration,
           "incomplete_metadata_enumeration"},
          {SanitizerPublicationExecutionReason::UnsupportedMetadataObserved,
           "unsupported_metadata_observed"},
          {SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation,
           "noncanonical_metadata_observation"},
          {SanitizerPublicationExecutionReason::DuplicateMetadataObservation,
           "duplicate_metadata_observation"},
          {SanitizerPublicationExecutionReason::UnexpectedMetadataObserved,
           "unexpected_metadata_observed"},
          {SanitizerPublicationExecutionReason::MissingMetadataObservation,
           "missing_metadata_observation"},
          {SanitizerPublicationExecutionReason::MetadataPresenceMismatch,
           "metadata_presence_mismatch"},
          {SanitizerPublicationExecutionReason::MetadataDigestMismatch,
           "metadata_digest_mismatch"},
          {SanitizerPublicationExecutionReason::
               DestinationReauthenticationFailed,
           "destination_reauthentication_failed"},
          {SanitizerPublicationExecutionReason::NamespacePublishNotPerformed,
           "namespace_publish_not_performed"},
          {SanitizerPublicationExecutionReason::NamespacePublishIndeterminate,
           "namespace_publish_indeterminate"},
          {SanitizerPublicationExecutionReason::CallbackException,
           "callback_exception"},
          {SanitizerPublicationExecutionReason::MaterialByteBudgetExceeded,
           "material_byte_budget_exceeded"},
          {SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded,
           "readback_byte_budget_exceeded"},
          {SanitizerPublicationExecutionReason::ArithmeticOverflow,
           "arithmetic_overflow"},
          {SanitizerPublicationExecutionReason::InvalidCanonicalMaterial,
           "invalid_canonical_material"},
          {SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
           "invalid_source_metadata_manifest"},
          {SanitizerPublicationExecutionReason::SourceMetadataEnumerationFailed,
           "source_metadata_enumeration_failed"},
          {SanitizerPublicationExecutionReason::InvalidArtifactBinding,
           "invalid_artifact_binding"},
          {SanitizerPublicationExecutionReason::SourceAuthenticationFailed,
           "source_authentication_failed"},
          {SanitizerPublicationExecutionReason::SourceAuthenticationMismatch,
           "source_authentication_mismatch"},
          {SanitizerPublicationExecutionReason::CandidateAuthenticationFailed,
           "candidate_authentication_failed"},
          {SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch,
           "candidate_authentication_mismatch"},
          {SanitizerPublicationExecutionReason::
               PublishedFinalAuthenticationFailed,
           "published_final_authentication_failed"},
          {SanitizerPublicationExecutionReason::
               PublishedFinalAuthenticationMismatch,
           "published_final_authentication_mismatch"},
          {SanitizerPublicationExecutionReason::PublishedFinalizationFailed,
           "published_finalization_failed"},
      }};
  for (const auto &[Reason, Name] : Reasons)
    EXPECT_STREQ(toString(Reason), Name);
  EXPECT_STREQ(toString(static_cast<SanitizerPublicationOutcome>(0xff)),
               "unknown");
  EXPECT_STREQ(toString(static_cast<CandidatePublishOperandBindingV1>(0xff)),
               "unknown");
  EXPECT_STREQ(
      toString(static_cast<SanitizerPublicationExecutionReason>(0xffff)),
      "unknown");
}

} // namespace
