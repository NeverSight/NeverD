//===- SanitizerPublicationExecutor.cpp - Fail-closed publication -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/safety/SanitizerPublicationExecutor.h"

#include "llvm/Support/SHA256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace neverd::safety::sanitizer_publication_metadata {
namespace {

struct ValidationFailure {
  SanitizerPublicationExecutionReason Reason =
      SanitizerPublicationExecutionReason::None;
  const char *Detail = "";

  explicit operator bool() const {
    return Reason != SanitizerPublicationExecutionReason::None;
  }
};

constexpr size_t kSubjectSlots = 11;

bool isKnownPlatform(PublicationPlatform Platform) {
  switch (Platform) {
  case PublicationPlatform::Linux:
  case PublicationPlatform::MacOS:
  case PublicationPlatform::Windows:
    return true;
  }
  return false;
}

bool isKnownCandidateMutation(CandidateMutationDisposition Disposition) {
  switch (Disposition) {
  case CandidateMutationDisposition::Unauthenticated:
  case CandidateMutationDisposition::ByteIdentical:
  case CandidateMutationDisposition::ByteMutated:
    return true;
  }
  return false;
}

bool isKnownCandidatePublishOperandBinding(
    CandidatePublishOperandBindingV1 Binding) {
  switch (Binding) {
  case CandidatePublishOperandBindingV1::None:
  case CandidatePublishOperandBindingV1::
      AccessControlConfinedDistinctCredentials:
  case CandidatePublishOperandBindingV1::KernelHeldObject:
    return true;
  }
  return false;
}

bool isKnownSELinuxPolicy(SELinuxPolicyState State) {
  switch (State) {
  case SELinuxPolicyState::DisabledAttested:
  case SELinuxPolicyState::FinalPathDerived:
  case SELinuxPolicyState::Unavailable:
    return true;
  }
  return false;
}

bool isKnownSubject(SecurityMetadataSubject Subject) {
  switch (Subject) {
  case SecurityMetadataSubject::Owner:
  case SecurityMetadataSubject::ACL:
  case SecurityMetadataSubject::SetUIDBit:
  case SecurityMetadataSubject::SetGIDBit:
  case SecurityMetadataSubject::MacOSQuarantine:
  case SecurityMetadataSubject::MacOSProvenance:
  case SecurityMetadataSubject::LinuxCapability:
  case SecurityMetadataSubject::LinuxSELinux:
  case SecurityMetadataSubject::WindowsMOTW:
  case SecurityMetadataSubject::POSIXMode:
    return true;
  }
  return false;
}

bool isKnownDisposition(SecurityMetadataDisposition Disposition) {
  switch (Disposition) {
  case SecurityMetadataDisposition::PreserveFromSource:
  case SecurityMetadataDisposition::Clear:
  case SecurityMetadataDisposition::ApplyFinalPathDerived:
  case SecurityMetadataDisposition::ClearAndVerifyAbsent:
    return true;
  }
  return false;
}

bool usesMaterial(SecurityMetadataDisposition Disposition) {
  return Disposition == SecurityMetadataDisposition::PreserveFromSource ||
         Disposition == SecurityMetadataDisposition::ApplyFinalPathDerived;
}

size_t subjectIndex(SecurityMetadataSubject Subject) {
  return static_cast<size_t>(static_cast<uint8_t>(Subject));
}

bool hasSubject(const std::array<bool, kSubjectSlots> &Subjects,
                SecurityMetadataSubject Subject) {
  const size_t Index = subjectIndex(Subject);
  return Index < Subjects.size() && Subjects[Index];
}

ValidationFailure
chargeRawBytes(uint64_t &Used, size_t Size,
               SanitizerPublicationExecutionReason BudgetReason,
               const char *BudgetDetail) {
  if constexpr (sizeof(size_t) > sizeof(uint64_t))
    if (Size > std::numeric_limits<uint64_t>::max())
      return {SanitizerPublicationExecutionReason::ArithmeticOverflow,
              "raw metadata byte count does not fit the v1 counter"};
  const uint64_t Amount = static_cast<uint64_t>(Size);
  if (Amount > std::numeric_limits<uint64_t>::max() - Used)
    return {SanitizerPublicationExecutionReason::ArithmeticOverflow,
            "raw metadata byte count overflowed the v1 counter"};
  Used += Amount;
  if (Used > kSanitizerPublicationMetadataMaxEncodedBytes)
    return {BudgetReason, BudgetDetail};
  return {};
}

ValidationFailure
chargePlannedBytes(uint64_t &Used, uint64_t Amount,
                   SanitizerPublicationExecutionReason BudgetReason,
                   const char *BudgetDetail) {
  if (Amount > std::numeric_limits<uint64_t>::max() - Used)
    return {SanitizerPublicationExecutionReason::ArithmeticOverflow,
            "planned metadata byte count overflowed the v1 counter"};
  Used += Amount;
  if (Used > kSanitizerPublicationMetadataMaxEncodedBytes)
    return {BudgetReason, BudgetDetail};
  return {};
}

ValidationFailure validateAction(PublicationPlatform Platform,
                                 const SecurityMetadataAction &Action) {
  if (!isKnownSubject(Action.Subject) ||
      !isKnownDisposition(Action.Disposition))
    return {SanitizerPublicationExecutionReason::InvalidAction,
            "publication plan contains an unknown metadata action"};

  const bool HasDigest = Action.ExpectedDigest.has_value();
  switch (Action.Subject) {
  case SecurityMetadataSubject::Owner:
    if (Action.Disposition != SecurityMetadataDisposition::PreserveFromSource ||
        !HasDigest)
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "owner action must preserve authenticated source material"};
    break;
  case SecurityMetadataSubject::ACL:
    if (Platform == PublicationPlatform::Windows) {
      if (Action.Disposition !=
              SecurityMetadataDisposition::PreserveFromSource ||
          !HasDigest)
        return {SanitizerPublicationExecutionReason::InvalidAction,
                "Windows ACL action must preserve a complete security "
                "descriptor"};
    } else if (!((Action.Disposition ==
                      SecurityMetadataDisposition::PreserveFromSource &&
                  HasDigest) ||
                 (Action.Disposition == SecurityMetadataDisposition::Clear &&
                  !HasDigest)))
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "ACL action has an invalid disposition or digest"};
    break;
  case SecurityMetadataSubject::SetUIDBit:
  case SecurityMetadataSubject::SetGIDBit:
    if (Platform == PublicationPlatform::Windows ||
        Action.Disposition != SecurityMetadataDisposition::Clear || HasDigest)
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "set-id action must be a digest-free POSIX clear"};
    break;
  case SecurityMetadataSubject::MacOSQuarantine:
  case SecurityMetadataSubject::MacOSProvenance:
    if (Platform != PublicationPlatform::MacOS ||
        Action.Disposition != SecurityMetadataDisposition::PreserveFromSource ||
        !HasDigest)
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "macOS xattr action is invalid for this platform"};
    break;
  case SecurityMetadataSubject::LinuxCapability:
    if (Platform != PublicationPlatform::Linux ||
        Action.Disposition != SecurityMetadataDisposition::ClearAndVerifyAbsent)
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "Linux capability action must clear and verify absence"};
    break;
  case SecurityMetadataSubject::LinuxSELinux:
    if (Platform != PublicationPlatform::Linux ||
        !((Action.Disposition ==
               SecurityMetadataDisposition::ApplyFinalPathDerived &&
           HasDigest) ||
          Action.Disposition ==
              SecurityMetadataDisposition::ClearAndVerifyAbsent))
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "SELinux action is invalid for this platform"};
    break;
  case SecurityMetadataSubject::WindowsMOTW:
    if (Platform != PublicationPlatform::Windows ||
        Action.Disposition != SecurityMetadataDisposition::PreserveFromSource ||
        !HasDigest)
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "Windows MOTW action must preserve authenticated source data"};
    break;
  case SecurityMetadataSubject::POSIXMode:
    if (Platform == PublicationPlatform::Windows ||
        Action.Disposition != SecurityMetadataDisposition::PreserveFromSource ||
        !HasDigest ||
        Action.ExpectedDigest->EncodedBytes !=
            kSanitizerPublicationPOSIXModeEncodedBytes)
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "POSIX mode action must preserve a canonical four-byte source "
              "mode on a POSIX platform"};
    break;
  }

  if (Action.ExpectedDigest && Action.ExpectedDigest->EncodedBytes >
                                   kSanitizerPublicationMetadataMaxEncodedBytes)
    return {SanitizerPublicationExecutionReason::InvalidAction,
            "metadata action exceeds the v1 encoded-byte ceiling"};
  return {};
}

const SourceMetadataManifestEntryV1 *
findSourceMetadata(const SanitizerPublicationMetadataPlanV1 &Plan,
                   SecurityMetadataSubject Subject) {
  for (const SourceMetadataManifestEntryV1 &Entry : Plan.SourceMetadata)
    if (Entry.Subject == Subject)
      return &Entry;
  return nullptr;
}

const SecurityMetadataAction *
findAction(const SanitizerPublicationMetadataPlanV1 &Plan,
           SecurityMetadataSubject Subject) {
  for (const SecurityMetadataAction &Action : Plan.Actions)
    if (Action.Subject == Subject)
      return &Action;
  return nullptr;
}

bool sourceSubjectMatchesPlatform(SecurityMetadataSubject Subject,
                                  PublicationPlatform Platform) {
  switch (Subject) {
  case SecurityMetadataSubject::Owner:
  case SecurityMetadataSubject::ACL:
    return true;
  case SecurityMetadataSubject::SetUIDBit:
  case SecurityMetadataSubject::SetGIDBit:
  case SecurityMetadataSubject::POSIXMode:
    return Platform != PublicationPlatform::Windows;
  case SecurityMetadataSubject::MacOSQuarantine:
  case SecurityMetadataSubject::MacOSProvenance:
    return Platform == PublicationPlatform::MacOS;
  case SecurityMetadataSubject::LinuxCapability:
  case SecurityMetadataSubject::LinuxSELinux:
    return Platform == PublicationPlatform::Linux;
  case SecurityMetadataSubject::WindowsMOTW:
    return Platform == PublicationPlatform::Windows;
  }
  return false;
}

ValidationFailure
validateSourceMetadataManifest(const SanitizerPublicationMetadataPlanV1 &Plan,
                               PublicationPlatform Platform) {
  if (Plan.SourceMetadata.size() > kSanitizerPublicationMetadataMaxRecords)
    return {SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "source metadata manifest exceeds the v1 record ceiling"};

  std::array<bool, kSubjectSlots> Seen{};
  uint8_t Previous = 0;
  uint64_t RecomputedBytes = 0;
  for (const SourceMetadataManifestEntryV1 &Entry : Plan.SourceMetadata) {
    if (!isKnownSubject(Entry.Subject) ||
        !sourceSubjectMatchesPlatform(Entry.Subject, Platform))
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "source metadata manifest contains an unknown or cross-platform "
          "subject"};
    const uint8_t Current = static_cast<uint8_t>(Entry.Subject);
    if (Current == Previous)
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "source metadata manifest contains a duplicate subject"};
    if (Current < Previous)
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "source metadata manifest is not in canonical order"};
    Previous = Current;
    Seen[subjectIndex(Entry.Subject)] = true;

    const bool PresenceOnly =
        Entry.Subject == SecurityMetadataSubject::SetUIDBit ||
        Entry.Subject == SecurityMetadataSubject::SetGIDBit;
    if (PresenceOnly == Entry.ExpectedDigest.has_value())
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "only set-id source entries may omit a digest"};
    if (!Entry.ExpectedDigest)
      continue;
    if (Entry.Subject == SecurityMetadataSubject::POSIXMode &&
        Entry.ExpectedDigest->EncodedBytes !=
            kSanitizerPublicationPOSIXModeEncodedBytes)
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "source POSIX mode manifest entry is not four bytes"};
    if (ValidationFailure Failure = chargePlannedBytes(
            RecomputedBytes, Entry.ExpectedDigest->EncodedBytes,
            SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "source metadata manifest exceeds the v1 byte ceiling"))
      return Failure;
  }

  if (!hasSubject(Seen, SecurityMetadataSubject::Owner))
    return {SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "source metadata manifest omits owner"};
  if (Platform == PublicationPlatform::Windows) {
    if (!hasSubject(Seen, SecurityMetadataSubject::ACL))
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "Windows source metadata manifest omits its security descriptor"};
  } else if (!hasSubject(Seen, SecurityMetadataSubject::POSIXMode)) {
    return {SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "POSIX source metadata manifest omits mode"};
  }

  uint64_t RecomputedRecords = Plan.SourceMetadata.size();
  for (const SecurityMetadataAction &Action : Plan.Actions) {
    if (Action.Disposition !=
        SecurityMetadataDisposition::ApplyFinalPathDerived)
      continue;
    if (!Action.ExpectedDigest ||
        RecomputedRecords == std::numeric_limits<uint64_t>::max())
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "final-path-derived material is missing or overflows record count"};
    ++RecomputedRecords;
    if (ValidationFailure Failure = chargePlannedBytes(
            RecomputedBytes, Action.ExpectedDigest->EncodedBytes,
            SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "observed metadata exceeds the v1 byte ceiling"))
      return Failure;
  }
  if (RecomputedRecords != Plan.ObservedRecordCount ||
      RecomputedBytes != Plan.ObservedEncodedBytes)
    return {SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "observed metadata counts do not match the canonical manifest"};
  return {};
}

ValidationFailure validateActionsAgainstSourceMetadata(
    const SanitizerPublicationMetadataPlanV1 &Plan) {
  for (const SecurityMetadataAction &Action : Plan.Actions) {
    const SourceMetadataManifestEntryV1 *Source =
        findSourceMetadata(Plan, Action.Subject);
    switch (Action.Disposition) {
    case SecurityMetadataDisposition::PreserveFromSource:
      if (!Source || Source->ExpectedDigest != Action.ExpectedDigest)
        return {
            SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "preserve action disagrees with source metadata manifest"};
      break;
    case SecurityMetadataDisposition::Clear:
      if (Action.Subject == SecurityMetadataSubject::ACL && Source)
        return {
            SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "ACL clear action disagrees with source metadata manifest"};
      break;
    case SecurityMetadataDisposition::ClearAndVerifyAbsent:
      if (Action.ExpectedDigest !=
          (Source ? Source->ExpectedDigest : std::nullopt))
        return {
            SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
            "clear-and-verify action disagrees with source metadata manifest"};
      break;
    case SecurityMetadataDisposition::ApplyFinalPathDerived:
      break;
    }
  }

  for (const SourceMetadataManifestEntryV1 &Entry : Plan.SourceMetadata) {
    const SecurityMetadataAction *Action = findAction(Plan, Entry.Subject);
    if (!Action)
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "source metadata manifest contains a subject absent from actions"};
    if (Entry.Subject == SecurityMetadataSubject::SetUIDBit ||
        Entry.Subject == SecurityMetadataSubject::SetGIDBit ||
        Entry.Subject == SecurityMetadataSubject::LinuxCapability ||
        Entry.Subject == SecurityMetadataSubject::LinuxSELinux)
      continue;
    if (Action->Disposition != SecurityMetadataDisposition::PreserveFromSource)
      return {
          SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest,
          "source metadata manifest subject is not preserved by its action"};
  }
  return {};
}

ValidationFailure validatePlan(const SanitizerPublicationMetadataPlanV1 &Plan) {
  if (Plan.Version != kSanitizerPublicationMetadataSchemaVersion)
    return {SanitizerPublicationExecutionReason::UnsupportedPlanVersion,
            "publication plan schema is unsupported"};
  if (!Plan.Complete || Plan.Reason != SanitizerPublicationMetadataReason::None)
    return {SanitizerPublicationExecutionReason::PlanNotReady,
            "publication plan is not complete and ready"};
  if (!Plan.Platform)
    return {SanitizerPublicationExecutionReason::MissingPlanPlatform,
            "publication plan has no authenticated platform"};
  if (!isKnownPlatform(*Plan.Platform))
    return {SanitizerPublicationExecutionReason::UnsupportedPlanPlatform,
            "publication plan platform is unsupported"};
  if (Plan.Guarantees.NamespaceAtomic || Plan.Guarantees.CompareAndSwap ||
      Plan.Guarantees.CrashDurable)
    return {SanitizerPublicationExecutionReason::InvalidPlannerGuarantees,
            "pure metadata planner must not claim execution guarantees"};
  if (!isKnownCandidateMutation(Plan.CandidateMutation))
    return {SanitizerPublicationExecutionReason::
                InvalidCandidateMutationDisposition,
            "publication plan has an invalid candidate mutation state"};
  if (Plan.SELinuxPolicy && !isKnownSELinuxPolicy(*Plan.SELinuxPolicy))
    return {SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState,
            "publication plan has an invalid SELinux policy state"};
  if (Plan.ObservedRecordCount > kSanitizerPublicationMetadataMaxRecords ||
      Plan.ObservedEncodedBytes >
          kSanitizerPublicationMetadataMaxEncodedBytes ||
      Plan.Actions.size() > kSanitizerPublicationMetadataMaxActions)
    return {SanitizerPublicationExecutionReason::InvalidAction,
            "publication plan exceeds the v1 hard limits"};

  if (ValidationFailure Failure =
          validateSourceMetadataManifest(Plan, *Plan.Platform))
    return Failure;

  if (Plan.NamespaceDisposition == PublicationNamespaceDisposition::NoChange) {
    if (Plan.GuardedSiteCount != 0 ||
        Plan.CandidateMutation != CandidateMutationDisposition::ByteIdentical ||
        !Plan.Actions.empty())
      return {SanitizerPublicationExecutionReason::InvalidNoChangePlan,
              "no-change plan must be byte-identical and action-free"};
    if (*Plan.Platform == PublicationPlatform::Linux) {
      if (!Plan.SELinuxPolicy)
        return {SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState,
                "Linux plan is missing its attested SELinux policy state"};
    } else if (Plan.SELinuxPolicy) {
      return {SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState,
              "non-Linux plan contains a SELinux policy state"};
    }
    return {};
  }
  if (Plan.NamespaceDisposition !=
      PublicationNamespaceDisposition::CreateExclusive)
    return {SanitizerPublicationExecutionReason::InvalidNamespaceDisposition,
            "executor accepts only no-change or create-exclusive plans"};

  const PublicationPlatform Platform = *Plan.Platform;
  if (Platform == PublicationPlatform::Linux) {
    if (!Plan.SELinuxPolicy ||
        *Plan.SELinuxPolicy == SELinuxPolicyState::Unavailable)
      return {SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState,
              "Linux create-exclusive plan lacks an executable SELinux policy"};
  } else if (Plan.SELinuxPolicy) {
    return {SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState,
            "non-Linux plan contains a SELinux policy state"};
  }

  std::array<bool, kSubjectSlots> Subjects{};
  uint8_t Previous = 0;
  for (const SecurityMetadataAction &Action : Plan.Actions) {
    if (ValidationFailure Failure = validateAction(Platform, Action))
      return Failure;
    const uint8_t Current = static_cast<uint8_t>(Action.Subject);
    if (Current == Previous)
      return {SanitizerPublicationExecutionReason::DuplicateAction,
              "publication plan contains a duplicate metadata action"};
    if (Current < Previous)
      return {SanitizerPublicationExecutionReason::NonCanonicalActionOrder,
              "publication plan actions are not in canonical order"};
    Previous = Current;
    Subjects[subjectIndex(Action.Subject)] = true;
  }

  if (!hasSubject(Subjects, SecurityMetadataSubject::Owner) ||
      !hasSubject(Subjects, SecurityMetadataSubject::ACL))
    return {SanitizerPublicationExecutionReason::IncompleteActionSet,
            "publication plan omits owner or ACL policy"};
  if (Platform != PublicationPlatform::Windows &&
      (!hasSubject(Subjects, SecurityMetadataSubject::SetUIDBit) ||
       !hasSubject(Subjects, SecurityMetadataSubject::SetGIDBit) ||
       !hasSubject(Subjects, SecurityMetadataSubject::POSIXMode)))
    return {SanitizerPublicationExecutionReason::IncompleteActionSet,
            "POSIX publication plan omits set-id clearing or mode policy"};
  if (Platform == PublicationPlatform::Linux &&
      (!hasSubject(Subjects, SecurityMetadataSubject::LinuxCapability) ||
       !hasSubject(Subjects, SecurityMetadataSubject::LinuxSELinux)))
    return {SanitizerPublicationExecutionReason::IncompleteActionSet,
            "Linux publication plan omits capability or SELinux policy"};

  if (Platform == PublicationPlatform::Linux) {
    const SecurityMetadataAction *SELinux = nullptr;
    for (const SecurityMetadataAction &Action : Plan.Actions)
      if (Action.Subject == SecurityMetadataSubject::LinuxSELinux)
        SELinux = &Action;
    if ((*Plan.SELinuxPolicy == SELinuxPolicyState::FinalPathDerived) !=
        (SELinux->Disposition ==
         SecurityMetadataDisposition::ApplyFinalPathDerived))
      return {SanitizerPublicationExecutionReason::InvalidAction,
              "SELinux action disagrees with the attested policy state"};
  }
  if (ValidationFailure Failure = validateActionsAgainstSourceMetadata(Plan))
    return Failure;
  return {};
}

ValidationFailure
validateArtifactBinding(const SanitizerPublicationMetadataPlanV1 &Plan,
                        const ArtifactBindingV1 &Binding) {
  if (Binding.Version != kSanitizerPublicationArtifactBindingVersion)
    return {SanitizerPublicationExecutionReason::InvalidArtifactBinding,
            "artifact binding schema is unsupported"};
  if (Plan.NamespaceDisposition ==
          PublicationNamespaceDisposition::CreateExclusive &&
      Plan.CandidateMutation == CandidateMutationDisposition::Unauthenticated)
    return {SanitizerPublicationExecutionReason::InvalidArtifactBinding,
            "create-exclusive publication requires authenticated candidate "
            "content"};

  const bool ContentEqual =
      Binding.ExpectedSourceContent == Binding.ExpectedCandidateContent;
  if ((Plan.CandidateMutation == CandidateMutationDisposition::ByteIdentical &&
       !ContentEqual) ||
      (Plan.CandidateMutation == CandidateMutationDisposition::ByteMutated &&
       ContentEqual))
    return {SanitizerPublicationExecutionReason::InvalidArtifactBinding,
            "candidate mutation disposition disagrees with bound content "
            "digests and sizes"};
  return {};
}

ValidationFailure validateArtifactAuthentication(
    const ArtifactAuthenticationV1 &Authentication,
    const ArtifactContentDigestV1 &ExpectedContent,
    const ArtifactStableIdentityDigestV1 *ExpectedIdentity,
    const ArtifactStableIdentityDigestV1 *ForbiddenIdentity,
    SanitizerPublicationExecutionReason MismatchReason,
    const char *MismatchDetail) {
  if (Authentication.Node != PublicationNodeKind::RegularFile ||
      Authentication.LinkCount != 1 ||
      Authentication.Content != ExpectedContent ||
      (ExpectedIdentity &&
       Authentication.StableIdentity != *ExpectedIdentity) ||
      (ForbiddenIdentity &&
       Authentication.StableIdentity == *ForbiddenIdentity))
    return {MismatchReason, MismatchDetail};
  return {};
}

ArtifactAuthenticationReceiptV1
makeArtifactReceipt(const ArtifactAuthenticationV1 &Authentication) {
  ArtifactAuthenticationReceiptV1 Receipt;
  Receipt.Content = Authentication.Content;
  Receipt.StableIdentity = Authentication.StableIdentity;
  Receipt.RegularFileMatched = true;
  Receipt.SingleLinkMatched = true;
  Receipt.ContentMatched = true;
  Receipt.StableIdentityMatched = true;
  return Receipt;
}

ValidationFailure
validateOperations(const SanitizerPublicationExecutorOperationsV1 &Operations) {
  if (!Operations.Begin || !Operations.AuthenticateSourceAfterBegin ||
      !Operations.CreateExclusiveCandidate ||
      !Operations.AuthenticateCandidateAfterCreate ||
      !Operations.ReadMaterial || !Operations.ApplyMetadata ||
      !Operations.AuthenticateCandidateAfterMetadata ||
      !Operations.ReauthenticateDestinationAbsent ||
      !Operations.PublishNoReplace || !Operations.AuthenticatePublishedFinal ||
      !Operations.FinalizePublished || !Operations.Discard)
    return {SanitizerPublicationExecutionReason::MissingOperation,
            "publication executor is missing a required operation"};
  const SanitizerPublicationExecutorCapabilitiesV1 &Capabilities =
      Operations.Capabilities;
  if (!Capabilities.SourceIdentityPinned ||
      !Capabilities.DestinationDirectoryAnchored ||
      !Capabilities.TemporaryCreationExclusive ||
      !isKnownCandidatePublishOperandBinding(
          Capabilities.CandidatePublishOperandBinding) ||
      Capabilities.CandidatePublishOperandBinding ==
          CandidatePublishOperandBindingV1::None ||
      !Capabilities.CompleteSourceMetadataEnumeration ||
      !Capabilities.CompleteCandidateMetadataEnumeration ||
      !Capabilities.CompletePublishedFinalMetadataEnumeration ||
      !Capabilities.ArtifactContentAuthentication ||
      !Capabilities.StableObjectIdentityAuthentication ||
      !Capabilities.AnchoredPublishedFinalAuthentication ||
      !Capabilities.AtomicNoReplacePublish)
    return {
        SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities,
        "publication adapter cannot prove the required handle-bound "
        "create-exclusive transaction"};
  return {};
}

ValidationFailure
validateCandidateSnapshot(const SanitizerPublicationMetadataPlanV1 &Plan,
                          const CompleteMetadataSnapshotV1 &Snapshot) {
  if (!Snapshot.EnumerationComplete)
    return {SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration,
            "candidate metadata enumeration is incomplete"};
  if (Snapshot.UnsupportedMetadataCount != 0)
    return {SanitizerPublicationExecutionReason::UnsupportedMetadataObserved,
            "candidate contains unrepresentable xattr, ADS, or security "
            "metadata"};
  if (Snapshot.Values.size() > kSanitizerPublicationMetadataMaxActions)
    return {SanitizerPublicationExecutionReason::UnexpectedMetadataObserved,
            "candidate metadata observation exceeds the v1 hard limit"};

  std::array<const SecurityMetadataAction *, kSubjectSlots> Expected{};
  for (const SecurityMetadataAction &Action : Plan.Actions)
    Expected[subjectIndex(Action.Subject)] = &Action;
  std::array<bool, kSubjectSlots> Seen{};

  uint64_t ReadbackBytes = 0;
  uint8_t Previous = 0;
  for (const CompleteMetadataValueV1 &Value : Snapshot.Values) {
    if (ValidationFailure Failure = chargeRawBytes(
            ReadbackBytes, Value.CanonicalBytes.size(),
            SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded,
            "candidate metadata readback exceeds the v1 byte budget"))
      return Failure;
    if (!isKnownSubject(Value.Subject))
      return {SanitizerPublicationExecutionReason::UnsupportedMetadataObserved,
              "candidate metadata observation has an unknown subject"};
    const uint8_t Current = static_cast<uint8_t>(Value.Subject);
    if (Current == Previous)
      return {SanitizerPublicationExecutionReason::DuplicateMetadataObservation,
              "candidate metadata observation contains a duplicate subject"};
    if (Current < Previous)
      return {
          SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation,
          "candidate metadata observations are not in canonical order"};
    Previous = Current;

    const size_t Index = subjectIndex(Value.Subject);
    const SecurityMetadataAction *Action = Expected[Index];
    if (!Action)
      return {SanitizerPublicationExecutionReason::UnexpectedMetadataObserved,
              "candidate contains a metadata subject absent from the plan"};
    if (!usesMaterial(Action->Disposition))
      return {SanitizerPublicationExecutionReason::MetadataPresenceMismatch,
              "candidate retains metadata that the plan requires absent"};
    if (!Action->ExpectedDigest ||
        digestCanonicalSecurityMetadata(Value.CanonicalBytes) !=
            *Action->ExpectedDigest)
      return {SanitizerPublicationExecutionReason::MetadataDigestMismatch,
              "candidate metadata readback differs from the plan"};
    Seen[Index] = true;
  }

  for (const SecurityMetadataAction &Action : Plan.Actions)
    if (usesMaterial(Action.Disposition) && !Seen[subjectIndex(Action.Subject)])
      return {SanitizerPublicationExecutionReason::MissingMetadataObservation,
              "candidate is missing metadata required by the plan"};
  return {};
}

ValidationFailure
validateCanonicalMaterial(SecurityMetadataSubject Subject,
                          llvm::ArrayRef<uint8_t> CanonicalBytes) {
  if (Subject != SecurityMetadataSubject::POSIXMode)
    return {};
  if (CanonicalBytes.size() != kSanitizerPublicationPOSIXModeEncodedBytes)
    return {SanitizerPublicationExecutionReason::InvalidCanonicalMaterial,
            "canonical POSIX mode is not exactly four bytes"};

  const uint32_t Mode = static_cast<uint32_t>(CanonicalBytes[0]) |
                        (static_cast<uint32_t>(CanonicalBytes[1]) << 8) |
                        (static_cast<uint32_t>(CanonicalBytes[2]) << 16) |
                        (static_cast<uint32_t>(CanonicalBytes[3]) << 24);
  constexpr uint32_t AllowedPOSIXModeBits = 01777u;
  if ((Mode & ~AllowedPOSIXModeBits) != 0)
    return {SanitizerPublicationExecutionReason::InvalidCanonicalMaterial,
            "canonical POSIX mode contains file-type, set-id, or reserved "
            "bits"};
  return {};
}

ValidationFailure
validateSourceMetadataSnapshot(const SanitizerPublicationMetadataPlanV1 &Plan,
                               const CompleteMetadataSnapshotV1 &Snapshot) {
  if (!Snapshot.EnumerationComplete)
    return {SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration,
            "source metadata enumeration is incomplete"};
  if (Snapshot.UnsupportedMetadataCount != 0)
    return {SanitizerPublicationExecutionReason::UnsupportedMetadataObserved,
            "source contains unrepresentable xattr, ADS, or security "
            "metadata"};
  if (Snapshot.Values.size() > kSanitizerPublicationMetadataMaxRecords)
    return {SanitizerPublicationExecutionReason::UnexpectedMetadataObserved,
            "source metadata observation exceeds the v1 hard limit"};

  std::array<bool, kSubjectSlots> Seen{};
  uint64_t ReadbackBytes = 0;
  uint8_t Previous = 0;
  for (const CompleteMetadataValueV1 &Value : Snapshot.Values) {
    if (ValidationFailure Failure = chargeRawBytes(
            ReadbackBytes, Value.CanonicalBytes.size(),
            SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded,
            "source metadata readback exceeds the v1 byte budget"))
      return Failure;
    if (!isKnownSubject(Value.Subject))
      return {SanitizerPublicationExecutionReason::UnsupportedMetadataObserved,
              "source metadata observation has an unknown subject"};
    const uint8_t Current = static_cast<uint8_t>(Value.Subject);
    if (Current == Previous)
      return {SanitizerPublicationExecutionReason::DuplicateMetadataObservation,
              "source metadata observation contains a duplicate subject"};
    if (Current < Previous)
      return {
          SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation,
          "source metadata observations are not in canonical order"};
    Previous = Current;

    const SourceMetadataManifestEntryV1 *Expected =
        findSourceMetadata(Plan, Value.Subject);
    if (!Expected)
      return {SanitizerPublicationExecutionReason::UnexpectedMetadataObserved,
              "source contains a metadata subject absent from the plan"};
    const size_t Index = subjectIndex(Value.Subject);
    if (!Expected->ExpectedDigest) {
      if (!Value.CanonicalBytes.empty())
        return {SanitizerPublicationExecutionReason::MetadataPresenceMismatch,
                "presence-only source metadata contains canonical bytes"};
    } else {
      if (digestCanonicalSecurityMetadata(Value.CanonicalBytes) !=
          *Expected->ExpectedDigest)
        return {SanitizerPublicationExecutionReason::MetadataDigestMismatch,
                "source metadata differs from the planned snapshot"};
      if (ValidationFailure Failure =
              validateCanonicalMaterial(Value.Subject, Value.CanonicalBytes))
        return Failure;
    }
    Seen[Index] = true;
  }

  for (const SourceMetadataManifestEntryV1 &Entry : Plan.SourceMetadata)
    if (!Seen[subjectIndex(Entry.Subject)])
      return {SanitizerPublicationExecutionReason::MissingMetadataObservation,
              "source is missing metadata present in the planned snapshot"};
  return {};
}

std::string takeErrorDetail(llvm::Error Error) {
  return llvm::toString(std::move(Error));
}

void appendDiscardFailure(std::string &Primary, llvm::Error DiscardError) {
  if (!DiscardError)
    return;
  if (!Primary.empty())
    Primary += "; ";
  Primary += "discard failed: ";
  Primary += takeErrorDetail(std::move(DiscardError));
}

void appendFinalizationFailure(std::string &Primary,
                               llvm::Error FinalizationError) {
  if (!FinalizationError)
    return;
  if (!Primary.empty())
    Primary += "; ";
  Primary += "published finalization failed: ";
  Primary += takeErrorDetail(std::move(FinalizationError));
}

} // namespace

const char *toString(SanitizerPublicationOutcome Outcome) {
  switch (Outcome) {
  case SanitizerPublicationOutcome::NotPublished:
    return "not_published";
  case SanitizerPublicationOutcome::Published:
    return "published";
  case SanitizerPublicationOutcome::Indeterminate:
    return "indeterminate";
  }
  return "unknown";
}

const char *toString(CandidatePublishOperandBindingV1 Binding) {
  switch (Binding) {
  case CandidatePublishOperandBindingV1::None:
    return "none";
  case CandidatePublishOperandBindingV1::
      AccessControlConfinedDistinctCredentials:
    return "access_control_confined_distinct_credentials";
  case CandidatePublishOperandBindingV1::KernelHeldObject:
    return "kernel_held_object";
  }
  return "unknown";
}

const char *toString(SanitizerPublicationExecutionReason Reason) {
  switch (Reason) {
  case SanitizerPublicationExecutionReason::None:
    return "none";
  case SanitizerPublicationExecutionReason::UnsupportedPlanVersion:
    return "unsupported_plan_version";
  case SanitizerPublicationExecutionReason::PlanNotReady:
    return "plan_not_ready";
  case SanitizerPublicationExecutionReason::MissingPlanPlatform:
    return "missing_plan_platform";
  case SanitizerPublicationExecutionReason::UnsupportedPlanPlatform:
    return "unsupported_plan_platform";
  case SanitizerPublicationExecutionReason::InvalidPlannerGuarantees:
    return "invalid_planner_guarantees";
  case SanitizerPublicationExecutionReason::InvalidNamespaceDisposition:
    return "invalid_namespace_disposition";
  case SanitizerPublicationExecutionReason::InvalidNoChangePlan:
    return "invalid_no_change_plan";
  case SanitizerPublicationExecutionReason::InvalidCandidateMutationDisposition:
    return "invalid_candidate_mutation_disposition";
  case SanitizerPublicationExecutionReason::InvalidSELinuxPolicyState:
    return "invalid_selinux_policy_state";
  case SanitizerPublicationExecutionReason::NonCanonicalActionOrder:
    return "noncanonical_action_order";
  case SanitizerPublicationExecutionReason::DuplicateAction:
    return "duplicate_action";
  case SanitizerPublicationExecutionReason::InvalidAction:
    return "invalid_action";
  case SanitizerPublicationExecutionReason::IncompleteActionSet:
    return "incomplete_action_set";
  case SanitizerPublicationExecutionReason::MissingOperation:
    return "missing_operation";
  case SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities:
    return "insufficient_executor_capabilities";
  case SanitizerPublicationExecutionReason::NoChangeReauthenticationFailed:
    return "no_change_reauthentication_failed";
  case SanitizerPublicationExecutionReason::BeginFailed:
    return "begin_failed";
  case SanitizerPublicationExecutionReason::CreateExclusiveFailed:
    return "create_exclusive_failed";
  case SanitizerPublicationExecutionReason::CandidateReauthenticationFailed:
    return "candidate_reauthentication_failed";
  case SanitizerPublicationExecutionReason::MaterialReadFailed:
    return "material_read_failed";
  case SanitizerPublicationExecutionReason::MaterialDigestMismatch:
    return "material_digest_mismatch";
  case SanitizerPublicationExecutionReason::MetadataApplyFailed:
    return "metadata_apply_failed";
  case SanitizerPublicationExecutionReason::MetadataEnumerationFailed:
    return "metadata_enumeration_failed";
  case SanitizerPublicationExecutionReason::IncompleteMetadataEnumeration:
    return "incomplete_metadata_enumeration";
  case SanitizerPublicationExecutionReason::UnsupportedMetadataObserved:
    return "unsupported_metadata_observed";
  case SanitizerPublicationExecutionReason::NonCanonicalMetadataObservation:
    return "noncanonical_metadata_observation";
  case SanitizerPublicationExecutionReason::DuplicateMetadataObservation:
    return "duplicate_metadata_observation";
  case SanitizerPublicationExecutionReason::UnexpectedMetadataObserved:
    return "unexpected_metadata_observed";
  case SanitizerPublicationExecutionReason::MissingMetadataObservation:
    return "missing_metadata_observation";
  case SanitizerPublicationExecutionReason::MetadataPresenceMismatch:
    return "metadata_presence_mismatch";
  case SanitizerPublicationExecutionReason::MetadataDigestMismatch:
    return "metadata_digest_mismatch";
  case SanitizerPublicationExecutionReason::DestinationReauthenticationFailed:
    return "destination_reauthentication_failed";
  case SanitizerPublicationExecutionReason::NamespacePublishNotPerformed:
    return "namespace_publish_not_performed";
  case SanitizerPublicationExecutionReason::NamespacePublishIndeterminate:
    return "namespace_publish_indeterminate";
  case SanitizerPublicationExecutionReason::CallbackException:
    return "callback_exception";
  case SanitizerPublicationExecutionReason::MaterialByteBudgetExceeded:
    return "material_byte_budget_exceeded";
  case SanitizerPublicationExecutionReason::ReadbackByteBudgetExceeded:
    return "readback_byte_budget_exceeded";
  case SanitizerPublicationExecutionReason::ArithmeticOverflow:
    return "arithmetic_overflow";
  case SanitizerPublicationExecutionReason::InvalidCanonicalMaterial:
    return "invalid_canonical_material";
  case SanitizerPublicationExecutionReason::InvalidSourceMetadataManifest:
    return "invalid_source_metadata_manifest";
  case SanitizerPublicationExecutionReason::SourceMetadataEnumerationFailed:
    return "source_metadata_enumeration_failed";
  case SanitizerPublicationExecutionReason::InvalidArtifactBinding:
    return "invalid_artifact_binding";
  case SanitizerPublicationExecutionReason::SourceAuthenticationFailed:
    return "source_authentication_failed";
  case SanitizerPublicationExecutionReason::SourceAuthenticationMismatch:
    return "source_authentication_mismatch";
  case SanitizerPublicationExecutionReason::CandidateAuthenticationFailed:
    return "candidate_authentication_failed";
  case SanitizerPublicationExecutionReason::CandidateAuthenticationMismatch:
    return "candidate_authentication_mismatch";
  case SanitizerPublicationExecutionReason::PublishedFinalAuthenticationFailed:
    return "published_final_authentication_failed";
  case SanitizerPublicationExecutionReason::
      PublishedFinalAuthenticationMismatch:
    return "published_final_authentication_mismatch";
  case SanitizerPublicationExecutionReason::PublishedFinalizationFailed:
    return "published_finalization_failed";
  }
  return "unknown";
}

OpaqueMetadataDigest
digestCanonicalSecurityMetadata(llvm::ArrayRef<uint8_t> CanonicalBytes) {
  OpaqueMetadataDigest Digest;
  Digest.SHA256 = llvm::SHA256::hash(CanonicalBytes);
  Digest.EncodedBytes = static_cast<uint64_t>(CanonicalBytes.size());
  return Digest;
}

SanitizerPublicationExecutionResultV1 executeSanitizerPublicationMetadata(
    const SanitizerPublicationMetadataPlanV1 &Plan,
    const ArtifactBindingV1 &Binding,
    const SanitizerPublicationExecutorOperationsV1 &Operations) {
  SanitizerPublicationExecutionResultV1 Result;
  if (ValidationFailure Failure = validatePlan(Plan)) {
    Result.Reason = Failure.Reason;
    Result.Detail = Failure.Detail;
    return Result;
  }
  if (ValidationFailure Failure = validateArtifactBinding(Plan, Binding)) {
    Result.Reason = Failure.Reason;
    Result.Detail = Failure.Detail;
    return Result;
  }

  Result.Receipt.Platform = Plan.Platform;
  Result.Receipt.NamespaceDisposition = Plan.NamespaceDisposition;

  if (Plan.NamespaceDisposition == PublicationNamespaceDisposition::NoChange) {
    if (!Operations.ReauthenticateNoChange) {
      Result.Reason = SanitizerPublicationExecutionReason::MissingOperation;
      Result.Detail = "no-change reauthentication operation is unavailable";
      return Result;
    }
    if (!Operations.Capabilities.SourceIdentityPinned ||
        !Operations.Capabilities.DestinationDirectoryAnchored ||
        !Operations.Capabilities.CompleteSourceMetadataEnumeration ||
        !Operations.Capabilities.CompletePublishedFinalMetadataEnumeration ||
        !Operations.Capabilities.ArtifactContentAuthentication ||
        !Operations.Capabilities.StableObjectIdentityAuthentication ||
        !Operations.Capabilities.AnchoredNoChangeFinalAuthentication) {
      Result.Reason =
          SanitizerPublicationExecutionReason::InsufficientExecutorCapabilities;
      Result.Detail =
          "no-change adapter cannot prove held-source and anchored-final "
          "object authentication with complete metadata enumeration";
      return Result;
    }
    try {
      llvm::Expected<AuthenticatedNoChangeArtifactsV1> Observation =
          Operations.ReauthenticateNoChange();
      if (!Observation) {
        Result.Reason =
            SanitizerPublicationExecutionReason::NoChangeReauthenticationFailed;
        Result.Detail = takeErrorDetail(Observation.takeError());
        return Result;
      }
      if (ValidationFailure Failure = validateArtifactAuthentication(
              Observation->HeldSource.Artifact, Binding.ExpectedSourceContent,
              &Binding.ExpectedSourceStableIdentity, nullptr,
              SanitizerPublicationExecutionReason::SourceAuthenticationMismatch,
              "no-change source object does not match its artifact binding")) {
        Result.Reason = Failure.Reason;
        Result.Detail = Failure.Detail;
        return Result;
      }
      if (ValidationFailure Failure = validateSourceMetadataSnapshot(
              Plan, Observation->HeldSource.Metadata)) {
        Result.Reason = Failure.Reason;
        Result.Detail = Failure.Detail;
        return Result;
      }
      if (ValidationFailure Failure = validateArtifactAuthentication(
              Observation->AnchoredFinal.Artifact,
              Binding.ExpectedSourceContent,
              &Binding.ExpectedSourceStableIdentity, nullptr,
              SanitizerPublicationExecutionReason::
                  PublishedFinalAuthenticationMismatch,
              "no-change anchored final name is not the held source object")) {
        Result.Reason = Failure.Reason;
        Result.Detail = Failure.Detail;
        return Result;
      }
      if (ValidationFailure Failure = validateSourceMetadataSnapshot(
              Plan, Observation->AnchoredFinal.Metadata)) {
        Result.Reason = SanitizerPublicationExecutionReason::
            PublishedFinalAuthenticationMismatch;
        Result.Detail = Failure.Detail;
        return Result;
      }
      Result.Receipt.SourceArtifact =
          makeArtifactReceipt(Observation->HeldSource.Artifact);
      Result.Receipt.FinalArtifact =
          makeArtifactReceipt(Observation->AnchoredFinal.Artifact);
      Result.Receipt.ObservedSourceMetadataCount =
          static_cast<uint32_t>(Observation->HeldSource.Metadata.Values.size());
      Result.Receipt.ObservedFinalMetadataCount = static_cast<uint32_t>(
          Observation->AnchoredFinal.Metadata.Values.size());
    } catch (const std::exception &Exception) {
      Result.Reason = SanitizerPublicationExecutionReason::CallbackException;
      Result.Detail =
          std::string("no-change callback threw: ") + Exception.what();
      return Result;
    } catch (...) {
      Result.Reason = SanitizerPublicationExecutionReason::CallbackException;
      Result.Detail = "no-change callback threw a non-standard exception";
      return Result;
    }
    Result.Receipt.Complete = true;
    return Result;
  }

  if (ValidationFailure Failure = validateOperations(Operations)) {
    Result.Reason = Failure.Reason;
    Result.Detail = Failure.Detail;
    return Result;
  }

  enum class CleanupState : uint8_t {
    None,
    DiscardBeforeCommit,
    FinalizeAfterCommit,
  };
  CleanupState Cleanup = CleanupState::None;
  bool PublishInvoked = false;
  auto FailBeforeCommit =
      [&](SanitizerPublicationExecutionReason Reason,
          SanitizerPublicationOutcome Outcome,
          std::string Detail) -> SanitizerPublicationExecutionResultV1 {
    Result.Receipt.Complete = false;
    Result.Receipt.Guarantees = {};
    Result.Reason = Reason;
    Result.Outcome = Outcome;
    Result.Detail = std::move(Detail);
    if (Cleanup == CleanupState::DiscardBeforeCommit) {
      // Clear before invoking the adapter so even an exception cannot cause a
      // second discard attempt through another failure path.
      Cleanup = CleanupState::None;
      try {
        appendDiscardFailure(Result.Detail, Operations.Discard());
      } catch (const std::exception &Exception) {
        if (!Result.Detail.empty())
          Result.Detail += "; ";
        Result.Detail += "discard failed: callback threw: ";
        Result.Detail += Exception.what();
      } catch (...) {
        if (!Result.Detail.empty())
          Result.Detail += "; ";
        Result.Detail +=
            "discard failed: callback threw a non-standard exception";
      }
    }
    return Result;
  };

  auto FinishAfterCommit =
      [&](SanitizerPublicationExecutionReason Reason,
          std::string Detail) -> SanitizerPublicationExecutionResultV1 {
    Result.Receipt.Complete = false;
    Result.Receipt.Guarantees = {};
    Result.Reason = Reason;
    Result.Outcome = SanitizerPublicationOutcome::Published;
    Result.Detail = std::move(Detail);

    if (Cleanup == CleanupState::FinalizeAfterCommit) {
      // Clear before invoking the callback so an exception cannot trigger a
      // retry.  Published cleanup is deliberately mutually exclusive with
      // Discard and must never unlink the committed final artifact.
      Cleanup = CleanupState::None;
      try {
        if (llvm::Error Error = Operations.FinalizePublished()) {
          if (Result.Reason == SanitizerPublicationExecutionReason::None) {
            Result.Reason = SanitizerPublicationExecutionReason::
                PublishedFinalizationFailed;
            Result.Detail = takeErrorDetail(std::move(Error));
          } else {
            appendFinalizationFailure(Result.Detail, std::move(Error));
          }
        }
      } catch (const std::exception &Exception) {
        if (Result.Reason == SanitizerPublicationExecutionReason::None) {
          Result.Reason =
              SanitizerPublicationExecutionReason::CallbackException;
          Result.Detail =
              std::string("published finalization callback threw: ") +
              Exception.what();
        } else {
          if (!Result.Detail.empty())
            Result.Detail += "; ";
          Result.Detail += "published finalization failed: callback threw: ";
          Result.Detail += Exception.what();
        }
      } catch (...) {
        if (Result.Reason == SanitizerPublicationExecutionReason::None) {
          Result.Reason =
              SanitizerPublicationExecutionReason::CallbackException;
          Result.Detail =
              "published finalization callback threw a non-standard exception";
        } else {
          if (!Result.Detail.empty())
            Result.Detail += "; ";
          Result.Detail += "published finalization failed: callback threw a "
                           "non-standard exception";
        }
      }
    }

    if (Result.Reason != SanitizerPublicationExecutionReason::None)
      return Result;
    Result.Detail.clear();
    Result.Receipt.Complete = true;
    Result.Receipt.Guarantees.NamespaceAtomic = true;
    Result.Receipt.Guarantees.DestinationCreateExclusive = true;
    Result.Receipt.Guarantees.CompareAndSwap = false;
    Result.Receipt.Guarantees.CrashDurable = false;
    Result.Receipt.Guarantees.CandidatePublishOperandBinding =
        Operations.Capabilities.CandidatePublishOperandBinding;
    return Result;
  };

  try {
    // Begin may acquire only part of the pinned transaction state before it
    // reports failure.  Arm cleanup before invoking it and require Discard to
    // tolerate that partial state.
    Cleanup = CleanupState::DiscardBeforeCommit;
    if (llvm::Error Error = Operations.Begin())
      return FailBeforeCommit(SanitizerPublicationExecutionReason::BeginFailed,
                              SanitizerPublicationOutcome::NotPublished,
                              takeErrorDetail(std::move(Error)));

    llvm::Expected<AuthenticatedSourceArtifactV1> Source =
        Operations.AuthenticateSourceAfterBegin();
    if (!Source)
      return FailBeforeCommit(
          SanitizerPublicationExecutionReason::SourceAuthenticationFailed,
          SanitizerPublicationOutcome::NotPublished,
          takeErrorDetail(Source.takeError()));
    if (ValidationFailure Failure = validateArtifactAuthentication(
            Source->Artifact, Binding.ExpectedSourceContent,
            &Binding.ExpectedSourceStableIdentity, nullptr,
            SanitizerPublicationExecutionReason::SourceAuthenticationMismatch,
            "held source object does not match its artifact binding"))
      return FailBeforeCommit(Failure.Reason,
                              SanitizerPublicationOutcome::NotPublished,
                              Failure.Detail);
    if (ValidationFailure Failure =
            validateSourceMetadataSnapshot(Plan, Source->Metadata))
      return FailBeforeCommit(Failure.Reason,
                              SanitizerPublicationOutcome::NotPublished,
                              Failure.Detail);
    Result.Receipt.SourceArtifact = makeArtifactReceipt(Source->Artifact);
    Result.Receipt.ObservedSourceMetadataCount =
        static_cast<uint32_t>(Source->Metadata.Values.size());

    if (llvm::Error Error = Operations.CreateExclusiveCandidate())
      return FailBeforeCommit(
          SanitizerPublicationExecutionReason::CreateExclusiveFailed,
          SanitizerPublicationOutcome::NotPublished,
          takeErrorDetail(std::move(Error)));
    llvm::Expected<ArtifactAuthenticationV1> InitialCandidate =
        Operations.AuthenticateCandidateAfterCreate();
    if (!InitialCandidate)
      return FailBeforeCommit(
          SanitizerPublicationExecutionReason::CandidateAuthenticationFailed,
          SanitizerPublicationOutcome::NotPublished,
          takeErrorDetail(InitialCandidate.takeError()));
    if (ValidationFailure Failure = validateArtifactAuthentication(
            *InitialCandidate, Binding.ExpectedCandidateContent, nullptr,
            &Binding.ExpectedSourceStableIdentity,
            SanitizerPublicationExecutionReason::
                CandidateAuthenticationMismatch,
            "new candidate object does not match its artifact binding or "
            "aliases the source"))
      return FailBeforeCommit(Failure.Reason,
                              SanitizerPublicationOutcome::NotPublished,
                              Failure.Detail);
    const ArtifactStableIdentityDigestV1 CandidateStableIdentity =
        InitialCandidate->StableIdentity;

    uint64_t MaterialBytes = 0;
    for (const SecurityMetadataAction &Action : Plan.Actions) {
      CanonicalSecurityMetadata Material;
      if (usesMaterial(Action.Disposition)) {
        const SecurityMetadataOrigin Origin =
            Action.Disposition ==
                    SecurityMetadataDisposition::PreserveFromSource
                ? SecurityMetadataOrigin::SourceSnapshot
                : SecurityMetadataOrigin::FinalPathDerived;
        llvm::Expected<CanonicalSecurityMetadata> Read =
            Operations.ReadMaterial(Action.Subject, Origin);
        if (!Read)
          return FailBeforeCommit(
              SanitizerPublicationExecutionReason::MaterialReadFailed,
              SanitizerPublicationOutcome::NotPublished,
              takeErrorDetail(Read.takeError()));
        Material = std::move(*Read);
        if (ValidationFailure Failure = chargeRawBytes(
                MaterialBytes, Material.size(),
                SanitizerPublicationExecutionReason::MaterialByteBudgetExceeded,
                "raw metadata material exceeds the v1 byte budget"))
          return FailBeforeCommit(Failure.Reason,
                                  SanitizerPublicationOutcome::NotPublished,
                                  Failure.Detail);
        if (!Action.ExpectedDigest ||
            Action.ExpectedDigest->EncodedBytes != Material.size() ||
            digestCanonicalSecurityMetadata(Material).SHA256 !=
                Action.ExpectedDigest->SHA256)
          return FailBeforeCommit(
              SanitizerPublicationExecutionReason::MaterialDigestMismatch,
              SanitizerPublicationOutcome::NotPublished,
              "raw canonical metadata does not match the planned digest");
        if (ValidationFailure Failure =
                validateCanonicalMaterial(Action.Subject, Material))
          return FailBeforeCommit(Failure.Reason,
                                  SanitizerPublicationOutcome::NotPublished,
                                  Failure.Detail);
      }

      if (llvm::Error Error = Operations.ApplyMetadata(Action, Material))
        return FailBeforeCommit(
            SanitizerPublicationExecutionReason::MetadataApplyFailed,
            SanitizerPublicationOutcome::NotPublished,
            takeErrorDetail(std::move(Error)));
      ++Result.Receipt.AppliedActionCount;
    }

    llvm::Expected<AuthenticatedCandidateArtifactV1> Candidate =
        Operations.AuthenticateCandidateAfterMetadata();
    if (!Candidate)
      return FailBeforeCommit(
          SanitizerPublicationExecutionReason::CandidateAuthenticationFailed,
          SanitizerPublicationOutcome::NotPublished,
          takeErrorDetail(Candidate.takeError()));
    if (ValidationFailure Failure = validateArtifactAuthentication(
            Candidate->Artifact, Binding.ExpectedCandidateContent,
            &CandidateStableIdentity, nullptr,
            SanitizerPublicationExecutionReason::
                CandidateAuthenticationMismatch,
            "candidate object changed after metadata application"))
      return FailBeforeCommit(Failure.Reason,
                              SanitizerPublicationOutcome::NotPublished,
                              Failure.Detail);
    if (ValidationFailure Failure =
            validateCandidateSnapshot(Plan, Candidate->Metadata))
      return FailBeforeCommit(Failure.Reason,
                              SanitizerPublicationOutcome::NotPublished,
                              Failure.Detail);

    Result.Receipt.CandidateArtifact = makeArtifactReceipt(Candidate->Artifact);
    Result.Receipt.ObservedCandidateMetadataCount =
        static_cast<uint32_t>(Candidate->Metadata.Values.size());
    Result.Receipt.VerifiedActionCount =
        static_cast<uint32_t>(Plan.Actions.size());

    // Candidate object authentication and complete metadata enumeration above
    // are one typed held-handle observation.  The capability preflight proves
    // that the publish primitive's source namespace entry remains bound to
    // that object; only anchored destination absence remains before commit.
    if (llvm::Error Error = Operations.ReauthenticateDestinationAbsent())
      return FailBeforeCommit(SanitizerPublicationExecutionReason::
                                  DestinationReauthenticationFailed,
                              SanitizerPublicationOutcome::NotPublished,
                              takeErrorDetail(std::move(Error)));

    PublishInvoked = true;
    NamespacePublishResultV1 Publish = Operations.PublishNoReplace();
    switch (Publish.Outcome) {
    case SanitizerPublicationOutcome::Published: {
      Cleanup = CleanupState::FinalizeAfterCommit;
      Result.Outcome = SanitizerPublicationOutcome::Published;
      llvm::Expected<AuthenticatedPublishedFinalArtifactV1> Final =
          Operations.AuthenticatePublishedFinal();
      if (!Final)
        return FinishAfterCommit(SanitizerPublicationExecutionReason::
                                     PublishedFinalAuthenticationFailed,
                                 takeErrorDetail(Final.takeError()));
      if (ValidationFailure Failure = validateArtifactAuthentication(
              Final->Artifact, Binding.ExpectedCandidateContent,
              &CandidateStableIdentity, nullptr,
              SanitizerPublicationExecutionReason::
                  PublishedFinalAuthenticationMismatch,
              "anchored final object is not the authenticated candidate"))
        return FinishAfterCommit(Failure.Reason, Failure.Detail);
      if (ValidationFailure Failure =
              validateCandidateSnapshot(Plan, Final->Metadata))
        return FinishAfterCommit(SanitizerPublicationExecutionReason::
                                     PublishedFinalAuthenticationMismatch,
                                 Failure.Detail);
      Result.Receipt.FinalArtifact = makeArtifactReceipt(Final->Artifact);
      Result.Receipt.ObservedFinalMetadataCount =
          static_cast<uint32_t>(Final->Metadata.Values.size());
      return FinishAfterCommit(SanitizerPublicationExecutionReason::None, {});
    }
    case SanitizerPublicationOutcome::NotPublished:
      return FailBeforeCommit(
          SanitizerPublicationExecutionReason::NamespacePublishNotPerformed,
          SanitizerPublicationOutcome::NotPublished,
          Publish.Detail.empty()
              ? "atomic no-replace publication was not performed"
              : std::move(Publish.Detail));
    case SanitizerPublicationOutcome::Indeterminate:
      return FailBeforeCommit(
          SanitizerPublicationExecutionReason::NamespacePublishIndeterminate,
          SanitizerPublicationOutcome::Indeterminate,
          Publish.Detail.empty()
              ? "atomic no-replace publication outcome is indeterminate"
              : std::move(Publish.Detail));
    }
    return FailBeforeCommit(
        SanitizerPublicationExecutionReason::NamespacePublishIndeterminate,
        SanitizerPublicationOutcome::Indeterminate,
        "atomic no-replace publication returned an invalid outcome");
  } catch (const std::exception &Exception) {
    const std::string Detail =
        std::string("publication callback threw: ") + Exception.what();
    if (Cleanup == CleanupState::FinalizeAfterCommit)
      return FinishAfterCommit(
          SanitizerPublicationExecutionReason::CallbackException, Detail);
    return FailBeforeCommit(
        SanitizerPublicationExecutionReason::CallbackException,
        PublishInvoked ? SanitizerPublicationOutcome::Indeterminate
                       : SanitizerPublicationOutcome::NotPublished,
        Detail);
  } catch (...) {
    constexpr const char *Detail =
        "publication callback threw a non-standard exception";
    if (Cleanup == CleanupState::FinalizeAfterCommit)
      return FinishAfterCommit(
          SanitizerPublicationExecutionReason::CallbackException, Detail);
    return FailBeforeCommit(
        SanitizerPublicationExecutionReason::CallbackException,
        PublishInvoked ? SanitizerPublicationOutcome::Indeterminate
                       : SanitizerPublicationOutcome::NotPublished,
        Detail);
  }
}

} // namespace neverd::safety::sanitizer_publication_metadata
